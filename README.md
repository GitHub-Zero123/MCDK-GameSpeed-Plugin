# MCDK GameSpeed Plugin

Windows x64 游戏变速插件，参考 VoxelDirective 的 MCDevTool SDK 插件布局。目标为网易 Minecraft Windows 客户端 `3.10.0.420447`。已在实际游戏进程确认 QPC Hook 命中，以及 OpenGL Core 渲染上下文；界面使用支持 Core 的 GL3 后端。

项目包含三个产物：

- `game_speed_plugin.dll`：加载到 MCDevTool，在 runtime 阶段记录游戏 PID，等首次有效调试 IPC 连接后异步注入。
- `gamespeed_hook.dll`：加载到游戏，Hook QPC；在 OpenGL `SwapBuffers` 上绘制 RmlUi，并接收本机控制管道命令。
- `gamespeed.exe`：独立注入、查询和控制工具，可绕开 MCDevTool 测试。

## 构建

本机预设使用 Visual Studio 2026、Windows SDK、CMake 3.25+。第三方代码全部由 Git 子模块固定提交，不依赖机器上预装的 FreeType。

```powershell
git submodule update --init --recursive
cmake --preset windows-x64
cmake --build --preset release --parallel
ctest --preset release
```

若使用 VS 2022，可直接指定生成器，其他构建步骤相同：

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Release --parallel
ctest --test-dir build/vs2022 -C Release --output-on-failure
```

预设的完整插件目录为 `build/windows-x64-gl32/plugins/game-speed/Release/`，控制器在 `build/windows-x64-gl32/bin/Release/gamespeed.exe`。整个插件目录一起部署，保留 `plugin.json`、两个 DLL 和 `assets/`。各库统一使用静态 CRT；RmlUi、FreeType 和 MinHook 静态链接进注入 DLL。

若旧插件 DLL 正被宿主或游戏占用，可通过 CMake 缓存项 `GAMESPEED_PACKAGE_DIR` 将新包输出到另一个目录，然后修改 `.mcdev.json` 的插件 `path`。本工作区的 F8 更新使用 `build/windows-x64-gl32/plugins/game-speed-f8/Release/`；默认预设的构建目录保持不变。更换插件包后重新启动 MCDevTool 游戏会话，以加载新的宿主插件和注入 DLL。

纯时钟测试不需要初始化子模块，也可跨平台构建：

```sh
cmake --preset core
cmake --build --preset core
ctest --preset core
```

## MCDevTool 接入

将以下条目合并到游戏 Mod 工程现有 `.mcdev.json` 的 `plugins` 数组；路径应指向实际插件包，可以使用绝对路径。不要覆盖已有工程配置。配置样例位于 `examples/mcdev.plugins.json`。

```json
{
  "enable": true,
  "id": "com.github-zero123.game-speed",
  "path": "D:/Zero123/CPP/CMAKE/MCDK-GameSpeed-Plugin/build/windows-x64-gl32/plugins/game-speed/Release",
  "config": { "autoInject": true, "initialSpeed": 1.0, "showUi": false }
}
```

宿主配置支持 `autoInject`、`initialSpeed`、`showUi`。`showUi` 默认 `false`：首次 IPC 连接后完成注入，仅显示 Ore UI 浮动快捷键教程，8 秒后自动消失；按 `F8` 或 `Ctrl+Shift+G` 打开面板。提示自身使用真实计时，即使游戏时间暂停也会消失，且不会捕获游戏输入。显式设置 `showUi:true` 可进入后直接打开面板。本项目的 MCP 服务配置关闭。

自动注入使用游戏调试 IPC 的首次有效连接作为就绪信号，需要启用 `include_debug_mod`。插件处理连接先于 runtime 的情况，也检查当前 IPC 连接快照；同一游戏进程只发起一次注入，重复连接不会重复加载 DLL。首次进入世界前，宿主会显示等待 IPC 的提示。

宿主必须启用 MCDevTool 插件系统。当前 VS Code 扩展 `0.1.32` 附带的旧 `mcdk.exe` 不支持插件加载，会忽略 `plugins` 配置。本机工作区 `.vscode/settings.json` 已将 `mcdev-tools.mcdkPath` 指向已启用插件的本地宿主：`D:/Zero123/CPP/CMAKE/MCDevTool/build/x64-msvc-release/tools/mcdk/mcdk.exe`。使用其他机器时，在该工作区设置里选择对应的新宿主，然后重新启动游戏会话。

## 独立使用

先通过 MCStudio 正常启动客户端，取得实际游戏 PID。以下以 `12345` 为示例：

```powershell
$controller = './build/windows-x64-gl32/bin/Release/gamespeed.exe'
$hookDll = (Resolve-Path './build/windows-x64-gl32/plugins/game-speed/Release/gamespeed_hook.dll').Path
& $controller inject 12345 $hookDll
& $controller set 12345 2
& $controller status 12345
& $controller pause 12345
& $controller resume 12345
& $controller reset 12345
& $controller shutdown 12345
```

`status` 返回计时倍率、Hook 命中次数 `scaledCalls`、`overlayReady`、`overlayError` 和 `startupHintVisible`。`inject` 重用已经加载的同一 DLL，避免重复创建 Hook。

倍率为 `0.01～16`，`0` 表示暂停；`resume` 恢复暂停前倍率。`F8` 或 `Ctrl+Shift+G` 显示/隐藏操作面板；若笔记本 F8 控制系统功能，可按 `Fn+F8`，或直接使用 `Ctrl+Shift+G`。旧的 `Insert` 快捷键仍有效。面板提供预设倍率、滑块、数值输入、暂停和恢复 1 倍；打开时还可用 `Pause`、`Home`、`+/-`。UI 自身使用真实 QPC 时间，因此暂停游戏计时后仍能操作。

界面采用用户提供的 Ore UI 组件库风格：原版绿色主按钮、浅灰次按钮、方角像素边框、按下下沉效果及 Minecraft Seven/Ten 字体。按钮直接使用 `D:/Zero123/.Temps/MCTest/oreui-unpacked/test/hbui` 的原始默认、悬停、按下及焦点贴图，按原图九宫格切片渲染，保留深色外框、像素高光和下沿；使用最近邻采样避免边缘模糊。颜色、间距和字体适配为 RmlUi RCSS；字体及按钮资源随插件包提供，来源和原始文件校验见 `assets/oreui/provenance.json`。

主面板首次打开在游戏窗口内居中，标题栏可拖动，鼠标离开游戏窗口后松开也能结束拖动；后续打开保留调整的位置，窗口缩小时限制在可见范围。倍率轨迹记录最近 12 秒的真实倍率，包含暂停状态及当前倍率持续时间。小窗口下内容可滚动，标题和快捷键栏保持可见。`Esc` 只隐藏面板，再按 `F8` 或 `Ctrl+Shift+G` 可重新打开。

打开主面板时，游戏背景覆盖 20% 黑色全屏遮罩；遮罩随面板收起，启动快捷键提示不触发遮罩。拖动期间保持标题栏原有配色。

滑块按原版 BaseSlider 的灰色轨道、绿色进度和 32×32 方形手柄绘制，保留悬停、拖动及键盘焦点反馈。刻度采用两段对数映射：左半段为 0.01～1 倍，右半段为 1～16 倍，1 倍位于正中；标尺为 0.01 / 0.1 / 1 / 4 / 16 倍。拖动实时更新倍率并保留两位小数，暂停时显示恢复倍率；拖动期间状态刷新不会抢回手柄位置。

控制管道 `\\.\pipe\MCDK.GameSpeed.<pid>` 只允许同一 Windows 用户和 SYSTEM，并拒绝远程连接。注入器检查目标和 DLL 均为 x64，使用实际远程系统模块加 RVA 定位 `LoadLibraryW`，支持 Unicode DLL 路径。连接或加载超时会返回错误。

若进程仍在启动，注入器会在总超时内等待目标加载器模块，重试暂时不完整的模块枚举；进程退出立即返回错误。失败信息保留 Windows 错误码，区分尚未初始化、枚举失败和 DLL 加载失败。

## 计时与补偿

不修改 QPC frequency。虚拟计数器从原始 QPC 值开始，逐段积分：

```text
virtual_now = virtual_anchor + (real_now - real_anchor) * speed
```

切换倍率前，先按旧倍率结算到切换时刻，再修改倍率。暂停时虚拟时间保持不变；恢复时从该值继续，暂停期间的真实时间不会补入游戏。保留小数 tick，拒绝非有限倍率，处理并发旧采样和整数溢出，返回值始终不递减。

这保证使用同一 QPC 时间域的 `before < now` / `now - before` 累积器不会因为切倍率或重置发生计时回退。`reset` 和 `shutdown` 将斜率恢复为 1 倍，保留已累计的偏移；直接卸载 Hook 会令计数器跳回原始时间域，破坏这项保证。因此注入 DLL 和 trampoline 保持驻留，直到游戏退出。`shutdown` 关闭面板并恢复连续 1 倍；之后可再次 `show`。更换 DLL 构建需要重启游戏。

默认缩放**主 EXE 直接调用 QPC** 的路径，并在已加载的 `MSVCP140.dll` 存在时单独处理主 EXE 调用 `_Query_perf_counter` 的 MSVC `steady_clock` 路径，避免改变 Windows、音频库及插件 UI 的计时。若实际 tick 经其他 DLL、另一种计时器或其他运行库包装函数调用，需为该路径添加适配；先通过 `scaledCalls` 和游戏 tick 观察确认。游戏引擎自身的单帧补 tick 上限、真实时间等待、联网同步和服务器 tick 限制仍由游戏决定，16 倍不保证产生 16 倍有效 tick。

OpenGL 界面使用 RmlUi GL3 后端，要求桌面 OpenGL 3.2+，支持 Core 和兼容上下文。实际客户端创建的是 OpenGL 3.2 / GLSL 1.50；构建时在生成的后端副本中使用 GLSL 150，保留子模块原始代码。采样器对象按运行时版本和扩展启用。OpenGL ES、DirectX 和 Vulkan 暂未支持。窗口输入进入有界队列，RmlUi 操作仅在首次选定的渲染线程和上下文处理；线程或上下文更换需要重启客户端，避免将旧上下文的 GPU 资源错误删除到新上下文。UI 前后保存并恢复受影响的 GL 状态，窗口缩放更新布局；缺少渲染入口时，计时命令仍可单独使用。

## 验证与依赖

CTest 包含确定性时钟测试、边界/溢出/小数累计和多线程测试，以及加载真实宿主插件 DLL 的 ABI/配置和首次 IPC 注入时序测试。加载器回归测试创建暂停的独立进程，验证未初始化时的超时和恢复后注入成功；Windows 界面测试创建隐藏 OpenGL 3.2 Core 进程，验证 QPC/chrono、暂停恢复、RmlUi 绘制、GL 状态恢复，以及重复 Esc 关闭后使用 F8 / Ctrl+Shift+G 重开。组合键测试只修改独立测试线程的键盘状态，不占用桌面焦点。另有 420×360 小窗口验证。测试不启动本机游戏。

依赖与固定版本：

| 子模块 | 版本/提交 | 用途 |
| --- | --- | --- |
| [MCDevTool](https://github.com/GitHub-Zero123/MCDevTool) | `ca1f2e01a0600db7202e94ca1930b1268b48ad75` | 宿主插件 ABI/SDK；仅构建 SDK |
| [RmlUi](https://github.com/mikke89/RmlUi) | `6.3` | HTML/RCSS UI、GL3/Win32 后端 |
| [MinHook](https://github.com/TsudaKageyu/minhook) | `v1.3.4` | QPC 和 OpenGL 入口 Hook |
| [FreeType](https://github.com/freetype/freetype) | `VER-2-14-3` | 字体渲染 |

RmlUi [渲染接口文档](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html)与[集成文档](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/integrating.html)说明了 UI 接口和渲染生命周期。各第三方库许可证保留在子模块中；Ore UI 字体保留原始文件及嵌入元数据。`.gitignore` 排除构建目录、IDE 状态、二进制、日志和本机 `.mcdev.json`，不会忽略源代码或子模块。
