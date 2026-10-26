# M5Stack Qiuqiu（球球）

[English](README.md) | **中文**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Web Flasher](https://img.shields.io/badge/flash-in%20browser-brightgreen)](https://wen0531.github.io/m5stack-qiuqiu/flasher/)
[![Latest release](https://img.shields.io/github/v/release/wen0531/m5stack-qiuqiu)](https://github.com/wen0531/m5stack-qiuqiu/releases)

> **一键刷机**：手机或电脑打开 <https://wen0531.github.io/m5stack-qiuqiu/flasher/>，
> USB 连接手表即可在浏览器里刷入，无需安装工具链。

> 作者：wen0531 · 2026 · 代码 MIT，球球形象遵循 Emotion Ball 社区许可（非商业）

M5Stack Qiuqiu（球球）把 M5Stack StopWatch 手表改造成一个随身语音助手：在球球界面按住 A 说话，松手后由手机和电脑完成语音识别、Agent 推理与语音合成，回答从手表喇叭播出。手表负责采集、播放和交互，高负载计算全部留在手机与电脑侧。

项目面向自用与二次开发，不是量产消费产品。它解决的是“随手说一句、马上有回答”的交互问题，同时保留原厂 UserDemo 的启动器、表盘、秒表、闹钟等应用。

做这个项目的原因很朴素：想要一个“抬手就能问”的设备，但不想为此换手机、换耳机或再戴一块智能手表。恰好 M5Stack StopWatch 有麦克风、喇叭、圆形 AMOLED 和一颗能跑 BLE 与 Opus 的 ESP32-S3，于是把它改造成只负责“听”和“说”的终端，重活留给手机和电脑。

## 目录

- [1. 项目简介](#1-项目简介)
- [2. 能力与边界](#2-能力与边界)
- [3. 系统组成与数据流](#3-系统组成与数据流)
- [4. 硬件与软件前提](#4-硬件与软件前提)
- [5. 快速开始](#5-快速开始)
- [6. 仓库结构](#6-仓库结构)
- [7. 配置说明](#7-配置说明)
- [8. 许可与致谢](#8-许可与致谢)
- [9. 已知限制与隐私](#9-已知限制与隐私)
- [10. 故障排查与项目状态](#10-故障排查与项目状态)

---

## 1. 项目简介

一句话：**手表录音 → BLE → 手机中继 → WebSocket → 电脑桥 → ASR → LLM Agent → TTS → Opus → BLE → 手表播放。**

```
 [M5Stack StopWatch C152]
   │   BLE：上行 IMA-ADPCM / 下行 Opus
   ▼
 [Android 手机 · RelayService 前台服务]
   │   WSS
   ▼
 [电脑 · Python 桥 bridge.py]
   ├──► 流式 ASR（默认云端） ──► 文本
   ├──► LLM Agent（HTTP / CLI） ──► 回复文本
   └──► edge-tts ──► 16 kHz WAV ──► Opus 24 kbps
   │
   └──► 原路返回手表播放
```

设计取舍：

- **手表只做终端。** ESP32-S3 不运行 ASR、LLM 或 TTS，避免在 MCU 上做不现实的模型推理，也避免让 450 mAh 的电池承担额外的功耗与发热。
- **BLE 而不是 Wi-Fi。** 音频链路使用自定义 GATT 服务，连接间隔按“说话/播放”和“空闲”分级，减少射频占空比。
- **手机做中继。** Android 前台服务负责 BLE 与 WebSocket 的转接，锁屏或切换应用后仍能维持链路。
- **电脑做编排。** Python 桥负责 ASR、Agent、TTS、Opus 编码、降级与日志，替换模型或语音服务不需要改固件。
- **形象程序化生成。** 球球由 C++ 几何与弹簧动画实时绘制，不依赖图片资源，方便换色、换表情和二次创作。

## 2. 能力与边界

### 2.1 能做什么

默认形象是一个纯白球体、黑色眼睛、没有嘴；切换后台时球体变为白、蓝或红。球体会呼吸、轻微果冻形变、眨眼，表情之间平滑过渡。

| 能力 | 说明 | 状态 |
|---|---|---|
| 按住 A 说话 | 仅在球球界面生效；长按超过 500 ms 进入按住说话，松手自动发送 | 已实测 |
| 播放中按 A 打断 | 丢弃剩余下行音频、清空缓冲、通知桥停止发送 | 已实测 |
| B 键切换后台 | 球球本地立即变色（白/蓝/红），桥异步切换 Agent；连按不丢 | 已实测 |
| 语音切换后台 | 说“小马 / 小地 / 大地”等别名切换；支持“别名 + 问题” | 已实现 |
| 15 个表情 | NEUTRAL、HAPPY、SAD、ANGRY、DOUBT、SLEEPY、LISTENING、THINKING、EXCITED、CURIOUS、CONFUSED、SURPRISED、DIZZY、AFFECTION、BORED | 已实现 |
| 语音状态驱动表情 | 聆听、思考、说话、开心、困倦、待机自动切换 | 已实现 |
| 程序化球球 | 变形球体 + 弹簧物理 + Chaikin 平滑 + 扫描线抗锯齿，无图片资源 | 已实现 |
| Opus 24 kbps 下行 | 60 ms 帧、固定 20 ms 播放节奏、200 ms 起播预缓冲 | 已实测 |
| 自定义 BLE GATT | 控制、音频、状态三个特征；MTU 400；断线自动重连 | 已实测 |
| 省电组合 | 熄屏后自动 light sleep、CPU 240 MHz 亮屏全速、BLE modem sleep、功放按需 | 已实测（40 分钟无异常） |
| **A/B 双系统** | 设置页查看槽位、切换系统、克隆备份；新固件自检失败自动回滚 | 已实现 |
| **多语言界面** | 10 种语言（中/繁中/日/韩/法/德/西/俄/拉丁/英），子集字体仅 +160 KB；含启动器与设置子页 | 已实现 |
| **语言选择页** | 设置 → Language 打开语言列表，每行用自身字体，当前语言高亮 | 已实现 |
| 充电指示灯 | USB 插入亮绿灯，拔掉熄灭 | 已实测 |
| 电池遥测 | 每分钟采样，24 小时环形缓冲，CSV 导出与平均电流分析 | 已实现 |
| 外网接入 | 可选公网 IPv6 + DDNS；手机在 5G 下可直连 | 可选 |
| 网页调试页 | Chrome Web Bluetooth 连接手表，不装 App 也能做链路验证；自动跟随桥的 codec 公告 | 可选 |

### 2.2 明确的边界

- **不是独立设备。** 手表离开手机和电脑后只能显示球球与本地应用，不能独立完成一次问答。
- **手机必须常开。** Android App 需要前台服务与蓝牙、通知权限；系统省电策略可能杀掉服务，需要加入白名单。
- **电脑必须常开。** 桥负责 ASR、Agent 与 TTS；电脑休眠或断网时链路中断。
- **默认依赖公网。** 默认 ASR 是云端服务，Agent 与 TTS 也通常需要外网；离线部署需要自行替换为本地 ASR、本地模型与本地 TTS。
- **没有唤醒词。** 麦克风只在按住 A 时采集，不做常听、不做关键词唤醒。
- **单用户、单会话。** 桥只保留最后一个手机连接；没有账号、配对、鉴权或多设备管理。
- **中文优先。** 默认 TTS 音色为中文；英文或其他语言需要改配置，识别效果取决于 ASR。
- **不是量产固件。** 没有 OTA 升级界面、没有产测流程、没有长期稳定性承诺。

## 3. 系统组成与数据流

### 3.1 手表端固件

固件基于 M5Stack 官方 `M5StopWatch-UserDemo`，保留原厂启动器与全部内置应用，并新增 `Qiuqiu` 应用。主要模块：

| 模块 | 位置 | 职责 |
|---|---|---|
| 应用框架 | `firmware/main/apps/` | 启动器、表盘、秒表、闹钟、IMU、FFT、徽章、幸运转盘、设置、球球 |
| 球球应用 | `firmware/main/apps/app_qiuqiu/` | 表情状态机、帧缓冲、触摸/按键交互、配色 |
| 球球引擎 | `qiuqiu_engine.cpp` | 变形球体、眼睛轮廓、弹簧物理、抗锯齿填充、眨眼与视线 |
| 语音任务 | `firmware/main/voice/voice_task.cpp` | 按键语义、44.1 kHz 采集、16 kHz 重采样、ADPCM 编码、下行解码与播放 |
| BLE 服务 | `firmware/main/voice/ble_svc.c` | NimBLE 外设、GATT 特征、命令分发、连接参数切换 |
| 电池遥测 | `firmware/main/voice/batt_log.cpp` | 每分钟采样 VBAT/电量，环形缓冲，CSV 导出 |
| HAL | `firmware/main/hal/` | CO5300 AMOLED、ES8311、CST820 触摸、BMI270、M5PM1 电源管理 |

球球渲染路径不经过 LVGL 画布：引擎先画到 PSRAM 中的 RGB565 帧缓冲，再按 2 倍最近邻放大后直接推送到 M5GFX 面板。这样避开了 LVGL 图像 blit 的额外拷贝和撕裂问题。动画时约 20 fps，静止后降到约 8 fps；熄屏时停止渲染。

### 3.2 手机端 App

`android/` 是一个 Kotlin Android 应用，核心是 `RelayService` 前台服务：

- 按设备名 `StopWatch-BLE` 扫描并连接手表，记住 MAC 后优先后台重连。
- 订阅音频与状态特征，把上行音频的 4 字节包头剥掉，经 WebSocket 发给桥。
- 把桥下发的二进制音频补上包头，写入手表的音频特征。
- 处理 `start` / `stop` / `agent_next` / `stop_play` / `battery` 等控制消息。
- 维护扫描看门狗，扫描卡死时自动重启扫描。
- 开机广播后自动拉起服务，减少手动操作。

App 支持三种地址来源：局域网地址、DDNS 域名、本机回环（用于 `adb reverse`）。地址输入框留空时按上述顺序自动尝试。

### 3.3 电脑端桥

`bridge/` 是 Python 3 编写的 aiohttp 服务，同时提供：

- **HTTPS/WSS 8443**：手机网页与原生 App 的加密入口。
- **明文 WS 8444**：仅本机或可信内网调试使用。
- **HTTP/WS 路由**：`/ws`、`/agent`、`/battery`，用于会话、后台查询/切换和电池 CSV 导出。
- **音频编排**：上行 ADPCM 解码为 16 kHz WAV；下行 TTS 结果用 Opus 编码。
- **ASR**：默认调用火山引擎流式识别，失败时回退到任意 OpenAI 兼容的 `/v1/audio/transcriptions` 接口。
- **Agent**：读取 `agents.json`，支持 HTTP（OpenAI 兼容）与 CLI 两类后台，失败时按 `fallback` 依次降级。
- **TTS**：`edge-tts` 生成 MP3，`ffmpeg` 转成 16 kHz 单声道 WAV。
- **辅助工具**：`selftest.py` 健康检查、`battery_report.py` 续航估算、`ddns.py` Cloudflare AAAA 记录更新。

桥维护最近 6 轮对话上下文，空闲 30 分钟或切换后台时清空。上下文只对 HTTP 类后台完整生效；CLI 类后台会把最近几轮对话拼进提示词。

### 3.4 一次完整回合

1. 手表在球球界面检测到 A 长按 ≥ 500 ms，进入录音；ES8311 以 44.1 kHz 采集，线性插值到 16 kHz。
2. 每 20 ms 编码为 160 字节 IMA-ADPCM，加 4 字节包头后通过 BLE 音频特征通知手机。
3. 手机剥掉包头，把 160 字节载荷经 WebSocket 发给桥；桥解码为 16 kHz PCM，松手后落盘 WAV。
4. 桥调用 ASR 得到文本，检查是否包含后台切换别名；必要时先切换，再把剩余问题交给 Agent。
5. Agent 返回文本后，桥调用 edge-tts 合成语音，`ffmpeg` 转为 16 kHz 单声道 WAV。
6. 桥用 Opus CBR 24 kbps、60 ms 帧编码，按媒体时钟发送；手机补包头后写入手表。
7. 手表解码、重采样到 44.1 kHz，按固定 20 ms 节奏送喇叭；连续 2 秒没有新数据后结束播放。

### 3.5 BLE 协议摘要

服务 UUID 前缀为 `4f5a00xx-1e3b-4d8a-9c7e-5a1b2c3d4e5f`。

| 特征 | UUID 后缀 | 方向 | 用途 |
|---|---|---|---|
| 控制 | `0002` | 手机 → 手表 | 说话开关、命令、配色、电池导出 |
| 音频 | `0003` | 双向 | 上行音频通知、下行音频写入、事件与调试通道 |
| 状态 | `0004` | 手表 → 手机 | 连接/订阅/说话状态、播放事件 |

音频包统一为 `u16 seq（小端） | u8 codec | u8 flags | payload`：

- **上行**：`codec=1`，IMA-ADPCM 4 bit，16 kHz，20 ms / 160 B 载荷，约 64 kbps 净荷。
- **下行**：`codec=2`，Opus 16 kHz 单声道，60 ms 帧，默认 24 kbps CBR，240 B 载荷。
- **备选下行**：`codec=1`，ADPCM，20 ms / 160 B，用于兼容旧固件或对比音质。
- **事件/调试**：`codec=0x7E` 事件，`codec=0x7F` 调试透传（电池 CSV、帧缓冲转储）。

连接参数分两档：说话或播放时 30–50 ms、延迟 0；空闲时 100–150 ms、从机延迟 4。广播在开机或断线后先以 100 ms 快速广播 30 秒，再降到 1 秒。完整字段、命令码和错误降级见 [`docs/02-protocol.md`](docs/02-protocol.md)。

## 4. 硬件与软件前提

### 4.1 硬件

| 部件 | 要求 |
|---|---|
| 手表 | M5Stack StopWatch C152（ESP32-S3R8） |
| 主控 | ESP32-S3，双核，最高 240 MHz |
| 内存 | 8 MB Octal PSRAM，16 MB Flash |
| 屏幕 | 466×466 AMOLED，CO5300，QSPI |
| 音频 | ES8311 codec，板载麦克风与扬声器 |
| 触摸 | CST820 |
| 传感器 | BMI270 |
| 电源 | M5PM1 电源管理，450 mAh 电池 |
| 接口 | USB-C，支持数据与充电 |

### 4.2 软件与账号

| 用途 | 要求 |
|---|---|
| 固件构建 | ESP-IDF v5.5.4、Python 3、Git；首次拉取依赖约 1 GB |
| 手机 App | Android Studio 或 Android SDK 34、JDK 17；建议 Android 12（API 31）及以上（代码最低 API 26，旧版权限路径未完整验证） |
| 电脑桥 | Linux、Python 3.10+、`aiohttp`、`edge-tts`、`ffmpeg`、`libopus`、OpenSSL |
| ASR | 火山引擎流式识别账号，或任意 OpenAI 兼容的语音转写服务 |
| Agent | 任意 OpenAI 兼容 HTTP 接口，或一个可非交互调用的 CLI Agent |
| 外网（可选） | 公网 IPv6 + 域名 + Cloudflare DNS，或内网穿透/VPN |
| 浏览器（可选） | Chrome 或 Edge，用于网页刷写与 Web Bluetooth 调试 |

说明：

- 语音链路只使用 BLE，不依赖手表 Wi-Fi；手机与电脑之间需要网络可达。
- Windows 可以通过 WSL 运行桥，但音频、蓝牙与自签证书路径未做完整验证。
- 火山引擎 ASR 的凭据放在用户配置目录，不写入仓库；也可以用 `ASR_URL` 指向本地 ASR。
- 如果只用局域网，电脑与手机在同一网段即可；跨网访问需要公网地址或隧道。

## 5. 快速开始

完整步骤、命令和常见错误见 [`docs/01-quickstart.md`](docs/01-quickstart.md)。下面是五步概览，首次部署通常需要 1–2 小时。

### 第 1 步：刷固件

先备份手表原有固件与数据。仓库提供 `tools/flasher/` 网页刷写页（需要把构建产物放在同目录），也可以按 `docs/01-quickstart.md` 用 ESP-IDF 编译后写入。

自行编译时，若只想替换应用，写入 app 分区起始地址 `0x20000`；不要覆盖 bootloader、分区表和 NVS。网页刷写会写入 bootloader、分区表和 app，首次使用前确认可以接受整机覆盖。刷写完成后拔插一次 USB，避免芯片停在下载模式。

### 第 2 步：安装手机 App

用 Android Studio 打开 `android/`，构建并安装 Debug APK。首次启动授予蓝牙扫描、蓝牙连接和通知权限；App 会自动拉起前台服务。若系统限制后台运行，需要把 App 加入电池优化白名单。

### 第 3 步：启动电脑桥

在 `bridge/` 下创建 Python 虚拟环境，安装 `requirements.txt` 中的依赖（`aiohttp`、`edge-tts`），并确保系统已安装 `ffmpeg` 与 `libopus`。配置 ASR 凭据或本地 ASR，生成自签证书后启动桥；第一次建议用 `REPLY_MODE=echo`，不接大模型，直接回放识别文本，先验证链路。

### 第 4 步：在 App 里填地址并连接

手机与电脑在同一局域网时，填电脑地址加端口，例如 `192.168.1.10:8443`。跨网时填域名，例如 `watch.example.com`。留空则按“局域网 → DDNS → 本机回环”的顺序自动尝试。App 连上手表与桥后，日志会显示 BLE 与中继状态。

### 第 5 步：说话验证

在手表上打开“球球”，按住 A 说一句话，松手。手机日志应出现录音帧数与识别文本，电脑桥日志应出现 ASR 与回复，手表应播放回答。听到回声或回答即表示全链路打通。

链路打通后再编辑 `bridge/agents.json` 接入真实 Agent；ASR 已在前一步配置。不要一开始就同时调模型、ASR 和蓝牙，否则很难定位问题。

## 6. 仓库结构

| 路径 | 内容 |
|---|---|
| `firmware/` | ESP-IDF 固件：官方 UserDemo、球球应用、BLE 语音、HAL、电池遥测 |
| `firmware/main/apps/app_qiuqiu/` | 球球应用与程序化渲染引擎、15 个表情的数据 |
| `firmware/main/voice/` | 按键语义、ADPCM/Opus 音频路径、NimBLE GATT 服务 |
| `firmware/patches/` | 对 M5GFX 等第三方库的补丁说明 |
| `android/` | Android 中继 App：BLE 客户端、WebSocket、前台服务、开机自启 |
| `bridge/` | 电脑端 Python 桥：WebSocket/HTTP、ASR、Agent、TTS、Opus、DDNS |
| `docs/` | 快速开始、协议、配置、踩坑记录、省电报告等文档 |
| `tools/` | 网页刷写页、链路自检脚本、电池 CSV 分析脚本 |
| `licenses/` | Emotion Ball 社区许可与使用声明 |

根目录另有 `LICENSE`（MIT）、`SECURITY.md`、`CHANGELOG.md` 和 `.gitignore`。固件第三方依赖不随仓库提供，由 `firmware/fetch_repos.py` 拉取。

## 7. 配置说明

所有配置项的完整说明见 [`docs/04-config.md`](docs/04-config.md)。这里只列最常用的部分。

### 7.1 后台 Agent：`bridge/agents.json`

每个后台一项，可以任意增减。示例：

```json
{
  "default": "http-agent",
  "fallback": ["cli-agent"],
  "agents": {
    "http-agent": {
      "type": "http",
      "url": "http://192.168.1.10:8080/v1/chat/completions",
      "provider": "your-provider",
      "model": "your-model",
      "api_key_env": "RELAY_API_KEY",
      "timeout": 40,
      "aliases": ["小马"],
      "color": 0
    },
    "cli-agent": {
      "type": "cli",
      "cmd": ["your-agent-cli", "--print"],
      "timeout": 120,
      "aliases": ["小地"],
      "color": 1
    }
  }
}
```

| 字段 | 说明 |
|---|---|
| `default` | 当前默认后台的名称 |
| `type` | `http`（OpenAI 兼容接口）或 `cli`（命令行程序） |
| `url` / `provider` / `model` | HTTP 模式的接口地址、提供方与模型名；有的网关必须同时传 `provider` |
| `api_key_env` | 建议填 `RELAY_API_KEY`；当前 HTTP 后端固定从该环境变量读取 key |
| `env_file` / `env_keys` | CLI 模式可从文件注入指定环境变量 |
| `timeout` | 单轮超时，单位秒 |
| `aliases` | 语音切换别名；整句匹配、“换/叫/切到 + 别名”或“别名 + 分隔符 + 问题”会触发切换 |
| `color` | 示例配色索引（0 白 / 1 蓝 / 2 红）；当前 App 按后台名称硬编码映射，不读取此字段 |
| `fallback` | 主后台失败或返回空时的降级顺序 |

颜色索引 0/1/2 分别对应白/蓝/红；别名到颜色的绑定由 App 按后台名称映射决定。仓库的 `agents.json` 只保留两个占位示例，接入第三个后台时按同样格式添加即可。别名和配色都可以改；如果改了后台名称，需要同步调整 App 与桥里写死的名称映射，否则 B 键循环和配色会不一致。

### 7.2 桥的环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `RELAY_PORT` | `8443` | HTTPS/WSS 端口 |
| `RELAY_PLAIN_PORT` | `8444` | 明文 WS 端口，仅本机或可信内网 |
| `DOWNLINK_CODEC` | `opus` | 下行编码，可选 `opus` 或 `adpcm` |
| `OPUS_BITRATE` | `32000` | Opus 码率；BLE 不稳时可降到 `24000` |
| `OPUS_HEAD_START_MS` | `300` | 起播前背靠背推送的音频时长 |
| `REPLY_MODE` | `agent` | 设为 `echo` 时不接大模型，直接回放识别文本，用于链路测试 |
| `VOICE_HISTORY` | `1` | 设为 `0` 关闭多轮上下文 |
| `ASR_URL` | 本机 ASR 接口 | 云端 ASR 失败时的回退接口 |
| `RELAY_PY` | 当前解释器 | 调用 ASR 子进程使用的 Python |
| `DDNS_ZONE` | `example.com` | Cloudflare 区域 |
| `DDNS_RECORD` | `watch.example.com` | AAAA 记录名 |
| `DDNS_IFACE` | `eth0` | 有公网 IPv6 的网卡 |
| `CF_TOKEN_FILE` | 用户配置目录 | Cloudflare API token 文件路径 |

### 7.3 ASR 凭据

默认使用火山引擎流式识别。把 AppID、Access Token 和 Resource ID 写入用户配置目录下的 `volc_asr.env`（键名为 `VOLCENGINE_ASR_APP_ID`、`VOLCENGINE_ASR_ACCESS_TOKEN`、`VOLCENGINE_ASR_RESOURCE_ID`），不要提交到仓库。具体格式与本地 Whisper 替换方式见 [`docs/04-config.md`](docs/04-config.md)。

如果不想用云端 ASR，可以把 `ASR_URL` 指向本地 OpenAI 兼容的语音转写服务，或在桥里替换 `asr()` 实现；输入音频是 16 kHz 单声道 WAV，返回值应为纯文本。

### 7.4 App 地址格式

| 输入 | 实际连接 |
|---|---|
| `192.168.1.10:8443` | `wss://192.168.1.10:8443/ws` |
| `watch.example.com` | `wss://watch.example.com:8443/ws` |
| `2001:db8::1` | `wss://[2001:db8::1]:8443/ws` |
| `wss://watch.example.com/custom` | 原样使用 |
| 留空 | 局域网 → DDNS → 本机回环，依次尝试 |

### 7.5 固件可调参数

| 文件 | 参数 | 说明 |
|---|---|---|
| `firmware/main/voice/voice_task.cpp` | `PTT_LONG_MS` | 按住多久进入说话，默认 500 ms |
| 同上 | `IN_BLOCK` / `PREBUFFER` | 播放块 20 ms，起播预缓冲 200 ms |
| 同上 | 环形缓冲 `RING_SAMPLES` | 16 kHz 下 2 秒 |
| `firmware/main/voice/ble_svc.c` | 连接间隔两档 | 音频 30–50 ms，空闲 100–150 ms + latency 4 |
| `firmware/main/main.cpp` | `esp_pm_configure` | CPU DFS 160/80 MHz；light sleep 关闭 |
| `firmware/main/hal/hal_display.cpp` | `SCREEN_IDLE_MS` | 无操作熄屏，默认 5 秒 |
| `firmware/main/hal/hal_audio.cpp` | `idleCheck()` | 30 秒无音频关闭 codec 与功放 |
| `firmware/main/apps/app_qiuqiu/app_qiuqiu.cpp` | `FRAME_MS` / `FRAME_MS_IDLE` | 动画 20 fps，静止 8 fps |

省电功能依赖 `CONFIG_PM_ENABLE`、`CONFIG_BT_CTRL_MODEM_SLEEP` 与 `CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1`；使用仓库默认配置构建前应确认这些选项已启用。

### 7.6 密钥与证书

- API key 通过环境变量或本地文件注入，不写入 `agents.json`，也不提交到仓库。
- 桥使用自签证书时，`cert.pem` 与 `key.pem` 只放在本机，不随仓库分发。
- 手机端当前信任任意服务端证书，桥也不验证客户端身份；这套链路没有配对、绑定、鉴权或应用层加密，只应部署在可信网络，或通过 VPN/反向代理收敛访问。
- 外网暴露时至少限制来源地址，并定期更换 token。

## 8. 许可与致谢

### 8.1 许可

| 范围 | 许可 |
|---|---|
| 本项目代码 | MIT，见根目录 `LICENSE` |
| `firmware/main/apps/app_qiuqiu/qiuqiu_data.h` 与球球视觉形象 | Emotion Ball Community License（非商业） |
| 固件基线 | 基于 M5Stack `M5StopWatch-UserDemo`（MIT） |

**球球形象数据不是 MIT。** 球形角色的视觉形象仅允许个人学习、研究和技术交流，禁止商业用途；该部分不提供商业授权。商业使用前必须阅读 [`licenses/LICENSE-emotion-ball.txt`](licenses/LICENSE-emotion-ball.txt) 与 [`licenses/NOTICE-emotion-ball.md`](licenses/NOTICE-emotion-ball.md)，并替换为自有或已获授权的角色形象。表情引擎代码与表情配置数据的授权范围以 `licenses/` 中的原文为准。

### 8.2 致谢

- **M5Stack**：StopWatch 硬件、官方 UserDemo 固件与开发文档。
- **M5GFX / LovyanGFX**：AMOLED 面板驱动、M5GFX 绘图与 DMA 路径。
- **mooncake**：应用框架、启动器与生命周期管理。
- **LVGL**：设置页、启动器等界面的 UI 框架。
- **Emotion Ball 作者 sam70361**：球球眼环几何与表情数据的设计来源。
- **Espressif**：ESP-IDF、NimBLE、esp_codec_dev、esp-dsp 与 ESP32-S3 工具链。
- **Opus / libopus**：下行语音编码。
- **edge-tts**：文字转语音。
- **KK（Trentct/m5stack-stopwatch-avatar）**：程序化球体渲染思路的参考。
- **Cloudflare**：可选的 DDNS API。

固件第三方依赖不随仓库提供，由 `firmware/fetch_repos.py` 拉取；各组件的许可证以各自仓库为准。

## 9. 已知限制与隐私

### 9.1 续航仍在实测

截至 2026-10-23 的实测：**约 36 mA，450 mAh 折算约 12–13 小时**（熄屏、BLE 与手机保持连接、Opus 24 kbps 下行）。

原因很明确：**light sleep 在本硬件上不可用**，CPU 无法真正休眠。即使开 DFS 160/80 MHz，空闲时仍要 ~27 mA，加上 BLE 约 8 mA，就是 30 mA 以上。项目最初目标 3–4 天，需要 3.3 mA 级别的睡眠电流，只有让整机进入 light sleep 才可能达到——这需要重做屏与 PSRAM 驱动的电源管理适配，尚未完成。

对比参考：Espressif 官方数据中，无 32 kHz 晶振的 S3 在 BLE 保连 + light sleep 下典型值约 3.3 mA（有晶振 230 µA）；本板的 QSPI AMOLED 与 Octal PSRAM 会显著增加适配难度。

### 9.2 light sleep 需要前提

早期版本开启 light sleep 会挂死，原因是：调试日志每秒刷几十行、主循环无延时满速空转、屏/音频/会话都没有睡眠门禁。

现在的做法：

- **三道门禁**：屏幕亮、音频播放、语音会话进行中 → 禁止睡眠；三者都空闲才允许睡；
- 熄屏时停 LVGL 定时器、关调试日志、降低日志级别；
- 按键（GPIO 低电平）唤醒，唤醒后立即收回门禁再点屏。

实测：熄屏后串口断流（说明真的在睡），连续 40 分钟无异常。深度睡眠仍未实现（会断开 BLE）。

### 9.3 需要外网与常开设备

- 手机必须运行中继 App，电脑必须运行桥；任一环节休眠、断网或被系统清理，链路都会中断。
- 默认 ASR、Agent、TTS 都可能依赖外网；完全离线需要自行部署本地 ASR、本地模型与本地 TTS。
- 跨网访问需要公网 IPv6、域名、内网穿透或 VPN；没有内置 NAT 穿透服务。
- 蓝牙是 2.4 GHz 链路，距离、遮挡和 Wi-Fi 干扰都会影响音频连续性。

### 9.4 隐私提示

- **默认云端 ASR。** 按住 A 期间的语音会被上传到火山引擎流式识别服务；如果配置了其他云端 ASR，音频同样会离开本机。
- **转写文本会发给 Agent。** 桥把识别文本发送到 `agents.json` 中配置的 HTTP 接口或 CLI 程序，具体数据去向取决于所接的服务。
- **TTS 文本会发给 edge-tts。** 回复文本会发送到 edge-tts 服务合成语音。
- **本机临时文件。** 桥会把录音与合成的 WAV/MP3 临时写到本机临时目录，便于排查；长期使用应定期清理。
- **没有唤醒词。** 麦克风只在按住 A 时采集，不会常听；但手机 App 与桥仍保持常驻连接。
- **链路没有鉴权。** BLE 没有配对与加密，桥的 WebSocket 也没有账号体系；自签证书只提供传输层加密；手机端不验证服务端身份，桥也不验证客户端身份。不要把 8443/8444 直接暴露到不可信网络。更完整的安全边界与报告方式见 [`SECURITY.md`](SECURITY.md)。

### 9.5 其他限制

- **单手机、单会话。** 桥只保留最后一个连接，多手机同时连接的行为未定义。
- **网页调试页只验证过 ADPCM。** 网页端把下行包头写死为 ADPCM，使用 Opus 下行时不要用网页页做播放验证。
- **iOS 没有原生 App。** 网页页依赖 Chrome 的 Web Bluetooth，iOS Safari 不支持。
- **电池遥测是内存环。** 1440 条样本存在 PSRAM，重启后清空；跨重启的续航分析需要连续不重启采样。
- **音频质量受硬件限制。** 小喇叭在接近满幅时会出现机械失真，固件不做软件增益；Opus 24 kbps 也不是高保真音乐编码。
- **延迟以秒计。** 端到端时间主要花在 ASR 与 Agent 上，具体取决于网络和后台；不适合对实时性要求高的对话。
- **没有 OTA 界面。** 升级需要重新刷写；分区表保留了 OTA 槽位，但没有做应用内升级流程。

## 10. 故障排查与项目状态

### 10.1 先检查这五件事

1. 手机是否授予蓝牙扫描、蓝牙连接、通知权限，App 是否在前台服务运行。
2. 手表是否广播 `StopWatch-BLE`，App 日志是否显示已连接手表与中继。
3. 电脑桥是否监听 8443/8444，证书是否与 App 的信任策略匹配。
4. `DOWNLINK_CODEC` 是否与固件匹配；不确定时先用 `adpcm` 对比。
5. 先用 `REPLY_MODE=echo` 验证“录音→识别→回放”，再接入 Agent。

更多现象、根因与修复见 [`docs/05-pitfalls.md`](docs/05-pitfalls.md)，协议细节见 [`docs/02-protocol.md`](docs/02-protocol.md)，省电结论见 [`docs/06-power.md`](docs/06-power.md)。

### 10.2 项目状态

- 当前版本 `0.1.0`，对应 [`CHANGELOG.md`](CHANGELOG.md)。
- 端到端链路已在真机跑通：按住 A 说话、松手、识别、Agent 回复、手表播放。
- 播放打断、B 键切换、语音切换、充电灯、电池 CSV 导出均已接入。
- 续航仍在测试；最新真机实测约 **12–13 小时**（熄屏、BLE 保连，见 9.1），3–4 天的目标取决于 light sleep 能否落地。
- 未验证或未完成：长期稳定性、24 kbps 下行的抓包验证、多手机并发、iOS 客户端、量产测试与 OTA 升级。
- 首次部署以 [`docs/01-quickstart.md`](docs/01-quickstart.md) 的步骤为准，遇到与本文不一致的地方以代码和该文档为准。
