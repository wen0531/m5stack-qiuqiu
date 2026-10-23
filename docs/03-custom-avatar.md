# 03 · 自定义角色：用 C++ 画一张你自己的脸

> 本文是本项目的二次开发教程：把 466×466 的 AMOLED 手表变成任意 2D 角色的舞台。
> 读完能完成三件事：
>
> 1. **5 分钟**：换球色、眼睛圆度、弹性手感；
> 2. **半小时**：改表情数据，或自己生成一套眼型；
> 3. **半天**：换掉整个渲染器——方块脸、贴图、粒子都行。
>
> 文中代码基于仓库当前实现；host 预览程序、每种示例都在本机编译运行过。路径均相对仓库根目录。

---

## 0. 四个文件，一张地图

| 路径 | 内容 | 你大概率会改它 |
|---|---|---|
| `firmware/main/apps/app_qiuqiu/qiuqiu_engine.h` | 16 个公开 API 声明 | 换渲染器时保持签名 |
| `firmware/main/apps/app_qiuqiu/qiuqiu_engine.cpp` | 程序化绘制：球体变形、眼睛多边形、抗锯齿填充、眨眼、果冻弹簧 | 改手感 / 换 `render()` |
| `firmware/main/apps/app_qiuqiu/qiuqiu_data.h` | 18 个眼环（每个 24 点）、18 个眼位、15 个表情 | 改表情 / 换眼型 |
| `firmware/main/apps/app_qiuqiu/app_qiuqiu.cpp` | PSRAM 帧缓冲、20/8 fps 节奏、推屏、按键与触摸、熄屏 | 改分辨率 / 推屏路径 |
| `licenses/` | Emotion Ball 许可与 NOTICE 原文 | 分发时原样保留 |

### 设计坐标：320×240 的"草稿纸"

引擎内部沿用上游设计工具的画布：**320×240，球心 (160,120)，半径 80**。`qiuqiu_data.h` 里所有整数都是这个坐标系下的"设计像素"（`int8_t`，范围 ±127）。

`qiuqiu::init(w, h, ball_radius_px)` 负责把这套坐标映射到屏幕：

```text
k        = ball_radius_px / 80
screen_x = (design_x - 160) * k + w / 2
screen_y = (design_y - 120) * k + h / 2
```

所以：

- 改 `ball_radius_px` 等于整体缩放，数据文件一个数字都不用动；
- **分辨率减半时半径也要减半**（固件里 186×186 用 84，372×372 用 168），否则球会缩成屏幕中间的小点；
- 同一个角色在 186 和 466 下形状完全一致，只是清晰度不同。

---

## 1. 为什么用程序化绘制，而不是一堆图片

这是本项目的核心取舍，值得先说清楚。

| 对比项 | 贴图序列 | 程序化绘制（本实现） |
|---|---|---|
| 15 个表情的存储 | 15 表情 × 12 帧 × 372² × 2 B ≈ **48 MiB**（单个固件槽只有 4.94 MiB） | **1.6 KiB** 数据 + 约 14 KiB 代码 |
| 任意分辨率 | 每种分辨率重新出图 | 改一个半径参数 |
| 改一个眼神 | 重画、重导、重刷 | 改 4 个数字 |
| 表情之间的过渡 | 要么硬切，要么补帧 | 插值天然平滑 |
| 物理手感（果冻/回弹） | 只能逐帧画 | 弹簧方程一次写完，处处受益 |
| 画风上限 | 任意（像素画、插画都行） | 需要能用数学描述，细节有限 |

具体收益：

1. **无资源**：不占文件系统、不占 LVGL 图片缓存，固件里没有一张角色 PNG。启动不需要解码，换角色不用重新打包图片。
2. **任意分辨率**：同一份数据在 186×186 引擎里和 466×466 全屏里长得一样。想让球更大，只调 `init()` 的半径。
3. **可参数化变形**：整颗球是一个半径函数 `r(θ)`，表情只是给几个低阶模态设目标值，压扁、拉长、倾斜、弹跳都是同一个公式的自然结果。
4. **省 flash**：形象数据总共 1.6 KiB，代码约 14 KiB；对比之下，48 MiB 的贴图连一个 OTA 槽都放不下。
5. **好调**：颜色、圆度、弹性、表情全部是运行时接口，滑一下参数下一帧就能看到，不需要重新出资源。

代价也说清楚：程序化适合"几何清晰、颜色简单"的角色（球、方块、云、果冻）。如果你想画带渐变、毛边、手绘纹理的角色，正确做法是走第 4.3 节的"换渲染器"路线，直接 blit 一张 RGB565 贴图——接口是同一套。

---

## 2. 渲染管线

一帧从参数到屏幕，顺序是这样的：

```text
                 qiuqiu_data.h（纯数据，1.6 KiB）
   ┌───────────────────────────────────────────────────────────┐
   │ 18 个眼环(24 点)    18 个眼位    15 个表情(睁眼/偏移/动画)  │
   └──────────────┬────────────────────────────────────────────┘
                  │  pools + pool_ms 选环 · openness · anims · gaze
                  ▼
     ┌────────────────────────┐        ┌──────────────────────────┐
     │ 24 点眼轮廓（设计坐标） │        │ 果冻弹簧：k=2/3/4 三个模态 │
     └───────────┬────────────┘        └────────────┬─────────────┘
                 │ Chaikin 平滑 1~3 次              │ 半径调制 r(θ)
                 │ 24 → 48 / 96 / 192 点             │
                 ▼                                  ▼
     ┌────────────────────────┐        ┌──────────────────────────┐
     │ 椭圆拟合混合（圆度参数）│        │ 96 点球体轮廓              │
     └───────────┬────────────┘        └────────────┬─────────────┘
                 └───────────────┬─────────────────┘
                                 ▼
              ┌────────────────────────────────────────┐
              │ fill_poly_aa：2 条子扫描线 + 水平分数   │
              │ 覆盖率；内部整块写，只有边缘做混合      │
              └───────────────────┬────────────────────┘
                                  ▼
              RGB565 帧缓冲（W×H，行优先，无 padding）
                                  │
                    最近邻 2× 放大 → 372×372 视口
                                  │
              setAddrWindow + writePixels（分块 8192 px）
                                  ▼
                       466×466 AMOLED（CO5300）
```

