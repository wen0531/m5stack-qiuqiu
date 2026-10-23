# 03 · 自定义角色：用 C++ 画一张你自己的脸

> 本项目的二次开发教程：把 466×466 AMOLED 手表变成任意 2D 角色的舞台。
> 目标：5 分钟换色与手感；半小时改表情或眼型；半天换掉整个渲染器。
> 代码基于仓库当前实现，host 预览与示例均在本机编译运行过；路径相对仓库根目录。

## 0. 四个文件，一张地图

| 路径 | 内容 | 会改它吗 |
|---|---|---|
| `firmware/main/apps/app_qiuqiu/qiuqiu_engine.h` | 16 个公开 API | 换渲染器时保持签名 |
| `firmware/main/apps/app_qiuqiu/qiuqiu_engine.cpp` | 球体变形、眼睛多边形、抗锯齿填充、眨眼、弹簧 | 改手感 / 换 `render()` |
| `firmware/main/apps/app_qiuqiu/qiuqiu_data.h` | 18 个眼环（各 24 点）、18 个眼位、15 个表情 | 改表情 / 换眼型 |
| `firmware/main/apps/app_qiuqiu/app_qiuqiu.cpp` | PSRAM 帧缓冲、20/8 fps 节奏、推屏、按键、熄屏 | 改分辨率 / 推屏 |
| `licenses/` | Emotion Ball 许可与 NOTICE 原文 | 分发时保留 |

**设计坐标是 320×240 的"草稿纸"**：球心 (160,120)、半径 80，`qiuqiu_data.h` 里的整数都是这个坐标系下的设计像素（`int8_t`）。`init(w, h, ball_radius_px)` 负责映射：

```text
k        = ball_radius_px / 80
screen_x = (design_x - 160) * k + w / 2
screen_y = (design_y - 120) * k + h / 2
```

因此改半径等于整体缩放，数据文件一个数都不用动；但**分辨率减半时半径也要减半**（固件 186×186 用 84，372×372 用 168），否则球会缩成屏幕中间的小点。

## 1. 为什么用程序化绘制，而不是一堆图片

| 对比 | 贴图序列 | 程序化（本实现） |
|---|---|---|
| 15 个表情的存储 | 15 × 12 帧 × 372² × 2 B ≈ **48 MiB**（单个固件槽 4.94 MiB） | **1.6 KiB** 数据 + 约 14 KiB 代码 |
| 任意分辨率 | 每种分辨率重新出图 | 改一个半径参数 |
| 改一个眼神 | 重画、重导、重刷 | 改 4 个数字 |
| 表情过渡 | 硬切或补帧 | 插值天然平滑 |
| 果冻/回弹 | 只能逐帧画 | 一个弹簧方程，全局受益 |

具体收益：**无资源**（固件里没有角色图片，不占 LVGL 图片缓存、不用解码）；**任意分辨率**（同一份数据在 186 和 466 下形状一致）；**可参数化变形**（整颗球是一个半径函数，表情只是给低阶模态设目标值）；**省 flash**（48 MiB 的贴图连一个 OTA 槽都放不下）；**好调**（颜色、圆度、弹性、表情全是运行时接口，下一帧就生效）。

代价也说清楚：程序化适合"几何清晰、颜色简单"的角色（球、方块、云、果冻），做不出毛边、手绘纹理。要画带渐变和细节的角色，走 4.3 节的"换渲染器"路线直接 blit RGB565 贴图，接口是同一套。

## 2. 渲染管线

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
                                  │  最近邻 2× 放大 → 372×372 视口
                                  │  setAddrWindow + writePixels(8192 px/块)
                                  ▼
                       466×466 AMOLED（CO5300）
