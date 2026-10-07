# BACKLOG

按方案 §15/§16 的路线，V0.1 之后的顺序。每条都写了"怎么算做完"，避免做完了没法判。

## V0.2 可用性

1. **图像壁纸预设**（`renderer: "image"`）：挑一张 JPG/PNG 铺满/裁切/居中。
   判据：给一张已知尺寸的图，回读表面角点像素与图的中心像素一致。
   **铺展方式已做**（2026-10-06：`config.json` 的 `image_fit` = fill/fit/stretch/center/tile，
   走 `uPerf.w`，设置 → 通用 那颗下拉）。判据是按 800x800 夹具量黑边，见 README「壁纸铺展」。
   **未量的只剩桌面那一层**：省电停帧时抓屏拿到的是过期帧，量不了。
   包内声明 `renderer: "image"` 仍未做（图片包现在靠 `shader: "Image.hlsl"` 认）。
2. **设置界面的点击链路**（Qt UI 本体已完成）。要么把自绘控件暴露给 UIA（现在 `Accessible.role` 加了
   但 UIA 枚举不到），要么用 `SendInput` 真实点击并断言 `--ctl status` 的字段变化。
   判据：点 Embers 的"应用到目标"后，`status.monitors[0].wallpaper == "embers"`。
   试过 `SendInput` 真实点击：前台是全屏游戏客户端时，点击被游戏吃掉（`SetWindowPos(HWND_TOP)`
   越不过全屏窗口），所以模态框"点开"这一步仍未验证。要么等他桌面空闲时再点，要么给界面加一个
   `--preview <id>` 启动参数直接把模态框打开来验渲染。→ 已用 `--preview aurora` 验过模态框渲染
   （`build/shots/modal_open.png`），但"鼠标点卡片→打开"仍未验；另外 `动态预览` 按钮的按下也没有
   程序化验证，只验了 8 帧内容互不相同。
   **2026-10-06 补：`PostMessage` 三个鼠标消息给窗口自己的队列，绕过了这个问题** ——
   全屏 topmost 游戏客户端在场、`WindowFromPoint` 两次拒绝发 `SendInput` 的情况下，
   `tools/guarded-click.ps1 -Post` 点开了「壁纸铺展」下拉并选中"适应"，
   断言到 `--ctl status` 的 `image_fit` 变成 `fit`（渲染器日志 `command fit(fit,)`）。
   剩下没验的还是那几处：卡片→打开模态框、`动态预览` 的按下。
