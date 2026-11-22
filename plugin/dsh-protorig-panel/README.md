# dsh-protorig-panel

dsh-protorig-panel 将 PROTORIG ES 用作 480×480 副屏和遥控器。USB 由电脑渲染画面并接收手表触摸，BLE 由手表显示电脑指标和交互卡片。本仓库中的插件版本为 **0.4.21**。

设置页支持设备绑定、显示参数、审批与问答、图片表盘安装、默认表盘下半区换图，以及 Windows/macOS 烧录。

## 平台支持

| 能力 | Windows | macOS | 说明 |
|---|---|---|---|
| USB 连接（像素面板/触摸/审批/问答） | ✅ | ✅ | 标准 CDC：Windows `COMx`（VID 349B / MI_00），macOS `/dev/cu.usbmodem*` |
| 设备身份（`admin_id` 绑定） | ✅ | ✅ | USB 用 shell `adminid`；BLE 用广播名后缀。两边同一个 6 位编号 |
| BLE 数字通道（USB 不在时） | ✅ | ✅ | 纯 npm：`@stoprocent/noble`（macOS CoreBluetooth / Windows WinRT 原生绑定，**不需要 Python**）；macOS 首次要给 DSH 蓝牙授权 |
| 烧录 | ✅ | ✅ | Windows：`flash.ps1` + `BLFlashCommand.exe`；macOS：调用 SDK 的 `BLFlashCommand-macos`，需要本机已编译固件，Apple Silicon 需 Rosetta 2 |
| 表盘切换 / 图片安装 | ✅ | ✅ | 走 USB shell（`face` / `faceimg`）；**只支持 0 默认 / 3 图片**；图片 480×480×256 色，上传约 45 s；切表盘会退出电脑模式回手表模式 |
| 默认表盘下半区 | ✅ | ✅ | 走 USB shell（`facepanel begin/w/end`、`facepanel clear`）；480×232×256 色，约 110 KB，上传约 20 s |

### 对接的固件

固件页面（`firmware/lvgl_test/eez_ui_glue.c`，`PAGE_N=8`）：0 默认表盘 / 1 协议网络（占位，
App 列表 `hidden=1` 不显示）/ 2 相遇记录（占位，隐藏）/ 3 终端设置 / 4 USB 监控（= 电脑模式，
表盘右滑进入）/ 5 秒表 / 6 计时器 / 7 闹钟。表盘只有 **face 0 默认** 与 **face 3 图片**。

> ⚠️ **media 分区烧录不会擦**：换固件后自定义图片表盘 / 下半区面板仍然在
> （face 3 还是你的图，face 0 下半区还盖着内置默认图）。要回内置默认图：shell `facepanel clear`，
> 或设置页「恢复内置」。

### 连接开关 / 探测注册 / 分通道显示

- **USB 连接**、**蓝牙探测连接**两个开关：关掉对应链路后不会自动打开串口/扫描广播；
- **USB 探测注册**、**蓝牙探测注册**两个按钮：只扫描并登记编号到「已登记设备」，不改变当前连接；
- 显示参数分两套：`USB` 与 `蓝牙` 各自的**亮度 / 常亮 / 息屏时间**（息屏时间走 PC 帧 `0x16`；
  下拉里的「跟随手表」= 不覆盖手表自己的息屏设置，固件默认 15 s，可在终端设置里改；
  只作用于电脑模式，不改手表模式的设置）；
- USB 电脑模式透传时**本地滑动/点击被固件忽略**（触摸仍转发给电脑），只能拔线或关 DSH 退出；
  BLE 模式仍可随时左滑退出、再滑入重新握手。

### 设备选择（USB / BLE 都只连指定的那一块）

- 插件会**自动登记**见过的表：USB 握手问出 `admin_id`、扫描候选口探活、BLE 广播名
  `PROTORIG ES <ADMIN_ID>` 解析 —— 都算"见过一次"，记到 `<DATA_DIR>/devices.json`；
- 设置页「设备」列出**已登记设备**（含 USB/BLE 来源、上次出现时间）；点「绑定」后，
  **USB 和 BLE 都只连接这一块表**，其它表只登记、不连接、不下发设置；
- 绑定留空时自动选择第一个可用设备。

同一块手表在 Windows 和 macOS 上显示**同一个编号**（例如 `2BEA4E`）——编号是手表自己从
eFuse chip ID 算出。实现见固件 `services/svc_device_id.c`。

## 安装

### 从 npm 安装（公网）

插件已发布到公网 npm（当前 0.4.21）：

```bash
npm i -g dsh-protorig-panel@0.4.21
# 或装进 DSH profile
dsh plugin --profile <name> add dsh-protorig-panel@0.4.21 --allow-build=@serialport/bindings-cpp
```

### macOS 上用 DSH 自带的 pnpm

装有 DeepSeek Harness 桌面版时，可用 `tools/pnpm-dsh.sh` 调用应用内的 Node 和 pnpm。插件需要 Node.js 22 或更高版本。

```bash
cd plugin/dsh-protorig-panel
./tools/pnpm-dsh.sh install --frozen-lockfile   # 首次 / 锁文件变更后
./tools/pnpm-dsh.sh test                        # 运行插件测试
./tools/pnpm-dsh.sh pack                        # → dsh-protorig-panel-0.4.21.tgz
```

