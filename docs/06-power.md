# StopWatch 省电调研报告（2026-10-21）

> 目标：当前续航 < 2 天，希望 3–4 天。电池 450mAh，反推平均电流需从 >9.4mA 降到 **<4.7mA**。

## 一、当前固件审计（耗电点）

| # | 项目 | 现状 | 影响 |
|---|---|---|---|
| 1 | 电源管理 | `CONFIG_PM_ENABLE` **未开启**，CPU 固定 240MHz，无 DFS/light sleep | 大 |
| 2 | 启动器息屏 | **只有 Qiuqiu App 有 20 秒息屏**；启动器界面不熄屏 | 大（AMOLED 常亮） |
| 3 | 熄屏方式 | 仅 `setBackLightBrightness(0)`，**没发 CO5300 的 panel sleep (0x28/0x10)**，AMOLED 仍在扫描 | 中 |
| 4 | 音频 codec | ES8311 + I2S 时钟**开机后常开**，空闲也不关 | 中 |
| 5 | BLE 广播 | 用默认广播参数（未拉长间隔），断开时一直快广播 | 中小 |
| 6 | BLE 连接参数 | 固定 30–50ms（为音频设的），空闲时也是这个值 | 中小 |
| 7 | Qiuqiu 渲染 | App 打开时 19.5fps 全速渲染白色球体 | 中（仅打开时） |
| 8 | IMU/传感器 | `Hal::imu_init()` 在**开机时就初始化**（BMI270 默认正常模式，约 1mA），平时界面用不到也不休眠 | 中 |
| 9 | PSRAM | Octal 80MHz 常开 | 中小 |
| 10 | USB Serial/JTAG | 串口日志常开（console） | 小 |

## 二、官方数据参考

