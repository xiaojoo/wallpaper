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
3. **视频壁纸**（Media Foundation 硬解 → D3D11 纹理）。
   判据：CPU 占用不随分辨率上涨；`nvidia-smi` 能看到解码会话；暂停/seek 生效。

## V0.2.1 设置界面还缺的

- ~~开机自启~~ 已做（HKCU Run 单值，默认关）；未验：注销后重新登录是否真的起来
- 壁纸缩略图已做（离屏渲 still/large/8 帧）；未做：壁纸包自己声明预览时间点（现在固定 t=2.0s 起 8 帧）
- 本地图片已做（`addimage` + `Shaders/Image.hlsl` 覆盖裁切 + Ken Burns）；未做：删除/重命名导入项、批量导入
- ~~`--select <id>`~~ 已加并截图自证（`build/shots/drawer4.png`）
- 每显示器分别设帧率上限（后端 `assignments[].fps` 支持，界面只给了全局上限）

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

- ~~双实例抢管道~~ 已修：`Local\SmartWallpaper.Renderer` 互斥体，第二个实例直接退出
  （实测起两个只剩 1 个）。未做：崩溃后自动重启。

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
