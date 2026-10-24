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
