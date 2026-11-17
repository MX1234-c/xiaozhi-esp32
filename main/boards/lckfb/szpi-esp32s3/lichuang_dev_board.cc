#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "press_to_talk_mcp_tool.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <string_view>

#define TAG "LichuangDevBoard"

namespace {

constexpr char kRoomLightUrl[] = "http://192.168.101.246/api/light";
constexpr char kRoomLightEffectUrl[] = "http://192.168.101.246/api/light/effect";

std::expected<std::string, std::string> RequestRoomDevice(const char* method, const char* url,
                                                         std::string body = {},
                                                         int expected_status = 200,
                                                         size_t max_response_size = 512) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        return std::unexpected("Could not create room device HTTP client");
    }
    http->SetTimeout(2000);
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(body));
    }
    if (!http->Open(method, url)) {
        return std::unexpected("Could not connect to the room device");
    }
    const auto status = http->GetStatusCode();
    if (!status || *status != expected_status) {
        http->Close();
        if (!status) {
            return std::unexpected("No HTTP response from the room device");
        }
        if (*status == 202) {
            return std::unexpected("Light settings accepted, but a temporary display is active");
        }
        return std::unexpected("Room device returned HTTP " + std::to_string(*status));
    }

    std::string response;
    char buffer[128];
    while (true) {
        const auto size = http->Read(buffer, sizeof(buffer));
        if (!size || response.size() + *size > max_response_size) {
            http->Close();
            return std::unexpected("Could not read room device response");
        }
        if (*size == 0) {
            break;
        }
        response.append(buffer, *size);
    }
    http->Close();
    return response;
}

ToolResult UpdateRoomLight(std::string body) {
    auto response = RequestRoomDevice("POST", kRoomLightUrl, std::move(body));
    if (!response) {
        return std::unexpected(response.error());
    }
    CJsonUniquePtr state(cJSON_Parse(response->c_str()));
    const auto* output = cJSON_GetObjectItemCaseSensitive(state.get(), "led_output");
    if (!cJSON_IsString(output) || std::string_view(output->valuestring) != "LED READY") {
        return std::unexpected("Room light did not confirm the update");
    }
    return ReturnValue(std::move(*response));
}

std::expected<std::string, std::string> GetRoomLightDefaults() {
    auto response = RequestRoomDevice("GET", kRoomLightUrl);
    if (!response) {
        return std::unexpected(response.error());
    }
    CJsonUniquePtr state(cJSON_Parse(response->c_str()));
    const auto* brightness = cJSON_GetObjectItemCaseSensitive(state.get(), "brightness");
    const auto* rgb = cJSON_GetObjectItemCaseSensitive(state.get(), "rgb");
    const auto* red = cJSON_GetArrayItem(rgb, 0);
    const auto* green = cJSON_GetArrayItem(rgb, 1);
    const auto* blue = cJSON_GetArrayItem(rgb, 2);
    if (!cJSON_IsNumber(brightness) || !cJSON_IsNumber(red) || !cJSON_IsNumber(green) ||
        !cJSON_IsNumber(blue)) {
        return std::unexpected("Invalid room light state");
    }

    std::string defaults;
    if (brightness->valueint == 0) {
        defaults += R"(,"brightness":20)";
    }
    if (red->valueint == 0 && green->valueint == 0 && blue->valueint == 0) {
        defaults += R"(,"rgb":[255,255,255])";
    }
    return defaults;
}

ToolResult SetRoomLightPower(const PropertyList& properties) {
    if (!properties["on"].value<bool>()) {
        return UpdateRoomLight(R"({"on":false})");
    }
    auto defaults = GetRoomLightDefaults();
    if (!defaults) {
        return std::unexpected(defaults.error());
    }
    return UpdateRoomLight(R"({"on":true)" + *defaults + '}');
}

ToolResult SendRoomLightEffect(std::string body, int expected_status) {
    auto response =
        RequestRoomDevice("POST", kRoomLightEffectUrl, std::move(body), expected_status);
    if (!response) {
        return std::unexpected(response.error());
    }
    CJsonUniquePtr state(cJSON_Parse(response->c_str()));
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state.get(), "ok"))) {
        return std::unexpected("Room light did not confirm the effect command");
    }
    return ReturnValue(std::move(*response));
}

ToolResult StopRoomLightEffect(const PropertyList&) {
    return SendRoomLightEffect(R"({"effect":"none"})", 200);
}

ToolResult StartRoomLightEffect(std::string body) {
    auto defaults = GetRoomLightDefaults();
    if (!defaults) {
        return std::unexpected(defaults.error());
    }
    body += *defaults;
    body += '}';
    return SendRoomLightEffect(std::move(body), 202);
}

ToolResult SetRoomLightBlink(const PropertyList& properties) {
    const int count = properties["count"].value<int>();
    if (count == 0) {
        return StopRoomLightEffect(properties);
    }
    return StartRoomLightEffect(
        R"({"effect":"blink","count":)" + std::to_string(count) +
        R"(,"interval_ms":)" + std::to_string(properties["interval_ms"].value<int>()));
}

ToolResult SetRoomLightBreathing(const PropertyList& properties) {
    return StartRoomLightEffect(
        R"({"effect":"breathing","period_ms":)" + std::to_string(properties["period_ms"].value<int>()) +
        R"(,"duration_ms":)" + std::to_string(properties["duration_ms"].value<int>()));
}

ToolResult SetRoomLightRainbow(const PropertyList& properties) {
    return StartRoomLightEffect(
        R"({"effect":"rainbow","speed":)" + std::to_string(properties["speed"].value<int>()) +
        R"(,"duration_ms":)" + std::to_string(properties["duration_ms"].value<int>()));
}