逐步说：

**① 选表情、选眼环。** `kExprs[expr]` 给出这一帧用哪些眼环、睁眼百分比、双眼偏移和持续动画。`pools[]` 里存 1~4 个眼环下标，`pool_ms` 是每个环的停留时间；引擎按下标做伪随机轮换（黄金比例哈希），所以眼睛会在几种眼型之间自然变化，而不是机械循环。

**② 算 24 个眼轮廓点。** 眼位 = `(160 + dxl + place.lx, 120 + dyl + place.ly)`，环的每个点再叠加本帧的动画偏移和注视偏移：

```text
x[i] = 160 + dxl + place.lx + ring.x[i] + anim_x + gaze.x
y[i] = 120 + dyl + place.ly + ring.y[i] * openness + anim_y + gaze.y
```

`openness` 只缩放 y，等于"眯眼"；眨眼包络也乘在这里（最低 0.045，不会完全闭死）。

**③ Chaikin 平滑。** 24 个点直接连多边形会有折线感，所以做 1~3 次角切细分：每次把每条边替换成 `0.75·p + 0.25·q` 和 `0.25·p + 0.75·q` 两个点，点数翻倍。默认 1 次（48 点），`set_eye_smoothing(2)` 是 96 点、`3` 是 192 点。

**④ 圆度。** `set_eye_roundness(r)` 会把轮廓朝"最佳拟合椭圆"混合 `r` 比例（PCA 求主轴，再把点投影到椭圆上）。`0` 是原版眼环形状，`0.45` 是固件默认的圆润版，上限 0.6。

**⑤ 球体变形。** 球不是正圆，半径是角度函数，来自三个低阶模态：

```cpp
    const float R = 80.0f * g_cs * (1.0f + 0.008f * sinf(g_phase * 1.6f));  // 呼吸
    for (int i = 0; i < kBallSegs; i++) {
        float th = 6.2831853f * (float)i / (float)kBallSegs;
        float c = cosf(th), s = sinf(th);
        float c2 = 2.0f * c * c - 1.0f, s2 = 2.0f * s * c;
        float c3 = 4.0f * c * c * c - 3.0f * c;
        float s3 = 3.0f * s - 4.0f * s * s * s;
        float c4 = 8.0f * c * c * c * c - 8.0f * c * c + 1.0f;
        float rad = R * (1.0f + jc2 * c2 + js2 * s2 + jc3 * c3 + js3 * s3 + jc4 * c4);
        bx[i] = g_cx + rad * c;
        by[i] = cy_j + rad * s;
    }
    fill_poly_aa(fb, bx, by, kBallSegs, g_ball_color);
```

`jc2/js2/jc3/js3/jc4` 每个都是一个**二阶弹簧**的当前值：

```text
x'' = ω²(target − x) − 2ζω·x'
```

`target` 由表情查表得到，切表情、眨眼、点按都会给速度一个冲量，于是球会"duang"地回弹。ω 默认 `2π × 2.1 ≈ 13.19 rad/s`，阻尼比 ζ = 0.16。

**⑥ 眼睛跟着球一起压扁。** 球体模态 `jc2` 同时作用在眼睛上：

```cpp
    const float sqx = 1.0f + jc2, sqy = 1.0f - jc2;
    const float eye_dy = g_jelly.bounce * 0.8f;   // 弹跳时眼睛稍慢半拍
```

这一步是"脸不像贴在球上"的关键，自己做渲染器时容易漏掉。

**⑦ 抗锯齿填充。** `fill_poly_aa()` 对每条扫描线取 2 条子扫描线，求与多边形边的交点、排序，再成对填充：

- 中间整段用 32 位整块写（`memcpy` 一次写 2 像素），只有左右两端做分数覆盖率混合；
- 这个改动是性能拐点——之前的逐像素浮点循环占了大头（见第 6 节）。

**⑧ 只清必要区域，然后交给 app 推屏。** 引擎只清"球可能占到的包围盒"（`80 × cs × 1.12 + 8` 半径），不是全屏；`render()` 返回后由 `app_qiuqiu.cpp` 做放大和 `writePixels`。渲染和推屏分离，是为了让渲染逻辑能脱离 M5GFX 在电脑上跑（第 5 节）。

---

## 3. 五分钟：改出你自己的球

### 3.1 四个旋钮

| 想改什么 | 调用 | 取值范围 | 生效时机 |
|---|---|---|---|
| 球色 / 眼色 | `set_agent_color(0..2)`、改两行颜色常量 | 0=白 1=蓝 2=红 | 下一帧 |
| 眼睛圆度 | `set_eye_roundness(r)` | 0 ~ 0.6（超出被夹紧） | 下一帧 |
| 弹性 / 抖动 | `set_tuning(ω, ζ, amp, idle)` | ω>0.1、ζ>0、amp>0、idle≥0 | 下一帧 |
| 眼轮廓精度 | `set_eye_smoothing(n)` | 1 / 2 / 3 → 48 / 96 / 192 点 | 下一帧 |
| 表情 | `set_expression(i, now_ms)` | 0 ~ `expression_count()-1` | 约 30 ms 时间常数平滑过渡 |

所有 `set_*` 都只改全局状态、不做绘制；下一次 `render()` 才生效。这意味着你可以在任意任务里改参数，只要别和渲染同时写。

### 3.2 换颜色