- 构建脚本白名单在 `pnpm-workspace.yaml` 的 `allowBuilds`（pnpm 11 起不再读
  `package.json` 的 `pnpm` 字段）：只放行 `@serialport/bindings-cpp` 的
  `node-gyp-build`（它只挑预编译二进制，不编译源码）。
- 找不到 DSH 时脚本退回系统 `pnpm`；也可用 `DSH_APP=` / `DSH_NODE=` / `DSH_PNPM=` 覆盖。
- Windows 开发机同理：用 DSH 便携包里的 node 跑 pnpm，或装独立 Node/npm 后走下面的 npm 路径。

### 从源码 / tarball 安装

从仓库根目录运行以下命令，生成供 DSH 安装的 tarball。需要 Node.js 22 或更高版本。

```bash
cd plugin/dsh-protorig-panel
npm ci                            # 或 npm install
npm test
npm pack                          # → dsh-protorig-panel-0.4.21.tgz
```

### 装进 DSH profile / 插件管理器

> **安装时会出现一次"构建脚本批准"**：pnpm 11 默认拦截依赖的 install 脚本，
> 首次装会提示 `Ignored build scripts: @serialport/bindings-cpp`。
> DSH 插件管理器会把它作为 `pendingBuilds` 交给用户批准，批准后写入 profile 的
> `pnpm-workspace.yaml` 的 `allowBuilds` 并自动重试安装 —— 这是**预期的一次性动作**。
> 该脚本只跑 `node-gyp-build` 选平台预编译包，**不编译源码、不需要编译器**。
>
> 非交互安装（CLI）时可以直接放行，等价于批准：
>
> ```bash
> dsh plugin --profile <name> add ./dsh-protorig-panel-0.4.21.tgz \
>   --allow-build=@serialport/bindings-cpp
> ```

### 装进 DSH profile（Windows 便携版示例）

```bash
cd <dsh-portable-dir>                     # DSH 便携版目录
npm_config_store_dir="<dsh-portable-dir>/store" \
./node/bin/node.exe app-npm/node_modules/@deepseek-ai/dsh/lib/bin.js \
  plugin --profile web add "<repo>/plugin/dsh-protorig-panel"

# 装完重启 DSH 宿主（插件在宿主进程内加载）
```

也可以用 DSH 的插件管理器装 tarball 路径（`plugin add ./dsh-protorig-panel-0.4.21.tgz`）。
插件靠 `package.json` 的 `dsh.bundle.patch → cordis.patch.yml` 被加载；
`plugin add` 会把 bundle 追加到 profile 的 `dsh.profile.bundles`。

### 依赖与编译（发行版：只有 npm 依赖 + 包内资源）

