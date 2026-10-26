# Changelog · 更新日志

English first, then 中文. Format follows [Keep a Changelog](https://keepachangelog.com/).

> **Correction (2026-10-26)**: the previous `v1.1` section mixed in work that shipped in v1.2.0
> (multilingual UI, simulator, time-sync commands). Entries are now grouped by the release that
> actually contained them.
>
> **更正（2026-10-26）**：上一版 `v1.1` 段落混入了实际随 v1.2.0 发布的内容
> （多语言、模拟器、对时命令）。以下按真实发布版本重新归类。

---

# English

## v1.2.0 (2026-10-26)

### Added

- **Ten-language UI**: English / 简体中文 / 繁體中文 / 日本語 / 한국어 / Français / Deutsch /
  Español / Русский / Latina
  - Subset fonts: each language packs only the glyphs actually used — the firmware grows by ~160 KB
  - Launcher app names follow the language; labels switched to a font that carries CJK glyphs
  - Settings, alarm, stopwatch, watch-face weekday labels and the button/time/date sub-pages are
    all localised (23 new strings)
- **Language picker page**: Settings → Language opens a ten-row list instead of cycling on tap;
  each row is rendered in its own font, the active language is highlighted, and the menu rebuilds
  on return
- **Host simulator** (`sim/`): runs the real firmware UI on a PC — screenshots, key/touch injection,
  widget enumeration, crash and hang detection; a unified virtual panel captures both the LVGL
  flush path and the ball's direct M5GFX writes
- **Simulator web UI** (`sim/web/`): `http://127.0.0.1:8099/` with a live ~12 fps view, A/B/A+B keys,
  tap and swipe, ten-language switching, status bar and log panel
- **Deterministic simulator mode + screenshot regression**: fixed RTC, time advances only on `step`;
  `tools/regress.py` compares launcher, watch face, settings, ball and Chinese/Russian/Japanese pages
  by hash (16 scenarios, byte-identical across runs)
- **Simulator voice-state injection**: a `voice` command feeds link state (idle/listening/thinking/
  speaking/happy/sleepy) straight into the UI; `open` now closes the running app first, so the ball
  page is no longer hidden behind the launcher overlay
- **BLE time and timezone sync**: the control characteristic gained `0x60` (Unix seconds) and `0x61`
  (timezone offset in hours); the host can set the watch clock, the timezone is stored in NVS, and
  the RTC is written and read back for verification
- **Web flasher**: <https://wen0531.github.io/m5stack-qiuqiu/flasher/> — flash from Chrome or Edge
  without a toolchain; the page itself supports ten languages

### Changed

- Dropping the last reference to `MontserratSemiBold26` let the linker discard it: the firmware is
  actually 22 KB smaller than the previous build

### Known limitations

- The language list is a scroll-and-tap flow and is not part of the deterministic screenshot
  regression (gesture timing varies); it is verified by hand

## v1.1 (2026-10-23)

### Added

- **A/B firmware slots**: Settings → System Slots shows slot status, switches systems and clones the
  current system into the other slot
- **Automatic rollback**: a new image that fails self-test within 30 s is rolled back by the bootloader
- **Heartbeat watchdog**: the device restarts after 15 s without main-loop progress (the prerequisite
  for rollback)
- **Boot menu**: pressing B within 5 s of boot starts the other system (only shown when the other slot
  holds a valid image)
- **Full image verification** (`esp_image_verify`); a failed clone aborts cleanly

### Power

- **Light sleep works**: screen off → sleep, wake on button press, with three gates
  (screen / audio / voice session)
- Debug logging cut from dozens of lines per second to warn level
- The LVGL tick timer stops when the screen is off; the main loop runs at 100 ms with the screen off
  and 5 ms with it on

### Fixed

- The main loop was a delay-free busy loop (CPU spinning at 100%)
- Touch could not wake the device after screen-off
- The screen stayed dark when an alarm fired
- Brightness was zeroed, causing a black flash on wake
- The power-gate counter was not locked

### Known limitations

- Rollback needs bootloader support: **flashing only the app partition has no effect**, a full flash
  is required
- A/B protects the firmware only, not NVS or the FAT partition
- Cloning is allowed only into an empty or corrupt slot; valid backups are never overwritten

## v0.1.0 (2026-10-23)

First public release.

### Features

- Hold A on the watch to speak, release to send; press A during playback to interrupt
- Button B cycles the backend agent (ball colour: white / blue / red)
- Voice-driven backend switch (say an alias such as "小马 / 小地 / 大地", configurable)
- Procedural C++ ball character with 15 expressions, no image assets
- Opus downlink (24 kbps adjustable) with a fixed 20 ms playback cadence
- Phone app: BLE + WebSocket relay, foreground service, autostart on boot, reconnect by MAC
- PC bridge: HTTPS/WSS, Volcengine streaming ASR, edge-tts, multi-agent adapters (HTTP/CLI),
  automatic fallback
- IPv6 DDNS (Cloudflare) script + systemd timer
- Battery telemetry: one sample per minute, 24-hour ring buffer, CSV export from the app or PC
- Charging LED (green while USB is plugged in)
- Self-test script `tools/selftest.py`

### Power

- CPU DFS 160/80 MHz
- BLE modem sleep (Mode 1) + MAC/BB power-down + main crystal kept alive
- Amplifier follows the codec
- Idle-task throttling (voice 30 ms, downlink 20 ms polling)
- **Light sleep unavailable**: the QSPI AMOLED + Octal PSRAM combination hung; the stock firmware
  did not enable it either

### Known limitations

- Battery life still under measurement at the time (expected 25–35 hours on 450 mAh)
- Requires both the phone and the PC online
- Cloud ASR by default; audio leaves the device

---

# 中文

## v1.2.0 (2026-10-26)

### 新增

- **10 种语言界面**：English / 简体中文 / 繁體中文 / 日本語 / 한국어 / Français / Deutsch /
  Español / Русский / Latina
  - 子集字体方案，每语言只打包实际用到的字形，固件仅 +160 KB
  - 启动器应用名跟随语言切换，标签改用带 CJK 字形的字体
  - 设置、闹钟、秒表、表盘星期缩写、按键/时间/日期等子页全部本地化（新增 23 条文案）
- **语言选择页**：设置 → Language 打开十行列表，不再是"点一下轮换"；每行用该语言自己的字体
  渲染，当前语言高亮，选完立即返回并重建菜单
- **主机模拟器**（`sim/`）：在电脑上跑真实固件 UI，支持截图、按键/触摸注入、界面元素枚举、
  崩溃与卡死检测；统一虚拟面板同时捕获 LVGL flush 与球球 M5GFX 直写两条显示路径
- **模拟器网页界面**（`sim/web/`）：`http://127.0.0.1:8099/`，实时画面 ~12 fps、A/B/A+B 按键、
  点击与拖动、十语言切换、状态栏与日志面板
- **模拟器确定性模式 + 截图回归**：固定 RTC、时间只在 `step` 时前进；`tools/regress.py`
  对 launcher、表盘、设置、球球、中/俄/日文页面做哈希比对（16 场景，多次运行逐字节一致）
- **模拟器语音状态注入**：`voice` 命令把链路状态（idle/listening/thinking/speaking/happy/
  sleepy）直接喂给 UI；`open` 会先关掉当前应用，球球页面不再被启动器画面盖住
- **BLE 对时与时区**：控制特征新增 `0x60`（Unix 秒）与 `0x61`（时区小时偏移）；主机可对准
  手表时钟，时区存 NVS，写入 RTC 后回读校验
- **网页刷机页**：<https://wen0531.github.io/m5stack-qiuqiu/flasher/>，Chrome / Edge 打开即可刷，
  不需要工具链；页面本身也支持十种语言

### 变更

- 去掉对 `MontserratSemiBold26` 的最后引用后，链接器把它丢掉，固件反而比上一版小 22 KB

### 已知限制

- 语言列表是"手势滚动 + 点击"的流程，未纳入确定性截图回归（滚动惯性受帧时序影响），改为人工验证

## v1.1 (2026-10-23)

### 新增

- **A/B 双固件槽**：设置 → System Slots 查看槽位状态、切换系统、把当前系统克隆到另一槽
- **自动回滚**：新固件 30 秒内未通过自检，bootloader 自动切回上一槽
- **心跳看门狗**：主循环 15 秒无推进自动重启（回滚的前提）
- **开机启动选单**：开机 5 秒内按 B 键启动另一系统（仅当另一槽有有效固件时出现）
- **完整镜像校验**（`esp_image_verify`），克隆失败自动 abort

### 省电

- **light sleep 可用**：熄屏 → 睡眠，按键唤醒；三道门禁（屏幕 / 音频 / 语音会话）
- 调试日志从每秒数十行降到 warn 级别
- LVGL 定时器在熄屏时停止；主循环熄屏时 100 ms、亮屏时 5 ms

### 修复

- 主循环原本是无延时死循环（CPU 100% 空转）
- 熄屏后触摸唤醒失效
- 闹钟触发时屏幕不亮
- 亮度被置 0 后唤醒黑闪
- 电源门禁计数未加锁

### 已知限制

- 回滚需要 bootloader 支持，**只刷 app 不生效**，必须完整刷写
- A/B 只保护固件，不保护 NVS/FAT 设置
- 克隆仅允许写入空槽/损坏槽，禁止覆盖有效备份

## v0.1.0 (2026-10-23)

首个公开版本。

### 功能

- 手表按住 A 说话、松手发送；播放中按 A 打断
- B 键循环切换后台 Agent（球球颜色：白/蓝/红）
- 语音切换后台（说别名，如"小马/小地/大地"，可配置）
- 程序化 C++ 球球形象，15 个表情，无图片资源
- Opus 下行编码（24 kbps 可调），固定 20 ms 播放节奏
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

- 续航实测中（当时预期 25–35 小时，450 mAh）
- 依赖手机与电脑同时在线
- 默认云端 ASR，音频会上传