引擎内置三色（对应三种后端身份）：白 `0xFFFF`、蓝 `0x04BF`、红 `0xF800`；眼睛固定纯黑 `0x0000`。先用一个顺手的小工具把 `#RRGGBB` 转成 RGB565：

```cpp
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
```

然后把 `qiuqiu_engine.cpp` 里的 `set_agent_color()` 换成自己的调色板（下面这份可以直接整段替换）：

```cpp
void set_agent_color(int idx)
{
    if (idx < 0 || idx > 2) idx = 0;
    g_agent_color_idx = idx;

    static const uint16_t kBallColors[3] = {
        rgb565(0xFF, 0x8A, 0x3D),   // 0 橘   0xFC47
        rgb565(0x3D, 0xD6, 0xC4),   // 1 青   0x3EB8
        rgb565(0xFF, 0x6B, 0x6B),   // 2 珊瑚 0xFB4D
    };
    g_ball_color = kBallColors[idx];
}
```

眼睛颜色是文件顶部的 `constexpr uint16_t kColorEye`，改成 `rgb565(0x33, 0x33, 0x33)`（深灰 `0x3186`）会柔和很多——纯黑在 AMOLED 上对比度非常硬。

> 注意：`set_agent_color` 只接受 0/1/2，传别的值会被夹回 0。这条 API 同时被按键（B 键循环换色）和蓝牙指令 `0x50` 使用，见 `firmware/main/voice/voice_task.cpp`。

### 3.3 眼睛圆度

```cpp
qiuqiu::set_eye_roundness(0.0f);   // 忠于原版眼环：偏长、眼角略尖
qiuqiu::set_eye_roundness(0.45f);  // 固件默认：明显更圆润
qiuqiu::set_eye_roundness(0.6f);   // 上限：接近椭圆，圆到有点"呆"
```

圆度是"朝拟合椭圆混合"的比例，不改变眼睛大小和位置。想更圆又不想丢细节，可以把圆度和小眼环配合用。

### 3.4 弹性手感

```cpp
// set_tuning(omega, zeta, amp_scale, idle_scale)
//   omega      : 弹簧角频率 = 2π × 回弹频率(Hz)，越大弹得越快
//   zeta       : 阻尼比，越小晃得越久（0.08 会抖好几下，0.35 几乎不晃）
//   amp_scale  : 表情形变幅度倍数（1.0 = 数据原值）
//   idle_scale : 常驻微抖动倍数（0 = 表面完全静止）

// 出厂手感：2.1 Hz，ζ=0.16，动画幅度和微抖动都是 1.0
qiuqiu::set_tuning(13.194689f, 0.16f, 1.0f, 1.0f);

// 稳重一点：1.6 Hz，阻尼大，几乎不晃
qiuqiu::set_tuning(10.053096f, 0.35f, 0.8f, 0.5f);

// 弹过头：2.6 Hz，阻尼小，切表情能晃三下
qiuqiu::set_tuning(16.336281f, 0.08f, 1.3f, 1.2f);
```

`amp_scale` 别开太大：形变幅度超过约 ±15% 后，球的轮廓会顶出引擎默认的清理区，偶尔留一条旧边缘的残影。想大改形状，去改 `shape_for_expr()` 里的目标值，并顺手把清理半径加大。

### 3.5 改表情：编辑 `qiuqiu_data.h`

数据结构只有两个：

```cpp
struct AnimData {
    uint8_t wave, axis, target, amp;   // 波形 / 轴 / 作用眼 / 幅度（设计像素）
    uint16_t period_ms, phase_ms;      // 周期与相位
};

struct ExprData {
    const char* name;
    uint8_t pools[4];        // 眼环下标（kRings 的索引），最多 4 个
    uint8_t pool_count;      // 实际用几个（1~4）
    uint16_t pool_ms;        // 每个环停留多久（毫秒）
    uint8_t openness;        // 睁眼百分比（0~100 的整数）
    int8_t dxl, dyl, dxr, dyr;         // 左/右眼中心偏移（设计像素）
    AnimData anims[2];       // 最多两条持续动画
    uint8_t anim_count;      // 实际用几条（0~2）
};
```

三个枚举决定了动画长什么样：

| 枚举 | 取值 | 行为 |
|---|---|---|
| `Wave` | `WAVE_NONE` 0 | 不动 |
| | `WAVE_GLANCE` 1 | 看左/看右并停留，带缓入缓出（像"张望"） |
| | `WAVE_SINE` 2 | 平滑往复（正弦感） |
| | `WAVE_JITTER` 3 | 每个周期换一个随机位置（黄金比例哈希） |
| `Axis` | `AXIS_X` 0 / `AXIS_Y` 1 | 水平 / 垂直 |
| `Target` | `TARGET_BOTH` 0 / `TARGET_LEFT` 1 / `TARGET_RIGHT` 2 | 双眼 / 只左眼 / 只右眼 |

举个完整例子：新增一个"眯眼、慢慢左右看、视线略向下"的表情 `CHILL`。

```cpp
// ① qiuqiu_data.h 顶部：表情数量 +1
constexpr int kExprCount = 16;          // 原来是 15

// ② 数组末尾追加一条（保持缩进对齐即可）
inline constexpr ExprData kExprs[kExprCount] = {
    /* ……原有 15 条不动…… */
    {"BORED", {4, 16, 0}, 3, 4750, 100, -3, 1, 4, -1, {{2, 0, 0, 2, 6800, 0}, {0, 0, 0, 0, 0, 0}}, 1},
    // 眯到 55%、眼睛下移 3 px、两家小眼环每 4.8 s 换一次、水平方向缓慢摆 ±2 px
    {"CHILL", {11, 15}, 2, 4800, 55, 0, 3, 0, 3, {{2, 0, 0, 2, 3200, 0}, {0, 0, 0, 0, 0, 0}}, 1},
};
```

