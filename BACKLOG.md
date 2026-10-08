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
     **3.78 s → 1.06 s**。2026-10-07 深夜重量：那个 1.06 s 是**抓到的是一幅空平面**的代价（见下面那条
     `ShowFrameAt` 根因），真的走到 t=2.0s 之后是 **521 ms**（3440x1440 那条）/ **699 ms**（4K 那条）每片，
     大头是从前一个关键帧快进到目标的那 ~120 帧解码。仍然在循环线程上，这一秒还会看见。
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

   **2026-10-07 13:47 卡顿定位：真正的可见抖动是"循环接缝"，不是解码吞吐。**
   - 量法：`build/tmp/dwell-ruler.ps1`（CopyFromScreen+LockBits+FNV，143.8 Hz、分辨 7 ms，量**真合成**的桌面）。
     先标定：中位停留 **20.86 ms = 144 Hz 的整 3 个刷新周期**。判据 = 离群是否与片长同周期。
     两次独立 14 秒窗口各数到 **3 次离群 145.7–167.7 ms**（片长 4.7 s ⇒ 正好 3 次）= 每次循环画面冻
     **7–8 倍于正常停留**。
   - 拆开（`build/tmp/seamcost.cpp`，离线复刻 `SetCurrentPosition`→`FLUSH`→喂到第一帧，隔离态 6 次）：
     **seek 7.5–19 + flush 66.5–104.5 + 重 priming 117–129 ms（36–37 步全 NEED_MORE_INPUT）**，其中
     **读文件只占 2.6–3.5 ms**（把片头 120 个压缩样本先放进内存再走同一条接缝，pump 仍然 118.6 ms ⇒ 盘不是成本）；
     不显式 flush 则 33.7 ms 就出画 ⇒ 代价全在 flush + 流水线深度。**接缝与帧率档无关**（三档 15 s 窗口都是 3–4 次）。
   - 因果确认不用改码：`--ctl video seek 1.0` 之后**同一秒**的 `[video]` 行立刻出现
     `steps 89 / empty 37 / max 72.6 ms`，干净 3 秒后（= 片长 − seek 点）自动重复一次。
   - 探针忠实性用稳态数字交叉验：每帧 `read 0.05 / input 0.00 / ProcessOutput 2.12 / 自己的帧复制 2.61 = 4.85 ms`
     对上活引擎 `[video]` 的 `step mean 4.8`。**所以换 NVDEC 治不了这 146–167 ms**（那是吞吐，不是 flush）。
     未圆过去的一条：探针隔离态总接缝 204–239 ms > 抓屏 146–167 ms，差 40–70 ms，原因没查（候选：取样块偏暗
     把"第一次真的变了"读漏；或活进程的 MF 缓冲池已经热过）。

   **2026-10-07 13:57 这条已经治掉：循环点不再 flush。** 依据是 `build/tmp/seamcost.cpp` 里新增的
   `LOOP WITHOUT FLUSH` 实验：只回绕 demuxer、让流水线保持暖的，rewind 后**第 #37 个输出**才是片头第 0 帧
   —— 前 36 个正是旧 flush 路径每圈丢掉的片尾；而那三帧片头与冷启动解出来的**逐字节相同**
   （每帧 12,441,600 B，worst |delta| = 0）。H.264 的 IDR 本来就刷新全部参考帧，所以这是规范内的做法。
   实现：EOS 且 loop 时只 `SetCurrentPosition(0)`，喂进去的时间戳按圈重定基（`bias_ += (duration+2)s`，
   保持单调），`position_` 用输出 ts 减掉**对应圈**的 bias 反算（流水线里那 36 帧还带着上一圈的 bias）；
   显式 seek 仍然 flush 并把 bias 归零。量到的结果（桌上那张 4K60，20 秒 = 4 圈以上）：
   **抓屏最坏停留 167 → 48.6 ms，">2.5×中位"的离群从每圈一次降到 0 次**，中位 20.82 ms 不变、
   `late_frames=0`、交付仍 60/秒、`render took 47-48`。seek / pause / resume 三条都复验过
   （`seek 2.0` 读到 `pos=2.90`，正是既有的 codec-accurate 行为，说明重定基没把走带搞乱）。
   **残留没解释的一条**：抓屏尺子在这块左下 240×140 区域会读出 13.8 / 14.3 ms 这种**小于一个刷新周期整数倍**
   的停留；退回原始代码后同样存在，所以不是这次改动带进来的，p90 因此不当结论用。

   **2026-10-07 18:12 更正一条被两把错尺子互相掩盖的缺陷：MF 路线一直在垂直翻转画面。**
   起点是 FFmpeg 后端接进同一条管线后，`tools/preview-pixels.py` 把 8 条饱和色条全读成 (129,128,129)，
   而 11 格灰阶带读对 —— 那不是色度丢了，是**画面上下颠倒**（夹具的色条带和灰带被互换了位置）。
   三个互不相干的仪表定的案：
   - 外部参照：`build/ffm-lgpl/bin/ffmpeg.exe` 把夹具解成 PNG（PNG 行序无歧义）⇒ 夹具文件里存的是
     **色条在下三分之一**，而 `preview-pixels.py` 的期望值写的是色条在上。
   - 原因在生成器：`tools/mkfixture.cpp` 给一张自顶向下的 BGRA 声明了**负** `MF_MT_DEFAULT_STRIDE`
     （那行的注释断言"负 stride = 行自上而下"，方向正好说反），Sink Writer 于是把整片倒着编码。
   - MF 的真实行序：`VideoPlayer::ProbeRowOrder` 直接量解码缓冲里第一/最后一条色度行的 |chroma−128|
     （这块夹具上实测：灰带 **1**、饱和色条 **256**）⇒ MFT 交回的是**自顶向下**，而 `Nv12Uploader`
     一直按 bottom-up 翻一次。
   两个错各自独立、相加正好抵消，所以 09:20 起"色条 Δ≤2"一直是绿的：那条绿描述的是夹具**想做**的布局，
   不是文件**实际存**的布局。`tools/mkdecode.cpp` 里"top 行读到灰 ⇒ 解码器是 bottom-up"就是把意图当成了
   事实，注释已就地更正。
   修：夹具改正 stride 并重新生成（外部参照现在色条在上）；`Nv12Uploader` 删掉 `srcBottomUp` 参数，
   两个后端一律按自顶向下做 coded→display 裁行；`ProbeRowOrder` 当时留作常备仪表（每个播放器首帧一行）——
   **它随 MF 一起删了（见下面 20:50 那条）**，现在判行序的常备读数换成夹具自己的头 8 行白条 / 尾 8 行黑条。
   修后同一把尺子（色条最大 |Δ| / 灰阶误差 / 通道裂格数 / 白块两帧位移）：
   **MF 2 / +0..+1 / 0 格 / 4–8 px，FFmpeg(sw) 2 / +0..+1 / 0 格 / 4–8 px，FFmpeg(nvdec) 2 / +0..+1 / 0 格 / 8 px**。
   两个后端在静态区域（上 2/3，614,400 px）逐像素比对：**77 px 不同、最大 2/255**（H.264 舍入噪声级；
   同一比特流两个 conform 解码器本就该 byte-exact）。
   行序和 coded→display 裁行还有一把现成的尺子，之前没人用它：夹具自己在**头 8 行画白条、尾 8 行画黑条**
   （`DrawFrame` 的 `edge`）。修后两条预览表面读回都是 y=0..4 → **255**、末 4 行 → **1**，而且 MF 那一行
   正是 coded 1088 / 显示 1080 的那个包，所以"多余行在底部、取前 1080 行"也是量过的，不是惯例。
   **判据加一条**：以后"颜色对了"必须同时报"没颠倒"，判据是色条带落在上还是下三分之一，不靠人眼。
   nvdec 那一行第一次量出来是错的（色条读成暗蓝照片），`status.preview` 却仍报 `on / local_02`。
   **我当场给的"那块段有两个写者"的解释是错的，收回**：`Local\SmartWallpaper.Preview` 只有渲染器一个写者
   （`PreviewStream::OpenWriter`），设置窗的 `LivePreview` 只读。真因在渲染器自己：
   `Application::DrawPreview` 收 GPU 回读时把帧盖上 **`p.idHash`（此刻请求的那张）**，而预览用的是
   **两块 staging 轮转**、驱动常晚一帧 —— 所以切换后的头几帧其实是**上一张**画面，却被标成新那张。
   设置窗和用户都受害（切壁纸瞬间会闪一下上一张的动画），量具受害更狠：`status` 和段头两个来源
   同时说"这是 local_02"，就没有任何仪器能发现量错了东西。
   修（2026-10-07 19:30）：每块 staging 在**发起拷贝那一刻**记下自己的 `slotHash`，回读时按它盖章；
   新增 `status.preview.draining` = 被写出去但 producer 不是当前请求的帧数。
   量到的窗口大小：**每次 re-target 恰好 1 帧**（连续 5 次切换 → draining 1→6）。
   配套把尺子也补了第二道：`tools/preview-pixels.py` 现在按**段头自带的 idHash**（FNV-1a，与
   `PreviewStream.hpp::IdHash` 同算法）逐帧过滤，不认 `status` 的口头归属 —— 修之前那次实测
   `refused 3 frame(s) stamped datarain`，而同一时刻 `status.preview.id` 正是 `local_02`。
   **还有第二种污染，跟 provenance 无关**：local_02 的预览实例刚建好时解码器还没出帧，那几帧是
   着色器采空平面 ⇒ 整幅 (0,76,0) 均一绿，色条判据当场全红（最大 |Δ| 255）。所以尺子先等
   `status.preview.video_frames > 0`（新字段，非视频包没有这个键就直接过）再采集，
   等待期间反复 `preview <id>` 顺手续上 3 秒心跳。修后同一实验：`channel ready after 0.4 s`、
   色条最大 |Δ| 2/255、灰阶 +0/+1、裂格 0、白块两帧位移 9 px。
   规则：**通道只有一份、副本在飞，那么"归属"必须随帧走，不能靠旁路查询**；
   任何"读一块共享内存"的判据都要问一句"这一帧是谁写的、里面有东西吗"。
   仍未量的只剩桌面那一层：壁纸在应用窗口之下，`CopyFromScreen` 量到的是窗口。

   **2026-10-07 18:31 FFmpeg 路线的循环接缝（MF 那条教训的搬运）**：EOS 之后原本照例
   `av_seek_frame + avcodec_flush_buffers`。4K60 那圈量到 **下一圈第一帧之前 105–118 ms**（稳态步
   3.7–5.6 ms），而且 flush 把还留在解码器队列里的 **15 帧片尾**扔了 ⇒ 每圈少播约 250 ms 内容。
   改成回绕不 flush（IDR 自带刷新，和 MF 同一依据）后：**本圈最后一帧 4.683 s → 下一圈 0.000 s，
   解码侧相隔 3.8–17.7 ms**，步数从每圈掉到 53–54 恢复成稳定 60–61。
   中间反向踩过一次：把那 15 帧当"陈旧帧"丢弃，接缝立刻回到 105–118 ms 且内容仍然缺 —— 与 MF
   "rewind 后第 #37 个输出才是片头第 0 帧"是同一件事：**队列里的是片尾，不是垃圾**。仪表是
   `[video] ... loop seam:` 一行，直接报这两个位置和它们之间的毫秒；`--ctl video seek 2.5` 复验了
   用户 seek 仍然走 flush（pos 读到 2.500，恢复播放后走到 3.65），缩略图的 live-source 取帧 24/24 保留。

   **2026-10-07 19:05 默认后端换成 FFmpeg（NVDEC 优先），LGPL 那条清单补齐。** 决定性的数是桌面上
   那张真实的 4K60 循环片、同一把尺子（`--ctl status` 两次采样之间 10 秒的 `cpu_percent` 与
   `working_set_mb`）、同一轮里各跑一次：
   MF 290–313% 单核 / 779–790 MB / 59.8–60.4 帧每秒；FFmpeg 原生 216% / 556 MB / 54.8；
   **FFmpeg h264_cuvid 37–49% / 299–328 MB / 60.0–60.7**（两次独立窗口，不是单次读数）。
   选择规则：默认先问 cuvid，驱动不给就退原生解码器，文件打不开再退 MF ⇒ 最坏情况等于 FFmpeg 软解，
   仍然优于旧默认；`WALLPAPER_VIDEO=mf` 这条退路复验过（`video_output` 读回 `sw Microsoft H264 Video
   Decoder MFT`）。没编进 FFmpeg 的树照旧走 MF，不报错。
   合规落成了四件可查的东西，全部**从二进制读回来**而不是写死在文档里：
   ① `tools/ffmpeg-notice.sh` 生成 `licenses/FFmpeg.txt`（版本、库自己的许可原文、configuration、
   sha256、上游 commit、"latest 是滚动 tag"这条风险、以及"DLL 可替换"），构建时随 exe 出成
   `NOTICE-FFmpeg.txt`；② 渲染器启动两行日志 + `--ctl status` 的 `ffmpeg` 字段；
   ③ 设置界面 → 通用 最下面两行（解码器名 + 许可说明），中英文表都补齐、并从快照读回；
   ④ 生成器带**两个负面自检**：喂它一个 `--enable-gpl` 的假构建、和一个读不出 configuration 的假
   构建，各自 REFUSE/2 —— 第一版就是因为解析 `-buildconf` 拿到空串而让 GPL 检查**空过**了一轮。
   仍未做：仓库自己没有 LICENSE 文件（他自己那份代码的授权要他定）；HEVC/VP9/AV1 走 cuvid 一行没量；
   NVDEC 的 `private` 比软解高（657–716 vs 607 MB）没查原因。

   **2026-10-07 20:50 Media Foundation 后端删除，FFmpeg 成为编译硬依赖。** 决定性的不是行数
   （`VideoPlayer.cpp` + `.hpp` 共 788 行）而是这一条：**导入那一步 `AddVideoPackage` 用 MF 探测**，
   于是"这个库能收哪些格式"由一个已经不再用的后端决定。删的东西：`Engine/Graphics/VideoPlayer.{cpp,hpp}`、
   `MFStartup/MFShutdown`、CMake 里的 `mfplat mfreadwrite mf mfuuid`、`WALLPAPER_HAVE_FFMPEG` 开关与
   `FFmpegPlayer.cpp` 末尾那份 stub（找不到 SDK 现在编不过，stub 就是死码）、
   `FFmpegAvailable()`（从没被调用过）、以及 `WALLPAPER_VIDEO=mf|ffmpeg|ffmpeg-nvdec` 三个取值。
   同时做/验的：
   - **探测搬到 FFmpeg**（`FfmpegProbeVideo`）：清单字段与 MF 版逐位相同 —— 夹具 `duration_s 9.966633`、
     `1920x1080`、`fps 30`、`src_aspect 1.777778`、`bt601 0`；日志多带一句解码器名
     （`decoded by h264`）。矩阵判据按 FFmpeg 报的 `color_space`（170M/240M/BT470BG/FCC 才算 601），
     与原来"只有声明的 601 族才切"同语义。
   - **只留一个开关** `WALLPAPER_VIDEO=sw`（不问 NVDEC），理由写进代码：NVDEC 是我们不控制的第二方，
     画面坏了时要能在"驱动解码器"和"我们管线"之间判别，不该为重编一次。
   - **CMake 变成硬依赖**，并且两个方向都跑了对照：指到一个空路径 → `configure` 失败并打印它找过的路径；
     不加参数 → 自动检出 `build/ffm-lgpl` 并配置通过。**第一版这里有个真 bug**：错误信息里写
     `-DFFMPEG_DIR`，而代码只读环境变量，`-D` 根本传不进去 —— 已改成 cache 变量 `WALLPAPER_FFMPEG_DIR`
     （env `FFMPEG_DIR` 仍作种子）。
   - 全量构建（两个 exe + `stage`）RC=0；唯一一条 warning C4100 在 `IsAutostartEnabled`，
     删改前的构建日志里同样有 1 条，**净增 0**。
   - 二进制里 MF 真的没了：`mfplat / mfreadwrite / MFStartup / MFEnumDeviceSources / MFCreateSourceReader /
     IMFSourceReader` 在 exe 内全 0 命中，对照组 `avcodec-63.dll / avformat-63.dll` 各 1 命中
     （证明这条字符串判据能读到东西）。
   - 运行时复验：桌面 `local_05` 起在 `nvdec libavcodec h264_cuvid`、0 条 warn/error、
     循环接缝仍是 14.9–17.7 ms、61 步/秒；夹具重新导入后像素门禁仍是色条 2/255、灰阶 +0/+1、裂格 0、
     白块位移 8 px。设置窗重编后正常出图（`tray: icon shown=true` + 抓窗确认非白屏）。
   **删掉的代价，写清楚**：进程内只剩一个解码器，"两个后端逐像素互证"（今天 614,400 px 只差 77 个那种）
   以后做不了了；行序判据从"量解码器缓冲"退成"看夹具自带的头/尾边缘条"（仍是硬读数：顶部 255、底部 1）；
   `tools/` 里那 5 个 MF 工具（含夹具编码器 `mkfixture.cpp`）保留 —— 它们是开发期程序，不进包。
   没有 FFmpeg 的树现在**编不过**，这是有意的：过去那种"照样编出来但视频静默没了"更坏。

   **2026-10-07 21:30–22:40 他一句"当前 CPU 和内存，用 500 多内存，能再优化吗，可以用 GPU 渲染吗；
   客户端展开时鼠标失去焦点动态图就静止了，这个不要"，拆出四件事。**
   - **失焦静止的闸挪对了地方**：`motionWanted` 从 `root.active` 改成
     `root.visible && root.visibility !== Window.Minimized`，并把 `onActiveChanged: syncPreview()`
     换成 **`onVisibilityChanged`**（Qt 最小化时 `visible` 仍为 true，只有 `visibility` 会动；少了这个钩子
     "恢复"就没有重新武装的路径）。实测：改之前（前台换成我自己起的 notepad、窗仍可见）大图 1.5 秒
     **0/724,880 像素变化**、`status.preview.on=false`；改之后同一状态 2 秒 **203,376 像素变化 (28.1%)、
     meanAbsDiff 4.18**、`preview.drew` 2 秒 +80。反证两条都过：最小化 → `on=false`、drew 冻在 35598；
     恢复 → 3 秒 +95 帧、30 fps。**4 秒轮播仍然只在有焦点时走**（它换的是"选中哪张"，不是"动不动"）。
   - **`cpu_percent` 这个仪表以前会说谎，而他看到的就是它**：它是"两次 status 调用之间"的差值，
     而设置窗每秒调一次 —— 所以状态条上那个数是≈1 秒窗口里的一次尖峰。同一稳态三次读出
     168.9% / 29.0% / 18.0%，他截图里那个 177.08% 就是这么来的。改成渲染器自己的 **≥2 秒窗口**
     （窗口不够长就重复上一个完整窗口，绝不掉回 0），并把窗口长度一起报成 `process.cpu_window_s`，
     状态条与 渲染器 页都跟着显示"均值窗口 2.0 s"。修后同一稳态四次 4 秒采样 25.3 / 27.5 / 28.3 / 31.4%。
   - **上传去掉 CPU 中转**：`Nv12Uploader` 不再把每行 memcpy 进 `rows_`/`chroma_`，改成两次
     `UpdateSubresource` 带 box（box 顺手做 coded→display 裁行；行距 16 字节对齐的理由写在注释里）。
     4K60 桌面那一路 `FrameY mean 2.00-2.09 ms → 0.99-1.41 ms`，`transpose` 一项整个消失，日志行同步
     改成只报 upload；`transUs` 字段与从没自增过的 `packUsSum` 一起删掉。判据仍是色条 2/255 +
     灰阶 +0/+1 + 头 8 行白条/尾 8 行黑条在正确一侧（box 写错恰好会被这两条抓住）。
   - **大图与桌面是同一张片时共享解码器**：`StartPreview` 先找一个正在显示该包的活槽实例，
     直接把它再画一遍到预览表面（`p.shared`；粒子包排除，因为绘制路径会步进仿真，走两遍=双倍速）。
     三档对照（4K60 真实片、修好后的仪表、同一心跳条件下）：
     只有桌面 **25.3% / 347 MB**，大图=桌面这张（共享）**26.0% / 335 MB**，大图是另一张视频
     （第二个解码器）**66.5% / 503 MB / private 1222 MB** ⇒ 重复解码一张 4K 的代价是
     **+41 个百分点单核、+156 MB 工作集**，而这正是他截图那个状态。
     共享后日志明写 `preview on: <id> (…, sharing the live decoder)`。
     **`StopPreview` 绝不 park 共享实例**（那会让桌面冻成静帧，正是之前那条"启动即静止"的根因家族），
     `BuildSlots` 在 `slots_.clear()` 前先 `StopPreview("slots rebuilt")`（共享指针指向槽内对象）。
     复验：共享路径下像素门禁仍是色条 2/255、灰阶 +0/+1、裂格 0；`live players 1 / closed 平衡`；
     0 条 warn/error。
   - **删 MF 时我把一件仪表一起删了**：`closed a player: N still live` 和 `gLivePlayers` 计数器原本在
     `VideoPlayer.cpp` 里，导致这轮 `opened=3 / closed=0` 根本没法解读（其实那 3 个是"缩略图临时实例 +
     桌面实例"，配对正常）。已在 FFmpeg 侧补回同名日志。**教训：删一个后端要连它携带的仪表一起清点。**
   - 仍未做：**D3D11VA 零拷贝**（解码直接写进 GPU 纹理数组，连 `Pack` 那一次 memcpy 都省掉，
     这才是字面意义的"全程 GPU"）；测量噪声：我那个每秒起一个 `--ctl` 进程的心跳循环会把预览拖到
     1.3 fps、桌面拖到 36 f/s，被污染的那组读数（97.0% / 108.5%）已排除，不进任何结论。

   **2026-10-07 23:05 那张绿卡片：根因是"平面没绑上/没上传"，不是颜色管理。**
   他贴的卡片图实测磁盘上 `cache/thumbs/local_05_still.png` 与 `_large.png` **100.00% 是 (0,76,0)**。
   这个颜色是 `Video.hlsl` 对 **Y=0、U=0、V=0** 的确定输出（r=-0.97→0、g=+0.30→**76**、b→0），
   而零来自两处：D3D11 新建纹理默认清零 + `MakeArgs` 只在 `FrameY()` 交出视图后才绑 t0/t1，
   所以**首帧到达之前的任何一次绘制都在采样空槽位**。同一时刻桌面那一路也会绿一下，
   他报的"重新启用渲染器后视频背景是绿的"和"设置了壁纸桌面没显示"是这一个根因的两个表面。
   三层都补了（缺一层都还会漏）：
   - `Nv12Uploader::Ensure` 建完纹理立刻填**限定域视频黑**（Y=16、U=V=128 ⇒ 纯黑），日志行改成
     `plane textures … (cleared to video black)`。零平面从此不再是"某个颜色"。
   - `FFmpegPlayer::Open` 在起线程**之前**就 `Ensure`，失败即 `Open` 失败 —— 保证 `FrameY()` 之后
     永不返回 null，也就是 `srvCount_` 从第一次绘制起就是 2，不再有"空槽位"这个状态。
   - `Thumbnailer::Grab` 落盘前判一次**均匀帧**（亮度 min..max 差 <6 即拒写 + Warn，只对视频包生效，
     纯色图片壁纸本来就该是均匀的）。判据用"均匀"而不是"等于绿色"：黑、白、任何未上传态一并挡掉，
     而且旧卡片会留着 —— 因为缩略图只有重启渲染器才重建，写坏一张就等于永久坏一张。
   复验：删掉那两张绿卡 + `local_05_frames/` 后重启，重建出来 green-ish **0.00%**、亮度 2..242、
   中心 (135,243,254)（那张片本身的青色）；桌面那一路同时在画：`state=background`、`eff=48`、
   `measured_fps=48.00`、6.1 秒 `video_frames_delivered +366`、`frames +293`、
   `corner_samples_changed=208`。
   **均匀帧那道闸一装上去，立刻把更深的一层顶了出来：`ShowFrameAt` 会"报找到帧"而平面根本没上传。**
   23:27 那次重启，`local_02`/`local_03` 两张卡直接没了（`thumb_error: the grabbed video frame is
   uniform (luma 0..0)`）。日志时间线：opened 24.047 → `ShowFrameAt found a frame, 1 frames delivered`
   24.101 → 拒写 24.119。原因是它的等待条件 `delivered_ > want && position_ >= seconds` 会被**那次 seek
   自己**满足：seek 分支在锁内 `position_ = seekSeconds` 并且 `pending_.clear()`（flush 之后队列是空的），
   于是等待立刻退出、`FrameY()` 取不到任何字节、平面停在视频黑清底（Y=16 ⇒ 限定域出来正好是 0..0）。
   也就是说**卡片"取 t=2.0s 那一帧"这条路从来没真的取到过帧**，以前只是把空平面存成绿卡/黑卡没人发现。
   三处改动：
   - `seekSeq_` 在 flush/清队列之后才自增，`ShowFrameAt` 等的是"这次 seek 被取走"，并在同一个临界区里
     抄下 `delivered_` 当基线 ⇒ 只有 seek 之后交付的帧才算数。
   - 朝停车点赶路时不再按帧距 sleep（`holdNext_` 为真就跳过 pacing 等待）：那 120 张中间帧根本没进过屏幕，
     按 60 fps 的节奏走要 120 × 16.6 ms，超过抓取的全部预算。
   - 已经停在这个时刻就直接返回（still 和 large 两次抓取问的是同一个 2.0 s）。
   复验（同一台机、同一个 4K 片）：`local_02` 902→**521 ms**、`local_03` 1290→**699 ms**，两张卡的
   still/large 都是**同一个 121 帧**（去重生效）；卡片内容用眼睛看过（地球/太空，不是纯色）；
   18 张卡 green-ish 全部 **0.000%**，尺子本身用合成图标定过（一半 (0,76,0) + 一半 (16,16,16) ⇒
   读出 50.000% / 50.000%，所以 0 不是尺子瞎）；整份启动日志 warn/error **0 条**；桌面那一路同时还在跑：
   3 秒 `frames +139`、`video_frames_delivered +172`、`video_pos` 2.98→1.15（正常换圈）、`draw_errors=0`。
   **这条闸的红灯一侧从导入这条路走不到**：两种坏文件都在 `addvideo` 就被探测拒了，所以
   `not one frame produced` 那条 Warn 只能作为"事后证人"存在，我没能真把它触发一次。

   **顺手纠正我自己上一轮的一句话**：`0 of 24 motion frames kept` **不是缺陷**，是 `Thumbnailer::Build`
   里明写的设计（`framesToGrab = inst.hasVideo() ? 0 : frameCount_`，理由就是第 65 条那 3.78 秒和
   19/24 重复）。那行日志把设计印成了故障，我连着读了两次都当 bug 报。现在视频包印的是
   `no motion strip (video cards play live, by design)`，图片包仍印 `N of 24 kept`。② "设置了壁纸但桌面
   没显示"这次仍然没能自己复现，`state_reason` 现在会写明是哪个窗口盖着（这次是 `0x7A1322 overlaps this
   monitor … 1000 ms 后换回`），下次直接看渲染器那一行就能分清"被判定遮挡"和"真的没画"。

   **2026-10-08 00:30 「重头播放又卡一下」：不是换圈，是切到一张视频时呈现线程被占住 100-180 ms。**
   他重新导入了两个本地视频（走设置窗导入，23:19 / 23:23，落成 `local_02` / `local_03`）。先把嫌疑清掉：
   **换圈接缝在三个片上都是 14.5-22.7 ms**（一帧），`local_02` 跑满 60 秒、14 次换圈，每一次跨接缝那一帧的真实停留都是 **21.0 ms**，全片众数 20.9 ms，60 秒里没有任何一帧超过
   100 ms ⇒ 循环这条路是干净的（判据：抓真合成屏，`build/tmp/dwell-times.ps1`，144 Hz 采样）。
   真正的成本在**切换**那一刻：把 `local_05` 换成别的片，屏幕上那张画按住了 **138 ms**。按日志时间戳拆开：
   容器探测 16-99 ms、解码器+硬件会话 40-49 ms、着色器编译 9-104 ms、上一个播放器析构 28-59 ms、
   `config.json` 落盘 8-43 ms。设置窗轮播到一张视频时更贵（173 ms），因为那一路还叠了缓存淘汰的析构。
   三处改动（都写清在代码注释里）：
   - `VideoSource` 的打开拆成 `OpenFile`（只碰 libav 和磁盘）+ `Attach`（建平面纹理、起解码线程），
     `Application::OpenVideoWhileDrawing` 把前半放到 worker 上，等的期间继续 `DrainMessages()+DrawDue()`
     ——所以桌面是在放上一张片，不是停住。`Prepare` 多一个 `warmMedia` 参数接住它。
   - 换下去的那个实例交给 **reaper 线程**析构（`ReapLater`），28-59 ms 的播放器关停不再落在呈现线程上。
     `ReloadSlot` 里顺手补了闸：预览正共用这一槽的解码器时先 `StopPreview`，否则它会被指到一个正在
     别的线程上拆掉的对象。
   - `Shader::Compile` 加了一张**按"编译器会读到什么"作键**的 blob 缓存（源码文本 + 五个入口名 + 各
     include 目录里每个文件的名字/大小/写入时间）。反证做过：`touch` 掉 `Shaders/main.hlsl` 之后下一次
     prepare 打的是 `compiled main.hlsl` 而不是 `reusing the compiled`，所以改了着色器不会画旧的。
   复验（同一台机、同一条抓屏尺子）：40 秒里做了 2 次桌面切换 + 5 次设置窗换 hero，
   **每一次事件附近的最差停留 34.8-48.8 ms，整轮没有一个 dwell 超过 60 ms**（改之前是 104/138/173 ms），
   已经落进这台机的环境噪声里（雪花壁纸空跑时的离群就是 41.9 ms）。同时：卡片像素与改动前逐项相同
   （`local_03_still` 视频黑 2.521%、`local_02` 0.000%、green-ish 全 0.000%）、接缝仍 15.3-21.0 ms、
   整份日志 warn/error **0 条**、`draw_errors=0`、`measured_fps=47.4`。
   **还剩下的**：① ~~一个着色器第一次被编译时那 46-104 ms 仍在呈现线程上~~ **2026-10-08 13:50 已做**：
   `WarmPackageShader` 把 `D3DCompile` 挪进 worker（`Shader::Compile` 不需要 device，只有 `CreatePipeline`
   需要），切换时呈现线程只做"查缓存 + 建管线"。② ~~`config.json` 那 8-43 ms 还在 `apply` 里同步写~~
   **已做**：`WallpaperManager::Save` 拆成 `ConfigText()`（本线程，微秒）+ `WriteConfigText()`（后台线程，
   内部一把互斥锁挡住并发写），走 `PostBackground`。代价说清楚：**崩溃会丢最近一次应用**（正常退出会先把
   队列排空，实测 14:01:57 落盘）。③ ~~启动/导入时建卡片冻住呈现线程 521-699 ms~~ **已做**：
   `VideoSource::ShowFrameAt` 多一个 `idle` 回调，`Thumbnailer` 把它接到 `DrainMessages()+DrawDue()`，
   实测在那次 591 ms 的建卡窗口里桌面照常按 21 ms 中位连续出了 23 帧（改之前"整段冻住"是按代码推的——
   那 591 ms 里线程一直卡在 `ShowFrameAt` 的等待里，没有别的路径会去呈现；这一侧没有改前的实测数）。
   三件做完后重测切换：apply 最差 42.2 / 49.0 / 62.6 ms、换 hero 41.9 ms（改之前 138-173 ms）。
   **自己改出来的一个回归，已修**：pump 会在预览实例正被替换的那一刻去画预览，日志
   `preview draw failed: missing rtv or shader` —— 装了 pump 的那几个进程各 12-39 次，没装的是 0 次；
   改成 `DrawDue(bool withPreview)`，预览切换期间的 pump 不画预览（14:0x 复测 25 次切换 0 条 warn）。
   **2026-10-08 14:50 两条他当场报回来的缺陷，都是真的，且都不是上一轮改出来的。**
   - **重载壁纸之后"桌面退出来"**：每 reload 一次就**泄漏一个全屏壁纸窗口**（实测 3 → 4 → 5 个
     `SmartWallpaperDesktop` 子窗口挂在同一个 WorkerW 下，全是 visible=True、3840x2160），活着的那个被压在
     最底下，屏幕上看到的是**系统静态壁纸**，而引擎还在往那个看不见的窗口以 48 fps 呈现
     （`measured_fps=48.0`、`frames` 每秒 +245、`corner_samples_changed` 在涨）。根因：`DesktopWindow`
     **没有析构函数**，`BuildSlots` 的 `slots_.clear()` 只销毁 C++ 对象、不销毁 HWND（`Destroy()` 只在
     显式失败路径和 `DestroySlots()` 里调）。修法：`~DesktopWindow(){ Destroy(); }`。
     复验：连做两次 reload，窗口数 **1、1**；reload 之后抓屏 200 ms 间隔三张的差是 **1.33 / 1.35**
     （修之前是 **0.00 / 0.00** = 整屏冻住的静态壁纸）。
     中途我先试的是"新槽建在旁边、换上去之后再拆旧的"，量下来**症状一模一样**（也 0.00），已经回退 ——
     这条不是窗口顺序问题，是句柄泄漏问题，别再去动 `BuildSlots` 的时序。
   - **删除一个正在被 hero 显示的视频包，删不掉**：`delimage` 回
     `{"ok":false,"code":"io","error":"The process cannot access the file..."}`（他 14:21:50 那次、
     我 14:45:19 复现那次都是这条）。根因：`DeleteImagePackage` 先 `fs::remove_all` 再释放预览实例 ——
     预览的那个播放器正开着 `Wallpapers\<id>\video.mp4`，Windows 拒绝删除含打开文件的目录。
     修法：把释放挪到删除**之前**，并且要**等后台线程真的把它拆完**（上一轮我把播放器析构挪到了
     `PostBackground`，所以这里必须等）。
     **顺带修掉我自己那条假屏障**：被延后析构的实例是装在 job 对象里的，而 job 的析构发生在
     `bgBusy_=false` **之后**，于是 `WaitBackgroundIdle` 会在句柄还没关的时候就返回 —— 实测就是
     "closed a player" 比删除失败晚 23 ms。改成 job 跑完立刻 `job = nullptr`，再清 busy 标志。
     复验：导入 → preview 它 → 立刻 delimage ⇒ `{"ok":true,"deleted":"local_04"}`、目录真没了。
     **反证**：删桌面上那张（local_05）仍然被拒 `{"ok":false,"code":"in_use","monitor":"M0"}`。
     设置窗那句绿色「已删除」**没有撒谎** —— `Bridge::deleteImage` 只在 `ok:true` 时走绿条，
     失败走红色「删除失败: io」；他看到的绿条是 14:23:14 那一次真的成功了，红条是 14:21:50 那一次。


   **导入这条路还剩两笔没动**（都是实测）：探测+拷贝+重扫目录约 152 ms、卡片那次超采样回读+盒式平均约
   152 ms，两次都在呈现线程上，都只在"导入这一个文件"时发生一次。

   **另外一条与卡顿无关但同源的观察**：`PowerManager` 的 `displayOff_` 只由 `RegisterPowerSettingNotification`
   的广播驱动，没有任何轮询兜底 —— 进程恰好在显示器关闭时启动、又错过"开"的那一次广播，桌面就会一直停在
   `display_off`（0 帧）。这次就撞上一回：13:53 起进程只记到 `monitor power -> off`，之后我用
   `SC_MONITORPOWER=2` 唤回，状态才在 14:01 回到 `background`。他会把这种状态报成"设置了壁纸桌面没显示"。

   **试做过一条优化，取舍没定、代码留在工作树里没提交（副本在 `build/tmp/handoff.patch`）**：不再每帧把 15.9 MB NV12 复制进
   `pending_`（那复制实测占每帧 2.61 ms / 4.85 ms），改成把 MFT 自己的 `IMFSample` 交给渲染线程。
   量到的收益：`step mean 4.8→2.3 ms`、工作集 855→771 MB、交付率仍 60/秒、`late_frames=0`。
   量到的代价：**抓屏 p90 停留 21.5→40.9 ms**（渲染线程每秒少取 3–4 帧 ⇒ 约每 5 帧一次重帧）。
   把 GPU 上传挪出持缓冲范围（锁内只做转置）只救回三分之一：p90 → **34.5 ms**，仍不如原样。
   ⇒ 归因未完成，**别在这条上继续叠改动**；而且注意 `osi_.dwFlags=0x7` 含 `PROVIDES_SAMPLES`，
   所以"每次 ProcessOutput 分配 15 MB"是错的，那 15.9 MB 一直是 MF 自己出的。

   **还没做的三件：**
   - **设置界面的视频入口**：导入按钮、播放/暂停、进度、`本地视频` 分类下的卡片文案，一行都没动。
     现在只能用 `--ctl addvideo "<路径>"`（导入时用 MF 探分辨率/帧率/时长写进 `wallpaper.json`）。
   - **只支持 H.264**：解码器按 `H264 → NV12` 枚举，所以 HEVC/VP9/AV1/MKV 会被 `addvideo` 的探测放过
     （探得出分辨率和时长）而在 `Open()` 时才失败。应该在导入那一步就按 subtype 拒绝并给中文文案，没做。
     4K 工作集 ≈873 MB 也偏大。**这句原先的解释是错的，更正过**：转置用的 `rows_`/`chroma_` 一直是复用
     的（`VideoPlayer.hpp:105`），真正每帧重新分配的是**解码线程交给渲染线程的那一整帧 `pending_`**——
     量过是每帧 2.61 ms 的分配+复制（占 4.85 ms 的 54%）；而 `osi_.dwFlags=0x7` 含 `PROVIDES_SAMPLES`，
     所以 MF 的输出缓冲从来不是我们分配的。去掉那次复制能省下工作集约 90 MB，但会换掉一点节奏，
     见上面那条未定的取舍。
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

- **切换动画**已做（2026-10-07：`ui.ini` 的 `transition` = 无/溶解/液态，设置 → 通用 那颗下拉；
  液态按 Codrops Liquid Distortion **demo 3** 逐条搬，参数取自线上页面的 `main3.js` 与 Pixi 4.5.1 的
  `DisplacementFilter`，见 README「切换动画」）。剩下的四件：
  **和参考有一处刻意的偏离，别被"复原"回去**：参考把位移同时加在旧帧和新画面上，他 2026-10-08 说
  「液态的动画导致壁纸渲染出来有移动的效果，能不能把显示的壁纸固定不动」⇒ 现在只搅被换掉的旧抓帧。
  这条不是顺手改的：淡变走完（prog≈0.54）到位移归零（prog 0.85）之间，留在屏上的那张确实被推着起皱
  （prog 0.55 差 9.3 级 / 43.7% 像素，最佳拟合平移 dx=dy=0，所以是碎块起皱），改完同一批数是 0.000 /
  0.0%，而搅的动作在早期两版抓帧逐位一致（数字在 README）。要恢复参考的原样就只改
  `HeroLiquid.frag` 最后那行 `mix(...)` 的第二个取样坐标；
  **没有和参考逐帧对过像素**——`index3.html` 不在公开仓库里、站点挡爬取，浏览器内建视图当时没有可见
  表面，抓不到它的中间帧；机制/时间线/单位是从它自己的代码里读出来的，但"看着一样"要他眼睛判
  （要改就 `Main.qml` 的 `heroLiquidMs` / `heroLiquidScalePx` / `heroLiquidCells`）；
  位移图是程序化 Voronoi 复刻的（他们那张 `crystalize.jpg` 是资产，没照搬），四项统计对上但**胞的排布
  不可能逐像素相同**；桌面那一层的切换目前没有过渡（这套抓帧+着色器只在设置窗里）；
  **切换时的黑帧已治**（2026-10-08：抓帧落盘 PNG→BMP 去掉 GUI 线程上 13~40 ms 的编码，效果加一帧出手延迟 `heroLiquidArmed` 堵住"纹理没到就画"，预热那套按错误归因加的已删）；复测 1000+ 帧 0 次整块底色，但**是他报的、我拍到过一次的那一帧**，所以这条留到他自己再看一次才算完；
  **两张"先闪出新那张"已治**（2026-10-08：溶解起手盖上一次抓帧的 opacity 残留、以及活帧交接的 120 ms 窗口被每秒的
  同 id 重新赋值挪位；数字、反证配置和三档复测都在 README「切换动画」，量具 `build/tmp/uipkg3`）——这条同样要他自己
  在界面里连点几次才算完，因为"闪"的判据最终是他眼睛；
  还有这份改动**没编进 `bld/bin/.../Wallpaper.exe`**（那棵树属于第二个会话），走的是独立树
  `build/ui2/bin/RelWithDebInfo/`：QML + qsb 已 stage，`Wallpaper` 目标已重编（引擎自愈那半在 exe 里，
  不重编不生效），他那扇设置窗重启在这份上（当前 pid 43912；`ui.ini` 的 `transition=liquid` 全程没被探针写过）。

- **最小化/退托盘后大图"换脸"已治**（2026-10-08：`LivePreview` 在停流 1200 ms 后丢了最后一帧，恢复要
  691/737 ms 才回来活帧，中间屏上站的是静帧 ⇒ 静帧→活帧那一跳；现在自己叫的停挂住最后一帧、别人叫的停
  仍按 1200 ms 交回静帧/切片，数字与复测在 README）。剩下的两件：
  恢复那一刻画面仍**冻** ~0.7 s（第二遍本来就是停的，那是他要的省电闸；要恢复即动只能让第二遍继续跑，
  得他点头才改）；以及这条 A/B 是在**多个设置窗共用一条第二遍**的环境下量的（探针窗会和他那扇窗互相
  抢 `preview` 目标），他一个人用的时候静帧那一跳的成因是停流超时，判据干净，但"0 次露头"这个数
  要他自己最小化→打开看一眼才算收。

- **第二遍不再因窗口最小化/退托盘而停**（2026-10-08 他推翻 10-07 那条省电闸：「取消最小化、任务栏时候
  动态壁纸的暂停吧，让它一直播放」）。代价按他要求量了才接受：同窗同卡只拨 `useLivePreview` 做 A/B，
  引擎 CPU 测不出差（粒子卡 开 54.8/45.9 vs 关 58.7/58.2，噪声 ±20），`nvidia-smi` 功耗 28.19~28.27 W
  vs 28.11~28.40 W、利用率 34~39% vs 35~40% ⇒ 他说"没有额外消耗"在这台机器上成立。恢复时那 539/737 ms
  的换脸因此从根上没有了；`liveView.paused` 只留给 `useLivePreview` 关掉/没有大图这两条路。

- **第二遍发布过一帧全黑**（2026-10-08：`Application::RenderPreview` 现在在 `hasVideo() &&
  args.srvCount == 0` 时不发布；数字、范围和"为什么只有 无/溶解 报"在 README）。**要重编引擎才生效**，
  与"崩了不弹框"同批；重编 `bld` 会连带把第二个会话未编的 `Shader.cpp` 着色器编译缓存（+57 行，
  已在我的独立目录 `build/chk` 编过，只余既有告警）一起带上去，所以等他点头。

- **`WallpaperRenderer.exe` 会崩，崩在"桌面槽放 4K 视频 + 第二遍预览同时开 4K60"那条路上**
  （2026-10-08 02:20:56，他报「突然退出桌面壁纸，但是程序还是打开的」）：WER 记的是
  `APPCRASH / c0000005 / 偏移 00000000000831a4`，报表在
  `C:\ProgramData\Microsoft\Windows\WER\ReportArchive\AppCrash_WallpaperRendere_3a6877bd...`；
  `renderer-34324.log` 停在两行 info 中间、一句收尾都没有，最后三条是
  `closed a player: 1 still live, process ws 404 MB` → `opened local_03\video.mp4 via FFmpeg (3840x2160, 60.00 fps)`
  → 4 秒后死。**根因没查**（在第二个会话正在改的 FFmpeg 播放器那条路上），要追从 WER 的 offset + `bld` 的
  .pdb 起；这次改动不掩盖它，只是把症状从"桌面变黑、要他自己去按「启动壁纸进程」"变成"黑 4.5 秒自己回来"。
  已做的兜底：`Bridge::engineDied()` 自动重启（30 秒内不重复 / 10 分钟最多 3 次），**崩掉还是他自己叫停的**
  看引擎日志的最后一行（干净退出有 `final:`，崩溃停在中途的 info 上）——认成"他停的"就不碰；他自己按
  「退出壁纸进程」另外记 `quitAsked_`。实测：杀（`TerminateProcess`）→ 2452 ms 回来；`--ctl quit` → 观察
  13 秒没有再起来。引擎入口加 `SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS)`，`main.cpp` 两处
  `MessageBoxA` 改成写 `logs/startup-error.log`——他要的是"崩了就退出，不要弹窗提示"。
  **崩了不弹框这半要重编引擎才生效**，而 `Engine/Graphics/Shader.cpp`（00:41）比在跑的
  `WallpaperRenderer.exe`（00:33）新，那是他们没编的改动，所以这次没替他们重编。

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
- **`ShowFrameAt` 会把被它抓帧的那个播放器挂起**，而只有下一次 seek 会清 `holding_`
  （`VideoPlayer.cpp:420`）。2026-10-07 就是这条让**每次开机后壁纸冻在某一帧**：`BuildThumbnails` 对
  "某块屏正在显示的那个包"**复用活槽实例**（`Application.cpp:825-827`），于是缩略图抓帧把桌面上的播放器
  挂起了。已修（视频包不复用活槽实例，判据：全新启动、不发任何 seek，5 秒差值 = 解码 +296/5 s、
  绘制 +245/5 s）。**这条坑本身还在**：任何新代码想在活槽实例上调 `ShowFrameAt`，都会把桌面冻住，
  所以要么给它一个临时实例，要么抓完立刻发一次 seek 解锁。分辨是不是这条只看两点：
  `video_frames_delivered` 冻住 **且** 进程 CPU 0%（CPU 爆高是另一类：喂帧循环在失败）。
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
