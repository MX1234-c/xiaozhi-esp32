# 小智 × PicoClaw

桥接程序在 PicoClaw 主机运行，主动连接智能体的 MCP 接入点。固定动作支持查询时间、发送 WOL。独立进程使用 PicoClaw v0.3.0 的 CronService；任务存入该服务账户配置目录下的 xiaozhi-picoclaw/jobs.json。即使小智离线，只要本服务在运行，调度器就能执行。原 PicoClaw gateway 的任务列表与桥接任务分开管理。agent_execute 使用同一主机 PATH 中的 picoclaw 和服务账户的模型配置。

## Linux 部署

以下步骤适用于 systemd。先确认 picoclaw agent --help 可用，并选择与已配置 DeepSeek 的 PicoClaw 相同的服务用户、HOME、PATH 及 PICOCLAW_HOME。如需修改已有同名服务、环境文件或程序目录，先复制备份。将本目录复制到 /opt/xiaozhi-picoclaw，在该目录构建：

    go mod tidy
    go build -o scheduler .
    python3 -m venv .venv
    .venv/bin/pip install websocket-client==1.8.0 jsonschema
    .venv/bin/python bridge.py --action time

在权限 0600 的 /etc/xiaozhi-picoclaw.env 中配置 XIAOZHI_MCP_ENDPOINT（含令牌的完整 wss 地址）、WOL_MAC（目标网卡 MAC）、可选 WOL_BROADCAST（子网广播地址）。确认 service 的用户及其环境后安装：

    install -m 644 xiaozhi-picoclaw.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable --now xiaozhi-picoclaw
    systemctl status xiaozhi-picoclaw

实测建议先创建两分钟后的 time 动作，断开小智会话，检查任务状态与 jobs.json；再新建一个未来任务，查询并按 ID 取消。最后单独用 agent_execute 发一次短问题验证模型。WOL 只报告发送成功。

## 回滚

停用新增服务：systemctl disable --now xiaozhi-picoclaw。保留 jobs.json 以便恢复。若替换过同名安装，恢复备份文件，再执行 systemctl daemon-reload 并恢复原服务状态。
