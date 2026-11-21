#include "water_dispenser_tools.h"

#include <string_view>
#include "board.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "mcp_server.h"
#include "sdkconfig.h"

#ifdef CONFIG_WATER_DISPENSER_TOOLS
namespace {

bool IsPrivateAddress(const char* address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, address, &parsed) != 1) {
        return false;
    }
    const uint32_t ip = ntohl(parsed.s_addr);
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
}

bool ValidRelay(const cJSON* root, const char* name) {
    const auto* relay = cJSON_GetObjectItemCaseSensitive(root, name);
    const auto* elapsed = cJSON_GetObjectItemCaseSensitive(relay, "elapsed_seconds");
    return cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(relay, "on")) && cJSON_IsNumber(elapsed) &&
           elapsed->valuedouble >= 0;
}

ToolResult Request(const char* suffix, const char* method, const char* body = nullptr) {
    constexpr auto address = CONFIG_WATER_DISPENSER_IPV4;
    if (std::string_view(address).empty()) {
        return std::unexpected("小智固件没有配置饮水机地址。");
    }
    if (!IsPrivateAddress(address)) {
        return std::unexpected("小智固件配置的饮水机地址无效：" + std::string(address));
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        return std::unexpected("小智的网络未就绪。");
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        return std::unexpected("无法创建饮水机 HTTP 连接。");
    }
    http->SetTimeout(3000);
    if (body) {
        http->SetHeader("Content-Type", "text/plain");
        http->SetContent(std::string(body));
    }
    const auto url = std::string("http://") + address + "/api/v1/relays" + suffix;
    if (!http->Open(method, url)) {
        return std::unexpected("无法连接饮水机，请检查地址和局域网连接。");
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        return std::unexpected("饮水机没有确认操作成功，请查询当前状态。");
    }
    std::string response;
    char buffer[128];
    for (;;) {
        auto count = http->Read(buffer, sizeof(buffer));
        if (!count || *count < 0 || response.size() + *count > 512) {
            return std::unexpected("读取饮水机状态失败。");
        }
        if (*count == 0) {
            break;
        }
        response.append(buffer, *count);
    }
    http->Close();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLengthOpts(response.c_str(), response.size() + 1, nullptr, true),
        cJSON_Delete);
    if (!cJSON_IsObject(json.get()) || !ValidRelay(json.get(), "heating") ||
        !ValidRelay(json.get(), "cooling")) {
        return std::unexpected("饮水机返回了无法识别的继电器状态。");
    }
    return ReturnValue{json.release()};
}

}  // namespace
#endif

void RegisterWaterDispenserTools(McpServer& server) {
#ifdef CONFIG_WATER_DISPENSER_TOOLS
    constexpr auto address = CONFIG_WATER_DISPENSER_IPV4;
    ESP_LOGI("WaterDispenser", "Target IPv4: %s (%s)", *address ? address : "(empty)",
             IsPrivateAddress(address) ? "valid" : "invalid");
    auto tool = std::make_unique<McpTool>(
        "self.water_dispenser.control",
        "查询或控制饮水机。action 为 status 时查询两路状态和持续秒数；"
        "只有用户明确要求打开或关闭时才使用 heating_on、heating_off、"
        "cooling_on、cooling_off。所有操作都返回两路状态和持续秒数。",
        PropertyList({Property("action", kPropertyTypeString)}),
        [](const PropertyList& args) -> ToolResult {
            const auto action = args["action"].value<std::string>();
            if (action == "status") {
                return Request("", "GET");
            }
            if (action == "heating_on" || action == "heating_off") {
                return Request("/heating", "PUT", action == "heating_on" ? "on" : "off");
            }
            if (action == "cooling_on" || action == "cooling_off") {
                return Request("/cooling", "PUT", action == "cooling_on" ? "on" : "off");
            }
            return std::unexpected("不支持的饮水机操作。");
        });
    tool->set_background();
    server.AddTool(std::move(tool));
#else
    (void)server;
#endif
}