```

① **选表情、选眼环**：`kExprs[expr]` 给出眼环池、睁眼百分比、双眼偏移和持续动画。`pools[]` 存 1~4 个眼环下标，`pool_ms` 是停留时间；引擎用黄金比例哈希做伪随机轮换，眼睛会在几种眼型间自然变化，不是机械循环。

② **算 24 个眼轮廓点**：眼位 = `(160 + dxl + place.lx, 120 + dyl + place.ly)`，再叠加环坐标、动画偏移与注视偏移。`openness` 只缩放 y（眯眼），眨眼包络也乘在这里，最低 0.045，不会完全闭死。

③ **Chaikin 平滑**：24 点直接连多边形有折线感，每次角切把每条边换成 `0.75p+0.25q` 和 `0.25p+0.75q` 两个点，点数翻倍。默认 1 次（48 点），`set_eye_smoothing(2)` 为 96 点、`3` 为 192 点。

④ **圆度**：`set_eye_roundness(r)` 把轮廓朝最佳拟合椭圆混合 `r` 比例（PCA 求主轴、投影到椭圆）。`0` 是原版眼环，`0.45` 是固件默认，上限 0.6。

⑤ **球体变形**：球不是正圆，半径是角度函数，来自三个低阶模态。每个系数都是一个二阶弹簧 `x'' = ω²(target − x) − 2ζω·x'`，`target` 由表情查表，切表情/眨眼/点按都会给速度冲量，于是球会"duang"地回弹：

```cpp
    const float R = 80.0f * g_cs * (1.0f + 0.008f * sinf(g_phase * 1.6f));  // 呼吸
    const float jc2 = g_jelly.c2.x, js2 = g_jelly.s2.x;      // 弹簧状态
    const float jc3 = g_jelly.c3.x, js3 = g_jelly.s3.x;
    const float jc4 = g_jelly.c4.x;
    const float cy_j = g_cy + g_jelly.bounce * g_cs;
    static float bx[96], by[96];
    for (int i = 0; i < 96; i++) {
        float th = 6.2831853f * (float)i / 96.0f;
        float c = cosf(th), s = sinf(th);
        float c2 = 2.0f * c * c - 1.0f, s2 = 2.0f * s * c;
        float c3 = 4.0f * c * c * c - 3.0f * c;
        float s3 = 3.0f * s - 4.0f * s * s * s;
        float c4 = 8.0f * c * c * c * c - 8.0f * c * c + 1.0f;
        float rad = R * (1.0f + jc2 * c2 + js2 * s2 + jc3 * c3 + js3 * s3 + jc4 * c4);
        bx[i] = g_cx + rad * c;
        by[i] = cy_j + rad * s;
    }
    fill_poly_aa(fb, bx, by, 96, g_ball_color);
```

⑥ **眼睛跟着球一起压扁**：`sqx = 1 + jc2`、`sqy = 1 - jc2`，再加 `eye_dy = bounce * 0.8`。这一步是"脸不像贴在球上"的关键，自己做渲染器时最容易漏。

⑦ **抗锯齿填充**：每条扫描线取 2 条子扫描线求交点、排序、成对填充；中间整段用 32 位整块写（一次 2 像素），只有左右两端做分数覆盖率混合。这是性能拐点：之前的逐像素浮点循环占了大头。

⑧ **只清必要区域**：引擎只清球可能占到的包围盒（半径 `80×cs×1.12+8`），不是全屏；`render()` 只画不推，放大和 `writePixels` 由 app 做——这样渲染逻辑能脱离 M5GFX 在电脑上跑。

## 3. 五分钟：改出你自己的球

| 想改什么 | 调用 | 范围 | 生效 |
|---|---|---|---|
| 球色 / 眼色 | `set_agent_color(0..2)`、改颜色常量 | 0=白 1=蓝 2=红 | 下一帧 |
| 眼睛圆度 | `set_eye_roundness(r)` | 0 ~ 0.6（超出夹紧） | 下一帧 |
| 弹性 / 抖动 | `set_tuning(ω, ζ, amp, idle)` | ω>0.1、ζ>0、amp>0、idle≥0 | 下一帧 |
| 眼轮廓精度 | `set_eye_smoothing(n)` | 1/2/3 → 48/96/192 点 | 下一帧 |
| 表情 | `set_expression(i, now_ms)` | 0 ~ `expression_count()-1` | 约 30 ms 平滑过渡 |