3. **视频壁纸**（H.264 → 自驱 MFT → NV12 双平面 → `Shaders/Video.hlsl`）。**2026-10-07 09:20 已经出画，并且是真合成的像素量过的。**

   量到的（`build/fixtures/fixture_1080.mp4` / `fixture_2160.mp4`，桌面 M0 = 3840x2160，`state=background`、
   档 24 fps；读回工具 `tools/preview-pixels.py`，预览共享内存和桌面截图各查一遍）：
   - **颜色**：8 条饱和色条最大 `|Δ| = 2/255`（错用 BT.601 矩阵时红条差 ~54），11 格 studio-swing 灰阶带
     每格 `+1`。两把尺子分别判"矩阵 + 行序"和"平面几何"——**只看色条查不出纵向色度偏移**，色条带在 y 上是平的。
   - **在动**：底部白块两帧质心移动 4–5 px。（`corner_samples_*` 对这个夹具无效：它四条角本来就恒定。）
   - **代价**：1080p30 满速交付（`video_frames_delivered` +156/5 s），进程 CPU（100% = 1 核）16–40%，
     工作集 ≈410 MB；4K30 同样满速、`late_frames=0`，CPU 43–83%（均值 ≈62% 单核），工作集 ≈873 MB。
     采样窗口里有并发会话在同一进程内建预览/缩略图，所以别把这些数当纯解码。
   - **控制**：`--ctl video pause` 后计数冻结、`resume` 恢复；`seek` 是 **codec-accurate**——落在前一个关键帧
     再向前解（seek 2.0，两秒后读到 3.37）；seek 超过时长会被夹到末尾，然后正常循环回 0。

   判据改过一处。原来的"CPU 占用不随分辨率上涨"在 MF 路线上本机做不到：`MFTEnumEx(HARDWARE)` 只发现
   **NVIDIA MJPEG 解码器**（NVENC 注册给 MF 了，NVDEC 没有），且 Source Reader 的 `SetCurrentMediaType` 在
   RGB32/NV12 × 带/不带 DXGI manager × 精简类型/克隆原生类型 全部回 `0x80070057 pdwResult=0`
   （`tools/mkprobe.cpp`，一条命令 `tools/mkprobe-build.bat` 可重跑；MP4 本身**能识别**，demux、时长、帧率都正常）。
   所以现在是"reader 只作解复用 + 手工驱动 `Microsoft H264 Video Decoder MFT`"，判据换成上面那组实测数；
   枚举仍按**硬件优先**，换一家把硬解注册给 MF 的机器（Intel 核显）同一份代码就走 GPU 解，不用改代码。
   主流的"MF 解码 + 自己转色"确实不是常规做法：FFmpeg 在 Windows 上走 NVDEC/CUVID、D3D11VA、DXVA2 而不用
   MediaFoundation 解码，mpv/VLC 同理，Lively 的视频后端是 mpv(默认)/libVLC/VLC/WMF 并把自己的子窗口 HWND
   交给播放器渲染。要 NVDEC 就得引外部解码栈。**许可这条更正过一次**：mpv=GPL、libVLC=GPL/LGPL 没错，
   但 FFmpeg 可以按 LGPL 构建（不加 `--enable-gpl`）而 NVDEC/CUVID 仍然可用——它只依赖 MIT 的
   `nv-codec-headers`，我们只用解码不用 x264 那类 GPL 组件，所以约束是"动态链接 LGPL DLL + 随包给源码
   获取说明"，比引 mpv 轻一档。

   **驱动细节里三条会再咬人的，写在代码注释里也记在这儿**：`MFT_INPUT_STREAM_INFO.cbSize` 是**对齐余量不是上限**
   （按它分配会把 8434 字节的样本截成 4096，一帧都解不出）；`ProcessInput` 回 `MF_E_NOTACCEPTING` 时必须
   **排空输出后重试同一个样本**（丢掉就永远丢了一个参考帧）；编码面高度容器谎报（写 1080 实为 1088），
   只能从流变更后 `GetOutputStreamInfo().cbSize` 反推，且该 MFT 交的 NV12 是**倒序行**。
   夹具工具的两个坑：Sink Writer 不 `Finalize()` 就没有 `moov`（报 `MF_E_UNSUPPORTED_REPRESENTATION 0xC00D36C4`，
   看起来像"这台机没解码器"），而 `Finalize()` 在 STA 控制台线程会**死锁**——muxer 完成事件要投回创建它的套间，
   改 `COINIT_MULTITHREADED` 立刻正常。一条命令重建夹具：`bash tools/make-fixture.sh`。

   **2026-10-07 10:40 又量掉三块开销（他报"435.73% / 2457 MB，还会卡"）**：
   - **预览换目标没停上一个实例** → 每预览一个视频就留一个全速解码器，浏览一页攒到 15 个。
     `tools/thread-cpu.ps1` 实测 **154 线程 / 逐线程求和 846% 单核 / 2527 MB**（`status` 自报 1221%）。
     改成"换目标即停媒体 + 视频实例不进预览缓存"（切走和关预览两处都丢），同样浏览条件下 **82 线程 / 290% /
     806 MB**，此时桌面正放着一张 4K60。
   - **导入一张 4K 会冻桌面 3.78 秒**：视频的缩略图在循环线程上做 26 次 seek+解码+回读，而 24 张动图里 19 张
     是同一帧（解码落在前一个关键帧，被去重丢掉）。视频包现在只出静帧 + 大图（动图交给实时预览），
     **3.78 s → 1.06 s**。仍然在循环线程上，那 1 秒还会看见。
   - **一条"优化"反例记着别再做**：把 `ProcessOutput` 的输出样本改成复用（省下 4K 每帧 12 MB 的分配）会被这个
     MFT 留住样本，之后每次调用回 `0x80004005`，而取帧循环只在成功时节拍 → 空转到 1900% 单核、只出 1 帧。
     已撤回，并给"这一步没出帧"加了退避（连续 4 次空手就等一个帧间隔）+ 一次性告警。

   单张片的真实代价（同一构建、关预览、按"有没有视频"的差值算，工作集是高位水位不能直接读）：
   | 桌面内容 | 线程 | 工作集 | 进程 CPU（100% = 1 核） |
   |---|---|---|---|
   | 无视频（snowfall） | 68 | 118 MB | ≈2 % |
   | 1080p30 | +14 | +164 MB | ≈30-40 % |
   | 4K60 | +17 | +696 MB | ≈200-290 %（软解 MFT 内部并行，摊在 ~15 个 MF 线程上） |
   内存基本都在 MF 解码器的面缓冲池里（我们的只有 ~3 份整帧拷贝 + 2 张平面纹理）。**没隔离成功的一条**：
   并发会话的设置窗每秒抢一次预览通路，所以"桌面上只放一张片、别的都不动"的纯 CPU 数没拿到，上表是带它一起跑的量。

   **还没做的三件：**
   - **设置界面的视频入口**：导入按钮、播放/暂停、进度、`本地视频` 分类下的卡片文案，一行都没动。
     现在只能用 `--ctl addvideo "<路径>"`（导入时用 MF 探分辨率/帧率/时长写进 `wallpaper.json`）。
   - **只支持 H.264**：解码器按 `H264 → NV12` 枚举，所以 HEVC/VP9/AV1/MKV 会被 `addvideo` 的探测放过
     （探得出分辨率和时长）而在 `Open()` 时才失败。应该在导入那一步就按 subtype 拒绝并给中文文案，没做。
     4K 工作集 ≈873 MB 也偏大：解码帧和两份转置临时缓冲都按整帧反复分配。
   - **`parked/` 里他那两张真视频**（`bld/bin/RelWithDebInfo/parked/local_02`、`local_03`）清单里的 `shader`
     还是 `Image.hlsl`。引擎现在对视频包**强制**用 `Video.hlsl` 并 Warn，所以挪回 `Wallpapers/` 就能播；
     老清单没有 `bt601` 这个参数，缺省按 BT.709 解（HD 的现实默认）。

