# ImGui Launcher

无边框启动器。C++17，Dear ImGui 1.93.1 WIP，Win32 + D3D11。只编 x64。

启动时按屏幕高度算一次缩放，窗口尺寸随后固定。清单和代码里都声明了 Per-Monitor V2，按物理像素排版。

## 构建

需要 Visual Studio 2022（工具集 v143）、Windows SDK，以及随 MSVC 一起装的 MASM（`ml64`）。工程里有一份 x64 汇编。

1. 打开 `ImGui_Launcher.sln`
2. 配置选 Debug 或 Release，平台选 x64
3. 生成

输出在 `Build\Debug\ImGui_Launcher.exe` 或 `Build\Release\ImGui_Launcher.exe`。Debug 会额外开一个控制台。

`Build/`、`tmp/`、`.vs/` 不进版本库。

## 目录

| 路径 | 内容 |
|---|---|
| `ImGui_Launcher/src` | 窗口、渲染、页面、线程池 |
| `ImGui_Launcher/src/imgui` | Dear ImGui 本体和 Win32 / DX11 后端，不要按本仓库风格改 |
| `ImGui_Launcher/src/imgui/custom` | 自写控件和特效 |
| `ImGui_Launcher/src/effects` | 背景光波、面板模糊 |
| `ImGui_Launcher/src/shaders` | 运行时编译的 HLSL |
| `ImGui_Launcher/src/misc` | FNV-1a 练习，含一份汇编 |

登录请求还是占位，个人页上的 toast / 哈希按钮是手动测试。

## 许可

本仓库代码使用 [MIT License](LICENSE)。
