# Game Speed — Windows x64

版本 0.1.0。适配网易 Minecraft Windows 客户端 **3.10.0.420447**，需要支持插件和首次 IPC 连接事件的 MCDevTool 宿主。渲染接口为桌面 OpenGL 3.2 或更高。其他游戏版本的计时适配未验证，检测不匹配时只允许 1×。

## 安装

1. 将压缩包中的整个 `game-speed` 文件夹放到 MCDevTool 项目的 `plugins` 目录，保持 DLL、`assets` 和 `licenses` 的相对位置。
2. 将以下条目加入项目 `.mcdev.json` 的 `plugins` 数组。包内也提供 `mcdev.plugins.example.json`；请合并插件条目，保留自己的游戏路径和其他配置。

```json
{
  "enable": true,
  "id": "com.github-zero123.game-speed",
  "path": "./plugins/game-speed",
  "config": {
    "autoInject": true,
    "initialSpeed": 1.0,
    "showUi": false
  }
}
```

3. 通过 MCDevTool 启动游戏并进入世界。首次 IPC 连接后自动注入，默认只显示短暂的快捷键提示。

本包不包含 MCDevTool 宿主或游戏客户端，也不需要 MCP。C++ 运行库和 RmlUi、MinHook、FreeType 已静态链接，不需要另外安装这些库。

## 操作

- `F8` 或 `Ctrl+Shift+G` 打开/收起面板；部分笔记本需按 `Fn+F8`。
- `Esc` 收起面板。拖动标题栏移动窗口，位置只在当前运行期间保留。
- 可调倍率为 `0.01～16×`，支持暂停和恢复 1×。仅缩放已适配的 simulation Timer，渲染、输入及 UI 保持真实时间。
- 面板打开时接管鼠标、键盘和光标，回焦的首次激活点击会被消耗；关闭面板后归还游戏输入。

## 独立注入和控制

不使用宿主插件时，可在 `game-speed` 文件夹内打开 PowerShell，使用实际游戏 PID 替换示例中的 `12345`：

```powershell
.\gamespeed.exe inject 12345 "$PWD\gamespeed_hook.dll"
.\gamespeed.exe show 12345
.\gamespeed.exe status 12345
.\gamespeed.exe set 12345 2
.\gamespeed.exe pause 12345
.\gamespeed.exe resume 12345
.\gamespeed.exe shutdown 12345
```

`shutdown` 恢复 1×并隐藏 UI，DLL 保持驻留。更新插件前请关闭游戏，完整替换文件夹后重新启动。

## 文件与验证

- `game_speed_plugin.dll`：MCDevTool 宿主插件。
- `gamespeed_hook.dll`：游戏内计时、输入和 RmlUi 界面。
- `gamespeed.exe`：独立注入与命令行控制工具。
- `assets/`：OreUI 界面、字体和按钮资源，须完整保留。
- `licenses/`、`THIRD-PARTY-NOTICES.txt`：第三方许可与资源署名。
- `BUILD-INFO.json`：构建版本、依赖及验证记录。
- `SHA256SUMS.txt`：包内文件的 SHA-256 校验清单。

输入问题诊断：执行 `gamespeed.exe input-trace-start <pid>`，复现后执行 `gamespeed.exe input-trace-stop <pid>`。采样默认关闭，结果写入插件目录的 `input-trace-<pid>.json`。