要点：

- `pools` 是**眼环下标**，必须小于 `kRingCount`（18）；`11`、`15` 是比较小的眼环，适合眯眼。
- `openness` 是百分比，会乘上眨眼包络；`55` 表示眼睛只有 55% 高，不是半闭眼皮。
- `anims` 里的 `amp` 单位是设计像素，2 表示 ±2 px；`period_ms` 决定快慢，`phase_ms` 让两只眼错开。
- 左/右眼**不能单独设睁眼程度**——数据格式里只有位置偏移是分左右的。要做"单眼眨"，得改渲染器（4.3 节）。
- 改完直接编译，按 A/B 键循环表情就能看到；`expression_count()` 自动变成 17 个可选值。

### 3.6 接回屏幕：app 里的那 30 行

`render()` 只画到内存，推屏是 app 的事。核心路径（摘自 `app_qiuqiu.cpp`，去掉日志和错误处理）：

```cpp
// ── 打开应用：分配 PSRAM + 初始化 ───────────────────────────────
static constexpr int RW = 186, RH = 186;          // 引擎渲染分辨率
static constexpr int QW = 372, QH = 372;          // 推屏视口
static constexpr int VX = (466 - QW) / 2, VY = (466 - QH) / 2;

_fb[0] = (uint16_t*)heap_caps_malloc((size_t)RW * RH * 2, MALLOC_CAP_SPIRAM);
_fb[1] = (uint16_t*)heap_caps_malloc((size_t)RW * RH * 2, MALLOC_CAP_SPIRAM);
_up    = (uint16_t*)heap_caps_malloc((size_t)QW * QH * 2, MALLOC_CAP_SPIRAM);

qiuqiu::init(RW, RH, 168.0f * 0.5f);              // 半径随分辨率同步减半
qiuqiu::set_tuning(13.194689f, 0.16f, 1.0f, 1.0f);
qiuqiu::set_eye_roundness(0.45f);
qiuqiu::set_expression(0, GetHAL().millis());

// ── 每帧：渲染 → 2× 最近邻放大 → 推窗 ─────────────────────────
uint32_t now = GetHAL().millis();
if (now - _last_render < frame_ms) return;        // 动画 50 ms / 静止 120 ms
_last_render = now;

int back = 1 - _fb_idx;                           // 双缓冲，画另一块
qiuqiu::render(_fb[back], now);

for (int y = 0; y < RH; y++) {
    const uint16_t* src = _fb[back] + (size_t)y * RW;
    uint16_t* d0 = _up + (size_t)(2 * y) * QW;
    uint16_t* d1 = d0 + QW;
    for (int x = 0; x < RW; x++) {
        uint16_t c = src[x];
        d0[2 * x] = d0[2 * x + 1] = c;            // 放大 2×2
        d1[2 * x] = d1[2 * x + 1] = c;
    }
}

LGFX_Device& gfx = GetHAL().getDisplay();
GetHAL().lvglLock();                              // LVGL 可能同时在用面板
gfx.startWrite();
gfx.setAddrWindow(VX, VY, QW, QH);
const lgfx::rgb565_t* p = (const lgfx::rgb565_t*)_up;
for (uint32_t off = 0; off < (uint32_t)QW * QH; off += 8192) {
    uint32_t left = (uint32_t)QW * QH - off;
    gfx.writePixels(p + off, left > 8192 ? 8192 : left);
}
gfx.endWrite();
GetHAL().lvglUnlock();
_fb_idx = back;
```

三个容易踩的点：

- **半径和分辨率要一起缩放**。固件里 168 px 是 466 屏上的目标大小，引擎只画 186，所以传 84。
- **放大必须用最近邻**（2×2 复制）。双线性要读 4 个像素做插值，在这块屏上不划算，边缘还会发灰。
- **`writePixels` 的指针类型是 `lgfx::rgb565_t*`**。RGB565 的字节序由 M5GFX 处理，你自己拼 SPI 时别忘记。

---

## 4. 进阶：完全自定义角色

### 4.1 三条路线，按投入排序

| 路线 | 改什么 | 工作量 | 能做成什么样 |
|---|---|---|---|
| A. 改数据 | `qiuqiu_data.h` 的颜色/圆度/表情 | 分钟级 | 同一个球的不同性格 |
| B. 换几何 | 自己生成 24 点眼环 | 半小时 | 猫眼、星星眼、方眼、闭眼弧线 |
| C. 换渲染器 | 重写 `render()` | 半天 | 任意 2D 风格：贴图、粒子、像素画 |

三条路线可以叠加：先用 B 换眼型，再用 C 换成"球 + 粒子"。

### 4.2 方式一：自己生成 24 点环

眼环就是"围绕眼中心的一圈 24 个点"，引擎把它平滑后填充成黑色多边形。写个 Python 生成器最省事：

```python
#!/usr/bin/env python3
"""生成 qiuqiu 的 24 点眼环（设计坐标：中心 (0,0)，y 向下为正）。"""
import math

N = 24  # 引擎写死 24 点，不要改


def ellipse(rx, ry):
    return [(rx * math.cos(2 * math.pi * i / N),
             ry * math.sin(2 * math.pi * i / N)) for i in range(N)]


def cat_eye(w, h, lid=0.62, sharp=1.35):
    """超椭圆杏仁眼：sharp 越小眼角越尖；lid<1 压扁上眼睑。"""
    pts = []
    for i in range(N):
        t = 2 * math.pi * i / N
        c, s = math.cos(t), math.sin(t)
        x = math.copysign(abs(c) ** (2.0 / sharp), c) * w
        y = math.copysign(abs(s) ** (2.0 / sharp), s) * h
        if s < 0:            # 屏幕坐标里 s<0 是上半部分
            y *= lid
        pts.append((x, y))
    return pts


def to_int8(pts):
    xs = [int(round(x)) for x, _ in pts]
    ys = [int(round(y)) for _, y in pts]
    for v in xs + ys:
        assert -127 <= v <= 127, f"坐标越界: {v}"
    return xs, ys


def emit(pts, label):
    xs, ys = to_int8(pts)
    print("    {{{{{}}}, {{{}}}}},  // {}".format(
        ", ".join(map(str, xs)), ", ".join(map(str, ys)), label))


if __name__ == "__main__":
    emit(ellipse(13, 17), "椭圆 rx=13 ry=17")
    emit(cat_eye(15, 13), "猫眼 almond")
```

