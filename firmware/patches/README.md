# 第三方库补丁

`fetch_repos.py` 会把 M5GFX / LVGL / mooncake 拉到 `components/` 下。下面是我们对其做的修改，**重新拉取依赖后需要手动应用**。

## M5GFX：AMOLED 双缓冲空指针保护

**文件**：`components/M5GFX/src/lgfx/v1/panel/Panel_AMOLED.cpp`

**问题**：`Panel_AMOLED_Framebuffer::display()` 在 DMA 缓冲（`buf[0]` / `buf[1]`）尚未分配时会执行 `memcpy`，导致 ESP32 `StoreProhibited` 崩溃（表现为按 A 键松手时设备重启）。

**修复**：在写入前判空，两个缓冲都要检查。

```cpp
if (buf[0] == nullptr || buf[1] == nullptr) {
    return;   // buffers not ready yet: skip this frame instead of crashing
}
```

## 其他

固件其余改动都在 `main/` 内（本项目自有代码），不涉及第三方库。
