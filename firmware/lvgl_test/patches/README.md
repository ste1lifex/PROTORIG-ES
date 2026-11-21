# BLE 补丁参考

`W1_ble_integration.patch` 是随源码保留的参考文件。当前固件已在
[`CMakeLists.txt`](../CMakeLists.txt) 中编入 `ble_link.c`、`ble_svc.c` 和 `services/svc_ble.c`。

构建当前固件请按[固件指南](../../README.md)使用工程脚本。构建期间需要的 SDK 补丁见 [sdk_patches/](../sdk_patches/)。