运行后每行就是一个可以直接粘进 `kRings` 的条目（数值已实测在 ±127 内）：

```text
    {{13, 13, 11, 9, 7, 3, 0, -3, -6, -9, -11, -13, ...}, {0, 4, 8, ...}},  // 椭圆 rx=13 ry=17
    {{15, 14, 12, 9, 5, 2, 0, -2, -5, -9, ...}, {0, 2, 5, 8, 11, 12, 13, ...}},  // 猫眼 almond
```

落地步骤：

1. 挑一个不常用的下标（比如 `kRings[0]`），整行替换成生成的行；
2. 想保留原环就追加，同时把 `kRingCount` 加 1，并**在 `kPlaces` 里补一条对应眼位**（两个数组必须一样长）；
3. 把想用它的表情的 `pools` 指到这个下标；
4. 用第 5 节的 host 预览确认形状，再刷机。

约束与坑：

- **必须 24 个点、首尾相连**，且按角度顺序排列（顺/逆时针都行）。乱序会让扫描线填充出现自交的怪块。
- 坐标是 `int8_t`，范围 ±127；眼睛形状常规在 ±20 以内，超过 30 会顶到别的眼。
- **左右眼共用同一组点，不做镜像**。猫眼会两只朝同一方向；要左右对称就画对称形状，或直接改渲染器。
- 环只描述形状，**位置由 `kPlaces` 决定**。`kPlaces` 的四元组是 `{左眼x, 左眼y, 右眼x, 右眼y}` 的偏移。
- `pools` 里是环下标，不是眼位下标——两个数组各管各的。

### 4.3 方式二：换整个渲染器

如果几何已经描述不了你的角色，就自己实现 `render()`。约定只有一条：**把 `uint16_t* fb` 当成一块 `w × h` 的 RGB565 画布，画成什么样都行**。

下面这个方块脸约 30 行，直接替换 `qiuqiu_engine.cpp` 里的 `render()` 就能跑（其余函数保持原样，链接不受影响）：

```cpp
void render(uint16_t* fb, uint32_t now_ms)
{
    memset(fb, 0, (size_t)g_w * g_h * sizeof(uint16_t));   // 自己清屏

    auto rect = [&](int x0, int y0, int w, int h, uint16_t c) {
        for (int y = y0; y < y0 + h; y++) {
            if (y < 0 || y >= g_h) continue;
            int a = x0 < 0 ? 0 : x0;
            int b = x0 + w > g_w ? g_w : x0 + w;
            for (int x = a; x < b; x++) fb[(size_t)y * g_w + x] = c;
        }
    };

    const int cx = g_w / 2;
    const int bob = (int)(6.0f * ((now_ms % 2400) - 1200) / 1200.0f);  // ±6 px 点头
    const int cy = g_h / 2 + bob;
    const int s = (int)(80.0f * g_cs * 1.4f);            // 脸边长，跟随 init 的半径

    rect(cx - s / 2, cy - s / 2, s, s, g_ball_color);             // 方块脸
    const int eye = s / 7, gap = s / 5;
    const int eh = ((now_ms % 3000) < 120) ? eye / 6 : eye;       // 每 3 s 眨 120 ms
    rect(cx - gap - eye, cy - eye / 2, eye, eh, 0x0000);          // 左眼
    rect(cx + gap, cy - eye / 2, eye, eh, 0x0000);                // 右眼
    rect(cx - s / 4, cy + s / 5, s / 2, s / 18, 0x0000);          // 一条直线嘴
}
```

它复用了引擎里的 `g_w / g_h / g_cs / g_ball_color` 这几个静态变量，所以能直接用 `init()` 传进来的尺寸和半径。想做贴图角色，就把 `rect()` 换成"逐像素从一张 RGB565 数组里取样"；想做粒子，就把 `rect()` 换成"画 1~3 px 的小点"，逻辑一样。

### 4.4 接口契约

换渲染器时，下面这些必须遵守——它们决定了 app 那边不改一行代码也能跑：

| 约定 | 说明 |
|---|---|
| 帧缓冲格式 | `uint16_t*`，长度 `w*h`，**行优先，无 padding**；每个元素是原生 RGB565（不是字节流） |
| 绘制责任 | 你必须自己清屏或覆盖要画的区域；不要假设缓冲区里是上一帧 |
| 单帧时间 | 目标 20 fps（50 ms/帧）；单帧超过 40 ms 会让按键/触摸明显迟滞 |
| 调用频率 | 动画时 50 ms 一次，静止时 120 ms 一次；由 app 决定，不保证固定节拍 |
| 时间来源 | 只用传入的 `now_ms`（uint32 毫秒，可回绕）；不要在里面读 `millis()`，host 预览没有 HAL |
| 内存 | 不要在 `render()` 里 `malloc`；临时缓冲用 `static` 或 PSRAM 预分配 |
| 线程 | 单任务调用；引擎是全局单例，不支持多实例 |
| 依赖 | `render()` 里不要碰 LVGL / M5GFX；推屏由 app 在返回后统一做 |
| 尺寸 | 任意 `w/h`；所有坐标按 `g_w/g_h` 自适应，别写死 466 |
| 混合 | 需要透明/半透明就自己算 RGB565 混合（参考引擎里的 `blend565()`） |

