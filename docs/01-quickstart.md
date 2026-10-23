# 快速开始

从零到跑通，大约需要 1–2 小时（第一次装 ESP-IDF 比较久）。

## 0. 你需要

| 东西 | 说明 |
|---|---|
| M5Stack StopWatch（C152） | ESP32-S3R8 / 8MB PSRAM / 466×466 AMOLED |
| 一台 Linux 电脑 | 跑桥（Windows 可用 WSL，但 WebSocket + 音频路径未验证） |
| 一部 Android 手机 | Android 12+，需要能装 ADB 调试包 |
| USB 数据线 | 刷固件用 |

选配：
- 自己的域名 + Cloudflare（外网访问用，DDNS 脚本已提供）
- 火山引擎语音识别账号（ASR）；也可以先接任意 OpenAI 兼容的 ASR

## 1. 刷固件

### 方式一：网页一键刷（推荐）

1. 打开项目的 GitHub Pages 刷机页（`docs/flasher/`，仓库部署后可见）
2. 用 Chrome 打开，插上手表 USB
3. 点 "Install"，选串口，等待完成

### 方式二：自己编译

```bash
git clone https://github.com/<你的账号>/m5stack-qiuqiu.git
cd m5stack-qiuqiu/firmware

# 1) 拉依赖（M5GFX / LVGL / mooncake 等，约 1GB）
python fetch_repos.py

# 2) 设置 ESP-IDF v5.5.4 环境
. $IDF_PATH/export.sh

# 3) 编译
idf.py build

# 4) 刷写（注意：app 从 0x20000 开始）
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  write-flash 0x20000 build/StopWatch-UserDemo.bin
```

> ⚠️ 刷完如果屏幕不亮：拔插一次 USB。`esptool --after no_reset` 会让芯片停在下载模式。

## 2. 装手机 App

```bash
cd ../android
# 用 Android Studio 打开这个目录直接 Build，或用系统安装的 gradle：
gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk

# 授权（必须！否则蓝牙用不了）
adb shell pm grant dev.wen.stopwatchrelay android.permission.BLUETOOTH_SCAN
adb shell pm grant dev.wen.stopwatchrelay android.permission.BLUETOOTH_CONNECT
adb shell pm grant dev.wen.stopwatchrelay android.permission.POST_NOTIFICATIONS
```

打开 App → 点「启动中继」。App 会：
1. 扫描并连接手表（首次需要扫描，之后记住 MAC 自动重连）
2. 连接电脑端桥

## 3. 跑电脑端桥

```bash
cd bridge
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt

# 生成自签证书（App 信任任意证书，测试用足够）
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout key.pem -out cert.pem -subj "/CN=qiuqiu"

# 先跑 echo 模式（不接任何大模型，只回放你听到的话）
REPLY_MODE=echo python bridge.py
```

App 里地址填 `<电脑IP>:8443`，点启动，对着手表按住 A 说话，松手后应该能听到自己的回声。

**看到回声 = 全链路通了。**

## 4. 接入大模型

编辑 `bridge/agents.json`，指向任意 OpenAI 兼容接口：

```json
{
  "default": "my-llm",
  "agents": {
    "my-llm": {
      "type": "http",
      "url": "http://127.0.0.1:11434/v1/chat/completions",
      "model": "qwen2.5:7b",
      "api_key_env": "RELAY_API_KEY",
      "timeout": 60
    }
  }
}
```

重启桥，再说话就是真回答了。详见 [04-config.md](04-config.md)。

## 5. 接语音识别（ASR）

桥默认用火山引擎流式 ASR：

```bash
mkdir -p ~/.config/qiuqiu
cat > ~/.config/qiuqiu/volc_asr.env <<'EOF'
VOLCENGINE_ASR_APP_ID=你的APPID
VOLCENGINE_ASR_ACCESS_TOKEN=你的TOKEN
VOLCENGINE_ASR_RESOURCE_ID=volc.seedasr.sauc.duration
EOF
```

没有账号？可以先改用本地 Whisper：见 [04-config.md](04-config.md#本地-asr)。

## 6. 外网访问（可选）

手机和电脑不在同一网络时，两条路：

**A. 公网 IPv6 + DDNS（本项目采用）**

```bash
sudo ufw allow 8443/tcp
export DDNS_ZONE=example.com DDNS_RECORD=watch.example.com
python ddns.py          # 手动跑一次
```

再用 systemd timer 每 5 分钟跑一次（见 `bridge/ddns.py` 顶部注释）。

**B. 内网穿透**（Cloudflare Tunnel / frp / Tailscale）——任选，App 里填对应的域名即可。

## 7. 验证

```bash
python selftest.py
```

应该全部通过：

```
✓ 桥服务  ✓ 端口 8443/8444  ✓ 本机 HTTPS
✓ agent 接口  ✓ 手机 App 已连接  ✓ 手表状态
```

## 常见问题

| 现象 | 原因 | 解决 |
|---|---|---|
| App 一直"搜索手表" | Android 扫描器卡死 | 关掉 App 重开（会自动按 MAC 重连） |
| 刷完屏幕黑 | 芯片停在下载模式 | 拔插 USB |
| 有回复没声音 | 下行 codec 与固件不匹配 | 桥设 `DOWNLINK_CODEC=adpcm` 试 |
| 声音断续 | 蓝牙带宽/丢包 | 用 `OPUS_BITRATE=24000` 降码率 |
| DDNS 不更新 | 网卡名或 token 不对 | 改 `DDNS_IFACE` / `CF_TOKEN_FILE` |

更多见 [05-pitfalls.md](05-pitfalls.md)。
