# Source Han Sans SC（思源黑体）字体源

固件 UI 使用 Source Han Sans SC。源码包含生成后的字库和字体许可，重生成时请从下列地址获取 OTF 文件。

- 字体：Source Han Sans SC Regular / Medium / Bold（OTF）
- 版权：Copyright 2014-2025 Adobe, with Reserved Font Name 'Source'
- 许可证：SIL Open Font License 1.1，见 `LICENSE.txt`
- 来源：
  - https://github.com/adobe-fonts/source-han-sans/raw/release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf
  - https://github.com/adobe-fonts/source-han-sans/raw/release/OTF/SimplifiedChinese/SourceHanSansSC-Medium.otf
  - https://github.com/adobe-fonts/source-han-sans/raw/release/OTF/SimplifiedChinese/SourceHanSansSC-Bold.otf
- 下载日期：2026-11-03

## 重新生成字库

```powershell
# 从仓库根目录运行。需要 Python、Node.js 和 npx。
$env:PROTORIG_FONT_DIR='<包含上述三个 OTF 文件的目录>'
python -X utf8 firmware/lvgl_test/tools/gen_lvgl_fonts_open.py   # 生成 fonts_gen/lv_font_*.c
python -X utf8 firmware/lvgl_test/tools/font_coverage.py         # 检查 UI 中文字符覆盖，缺失应为 0
```

生成参数：`lv_font_conv@1.5.3`，`--bpp 4`，`--no-compress`，
UI 字体（ui14/ui16/ui20/ui28/monitor20）覆盖源码中的界面中文，ui25 用于 BLE 卡片的固定文案，ui48/ascii28/num104 使用数字或 ASCII 子集。

字符集来源：`eez_ui/src`、`apps/`、`ui/`、`main.c` 的字符串常量，
加上现有字体 symbols、ASCII 0x20-0x7F 和设计标点。
