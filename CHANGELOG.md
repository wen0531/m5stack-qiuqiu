# 更新日志

## v0.1.0 (2026-10-23)

首个公开版本。

### 功能
- 手表按住 A 说话、松手发送；播放中按 A 打断
- B 键循环切换后台 Agent（球球颜色：白/蓝/红）
- 语音切换后台（说别名，如"小马/小地/大地"，可配置）
- 程序化 C++ 球球形象，15 个表情，无图片资源
- Opus 下行编码（24/24 kbps 可调），固定 20 ms 播放节奏
- 手机 App：BLE + WebSocket 中继、前台服务保活、开机自启、按 MAC 自动重连
- 电脑桥：HTTPS/WSS、火山引擎流式 ASR、edge-tts、多 Agent 适配（HTTP/CLI）、失败自动降级
- IPv6 DDNS（Cloudflare）脚本 + systemd 定时器
- 电池遥测：每分钟采样，24 小时环形缓冲，可通过 App 或 PC 导出 CSV
- 充电指示灯（插 USB 绿灯，拔掉熄灭）
- 一键自检脚本 `tools/selftest.py`

### 省电
- CPU DFS 160/80 MHz
- BLE modem sleep（Mode 1）+ MAC/BB 掉电 + 主晶振保活
- 功放随 codec 开关
- 空闲任务降频（语音 30 ms、下行 20 ms 轮询）
- **light sleep 不可用**：QSPI AMOLED + Octal PSRAM 组合会挂死，官方固件同样未启用

### 已知限制
- 续航实测中（当前预期 25–35 小时，450 mAh）
- 依赖手机与电脑同时在线
- 默认云端 ASR，音频会上传
