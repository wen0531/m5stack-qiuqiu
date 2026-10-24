# 固件模拟器（主机运行）

在电脑上跑**真实的固件 UI**（同一套 mooncake 应用框架、同一套 i18n 表、同一套字体资源），
不需要手表、不需要显示器。用于：

- 改完界面立刻看效果（导出 PNG）
- 验证多语言是否有缺字/溢出
- 验证菜单跳转、按键、触摸流程
- 捕获崩溃与卡死（由监督进程负责）

## 它不是什么

| 不能验证 | 原因 |
|---|---|
| 功耗 / light sleep | 物理层面无法模拟 |
| BLE 协议 | 用桩替代 |
| 音频质量 | 只导出数据，不代表真机音质 |
| AMOLED 真实观感/刷新 | 主机是内存帧缓冲 |
| AMOLED/PMIC 时序 | 需要硬件模型 |

## 构建

依赖：`cmake`、`g++`、`zlib`、系统 `fmt`（`libfmt-dev`）、LVGL 源码（在本仓库 `components/`）。

```bash
cd sim
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

产物：`sim/build/stopwatch_sim`

## 网页界面（推荐）

一条命令启动模拟器和本地网页：

```bash
./web/run.sh
# 打开 http://127.0.0.1:8099/
```

页面提供：

- **实时画面**（MJPEG，约 12 fps）
- **A / B / A+B 按键**（按住即按下，松开即抬起）
- **触摸**：点一下 = 点击，拖动 = 滑动
- **语言下拉**：10 种语言即时切换
- **状态**：应用、语言、帧数、循环数、电量
- **日志**：最近 300 行（LVGL 日志 + 应用日志）

只监听 `127.0.0.1`，不对外网、不对局域网开放，属于本地开发工具。

## 使用

```bash
./build/stopwatch_sim            # 启动，控制接口监听 127.0.0.1:9123
python3 tools/simctl state       # 状态：帧数/循环数/语言/应用/电池
python3 tools/simctl tap 233 233 # 点击屏幕
python3 tools/simctl key A --hold 300
python3 tools/simctl swipe 233 380 233 120
python3 tools/simctl dump        # 列出界面上的文字与坐标
python3 tools/simctl tap_text Settings
python3 tools/simctl lang zh-Hant
python3 tools/simctl shot out/a.png
```

批量与监督：

```bash
python3 tools/langsweep.py       # 10 种语言各截一张
python3 tools/simrun.py "shot out/x.png" "raw crash"   # 崩溃检测
```

## 确定性模式与回归测试

模拟器可以完全脱离真实时间运行：时间只在测试要求时前进，因此同一脚本每次得到**逐字节相同**的截图。

```bash
SIM_START_PAUSED=1 ./build/stopwatch_sim     # 启动即暂停
simctl raw step 1500                          # 推进 1.5 秒模拟时间
simctl raw "clock 09:41:00"                   # 固定 RTC
simctl raw "clock date 2026-10-24"
simctl open Settings                          # 按名字打开应用（不走手势）
```

回归测试：

```bash
python3 tools/regress.py            # 与基线比对
python3 tools/regress.py --update   # 重建基线
```

场景：launcher / watchface / settings / qiuqiu / settings_zh。
每个场景独立进程 + 独立控制端口，连续三次运行哈希一致。

> 已知差异：球球页面在真机上会隐藏启动器，模拟器里 LVGL 透明层仍会显示启动器文字/箭头，
> 因此球球截图会多出 `AlarmClock` 与左右箭头。

## 设备容量约束（重要）

PC 的内存和 Flash 远大于手表，不加限制就会跑出"真机根本装不下"的固件。
模拟器因此模拟了手表的预算：

| 资源 | 手表 | 模拟器行为 |
|---|---|---|
| **内部 RAM** | ~512 KB | 超限 **立即 abort** 并打印 `[mem] OUT OF internal RAM ...` |
| **PSRAM** | 8 MB | 超限 **立即 abort** 并打印 `[mem] OUT OF PSRAM ...` |
| 分配策略 | ≤4 KB 走内部 RAM，>4 KB 走 PSRAM | 相同（`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`） |
| **Flash 应用槽** | 4.94 MB | `tools/check_size.py` 对照分区表检查，>90% 警告，超出报错 |

预算可通过环境变量调整：

```bash
SIM_INTERNAL_KB=512 SIM_PSRAM_MB=8 ./build/stopwatch_sim
```

实测：

```
起始:           内部 RAM 37/512 KB     PSRAM 1272/8192 KB
分配 4 MB PSRAM:  → PSRAM 5368/8192 KB
再分配 6 MB:      → [mem] OUT OF PSRAM: tried 6144 KB, budget 8192 KB, in use 5368 KB  → abort
内部申请 600 KB:  → [mem] OUT OF internal RAM: tried 600 KB, budget 512 KB              → abort
```

超限的 abort 会被监督进程识别为崩溃（SIGABRT），网页上也会显示两条容量条。

固件体积检查：

```bash
python3 tools/check_size.py
# app image : 3,392,480 bytes (3.24 MB)
# slot ota_0: 4.94 MB, free 1743 KB
# OK: fits comfortably
```

## LVGL 配置与设备对齐

设备用 `CONFIG_LV_CONF_SKIP=y` + Kconfig 生成 LVGL 配置，主机没法用 Kconfig。
因此 `sim/lv_conf.h` 由脚本从 `sdkconfig` 生成：

```bash
python3 tools/sim/gen_lv_conf.py    # sdkconfig → sim/lv_conf.h
```

生成时只覆盖 PC 必须不同的项（内存池大小、日志开关、OS 层、SIMD 选择），
其余 280+ 个选项（控件、字体、绘制、断言）与设备一致。

> 改了任何 `CONFIG_LV_*` 选项后要重新生成，否则渲染细节会和设备不一致。

## 架构要点

**统一虚拟面板**：LVGL 的 flush 回调和球球 App 的 `M5GFX::writePixels` 都写进同一块
466×466 RGB565 缓冲（`src/sim_display.cpp`），截图、录帧只看这一块，所以两条显示路径
都不会漏。

**代码复用**：`sim/` 不复制 app / i18n / 字体 / 球球引擎，全部从 `../main/` 编译；
差异只有 `sim/stubs/` 下的平台替身。

**监督进程**：被测进程崩溃后无法自报，因此 `tools/simrun.py` 在外部监控退出码/信号、
最后日志与最后一张完整截图；卡死检测看的是主循环心跳（`loops`），不是帧数——静态页面
本来就不刷新。

## 已知限制

- 设置菜单用 canvas 绘制，`dump`/`tap_text` 看不到文字，只能用坐标操作
- PTT（语音按下）走 GPIO 直读，模拟器不验证其阈值
- 音频/IMU/PMIC/OTA 全部是桩