所有 `set_*` 只改全局状态、不做绘制，下一次 `render()` 才生效。

### 3.1 换颜色

内置三色：白 `0xFFFF`、蓝 `0x04BF`、红 `0xF800`；眼睛固定纯黑 `0x0000`。先把 `#RRGGBB` 转成 RGB565：

```cpp
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
```

把它放在 `set_agent_color()` 之前，然后整段替换这个函数（其余代码不用动）：

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

眼睛颜色在文件顶部：`constexpr uint16_t kColorEye = 0x0000;`，换成 `rgb565(0x33,0x33,0x33)`（深灰 `0x3186`）会柔和很多——纯黑在 AMOLED 上对比度非常硬。

> `set_agent_color` 只接受 0/1/2，其它值夹回 0。这条 API 同时被按键（B 键循环换色）和蓝牙指令 `0x50` 使用，见 `firmware/main/voice/voice_task.cpp`。

### 3.2 眼睛圆度

```cpp
qiuqiu::set_eye_roundness(0.0f);   // 忠于原版眼环：偏长、眼角略尖
qiuqiu::set_eye_roundness(0.45f);  // 固件默认：明显更圆润
qiuqiu::set_eye_roundness(0.6f);   // 上限：接近椭圆，圆到有点"呆"
```

圆度只改变轮廓形状，不改变大小和位置；想更圆又不想丢细节，可配合 4.2 节自己生成小眼环。

### 3.3 弹性手感

```cpp
// set_tuning(omega, zeta, amp_scale, idle_scale)
//   omega      : 弹簧角频率 = 2π × 回弹频率(Hz)，越大弹得越快
//   zeta       : 阻尼比，越小晃得越久（0.08 抖好几下，0.35 几乎不晃）
//   amp_scale  : 表情形变幅度倍数（1.0 = 数据原值）
//   idle_scale : 常驻微抖动倍数（0 = 表面完全静止）

qiuqiu::set_tuning(13.194689f, 0.16f, 1.0f, 1.0f);  // 出厂：2.1 Hz，ζ=0.16
qiuqiu::set_tuning(10.053096f, 0.35f, 0.8f, 0.5f);  // 稳重：1.6 Hz，阻尼大
qiuqiu::set_tuning(16.336281f, 0.08f, 1.3f, 1.2f);  // 弹过头：2.6 Hz，晃三下
```

`amp_scale` 别开太大：幅度超过约 ±15% 后轮廓会顶出引擎默认清理区，偶尔留一条旧边缘残影。要大幅改形状，去改 `shape_for_expr()` 的目标值并顺手加大清理半径。

### 3.4 改表情：编辑 `qiuqiu_data.h`

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
    uint8_t openness;        // 睁眼百分比（0~100 整数）
    int8_t dxl, dyl, dxr, dyr;         // 左/右眼中心偏移（设计像素）
    AnimData anims[2];       // 最多两条持续动画
    uint8_t anim_count;      // 实际用几条（0~2）
};
```

| 枚举 | 取值 | 行为 |
|---|---|---|
| `Wave` | `NONE` 0 / `GLANCE` 1 / `SINE` 2 / `JITTER` 3 | 不动 / 张望并停留 / 平滑往复 / 每周期随机换位 |
| `Axis` | `X` 0 / `Y` 1 | 水平 / 垂直 |
| `Target` | `BOTH` 0 / `LEFT` 1 / `RIGHT` 2 | 双眼 / 只左眼 / 只右眼 |

完整例子：新增"眯眼、慢慢左右看、视线略向下"的 `CHILL`。

```cpp
// ① qiuqiu_data.h 顶部：表情数量 +1
constexpr int kExprCount = 16;          // 原来是 15

