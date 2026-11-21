# 打包与发布

## 源码归档

从仓库根目录运行。归档内容来自指定的 Git 提交，目录必须尚未存在。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/export-public.ps1 -Ref HEAD -Out ../PROTORIG-ES-source
```

归档包括固件、插件源码、文档、STEP/STL/3MF 模型和原理图 PNG。排除规则见 [`.gitattributes`](.gitattributes)。

## DSH 插件包

在仓库根目录执行以下命令检查包内容和依赖，再生成 tarball。

```powershell
cd plugin/dsh-protorig-panel
npm ci
npm test
npm run check:release
npm pack --dry-run
npm pack
```

生成的 `dsh-protorig-panel-0.4.19.tgz` 可按[插件安装说明](plugin/dsh-protorig-panel/README.md#装进-dsh-profile--插件管理器)装入 DSH。插件版本以 [`package.json`](plugin/dsh-protorig-panel/package.json) 为准。
