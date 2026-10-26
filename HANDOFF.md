# StopWatch 交接（最新在最上）

> 本文件只写 **M5Stack StopWatch（C152）** 线。其他设备见 `HANDOFF.md` 索引。

## 当前状态（2026-10-26 北京时间）｜StopWatch 线：多语言 + 模拟器完成，固件已上机

**一句话状态：** 固件（10 语言 + 启动器/设置全本地化 + 语言选择页 + A/B 双槽 + 轻睡眠）已刷入手表并经读回哈希验证；主机模拟器（真实 UI + 输入注入 + 网页前端 + 确定性截图回归）已建成并推仓库。

**设备**
- M5Stack StopWatch v1.0.1，ESP32-S3R8（8MB PSRAM / 16MB flash），CO5300 466×466，ES8311，CST820
- BLE 广播名 `StopWatch-BLE`，MAC `C4:9E:7E:12:A5:92`，服务 `4f5a0001-1e3b-4d8a-9c7e-5a1b2c3d4e5f`
- 运行固件：`firmware/stopwatch-userdemo/build/StopWatch-UserDemo.bin`（3,417,424 B，
  sha256 `087133afaa34513b1b30239c2201c9e7f514cf7c41ccd9d841c03c290ba83074`），
  **读回设备 ota_0 与文件哈希完全一致**；ota_0 占 68.5%，余 1719 KB
- 刷写命令：`esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 --before usb-reset --after hard-reset write-flash 0x20000 build/StopWatch-UserDemo.bin`
- 备份：`~/.local/share/esp32-backups/stopwatch/`（原厂整片、ota_0 原厂区、
  以及 `StopWatch-UserDemo-langpicker-20261025.bin` 等多个历史版本）

**已完成功能**
- 10 种语言（en/简中/繁中/日/韩/法/德/西/俄/拉丁），子集字体只打包用到的字形
- 启动器应用名、设置菜单、Button/时间/日期等子页全部本地化；**语言选择页**（列表点选，不再轮流切换）
- 球球头像（15 表情）、BLE 语音链路、A/B 双固件槽 + 开机 5 秒选单 + 自动回滚、轻睡眠省电、看门狗心跳
- BLE 对时：`0x60` 设置 Unix 时间、`0x61` 设置时区（各带回读校验）

**主机模拟器（新增，`sim/`）**
- 跑真实固件 UI：mooncake + 6 个 app + LVGL 9.5 + 全部资源，主机编译 5 MB 可执行
- 统一虚拟面板：LVGL flush 与球球 M5GFX 直写汇到同一 466×466 缓冲
- 输入注入：A/B/A+B、点击、滑动；状态查询、界面元素枚举、按名字打开应用
- 网页前端（`sim/web/`，仅 127.0.0.1:8099）：实时画面 12 fps、按键触摸、语言、状态、
  内部 RAM/PSRAM 容量条、日志面板
- 设备容量约束：内部 RAM 512 KB、PSRAM 8 MB、Flash 槽 4.94 MB，超限直接 abort
- LVGL 配置从 sdkconfig 生成（`tools/sim/gen_lv_conf.py`，281 个选项）
- 确定性模式：`SIM_START_PAUSED=1` + `step` + `clock` + `open`；截图回归 16 场景，三次运行逐字节一致
- 监督进程 `tools/simrun.py`：捕获崩溃信号、最后日志、最后完整帧；卡死看主循环心跳

**下一步（未做）**
- 手机 App 连上时自动发 `0x60` 实现长期自动对时
- 语言列表这类"手势滚动 + 点击"流程在确定性回归里不稳定，目前靠人工验证
- 仓库仍为 private；要开 GitHub Pages 网页刷机需先转 public

**文档索引**
- 需求与计划：`资料/方案/StopWatch-模拟器-需求与计划-2026-10-24.md`
- 实施计划与日志：`资料/方案/StopWatch-模拟器-TODO-2026-10-24.md`
- 仓库（私有）：`wen0531/m5stack-qiuqiu`，最新提交 `06f7cf5`
