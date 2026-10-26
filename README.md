# M5Stack Qiuqiu

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Web Flasher](https://img.shields.io/badge/flash-in%20browser-brightgreen)](https://wen0531.github.io/m5stack-qiuqiu/flasher/)
[![Latest release](https://img.shields.io/github/v/release/wen0531/m5stack-qiuqiu)](https://github.com/wen0531/m5stack-qiuqiu/releases)

**English** | [中文](README.zh-CN.md)

> **Flash in one click**: open <https://wen0531.github.io/m5stack-qiuqiu/flasher/> on a phone or computer,
> connect the watch over USB and flash it in the browser — no toolchain required.

> Author: wen0531 · 2026 · Code is MIT; the ball character follows the Emotion Ball Community Licence (non-commercial)

M5Stack Qiuqiu turns an M5Stack StopWatch (ESP32-S3) into a wearable voice assistant.
Hold button A on the ball screen to speak; speech recognition, agent inference and speech synthesis
run on the phone and a PC, and the answer comes back through the watch's speaker.
The watch only captures, plays and handles interaction — all heavy lifting stays off the MCU.

This is a personal project meant for self-use and further development, not a mass-market product.
It keeps the original UserDemo launcher, watch faces, stopwatch and alarm apps intact.

The motivation is simple: a device you can raise and ask a question, without changing phones,
carrying another pair of earbuds or wearing a second smartwatch. The StopWatch happens to have a
microphone, a speaker, a round AMOLED and an ESP32-S3 capable of BLE and Opus, so it became the
"listen and speak" terminal while the phone and PC do the thinking.

## Table of contents

- [1. Overview](#1-overview)
- [2. Capabilities and boundaries](#2-capabilities-and-boundaries)
- [3. Architecture and data flow](#3-architecture-and-data-flow)
- [4. Hardware and software prerequisites](#4-hardware-and-software-prerequisites)
- [5. Quick start](#5-quick-start)
- [6. Repository layout](#6-repository-layout)
- [7. Configuration](#7-configuration)
- [8. Licence and credits](#8-licence-and-credits)
- [9. Known limitations and privacy](#9-known-limitations-and-privacy)
- [10. Troubleshooting and project status](#10-troubleshooting-and-project-status)

---

## 1. Overview

In one line: **watch records → BLE → phone relay → WebSocket → PC bridge → ASR → LLM agent → TTS → Opus → BLE → watch playback.**

```
 [M5Stack StopWatch C152]
   │   BLE: uplink IMA-ADPCM / downlink Opus
   ▼
 [Android phone · RelayService foreground service]
   │   WSS
   ▼
 [PC · Python bridge bridge.py]
   ├──► streaming ASR (cloud by default) ──► text
   ├──► LLM agent (HTTP / CLI) ──► reply text
   └──► edge-tts ──► 16 kHz WAV ──► Opus 24 kbps
   │
   └──► back to the watch for playback
```

Design decisions:

- **The watch is only a terminal.** ESP32-S3 runs no ASR, LLM or TTS — no unrealistic on-device inference, and no extra heat or drain on a 450 mAh battery.
- **BLE instead of Wi-Fi.** Audio uses a custom GATT service, with connection intervals split between "speaking/playing" and "idle" to reduce radio duty cycle.
- **The phone relays.** An Android foreground service bridges BLE and WebSocket and survives screen-off or app switching.
- **The PC orchestrates.** A Python bridge handles ASR, agent, TTS, Opus encoding, fallbacks and logging, so swapping models or voice services never needs a firmware change.
- **The character is procedural.** The ball is drawn in real time from C++ geometry and spring animation with no image assets, which makes recolouring, new expressions and derivative work straightforward.

## 2. Capabilities and boundaries

### 2.1 What it does

The default character is a pure white sphere with black eyes and no mouth; the sphere turns white, blue or red when the backend agent changes.
It breathes, deforms slightly like jelly, blinks and transitions smoothly between expressions.

| Capability | Notes | Status |
|---|---|---|
| Hold A to speak | Only on the ball screen; holding past 500 ms starts push-to-talk, releasing sends automatically | Verified on device |
| Press A to interrupt playback | Drops remaining downlink audio, clears buffers, tells the bridge to stop sending | Verified on device |
| B cycles the backend | Ball recolours locally (white/blue/red), bridge switches the agent asynchronously; rapid presses are not lost | Verified on device |
| Voice-driven backend switch | Say an alias such as "小马 / 小地 / 大地"; "alias + question" is supported | Implemented |
| 15 expressions | NEUTRAL, HAPPY, SAD, ANGRY, DOUBT, SLEEPY, LISTENING, THINKING, EXCITED, CURIOUS, CONFUSED, SURPRISED, DIZZY, AFFECTION, BORED | Implemented |
| Expression driven by voice state | Listening, thinking, speaking, happy, sleepy, idle switch automatically | Implemented |
| Procedural ball | Deforming sphere + spring physics + Chaikin smoothing + scanline antialiasing, no image assets | Implemented |
| Opus 24 kbps downlink | 60 ms frames, fixed 20 ms playback cadence, 200 ms start prebuffer | Verified on device |
| Custom BLE GATT | Control, audio and status characteristics; MTU 400; automatic reconnect | Verified on device |
| Power savings | Light sleep when the screen is off, 240 MHz while awake, BLE modem sleep, on-demand amplifier | Verified (40 min, no faults) |
| **A/B dual slots** | Settings shows slots, switches systems, clones backups; a new image that fails self-test rolls back automatically | Implemented |
| **Ten UI languages** | en / 简体中文 / 繁體中文 / 日本語 / 한국어 / Français / Deutsch / Español / Русский / Latina; subset fonts add only ~160 KB; includes launcher and settings sub-pages | Implemented |
| **Language picker page** | Settings → Language opens a list; each row uses its own font and the active language is highlighted | Implemented |
| Charging LED | Green while USB is plugged in, off when unplugged | Verified on device |
| Battery telemetry | One sample per minute, 24 h ring buffer, CSV export and average-current analysis | Implemented |
| Remote access | Optional public IPv6 + DDNS; the phone can connect over 5G | Optional |
| Web debug page | Chrome Web Bluetooth connects to the watch for link testing without the app; follows the bridge codec announcement | Optional |

### 2.2 Explicit boundaries

- **Not a standalone device.** Without the phone and PC the watch only shows the ball and local apps; it cannot answer on its own.
- **The phone must stay on.** The Android app needs a foreground service plus Bluetooth and notification permissions; battery optimisers may kill it, so whitelist it.
- **The PC must stay on.** The bridge runs ASR, agent and TTS; suspend or network loss breaks the link.
- **Cloud by default.** The default ASR is a cloud service and the agent/TTS usually need internet too. Fully offline deployments must bring their own local ASR, model and TTS.
- **No wake word.** The microphone only records while A is held; there is no always-listening or keyword spotting.
- **Single user, single session.** The bridge keeps only the last phone connection; there are no accounts, pairing, auth or multi-device management.
- **Chinese first.** The default TTS voice is Chinese; other languages need configuration and depend on ASR quality.
- **Not a production firmware.** No OTA UI, no factory test flow, no long-term stability guarantee.

## 3. Architecture and data flow

### 3.1 Watch firmware

Based on M5Stack's official `M5StopWatch-UserDemo`, keeping the stock launcher and all built-in apps, plus a new `Qiuqiu` app. Main modules:

| Module | Location | Responsibility |
|---|---|---|
| App framework | `firmware/main/apps/` | Launcher, watch faces, stopwatch, alarm, IMU, FFT, badge, lucky wheel, settings, ball |
| Ball app | `firmware/main/apps/app_qiuqiu/` | Expression state machine, frame buffers, touch/button interaction, colours |
| Ball engine | `qiuqiu_engine.cpp` | Deforming sphere, eye outlines, spring physics, antialiased fill, blink and gaze |
| Voice task | `firmware/main/voice/voice_task.cpp` | Button semantics, 44.1 kHz capture, 16 kHz resampling, ADPCM encode, downlink decode and playback |
| BLE service | `firmware/main/voice/ble_svc.c` | NimBLE peripheral, GATT characteristics, command dispatch, connection parameter switching |
| Battery telemetry | `firmware/main/voice/batt_log.cpp` | Per-minute VBAT/level sampling, ring buffer, CSV export |
| HAL | `firmware/main/hal/` | CO5300 AMOLED, ES8311, CST820 touch, BMI270, M5PM1 power management |

The ball does not render through an LVGL canvas: the engine draws into an RGB565 PSRAM framebuffer, upscales 2× with nearest-neighbour and pushes it straight to the M5GFX panel. That avoids LVGL image blit copies and tearing. About 20 fps while animating, ~8 fps when static, no rendering while the screen is off.

### 3.2 Phone app

`android/` is a Kotlin app whose core is the `RelayService` foreground service:

- Scans for `StopWatch-BLE`, connects, remembers the MAC and prefers background reconnects.
- Subscribes to the audio and status characteristics, strips the 4-byte header from uplink audio and sends it to the bridge over WebSocket.
- Prepends the header to downlink audio from the bridge and writes it to the watch.
- Handles control messages such as `start`, `stop`, `agent_next`, `stop_play`, `battery`.
- Runs a scan watchdog that restarts a stuck scan.
- Starts itself on boot to reduce manual steps.

The app accepts three address sources: LAN address, DDNS domain, or local loopback (for `adb reverse`). An empty field tries them in that order.

### 3.3 PC bridge

`bridge/` is a Python 3 aiohttp service exposing:

- **HTTPS/WSS 8443**: encrypted entry for the web page and native app.
- **Plain WS 8444**: local or trusted-LAN debugging only.
- **HTTP/WS routes**: `/ws`, `/agent`, `/battery` for sessions, backend query/switch and battery CSV export.
- **Audio orchestration**: uplink ADPCM decoded to 16 kHz WAV; downlink TTS encoded as Opus.
- **ASR**: Volcengine streaming by default, falling back to any OpenAI-compatible `/v1/audio/transcriptions` endpoint.
- **Agent**: reads `agents.json`, supports HTTP (OpenAI-compatible) and CLI backends, degrading through `fallback` on failure.
- **TTS**: `edge-tts` produces MP3, `ffmpeg` converts to 16 kHz mono WAV.
- **Utilities**: `selftest.py` health check, `battery_report.py` runtime estimate, `ddns.py` Cloudflare AAAA updates.

The bridge keeps the last six turns of context and clears it after 30 idle minutes or a backend switch. Full context applies to HTTP backends; CLI backends receive the recent turns inside the prompt.

### 3.4 One full round trip

1. On the ball screen the watch detects an A press of ≥ 500 ms and records; ES8311 captures at 44.1 kHz and interpolates to 16 kHz.
2. Every 20 ms it encodes 160 bytes of IMA-ADPCM, adds a 4-byte header and notifies the phone over the audio characteristic.
3. The phone strips the header and sends the 160-byte payload over WebSocket; the bridge decodes to 16 kHz PCM and writes a WAV on release.
4. The bridge runs ASR, checks for a backend-switch alias, switches if needed and passes the rest of the question to the agent.
5. The agent returns text; the bridge synthesises with edge-tts and converts to 16 kHz mono WAV with `ffmpeg`.
6. The bridge encodes Opus CBR 24 kbps with 60 ms frames and sends them on the media clock; the phone adds headers and writes them to the watch.
7. The watch decodes, resamples to 44.1 kHz, feeds the speaker on a fixed 20 ms cadence and stops after 2 s without new data.

### 3.5 BLE protocol summary

Service UUID prefix is `4f5a00xx-1e3b-4d8a-9c7e-5a1b2c3d4e5f`.

| Characteristic | UUID suffix | Direction | Purpose |
|---|---|---|---|
| Control | `0002` | Phone → watch | Talk toggle, commands, colour, battery export |
| Audio | `0003` | Bidirectional | Uplink notifications, downlink writes, events and debug channel |
| Status | `0004` | Watch → phone | Connection/subscription/talk state, playback events |

Audio packets are `u16 seq (LE) | u8 codec | u8 flags | payload`:

- **Uplink**: `codec=1`, IMA-ADPCM 4 bit, 16 kHz, 20 ms / 160 B payload (~64 kbps raw).
- **Downlink**: `codec=2`, Opus 16 kHz mono, 60 ms frames, 24 kbps CBR by default, 240 B payload.
- **Alternative downlink**: `codec=1`, ADPCM, 20 ms / 160 B, for older firmware or quality comparison.
- **Events/debug**: `codec=0x7E` events, `codec=0x7F` debug passthrough (battery CSV, framebuffer dumps).

Connection parameters come in two tiers: 30–50 ms with latency 0 while talking or playing; 100–150 ms with slave latency 4 when idle. Advertising starts at 100 ms for 30 s after boot or disconnect, then drops to 1 s. Full fields, command codes and error fallbacks are in [`docs/02-protocol.md`](docs/02-protocol.md).

## 4. Hardware and software prerequisites

### 4.1 Hardware

| Part | Requirement |
|---|---|
| Watch | M5Stack StopWatch C152 (ESP32-S3R8) |
| SoC | ESP32-S3, dual-core, up to 240 MHz |
| Memory | 8 MB Octal PSRAM, 16 MB flash |
| Display | 466×466 AMOLED, CO5300, QSPI |
| Audio | ES8311 codec, on-board microphone and speaker |
| Touch | CST820 |
| Sensor | BMI270 |
| Power | M5PM1 PMIC, 450 mAh battery |
| Port | USB-C with data and charging |

### 4.2 Software and accounts

| Purpose | Requirement |
|---|---|
| Firmware build | ESP-IDF v5.5.4, Python 3, Git; the first dependency fetch is about 1 GB |
| Phone app | Android Studio or Android SDK 34, JDK 17; Android 12 (API 31) or newer recommended (min SDK 26, legacy permission paths not fully verified) |
| PC bridge | Linux, Python 3.10+, `aiohttp`, `edge-tts`, `ffmpeg`, `libopus`, OpenSSL |
| ASR | A Volcengine streaming account, or any OpenAI-compatible transcription service |
| Agent | Any OpenAI-compatible HTTP endpoint, or a CLI agent callable non-interactively |
| Internet (optional) | Public IPv6 + domain + Cloudflare DNS, or a tunnel/VPN |
| Browser (optional) | Chrome or Edge for web flashing and Web Bluetooth debugging |

Notes:

- The voice link uses BLE only and never touches the watch's Wi-Fi; the phone and PC still need network reachability.
- The bridge can run under WSL on Windows, but audio, Bluetooth and self-signed certificate paths are not fully verified.
- Volcengine ASR credentials live in the user config directory, never in the repository; `ASR_URL` can point at a local ASR instead.
- For LAN-only use the phone and PC simply need to share a subnet; remote access needs a public address or a tunnel.

## 5. Quick start

Full steps, commands and common errors are in [`docs/01-quickstart.md`](docs/01-quickstart.md). Five steps below; a first deployment usually takes 1–2 hours.

### Step 1: flash the firmware

Back up the watch's existing firmware and data first. The repository ships a browser flasher in `tools/flasher/` (build outputs must sit in the same directory), or you can build with ESP-IDF and flash as described in `docs/01-quickstart.md`.

When building yourself, writing only the app partition at `0x20000` replaces the application without touching bootloader, partition table or NVS. The web flasher writes bootloader, partition table and app, so confirm you accept a full-image overwrite before using it. After flashing, unplug and replug USB once so the chip does not stay in download mode.

### Step 2: install the phone app

Open `android/` in Android Studio, build and install the debug APK. Grant Bluetooth scan, Bluetooth connect and notification permissions on first launch; the app starts its foreground service automatically. If the system restricts background work, whitelist the app from battery optimisation.

### Step 3: start the PC bridge

Create a Python virtual environment under `bridge/`, install `requirements.txt` (`aiohttp`, `edge-tts`) and make sure `ffmpeg` and `libopus` are available. Configure ASR credentials or a local ASR, generate a self-signed certificate and start the bridge. For the first run use `REPLY_MODE=echo` — no LLM, just echo the transcript — to validate the link.

### Step 4: point the app at the bridge

On the same LAN, enter the PC address and port, e.g. `192.168.1.10:8443`. Across networks use a domain, e.g. `watch.example.com`. Leaving it empty tries LAN → DDNS → loopback in order. Once the app connects to both the watch and the bridge, its log shows BLE and relay status.

### Step 5: speak to verify

Open "Qiuqiu" on the watch, hold A, say a sentence and release. The phone log should show frame counts and recognised text, the bridge log should show ASR and reply, and the watch should play the answer. Hearing the echo or the answer means the whole chain works.

Only after that, edit `bridge/agents.json` to plug in a real agent (ASR was configured in the previous step). Do not debug model, ASR and Bluetooth at the same time — it makes problems impossible to isolate.

## 6. Repository layout

| Path | Contents |
|---|---|
| `firmware/` | ESP-IDF firmware: official UserDemo, ball app, BLE voice, HAL, battery telemetry |
| `firmware/main/apps/app_qiuqiu/` | Ball app, procedural renderer and the 15-expression data |
| `firmware/main/voice/` | Button semantics, ADPCM/Opus audio paths, NimBLE GATT service |
| `firmware/patches/` | Patch notes for third-party libraries such as M5GFX |
| `android/` | Android relay app: BLE client, WebSocket, foreground service, boot autostart |
| `bridge/` | PC Python bridge: WebSocket/HTTP, ASR, agent, TTS, Opus, DDNS |
| `docs/` | Quick start, protocol, configuration, pitfalls, power report and more |
| `tools/` | Web flasher page, link self-test, battery CSV analysis |
| `licenses/` | Emotion Ball Community Licence and usage notice |

The root also contains `LICENSE` (MIT), `SECURITY.md`, `CHANGELOG.md` and `.gitignore`. Firmware third-party dependencies are not vendored; `firmware/fetch_repos.py` fetches them.

## 7. Configuration

Every option is documented in [`docs/04-config.md`](docs/04-config.md). The most common ones are below.

### 7.1 Backend agents: `bridge/agents.json`

One entry per backend; add or remove as you like. Example:

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

| Field | Meaning |
|---|---|
| `default` | Name of the current default backend |
| `type` | `http` (OpenAI-compatible endpoint) or `cli` (command-line program) |
| `url` / `provider` / `model` | Endpoint, provider and model for HTTP; some gateways need `provider` as well |
| `api_key_env` | Recommended value `RELAY_API_KEY`; the HTTP backend always reads the key from this variable |
| `env_file` / `env_keys` | CLI mode can inject selected environment variables from a file |
| `timeout` | Per-turn timeout in seconds |
| `aliases` | Voice switch aliases; a whole-sentence match, "switch/call/use + alias", or "alias + separator + question" triggers a switch |
| `color` | Example colour index (0 white / 1 blue / 2 red); the app currently maps colours by backend name and ignores this field |
| `fallback` | Degradation order when the primary backend fails or returns empty |

Colour indices 0/1/2 mean white/blue/red; the app binds aliases to colours by backend name. The shipped `agents.json` keeps two placeholder examples — add a third in the same shape. Aliases and colours are free to change, but renaming a backend requires updating the hardcoded name maps in both the app and the bridge, otherwise button-B cycling and colours drift.

### 7.2 Bridge environment variables

| Variable | Default | Purpose |
|---|---|---|
| `RELAY_PORT` | `8443` | HTTPS/WSS port |
| `RELAY_PLAIN_PORT` | `8444` | Plain WS port, local or trusted LAN only |
| `DOWNLINK_CODEC` | `opus` | Downlink codec, `opus` or `adpcm` |
| `OPUS_BITRATE` | `32000` | Opus bitrate; drop to `24000` if BLE is unstable |
| `OPUS_HEAD_START_MS` | `300` | Audio pushed back-to-back before playback starts |
| `REPLY_MODE` | `agent` | `echo` skips the LLM and replays the transcript, for link testing |
| `VOICE_HISTORY` | `1` | Set `0` to disable multi-turn context |
| `ASR_URL` | local ASR endpoint | Fallback endpoint when cloud ASR fails |
| `RELAY_PY` | current interpreter | Python used for the ASR subprocess |
| `DDNS_ZONE` | `example.com` | Cloudflare zone |
| `DDNS_RECORD` | `watch.example.com` | AAAA record name |
| `DDNS_IFACE` | `eth0` | Interface carrying the public IPv6 address |
| `CF_TOKEN_FILE` | user config directory | Path to the Cloudflare API token file |

### 7.3 ASR credentials

Volcengine streaming recognition is the default. Put AppID, Access Token and Resource ID into `volc_asr.env` in the user config directory (`VOLCENGINE_ASR_APP_ID`, `VOLCENGINE_ASR_ACCESS_TOKEN`, `VOLCENGINE_ASR_RESOURCE_ID`) and never commit it. Format and local-Whisper alternatives are in [`docs/04-config.md`](docs/04-config.md).

To avoid cloud ASR, point `ASR_URL` at a local OpenAI-compatible transcription service or replace `asr()` in the bridge; input audio is 16 kHz mono WAV and the return value must be plain text.

### 7.4 App address formats

| Input | Actual connection |
|---|---|
| `192.168.1.10:8443` | `wss://192.168.1.10:8443/ws` |
| `watch.example.com` | `wss://watch.example.com:8443/ws` |
| `2001:db8::1` | `wss://[2001:db8::1]:8443/ws` |
| `wss://watch.example.com/custom` | Used verbatim |
| empty | LAN → DDNS → loopback, tried in order |

### 7.5 Firmware tuning parameters

| File | Parameter | Meaning |
|---|---|---|
| `firmware/main/voice/voice_task.cpp` | `PTT_LONG_MS` | Hold time before talking, 500 ms by default |
| same | `IN_BLOCK` / `PREBUFFER` | 20 ms playback block, 200 ms start prebuffer |
| same | ring buffer `RING_SAMPLES` | 2 seconds at 16 kHz |
| `firmware/main/voice/ble_svc.c` | two interval tiers | 30–50 ms for audio, 100–150 ms + latency 4 when idle |
| `firmware/main/main.cpp` | `esp_pm_configure` | CPU DFS 160/80 MHz; light sleep disabled |
| `firmware/main/hal/hal_display.cpp` | `SCREEN_IDLE_MS` | Screen-off timeout, 5 s by default |
| `firmware/main/hal/hal_audio.cpp` | `idleCheck()` | Turn codec and amplifier off after 30 s of silence |
| `firmware/main/apps/app_qiuqiu/app_qiuqiu.cpp` | `FRAME_MS` / `FRAME_MS_IDLE` | 20 fps animating, 8 fps static |

Power features depend on `CONFIG_PM_ENABLE`, `CONFIG_BT_CTRL_MODEM_SLEEP` and `CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1`; confirm they are enabled before building with the shipped defaults.

### 7.6 Keys and certificates

- API keys come from environment variables or local files, never from `agents.json`, and are never committed.
- With a self-signed certificate, `cert.pem` and `key.pem` stay on the local machine.
- The phone currently trusts any server certificate and the bridge does not authenticate clients; there is no pairing, binding, auth or application-layer encryption, so deploy only on trusted networks or behind a VPN/reverse proxy.
- When exposing to the internet, restrict source addresses and rotate tokens regularly.

## 8. Licence and credits

### 8.1 Licence

| Scope | Licence |
|---|---|
| This project's code | MIT, see root `LICENSE` |
| `firmware/main/apps/app_qiuqiu/qiuqiu_data.h` and the ball's visual character | Emotion Ball Community Licence (non-commercial) |
| Firmware baseline | Built on M5Stack `M5StopWatch-UserDemo` (MIT) |

**The ball character data is not MIT.** The visual character may be used for personal study, research and technical exchange only; commercial use is prohibited and no commercial licence is offered. Before any commercial use, read [`licenses/LICENSE-emotion-ball.txt`](licenses/LICENSE-emotion-ball.txt) and [`licenses/NOTICE-emotion-ball.md`](licenses/NOTICE-emotion-ball.md) and substitute a character you own or have licensed. The licence scope for the expression engine and expression data is defined by the original texts under `licenses/`.

### 8.2 Credits

- **M5Stack**: StopWatch hardware, official UserDemo firmware and documentation.
- **M5GFX / LovyanGFX**: AMOLED panel driver, drawing and DMA paths.
- **mooncake**: app framework, launcher and lifecycle management.
- **LVGL**: UI framework for settings, launcher and other screens.
- **sam70361, author of Emotion Ball**: origin of the eye-ring geometry and expression data design.
- **Espressif**: ESP-IDF, NimBLE, esp_codec_dev, esp-dsp and the ESP32-S3 toolchain.
- **Opus / libopus**: downlink speech encoding.
- **edge-tts**: text to speech.
- **KK (Trentct/m5stack-stopwatch-avatar)**: reference for the procedural sphere rendering approach.
- **Cloudflare**: optional DDNS API.

Firmware third-party dependencies are not vendored; `firmware/fetch_repos.py` fetches them and each component keeps its own licence.

## 9. Known limitations and privacy

### 9.1 Battery life is still being measured

Measured as of 2026-10-23: **about 36 mA, roughly 12–13 hours from 450 mAh** (screen off, BLE connected to the phone, Opus 24 kbps downlink).

The reason is simple: **light sleep is not usable on this hardware**, so the CPU never really sleeps. Even with DFS at 160/80 MHz, idle costs ~27 mA; add ~8 mA for BLE and it exceeds 30 mA. The original 3–4 day target needs ~3.3 mA and therefore a proper light-sleep configuration, which requires reworking the display and PSRAM power management and has not been completed.

For reference, Espressif quotes roughly 3.3 mA for BLE-connected light sleep on an S3 without a 32 kHz crystal (230 µA with one); this board's QSPI AMOLED and Octal PSRAM make the adaptation significantly harder.

### 9.2 Light sleep needs prerequisites

Enabling light sleep in early builds hung the device: debug logging printed dozens of lines per second, the main loop spun with no delay, and display, audio and session had no sleep gating.

Current approach:

- **Three gates**: screen on, audio playing, or a voice session active all block sleep; only when all three are idle may it sleep;
- With the screen off, the LVGL tick timer stops, debug logging is disabled and the log level is lowered;
- Wake on button GPIO low level, then re-acquire the gates before touching the panel.

Measured: UART output stops once the screen is off (proving it sleeps) and 40 minutes passed without faults. Deep sleep is still unimplemented (it would drop BLE).

### 9.3 Internet and always-on devices

- The phone must run the relay app and the PC must run the bridge; suspend, network loss or being killed breaks the link.
- Default ASR, agent and TTS may all need internet; fully offline use requires your own ASR, model and TTS.
- Remote access needs public IPv6, a domain, a tunnel or a VPN; there is no built-in NAT traversal.
- Bluetooth is a 2.4 GHz link; distance, obstruction and Wi-Fi interference all affect audio continuity.

### 9.4 Privacy notes

- **Cloud ASR by default.** Speech recorded while A is held is uploaded to Volcengine streaming recognition; other cloud ASR services have the same effect.
- **Transcripts go to the agent.** The bridge sends recognised text to the HTTP endpoint or CLI program configured in `agents.json`; where it goes depends on the service you choose.
- **TTS text goes to edge-tts.** Reply text is sent to the edge-tts service for synthesis.
- **Local temporary files.** The bridge writes recorded and synthesised WAV/MP3 files to the local temp directory for debugging; clean them periodically.
- **No wake word.** The microphone only records while A is held, but the app and bridge keep persistent connections.
- **The link has no authentication.** BLE has no pairing or encryption, the bridge WebSocket has no accounts, self-signed certificates only provide transport encryption, the phone does not verify the server and the bridge does not verify clients. Never expose 8443/8444 to untrusted networks. See [`SECURITY.md`](SECURITY.md) for the full boundary and reporting.

### 9.5 Other limitations

- **One phone, one session.** The bridge keeps only the latest connection; multi-phone behaviour is undefined.
- **The web debug page only verified ADPCM.** It hardcodes the ADPCM downlink header, so do not use it to verify Opus playback.
- **No native iOS app.** The web page depends on Chrome Web Bluetooth, which iOS Safari does not support.
- **Battery telemetry is an in-memory ring.** 1440 samples live in PSRAM and are lost on reboot; cross-reboot analysis needs continuous sampling.
- **Audio quality is hardware-limited.** The small speaker distorts mechanically near full scale and the firmware applies no software gain; Opus 24 kbps is not hi-fi either.
- **Latency is measured in seconds.** End-to-end time is dominated by ASR and the agent; it is not suitable for latency-critical conversation.
- **No OTA UI.** Upgrading means reflashing; the partition table keeps OTA slots but no in-app update flow exists.

## 10. Troubleshooting and project status

### 10.1 Check these five things first

1. Did the phone grant Bluetooth scan, connect and notification permissions, and is the foreground service running?
2. Does the watch advertise `StopWatch-BLE`, and does the app log show both watch and relay connected?
3. Is the bridge listening on 8443/8444, and does the certificate match the app's trust policy?
4. Does `DOWNLINK_CODEC` match the firmware? If unsure, compare with `adpcm` first.
5. Validate "record → recognise → replay" with `REPLY_MODE=echo` before attaching an agent.

More symptoms, root causes and fixes are in [`docs/05-pitfalls.md`](docs/05-pitfalls.md); protocol details in [`docs/02-protocol.md`](docs/02-protocol.md); power findings in [`docs/06-power.md`](docs/06-power.md).

### 10.2 Project status

- Current version `0.1.0`, see [`CHANGELOG.md`](CHANGELOG.md).
- The end-to-end chain works on real hardware: hold A, speak, release, recognise, agent reply, watch playback.
- Playback interruption, button-B switching, voice switching, the charging LED and battery CSV export are all wired up.
- Battery life is still under test; the latest device measurement is **about 12–13 hours** with the screen off (see 9.1). Reaching the 3–4 day goal depends on making light sleep work.
- Unverified or unfinished: long-term stability, packet captures of the 24 kbps downlink, concurrent phones, an iOS client, factory testing and OTA updates.
- For a first deployment follow [`docs/01-quickstart.md`](docs/01-quickstart.md); where this document and the code disagree, the code and that guide win.

## 视频渲染测试（临时）

https://github.com/user-attachments/assets/ba25d027-334d-4b94-81de-2ca41a32aa31
