#include "fntv_media_tools.h"

#include "board.h"
#include "mcp_server.h"
#include "sdkconfig.h"

#include <climits>
#include <cstdio>
#include "mdns.h"
#include "esp_timer.h"
#include <string_view>

#ifdef CONFIG_FNTV_MEDIA_TOOLS
namespace {

std::string EncodeQuery(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.') {
            result += static_cast<char>(byte);
        } else {
            result += '%';
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
    }
    return result;
}

std::string DiscoverMediaUrl() {
    static bool initialized = false;
    static std::string cached;
    static int64_t expires = 0;
    if (esp_timer_get_time() < expires) return cached;
    if (!initialized) {
        if (mdns_init() != ESP_OK) return {};
        initialized = true;
    }
    mdns_result_t* results = nullptr;
    if (mdns_query_ptr("_fntv-media", "_tcp", 1800, 8, &results) != ESP_OK) return {};
    std::string found;
    size_t count = 0;
    for (auto* result = results; result; result = result->next) {
        std::string address;
        bool compatible = false;
        for (size_t i = 0; i < result->txt_count; ++i) {
            auto& txt = result->txt[i];
            if (!txt.key || !txt.value) continue;
            if (std::string_view(txt.key) == "version" && std::string_view(txt.value) == "1")
                compatible = true;
            if (std::string_view(txt.key) == "address") address = txt.value;
        }
        unsigned a, b, c, d;
        char trailing;
        if (!compatible || result->port == 0 ||
            sscanf(address.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing) != 4 ||
            b > 255 || c > 255 || d > 255 ||
            !(a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168)))
            continue;
        const auto url = "http://" + address + ":" + std::to_string(result->port);
        if (url != found) { found = url; ++count; }
    }
    mdns_query_results_free(results);
    // With several computers, use the configured address to avoid selecting the wrong player.
    cached = count == 1 ? found : "";
    expires = esp_timer_get_time() + 30000000;
    return cached;
}

ToolResult Request(const std::string& path, const char* method = "GET", std::string body = {}) {
    auto discovered = DiscoverMediaUrl();
    const std::string base = discovered.empty() ? CONFIG_FNTV_MEDIA_URL : discovered;
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) return std::unexpected("Windows media network unavailable");
    auto http = network->CreateHttp(3);
    if (!http) return std::unexpected("Windows media HTTP unavailable");
    http->SetTimeout(5000);
    http->SetHeader("Authorization", "Bearer " CONFIG_FNTV_MEDIA_TOKEN);
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(body));
    }
    if (!http->Open(method, base + "/v1/" + path)) {
        return std::unexpected("Cannot connect to Windows media service");
    }
    auto status = http->GetStatusCode();
    if (!status) return std::unexpected("Windows media service did not respond");
    std::string response;
    char buffer[512];
    for (;;) {
        auto count = http->Read(buffer, sizeof(buffer));
        if (!count) return std::unexpected("Could not read Windows media response");
        if (*count == 0) break;
        if (*count < 0 || response.size() + *count > 16384) {
            return std::unexpected("Windows media response too large");
        }
        response.append(buffer, *count);
    }
    http->Close();
    auto* json = cJSON_ParseWithLength(response.c_str(), response.size() + 1);
    if (!json) return std::unexpected("Invalid Windows media response");
    if (*status >= 200 && *status < 300) return ReturnValue{json};
    const auto* error = cJSON_GetObjectItemCaseSensitive(json, "error");
    std::string message = cJSON_IsString(error) ? error->valuestring : "Windows media request failed";
    cJSON_Delete(json);
    return std::unexpected(std::move(message));
}

}  // namespace
#endif

