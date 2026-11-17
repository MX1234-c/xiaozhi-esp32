# 小智控制 Windows 飞牛影视

固件默认关闭此功能。在 ESP-IDF 的 `menuconfig → Xiaozhi Assistant` 中启用 **Enable Windows Feiniu media tools**。工具由小智设备直接访问 Windows，设备与电脑需要在可互通的局域网上。

- Windows 开发版设置 `FNTV_MEDIA_HTTP_HOST` 为电脑自己的私网 IPv4，再设置原有的 `FNTV_MEDIA_HTTP_TOKEN`（64 位十六进制字符）并重启客户端。之前打包的安装版需要用包含此改动的新包。
- 固件的 base URL 填 `http://<Windows IP>:18473`，令牌填写与 Windows 相同的值。令牌会进入固件，避免公开对应的 sdkconfig 或固件文件。只在受信任的局域网使用，Windows 防火墙可把访问限制为小智设备的 IP。
- 编译并刷入与你的板子匹配的固件。先说「查询电脑飞牛影视播放状态」，再尝试「播放《绝命毒师》第一季第五集」「暂停电脑上的视频」或「下一集」。

点播流程为搜索、选择季、选择集、开播。客户端用当前 NAS 账号查询片库并从已保存的位置续播。已在播放时，开播请求会返回 `player_busy`。
