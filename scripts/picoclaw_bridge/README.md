# 小智 × PicoClaw

桥接程序在 PicoClaw 主机运行，主动连接智能体的 MCP 接入点。固定动作支持查询时间、发送 WOL。独立进程使用 PicoClaw v0.3.0 的 CronService；任务存入该服务账户配置目录下的 xiaozhi-picoclaw/jobs.json。即使小智离线，只要本服务在运行，调度器就能执行。原 PicoClaw gateway 的任务列表与桥接任务分开管理。agent_execute 使用同一主机 PATH 中的 picoclaw 和服务账户的模型配置。

## Linux 部署

以下步骤适用于 systemd。先确认 picoclaw agent --help 可用，并选择与已配置 DeepSeek 的 PicoClaw 相同的服务用户、HOME、PATH 及 PICOCLAW_HOME。如需修改已有同名服务、环境文件或程序目录，先复制备份。将本目录复制到 /opt/xiaozhi-picoclaw，在该目录构建：

    go mod tidy
    go build -o scheduler .
    python3 -m venv .venv
    .venv/bin/pip install websocket-client==1.8.0 jsonschema
    .venv/bin/python bridge.py --action time

创建权限为 0600 的 /etc/xiaozhi-picoclaw.env。XIAOZHI_MCP_ENDPOINT（含令牌的完整 wss 地址）可以暂留空，此时服务只运行调度器。填入地址后重启服务以连接小智。需要唤醒电脑时配置 WOL_MAC（目标网卡 MAC）及可选的 WOL_BROADCAST（子网广播地址）。确认 service 的用户及其环境后安装：

    install -m 644 xiaozhi-picoclaw.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable --now xiaozhi-picoclaw
    systemctl status xiaozhi-picoclaw

实测建议先创建两分钟后的 time 动作，断开小智会话，检查任务状态与 jobs.json；再新建一个未来任务，查询并按 ID 取消。最后单独用 agent_execute 发一次短问题验证模型。WOL 只报告发送成功。

## 统一局域网设备工具

`device_control` 接收 `device` 和 `action`。智能灯使用 `light`，饮水机使用
`water_dispenser`，飞牛影视使用 `fntv`。例如
`{"device":"fntv","action":"seek_relative","seconds":-30}`。这些操作直接访问设备的 HTTP 接口，无需调用模型。

在设备的环境文件中设置 `ESP32_LIGHT_URL`、`WATER_DISPENSER_IPV4`、`FNTV_MEDIA_URL`
和已有的 `FNTV_MEDIA_TOKEN`。地址使用私有 IPv4。飞牛使用配置的 Windows 监听地址，
`discover` 会检查该地址可用性。先用三类设备的 `status` 验证，再从小智语音调用一次。

编译小智固件时启用 `CONFIG_PICOCLAW_LAN_DEVICE_TOOLS=y`，送入固件的饮水机和飞牛
工具将停用。设备自身的音量、屏幕等固件工具仍可用。需要恢复旧设备端入口时，
使用迁移开关关闭且对应工具开启的原配置重新编译并刷写。

## 回滚

停用新增服务：systemctl disable --now xiaozhi-picoclaw。保留 jobs.json 以便恢复。若替换过同名安装，恢复备份文件，再执行 systemctl daemon-reload 并恢复原服务状态。