void RegisterFntvMediaTools(McpServer& server) {
#ifdef CONFIG_FNTV_MEDIA_TOOLS
    if (std::string_view(CONFIG_FNTV_MEDIA_TOKEN).size() != 64 ||
        !std::string_view(CONFIG_FNTV_MEDIA_URL).starts_with("http://")) {
        return;
    }
    auto addTool = [&server](const std::string& name, const std::string& description,
                             const PropertyList& properties, ToolCallback callback) {
        auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
        tool->set_background();
        server.AddTool(std::move(tool));
    };
    addTool("self.fntv.discover", "Find the Windows Feiniu player on the local network.", {},
            [](const PropertyList&) -> ToolResult {
                const auto url = DiscoverMediaUrl();
                if (url.empty()) return std::unexpected(
                    "No unique local Feiniu service found; the configured address will be used.");
                return ReturnValue{url};
            });
    addTool("self.fntv.status", "Get Windows Feiniu video title and playback progress.", {},
                   [](const PropertyList&) { return Request("status"); });
    addTool("self.fntv.pause", "Pause Feiniu video on Windows.", {},
                   [](const PropertyList&) { return Request("pause", "POST"); });
    addTool("self.fntv.resume", "Resume paused Feiniu video on Windows.", {},
                   [](const PropertyList&) { return Request("resume", "POST"); });
    addTool("self.fntv.next", "Play the next Feiniu episode on Windows.", {},
                   [](const PropertyList&) { return Request("next", "POST"); });
    addTool("self.fntv.seek_relative",
            "Skip the current Feiniu video by a signed number of seconds. Positive moves forward; "
            "negative moves backward.",
            PropertyList({Property("seconds", kPropertyTypeInteger)}),
            [](const PropertyList& args) {
                return Request("seek", "POST", "{\"mode\":\"relative\",\"seconds\":" +
                    std::to_string(args["seconds"].value<int>()) + "}");
            });
    addTool("self.fntv.seek_absolute",
            "Jump to a timestamp in the current Feiniu video. Convert minutes and hours to seconds.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 0, INT_MAX)}),
            [](const PropertyList& args) {
                return Request("seek", "POST", "{\"mode\":\"absolute\",\"seconds\":" +
                    std::to_string(args["seconds"].value<int>()) + "}");
            });
    addTool("self.fntv.fullscreen", "Enter full screen on the Windows Feiniu video player. "
                                   "Use maximize to leave full screen.", {},
            [](const PropertyList&) { return Request("fullscreen", "POST"); });
    addTool("self.fntv.maximize", "Restore, maximize and bring the Windows Feiniu video window on top.", {},
            [](const PropertyList&) { return Request("maximize", "POST"); });
    addTool("self.fntv.minimize", "Minimize the Feiniu video window without pausing playback.", {},
            [](const PropertyList&) { return Request("minimize", "POST"); });
    addTool("self.fntv.volume", "Set Feiniu player volume on Windows, from 0 to 100.",
                   PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
                   [](const PropertyList& args) {
                       return Request("volume", "POST",
                                      "{\"volume\":" + std::to_string(args["volume"].value<int>()) + "}");
                   });
    addTool("self.fntv.catalog",
            "Browse available movies and TV series on the Windows Feiniu media library. "
            "Use this for requests like 'what can I watch?', 'list my library', or for the next page. "
            "Summarize titles for the user and use the selected itemGuid if they ask to play it.",
            PropertyList({Property("page", kPropertyTypeInteger, 1, 1, 10000)}),
            [](const PropertyList& args) {
                return Request("catalog?page=" + std::to_string(args["page"].value<int>()));
            });
    addTool("self.fntv.search", "Find Feiniu movies and series by title.",
                   PropertyList({Property("title", kPropertyTypeString)}),
                   [](const PropertyList& args) -> ToolResult {
                       auto title = args["title"].value<std::string>();
                       if (title.empty() || title.size() > 200) return std::unexpected("Invalid title");
                       return Request("search?q=" + EncodeQuery(title));
                   });
    for (const std::string route : {"seasons", "episodes"}) {
        addTool("self.fntv." + route,
                       route == "seasons" ? "List seasons by series itemGuid from Feiniu search."
                                          : "List episodes by season itemGuid from Feiniu seasons.",
                       PropertyList({Property("itemGuid", kPropertyTypeString)}),
                       [route](const PropertyList& args) -> ToolResult {
                           auto id = args["itemGuid"].value<std::string>();
                           if (id.empty() || id.size() > 128) return std::unexpected("Invalid itemGuid");
                           return Request(route + "?itemGuid=" + EncodeQuery(id));
                       });
    }
    addTool("self.fntv.play", "Start Feiniu movie or episode by its selected itemGuid. "
                                   "Playback resumes at its saved position.",
                   PropertyList({Property("itemGuid", kPropertyTypeString)}),
                   [](const PropertyList& args) -> ToolResult {
                       auto id = args["itemGuid"].value<std::string>();
                       if (id.empty() || id.size() > 128 || id.find_first_not_of(
                               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                               std::string::npos) return std::unexpected("Invalid itemGuid");
                       return Request("play", "POST", "{\"itemGuid\":\"" + id + "\"}");
                   });
#else
    (void)server;
#endif
}
