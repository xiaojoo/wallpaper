# Wallpaper — Windows 动态壁纸引擎 V0.1

按方案落地：`C++20 + Win32 + Direct3D 11 + DirectComposition + HLSL`，渲染引擎与设置 UI 分离。
渲染器已跑通并量过；Qt/QML 设置界面、GPU 粒子已做；视频壁纸能播（H.264 / HEVC 走 D3D11VA 零拷贝，
VP9 / AV1 走 cuvid，设置界面能导入也能在线下载预览）、3D 与后处理还没做（缺点见 `docs/实现与量测.md`，
待办见 `BACKLOG.md`）。

## 技术栈

| | |
|---|---|
| 语言 / 标准 | C++20，MSVC（Visual Studio 2022），`CMAKE_CXX_STANDARD 20` |
| 构建 | CMake ≥ 3.24，`-G "Visual Studio 17 2022" -A x64`；`stage` 目标把 `Wallpapers/` 与 `Shaders/` 拷到 exe 旁边 |
| 图形 | Direct3D 11 + DirectComposition 表面 + DXGI 呈现；着色器 HLSL（`fxc`，`cs_5_0` / `ps_5_0`），运行时编译并按反射绑参数 |
| 桌面接入 | Win32：Progman / WorkerW 层级定位，每个显示器一路窗口 |
| 设置界面 | Qt 6.11（`msvc2022_64`）：Quick / Qml / Widgets / Network；控件全自绘，`ShaderEffect` 的 .frag 用 `qsb` 预编译 |
| 视频 | FFmpeg 的 **LGPL shared** 构建，pin 在 `n9.0`（BtbN/FFmpeg-Builds）；硬解优先（h264 / hevc 走 d3d11va 显存面，vp9 / av1 走 cuvid / libdav1d）。Media Foundation 那条后端已删除，FFmpeg 是唯一一条且是编译硬依赖 |
| 音频 | 可插拔：引擎**不链接** `swaudio_wasapi.dll`，运行时按 `Engine/Audio/IAudioPlugin.h` 的 C 契约加载；解码走 FFmpeg + `swresample` |
| 机器指标 | NVML（运行时加载，手写入口点声明）+ PDH；取不到的那一格直接不画，不影响壁纸 |
| 进程之间 | 命名管道 `\\.\pipe\SmartWallpaper.Renderer`，一行一个 JSON；预览帧走共享段 `Local\SmartWallpaper.Preview` |
| 在线壁纸源 | 站点是数据：`Online/sources/*.json` 写端点与解析规则，程序里没有一家站的硬编码 |

详细一点的实现说明、这台机器上的实测数、以及已知缺点，都在 `docs/实现与量测.md`。

## 编译

```
cmake -S . -B bld -G "Visual Studio 17 2022" -A x64
cmake --build bld --config RelWithDebInfo
```

产物 `bld/bin/RelWithDebInfo/WallpaperRenderer.exe`，`Wallpapers/` 与 `Shaders/` 每次构建自动
拷到 exe 旁边（`stage` 目标，改 .hlsl 不用重编 C++）。

视频解码只有 **FFmpeg** 一条路（Media Foundation 那条 2026-10-07 删掉了，见 `docs/实现与量测.md` 的「视频解码后端」），
所以它是**编译的硬依赖**：找不到 SDK 时 `configure` 直接失败，并把该跑的那条命令写在错误里。

```
bash tools/get-ffmpeg.sh        # 下载并校验 pinned 的 lgpl-shared 构建到 build/ffm-lgpl
bash tools/ffmpeg-notice.sh     # 由真实二进制生成 licenses/FFmpeg.txt（分发的随附说明）
```

CMake 找 SDK 的顺序：`-DWALLPAPER_FFMPEG_DIR=<路径>` → 环境变量 `FFMPEG_DIR` → `build/ffm-lgpl`。
两个许可证文件（`LICENSE-FFmpeg.txt` 与 `NOTICE-FFmpeg.txt`）由构建自动拷到 exe 目录，跟着程序一起分发。

## 运行与控制