ToolResult GetRoomEnvironment(const PropertyList&) {
    auto response = RequestRoomDevice("GET", ROOM_ENVIRONMENT_SENSOR_URL, {}, 200, 1024);
    if (!response) {
        return std::unexpected(response.error());
    }
    CJsonUniquePtr sensors(cJSON_Parse(response->c_str()));
    if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(sensors.get(), "co2"))) {
        return std::unexpected("Invalid room environment sensor response");
    }
    return ReturnValue(std::move(*response));
}

}  // namespace

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557)
        : BoxAudioCodec(i2c_bus, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, GPIO_NUM_NC,
                        AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE,
                        28.0f,  // Physical MIC1 gain
                        2,      // Physical MIC3 is the playback reference input
                        0.0f),
          pca9557_(pca9557) {}

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Display* display_;
    Pca9557* pca9557_;
    Esp32Camera* camera_;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // In press-to-talk mode the click event is handled by press down/up
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });

        boot_button_.OnPressDown([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp = nullptr;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_HEIGHT,
            .y_max = DISPLAY_WIDTH,
            .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
            .int_gpio_num = GPIO_NUM_NC, 
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 1,
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags =
            {
                .disable_control_phase = 1,
            }
        };
        tp_io_config.scl_speed_hz = 400000;

        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create touch panel IO, continuing without touch");
            return;
        }
        if (esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp) != ESP_OK || tp == nullptr) {
            ESP_LOGW(TAG, "FT5x06 touch controller not found, continuing without touch");
            esp_lcd_panel_io_del(tp_io_handle);
            return;
        }

        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(), 
            .handle = tp,
        };

        if(touch_cfg.disp) {
            lvgl_port_add_touch(&touch_cfg);
        } else {
            ESP_LOGE(TAG, "Touch display is not initialized");
        }
    }

    void InitializeCamera() {
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;
        config.ledc_timer = LEDC_TIMER_2;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });

        // Allow switching between press-to-talk (长按说话) and click-to-talk (单击唤醒)
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();

        mcp_server.AddBackgroundTool(
            "self.room_light.blink",
            "Blink the room lamp and restore its prior state afterward. Count 0 cancels the "
            "effect. The interval is the time in milliseconds between on and off transitions.",
            PropertyList({Property("count", kPropertyTypeInteger, 3, 0, 20),
                          Property("interval_ms", kPropertyTypeInteger, 500, 200, 10000)}),
            SetRoomLightBlink);
        mcp_server.AddBackgroundTool(
            "self.room_light.breathe",
            "Run a breathing or pulsing effect using the room lamp's current color. "
            "The period is one full dim-to-bright-to-dim cycle in milliseconds. "
            "The duration is the total time in milliseconds.",
            PropertyList({Property("period_ms", kPropertyTypeInteger, 3000, 1000, 60000),
                          Property("duration_ms", kPropertyTypeInteger, 30000, 1000, 120000)}),
            SetRoomLightBreathing);
        mcp_server.AddBackgroundTool(
            "self.room_light.rainbow",
            "Run the room lamp's moving rainbow effect for colorful cycling colors. "
            "Higher speed values make it move faster. The duration is in milliseconds.",
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 1, 100),
                          Property("duration_ms", kPropertyTypeInteger, 30000, 1000, 120000)}),
            SetRoomLightRainbow);
        mcp_server.AddBackgroundTool(
            "self.room_light.stop_effect",
            "Stop any active room lamp effect, including blink, breathing, or rainbow.",
            PropertyList(), StopRoomLightEffect);
        if (ROOM_ENVIRONMENT_SENSOR_URL[0] != '\0') {
            mcp_server.AddBackgroundTool(
                "self.room_environment.get_readings",
                "Read current indoor CO2 in ppm, temperature, humidity and pressure if available. "
                "Only report numbers with valid=true. If CO2 is warming up, say so.",
                PropertyList(), GetRoomEnvironment);
        }
        mcp_server.AddBackgroundTool(
            "self.room_light.set_power", "Turn the room lamp on or off.",
            PropertyList({Property("on", kPropertyTypeBoolean)}), SetRoomLightPower);
        mcp_server.AddBackgroundTool(
            "self.room_light.set_brightness",
            "Set the room lamp brightness as a percentage without changing power or color.",
            PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
            [](const PropertyList& properties) {
                return UpdateRoomLight(R"({"brightness":)" +
                                       std::to_string(properties["brightness"].value<int>()) + "}");
            });
        mcp_server.AddBackgroundTool(
            "self.room_light.set_color",
            "Set the room lamp RGB color without changing power. Interpret color descriptions as "
            "RGB, e.g. clear sky blue is (135, 206, 235). Include brightness (0-100 percent) in "
            "this same call if requested, or omit it to preserve the current brightness.",
            PropertyList({Property("r", kPropertyTypeInteger, 0, 255),
                          Property("g", kPropertyTypeInteger, 0, 255),
                          Property("b", kPropertyTypeInteger, 0, 255),
                          Property("brightness", kPropertyTypeInteger, -1, -1, 100)}),
            [](const PropertyList& properties) {
                std::string body = R"({"rgb":[)" + std::to_string(properties["r"].value<int>()) +
                                   "," + std::to_string(properties["g"].value<int>()) + "," +
                                   std::to_string(properties["b"].value<int>()) + "]";
                const int brightness = properties["brightness"].value<int>();
                if (brightness >= 0) {
                    body += R"(,"brightness":)" + std::to_string(brightness);
                }
                body += '}';
                return UpdateRoomLight(std::move(body));
            });
    }

public:
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeTouch();
        InitializeButtons();
        InitializeCamera();
        InitializeTools();

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(LichuangDevBoard);