// ② 数组末尾追加一条（原有 15 条不动）
inline constexpr ExprData kExprs[kExprCount] = {
    /* …… */
    {"BORED", {4, 16, 0}, 3, 4750, 100, -3, 1, 4, -1, {{2, 0, 0, 2, 6800, 0}, {0, 0, 0, 0, 0, 0}}, 1},
    // 眯到 55%、眼睛下移 3 px、两个小眼环每 4.8 s 换一次、水平缓慢摆 ±2 px
    {"CHILL", {11, 15}, 2, 4800, 55, 0, 3, 0, 3, {{2, 0, 0, 2, 3200, 0}, {0, 0, 0, 0, 0, 0}}, 1},
};
```

- `pools` 是**眼环下标**，必须小于 `kRingCount`（18）；`11`、`15` 是比较小的眼环。
- `openness` 是百分比，会再乘眨眼包络；`55` 表示眼睛只有 55% 高，不是半闭眼皮。
- `anims` 的 `amp` 单位是设计像素；`period_ms` 决定快慢，`phase_ms` 让两只眼错开。
- 左/右眼**不能单独设睁眼程度**——数据格式里只有位置偏移分左右。要做单眼眨，得改渲染器。
- 改完直接编译，A/B 键循环表情即可看到；`expression_count()` 自动变成 16。

### 3.5 接回屏幕

`render()` 只画到内存，推屏是 app 的事（摘自 `app_qiuqiu.cpp`）：

```cpp
static constexpr int RW = 186, RH = 186;          // 引擎渲染分辨率
static constexpr int QW = 372, QH = 372;          // 推屏视口
static constexpr int VX = (466 - QW) / 2, VY = (466 - QH) / 2;

_fb[0] = (uint16_t*)heap_caps_malloc((size_t)RW * RH * 2, MALLOC_CAP_SPIRAM);
_fb[1] = (uint16_t*)heap_caps_malloc((size_t)RW * RH * 2, MALLOC_CAP_SPIRAM);
_up    = (uint16_t*)heap_caps_malloc((size_t)QW * QH * 2, MALLOC_CAP_SPIRAM);
qiuqiu::init(RW, RH, 168.0f * 0.5f);              // 半径随分辨率同步减半
qiuqiu::set_tuning(13.194689f, 0.16f, 1.0f, 1.0f);
qiuqiu::set_eye_roundness(0.45f);

uint32_t now = GetHAL().millis();
if (now - _last_render < frame_ms) return;        // 动画 50 ms / 静止 120 ms
_last_render = now;
int back = 1 - _fb_idx;
qiuqiu::render(_fb[back], now);

