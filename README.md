# RizFalling · 落线

Rizline 4K 实时读谱叠加层（Windows / d3d11 进程内注入）。

在 Rizline 游戏画面上叠加一层 4 轨下落读谱层（轨道 + 判定线 + 球皮 tap + 投皮 hold），
用你自己的 4K 谱面辅助读谱。全部功能集成在**一个 `d3d11.dll`** 里：谱面解析、渲染、
配置、按键都在 DLL 内，不需要 Python 或后台进程。

> **非官方项目**，与 Pigeon Games / Rizline 官方无关。仅供个人学习与读谱辅助使用。
> 仓库不包含任何游戏本体、谱面、音乐或图片资源。

## 功能

- 进程内 D3D11 渲染：Present hook + ImGui，直接画在游戏交换链上，不经 DWM
- 直接读取 `.osz`（内嵌 miniz 解压 + osu!mania .osu 解析，无外部依赖）
- 游戏内 **INS 菜单**：加载谱面、分页列表、谱面文件夹选择、下一难度
- 两种锚定方式（每次载入谱面只生效一次）：
  - **任意普通键**：按任意字母/数字/符号键即锚定
  - **指定键**：默认 J，可自定义
- **自动对齐（可选）**：内置 WASAPI loopback 采集 + OGG 解码 + onset/NCC 歌头匹配，
  音乐响起约 1~2 秒自动锁定（实测误差约 20ms）；暂停冻结、原位恢复、重开一局自动识别
- **对拍校准**：锚定后跟着音符打拍 6 下以上，自动取中位差修到「偏移 ms」
- 可选：锚定成功后**模拟按住一个按键**（失焦/重载/关闭自动松开，防卡键）
- 显示调节：流速、偏移、轨道宽度/水平偏移、判定线高度、球大小、谱面缩放、
  轨道颜色/不透明度、全屏暗色、**镜像（左手）**、**投皮尾形**（尖尾/空缺/无）
- 所有调节自动保存到 INI，下次启动沿用

## 安装

1. 从 Releases 下载 `d3d11.dll`（或自行编译，见下）。
2. 复制到 `...\Steam\steamapps\common\Rizline\`。
3. 重启游戏。

> 这是 d3d11 代理 DLL（转发系统 d3d11 导出，并在 `IDXGISwapChain::Present` 上绘制）。
> 杀软可能误报注入类 DLL；异常时删除游戏目录下的 `d3d11.dll` 即可恢复。

## 使用

1. 游戏内按 **INS** 打开菜单。
2. 点 **谱面文件夹…** 选择存放 `.osz` 的目录（会记住）；点 **加载谱面…** 打开分页列表，
   每首右侧 `IN / HD / EZ` 直接加载；其它难度用 **下一难度** 循环。
3. 进入打歌，在**首音到达判定线**时触发锚定（见上「两种锚定方式」）。
4. 菜单里实时调整显示参数；约 1 秒后自动保存。

## 配置

`%LOCALAPPDATA%\RizFalling\native.ini`（自动创建）

| 键 | 说明 |
|---|---|
| `speed_milli` / `offset_ms` | 下落速度（千分比屏高）、时间偏移 |
| `lane_w_milli` / `gap_milli` | 轨道宽 / 间距（千分比屏高） |
| `judge_milli` / `note_milli` | 判定线位置、球大小（千分比） |
| `lane_a_milli` / `edge_milli` | 轨道底色 / 边框透明度 |
| `note_a_milli` / `hold_milli` | 球 / hold 透明度 |
| `note_color` / `hold_color` / `judge_color` / `lane_color` | 0xRRGGBB |
| `lane_off_milli` / `judge_off_milli` | 轨道水平 / 判定线垂直偏移 |
| `dark_on` / `dark_milli` | 全屏暗色开关 / 强度 |
| `scale` | 谱面缩放 %（50–200） |
| `visible` / `lane_vis` | 叠加层 / 轨道显示 |
| `anchor_any` | 1 = 任意普通键锚定，0 = 指定键 |
| `auto_align` | 1 = 自动对齐（音频匹配），0 = 手动 |
| `mirror` | 1 = 轨道镜像（左手） |
| `hold_tail` | 投皮尾形：0 尖尾 / 1 空缺 / 2 无 |
| `anchor_vk` | 指定锚定键的虚拟键码（默认 74 = J） |
| `hold_enabled` / `hold_vk` | 锚定后模拟按住的键 |

谱面文件夹记在 `%LOCALAPPDATA%\RizFalling\chart_dir.txt`。日志在 `native.log`。

## 编译

要求：Windows + MinGW-w64（gcc/g++），链接 `-ld3dcompiler`（系统自带
`D3DCOMPILER_47.dll`）。

```bat
cd native
build.bat
```

`gcc/g++` 不在 PATH 时，设置 `MINGW_BIN` 指向 MinGW 的 `bin` 目录：

```bat
set MINGW_BIN=C:\msys64\mingw64\bin
build.bat
```

产物 `native\d3d11.dll` 复制到游戏目录，重启游戏。

## 结构

```
native/
├─ dllmain.cpp       d3d11 代理导出转发 + DllMain 启动线程
├─ overlay.cpp       Present hook、ImGui 渲染、INS 菜单、INI、谱面加载
├─ chart_loader.cpp  .osz(zip) → .osu 解析（miniz）
├─ audio_align.cpp   WASAPI loopback + stb_vorbis + onset/NCC 自动对齐
├─ stb_impl.cpp      stb_vorbis 实现
├─ stb/              stb_vorbis（public domain）
├─ imgui/            ImGui（MIT）
├─ miniz/            miniz zip/inflate（public domain / MIT）
└─ build.bat
```

## 免责声明

本项目通过替换游戏目录下的 `d3d11.dll` 实现进程内叠加，属于非官方修改手段，
可能违反游戏用户协议或触发反作弊/杀软误报。使用风险自负，作者不对账号或数据损失负责。
请勿将本项目用于商业用途或分发任何游戏资源。

## 致谢

- [Dear ImGui](https://github.com/ocornut/imgui) — MIT
- [miniz](https://github.com/richgel999/miniz) — public domain / MIT
- d3d11 代理 + Present hook 思路参考了社区常见做法

## License

MIT — 见 [LICENSE](LICENSE)。