## V0.2.1 设置界面还缺的

- ~~开机自启~~ 已做（HKCU Run 单值，默认关）；未验：注销后重新登录是否真的起来
- 壁纸缩略图已做（离屏渲 still/large/8 帧）；未做：壁纸包自己声明预览时间点（现在固定 t=2.0s 起 8 帧）
- 本地图片已做（`addimage` + `Shaders/Image.hlsl` 覆盖裁切 + Ken Burns）；未做：删除/重命名导入项、批量导入
- ~~`--select <id>`~~ 已加并截图自证（`build/shots/drawer4.png`）
- 每显示器分别设帧率上限（后端 `assignments[].fps` 支持，界面只给了全局上限）
- **任务栏透明度**已做（2026-10-07：`tray_alpha` + 壁纸页那颗滑条，见 README「任务栏透明度」）。剩下的三件：
  副屏那条 `Shell_SecondaryTrayWnd` 没量过（本机一块屏）；`Engine/Desktop/TrayTransparency.cpp` 里
  日志标签的一处修正（用户从 0 打开时不再谎报"shell rebuilt"）**还没编进跑着的那个 exe**——当时树里有
  第二个会话的渲染器在跑，`tools/build-bld.sh` 自己 abort 了，没去抢；"只淡背景、图标保持清楚"这条
  实测做不到（三条外部路子都会连图标一起淡），要它就得进 explorer 进程里改。

## V0.3 视觉

4. ~~**GPU 粒子**~~ 已做（`Wallpapers/snowfall` + `Engine/Graphics/ParticleSystem.cpp`，见 README
   "GPU 粒子壁纸"）。原判据"10k 粒子时进程 CPU 与 0 粒子时差 < 1%"**这台机器的 CPU 量具做不到**：
   同一次运行里 `cpu=` 就在 0/1.4/3/6 % 之间跳，噪声比两张壁纸之差还大；换用帧时表量
   （25 万粒 2px 与 6000 粒同帧时，放大到 140px 才掉到 67 FPS）。剩下的：
   - `particles.count` 不随质量档缩水（battery 档应当砍过绘制，不是砍数量——实测数量不花钱）
   - 每秒一次的结构化缓冲回读是整块拷贝（25 万粒 = 12 MB），要变成只在启动几秒内跑，或只哈希前 N 条
   - 粒子壁纸没有第二条：再加一张（雨/萤火/灰尘）才能验证契约不是只为 snowfall 写的
5. **质量档真正降分辨率**：离屏 RT 按 `QualityScale` 渲染 + 一次 blit 上采样。
   判据：`low` 档下 GPU 占用明显低于 `ultra`，且帧时抖动变小。
6. **后处理链**（bloom / color grading），壁纸包可选开启。

## 已知隐患

