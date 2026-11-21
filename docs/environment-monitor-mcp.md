# 小智直连环境监测器

在小智固件的 `menuconfig → Xiaozhi Assistant` 中启用 **Enable local environment monitor tool**。环境监测器 IPv4 默认是 `192.168.101.239`，地址变化后需更新固件配置。小智和监测器需要在可互通的局域网。

升级小智固件后，可以说「小智，查一下现在的二氧化碳和光照」。小智通过一个只读工具 `self.environment.get` 直接访问监测器的 `GET /api/sensors`，一次获取温度、湿度、CO₂、气压和光照。电脑可以关机，无需运行 Python 或单独注册 MCP 网关。
