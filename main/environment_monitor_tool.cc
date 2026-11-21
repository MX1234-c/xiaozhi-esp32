#include "environment_monitor_tool.h"

#include "board.h"
#include "lwip/sockets.h"
#include "mcp_server.h"
#include "sdkconfig.h"

#ifdef CONFIG_ENVIRONMENT_MONITOR_TOOL
namespace {

bool IsPrivateAddress(const char* address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, address, &parsed) != 1) {
        return false;
    }
    const auto ip = ntohl(parsed.s_addr);
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
}

ToolResult ReadEnvironment(const PropertyList&) {
    constexpr auto address = CONFIG_ENVIRONMENT_MONITOR_IPV4;
    if (!IsPrivateAddress(address)) {
        return std::unexpected("环境监测器的局域网地址配置无效。");
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        return std::unexpected("小智的网络未就绪。");
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        return std::unexpected("无法创建环境监测器连接。");
    }
    http->SetTimeout(3000);
    if (!http->Open("GET", std::string("http://") + address + "/api/sensors")) {
        return std::unexpected("无法连接环境监测器，请检查设备和局域网。");
    }
    const auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        return std::unexpected("环境监测器未返回读数。");
    }

    std::string response;
    char buffer[256];
    for (;;) {
        const auto count = http->Read(buffer, sizeof(buffer));
        if (!count || *count < 0 || response.size() + *count > 4096) {
            return std::unexpected("读取环境监测器响应失败。");
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
    if (!cJSON_IsObject(json.get())) {
        return std::unexpected("环境监测器返回了无效数据。");
    }
    return ReturnValue{json.release()};
}

}  // namespace
#endif

void RegisterEnvironmentMonitorTool(McpServer& server) {
#ifdef CONFIG_ENVIRONMENT_MONITOR_TOOL
    auto tool = std::make_unique<McpTool>(
        "self.environment.get",
        "只读查询温度、湿度、CO₂、气压和光照。仅播报 valid 为 true 的读数；"
        "CO₂ 的 warming_up 表示预热中。",
        PropertyList{}, ReadEnvironment);
    tool->set_background();
    server.AddTool(std::move(tool));
#else
    (void)server;
#endif
}