- **一个陈旧的预览目标会把"渲染器崩一次"变成"重启永远失败"**（2026-10-07 实测）。`local_02` 在库里时，
  渲染器一起来，设置窗那一秒轮询发现 `preview.on=false` 就自动重发预览
  （日志 `command preview(local_02,) -> {"ok":true,"preview":"on"...}`），于是**同一个崩溃每次开机都再撞一遍**：
  08:56:27 / 08:57:14 / 09:00:17 / 09:08:34 四次 WER 同一签名（Faulting module `nvwgf2umx.dll`，`0xc0000005`，
  引用文件 `Wallpapers\local_02\video.mp4`）。**崩溃本身的根因不在这里**——那是 MF 自驱路径上我们自己的
  两个内存错（重复 `Release` 造成 double free，几秒后才在 MF 里炸出来，见 [[media-foundation-mft-traps]]），
  当天已由并行的会话修好；驱动模块名只是崩落点。**这条欠账单独留着，因为它讲的是另一件事**：UI 的
  "预览断了就重发"没有任何失败计数，所以只要某个包能让渲染器崩一次，它就会把"重启渲染器"这个动作本身
  变成崩溃循环。两个方向没定：渲染器启动后前几秒不接受预览请求，或 UI 重发前先查这个包还在不在、
  并对连续崩过的包拉黑。当时为了把桌面拉回来，把 `config.json` 的 M0 从 `local_02` 改成 `snowfall`，
  原文件留在 `bld/bin/RelWithDebInfo/config.json.bak-0924`。
- ~~双实例抢管道~~ 已修：`Local\SmartWallpaper.Renderer` 互斥体，第二个实例直接退出
  （实测起两个只剩 1 个）。未做：崩溃后自动重启。
- **present 没有和显示对齐**：壁纸走 `IDCompositionSurface::BeginDraw/EndDraw` + `Commit`
  （`Engine/Graphics/D3D11Renderer.cpp:73-147`），全仓库没有一个 swapchain（`allow-tearing` 只在
  `D3D11Device.cpp:48` 打了一行，没被用过）。所以每帧是把整块 3840x2160 BGRA 表面交给合成器搬一次，
  而不是翻转；相位也全靠 QPC 定时器。2026-10-07 先把**速率**吸附成整刷新周期（见 README「帧率吸附」，
  实测 60→48=3 个周期、`late_frames=0`），**相位和整块拷贝这两件还没治**：换 flip 模型 swapchain +
  `CreateSurfaceForSwapChain`，并用 present 等待对象当时钟。判据：同一块屏上 `nvidia-smi` 占用 A/B
  下降 + `late_frames` 不升。未量：拷贝到底占多少（需要前后各 20 秒同窗口）。
- **每秒一次的同步 GPU 回读夹在 `Render` 和 `EndDraw` 之间**（表面还锁着）：
  `RenderSurface::SampleCorner`（`D3D11Renderer.cpp:129`，`Map` 的 flags=0 = 等 GPU 交完）、
  `ParticleSystem::Sample`（`ParticleSystem.cpp:79-90`，整块 `CopyResource`（25 万粒=12 MB）+ 逐字节
  FNV）。这次没牵连上：当前壁纸 `particles=0`，且 103 个采样秒里掉速秒为 0。修法照 `DrawPreview`
  已有的 query + 双槽异步取帧，并把粒子回读限制在启动几秒内（`--ctl status` 的
  `particle_*` 只在头几秒有意义）。

## 未验证项（V0.1 欠的账）

7. 第二块显示器：多屏各自帧率预算、各自遮挡判定、拔插重建。这台机器只有一块屏。
8. 锁屏 / 关屏 → 0 FPS 的实测（通知注册成功但没真触发过）。
9. 桌面图标：可见性、单击、双击都已实测（见 README 的"输入归属"），Z 序也量过。
   **只剩"手动拖图标"这一条要他人确认**：合成输入（`mouse_event` 和 `SendInput` 绝对坐标都试过）
   启动不了 shell 的拖拽，而且**关掉壁纸的对照组同样启动不了**，所以这条测不了不是壁纸的问题。
10. explorer 重启后的重挂：`WorkerW::Recheck()` 有实现，但没实测过"结束 explorer 再启动"。

## 已知代码味道

- `Application::Run()` 里循环探针（`loopIters_`/`LoopDebug`）是调试期加的，稳定后应降级到 trace 级或删除。
- `Shaders/Fullscreen.hlsl` 的 `Fbm` 用 `[loop]` 动态循环，低档壁纸应改用编译期常量八度。