如果你想把整个 `qiuqiu_engine.cpp` 换成新文件（而不只是改 `render()`），这些符号必须保留，否则 app 链接失败：

| 符号 | app 是否用到 | 空实现建议 |
|---|---|---|
| `init(w, h, radius)` | 是 | 记录尺寸/半径 |
| `render(fb, now)` | 是 | 你的绘制核心 |
| `set_expression(i, now)` / `expression()` / `expression_count()` | 是 | 至少支持 1 个表情，返回 1 |
| `set_tuning(...)` / `set_eye_roundness(...)` | 是 | 空函数即可 |
| `set_agent_color(i)` / `cycle_agent_color()` | 是（按键与蓝牙换色） | 维护一个 0~2 的下标 |
| `agent_color_index()` / `expression_name(i)` | 否 | 返回 0 / 名字字符串 |
| `set_gaze(h, v)` / `request_blink(now)` / `poke(now)` | 否 | 空函数（想支持就自己实现） |
| `set_eye_smoothing(n)` / `init(w, h)` 两参重载 | 否 | 空函数（两参重载在头文件里但没有实现，**请只用三参版本**） |

### 4.5 接回固件

1. **只留一份实现**。`firmware/main/CMakeLists.txt` 用 `file(GLOB_RECURSE ... "apps/*.cpp")` 收集源文件，同目录下两个 `.cpp` 会一起编译并报重复定义。把旧文件改名成 `qiuqiu_engine.cpp.bak`（glob 不认 `.bak`），或干脆覆盖原文件。
2. 先在电脑上按第 5 节验证形状和帧耗时，再 `idf.py build`。
3. 上机后确认三件事：表情能切（A/B 键、触摸）、推屏不闪、熄屏/唤醒正常。
4. 如果你的渲染器比原引擎重，先把 `frame_ms` 从 50 调大再逐步逼近，别一上来就满帧。

---

## 5. 在电脑上预览（不刷机）

### 5.1 为什么能直接跑

`qiuqiu_engine.cpp` 只 include 了 `<math.h>` 和 `<string.h>`，不依赖 ESP-IDF、M5GFX、LVGL。整个引擎就是一个"给我一块内存，我画一帧"的纯函数——所以能在 PC 上原样编译，几秒钟出图。

### 5.2 一个零依赖的 PNG 写出（`png_io.h`）

固件里没有文件系统需求，但电脑上要出图。用 zlib 写 PNG 最短，50 行够了：

```cpp
// png_io.h — 最小 PNG 写出（RGB888），只依赖 zlib
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <zlib.h>

namespace pngio {

inline void put_be32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

inline void put_chunk(std::vector<uint8_t>& out, const char type[4],
                      const uint8_t* data, size_t n)
{
    put_be32(out, (uint32_t)n);
    const size_t body = out.size();
    out.insert(out.end(), type, type + 4);
    if (n) out.insert(out.end(), data, data + n);
    put_be32(out, (uint32_t)crc32(0, out.data() + body, (uInt)(n + 4)));
}

// fb: 行优先 RGB565，w*h 像素
inline bool save_png(const char* path, const uint16_t* fb, int w, int h)
{
    std::vector<uint8_t> raw;
    raw.reserve(((size_t)w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);                                 // 每行 filter = None
        for (int x = 0; x < w; x++) {
            uint16_t c = fb[(size_t)y * w + x];
            uint8_t r = (uint8_t)((c >> 11) & 0x1F);
            uint8_t g = (uint8_t)((c >> 5) & 0x3F);
            uint8_t b = (uint8_t)(c & 0x1F);
            raw.push_back((uint8_t)(r * 255 / 31));        // 5 bit -> 8 bit
            raw.push_back((uint8_t)(g * 255 / 63));        // 6 bit -> 8 bit
            raw.push_back((uint8_t)(b * 255 / 31));
        }
    }
    uLongf zlen = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 6) != Z_OK)
        return false;
    z.resize(zlen);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13] = {0};
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;                                            // bit depth
    ihdr[9] = 2;                                            // color type: RGB
    put_chunk(out, "IHDR", ihdr, 13);
    put_chunk(out, "IDAT", z.data(), z.size());
    put_chunk(out, "IEND", nullptr, 0);

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

}  // namespace pngio
```

### 5.3 主程序（`host_preview.cpp`）

```cpp
// host_preview.cpp — 在电脑上跑引擎，输出 PNG 序列
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "png_io.h"
#include "qiuqiu_engine.h"

int main(int argc, char** argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s OUT_DIR W H BALL_RADIUS EXPR FRAMES\n"
                        "  e.g. %s out 466 466 168 1 60\n", argv[0], argv[0]);
        return 1;
    }
    const int w = atoi(argv[2]), h = atoi(argv[3]);
    const float radius = (float)atof(argv[4]);
    const int expr = atoi(argv[5]), frames = atoi(argv[6]);
    const int fps = 20, step_ms = 1000 / fps;

    std::vector<uint16_t> fb((size_t)w * h, 0);
    qiuqiu::init(w, h, radius);
    qiuqiu::set_tuning(13.194689f, 0.16f, 1.0f, 1.0f);  // 与固件一致
    qiuqiu::set_eye_roundness(0.45f);

    uint32_t now = 1000;                 // 起始时刻随意，引擎只关心差值
    qiuqiu::set_expression(expr, now);   // 切表情会触发一次果冻冲量
    char path[256];
    for (int i = 0; i < frames; i++) {
        now += step_ms;
        if (i == 8) qiuqiu::request_blink(now);   // 第 8 帧眨一下
        qiuqiu::render(fb.data(), now);
        snprintf(path, sizeof(path), "%s/frame_%04d.png", argv[1], i);
        if (!pngio::save_png(path, fb.data(), w, h)) {
            fprintf(stderr, "write %s failed\n", path);
            return 1;
        }
    }
    printf("wrote %d frames (%dx%d, expr=%d)\n", frames, w, h, expr);
    return 0;
}
```

