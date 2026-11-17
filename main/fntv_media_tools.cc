#include "fntv_media_tools.h"

#include "board.h"
#include "mcp_server.h"
#include "sdkconfig.h"

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

ToolResult Request(const std::string& path, const char* method = "GET", std::string body = {}) {
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
    if (!http->Open(method, std::string(CONFIG_FNTV_MEDIA_URL) + "/v1/" + path)) {
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
    addTool("self.fntv.status", "Get Windows Feiniu video title and playback progress.", {},
                   [](const PropertyList&) { return Request("status"); });
    addTool("self.fntv.pause", "Pause Feiniu video on Windows.", {},
                   [](const PropertyList&) { return Request("pause", "POST"); });
    addTool("self.fntv.resume", "Resume paused Feiniu video on Windows.", {},
                   [](const PropertyList&) { return Request("resume", "POST"); });
    addTool("self.fntv.next", "Play the next Feiniu episode on Windows.", {},
                   [](const PropertyList&) { return Request("next", "POST"); });
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
