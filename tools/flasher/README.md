# 网页一键刷机

基于 [ESP Web Tools](https://esphome.github.io/esp-web-tools/)：用户插上 USB，用 Chrome 打开页面点一下即可刷写，不需要安装 ESP-IDF。

## 使用前需要把固件放进这个目录

```
tools/flasher/
├── index.html
├── manifest.json
├── bootloader.bin          ← 从固件构建目录复制
├── partition-table.bin     ← 同上
└── StopWatch-UserDemo.bin  ← 同上（app，偏移 0x20000）
```

构建后产物位置：

```bash
firmware/build/bootloader/bootloader.bin
firmware/build/partition_table/partition-table.bin
firmware/build/StopWatch-UserDemo.bin
```

## 发布到 GitHub Pages

1. 把本目录推到仓库（或复制到 `docs/flasher/`）
2. 仓库 Settings → Pages → Source 选 `main` 分支 `/docs` 或 `/root`
3. 把页面地址写进 README 的「快速开始」

## 注意

- 只支持 Chrome / Edge（Web Serial API）
- 需要 HTTPS 或 localhost
- 刷写会覆盖手表固件