```
WallpaperRenderer.exe                          双击/命令行直接开跑，读同目录 config.json
WallpaperRenderer.exe --selftest 20            跑 20 秒自动退出并把测量结果写日志
WallpaperRenderer.exe --wallpaper snowfall      指定壁纸
WallpaperRenderer.exe --window                 普通顶层窗口，不碰桌面（开发用）
WallpaperRenderer.exe --no-power               忽略遮挡/全屏降帧（开发用，量帧率时才用）

WallpaperRenderer.exe --ctl status             读运行中实例的真实状态（JSON）
WallpaperRenderer.exe --ctl apply matrixclock M0  换壁纸（M0/M1/all）
WallpaperRenderer.exe --ctl pause | resume | reload | quit
WallpaperRenderer.exe --ctl audio on 6           测音频后端：开设备、放 6 秒提示音（`off` 停并交还设备，
                                                 `auto` 交还给壁纸自己决定，光 `audio` 读回状态）
WallpaperRenderer.exe --ctl volume 25            环境音音量（0..100，实时生效并写回 config.json）
WallpaperRenderer.exe --ctl quality battery
WallpaperRenderer.exe --ctl fps 30
WallpaperRenderer.exe --ctl fps 30 M0            只给这一块屏设上限
WallpaperRenderer.exe --ctl rotate on | off | interval 30
WallpaperRenderer.exe --ctl rotate next | prev   手动走一张（走的是 rotate.pool，池空时用全部壁纸）
WallpaperRenderer.exe --ctl autostart on | off   登录自启（HKCU Run 单值）
WallpaperRenderer.exe --ctl hud on | off         资源状态卡：引擎画在壁纸之上的那一层
WallpaperRenderer.exe --ctl hudpos top-right     卡片放哪个角：top-right|top-left|bottom-right|bottom-left
WallpaperRenderer.exe --ctl hudscale 120         卡片大小（60..250，100 = 参考图的尺寸）
WallpaperRenderer.exe --ctl hudframe notch       卡片边框样式：notch|basetab|ticks|topbar|band|split
WallpaperRenderer.exe --ctl huddrag on           卡片上盖一个虚线框，按住就能在桌面上拖；松手即生效并自动关掉
WallpaperRenderer.exe --ctl hudmove 0.42 0.31    直接把卡片放到那里（单位：屏幕高，从这块屏左上角量）
WallpaperRenderer.exe --ctl hudhome              取消拖动的位置，回到上面 hudpos 指定的那个角
WallpaperRenderer.exe --ctl chart on | off         图表面板：第二块叠加层，独立于状态卡
                                                 四条 = CPU占用 / GPU占用 / 内存占用 / 硬盘温度
                                                 （温度只有 NVMe 答得上，答不上那一格不画）
WallpaperRenderer.exe --ctl charttype bar          图表类型：bar 柱状 | donut 环形 | line 折线
WallpaperRenderer.exe --ctl chartpos bottom-right  面板放哪个角（同 hudpos 的四个名字）
WallpaperRenderer.exe --ctl chartscale 120         面板大小（60..250）
WallpaperRenderer.exe --ctl chartframe notch       面板边框样式（同 hudframe 的六种）
WallpaperRenderer.exe --ctl chartdrag on           虚线框出现，按住可拖；松手即生效并自动关闭
WallpaperRenderer.exe --ctl chartmove 1.3 0.4      直接把面板放到那里；charthome 取消
WallpaperRenderer.exe --ctl addimage "D:\pics\a.jpg"   把一张图片变成壁纸包
```

控制通道是命名管道 `\\.\pipe\SmartWallpaper.Renderer`，一行一个 JSON。以后 Qt 设置界面走同一条管道，
不需要改渲染器。两处托盘图标：渲染器自己那颗（`config.json` 的 `tray`，默认关）带壁纸/显示器/质量/最大帧率/
暂停/重载/退出；设置窗那颗带 退出 / 下一张 / 上一张 / 自动播放 / 停止播放 / 暂停壁纸 / 暂停图标（那一节的细节在 `docs/实现与量测.md`）。

日志：`logs/renderer-<pid>.log`；配置：exe 同目录 `config.json`（首次运行自动写出）。

## 声明（第三方与许可）

- **本项目自己的许可证还没有**：仓库里只有 `licenses/FFmpeg.txt`，根目录没有 LICENSE 文件。
  这份代码要发出去之前得先补上，别把"没有"当成"默认允许"。
- 视频解码用 FFmpeg 的 **LGPL** 构建：不复制任何 FFmpeg 源码进本仓库，只通过公开头文件链接它的 DLL。
  `tools/get-ffmpeg.sh` 见到 `--enable-gpl` / `--enable-nonfree` 的构建会拒绝下载。
- 随程序分发的说明由**真实二进制**生成，不要手改：`bash tools/ffmpeg-notice.sh` → `licenses/FFmpeg.txt`；
  构建再把 `LICENSE-FFmpeg.txt` 与 `NOTICE-FFmpeg.txt` 拷到 exe 目录，跟着程序一起走。
- 用的是哪个 FFmpeg 可以从运行中的程序读回来：`--ctl status` 的 `ffmpeg` 字段来自
  `av_version_info` / `avcodec_version` / `avcodec_license`，换了 DLL 它跟着变。
- 音频后端 `swaudio_wasapi.dll` 是本仓库自己写的，运行时加载：删掉它壁纸照跑，只是不出声。

## 目录

```
Engine/Core        日志、JSON、Win32/D3D 头收口
Engine/Graphics    D3D11 设备、着色器编译+反射、WIC 纹理、DirectComposition 表面与绘制
Engine/Desktop     显示器枚举（物理像素/DPI/刷新率）、WorkerW 层级定位、每显示器窗口
Engine/Packages   壁纸包、实例（编译后）、配置与分配
Engine/Performance 帧率分配（每屏一路）、降功耗状态机
Engine/App         应用外壳、托盘、命名管道服务端/客户端
Wallpapers/        matrixclock、snowfall（GPU 粒子）。2026-10-11 他点名只留这两张，其余十张
                   （aurora、binarydrift、datarain、embers、machinedrift、neonfall、ironwalk、
                   ironstand、ironline、ironwire）已删除。下面各表里以那些名字出现的读数都是
                   删除之前量下的历史证据，保留原样，照做会复现不了。
Shaders/           Contract.hlsl（帧常量 + 噪声/色调工具）、Fullscreen.hlsl（全屏三角形）、
                   ParticleCommon.hlsl（48 字节粒子布局）
tools/             构建、桌面层级探针、屏幕截图与逐像素对比
```

配套文档：`docs/实现与量测.md` 是实现的细节、这台机器上的实测与已知缺点（2026-10-16 从本文件搬出去，
逐字未改）；`BACKLOG.md` 是待办，每条写了"怎么算做完"；`问题与解法.md` 是过程记录
（症状 → 走过的弯路 → 根因 → 解法 → 判据 → 遗留），量具自己的坑也记在那一份里。
