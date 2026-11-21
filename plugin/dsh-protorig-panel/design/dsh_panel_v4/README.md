# 电脑模式面板设计参数

[`tokens.json`](tokens.json) 是插件随包读取的面板布局和样式数据。修改默认面板时请编辑本文件夹中的 `tokens.json`，并从插件目录运行 `npm test`。

通过插件的 `design` 设置指定自定义目录。该设置为空时优先使用包内参数，包内缺少 `tokens.json` 时才检查 `PROTORIG_DESIGN_DIR`。