面板与连接功能使用 npm 依赖和包内资源。烧录还需 SDK 工具与固件构建产物，见下方[烧录说明](#烧录)。
`npm run check:release` 检查发行依赖。

| 依赖 | 形态 | 说明 |
|---|---|---|
| `serialport` 13 | 预编译：`@serialport/bindings-cpp`（win32-x64 / darwin-arm64+x64 …） | USB 串口；安装脚本只 `node-gyp-build` 选预编译，不编译 |
| `@napi-rs/canvas` 1.1 | 预编译 optional 包 | 面板 Skia 渲染 |
| `@resvg/resvg-js` 2.6 | 预编译 optional 包 | 设计稿 SVG → 像素 |
| `systeminformation` 5.x | **纯 JS**（调系统自带命令） | CPU/GPU/RAM/网络指标（包内 `src/metrics/`） |
| `@stoprocent/noble` 2.8 | 预编译：`prebuilds/darwin-x64+arm64`、`win32-x64` | BLE 数字通道（macOS CoreBluetooth / Windows WinRT）；**不需要 Python** |
| `assets/fonts/*.ttf` | 包内资源（2.4 MB × 3） | Noto Sans SC 子集（OFL），**macOS/Windows 同一字体**，见下 |

> **一次性"构建脚本批准"**：pnpm 11 默认拦截依赖的 install 脚本，首次安装会提示
> `@serialport/bindings-cpp` / `@stoprocent/noble` 需要批准。它们只做预编译选择
> （`node-gyp-build`），不编译源码、不需要编译器。DSH 插件管理器会列成 `pendingBuilds`
> 让用户确认一次；CLI 可用 `--allow-build=...`（见安装章节）。

**字体**：面板使用包内的 Noto Sans SC 子集 `PROTORIG Panel Sans`（OFL，400/500/700），
由 `tools/build_panel_fonts.py` 生成。可通过 `PROTORIG_PANEL_FONT_FILE` 和 `PROTORIG_PANEL_FONT_FAMILY` 配置自定义字体。

**BLE 说明**：`@stoprocent/noble` 直接调系统蓝牙栈；macOS 首次要在
系统设置 → 隐私与安全性 → 蓝牙 给 DSH 授权（USB 不需要）。蓝牙不可用时插件只警告一次，
**USB 功能完全不受影响**。

> 发行版依赖规则见 `tools/check-release.mjs`；Windows 侧验证见
> [测试与验收](../../firmware/lvgl_test/docs/TESTING.md)。

**面板设计参数**：插件用包内的 `design/dsh_panel_v4/tokens.json` 渲染面板。查找顺序如下。

```text
配置 design（绝对路径） > 包内 design/dsh_panel_v4 > PROTORIG_DESIGN_DIR
```

从源码改设计稿时请改 `design/dsh_panel_v4/`，再跑测试。

## 模块

| 文件 | 作用 |
|---|---|
| `src/index.mjs` | DSH 插件入口：`name` / `Config` / `apply(ctx, config)`，接 session、审批、问答 |
| `src/panel-service.mjs` | 链路 + 渲染 + 脏块刷新 + 触摸 + 提醒 + 设备身份/换表保护 + 审批/问答卡状态机 |
| `src/platform.mjs` | 平台层：串口过滤/排序（Windows `MI_00`、macOS `cu.usbmodem`）、烧录能力 |
| `src/device-id.mjs` | `admin_id` 解析（`adminid` 输出 / 广播名后缀）、候选选择、设备标签 |
| `src/link.mjs` | 串口链路：发现、开关、`queryAdminId`、逐口探活 `probePortAdminId`、文本行解析 |
| `src/focus.mjs` | 会话投影（复用 DSH 桌面样例，MIT，见 `LICENSE-focus-mit.txt`） |
| `src/approval.mjs` / `src/questions.mjs` | `approval/request`、`user-questions/request` waterfall（手表优先） |
| `src/ble-bridge.mjs` | USB 不在时用 `@stoprocent/noble`（npm 原生绑定）推 `0x52` 数字通道 + 卡片层；无子进程 |
| `src/fonts.mjs` | 面板字体解析：包内 Noto Sans SC 子集（OFL，mac/win 同一字体）+ 字重别名 + 系统回退 |
| `src/metrics/` | 指标采集（CPU/GPU/RAM/网络），依赖纯 JS 的 `systeminformation`；包内代码，无本地路径依赖 |
| `src/config.mjs` | Config schema（`.volatile()` 字段进 DSH 设置表单）+ 默认值 |
| `src/protocol.mjs` / `src/framebuffer.mjs` | 串口帧 / 脏块（已真机验证） |
| `src/render-monitor-canvas.mjs` | v4 面板 Skia 渲染（~2 ms/帧） |
| `src/log-file.mjs` | 可选落盘日志（`logFile`，开发副本用） |
| `src/client.js` | 设置页（浏览器半边）：概览/显示/提醒与审批/表盘/连接/设备/高级 七个分组；开关统一为拨动开关 |
| `design/dsh_panel_v4/` | 随包分发的面板布局和样式参数 |
| `src/flash.mjs` | 烧录（**Windows 后端**）：预检 sha256 + 调 `flash.ps1`。使用 `firmware/lvgl_test/flash.ps1`，可用 `PROTORIG_FLASH_SCRIPT` 指定脚本路径 |
| `src/flash-mac.mjs` | 烧录（**macOS 后端**）：serialport 发 `reboot uart` → 等 ROM 口 → `BLFlashCommand-macos --config` |
| `src/face-pack.mjs` | 图片表盘打包：PNG → cover 480×480 → 256 色（中位切分 + Floyd–Steinberg）→ PFI（256 B header + 1024 B palette + 230,400 B 索引，双 zlib CRC32）；另出 480×480 预览 PNG。**同时提供通用 I8 编码原语**（`decodeToCanvasSized` / `packI8Blob`）给下半区面板复用 |
| `src/face-panel.mjs` | 默认表盘下半区：cover 480×232 → 套内置图的羽化 mask（含上边缘）→ 256 色 → I8 112,384 B；`facepanel begin/w/end/clear` 命令序列、状态行解析、契约校验 |
| `src/face-ids.mjs` | 固件表盘 id（0 默认 / 3 图片）：服务、工具和设置页共用 |
| `assets/faces/default.png` | 插件设置页「默认表盘」预览（480×480）：**真机同源代码渲染**，由 `firmware/lvgl_test/tools/face_preview_host.py` 编译 `face_lvgl.c` + `watch_photo.c` + 字库，在 PC 上跑一帧导出 |
| `assets/faces/panel-mask.png` | 下半区面板的羽化 alpha（480×232，白底 + A 通道）：由 `tools/make-panel-mask.py` 从内置 `firmware/lvgl_test/assets/watch_photo/preview.png` 反解生成（几何量，与画面内容无关）；JS 运行时只加载它 |
| `src/face-cache.mjs` | 本地缩略图缓存：图片表盘 `<DATA_DIR>/faces/<CRC>.png`、下半区面板 `panel-<CRC>.png`（`PROTORIG_FACE_CACHE` 可覆盖目录），读失败/坏 CRC 静默当没有 |
| `src/tools.mjs` | agent 工具：`protorig_state` / `protorig_face` / `protorig_panel` / `protorig_conn` / `protorig_flash` / `protorig_notify` / `protorig_vibrate` / `protorig_screenshot`（低 token 渲染） |
| `src/routes.mjs` | `/api/dsh-protorig-panel/*` 本机 HTTP 路由（设置页用） |
| `tools/cli.mjs` | 不上 DSH 的联调命令：hello/sync/bands/panel/touch/testcard/bright/vibrate/flash |
| `tools/make-panel-mask.py` | 开发期工具（Pillow）：从内置下半区图（`firmware/lvgl_test/assets/watch_photo/preview.png`）反解羽化 alpha → `assets/faces/panel-mask.png`；只有改 `top_fade`/`edge_fade` 等渐变量时才重跑 |

## 配置

`cordis.patch.yml` 的 `config` 经 `Config` schema 校验；用户可调的字段标了
`.volatile()`，DSH 的插件设置表单只渲染 volatile 字段。

| 项 | 默认 | 说明 |
|---|---|---|
| `port` | `''` | 空 = 自动发现（Windows VID 349B / MI_00；macOS `/dev/cu.usbmodem*`）。一般不用填 |
| `deviceId` | `''` | **设备绑定**：6 位大写 `admin_id`（如 `2BEA4E`）。空 = 不绑定，自动选第一个 |
| `fps` | `2` | 面板刷新率（1/2/5）。整屏 1.7 fps，只有局部刷新才撑得起 5 |
| `autoEnter` | `true` | 插线后自动 `pcmode 1` 进电脑模式 |
| `reconnectDelayMs` | `1500` | 断线重连间隔 |
| `design` | `''` | 空 = 使用包内 `design/dsh_panel_v4`。自定义面板时填写目录的绝对路径 |
| `reminder.done/needs/failed` | `true` | 三类事件是否提醒（旧版 `both/vibrate/bright/off` 仍兼容：`off`→关） |
| `reminder.vibrate` | `true` | 震动通道开关（与亮度解耦） |
| `reminder.bright` | `true` | 亮度脉冲通道开关 |
| `reminder.vibratePreset` | `medium` | 震动时长预设 short/medium/long/custom |
| `reminder.vibrateMs` | `180` | custom 时的每响时长（20–2000 ms） |
| `display.brightness` | `128` | 进入电脑模式后下发的默认亮度（32–255），改完即时 0x02 |
| `display.alwaysOn` | `true` | 旧版兜底字段；设置页实际改的是 `usb.alwaysOn` / `ble.alwaysOn`（PC 帧 `0x15`，当前固件支持开关） |
| `approval.enabled` | `true` | 关掉则审批全部交给电脑 |
| `approval.watchTimeoutMs` | `10000` | 手表优先超时（5/10/20 s） |
| `questions.enabled` | `true` | 手表问答 |
| `questions.watchTimeoutMs` | `30000` | 手表独占问答卡时长；超时 `next()` 交电脑（手表有操作会续期） |
| `queueWaitSec` | `60` | 审批/问答排队上限（10–60 s，默认 60）：多张卡同时到达时，后一张最多等这么久，超时交回电脑 |
| `ble.enabled` | `true` | USB 不在时启用 BLE（`@stoprocent/noble`） |
| `ble.name` / `address` | `PROTORIG ES` / `''` | 广播名前缀（绑定后自动拼 `PROTORIG ES <ADMIN>`）/ 上次直连地址（本机句柄，不跨平台） |
| `balance.enabled` / `intervalSec` | `true` / `60` | 余额开关与刷新间隔：优先读 DSH 登录账号（`deepseekAccount.getBalance`，与设置页「账号与余额」同源）；账号不可用时回退 `/user/balance` + `DEEPSEEK_API_KEY`（失败不造数） |
| `flash.bin` / `flash.baud` | `''` / `2000000` | 烧录 bin 与 ROM 波特率 |
| `debug` / `logFile` | `false` / `''` | 调试日志 / 落盘日志（开发副本用） |

> 数据目录：`<DATA_DIR>` = `PROTORIG_DATA_DIR` 或 `~/protorig-panel`（Windows 上是
> `%LOCALAPPDATA%\protorig-panel`），存烧录锁 `flash.lock`、`devices.json`、表盘缩略图缓存
> `<DATA_DIR>/faces/`。

## 设备身份与多设备

设备身份是默认表盘上显示的六位十六进制管理员编号（`admin_id`）。固件对 eFuse chip ID 做 FNV-1a 哈希并取低 24 bit，因此 Windows 与 macOS 上看到的编号一致。

```text
USB：  shell `adminid`  →  admin=2BEA4E chip=0123456789ABCDEF   （进 pcmode 1 之前问）
BLE：  广播名             →  PROTORIG ES 2BEA4E               （后缀；前缀不变，旧客户端兼容）
BLE：  GET_INFO 末尾 4 B  →  4E EA 2B 00                       （连接后可再校验）
```

- **USB 挑口**：枚举 VID 349B 的所有候选口 → 开第一个 → 握手第一步问 `adminid`。
  - 未绑定：连第一个，顺手把编号记下来显示在设置页；
  - 已绑定：编号对得上才继续（进 PC 模式、下发亮度/常亮）；对不上立刻关掉这个口，
    换下一个候选；全部对不上 → 不连接，日志/设置页提示"未找到绑定的手表"；
  - 问不出编号（旧固件 / 串口被占）：只有 1 个候选口时容忍（否则老用户反而连不上），
    多个候选口时跳过 —— 宁可连不上，也不要把设置下发到另一块表上。
- **BLE 挑表**：绑定后桥的名字参数变成 `PROTORIG ES <ADMIN>`（桥内部是 `startswith`），
  两块表同时广播时只会连到绑定的那块。CoreBluetooth 的 UUID / Windows 的 MAC 只当
  **本机直连句柄**（`ble.address`、桥内存里的 `lastAddress`），**不写进配置**。
- **换表保护**：检测到 `admin_id` 变化（USB 或 BLE）→ 清 `lastAppliedBrightness` /
  `lastAppliedAlwaysOn`、`framebuffer.invalidate()`、重置 metrics/焦点状态 → 重新下发亮度/
  常亮并整屏重绘。
  焦点只重置临时状态（步数/工具/审批计数），**保留当前 sessionId**，不会丢会话卡片。

## 设置页（浏览器半边）

`package.json` 的 `dsh.client` + `exports["./client"]` 让 DSH 把 `src/client.js`
加载进 Web GUI；它注册一个 `settings.section`（id `protorig-panel`，侧栏「PROTORIG 面板」）。
表单读写走 DSH 的 `scope`/`configForms`（path 数组 ops，嵌套字段也支持），
保存后写 profile 的 `cordis.patch.yml`，volatile 引用立即可见 → 插件下一个 tick 生效。

| 分组 | 内容 | 展示规则 |
|---|---|---|
| **概览** | 连接状态（USB 端口/BLE/离线）、`PROTORIG ES · 2BEA4E`、电量、固件版本、PC 模式；重连/断开/震动识别 | 永远第一屏；离线时提示"等待手表连接" |
| **显示** | 「USB / 蓝牙」分段切换（两套独立参数）、亮度、常亮、息屏时间（「跟随手表」= 固件默认 15 s，不覆盖终端设置）；底部信息（余额开关/刷新间隔） | 常开；改完 250 ms 即时下发 |
| **提醒与审批** | 事件提醒（完成/需要确认/失败）、提醒方式（震动/亮度脉冲/振动模式/自定义时长）、审批与问答开关+超时+排队上限（10–60 s） | 默认折叠 |
| **表盘** | **2 个表盘卡片**（0 默认 / 3 图片；**单击切换 / 双击预览**）、**安装图片表盘**（选图 → 时间位置/字体/叠层/颜色 → 进度 → CRC 失败提示）、**默认表盘下半区**（选图 → 480×232 裁剪 → 安装/恢复内置 → 进度） | 分组常开；两个安装子分区默认折叠；需要 USB；图片安装与下半区共用同一把串口锁 |
| **连接** | USB / 蓝牙连接开关、设备探测（只登记编号） | 默认折叠 |
| **设备** | 已登记设备（USB/BLE/已连接/已绑定）、绑定/解绑、当前连接、上次扫描 | 单表已绑定默认折叠；多表/未绑定时展开 |
| **高级** | 链路（fps/串口/插线自动进入）、维护（重置下发缓存）、**烧录（Windows/macOS）**、关于/许可 | 默认折叠；Linux 等无后端平台不渲染烧录，只给一行说明 |

状态只读 Host 的 `/api/dsh-protorig-panel/state`（1.5 s 轮询，不各自维护副本）；
「刷新设备列表」走 `/devices?refresh=1&probe=1`（逐口开串口问 `adminid`，当前连接口跳过）。

## DSH 接线

### 会话

`ctx.on('session/event', (session, event) => …)`，投影逻辑与桌面样例一致：

- 只跟踪**最近一条事件所属的主会话**；`isSubagentSession()` 判断子代理并忽略；
- 状态：`IDLE → RUNNING → NEEDS → DONE/FAILED`；`turn/end` 后 6 s 内显示 DONE/FAILED；
- 步数 = 本轮 `step/start` 计数；工具短名白名单 bash/edit/read/search/other；
- 标题读 `~/.dsh/storages/session_projcache/sessions/<id>.json` 的 `record.rows.title.val`（2 s 缓存；
  有 `DSH_HOME` 时优先用它）。CTX 读同一文件的 `record.rows.contextPressure.val`。
  **只读这两个字段**，不匹配时回退 `SESSION` / -1。

### 审批

```js
ctx.on('approval/request', async (req, next) => …, { prepend: true });
```

- `prepend` 保证排在 Web UI 的转发器前面：先由手表出卡 10 s，没人点才 `next()` 交给电脑；
- **并发排队**：审批/问答共用一条 FIFO 队列（表上一次只显示一张卡）。第二张及以后排队，排队超过 `queueWaitSec`（10–60 s，默认 60）或队列超过 5 张才交回电脑。
  每张卡的 10 s 计时从它真正显示时开始；卡片前会带**会话短标题**（多对话并发时区分来源）；
- `req` 只有 `agent/toolName/callId/reason/signal`（**没有工具参数**）。命令文本从最近
  一次 `tool/call` 事件按 `callId` 取（只存内存、最多 16 条），拿不到就显示 reason；
- 电脑答完：`next()` 的 Promise 落定 → 撤卡；`approval/decided` 作为兜底；
- `signal` 中止 → 返回 `'cancelled'`，不把作废请求转给电脑。

### 问答

```js
ctx.on('user-questions/request', async (req, next) => …, { prepend: true });
```

- 手表每页 1 个选项（设计稿只有 option/next/submit 三个热区），`下一项`循环翻页，
  多选累加，`提交`回写 `{ answers: [{ id, selected: [label...] }] }`；多个问题按顺序逐题提交；
- **上下滑翻页**：向上 = 下一页、向下 = 上一页（垂直 ≥40 px、水平 <30 px、<800 ms）；
  左右滑由固件处理。BLE 电脑模式可用左滑退出，USB 像素透传时可从宿主退出；
- 没有选项/纯文本问题直接 `next()` 交电脑（表上没有输入法）；
- 手表独占 30 s 无操作 → `next()` 交电脑，手表提示到电脑上继续操作。电脑答完后撤卡；
- 与审批共用同一条 FIFO 队列（`queueWaitSec` 10–60 s，默认 60；最多 5 张），
  卡片题干前带会话短标题。

### 模式切换（PC 0↔1）

- `PC 0`（退出电脑模式）→ 立即停发像素（`#tick` 要求 `pcMode && passthrough`）；
- `PC 1`（进入电脑模式）→ 走与 USB 重连相同的握手：`adminid` → `settime` → HELLO（最多 3 次）→
  等 `MODE 1` → `fb.invalidate()` 整屏重发；握手进行中再次收到 `PC 1` 会排队补一轮；
- 模式切换后 500 ms 内的触摸全部丢弃（滑动的收尾残点不能当成点击）。

### 触摸护栏（防滑动误触）

1. `pcMode=0` 或 `passthrough=0`（本地页）→ 忽略所有触摸；
2. 模式切换后 500 ms 静默期；
3. 点击 = 同一热区按下→抬起，80 ms ≤ 时长 ≤ 2 s，累计位移 ≤30 px；
4. 上下滑（≥40 px / 水平 <30 px / <800 ms）只翻页，不点击；左右滑插件不处理。

> 热区命中要按当前叠加层过滤：审批的 deny/allow（y=296..392）与问答的 next/submit
> （y=314..410）在设计稿里坐标重叠，不过滤会"点了没反应"。

### 提醒

完成/需响应/失败三档，全部用固件执行器：震动 `0x03` + 亮度脉冲 `0x02`，
**不刷像素做闪屏**。闪完恢复最近一次 `0x91` 的 `bright`（再补发一次防丢）。
同一类事件 2 s 内去重。

### BLE 降级与 USB 回切

USB 不在（`link.state.connected === false` 持续 >2.5 s）→ 用 `@stoprocent/noble`
扫描广播名（默认前缀 `PROTORIG ES`，绑定后为 `PROTORIG ES <ADMIN>`）→ 连接 GATT →
`0x51 pc_mode 1` + 5 Hz `0x52` 指标。断开/连接失败按 15 s 退避重扫；
蓝牙不可用（未授权/未开）只警告一次，不影响 USB 面板。

断开期间插件**持续探测 USB**（每 `reconnectDelayMs` 一轮，异步不阻塞面板循环）。设备回来时：

```
停 BLE（停止扫描 → 断开 GATT）
→ 打开串口 → adminid（身份门禁）→ settime → HELLO/MODE 1 → 整屏重发（fb.invalidate）
```

- 端口被串口助手占用导致 open 失败时，15 s 内不再"先停 BLE"，避免 BLE 被反复停启；
- `BleBridge.start()` 幂等；`stop()` 可重复调用且断开 GATT；`dispose()` 不留后台扫描；
- 真机回归脚本：`node tools/reconnect-check.mjs`（运行前先从 DSH 释放目标串口和 BLE）。

### HTTP 路由

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/state` | 面板状态 + 设备身份 + 平台能力 + 候选缓存 + 烧录任务 + `focus`（会话投影摘要：state/tool/steps/turnOpen） |
| GET | `/devices` | 候选设备（`?refresh=1` 重新枚举，`?probe=1` 逐口问 `adminid`） |
| POST | `/devices/probe/usb` `/devices/probe/ble` | 探测注册：只扫描并把编号登记到「已登记设备」，不改变当前连接 |
| POST | `/reconnect` `/disconnect` `/identify` | 概览的快捷操作（重连/断开/震动识别） |
| POST | `/cache/reset` | 重置下发缓存（调试） |
| POST | `/preflight` `/flash` `/flash/cancel`、GET `/flash/state` | 烧录（Windows/macOS 双后端） |
| GET | `/faces` / `/faces/thumb?crc=` | 表盘状态（含 `thumbCached`、`panel` 缓存、`busy`）/ 本地缩略图 dataURL |
| GET | `/faces/preview` | 内置默认表盘的静态预览（`assets/faces/default.png`，dataURL） |
| GET | `/faces/panel-mask` | 内置下半区图的羽化 alpha（PNG dataURL；裁剪框 WYSIWYG 与服务端打包共用） |
| GET | `/facepanel/builtin-preview` | 内置下半区图的预览（`assets/faces/builtin-panel.png`，dataURL） |
| POST | `/faces/switch` `/faces/status` `/faces/install` `/faces/install/cancel`、GET `/faces/install/state` | 切表盘（**只接受 0/3**）/ 设备侧 CRC 复算 / 装图 / 取消 / 进度 |
| GET | `/facepanel`、`/facepanel/thumb?crc=` | 默认表盘下半区状态（`?crc=1` 让固件重算 CRC；含 `job`/`busy`）/ 面板预览 dataURL |
| POST | `/facepanel/install` `/facepanel/install/cancel` `/facepanel/clear`、GET `/facepanel/install/state` | 装下半区图 / 取消 / 恢复内置默认图 / 进度 |
| POST | `/display/apply` | 显示设置拖动后立即下发（USB 亮度/常亮/息屏，不必先保存） |
| POST | `/pcmode` | 手表模式 ↔ 电脑副屏 |
| GET | `/balance`、POST `/balance/refresh` | 余额 |

## 烧录

两端各有一个后端，对外接口一致（`supported / preflight / start / view / abort`）：

| 平台 | 后端 | 链路 |
|---|---|---|
| Windows | `src/flash.mjs` | `flash.ps1` + `BLFlashCommand.exe`（脚本路径 `PROTORIG_FLASH_SCRIPT` 覆盖；V1.6 固件工程在 `firmware/lvgl_test/`） |
| macOS | `src/flash-mac.mjs` | serialport 发 `reboot uart` → 等新 `/dev/cu.usbmodem*` → `BLFlashCommand-macos --interface=uart … --config=<build/flash_prog_cfg.ini>`（**需要 Rosetta 2 + 本机已编译固件**，见 `firmware/README.md` 与 macOS 构建脚本） |

共同流程：

1. 预检：文件存在、非空、`.bin`、大小、sha256 前 8 位；macOS 额外检查编译产物/工具/Rosetta；
2. 暂停面板（关串口、停 BLE、挂起 USB 重连）；
3. `reboot uart` → 等 ROM 下载口 → 写 flash（boot2 + partition + 应用）→ 校验；
4. 成功后保持 `flashHold`：板子停在 ROM 下载模式，**必须手动拨动开关重启（SW2 ON→OFF→ON；拔 USB 不会断开电池供电）**；
   重启后插件识别原应用口自动重连、重新握手、整屏重发。

入口两个：

- 设置页「高级 · 烧录」：预检 → 烧录 → 轮询阶段/日志 → 断电提示（Linux 等无后端平台不渲染）；
- agent 工具 `protorig_flash { bin, confirm: true, romPort?, baud? }`：不传 `romPort` 时自动判断应用口/下载口（应用口带 `MI_00`，下载口不带），返回
  `{ ok, stage, seconds, bin, sha8, size, needsPowerCycle, error, logTail }`，
  无后端平台直接返回说明并拒绝。

### Agent 工具

| 工具 | 用途 |
|---|---|
| `protorig_state` | 只读：连接/模式/电池/亮度/设备编号，agent 自检 |
| `protorig_face` | 表盘 `status`/`switch`/`install`/`pcmode` |
| `protorig_panel` | 默认表盘下半区 `status`/`install`/`clear`（480×232 I8，写 media 分区；install 需 `confirm:true`） |
| `protorig_conn` | 让出/收回串口与 BLE：`status`/`pause`（默认 120 s，到期自动恢复）/`resume`。**跑 CLI、`ble-e2e`、串口助手或自定义脚本前先 pause**，否则口被插件占用或刚让开又被抢回 |
| `protorig_flash` | 烧录（必须 `confirm:true`，烧完人工断电）；不传 `romPort` 时自动判断应用/下载模式；spawn 失败只报错不崩宿主 |
| `protorig_notify` | USB 限时通知卡（1–30 s，可选震动，按圆形安全区排版；标题≤24/正文≤100）；BLE 只震动；审批/问答在屏时返回忙 |
| `protorig_vibrate` | 震动 `ms`（20–2000）×`times`（1–5） |
| `protorig_screenshot` | 同一 renderer 重渲当前画面为 480×480 PNG，返回本机路径；需电脑模式 |

> ⚠️ 烧录期间面板会短暂断开；烧完必须手动拨动开关重启（SW2 ON→OFF→ON；拔 USB 不会断开电池供电），软复位无效。

## 表盘切换与图片安装

PFI 格式见[固件中的 `svc_face_store.h`](../../firmware/lvgl_test/services/svc_face_store.h)。

- **切换**：shell `face 0|3`（0 为默认表盘，3 为图片表盘）。
  切表盘会先 `pcmode 0` 退回手表模式，并用 `faceHold` 抑制"收到 PC 1 就自动回电脑模式"
  （否则表盘会被像素副屏盖住）；设置页有「回到电脑模式」。
- **安装**：
  1. 设置页选 PNG/JPG → `POST /faces/install`（dataURL）；
  2. Host 侧 `face-pack.mjs`：cover 裁成 480×480（LANCZOS 级）→ 加权中位切分 256 色 +
     Floyd–Steinberg 抖动 → palette B,G,R,A + 索引（231,424 B）+ header（256 B，双 zlib CRC32）→ PFI；
  3. shell 序列：`faceimg erase → begin <len> <crc32> → 4822×w <off> <48B hex> → end <x> <y> <font> <flags> <rrggbb>`；
     48 B/行、逐行等 `W-OK`（SDK shell 单行上限 120 字符、环形缓冲 512 B）；
  4. 固件读回 231 KB 重算 CRC → 一致才写 header；插件再 `faceimg info` 复核一次；
  5. `faceimg apply` 切到 face 3（持久化到 SD3010，断电保持）。
- **安装结果**：上传通常需要约 45 秒。固件校验格式和 CRC，校验通过后写入图片表盘，重启后仍可使用。
- **设置页体验**：**2 个表盘卡片**（0 默认 / 3 图片）；图片表盘的缩略图按 image CRC 缓存在
  `<DATA_DIR>/faces/<CRC>.png`（`PROTORIG_FACE_CACHE` 可覆盖；装图时自动写，换电脑/清缓存后
  旧图没有缩略图，显示"暂无本地缩略图"）；单击切换（延迟 260 ms 给双击让路）、双击打开大图预览
  （内置表盘提示"由手表渲染"，图片表盘显示 288×288 预览 + 叠层参数）。
- **agent 工具**：`protorig_face`
  - `{ action: 'status' }` → 当前 face / 图片槽 CRC / 安装进度
  - `{ action: 'switch', id: 0 }` → 切表盘（**只接受 0（默认）/ 3（图片）**）
  - `{ action: 'install', imagePath: '/path/a.png', confirm: true, layout?, apply? }`
    → PNG/JPG（或现成 `.pfi`）打包 + USB 上传；未传 `confirm:true` 直接拒绝
  - `{ action: 'pcmode', enter: true }` → 切回电脑副屏

## 默认表盘下半区

face 0 的 y=248..479 那 480×232 区域可以换成自己的图；上半区时间/表针/日期/电量由固件
实时绘制，**不受影响**（chrome 在图片之后创建，自动盖在上面）。

| 项 | 值 |
|---|---|
| 屏幕位置 | x=0, y=248, 480×232 |
| 格式 | I8：1024 B palette（`lv_color32_t` 内存序 B,G,R,A）+ 111,360 B 索引 = **112,384 B** |
| PC 管线 | PNG/JPG → cover 480×232 → 加权中位切分 256 色（可选抖动）→ I8 → zlib CRC32 |
| shell | `facepanel`（状态）→ `begin <len> <crc32>` → **2342 ×** `w <off> <48B hex>` → `end`（固件校验 CRC 后写 header 并重建 face 0）→ `info`（读回重算复核） |
| 恢复内置 | `facepanel clear`（只擦 header 那 4 KB 扇区，face 0 立刻回内置默认图（DSH 鲸鱼联动标）） |
| 持久化 | header 写在 flash `0x3B2000`（独立 4 KB 扇区），图片在 `0x3B3000`；断电保持 |
| 预览缓存 | `<DATA_DIR>/faces/panel-<CRC>.png`（本机装过才有；换电脑显示占位） |

- 实现：`src/face-panel.mjs`（打包 + 命令序列 + 状态解析）、`panel-service.mjs`
  （`panelStatus` / `panelInstallStart` / `panelClear`，与图片表盘**共用同一把串口锁**）、
  `routes.mjs` 的 `/facepanel*` 路由、`client.js` 的 `PanelSection`；
- 上传走 USB shell，每段写入等待固件确认；
- 坏 CRC 拒装且不写 header（固件 `end` 校验 + 插件 `info` 复核两道）；
  开始新上传时会擦除目标槽。若上传中断，请重新安装或选择恢复内置图。

## 联调命令

主机和实机验收见 [测试与验收](../../firmware/lvgl_test/docs/TESTING.md)。

```bash
cd plugin/dsh-protorig-panel
node tools/cli.mjs hello --enter                  # 握手 + 对时
node tools/cli.mjs panel --enter --seconds 10     # 像素面板
node tools/cli.mjs touch --enter --seconds 20     # 触摸帧
node tools/cli.mjs testcard --enter               # 几何标定卡
node tools/cli.mjs bright --level 200 --enter     # 0x02：用 0x91 的 bright 自证并恢复原值
node tools/cli.mjs vibrate --ms 300 --enter       # 0x03：无回执，需人确认震感
node tools/reconnect-check.mjs                    # 真机回归：USB→BLE→USB 回切 + 滑出滑回重握手
npm run check:release                             # 发行版依赖自检（只允许 npm 包 + 包内资源）
npm test                                          # 插件测试（不需要手表）
npm pack --dry-run                                # 看发布内容（assets/design/src/tools/README/许可）
```

无 USB 的 BLE 联调：拔掉 USB、确认蓝牙已授权后，直接跑 `node tools/reconnect-check.mjs`
或看插件日志里的「BLE 数字通道已连接」。

## 已知限制

- **media 分区不会被烧录擦除**：换固件后自定义图片表盘 / 下半区面板仍在
  （face 3 还是你的图，face 0 下半区还盖着内置默认图）—— 回内置要 `facepanel clear`
  或设置页「恢复内置」。
- macOS 烧录需要本机编译的固件和 SDK 烧录工具。Apple Silicon 上运行 x86_64 的烧录工具需要 Rosetta 2。
- **余额**优先读 DSH 登录账号（和设置页「账号与余额」是同一个数）；账号不可用时回退 DeepSeek `/user/balance` + `DEEPSEEK_API_KEY`。
  两者都没有 / 请求失败时显示占位（`--`）。
- 手表独占问答卡 30 s 是折中：电脑端在超时前看不到问题。
- 审批卡的"命令"来自 `tool/call` 参数；拿不到时按设计稿禁用"允许"（防误触）。
- 触摸在抬起时确认。
- 插件同一时间只服务一块手表。