for (int y = 0; y < RH; y++) {                    // 最近邻 2× 放大
    const uint16_t* src = _fb[back] + (size_t)y * RW;
    uint16_t* d0 = _up + (size_t)(2 * y) * QW;
    uint16_t* d1 = d0 + QW;
    for (int x = 0; x < RW; x++) {
        uint16_t c = src[x];
        d0[2 * x] = d0[2 * x + 1] = c;
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

三个易踩的点：**半径与分辨率一起缩放**（168 是 466 屏上的目标尺寸，引擎只画 186，所以传 84）；**放大必须最近邻**（2×2 复制，双线性要读 4 个像素，在这块屏上不划算且边缘发灰）；**`writePixels` 用 `lgfx::rgb565_t*`**（字节序由 M5GFX 处理，自己拼 SPI 别忘）。

## 4. 进阶：完全自定义角色

| 路线 | 改什么 | 工作量 | 能做成什么样 |
|---|---|---|---|
| A. 改数据 | 颜色 / 圆度 / 表情 | 分钟级 | 同一个球的不同性格 |
| B. 换几何 | 自己生成 24 点眼环 | 半小时 | 猫眼、星星眼、方眼、闭眼弧 |
| C. 换渲染器 | 重写 `render()` | 半天 | 任意 2D 风格：贴图、粒子、像素画 |

### 4.1 方式一：自己生成 24 点环

眼环就是"围绕眼中心的一圈 24 个点"，引擎平滑后填充成黑色多边形。用 Python 生成最省事：

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
        if s < 0:                      # 屏幕坐标里 s<0 是上半部分
            y *= lid
        pts.append((x, y))
    return pts

def emit(pts, label):
    xs = [int(round(x)) for x, _ in pts]
    ys = [int(round(y)) for _, y in pts]
    assert all(-127 <= v <= 127 for v in xs + ys), "坐标越界"
    print("    {{{{{}}}, {{{}}}}},  // {}".format(
        ", ".join(map(str, xs)), ", ".join(map(str, ys)), label))


if __name__ == "__main__":
    emit(ellipse(13, 17), "椭圆 rx=13 ry=17")
    emit(cat_eye(15, 13), "猫眼 almond")
```

输出每行就是一个能直接粘进 `kRings` 的条目（已实测在 ±127 内）：

```text
    {{13, 13, 11, 9, 7, 3, 0, -3, -6, -9, ...}, {0, 4, 8, 12, 15, 16, 17, 16, ...}},  // 椭圆
    {{15, 14, 12, 9, 5, 2, 0, -2, -5, -9, ...}, {0, 2, 5, 8, 11, 12, 13, 12, 11, ...}},  // 猫眼
```

落地步骤：① 挑一个不常用的下标（如 `kRings[0]`）整行替换；② 想保留原环就追加，同时给 `kRingCount` +1 并**在 `kPlaces` 补一条眼位**（两数组必须等长）；③ 把表情的 `pools` 指到该下标；④ 用第 5 节的 host 预览确认后再刷机。

约束：**必须 24 点、按角度顺序首尾相连**（顺/逆时针都行，乱序会让填充出现自交怪块）；坐标是 `int8_t`，常规 ±20 以内，超过 30 容易与另一只眼重叠或被球裁掉；**左右眼共用同一组点、不做镜像**（猫眼会两只朝同一方向）；环只描述形状，**位置由 `kPlaces` 决定**（四元组是左眼 x/y、右眼 x/y 偏移）；`pools` 存的是环下标，不是眼位下标。

### 4.2 方式二：换整个渲染器

如果几何描述不了你的角色，就自己实现 `render()`。约定只有一条：**把 `uint16_t* fb` 当成 `w × h` 的 RGB565 画布，画成什么样都行**。

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

它复用了引擎里的 `g_w / g_h / g_cs / g_ball_color`，所以直接用 `init()` 传进来的尺寸和半径。做贴图角色就把 `rect()` 换成"从 RGB565 数组逐像素取样"；做粒子就换成"画 1~3 px 小点"，逻辑一样。

### 4.3 接口契约

| 约定 | 说明 |
|---|---|
| 帧缓冲 | `uint16_t*`，长度 `w*h`，**行优先、无 padding**，元素是原生 RGB565（不是字节流） |
| 绘制责任 | 自己清屏或覆盖要画的区域，不要假设缓冲里是上一帧 |
| 单帧时间 | 目标 20 fps（50 ms）；超过 40 ms 会让按键/触摸明显迟滞 |
| 调用频率 | 动画 50 ms、静止 120 ms 一拍，由 app 决定，不保证固定节拍 |
| 时间来源 | 只用传入的 `now_ms`（uint32 毫秒，可回绕）；不要读 `millis()`，host 预览没有 HAL |
| 内存 | 不在 `render()` 里 `malloc`；临时缓冲用 `static` 或 PSRAM 预分配 |
| 线程 | 单任务调用；引擎是全局单例，不支持多实例 |
| 依赖 | `render()` 里不碰 LVGL / M5GFX；推屏由 app 在返回后统一做 |
| 尺寸 | 任意 `w/h`，所有坐标按 `g_w/g_h` 自适应，别写死 466 |
| 混合 | 需要透明就自己算 RGB565 混合（参考引擎里的 `blend565()`） |

只想改 `render()` 时，其它符号原样保留即可。若整个文件换掉，这些符号必须实现，否则 app 链接失败：`init(w,h,radius)`、`render`、`set_expression`/`expression`/`expression_count`、`set_tuning`、`set_eye_roundness`、`set_agent_color`/`cycle_agent_color`（按键与蓝牙换色在用）；`agent_color_index`、`expression_name`、`set_gaze`、`request_blink`、`poke`、`set_eye_smoothing` 目前没被调用，但建议一并留空实现。注意两参 `init(w,h)` 只有声明没有实现，**请只用三参版本**。

### 4.4 接回固件

`firmware/main/CMakeLists.txt` 用 `file(GLOB_RECURSE ... "apps/*.cpp")` 收集源文件，同目录两个 `.cpp` 会一起编译并报重复定义——把旧文件改名成 `qiuqiu_engine.cpp.bak`（glob 不认 `.bak`），或直接覆盖。先在电脑上按第 5 节验证形状和耗时，再 `idf.py build`；上机确认三件事：表情能切（A/B 键、触摸）、推屏不闪、熄屏/唤醒正常。新渲染器更重的话，先把 `frame_ms` 调大再逐步逼近。

## 5. 在电脑上预览（不刷机）

`qiuqiu_engine.cpp` 只 include `<math.h>` 和 `<string.h>`，不依赖 ESP-IDF、M5GFX、LVGL，所以能在 PC 上原样编译，几秒钟出图。

### 5.1 一个零依赖的 PNG 写出（`png_io.h`）

```cpp
// png_io.h — 最小 PNG 写出（RGB888），只依赖 zlib
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <zlib.h>

namespace pngio {

inline void be32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

inline void chunk(std::vector<uint8_t>& o, const char* t, const uint8_t* d, size_t n)
{
    be32(o, (uint32_t)n);
    size_t at = o.size();
    o.insert(o.end(), t, t + 4);
    if (n) o.insert(o.end(), d, d + n);
    be32(o, (uint32_t)crc32(0, o.data() + at, (uInt)(n + 4)));
}

inline bool save_png(const char* path, const uint16_t* fb, int w, int h)
{
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) {
        raw.push_back(0);                                  // filter = None
        for (int x = 0; x < w; x++) {
            uint16_t c = fb[(size_t)y * w + x];
            uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            raw.push_back((uint8_t)(r * 255 / 31));
            raw.push_back((uint8_t)(g * 255 / 63));
            raw.push_back((uint8_t)(b * 255 / 31));
        }
    }
    uLongf n = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(n);
    if (compress2(z.data(), &n, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    z.resize(n);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13] = {0};
    for (int i = 0; i < 4; i++) {
        ihdr[i]     = (uint8_t)((uint32_t)w >> (24 - 8 * i));
        ihdr[4 + i] = (uint8_t)((uint32_t)h >> (24 - 8 * i));
    }
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 2;   // color type: RGB
    chunk(out, "IHDR", ihdr, 13);
    chunk(out, "IDAT", z.data(), z.size());
    chunk(out, "IEND", nullptr, 0);

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

}  // namespace pngio
```

### 5.2 主程序（`host_preview.cpp`）

```cpp
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "png_io.h"
#include "qiuqiu_engine.h"

int main(int argc, char** argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s OUT_DIR W H RADIUS EXPR FRAMES\n"
                        "  e.g. %s out 466 466 168 1 60\n", argv[0], argv[0]);
        return 1;
    }
    const int w = atoi(argv[2]), h = atoi(argv[3]);
    const int expr = atoi(argv[5]), frames = atoi(argv[6]);

    std::vector<uint16_t> fb((size_t)w * h, 0);
    qiuqiu::init(w, h, (float)atof(argv[4]));
    qiuqiu::set_tuning(13.194689f, 0.16f, 1.0f, 1.0f);   // 与固件一致
    qiuqiu::set_eye_roundness(0.45f);

    uint32_t now = 1000;                 // 起始时刻随意，引擎只关心差值
    qiuqiu::set_expression(expr, now);   // 切表情会触发一次果冻冲量
    char path[256];
    for (int i = 0; i < frames; i++) {
        now += 50;                       // 固定 20 fps
        if (i == 8) qiuqiu::request_blink(now);
        qiuqiu::render(fb.data(), now);
        snprintf(path, sizeof(path), "%s/frame_%04d.png", argv[1], i);
        if (!pngio::save_png(path, fb.data(), w, h)) return 1;
    }
    printf("wrote %d frames (%dx%d, expr=%d)\n", frames, w, h, expr);
    return 0;
}
```

### 5.3 编译与运行

```bash
REPO=$(pwd)                                   # 在仓库根目录执行
mkdir -p /tmp/qiuqiu-preview && cd /tmp/qiuqiu-preview
cp "$REPO"/firmware/main/apps/app_qiuqiu/qiuqiu_engine.{h,cpp} \
   "$REPO"/firmware/main/apps/app_qiuqiu/qiuqiu_data.h .
# 把 png_io.h 和 host_preview.cpp 放进当前目录，然后：
g++ -std=c++17 -O2 host_preview.cpp qiuqiu_engine.cpp -lz -o host_preview
mkdir -p out && ./host_preview out 466 466 168 1 60
```

### 5.4 合成视频 / 表情九宫格

```bash
ffmpeg -framerate 20 -i out/frame_%04d.png \
  -vf "scale=320:-1:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
  -y qiuqiu.gif                                   # 动图
ffmpeg -framerate 20 -i out/frame_%04d.png -pix_fmt yuv420p -y qiuqiu.mp4

mkdir -p strip && for e in $(seq 0 14); do
  mkdir -p strip/$e && ./host_preview strip/$e 240 240 88 $e 20
done
magick montage strip/*/frame_0019.png -tile 5x3 -geometry 200x200+2+2 exprs.png
```

坑：半径要和尺寸匹配（`466 466 168` 是 466 屏上的球，`240 240 88` 是等比缩小）；固定 `+50` 表示 20 fps，改成 `+16` 只是采样更密，不代表固件能跑 60 fps；host 上的毫秒数不能当设备帧率。另外，强冲量 + 大半径偶尔会在球顶留一条细弧（清理区边界），预览时用 `request_blink()` 而不是 `poke()` 就看不到。

## 6. 性能预算

**真机实测**（466×466 AMOLED、ESP32-S3 @240 MHz、PSRAM 80 MHz，数字来自串口日志的 `render=…ms push=…ms`）：

| 配置 | 引擎渲染 | 推屏 | 合计 | 帧率 |
|---|---|---|---|---|
| 全屏 466×466 渲染 + 全屏推屏 | 72 ms | 36 ms | 108 ms | ≈9 fps |
| 186×186 渲染 + 2× 放大 + 372×372 视口（当前固件） | 25 ms（含放大） | 23 ms | 48 ms | ≈19–20 fps |

优化前全屏渲染是 110 ms；`fill_poly_aa()` 内层改整块写后降到 72 ms，再叠加"半分辨率 + 窗口推屏"才到 25 ms。

**帧预算**：20 fps → 50 ms/帧（当前用掉 48 ms，几乎没有余量）；8 fps → 125 ms（静止用 120 ms 一拍）。建议动画 20 fps、静止 8 fps——球动时人眼敏感，静止时降帧看不出来还省电。加新效果前按"渲染 ≤ 25 ms、推屏 ≤ 23 ms"分配；任一项翻倍都会掉到 12 fps 以下。只改颜色/表情时性能基本不变，瓶颈在填充像素数和推屏字节数。

**内存**：

| 缓冲 | 大小 | 位置 |
|---|---|---|
| 渲染双缓冲 `2 × 186×186×2` | 138,384 B（135 KiB） | PSRAM |
| 推屏缓冲 `372×372×2` | 276,768 B（270 KiB） | PSRAM |
| **常驻合计** | **415,152 B（405 KiB）** | PSRAM |
| 一次性清屏缓冲 `466×466×2`（用完释放） | 434,312 B（424 KiB） | PSRAM |
| 对比：全屏双缓冲 `2 × 466×466×2` | 868,624 B（848 KiB） | PSRAM |
| 引擎静态暂存（平滑/扫描线/轮廓） | 14,636 B | 内部 RAM `.bss` |
| 形象数据（18 环 + 18 眼位 + 15 表情 + 名字） | 1,584 B | flash `.rodata` |

**推屏带宽是硬上限**：372×372×2 = 276,768 B，实测 23–24 ms，即 ≈11.5 MB/s；加大 `writePixels` 分块没有改善，说明瓶颈是 SPI 带宽而非调用次数。全屏 466² 推屏 434,312 B ≈ 36 ms，与实测一致；想再快只能减像素（缩视口、脏矩形）或换更快的总线配置，继续调 chunk 没用。

**优化清单**（按性价比）：① 降渲染分辨率 + 最近邻放大；② 只推球所在视口；③ 填充内层整块写；④ 减球段数/子采样行数（当前 96 段、2 条子扫描线）；⑤ 静止降帧；⑥ 别每帧全屏 memset。host 上量算法成本（x86-64、`-O2`、单线程）：466²、r=168 约 0.11 ms/帧，186²、r=84 约 0.06 ms/帧——只用来比较算法改动前后，别外推到设备。

## 7. 许可提示

仓库自带的形象数据（`qiuqiu_data.h` 的眼环、眼位、表情参数与白球配色）来自 **Emotion Ball 社区许可**（见 `licenses/LICENSE-emotion-ball.txt`、`licenses/NOTICE-emotion-ball.md`）：

- 球形角色的**视觉形象**仅限个人学习、研究与非商业分享，**禁止任何商业用途，且永不提供商业授权**；
- 引擎代码与表情配置数据本身"非商业免费 + 可另行取得商业授权"，商业使用时必须搭配你自有或已获授权的角色形象；
- 分发本项目时请原样保留 `licenses/` 下的许可与 NOTICE。

| 用途 | 要做什么 |
|---|---|
| 自己玩、学习、非商业分享 | 直接用，注明出处 |
| 商用 / 上架 / 接商单 | **自己设计角色**：换掉 18 个眼环、18 个眼位、15 条表情和配色，必要时按 4.2 节换成自己的渲染器 |
| 二次分发 | 保留 `licenses/`，保留 `qiuqiu_data.h` / `qiuqiu_engine.h` 顶部的来源注释 |

两点补充：`qiuqiu_data.h` 头部注明它由设计工具生成，手改后不要再跑生成脚本（会覆盖）；引擎文件头注明绘制思路参考了一个 AGPL 社区项目，是否构成衍生作品、能否商用请以该上游许可为准，本仓库不做法律判断。

## 附录 A：API 速查

| 函数 | 作用 | 备注 |
|---|---|---|
| `init(w, h, radius_px)` | 设定画布与球半径 | 用三参版本 |
| `set_expression(i, now_ms)` | 切表情 | 同值直接返回，不触发冲量 |
| `expression()` / `count()` / `expression_name(i)` | 当前 / 总数 / 名字 | — |
| `set_agent_color(0..2)` | 白 / 蓝 / 红 | 其它值夹到 0 |
| `cycle_agent_color()` / `agent_color_index()` | 循环 / 当前下标 | B 键用 |
| `set_gaze(h, v)` | 注视偏移（-1~1） | 设计坐标 ±16.8 / ±10.5 px |
| `request_blink(now_ms)` / `poke(now_ms)` | 立刻眨眼 / 果冻冲量 | 自动眨眼另有一套（3.4~6.2 s） |
| `set_tuning` / `set_eye_roundness` / `set_eye_smoothing` | 手感 / 圆度 / 平滑精度 | 见 3.2–3.3 |
| `render(fb, now_ms)` | 画一帧 | 不推屏、不分配内存 |

## 附录 B：排错

| 现象 | 原因 | 处理 |
|---|---|---|
| 球很小 / 超出屏幕 | 半径没跟着分辨率缩放 | 按 466:186 同比减半 |
| 眼睛全是折线 | 平滑迭代被设成 1 且半径很大 | `set_eye_smoothing(2)` 或 3 |
| 切换表情时"炸开" | `amp_scale` 太大 | 降到 1.0 附近，或改 `shape_for_expr()` |
| host 预览全黑 | 没切表情且时间从 0 开始 | 起始 `now_ms` 给 1000，先 `set_expression()` |

---
**相关**：`docs/05-pitfalls.md`（真机踩坑记录）、`licenses/`（许可原文）。
