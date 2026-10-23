# 配置说明

## 1. 桥：`bridge/agents.json`

每个后台 Agent 一项，可任意增减。

```json
{
  "default": "http-agent",
  "fallback": ["cli-agent"],
  "agents": {
    "http-agent": {
      "type": "http",
      "url": "http://127.0.0.1:8642/v1/chat/completions",
      "provider": "opencode-go",
      "model": "deepseek-v4.1-flash",
      "api_key_env": "RELAY_API_KEY",
      "timeout": 40,
      "aliases": ["小马"],
      "color": 0
    },
    "cli-agent": {
      "type": "cli",
      "cmd": ["your-agent-cli", "--print"],
      "timeout": 120,
      "env_file": "~/.your-agent/.env",
      "env_keys": ["YOUR_API_KEY"],
      "aliases": ["小地"],
      "color": 1
    }
  }
}
```

| 字段 | 说明 |
|---|---|
| `type` | `http`（OpenAI 兼容接口）或 `cli`（命令行程序） |
| `url` / `model` / `provider` | HTTP 模式的接口地址与模型名；**有的网关必须同时传 provider，否则 model 被忽略** |
| `api_key_env` | 从哪个环境变量读 key（也可用 `env_file` + `env_keys`） |
| `timeout` | 单轮超时（秒） |
| `aliases` | 语音切换用的名字，说这些词就切到该后台 |
| `color` | 球球颜色：0 白 / 1 蓝 / 2 红 |
| `fallback` | 主后台失败时依次尝试的后台 |

**切换方式**：说别名（如"小马"）、按手表 B 键循环、或在 App 里点按钮。

## 2. 桥：环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `RELAY_PORT` | 8443 | HTTPS/WSS 端口 |
| `RELAY_PLAIN_PORT` | 8444 | 明文 WS 端口（仅本机用） |
| `DOWNLINK_CODEC` | `opus` | 下行编码：`opus` 或 `adpcm`（固件旧版本用 adpcm） |
| `OPUS_BITRATE` | 24000 | Opus 码率；BLE 不稳时降为 `24000` |
| `REPLY_MODE` | `agent` | `echo` = 回声模式（不接大模型，测试链路用） |
| `RELAY_PY` | 当前解释器 | 调 ASR 子进程用的 Python |
| `api_key_env` / `env_file` | 见 agents.json | 每个后台自己声明 key 从哪来 |
| `ASR_URL` | — | 自定义 ASR 服务地址 |

## 3. ASR 凭据

默认火山引擎流式识别：

```bash
mkdir -p ~/.config/qiuqiu
cat > ~/.config/qiuqiu/volc_asr.env <<'EOF'
VOLCENGINE_ASR_APP_ID=xxx
VOLCENGINE_ASR_ACCESS_TOKEN=xxx
VOLCENGINE_ASR_RESOURCE_ID=volc.seedasr.sauc.duration
EOF
```

### 本地 ASR

不想用云端 ASR 时，把 `bridge.py` 里的 `asr()` 换成任意本地实现，例如：

```python
import subprocess
def asr(wav_path):
    out = subprocess.run(
        ["whisper.cpp/build/bin/whisper-cli", "-m", "models/ggml-base.bin",
         "-f", str(wav_path), "-nt", "-l", "zh"],
        capture_output=True, text=True).stdout
    return out.strip()
```

（模型的路径和参数按你用的版本调整。）

## 4. App：地址配置

App 的地址输入框支持多种写法，**传输方式由地址决定**：

| 输入 | 实际连接 |
|---|---|
| `example.com` | `wss://example.com:8443/ws` |
| `203.0.113.10` | `wss://203.0.113.10:8443/ws` |
| `2001:db8::1` | `wss://[2001:db8::1]:8443/ws` |
| `host:9000` | `wss://host:9000/ws` |
| `wss://host/custom` | 原样使用 |

**留空** = 自动：局域网（若已配置 `lan`）→ DDNS（若已配置 `ddns`）→ localhost。

`lan` 和 `ddns` 两个偏好项由 App 设置页写入（或手动 `adb shell` 写 SharedPreferences）。

## 5. DDNS 配置

```bash
export DDNS_ZONE=example.com          # 你的 Cloudflare 区域
export DDNS_RECORD=watch.example.com  # AAAA 记录名
export DDNS_IFACE=eth0                # 有公网 IPv6 的网卡
export CF_TOKEN_FILE=~/.config/qiuqiu/cf_dns_token.txt
python ddns.py
```

Cloudflare token 需有 `Zone.DNS Edit` 权限。

## 6. 固件：可调参数

| 文件 | 参数 | 说明 |
|---|---|---|
| `main/voice/voice_task.cpp` | `IN_BLOCK` / `PREBUFFER` | 播放块大小与起播预缓冲（默认 20ms / 200ms） |
| `main/voice/ble_svc.c` | `set_audio_mode(true/false)` | 连接间隔：播放 30–50ms，空闲 80–120ms |
| `main/hal/hal_display.cpp` | 熄屏超时 | 默认 5 秒 |
| `main/main.cpp` | `esp_pm_configure(160, 80, ...)` | CPU 调频范围 |
| `main/hal/hal_audio.cpp` | `idleCheck()` 30 秒 | codec/功放关闭超时 |

改完 `idf.py build` 重新刷。