### 5.4 编译与运行

把引擎源码拷到一个临时目录（别污染仓库），再放入上面两个文件：

```bash
REPO=$(pwd)                       # 在仓库根目录执行
mkdir -p /tmp/qiuqiu-preview && cd /tmp/qiuqiu-preview
cp "$REPO"/firmware/main/apps/app_qiuqiu/qiuqiu_engine.h \
   "$REPO"/firmware/main/apps/app_qiuqiu/qiuqiu_engine.cpp \
   "$REPO"/firmware/main/apps/app_qiuqiu/qiuqiu_data.h .
# 把 png_io.h 和 host_preview.cpp 放进当前目录后：
g++ -std=c++17 -O2 host_preview.cpp qiuqiu_engine.cpp -lz -o host_preview

mkdir -p out
./host_preview out 466 466 168 1 60      # 60 帧 HAPPY
```

也可以不用拷贝、直接指向固件目录：

```bash
g++ -std=c++17 -O2 host_preview.cpp \
    "$REPO"/firmware/main/apps/app_qiuqiu/qiuqiu_engine.cpp \
    -I"$REPO"/firmware/main/apps/app_qiuqiu -I. -lz -o host_preview
```

CMake 版本（可选）：

```cmake
cmake_minimum_required(VERSION 3.16)
project(qiuqiu_host_preview CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(ZLIB REQUIRED)
add_executable(host_preview host_preview.cpp qiuqiu_engine.cpp)
target_include_directories(host_preview PRIVATE .)
target_link_libraries(host_preview PRIVATE ZLIB::ZLIB)
```

### 5.5 合成视频 / 表情九宫格

```bash
# 动图（2× 放大的帧 320px 宽，方便发群）
ffmpeg -framerate 20 -i out/frame_%04d.png \
  -vf "scale=320:-1:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
  -y qiuqiu.gif

# 视频
ffmpeg -framerate 20 -i out/frame_%04d.png -pix_fmt yuv420p -y qiuqiu.mp4

# 15 个表情拼一张对比图
mkdir -p strip
for e in $(seq 0 14); do
  mkdir -p strip/$e
  ./host_preview strip/$e 240 240 88 $e 20
done
magick montage strip/*/frame_0019.png -tile 5x3 -geometry 200x200+2+2 exprs.png
```

### 5.6 host 预览的坑

- **半径要和尺寸匹配**。`466 466 168` 是"466 屏上的球"，`240 240 88` 是等比缩小；乱配会出现球比画面还大或小得看不见。
- **时间步长**。引擎按 `now_ms` 积分弹簧，固定 `+50` 表示 20 fps；改成 `+16` 只是采样更密，不代表固件能跑 60 fps。
- **别在 host 上测帧率并当成设备数据**。x86 与 ESP32-S3 差两个数量级，第 6 节有真实设备数字。
- **强冲量 + 大半径**偶尔会在球顶留一条细弧（清理区边界），这是已知的窄边界情况；预览时用 `request_blink()` 而不是 `poke()` 就看不到。

---

## 6. 性能预算

### 6.1 真机实测

测试条件：466×466 AMOLED、ESP32-S3 @240 MHz、PSRAM 80 MHz、20 fps 循环。数字来自设备串口日志里的 `DEBUG fps~ render=…ms push=…ms`。

| 配置 | 引擎渲染 | 推屏 | 合计 | 帧率 |
|---|---|---|---|---|
| 全屏 466×466 渲染 + 全屏推屏 | 72 ms | 36 ms | 108 ms | ≈9 fps |
| 186×186 渲染 + 2× 放大 + 372×372 视口（当前固件） | 25 ms（含放大） | 23 ms | 48 ms | ≈19–20 fps |

优化前的全屏渲染是 110 ms；把 `fill_poly_aa()` 的内层改成整块写之后降到 72 ms，再叠加"半分辨率渲染 + 窗口推屏"才到 25 ms。这两步的收益大致是 1.5× 和 3×。

### 6.2 帧预算怎么算

```text
每帧预算 = 1000 / 目标帧率
  20 fps → 50 ms（当前用掉 48 ms，几乎没有余量）
   8 fps → 125 ms（静止时用 120 ms 一拍）
```

建议：

- **动画 20 fps、静止 8 fps**。球在动的时候人眼对帧率敏感，静止时降到 8 fps 完全看不出来，还省电。
- 加新效果前先按"渲染 ≤ 25 ms、推屏 ≤ 23 ms"分配预算；任何一项翻倍都会掉到 12 fps 以下。
- 如果只改颜色/表情（不动分辨率），性能基本不变——瓶颈在填充像素数和推屏字节数。

### 6.3 内存占用

| 缓冲 | 大小 | 位置 |
|---|---|---|
| 渲染双缓冲 `2 × 186×186×2` | 138,384 B（135 KiB） | PSRAM |
| 推屏缓冲 `372×372×2` | 276,768 B（270 KiB） | PSRAM |
| **合计（常驻）** | **415,152 B（405 KiB）** | PSRAM |
| 一次性清屏缓冲 `466×466×2`（用完释放） | 434,312 B（424 KiB） | PSRAM |
| 对比：若用全屏双缓冲 `2 × 466×466×2` | 868,624 B（848 KiB） | PSRAM |
| 引擎静态暂存（平滑/扫描线/轮廓） | 14,636 B | 内部 RAM `.bss` |
| 形象数据（18 环 + 18 眼位 + 15 表情 + 名字） | 1,584 B | flash `.rodata` |

半分辨率方案不只省 CPU，也把常驻 PSRAM 从 848 KiB 压到 405 KiB，给 BLE 语音和 LVGL 留出空间。

