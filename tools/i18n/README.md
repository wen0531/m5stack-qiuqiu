# 多语言（i18n）

固件支持 10 种语言，**不装完整字库**：每种语言只生成"文案里实际出现的字形"子集字体，
一门外语通常只占几十 KB。

## 现有语言

| 代码 | 语言 | 字形来源 |
|---|---|---|
| `en` | English | 内置 Montserrat |
| `zh-Hans` | 简体中文 | Noto Sans CJK SC |
| `zh-Hant` | 繁體中文 | Noto Sans CJK TC |
| `ja` | 日本語 | Noto Sans CJK JP |
| `ko` | 한국어 | Noto Sans CJK KR |
| `fr` | Français | DejaVu Sans |
| `de` | Deutsch | DejaVu Sans |
| `es` | Español | DejaVu Sans |
| `ru` | Русский | DejaVu Sans（西里尔） |
| `la` | Latina | DejaVu Sans |

## 添加一种新语言

1. 编辑 `strings.json`：
   - 在 `langs` 数组加语言代码，例如 `"it"`
   - 在 `lang_names` 加显示名
   - 在**每一条** `strings` 里加该语言的翻译
2. 编辑 `build_fonts.py` 的 `FONTS`，给新语言指定字体文件
3. 重新生成字体：
   ```bash
   python3 tools/i18n/build_fonts.py
   ```
4. 在 `main/i18n/i18n.cpp` 的 `kStrings` 表里加对应列（与 JSON 保持同步）
5. 编译：
   ```bash
   idf.py build
   ```

## 字体依赖

- 中日韩：`Noto Sans CJK`（Debian/Ubuntu: `fonts-noto-cjk`）
  该字体是 TTC 集合，`build_fonts.py` 会用 `fontTools` 自动拆出 SC/TC/JP/KR 四个面
  ```bash
  pip install fonttools
  ```
- 拉丁/西里尔：`DejaVu Sans`（`fonts-dejavu-core`）

## 设计要点

- **只打包用到的字形**：`--symbols` 取自全部译文，因此改文案后必须重新生成字体，
  否则新出现的字会显示成方框
- **语言选择存 NVS**：`namespace "system"`、key `lang`，断电保留
- **切换语言立即生效**：设置页点 "Language: xxx" 循环切换，菜单会用新语言重建
- **表外字符**：`tr()` 找不到 key 时返回 key 本身；字体缺字形时 LVGL 显示方框，
  所以新增文案后**务必重新跑 build_fonts.py**

## 已知限制

- 阿拉伯语/希伯来语需要 RTL 排版，未支持
- 泰语/印地语需要复杂文字整形，LVGL 不支持
- 文案长度差异（德语/俄语偏长）可能让按钮文字换行，需要实机确认
