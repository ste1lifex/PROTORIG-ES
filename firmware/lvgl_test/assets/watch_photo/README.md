# 默认表盘下半区图

默认图占据屏幕下半区 480×232（从 y=248 开始），内容为 DSH 鲸鱼、「探索未至之境」与终末地标志。

| 文件 | 说明 |
|---|---|
| [source.png](source.png) | 用于生成固件图片的原图 |
| [config.json](config.json) | 裁剪位置、边缘和顶部的渐变、底部压暗及像素校验值 |
| [preview.png](preview.png) | 应用裁剪与渐变后的预览 |
| [watch_photo.c](../../watch_photo.c) | 编入固件的 RGB565 像素数据 |

## 生成或替换内置图

在仓库根目录运行，需要 Python 和 Pillow。第一条命令从当前原图重建预览和 C 数组。换图时使用下面的 `--input` 示例。

```powershell
python firmware/lvgl_test/tools/make_watch_photo.py
python firmware/lvgl_test/tools/make_watch_photo.py --input my-image.png
```

改图后，将新 `preview.png` 同步到插件的 `assets/faces/builtin-panel.png`，并按[表盘渲染测试](../../docs/TESTING.md)更新整表盘预览。
修改顶部或圆边渐变时，用插件目录的 `tools/make-panel-mask.py` 重新生成 `assets/faces/panel-mask.png`（需要 Pillow 和 NumPy）。