### 6.4 推屏带宽是硬上限

372×372×2 = 276,768 B，实测约 23–24 ms，即 **≈11.5 MB/s**。加大 `writePixels` 的分块没有改善，说明瓶颈是 SPI 带宽而不是调用次数。由此可推：

- 全屏 466×466 推屏 = 434,312 B ≈ 36 ms（与实测一致）；
- 想再快只能减少像素（缩视口、脏矩形）或者换更快的总线配置（DMA 队列），继续调 chunk 没用。

### 6.5 优化清单（按性价比排序）

1. **降渲染分辨率 + 最近邻放大**：186→372 省掉约 3/4 的填充像素，最大的一笔。
2. **只推球所在视口**：372² 比 466² 少 36% 的字节。
3. **填充内层整块写**：32 位一次写 2 像素，只有边缘做覆盖率混合。
4. **减球段数 / 子采样行数**：当前 96 段、2 条子扫描线；降到 64 / 1 会更糙。
5. **静止降帧**：20 → 8 fps，平均功耗明显下降。
6. **别每帧全屏 memset**：引擎只清球包围盒；自绘渲染器也别在全屏尺寸上无条件清屏。

### 6.6 在电脑上量算法成本

host 侧的耗时只用来比较"改这个算法是变快还是变慢"，不要外推到设备：

```text
466×466, r=168 : 0.11 ms/帧
466×466, r=233 : 0.15 ms/帧
186×186, r=84  : 0.06 ms/帧
```

（x86-64、`-O2`、单线程；同一台机器上改算法前后对比有效。）设备端以串口日志的 `render=…ms push=…ms` 为准。

---

## 7. 许可提示

仓库自带的形象数据（`qiuqiu_data.h` 里的眼环、眼位、表情参数，以及白球配色）来自 **Emotion Ball 社区许可**（`licenses/LICENSE-emotion-ball.txt`、`licenses/NOTICE-emotion-ball.md`）：

- 球形角色的**视觉形象**仅限个人学习、研究与非商业分享，**禁止任何商业用途，且永不提供商业授权**；
- 引擎代码与表情配置数据本身是"非商业免费 + 可另行取得商业授权"，商业使用时必须搭配你自有或已获授权的角色形象；
- 分发本项目时请原样保留 `licenses/` 下的许可与 NOTICE 文件。

因此：

| 你的用途 | 要做什么 |
|---|---|
| 自己玩、学习、非商业分享 | 直接用，注明出处即可 |
| 想商用 / 上架 / 接商单 | **自己设计角色**：换掉 `qiuqiu_data.h` 的 18 个眼环、18 个眼位和 15 条表情，换掉配色；必要时按 4.3 节换成自己的渲染器 |
| 二次分发 | 保留 `licenses/`，并保留 `qiuqiu_engine.h` / `qiuqiu_data.h` 顶部的来源注释 |

补充两点，避免误解：

- `qiuqiu_data.h` 头部注明它由设计工具生成。手改之后不要再跑生成脚本，否则会被覆盖。
- 引擎文件头注明绘制思路参考了一个 AGPL 的社区项目。是否构成衍生作品、能否商用，请以该上游项目的许可为准；本仓库不对此做法律判断。

---

## 附录 A：API 速查

| 函数 | 作用 | 备注 |
|---|---|---|
| `init(w, h, radius_px)` | 设定画布尺寸和球半径 | 用三参版本；两参重载未实现 |
| `set_expression(i, now_ms)` | 切表情 | 相同值直接返回，不触发冲量 |
| `expression()` / `expression_count()` / `expression_name(i)` | 当前表情 / 总数 / 名字 | 名字可直接打日志 |
| `set_agent_color(0..2)` | 白 / 蓝 / 红 | 其它值夹到 0 |
| `cycle_agent_color()` / `agent_color_index()` | 循环切换 / 当前下标 | B 键用 |
| `set_gaze(h, v)` | 注视偏移（-1~1） | 设计坐标 ±16.8 / ±10.5 px |
| `request_blink(now_ms)` | 立刻眨一次 | 自动眨眼另有一套（3.4~6.2 s） |
| `poke(now_ms)` | 果冻冲量（点按） | 参数未使用 |
| `set_tuning(ω, ζ, amp, idle)` | 弹性手感 | 见 3.4 |
| `set_eye_roundness(r)` | 0~0.6 圆度 | 固件用 0.45 |
| `set_eye_smoothing(n)` | 1~3 → 48/96/192 点 | 实现默认 1（头文件注释写 2，以实际为准） |
| `render(fb, now_ms)` | 画一帧 | 不推屏、不分配内存 |

## 附录 B：排错

| 现象 | 原因 | 处理 |
|---|---|---|
| 球很小 / 超出屏幕 | 半径没跟着分辨率缩放 | 半径按 466:186 同比减半 |
| 眼睛全是折线 | 平滑迭代被设成 1 且半径很大 | `set_eye_smoothing(2)` 或 3 |
| 切换表情时"炸开"一下 | `amp_scale` 太大 | 降到 1.0 附近，或改 `shape_for_expr()` |
| 画面撕裂 / 闪烁 | 没双缓冲，或推屏没和 LVGL 互斥 | 双缓冲 + `lvglLock()` |
| 按键没反应 | 单帧耗时过长 | 打印 `render=…ms`；先降分辨率 |
| 编译报重复定义 | 同目录有两份渲染实现 | 把旧文件改名 `.bak`（4.5 节） |
| host 预览全黑 | 没调 `set_expression()` 且时间是 0 | 起始 `now_ms` 给 1000，先切一次表情 |
| 颜色偏色 / 反色 | RGB565 字节序搞反 | 用 `lgfx::rgb565_t*` 交给 M5GFX |

---

**相关文档**：`docs/05-pitfalls.md`（真机踩坑记录）、`licenses/`（许可原文）。