ESP32-S3 芯片功耗（[Deneyap Kart 实测汇总](https://docs.deneyapkart.org/en/Cards/Deneyap-Kart-1A-v2/Power-Consumption-esp32-s3)）：

| 模式 | 电流 |
|---|---|
| Modem Sleep，CPU idle @80MHz，RTC 关 | **10.5 mA** |
| Modem Sleep，CPU idle @160MHz | 14–16 mA |
| **Light Sleep**（Wi-Fi/SPI 关，IO 高阻） | **750 µA** |
| Deep Sleep（RTC timer） | 20–25 µA |
| 关机 | 1 µA |

> 注：以上是**芯片本体**；AMOLED 屏、PSRAM、codec、功放另算。AMOLED 全白画面可达几十 mA。

NimBLE 省电的关键配置（[ESP-IDF 官方 power_save 示例](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble/power_save)）：

```
Component config → Power Management → [*] Support for power management
Component config → FreeRTOS → Kernel → [*] configUSE_TICKLESS_IDLE
Component config → PHY → [*] Power down MAC and baseband ... when PHY is disabled
Component config → Bluetooth → Controller Options → MODEM SLEEP Options
    [*] Bluetooth modem sleep
    [*] Bluetooth Modem sleep Mode 1
    [*] power up main XTAL during light sleep
```
ESP32-S3 用 32kHz XTAL 作为低功耗时钟（`sdkconfig.defaults.esp32s3`）。

## 三、可执行改造清单（按收益排序）

### P0：立竿见影（预计 +1–1.5 天续航）

1. **启动器加自动熄屏**（10–15 秒无操作）
   - 位置：`main/apps/app_launcher`；参考 Qiuqiu 的 `SCREEN_OFF_MS` 逻辑
   - 熄屏时：亮度 0 **+ 发 panel sleep 命令**（CO5300 `0x28`）
   - 预计：AMOLED 常亮是最大头，熄屏后单这一项可省 20–40mA

2. **启用 ESP-IDF 电源管理 + light sleep**
   - `sdkconfig`：`CONFIG_PM_ENABLE=y`、`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`、`CONFIG_BT_CTRL_MODEM_SLEEP=y`、`CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y`、`CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y`
   - 代码：`esp_pm_configure({.max_freq_mhz=160, .min_freq_mhz=80, .light_sleep_enable=true})`
   - 注意：LVGL 刷新、I2S、SPI 会拿 PM lock，需确认不会阻止休眠
   - 预计：待机从 ~20mA 降到 ~5–8mA

3. **BLE 参数分级**
   - 未连接：广播间隔拉到 **1s**（`advp.itvl_min/max = 1600`，单位 0.625ms）
   - 已连接空闲：请求 **100–150ms** 连接间隔；进入音频流时再切回 30–50ms
   - 预计：省 1–2mA

### P1：进一步优化（预计 +0.5–1 天）

4. **音频 codec 按需开关**
   - 空闲 30 秒后 `esp_codec_dev_close()` + `i2s_channel_disable()`；播放/录音前再开
   - 注意：ES8311 重新初始化约几十 ms，按键响应可接受
   - 预计：省 2–5mA

5. **Qiuqiu 渲染降帧**
   - 打开时从 19.5fps 降到 **10fps**（动画仍可接受），或静止时降到 2fps
   - 预计：省 5–15mA（仅 App 打开时）

6. **AMOLED 内容优化**
   - 黑底 + 缩小白色球体面积；白色像素是 AMOLED 最耗电的部分
   - 亮度默认从 80 降到 50

### P2：深度省电（可到 4 天+）

7. **长时间无交互 → 深度睡眠**
   - 条件：无 BLE 连接 + 无操作 3–5 分钟
   - 唤醒源：按键 GPIO（ext1）、触摸中断、IMU any-motion、RTC 定时
   - 预计：待机 20–25µA，几乎不耗电

8. **PSRAM 降频/按需**：若非必须，`CONFIG_SPIRAM_SPEED_40M`；Qiuqiu 的 372×372 缓冲可改小
9. **关闭串口日志**（release 版本 `CONFIG_LOG_DEFAULT_LEVEL_NONE`）

## 四、实施顺序建议

1. 先做 P0-1（启动器息屏）+ P0-2（PM/light sleep）→ 实测电流/续航
2. 再做 P0-3（BLE 分级）+ P1-4（codec 按需）
3. 最后 P2（深度睡眠 + 唤醒源），这是"3–4 天"的关键

## 五、风险

- **light sleep 与 USB 串口日志冲突**：调试期建议保持 USB 供电测试，release 再开
- **BLE 连接间隔拉长会降低音频实时性**：需在"流式播放时"动态切回短间隔
- **panel sleep 后唤醒需要重新初始化**：CO5300 唤醒命令时序要验证
- **深度睡眠唤醒延迟**：GPIO 唤醒约几十 ms，可接受；但 BLE 需要重新广播+重连（手机端要有自动重连）

## 六、第二轮调研补充（同类项目实证）

**Waveshare ESP32-S3-Touch-AMOLED（同类 AMOLED 手表，[waveshare-watch-rs](https://github.com/infinition/waveshare-watch-rs)）的做法：**

- 主循环**事件驱动**（GPIO 中断唤醒），表盘状态下 **CPU 睡眠时间 > 99%**
- **4 级睡眠/唤醒状态机** + 息屏显示（AOD）
- IMU（QMI8658）**默认 power_down**
- CO5300 面板级休眠命令：`0x28 DISPOFF` / `0x29 DISPON` / `0x10 SLPIN` / `0x11 SLPOUT`（[co5300.rs](https://github.com/infinition/waveshare-watch-rs/blob/main/src/drivers/co5300.rs)）
- QSPI 80MHz DMA 刷屏，刷完即停

**对我们的直接启示：**
1. 我们现在是"轮询式主循环 + LVGL 定时器"，即使不刷新也醒着；应改成**事件驱动 + 长 tickless idle**
2. 熄屏不能只关背光，要发 **0x28 + 0x10**（DISPOFF + SLPIN），唤醒时 **0x11 + 0x29**
3. IMU 平时 power_down，用 any-motion 中断唤醒（比轮询省得多）
4. 参考他们的"4 级状态机"：亮屏 → 息屏（面板 sleep）→ light sleep → deep sleep

**续航换算：** 450mAh ÷ 4 天 ≈ **4.7mA 平均电流**；对照芯片数据（light sleep 0.75mA + BLE 广播 ~0.3–1mA + AMOLED 熄灭后 ~0.1mA + PSRAM/外设残余），**目标可达**，关键在"屏灭 + 面板 sleep + 事件驱动"三件事。

## 八、默认参数（审计补充）

- 屏幕默认亮度：**80**（`getBackLightBrightness` 从 settings 读，缺省 80）
- IMU：**开机初始化**（`Hal::imu_init()` 在 `hal.cpp` 启动序列里），BMI270 默认正常模式
- 音频：开机 `audio_init()` 打开 ES8311 + I2S（BOTH 模式）
- 触摸：LVGL 10ms 轮询

## 九、第三轮：子代理核实后的关键修正（源码级验证）

### 9.1 硬件事实：**这块板子没有 32.768kHz 晶振**

查 C152 原理图（[PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1242/C152-SCH_Stopwatch_PRJ_Main_VA_20251201_2026_04_24_17_46_22.pdf)）：

- `GPIO15/XTAL_32K_P` 被复用为 **G15_I2S_LRCK**
- `GPIO16/XTAL_32K_N` 被复用为 **G16_I2S_DADC**
- 板上只有 40MHz 主晶振；另有一颗 **RX8130CE 外部 RTC**（带唤醒），但它的 32k 输出没有接到 ESP32 的 32K 引脚

**结论：BLE 保持连接时的 light sleep 只能用「主 XTAL 睡眠」方案，约 3.3mA**（vs 32k 晶振的 230µA）。按 450mAh 算：3.3mA ≈ **5.6 天**理论待机，仍然满足 3–4 天目标（前提是屏灭、无音频）。

### 9.2 配置陷阱（我们正好踩中）

| 配置 | 现状 | 必须改成 |
|---|---|---|
| `CONFIG_PM_ENABLE` | **n** | y |
| `CONFIG_FREERTOS_USE_TICKLESS_IDLE` | 未开 | y（否则 esp_pm_configure 直接返回 NOT_SUPPORTED） |
| `CONFIG_BT_CTRL_MODEM_SLEEP` | **n（默认关）** | y + MODE_1 |
| `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP` | 未开 | **y（关键）**，否则控制器一直持 NO_LIGHT_SLEEP，整机永远睡不了 |
| `CONFIG_RTC_CLK_SRC` | INT_RC（内部 150k） | 保持（本板无 32k 晶振），但要知道它只能广播/扫描，连接时必须靠主 XTAL 睡眠 |

其他：`CONFIG_PM_SLP_DISABLE_GPIO=y`（省 200–300µA，但睡眠时 GPIO 高阻，需确认屏/触摸 CS 有外部上下拉）；`CONFIG_PM_SLP_IRAM_OPT=y`；`esp_pm_config{max=160, min=80, light_sleep=true}`（**min 取 80 而非 40**，因为 Octal PSRAM 在 40MHz 时序余量小）。

### 9.3 其他会阻止休眠的东西（已确认）

- **I2S + APLL** 会持 `ESP_PM_NO_LIGHT_SLEEP` → 放音期间睡不了；空闲必须关 codec/I2S
- **USB Serial/JTAG** 常开不支持 light sleep → release 版要 `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n`（调试期可开）
- LVGL 10ms 轮询、触摸 I2C 轮询、`esp_timer` 高频回调 → 都要降频
- 排查工具：`CONFIG_PM_PROFILING=y` + `esp_pm_dump_locks(stdout)`

### 9.4 电流参考（ESP32-S3 官方/techpedia）

| 场景 | 电流 |
|---|---|
| BLE 连接空闲（只有 modem sleep，CPU 没睡） | **17.9 mA** |
| BLE + 自动 light sleep（32k 晶振） | 230 µA |
| BLE + 自动 light sleep（主 XTAL） | **3.3 mA** |
| light sleep（含 8MB PSRAM） | ~380 µA |
| deep sleep | 7–8 µA（BLE 必断） |

**所以本板的现实目标：BLE 保连 ≈ 3.3mA + 屏灭 ≈ 0 → 理论 5 天+；实际按 4 天验收。**

## 十、晶振结论（查官方手册 + 原理图，已确认）

**这块板子对 ESP32-S3 没有 32.768kHz 晶振。**

证据：
1. `资料/StopWatch.pdf`（官方手册）只列 **RTC Clock Chip RX8130CE**（I2C 外部 RTC，带唤醒），未提 ESP32 的 32k 晶振
2. C152 原理图：`GPIO15/XTAL_32K_P` → **G15_I2S_LRCK**，`GPIO16/XTAL_32K_N` → **G16_I2S_DADC**（32k 引脚被复用为 I2S）
3. RX8130CE 的 `FOUT`（pin 4）在原理图上**没有网络名**，未接到 ESP32 的 32K 引脚
4. sdkconfig 现状：`CONFIG_RTC_CLK_SRC_INT_RC=y`（内部 150kHz RC）

**含义：**
- 内部 RC 只够广播/扫描，**BLE 连接时必须用主 XTAL 睡眠** → 约 **3.3mA**（而非 32k 晶振的 230µA）
- 3.3mA × 24h ≈ 79mAh/天 → 450mAh 理论 **5.6 天**，目标 3–4 天可行
- 若以后想更省，只能改硬件（飞线 32k 晶振到 GPIO15/16，但会占用 I2S 引脚，不可行）

## 十一、硬件级耗电清单（子代理核实）

| 电源轨 | 供什么 | 关闭方式 |
|---|---|---|
| L1（LDO3V3） | IMU + RTC | PM1 `setLdoEnable` |
| L2/L3A（DCDC3V3） | ESP32-S3、M5IOE1、按键、EXT | PM1 `setDcdcEnable` |
| L3B | **AMOLED、音频、马达**（由 M5IOE1 管） | M5IOE1 `IO8=L3B_EN` 拉低 |
| BOOST5V | Grove 5V | PM1 `setBoostEnable(false)` |

M5IOE1 关键脚：`IO3=AU_EN`（ES8311+MIC 供电）、`IO8=L3B_EN`（屏幕）、`IO10=AW8737A EN`（功放）、`IO4=TP_RST`、`IO5=OLED_RST`。

**重要坑：** StopWatch 用 PSRAM 帧缓冲面板，`Panel_AMOLED_Framebuffer::setSleep()` 是**空实现** → `M5.Display.sleep()` 只写亮度 0，**不会发 0x10 SLPIN**。要手动发命令或先 `disableFrameBuffer()`。

CO5300 命令：`0x51` 亮度、`0x28` DISPOFF、`0x10` SLPIN（典型 135µA）、`0x4F`+DSTB 深待机（3µA，退出需 RESX 低脉冲）。

**触摸省电**：CST820 `0xE5=0x03`（低功耗模式），约省 1.9mA。

## 十二、电池遥测（已实现，待刷机验证）

用户没有电流表，用 M5PM1 读 VBAT 替代：

- 新增 `main/voice/batt_log.cpp`：每分钟采样 `{uptime_s, vbat_mv, pct}`，环形缓冲 1440 条（24 小时，PSRAM）
- 控制命令 **0x40**：通过 BLE debug 特征导出 CSV（`uptime_s,vbat_mv,pct`）
- HAL 新增 `Hal::getBatteryVoltage()`（返回滤波后的 mV）
- 固件已编译通过，**待刷机验证**（按项目规则，刷机需单独授权）



## 实测记录（2026-10-23）

| 时间 | 电压 | 电量 | 备注 |
|---|---|---|---|
| 12:04 | 4176 mV | 97% | 拔线，测试开始 |
| 13:04 | 4161 mV | 95% | |
| 13:34 | 4133 mV | 92% | |
| 14:14 | 4080 mV | 86% | 稳态斜率 ≈36 mA |

**推算**：36 mA → 450 mAh ÷ 36 mA ≈ **12.5 小时**。

**与预期的差异**：文档早期估算 25–35 小时，实际差一倍以上。原因是当时低估了"CPU 无法睡眠"的代价——DFS 只降频不降功耗，空闲时 CPU 仍在 ~27 mA。
