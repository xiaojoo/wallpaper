// Wallpaper/qml/Main.qml - browse-first layout: the wallpaper fills the window, its controls
// sit on two scrims over it, and settings opens as an overlay.
// Chinese by default with an English switch; every control maps to a renderer command, and
// anything the renderer cannot do is not shown.
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Effects
import SW 1.0

ApplicationWindow {
    id: root
    width: 1360
    height: 860
    minimumWidth: 1080
    minimumHeight: 680
    visible: true
    title: "Wallpaper"
    // Frameless so the wallpaper reaches the glass. The cost, measured: DWM has no non-client area to
    // animate, so a maximize is one snap instead of a zoom (see the note in Bridge.cpp - the
    // eaten-frame variant animates but leaves a 16x39 px strip Qt never paints). The window commands
    // still go to the platform through Bridge.showCommand.
    flags: Qt.Window | Qt.FramelessWindowHint
    color: th.bg
    // Closing the window must not leave the renderer drawing a second pass forever: the heartbeat
    // would stop, but its watchdog only fires after three seconds. Coming back out of the tray has
    // to re-arm it here too - the carousel's own visible flag never changed, so nothing else does.
    // Minimising and restoring are the same question in a different form: Qt keeps `visible` true
    // while a window is minimised, so `visibility` is the signal that actually moves. Losing focus
    // deliberately no longer re-syncs anything - motion continues while the window is on screen.
    onVisibleChanged: carousel.syncPreview()
    onVisibilityChanged: carousel.syncPreview()

    QtObject {
        id: th
        readonly property color bg: "#0B0F16"
        readonly property color rail: "#0F141D"
        readonly property color surface: "#151C27"
        readonly property color surfaceAlt: "#1C2532"
        readonly property color hover: "#222D3D"
        readonly property color line: "#222C3A"
        readonly property color text: "#E8ECF3"
        readonly property color muted: "#8D9AA9"
        readonly property color accent: "#3B82F6"
        readonly property color accentDim: "#2A5FB8"
        readonly property color ok: "#3FB27F"
        readonly property color warn: "#E0A030"
        readonly property color err: "#E06A6A"
        // The filled version of the danger colour. err reads well as a tint on dark but not as a
        // plate under white text: measured on the 确认删除 badge it is #E06A6A, which is 3.26:1
        // against white - below AA. This plate is a deep red whose channel spread (max-min)/max is
        // 0.767, the same as the theme's other Dim fill accentDim (0.772), so it reads as one family:
        // R 164 -> 150 and G/B 77 -> 35 against the previous step, and white text goes 5.60:1 ->
        // 8.26:1.
        readonly property color errDim: "#962323"
    }

    // How much of the wallpaper reads through the top row's plates (the tabs and the search field).
    readonly property real plateAlpha: 0.72

    readonly property string fontFamily: "Microsoft YaHei UI"
    readonly property var intervalChoices: [10, 30, 60, 360, 1440]

    property string category: ""           // "" means every category, "fav" the favourites tab
    property string query: ""
    property string selectedId: ""
    property bool settingsOpen: false
    property bool previewMotion: true    // the big picture shows the motion frames unless told otherwise
    property int previewFrame: 0
    property real motionT0: Date.now()   // wall-clock origin of the current strip playback
    onMotionT0Changed: previewFrame = 0
    onHeroMotionChanged: if (heroMotion) motionT0 = Date.now()
    property string settingsTab: "general"
    // A command result is shown on the page that produced it and for three seconds: it used to sit at
    // the bottom of the dialog until the next one came, which is how 「已删除」 ended up greeting him on
    // the 显示器 page (2026-10-08). `msgTab` is the page the message was born on, not the page open now.
    property string msgTab: ""
    property bool msgLive: false
    property Item openField: null    // the Choice whose list is up; only one at a time
    property bool carouselPlaying: true
    // The big picture shows the wallpaper being drawn right now (Wallpaper/LivePreview.cpp).
    // It used to be an Image with a new URL per frame, which measured 16-18 fps and blanked the
    // area on every swap; the painted item reuses one texture, so the picture never disappears
    // between frames. Turning this off falls back to the saved strip.
    // 2026-10-09: this was switched off for one round on the reading that 「设置页面不要渲染成视频，
    // 只截取第一帧作封面」 meant this picture. He meant the 壁纸 page inside the settings dialog, which
    // is a plain Image over a file on disk and decodes nothing - so there was nothing to save there, and
    // the live hero came back the same day. What the experiment did measure stays in BACKLOG: with it off
    // the engine's second pass goes to +0 frames/s (`preview.drew`), the desktop path is unchanged
    // (decode step 2.60 ms, 10.15% of a core with the window open against 9.11~10.17% without), and the
    // window's own ~12% of a core does NOT drop - that cost is the 4 s carousel, not these frames.
    property bool useLivePreview: true

    function slideIndex() {
        for (var i = 0; i < root.shown.length; ++i)
            if (root.shown[i].id === root.selectedId) return i
        return 0
    }
    readonly property var current: root.shown.length > 0 ? root.shown[root.slideIndex()] : null
    // The wallpaper the desktop is actually showing. Only the picture layers use it: when a tab has
    // nothing in it, the hero must fall back to this instead of holding whatever the preview pass
    // happened to be drawing when the tab changed. The caption, the heart and the apply buttons stay
    // on `current`, so an empty tab still has nothing selected.
    readonly property var applied: {
        const mons = Bridge.monitors
        const cat = Bridge.catalog
        function find(id) {
            for (let i = 0; i < cat.length; ++i) if (cat[i].id === id) return cat[i]
            return null
        }
        for (let m = 0; m < mons.length; ++m)
            if (mons[m].tag === Bridge.targetMonitor) { const w = find(mons[m].wallpaper); if (w !== null) return w }
        for (let m = 0; m < mons.length; ++m) { const w = find(mons[m].wallpaper); if (w !== null) return w }
        return null
    }
    // Switching the big picture used to be a hard cut. Painting the outgoing wallpaper's *still* over
    // it to fake a crossfade made it worse (measured: 12 of 12 switches had the live pass on screen,
    // so that overlay was a foreign picture arriving at full opacity - the flash he saw), and dipping
    // the whole stack to the box's dark base was rejected for the same reason: it dims. So the
    // outgoing frame is *captured* instead - whichever layer really is on screen, which is why one
    // mechanism covers the live, motion and still cases alike - and the swap happens underneath it
    // while that capture is taken away by the transition picked in 设置 > 通用 > 切换动画: "dissolve"
    // fades it, "liquid" churns it into the new picture (shaders/HeroLiquid.frag), "none" cuts.
    readonly property var pendingHero: current !== null ? current : applied
    property var hero: null
    // 0 = nothing in flight, 1 = the capture is fading out, 2 = the liquid morph is running.
    property int heroFx: 0
    readonly property int fxDissolve: 1
    readonly property int fxLiquid: 2
    // The reference's own numbers: a 1.3 s sweep, displaceScale 300 px against its 1920 px render
    // target, a 512 px map tile with ~9.5 Voronoi cells across it, and the map sprite turning while
    // it churns. 0.78 rad is the *total* the reference accumulates at 60 fps (it adds
    // progress * 0.02 once per frame), so this port keeps the total rather than the per-frame step,
    // which would run about twice as far at this window's measured ~125 fps.
    property int heroLiquidMs: 1300
    property real heroLiquidScalePx: 300
    property real heroLiquidStageW: 1920
    property real heroLiquidCells: 9.5
    property real heroLiquidRotRad: 0.78
    // The reference never resets the map's rotation, so every switch starts where the last one
    // ended and the cells are turned by a different amount each time.
    property real heroLiquidRotBase: 0
    // Ready == 0; anything else means the .qsb did not load, and a morph with no texture would show
    // the box's base colour, so the fade is used instead.
    readonly property bool heroLiquidUsable: Bridge.transition === "liquid" && heroLiquid.status === 0
    function heroFxMode() {
        if (Bridge.transition === "none") return 0
        return heroLiquidUsable ? fxLiquid : fxDissolve
    }
    // saveToFile takes a QUrl, and QUrl("C:/Users/...") is not an absolute local file - it has to be
    // spelled file:/// or the write silently fails and the overlay is left with nothing to show.
    // Two names, alternated: an Image whose `source` is assigned the same URL it already holds does
    // not reload, so a single path would show the *first* grabbed frame on every later switch.
    // Not Qt.temporaryPath - measured undefined in this QML, which wrote the file to the working
    // directory under the name "undefinedwallpaper-hero-out.png".
    // The extension is what makes or breaks the frame: writing the grab as PNG measured
    // 13-40 ms on the GUI thread (and 2 ms as BMP, which is the same 24-bit pixels with no encoder),
    // and a 40 ms stall is presented as one near-black frame over the whole window.
    // .bmp is also what the grab already is, so nothing is decoded on the way back either.
    property int snapFlip: 0
    function snapUrlFor(i) {
        const dir = Bridge.rootDir.replace(/\\/g, "/")
        return "file:///" + dir + "/cache/hero-out-" + (i ? "b" : "a") + ".bmp"
    }
    property url snapUrl: ""
    property bool snapBusy: false
    function heroIdOf(v) { return v && v.id !== undefined ? v.id : "" }
    function heroOutItem() {
        // Whichever layer really is the picture on screen right now - that is the frame to keep.
        if (heroLive) return liveView
        if (heroMotion) return frameStack
        return heroImg
    }
    function heroFxStop() {
        heroOutFade.stop()
        heroLiquidRun.stop()
        heroLiquid.prog = 0
        heroLiquidArmed = false
        heroSnapFresh = false
        // Stand the capture down completely, including its opacity. The morph leaves it at 1 because
        // that is what its `from` texture needs, and `visible` for a dissolve is `opacity > 0.001`,
        // so the next switch would raise a *previous* switch's frame the moment heroFx is set -
        // which happens before that switch's own grab lands. Measured 9-24 ms of the wrong picture
        // at the start of every dissolve that followed a liquid one (3 of 4 switches in a probe run).
        heroSnap.opacity = 0
        heroFx = 0
        // A switch that is still waiting for its first picture has no transition left to run: the
        // watch would otherwise fire on a stopped effect and restart a fade nobody asked for.
        heroWaitPicture = false
        heroUncoverWatch.stop()
        // The transition just ended, so the frame on screen is now the frame a new capture would
        // have to start from. Replay the newest pick if one arrived while we were busy.
        if (heroRetry) { heroRetry = false; Qt.callLater(function () { root.startHeroSwap() }) }
    }
    // The morph may not paint before the grab of the outgoing frame has reached the GPU. On the
    // pass where it has not, the `from` texture is null and overBase() in the shader turns the
    // picture area into the box's own #080B10 - caught in a captured frame once: picture area mean
    // 11.7 with its brightest pixel 16.0, i.e. nothing but the base colour, while the rest of the
    // window was drawn normally. So the capture is painted as-is for one frame first (that is the
    // overlay mechanism he already accepted, and it is the same pixels he was looking at), and the
    // effect takes over on the frame after.
    property bool heroLiquidArmed: false
    // The capture is only worth painting once it is the *current* frame. heroFx is set before the
    // grab lands (the morph's two texture feeds hang off it), so painting on that alone shows the
    // previous switch's capture for as long as the grab takes - measured 7-12 ms on the still
    // wallpapers and 33-158 ms on the particle ones, which is the fast flicker he saw on every
    // liquid switch. Dissolve never had it because its overlay appears with the fade, after the
    // capture lands.
    property bool heroSnapFresh: false
    Timer {
        id: heroLiquidArm
        interval: 16
        onTriggered: root.heroLiquidArmed = true
    }
    // The cap on waiting for the picked clip's first frame. The slowest switch measured here was
    // 356 ms; this is that with room, and it is what hands the page back when the feed is never going
    // to deliver (engine gone, clip unreadable) instead of holding his old picture forever.
    Timer {
        id: heroUncoverWatch
        interval: 600
        onTriggered: root.heroUncoverNow()
    }
    // A second pick while a capture or a morph is in flight used to be dropped on the floor: the
    // guard below returned early and nothing replayed it, so the big picture stayed on the first
    // pick while the strip had already moved. The morph makes that window about a second long, so
    // the request is remembered and applied as soon as the swap in flight commits.
    // Only the *newest* pick is ever replayed (`pendingHero` is a binding, not a queue), so clicking
    // fast five times costs two morphs, not five. A morph is never restarted from underneath: the
    // capture would then come from the layer *under* the running morph, i.e. from a picture he is
    // not looking at, which is the pop this whole mechanism exists to avoid.
    property bool heroRetry: false
    // The cover has to stand down when the picture *under* it is the one he picked, not 280 ms after
    // the click. Measured across five real switches (probe copy, 8 ms timeline): the live feed carries
    // the new wallpaper after 59 / 61 / 98 ms for the shader and particle cards, but 233 ms and 356 ms
    // for a clip that has to open its decoder - and the cover is gone at ~320 ms. So on a clip he
    // watched the previous wallpaper's held live frame sit there for the last stretch and then the new
    // picture cut in underneath nothing. On 冷光-剪影, whose own picture is dark (mean 17/70/99), that
    // cut is the 闪黑屏 he reports, and it is why a video switch does not look like the 切换动画 he set.
    // Only the video path waits; the others land inside the cover already.
    property bool heroWaitPicture: false
    property real heroFramesAtSwap: 0
    function heroPictureIsHere() {
        return liveView.matching && liveView.frames > root.heroFramesAtSwap
    }
    function heroStartFx() {
        if (heroFx === fxLiquid) {
            heroLiquidArmed = false
            heroLiquidArm.start()
            // Like the reference's sprite: the turn is never wound back, so the next switch churns
            // through a different part of the cell pattern.
            heroLiquidRotBase = (heroLiquidRotBase + heroLiquidRotRad) % 6.28319
            heroLiquidRun.restart()
        }
        else heroOutFade.restart()
    }
    function heroUncoverNow() {
        if (!heroWaitPicture) return
        heroWaitPicture = false
        heroUncoverWatch.stop()
        heroStartFx()
    }
    function commitHeroSwap() {
        heroSnapFresh = snapUrl !== ""
        // Raise the cover *before* the swap, not after it. Both branches used to lean on their own
        // first value arriving with the next animation tick (the fade's `from: 1`, the morph's
        // opacity), which leaves the new picture as the thing on screen for that frame.
        if (heroSnapFresh) heroSnap.opacity = 1
        hero = pendingHero
        // No capture means nothing to dissolve: cutting is the only honest remaining option, and an
        // overlay with an empty texture would show the box's base colour.
        if (!heroSnapFresh) { heroFxStop(); return }
        // `matching` alone is not enough to ask for: the frame the section holds can be one that
        // arrived before this swap and still carries the new id, and the counter is what tells the two
        // apart. With the live pass off there is no picture to wait for, so it does not wait.
        heroFramesAtSwap = liveView.frames
        if (hero && hero.type === "video" && liveWanted && !heroPictureIsHere()) {
            heroWaitPicture = true
            heroUncoverWatch.restart()
            return
        }
        heroStartFx()
    }
    function startHeroSwap() {
        const nid = heroIdOf(pendingHero), cid = heroIdOf(hero)
        if (nid === cid) { hero = pendingHero; return }      // same wallpaper, new object: no fade
        // The first picture has nothing to dissolve *from*, and before the window has been drawn the
        // morph's shader has no status to report either, so it must land at once.
        if (hero === null) { hero = pendingHero; return }
        if (snapBusy || heroSnapWatch.running || heroFx !== 0) { heroRetry = true; return }
        if (Bridge.transition === "none") { heroFxStop(); snapUrl = ""; hero = pendingHero; return }
        // Set before the capture starts: the two grabs the morph needs are switched on by this.
        heroFx = heroFxMode()
        snapBusy = true
        heroSnapWatch.start()
        const it = heroOutItem()
        // Half size: the grab is a 250 ms dissolve of the same picture, and encoding 1360x860 measured
        // 300-350 ms on the particle wallpapers (long enough that the old picture visibly holds), while
        // 680x430 costs a quarter of the bytes.
        if (!it || !it.grabToImage(function (res) {
                snapBusy = false
                heroSnapWatch.stop()
                snapFlip = snapFlip ^ 1
                const u = root.snapUrlFor(snapFlip)
                snapUrl = res.saveToFile(u) ? u : ""
                commitHeroSwap()
            }, Qt.size(680, 430))) {
            snapBusy = false
            heroSnapWatch.stop()
            snapUrl = ""
            commitHeroSwap()
        }
    }
    onPendingHeroChanged: startHeroSwap()
    // The grab is asynchronous and a hidden or occluded window can leave it never completing; the
    // picture must not freeze on the old wallpaper because of that.
    Timer {
        id: heroSnapWatch
        interval: 250
        onTriggered: { snapBusy = false; snapUrl = ""; commitHeroSwap() }
    }
    function framesOf(w) { return w && w.frameUrls !== undefined ? w.frameUrls : [] }
    // How long one strip frame stays up. The renderer sampled the animation at this spacing, so
    // playing it back any faster or slower changes the speed of the motion, not just its smoothness.
    readonly property real frameMs: hero !== null && hero.frame_ms !== undefined && hero.frame_ms > 0
                                     ? hero.frame_ms : 42
    // Delivered frames, not a flag from the pipe: if the renderer is older than this build or the
    // section is gone, running stays false and the saved strip keeps playing.
    readonly property bool liveWanted: useLivePreview && previewMotion
    // The feed is alive as long as frames keep arriving, even if the newest one still belongs to
    // the wallpaper just left: gating on that would tear down and rebuild the 24 strip images on
    // every switch. When the renderer dies the feed goes stale, running falls to false, and the
    // saved strip loads again so the picture keeps moving instead of freezing.
    readonly property bool liveFed: liveWanted && liveView.running
    // Motion is wanted while this window is on screen - not only while it is the foreground window.
    // Gating it on `active` made the big picture freeze the moment he clicked into another program
    // (measured 2026-10-07: with the window visible and the game client in front, 0 of 724,880 hero
    // pixels changed over 1.5 s and status.preview.on read false), and a wallpaper that stops when
    // nobody is looking at it is the opposite of what this app is for. The case that *should* stop -
    // a full-screen game taking the screen over - is the renderer's own occlusion rule, not this one.
    // This is the gate for *painting* the saved strip. It deliberately does not gate the renderer's
    // second pass any more (2026-10-08, he asked): stopping that pass is what made a minimise cost
    // ~0.7 s of held/still picture on the way back.
    readonly property bool motionWanted: root.visible && root.visibility !== Window.Minimized
    // A switch costs the renderer one beat: measured 34-158 ms before the section holds the newly
    // picked wallpaper (20 switches through every card in the catalogue). Hiding the live item for
    // that stretch stops the motion, and the still and the live frame are different moments of the
    // animation, so it reads as a hitch. Holding the outgoing frame covers the gap; the cap is there
    // so a feed that never delivers the new wallpaper hands the page back to the still instead of
    // showing the wrong one forever.
    //
    // The window has to count from the switch and from nothing else: `hero` is also reassigned with
    // a new object carrying the *same* id once a second, when Bridge::tick re-reads the renderer's
    // status and the `current`/`applied` binding rebuilds it, and `onHeroChanged` cannot tell the two
    // apart. Re-arming on those puts the deadline after a refresh rather than after the switch, and
    // if the new wallpaper never arrives the picture alternates between the outgoing live frame and
    // the incoming still once a second.
    property int liveGraceMs: 350
    property bool liveHeld: false
    property string liveHeldFor: ""
    readonly property bool heroLive: liveFed && (liveView.matching || liveHeld)
    readonly property bool heroMotion: !liveFed && previewMotion && motionWanted && hero !== null
                                       && framesOf(hero).length > 1

    readonly property var settingsTabs: [
        { key: "general", icon: "gear", label: "tab_general" },
        { key: "monitors", icon: "monitor", label: "monitors" },
        { key: "power", icon: "bolt", label: "tab_power" },
        { key: "wallpapers", icon: "layers", label: "wallpapers" },
        { key: "renderer", icon: "cpu", label: "renderer" }
    ]

    function settingsTabLabel() {
        for (var i = 0; i < root.settingsTabs.length; ++i)
            if (root.settingsTabs[i].key === root.settingsTab) return trs(root.settingsTabs[i].label)
        return trs("settings")
    }

    readonly property var qualityItems: {
        const _lang = Bridge.lang
        const out = []
        for (let i = 0; i < Bridge.qualityLevels.length; ++i) {
            const q = Bridge.qualityLevels[i]
            out.push({ value: q, label: q === "package" ? trs("quality_package") : q })
        }
        return out
    }
    readonly property var intervalItems: {
        const _lang = Bridge.lang
        const out = []
        for (let i = 0; i < intervalChoices.length; ++i) out.push({ value: intervalChoices[i], label: intervalText(intervalChoices[i]) })
        return out
    }
    readonly property var fitItems: {
        const _lang = Bridge.lang
        const out = []
        for (let i = 0; i < Bridge.fitLevels.length; ++i) {
            const f = Bridge.fitLevels[i]
            out.push({ value: f, label: trs("fit_" + f) })
        }
        return out
    }
    readonly property var transitionItems: {
        const _lang = Bridge.lang
        const out = []
        for (let i = 0; i < Bridge.transitionLevels.length; ++i) {
            const t = Bridge.transitionLevels[i]
            out.push({ value: t, label: trs("transition_" + t) })
        }
        return out
    }

    function trs(key) { return Bridge.lang ? Bridge.t(key) : Bridge.t(key) }
    function num(v, digits) { return Number(v).toFixed(digits === undefined ? 0 : digits) }
    function inUse(w) {
        for (var i = 0; i < Bridge.monitors.length; ++i)
            if (Bridge.monitors[i].wallpaper === w.id) return true;
        return false;
    }
    // The title over the caption bar reads as a name, not as a path: the extension says nothing the
    // viewer asked for and the resolution is already printed in the meta line right beside it.
    // Only a *trailing* "_WxH" goes - "snow-3840x2160-mountains-cave-25813.jpg" carries it in the
    // middle, and stripping that would cut words out of the name rather than a tag off its end.
    function displayName(s) {
        var t = String(s).replace(/\.[A-Za-z0-9]{1,5}$/, "")
        t = t.replace(/_[0-9]{2,5}x[0-9]{2,5}$/, "")
        return t.length > 0 ? t : String(s)
    }
    function resText(w) {
        if (w === null || w.resolution === undefined) return trs("generated")
        return w.resolution === "generated" ? trs("generated") : w.resolution
    }
    function particleCount(w) { return w && w.particles !== undefined ? w.particles : 0 }
    function intervalText(m) { return m < 60 ? (m + " min") : (m === 60 ? "1 h" : (m / 60) + " h") }

    // Reading Bridge.lang / .catalog / .favorites inside the binding is what keeps the
    // grid live and makes every label re-evaluate when the language switch flips.
    readonly property var shown: {
        const _lang = Bridge.lang
        const favs = Bridge.favorites
        let out = []
        for (let i = 0; i < Bridge.catalog.length; ++i) {
            const w = Bridge.catalog[i]
            // "fav" is a tab, not an extra axis: it replaces the category rather than narrowing it.
            if (category === "fav") { if (favs.indexOf(w.id) < 0) continue }
            else if (category !== "" && w.category !== category) continue
            if (query.length > 0) {
                const hay = (w.name + " " + w.id + " " + w.category).toLowerCase()
                if (hay.indexOf(query.toLowerCase()) < 0) continue
            }
            out.push(w)
        }
        out.sort(function (a, b) { return String(a.name).localeCompare(String(b.name)) })
        return out
    }

    // Only the imported copies: the settings page offers to delete them, so the list it shows and the
    // list it can delete have to be the same list.
    readonly property var localImages: {
        const _lang = Bridge.lang
        const all = Bridge.catalog
        const out = []
        for (let i = 0; i < all.length; ++i)
            if (all[i].id !== undefined && String(all[i].id).indexOf("local_") === 0) out.push(all[i])
        return out
    }

    readonly property var categoryCounts: {
        const _lang = Bridge.lang
        const m = ({})
        for (let i = 0; i < Bridge.catalog.length; ++i) {
            const c = Bridge.catalog[i].category
            m[c] = (m[c] || 0) + 1
        }
        return m
    }

    Component.onCompleted: {
        // Not debug noise: the click harness reads this out of ui.log to turn QML coordinates into
        // screen coordinates (self-drawn controls have no UIA element to aim at).
        console.log("window geometry " + root.x + "," + root.y + " " + root.width + "x" + root.height)
        if (Bridge.selectAtStart.length > 0) root.selectedId = Bridge.selectAtStart
        if (Bridge.settingsAtStart) root.settingsOpen = true
        if (Bridge.settingsTabAtStart.length > 0) root.settingsTab = Bridge.settingsTabAtStart
        // `hero` follows `pendingHero` with a deliberate delay while a switch is fading; the first
        // picture must be there at once, not after the first dip.
        root.hero = root.pendingHero
    }

    // There is no top bar: the search box moved into the content header, the settings entry into the
    // right end of the bottom band, and the language switch into the settings dialog itself.

    // The window's floor. A press that gets this far landed on nothing clickable, so it is "somewhere
    // else" and has to end whatever edit is still running - otherwise the caret keeps blinking in the
    // search box after a click on empty page.
    MouseArea {
        anchors.fill: parent
        z: -1
        onPressed: root.contentItem.forceActiveFocus()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // -------------------------------------------------------------- grid column
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0


            // ---------------------------------------------------------- carousel
            // Big preview on top, the small cards underneath as its navigator.
            ColumnLayout {
                id: carousel
                Layout.fillWidth: true
                Layout.fillHeight: true
                // No gap on any side: the wallpaper runs to the window edge, and the two scrim
                // bands sit flush with it (they are square-cornered for the same reason).
                Layout.leftMargin: 0
                Layout.rightMargin: 0
                Layout.topMargin: 0
                Layout.bottomMargin: 0
                spacing: 10
                // Only a renderer with nothing in it collapses this column. A filter that matches
                // nothing must not take the chips and the settings entry down with it - that state
                // has no other way out.
                visible: Bridge.catalog.length > 0

                // The live pass runs while this page is on screen, whether or not this window is the
                // foreground one - see motionWanted for why focus is not part of it. Hiding the page,
                // minimising the window, or closing it to the tray hands the GPU back.
                function syncPreview() {
                    // The second pass is asked for whenever there is a big picture to show and the
                    // live-preview switch is on - window visibility is not part of it (2026-10-08, he
                    // asked for the motion to keep playing through a minimise or a trip to the tray:
                    // stopping it cost ~0.7 s of still/held picture on the way back). What is left in
                    // this gate is the two cases that genuinely have no picture to animate.
                    // ... with one exception he set on 2026-10-09: a clip that is *not* what the desktop
                    // is showing gets its cover instead of a live pass. Animating a different file needs
                    // a SECOND 4K decoder - the desktop's player cannot be shared with another clip -
                    // measured as `live players in this process: 2` and `process ws 946 MB` against ~320
                    // MB with one, the same +41 pt of a core / +156 MB that re-decoding a 4K clip has
                    // always cost. Shaders and particles are drawn rather than decoded, so they keep
                    // moving, and the clip actually on the desktop keeps its live picture.
                    // REJECTED the same day: 「没有设置成壁纸的动态视频，在点击到它时也要播放」. The
                    // second decoder is therefore the price of an honest preview and stays. The cost is
                    // not folklore - it is what the log showed on 2026-10-09 while this page was open
                    // with the old behaviour: `opened ... local_05\video.mp4 via FFmpeg`, then
                    // `live players in this process: 2`, then `closed a player: 1 still live, process
                    // ws 946 MB` (against ~320 MB while one player serves both passes).
                    const want = root.hero !== null && root.useLivePreview
                    if (want)
                        Bridge.showPreview(root.hero.id)
                    else
                        Bridge.hidePreview()
                    // Set after the request, because a *new* id clears the pause flag inside it: the
                    // clip the page moved to is not the one he stopped.
                    // While a clip is paused the window has to hold the frame it stopped on - the engine
                    // publishes nothing more while the pause is on the pass (that is how pausing the
                    // picture stays off his wallpaper when the same clip is on screen), and after 1200 ms
                    // of silence the item would otherwise hand the page back to the still.
                    liveView.paused = !want || Bridge.previewVideoPaused
                }
                Connections {
                    target: Bridge
                    function onPreviewVideoPausedChanged() { carousel.syncPreview() }
                }
                onVisibleChanged: syncPreview()
                Component.onCompleted: syncPreview()

                Timer {
                    id: liveGrace
                    interval: root.liveGraceMs
                    onTriggered: root.liveHeld = false
                }
                Connections {
                    target: root
                    function onHeroChanged() {
                        carousel.syncPreview()
                        const id = root.heroIdOf(root.hero)
                        if (id === root.liveHeldFor) return
                        root.liveHeldFor = id
                        // Held unconditionally rather than only when the feed is mismatched: the
                        // `wantedId` binding may or may not have re-evaluated by the time this signal
                        // runs, so `matching` here can still describe the wallpaper just left, and
                        // reading the stale value is exactly the miss this guards.
                        root.liveHeld = root.liveFed
                        if (root.liveHeld) liveGrace.restart()
                        else liveGrace.stop()
                    }
                }
                Connections {
                    target: liveView
                    function onFrameArrived() {
                        // First: the cover over a switch that was waiting for its picture can come
                        // down now. This has to be checked before the hold below, which returns early.
                        if (root.heroWaitPicture && root.heroPictureIsHere()) { root.heroUncoverNow(); return }
                        if (!root.liveHeld) return
                        if (liveView.matching || !root.liveFed) { root.liveHeld = false; liveGrace.stop() }
                    }
                }

                Timer {
                    interval: 4000
                    running: root.carouselPlaying && root.shown.length > 1 && root.active
                    repeat: true
                    onTriggered: strip.step()
                }

                Rectangle {
                    id: heroBox
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 200
                    radius: 8   // same corner as the strip cards under it
                    color: "#080B10"
                    clip: true
                    // The three layers inside used to sit 1 px in from this box. With the window
                    // frame gone that inset was the last thing between the wallpaper and the glass
                    // (the outermost column read #080B10), so they are flush now.

                    // The picture itself, in whichever form is alive right now, in one Item: the
                    // liquid morph takes the whole stack as its incoming texture and must not care
                    // which of the three layers happens to be up.
                    Item {
                        id: heroContent
                        anchors.fill: parent

                        // The renderer's live second pass: the same wallpaper drawn now, on the same
                        // clock as the desktop, so the snow never stops and there is no loop to see.
                        LivePreview {
                            id: liveView
                            anchors.fill: parent
                            anchors.margins: 0
                            active: root.liveWanted
                            wantedId: root.hero ? root.hero.id : ""
                        }

                        Image {
                            id: heroImg
                            anchors.fill: parent
                            anchors.margins: 0
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            visible: !root.heroMotion && !root.heroLive
                            source: root.hero ? (root.hero.thumb_large !== undefined ? root.hero.thumb_large
                                                                                     : root.hero.thumb) : ""
                        }

                        // Every frame of the strip is loaded once and kept, so playing it back is a
                        // visibility flip. Pointing one Image at the next file every 42 ms instead
                        // costs a fresh read plus a 3-20 ms decode, and the picture lands a tick late.
                        Item {
                            id: frameStack
                            anchors.fill: parent
                            anchors.margins: 0
                            visible: root.heroMotion
                            Repeater {
                                // Only while the strip really is the picture on screen: a hidden Image
                                // still fetches its source, and 24 of them is 22 MB of textures for
                                // nobody to see - whether because the live pass is up or because this
                                // window is not the focused one.
                                model: root.heroMotion ? root.framesOf(root.hero) : []
                                delegate: Image {
                                    required property int index
                                    required property var modelData
                                    anchors.fill: parent
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    visible: index === root.previewFrame
                                    source: modelData
                                }
                            }
                        }
                    }

                    // The new picture, taken as a texture. Live only while the morph runs: an always
                    // on grab here would render the whole stack twice per frame for nothing.
                    ShaderEffectSource {
                        id: heroToSrc
                        sourceItem: heroContent
                        live: root.heroFx === root.fxLiquid
                        visible: false
                    }

                    // The captured outgoing frame. Loaded synchronously and uncached on purpose: the
                    // file was written microseconds ago, an asynchronous Image would leave the first
                    // transition frames empty (the new picture showing before the old one was ever
                    // seen), and the cache keys on the URL - which is the same path every time.
                    // In the liquid mode it is never painted at all, only sampled; a hidden sourceItem
                    // still yields a valid texture (measured), so the grab does not double-paint.
                    Image {
                        id: heroSnap
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: false
                        cache: false
                        // Dissolving: this *is* the overlay, so it paints for the whole fade.
                        // Morphing: it paints until the effect has taken over, then stands down -
                        // the effect is opaque over the same rect, so painting both is for nothing.
                        visible: root.heroFx === root.fxDissolve ? opacity > 0.001
                                 : root.heroFx === root.fxLiquid ? (!root.heroLiquidArmed && root.heroSnapFresh)
                                 : false
                        opacity: 0
                        source: root.snapUrl
                        NumberAnimation {
                            id: heroOutFade
                            target: heroSnap
                            property: "opacity"
                            from: 1
                            to: 0
                            duration: 280
                            easing.type: Easing.InOutQuad
                            // The morph clears its own state when it finishes; the fade has nothing
                            // else watching it, and leaving heroFx set would keep the capture painted
                            // in the layer list for the next switch's mode decision.
                            onFinished: if (root.heroFx === root.fxDissolve) root.heroFxStop()
                        }
                    }

                    ShaderEffectSource {
                        id: heroFromSrc
                        sourceItem: heroSnap
                        live: root.heroFx === root.fxLiquid
                        visible: false
                    }

                    // The morph itself: both textures displaced by the same field and crossfaded, so
                    // the picture churns as it swaps instead of being wiped. prog is driven by
                    // heroLiquidRun; the curve shapes live in the shader. The property order below is
                    // the uniform block's order in shaders/HeroLiquid.frag - it is not free.
                    ShaderEffect {
                        id: heroLiquid
                        anchors.fill: parent
                        visible: root.heroFx === root.fxLiquid && root.heroLiquidArmed
                        property variant fromTex: heroFromSrc
                        property variant toTex: heroToSrc
                        property real prog: 0
                        property real aspect: heroBox.height > 0 ? heroBox.width / heroBox.height : 1.6
                        property real scalePx: root.heroLiquidScalePx
                        property real stageW: root.heroLiquidStageW
                        property real cells: root.heroLiquidCells
                        property real rotRad: root.heroLiquidRotRad
                        property real rotBase: root.heroLiquidRotBase
                        fragmentShader: "HeroLiquid.frag.qsb"
                    }

                    NumberAnimation {
                        id: heroLiquidRun
                        target: heroLiquid
                        property: "prog"
                        from: 0
                        to: 1
                        duration: root.heroLiquidMs
                        onFinished: root.heroFxStop()
                    }

                    // The clock is a Timer aimed at the wall clock, not at a fixed interval: a
                    // 125 ms Timer was measured firing 116-135 ms apart, and Windows rounds timer
                    // wakeups to ~15.6 ms, which would be a quarter of a 42 ms frame period. Each
                    // tick re-aims at the next boundary, so the sequence holds 1000/frameMs fps
                    // forever instead of drifting. FrameAnimation was tried first and never fired
                    // while this window is not the one on screen, so it cannot be the only clock.
                    //
                    // It walks the strip forwards then backwards rather than wrapping. The strip is a
                    // one second slice of an animation that has no period, so the last frame and the
                    // first one are a whole second of motion apart: measured against a normal step,
                    // that jump is 4.7x on snowfall (45 % of pixels move >25 levels where a step moves
                    // 15 %) and 15.8x on aurora, which is the snap he saw every second. Reversing makes
                    // every seam an ordinary step, at the cost of the motion running backwards for a
                    // second each cycle.
                    Timer {
                        id: motionClock
                        repeat: true
                        running: root.heroMotion
                        interval: root.frameMs
                        onTriggered: {
                            const n = root.framesOf(root.hero).length
                            if (n < 2) return
                            const elapsed = Date.now() - root.motionT0
                            const step = Math.floor(elapsed / root.frameMs)
                            const period = 2 * n - 2
                            const p = step % period
                            root.previewFrame = p < n ? p : period - p
                            const due = (step + 1) * root.frameMs
                            interval = Math.max(1, Math.round(due - elapsed))
                        }
                    }

                    Rectangle {
                        id: captionBar
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        // Insets read off the rendered pixels, not off these numbers: the band at the
                        // top of the picture has 14 px above its chips and 10 px below them (chips ink
                        // bottom 72 -> band bottom edge 82), so this band gets the same 14 / 10. The
                        // reserve is 14 above + 8 between the two rows + 10 below the last row; with
                        // 24 here the apply row sat 2 px off the window's bottom edge.
                        height: capRow.implicitHeight + applyRow.implicitHeight + 32
                        radius: 0
                        color: "#9905080C"
                        // One row, as he asked: the name, its meta, then the two buttons that used to
                        // sit at the bottom of the page, then the favourite.
                        RowLayout {
                            id: capRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 14
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 10
                            Text {
                                id: nameLabel
                                text: root.hero ? root.displayName(root.hero.name) : ""
                                color: "white"; font.family: root.fontFamily
                                font.pixelSize: 20; font.weight: Font.DemiBold
                                elide: Text.ElideMiddle
                                // An imported photo keeps its whole file name, and with no cap the
                                // layout gave the label its hint width and pushed the meta off the
                                // bar. Middle elide so the hash prefix and the extension both stay
                                // visible. No hover bubble for the full name - he asked for that back
                                // out: the truncated form is what he wants to see, not a second copy.
                                Layout.maximumWidth: 300
                            }
                            Text {
                                text: root.hero
                                      ? (root.resText(root.hero) + " · " + trs("quality") + ": " + root.hero.quality
                                         + " · " + trs("base_fps") + ": " + root.hero.fps
                                         + " · " + trs("params") + ": "
                                         + (root.hero.params === undefined ? 0 : root.hero.params)
                                         + (root.particleCount(root.hero) > 0
                                            ? " · " + trs("particles") + ": " + root.particleCount(root.hero) : ""))
                                      : ""
                                color: "#C6CEDA"; font.family: root.fontFamily; font.pixelSize: 12
                            }
                            Item { Layout.fillWidth: true }
                            // On-state is the icon's own colour, like the favourite - not a border.
                            IconBtn {
                                id: motionBtn
                                // On a clip this button is the transport, not the strip switch: an
                                // imported video has no 24-frame strip by design (its card plays
                                // live), so the button that means 动/不动 on every other card had
                                // nothing to act on and sat disabled - which is how he read it:
                                // 「自己上传的视频，这个播放、暂停按钮不能用」.
                                readonly property bool isClip: root.hero !== null && root.hero.type === "video"
                                icon: "motion"
                                tip: isClip ? trs(Bridge.previewVideoPaused ? "video_resume" : "video_pause")
                                            : trs("preview_motion")
                                flat: true
                                veil: "#40FFFFFF"      // a dark veil on a dark bar is no feedback at all
                                glyph: 17
                                enabled: root.hero !== null && (isClip || root.framesOf(root.hero).length > 1)
                                // Red while it is moving, either way that is decided.
                                tint: isClip ? (Bridge.previewVideoPaused ? th.text : th.err)
                                             : (root.previewMotion ? th.err : th.text)
                                onClicked: {
                                    if (isClip) Bridge.previewVideoPaused = !Bridge.previewVideoPaused
                                    else root.previewMotion = !root.previewMotion
                                }
                                Hint { label: motionBtn.tip; hovered: motionBtn.hoverArea; place: "top" }
                            }
                            IconBtn {
                                id: playBtn
                                icon: root.carouselPlaying ? "pause" : "play"
                                tip: trs(root.carouselPlaying ? "carousel_pause" : "carousel_play")
                                flat: true
                                veil: "#40FFFFFF"
                                glyph: 17
                                enabled: root.shown.length > 1
                                tint: root.carouselPlaying ? th.err : th.text
                                onClicked: root.carouselPlaying = !root.carouselPlaying
                                Hint { label: playBtn.tip; hovered: playBtn.hoverArea; place: "top" }
                            }
                            // A plain 28 px cell, same as the two icon buttons beside it. The bubble
                            // does not render when it is declared inside the Canvas.
                            Item {
                                id: favCell
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                readonly property bool fav: root.hero !== null && Bridge.isFavorite(root.hero.id)
                                Icon {
                                    anchors.centerIn: parent
                                    name: favCell.fav ? "heart_fill" : "heart"
                                    px: 18
                                    tint: favCell.fav ? th.err : "#D8DEE8"
                                }
                                MouseArea {
                                    id: favHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: root.hero !== null
                                    // A clickable place has to say so under the pointer, and taking the
                                    // press also drops the caret out of whichever field still holds it.
                                    cursorShape: Qt.PointingHandCursor
                                    onPressed: forceActiveFocus()
                                    onClicked: if (root.hero) Bridge.toggleFavorite(root.hero.id)
                                }
                                Hint { label: favCell.fav ? trs("favorite_drop") : trs("favorite_add")
                                       ; hovered: favHover; place: "top" }
                            }
                            // The settings entry used to sit on the window's bottom-right corner, one
                            // row below the caption. He wants it in this row, after the favourite.
                            IconBtn {
                                id: gearBtn
                                icon: "gear"
                                tip: trs("settings")
                                flat: true
                                veil: "#40FFFFFF"
                                glyph: 17
                                tint: root.settingsOpen ? th.accent : th.text
                                onClicked: root.settingsOpen = !root.settingsOpen
                                Hint { label: gearBtn.tip; hovered: gearBtn.hoverArea; place: "top" }
                            }
                        }
                        // The apply controls, on the same scrim as the caption - he asked for this row
                        // to sit at the bottom of the picture like the one above it.
                        RowLayout {
                            id: applyRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: capRow.bottom
                            anchors.topMargin: 8
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 10

                            // The button carries the state instead of a chip beside the name: the same
                            // test the chip used (this card is up on some screen) now decides the
                            // label, and the button greys out rather than disappearing - a button
                            // that reads 正在显示 and still applies on click would be lying about
                            // one of the two.
                            Btn {
                                readonly property bool up: root.hero !== null && root.inUse(root.hero)
                                text: up ? trs("current") : trs("set_as_wallpaper")
                                accent: true
                                enabled: Bridge.connected && root.hero !== null && !up
                                onClicked: { if (root.hero) Bridge.apply(root.hero.id) }
                            }

                            Repeater {
                                model: [
                                    { key: "mode_current", sel: Bridge.targetMonitor !== "all" && !Bridge.rotateOn },
                                    { key: "mode_all", sel: Bridge.targetMonitor === "all" && !Bridge.rotateOn },
                                    { key: "mode_random", sel: Bridge.rotateOn }
                                ]
                                delegate: Item {
                                    required property var modelData
                                    Layout.preferredWidth: modeRow.implicitWidth
                                    Layout.preferredHeight: 28
                                    RowLayout {
                                        id: modeRow
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 6
                                        Rectangle {
                                            width: 16; height: 16; radius: 8
                                            color: th.surface
                                            border.color: modelData.sel ? th.accent : th.line
                                            border.width: 1
                                            Rectangle {
                                                anchors.centerIn: parent
                                                width: 8; height: 8; radius: 4
                                                visible: modelData.sel
                                                color: th.accent
                                            }
                                        }
                                        Text {
                                            text: trs(modelData.key)
                                            color: th.text; font.family: root.fontFamily; font.pixelSize: 12
                                        }
                                    }
                                    MouseArea {
                                            // A clickable place has to say so under the pointer, and taking the press also
                                            // drops the caret out of whichever field still holds it.
                                            cursorShape: Qt.PointingHandCursor
                                            onPressed: forceActiveFocus()   // takes the caret out of any field
                                        anchors.fill: parent
                                        onClicked: {
                                            if (modelData.key === "mode_random") {
                                                Bridge.setRotate(true)
                                            } else {
                                                Bridge.setRotate(false)
                                                Bridge.targetMonitor = modelData.key === "mode_all"
                                                                        ? "all"
                                                                        : (Bridge.monitors.length ? Bridge.monitors[0].tag : "all")
                                            }
                                        }
                                    }
                                }
                            }

                            // The trailing spacer is load bearing: with no Layout.fillWidth item in
                            // this row, the leftover width is shared out and the three radios drift
                            // apart across the band (measured ~330 px between neighbours on a 1320 px one).
                            Item { Layout.fillWidth: true }
                            // The renderer's readout rides on this row so it shares the options' centre
                            // line (「这个要和左边的选项 一条线啊」). Anchored to the window's bottom edge
                            // before, it sat 10 px below that line. Light first, then the numbers
                            // (「绿点显示在文字左边吧」).
                            Rectangle {
                                Layout.preferredWidth: 8
                                Layout.preferredHeight: 8
                                radius: 4
                                color: Bridge.connected ? th.ok : th.err
                            }
                            Text {
                                // Every number on this line is this program's own: the CPU percent comes
                                // from GetProcessTimes of the renderer process, the MB from its working
                                // set, the GPU percent from the engine instances that carry its pid.
                                // Saying 本程序 in front of them was tried and he took the words back out
                                // - the numbers are the point here. The card as a whole (整机) is not on
                                // this line; it is in 壁纸进程管理, under its own label, together with the
                                // 3D / video-decode split. The window behind the CPU percent lives there
                                // too: a percent without its window is what once read a 2-second burst as
                                // a sustained 177% load.
                                // The busiest single engine, not the sum: `gpuSelfPercent` adds every
                                // engine our pid owns (3D + decode + copy) and each is measured against
                                // its own 100%, so the total passed 100% and the line read "GPU 150%"
                                // (measured 2026-10-09 while a Hyper-V VM held 88% of the decoder). A
                                // percent over 100 next to a label that says GPU is not a number he can
                                // read, so the line carries the peak engine and the 3D / decode split
                                // stays in 壁纸进程管理 under its own labels.
                                readonly property real gpuPeak: Math.max(Bridge.gpuSelf3d,
                                                                         Bridge.gpuSelfDecode)
                                text: trs(Bridge.connected ? "running" : "stopped")
                                      + " · " + trs("stat_cpu") + " " + root.num(Bridge.cpuPercent, 2) + "%"
                                      + " · " + trs("stat_mem") + " " + root.num(Bridge.workingSetMb, 1) + " MB"
                                      + (gpuPeak >= 0
                                         ? " · " + trs("stat_gpu") + " " + root.num(gpuPeak, 0) + "%"
                                         : "")
                                color: "#C6CEDA"; font.family: root.fontFamily; font.pixelSize: 12
                            }
                        }
                    }
                    // The filters can empty the page without the renderer being empty; say so where
                    // the picture would have been, and leave the chips alone so it can be undone.
                    Text {
                        anchors.centerIn: parent
                        visible: root.shown.length === 0
                        text: trs("none")
                        color: th.muted; font.family: root.fontFamily; font.pixelSize: 13
                    }

                    // The category chips and the search box, moved onto the top of the picture
                    // with the same scrim the caption row uses. Declared after the picture layers
                    // because heroBox paints its children in order.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        // His ask: the row's own dark wash is gone, so the picture reaches the tabs.
                        // The box stays - it is what the chips and the search field are laid out in,
                        // and the bottom band still copies this one's 14 / 10 padding.
                        color: "transparent"
                        radius: 0
                        // 14 above the tabs and 10 below: the tab-to-wallpaper gap becomes the same 10
                        // the wallpaper uses between its own rows, instead of a height balanced by hand.
                        implicitHeight: chipsCol.height + 24

                        // With no title bar, this strip is what you drag the window by. Declared
                        // before the chips so a press on a chip or on the search field never lands
                        // here; the empty wash between them is the only grabbable part.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            cursorShape: Qt.OpenHandCursor
                            onPressed: root.startSystemMove()
                            // Same platform path as the button: setting `visibility` here would put
                            // Qt back on its two-step geometry route and the jump would be back.
                            onDoubleClicked: Bridge.showCommand(
                                    root.visibility === Window.Maximized ? 9 : 3)
                        }

                        ColumnLayout {
                            id: chipsCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.leftMargin: 16
                            // 152, not 16: the three window buttons sit at this strip's right end
                            // (40 px each, 6 apart, 8 in from the glass = 140 px) and the search box
                            // stops 12 short of them. Both numbers move together: there are two gaps,
                            // so every px added to the buttons' own spacing costs 2 px here, or the
                            // box and the first plate end up touching.
                            anchors.rightMargin: 152
                            anchors.topMargin: 14
                            spacing: 10

                            // One row only: the category chips, with the search box at their right end.
                            RowLayout {
                                id: chipRow
                                Layout.fillWidth: true
                                spacing: 10

                                Row {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 28
                                    spacing: 6
                                    // Favourites is one more tab, not a second filter axis: picking it
                                    // drops the category exactly the way picking 极光星空 does.
                                    // Imported photos carry "本地图片" in their wallpaper.json, so the
                                    // order names that string rather than looking it up in the
                                    // translation table - the data is Chinese in both languages.
                                    //
                                    // The two imported kinds sit together at the right end, photos first
                                    // then videos (he asked for 本地视频 to the right of 本地图片 on
                                    // 2026-10-08). Nothing else about the rule changed: 本地图片 is still
                                    // always there, 本地视频 still only appears once a clip is in the
                                    // library, which is where it came from before.
                                    Repeater {
                                        model: ["all"]
                                               .concat(Object.keys(root.categoryCounts).filter(
                                                       function (k) { return k !== "本地图片" && k !== "本地视频" }))
                                               .concat(["fav", "本地图片"])
                                               .concat(root.categoryCounts["本地视频"] ? ["本地视频"] : [])
                                        delegate: Btn {
                                            required property var modelData
                                            readonly property string cat: modelData === "all" ? "" : modelData
                                            text: cat === "" ? trs("all") : (cat === "fav" ? trs("favorites") : cat)
                                            compact: true
                                            frame: true
                                            alpha: root.plateAlpha
                                            checked: root.category === cat
                                            onClicked: root.category = cat
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 260
                                    Layout.preferredHeight: 28
                                    radius: 6   // same corner as the chips beside it
                                    color: Qt.alpha(search.activeFocus ? th.surfaceAlt : th.surface,
                                                    root.plateAlpha)
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 4
                                        spacing: 6
                                        Icon { name: "search"; px: 14; tint: th.muted }
                                        TextField {
                                            id: search
                                            Layout.fillWidth: true
                                            placeholderText: trs("search_placeholder")
                                            color: th.text
                                            placeholderTextColor: th.muted
                                            background: null
                                            // The style's own leftPadding is `padding + 4` = 10 px, which
                                            // stacked on the 6 px row spacing to put 18 px of nothing
                                            // between the magnifier and the first glyph. The row's own
                                            // 12 / 6 insets are ours; this one is nobody's.
                                            leftPadding: 0
                                            font.family: root.fontFamily
                                            font.pixelSize: 12
                                            onTextChanged: root.query = text
                                        }
                                        IconBtn {
                                            visible: search.text.length > 0
                                            icon: "close"
                                            tip: trs("clear_search")
                                            onClicked: search.text = ""
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // The small cards: the carousel's navigator, floating over the bottom of the big
                    // picture and above the caption. Clicking one stops the rotation. They stay
                    // landscape like the sheet he pasted - only the selected one grows.
                    //
                    // Deliberately not a ListView. With the selected card 40 px wider than its
                    // neighbours, neither positionViewAtIndex() nor the highlight range lands the
                    // widened card on the centre (measured 40 px off on the middle slide, +40 on the
                    // last), and the content margins that were meant to let the first and last card
                    // reach the middle never count toward contentWidth (Qt logged width=1236,
                    // leftMargin=524, contentWidth=512), so every contentX was clamped away.
                    //
                    // The whole list is now laid out as one row in row coordinates and that row is
                    // slid under the window: centred on the selected card until an end of the list
                    // reaches an edge, then it stops there and the selected card walks off-centre.
                    // Cards that have left the window stop loading their picture, so the row covers
                    // the full width instead of parking every slide's texture in memory.
                    //
                    // The hand-over between two slides is animated over strip.animMs: the row's offset,
                    // the two cards' geometry and their frame colours move on one clock, so a step
                    // reads as one move. That is also why the Repeater is keyed on root.shown and not
                    // on the in-window subset: a model that changes when the selection changes is a
                    // full reset, which destroys every delegate and rebuilds it already sitting at its
                    // destination, and there is nothing left to animate. The subset is a per-card
                    // `visible` instead, so the cards that are off-window still cost no image load.
                    Item {
                        id: strip
                        // 260 ms is the whole move: long enough to read the two cards swapping roles,
                        // short enough that a 4 s rotation is over well before the next one.
                        readonly property int animMs: 260
                        property real bigW: 188
                        property real smallW: 148
                        property real bigH: 117
                        property real smallH: 92
                        property real gap: 14
                        // The rim sits this much OUTSIDE the picture's own octagon. He asked for
                        // "边框比里面图片的边框更外扩一点" without a number, so it is 1.6% of the 188 px
                        // selected card (3 px) - the smallest value that reads as a deliberate gap
                        // rather than a misregistration. Re-measure before changing the card size.
                        property real picInset: 3
                        readonly property int n: root.shown.length
                        readonly property int sel: root.slideIndex()
                        readonly property real pitch: smallW + gap
                        readonly property real rowW: bigW + (n - 1) * pitch
                        // The row's left edge in strip coordinates - the offset the row glides TO.
                        readonly property real rowX: {
                            if (rowW <= width) return (width - rowW) / 2
                            return Math.max(width - rowW,
                                            Math.min(0, (width - bigW) / 2 - sel * pitch))
                        }
                        function cardW(i) { return i === sel ? bigW : smallW }
                        function slotX(i) { return i * pitch + (i > sel ? bigW - smallW : 0) }
                        function cardX(i) { return rowX + slotX(i) }
                        // Which cards exist over the window. Measured against the offset the row is
                        // heading for, with a pitch of slack on each side: a card that has just been
                        // pushed out has to stay alive while the row carries it clear, or it blinks
                        // out mid-glide. The slack is why `visible` and not the model decides what is
                        // on screen - see the note above the strip.
                        function live(i) {
                            if (i < 0 || i >= n) return false
                            var x = cardX(i)
                            return x + cardW(i) > -pitch && x < width + pitch
                        }
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: captionBar.top
                        anchors.bottomMargin: 12
                        height: bigH
                        // No clip: the halo around the selected card is meant to spill onto the
                        // picture and the cards beside it. heroBox clips what runs past the window.

                        function step() {   // the rotation moving on its own
                            if (root.shown.length === 0) return
                            root.selectedId = root.shown[(root.slideIndex() + 1) % root.shown.length].id
                            root.motionT0 = Date.now()
                        }
                        function go(i) {
                            if (i < 0 || i >= root.shown.length) return
                            root.selectedId = root.shown[i].id
                            root.motionT0 = Date.now()
                            root.carouselPlaying = false   // a manual pick outranks the rotation
                        }

                        Repeater {
                            model: root.shown
                            delegate: Item {
                                id: thumbCell
                                required property int index
                                required property var modelData
                                readonly property int src: index
                                readonly property var slide: modelData
                                readonly property bool sel: src === strip.sel
                                readonly property bool onscreen: strip.live(src)
                                width: strip.cardW(src)
                                height: sel ? strip.bigH : strip.smallH
                                x: strip.cardX(src)
                                y: strip.bigH - height
                                z: sel ? 1 : 0
                                visible: onscreen
                                Behavior on x { NumberAnimation { duration: strip.animMs
                                                                  easing.type: Easing.OutCubic } }
                                Behavior on width { NumberAnimation { duration: strip.animMs
                                                                      easing.type: Easing.OutCubic } }
                                Behavior on height { NumberAnimation { duration: strip.animMs
                                                                       easing.type: Easing.OutCubic } }
                                // No Behavior on y: it is bigH - height, so it follows the height
                                // animation frame for frame and the cards stay on one bottom line.

                                // The glow: only the HUD marks are seeded, so the light gathers at the
                                // four corners and the bottom core instead of ringing the whole card.
                                TechFrame {
                                    id: haloSeed
                                    anchors.fill: parent
                                    seedOnly: true
                                    // Painted only for the selected card: the cut corners no longer
                                    // have a picture over them, so a seed that painted for idle cards
                                    // too would show its bright marks straight through those wedges.
                                    glow: thumbCell.sel ? th.accent : "transparent"
                                    Behavior on glow { ColorAnimation { duration: strip.animMs } }
                                }
                                MultiEffect {
                                    source: haloSeed
                                    anchors.fill: haloSeed
                                    blurEnabled: true
                                    blur: 1.0
                                    blurMax: 32
                                    autoPaddingEnabled: true
                                    // Faded, not switched: the halo is the one mark that would still
                                    // cut at the two ends of the glide. visible rides on the same
                                    // number so the blur pass is not paid for by an idle card.
                                    opacity: thumbCell.sel ? 1 : 0
                                    visible: opacity > 0.001
                                    Behavior on opacity { NumberAnimation { duration: strip.animMs
                                                                            easing.type: Easing.OutCubic } }
                                }

                                // The cut corners are real: the picture is masked to the octagon, so what
                                // shows in the four corners is the page behind the card. The mask is the
                                // same geometry painted solid (an opacity mask needs an opaque body - its
                                // RGB is irrelevant, only its alpha is read), and while a picture is up it
                                // stays underneath it, so the white never reaches the screen.
                                //
                                // It is painted ONLY while the picture is up. That is what fixes the white
                                // tile: when the renderer refuses to rewrite a cover the Image has no
                                // source at all, and an unmasked solid octagon showed as 56.6% pure
                                // #FFFFFF (measured 2026-10-09, local_05). Filling the card with a dark
                                // colour instead - the first attempt - is what he is rejecting now: it
                                // paints over the four corners, which are supposed to be the page.
                                TechFrame {
                                    id: picMask
                                    anchors.fill: parent
                                    anchors.margins: strip.picInset   // the rim sits outside the picture
                                    body: "#FFFFFFFF"
                                    visible: picImg.status === Image.Ready
                                }
                                Rectangle {
                                    id: card
                                    anchors.fill: parent
                                    anchors.margins: strip.picInset
                                    color: "transparent"
                                    Image {
                                        id: picImg
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        // Always opaque: the octagon it is cut by sits underneath it,
                                        // and any alpha at all lets that mask through. Measured on an
                                        // idle card at the old 0.55, the darks of the picture were
                                        // lifted from p5 14.9 to 135.9 - a white veil, which is what he
                                        // is rejecting here. The idle cards recede by rim colour and
                                        // size, not by fading.
                                        layer.enabled: true
                                        layer.effect: MultiEffect {
                                            maskEnabled: true
                                            maskSource: picMask
                                        }
                                        source: thumbCell.onscreen && thumbCell.slide.thumb !== undefined
                                                ? thumbCell.slide.thumb : ""
                                    }
                                }
                                // On top of the picture: the rim, its inner echo, the broken side lines
                                // and the HUD marks. Same geometry on every card. **Every colour on the
                                // frame follows the selection** - the corner brackets and the two gems
                                // are the loudest marks on the card. They used to carry a per-wallpaper
                                // "rarity" colour, which made the highlight a different hue on every
                                // card - and idle cards kept it too, so nothing read as selected.
                                // Selected is now the app's one accent blue everywhere; idle = steel.
                                TechFrame {
                                    anchors.fill: parent
                                    primary: thumbCell.sel ? th.accent : Qt.alpha(th.muted, 0.62)
                                    secondary: thumbCell.sel ? th.accentDim : Qt.alpha(th.muted, 0.55)
                                    glow: thumbCell.sel ? th.accent : Qt.alpha(th.muted, 0.72)
                                    // Steel to accent and back, on the same clock as the card's move:
                                    // a rim that snaps to a new colour while the box beside it glides
                                    // is the one thing that would still read as a cut.
                                    Behavior on primary { ColorAnimation { duration: strip.animMs } }
                                    Behavior on secondary { ColorAnimation { duration: strip.animMs } }
                                    Behavior on glow { ColorAnimation { duration: strip.animMs } }
                                }
                                MouseArea {
                                        // A clickable place has to say so under the pointer, and taking the press also
                                        // drops the caret out of whichever field still holds it.
                                        cursorShape: Qt.PointingHandCursor
                                        onPressed: forceActiveFocus()   // takes the caret out of any field
                                    anchors.fill: parent
                                    onClicked: strip.go(thumbCell.src)
                                }
                            }
                        }

                        // Prev / next at the two ends of the row: they move the selection, and the row
                        // slides under the window to follow. Disabled rather than hidden at an end of
                        // the list, so the pair does not appear and disappear between slides.
                        // On the idle cards' centre line, not the strip's: the idle cards are 25 px
                        // shorter than the strip and sit on its bottom edge, so the strip's centre is
                        // (bigH - smallH) / 2 above theirs (measured: chevron ink 695.5 vs card ink
                        // 707.5).
                        IconBtn {
                            icon: "chevL"
                            tip: trs("prev")
                            flat: true
                            box: 44
                            glyph: 34
                            stroke: 2.6
                            tint: th.warn
                            z: 2
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: (strip.bigH - strip.smallH) / 2
                            anchors.leftMargin: 12
                            enabled: strip.sel > 0
                            onClicked: strip.go(strip.sel - 1)
                        }
                        IconBtn {
                            icon: "chevR"
                            tip: trs("next")
                            flat: true
                            box: 44
                            glyph: 34
                            stroke: 2.6
                            tint: th.warn
                            z: 2
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: (strip.bigH - strip.smallH) / 2
                            anchors.rightMargin: 12
                            enabled: strip.sel < strip.n - 1
                            onClicked: strip.go(strip.sel + 1)
                        }
                    }
                }


            }

            // The carousel hides itself only when the renderer has nothing at all; a filter that matched
            // nothing is explained inside the picture box. The line that says why used to sit here as a
            // row of this column - see the centred overlay after the column closes.
        }
    }

    // "The renderer is not running" / "nothing in the library", with the settings entry next to it:
    // with the caption band inside the column that collapses in this state, the entry has to ride along
    // with the explanation, and the two gears are never on screen together.
    //
    // Centred on the window rather than laid out inside the grid column: measured 2026-10-09, as a
    // `Layout.alignment: Qt.AlignHCenter` row of that column it landed with its ink at x 0..413 of a
    // 1360 px window - the column sizes itself to its content once the carousel is hidden, so the row
    // had nothing wider than itself to centre in (its own box was 453 px, at the left edge).
    RowLayout {
        id: emptyHintRow
        anchors.centerIn: parent
        visible: Bridge.catalog.length === 0
        spacing: 12
        Text {
            id: emptyHintText
            text: Bridge.connected ? trs("no_catalog") : trs("not_connected_hint")
            color: th.muted; font.family: root.fontFamily; font.pixelSize: 13
        }
        IconBtn {
            id: emptyGear
            icon: "gear"
            tip: trs("settings")
            glyph: 17
            tint: root.settingsOpen ? th.accent : th.text
            onClicked: root.settingsOpen = !root.settingsOpen
            Hint { label: emptyGear.tip; hovered: emptyGear.hoverArea; place: "top" }
        }
    }

    // -------------------------------------------------------------- settings dialog
    // A dialog over the page, not a fourth column: opening it must not move the grid. The left
    // strip is the category list, so each tab holds one subject instead of one long scroll.
    Rectangle {
        id: settingsLayer
        anchors.fill: parent
        visible: root.settingsOpen
        color: "#A605080C"

        MouseArea { anchors.fill: parent; onClicked: { root.settingsOpen = false; root.contentItem.forceActiveFocus() } }

        Rectangle {
            id: settingsCard
            anchors.centerIn: parent
            width: Math.min(parent.width - 140, 880)
            height: Math.min(parent.height - 120, 496)
            radius: 10
            // A floating card is the darkest surface, so every control on it is one shade up:
            // with the borders gone that step is the only thing that reads.
            color: th.bg

            // Declared first so every other child lands on top of it. A Rectangle does not consume
            // the mouse, so before this a click on any empty part of the card - the whole header row
            // except the cross - fell through to the scrim behind and closed the dialog.
            // Clicking the dialog's own empty space is "somewhere else": it has to end
            // an edit, or the caret keeps blinking in a field nobody is using.
            MouseArea { anchors.fill: parent; onPressed: root.contentItem.forceActiveFocus() }

            Rectangle {
                id: setNav
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 176
                radius: 10
                color: th.rail

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 4

                    Repeater {
                        model: root.settingsTabs
                        delegate: Rectangle {
                            id: navRow
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: 38
                            radius: 8
                            property bool on: root.settingsTab === navRow.modelData.key
                            color: setNavHover.containsMouse ? th.surface : (on ? th.surfaceAlt : "transparent")
                            RowLayout {
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 9
                                Icon {
                                    name: navRow.modelData.icon
                                    px: 16
                                    tint: navRow.on ? th.text : th.muted
                                }
                                Text {
                                    text: trs(navRow.modelData.label)
                                    color: navRow.on ? th.text : th.muted
                                    font.family: root.fontFamily; font.pixelSize: 13
                                }
                            }
                            MouseArea {
                                    // A clickable place has to say so under the pointer, and taking the press also
                                    // drops the caret out of whichever field still holds it.
                                    cursorShape: Qt.PointingHandCursor
                                    onPressed: forceActiveFocus()   // takes the caret out of any field
                                id: setNavHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: { root.settingsTab = navRow.modelData.key; root.openField = null }
                            }
                        }
                    }
                }
            }

            RowLayout {
                id: setHead
                anchors.left: setNav.right
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 20
                anchors.rightMargin: 14
                anchors.topMargin: 16
                height: 30
                spacing: 8
                Text {
                    text: root.settingsTabLabel()
                    color: th.text; font.family: root.fontFamily; font.pixelSize: 17; font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                IconBtn { icon: "close"; tip: trs("close"); onClicked: root.settingsOpen = false }
            }

            Flickable {
                anchors.left: setNav.right
                anchors.right: parent.right
                anchors.top: setHead.bottom
                anchors.bottom: parent.bottom
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                anchors.topMargin: 14
                anchors.bottomMargin: 18
                contentHeight: tabCol.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // The list is anchored to the window, not to the row, so scrolling has to drop it -
                // otherwise it keeps pointing at where the field used to be.
                onContentYChanged: root.openField = null
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                ColumnLayout {
                    id: tabCol
                    width: parent.width
                    spacing: 14

                    // ---------------------------------------------------------- 通用
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.settingsTab === "general"
                        spacing: 14

                        Group {
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text { text: trs("language"); color: th.muted; font.family: root.fontFamily; font.pixelSize: 12 }
                                Item { Layout.fillWidth: true }
                                Repeater {
                                    model: ["zh", "en"]
                                    delegate: Btn {
                                        text: modelData === "zh" ? "中" : "EN"
                                        compact: true
                                        raised: true
                                        checked: Bridge.lang === modelData
                                        onClicked: Bridge.lang = modelData
                                    }
                                }
                            }
                        }

                        Group {
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("quality")
                                    color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Choice {
                                    items: root.qualityItems
                                    value: Bridge.quality
                                    enabled: Bridge.connected
                                    onPicked: function (v) { Bridge.setQuality(v) }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("fps_cap")
                                    color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                NumField { value: Bridge.maxFps; enabled: Bridge.connected; onCommitted: function(v) { Bridge.setMaxFps(v) } }
                            }
                        }

                        // How a picture that is not the screen's shape gets laid over the screen. The
                        // desktop itself changes as you pick, and so does the big picture on the home
                        // page, which draws the same shader into a smaller surface.
                        Group {
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("image_fit")
                                    color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Choice {
                                    items: root.fitItems
                                    value: Bridge.imageFit
                                    enabled: Bridge.connected
                                    onPicked: function (v) { Bridge.setFit(v) }
                                }
                            }
                        }

                        // How the big preview changes when another wallpaper is picked. This one is
                        // drawn by the settings window itself, so it stays live with the renderer
                        // stopped and is not gated on the pipe.
                        Group {
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("transition")
                                    color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Choice {
                                    items: root.transitionItems
                                    value: Bridge.transition
                                    onPicked: function (v) { Bridge.setTransition(v) }
                                }
                            }
                        }

                        // Auto-rotate lives here, not on the browse page.
                        Group {
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("enable_auto_rotate")
                                    color: th.text; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Slide {
                                    checked: Bridge.rotateOn
                                    enabled: Bridge.connected
                                    onToggled: function (on) { Bridge.setRotate(on) }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("interval")
                                    color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Choice {
                                    items: root.intervalItems
                                    value: Bridge.rotateIntervalMin
                                    enabled: Bridge.connected
                                    onPicked: function (v) { Bridge.setRotateInterval(v) }
                                }
                            }
                            Text {
                                visible: Bridge.rotateOn
                                text: trs("next_switch") + ": " + root.num(Bridge.rotateNextInS / 60, 1) + " min"
                                color: th.muted; font.family: root.fontFamily; font.pixelSize: 11
                            }
                        }

                    }

                    // ---------------------------------------------------------- 显示器
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.settingsTab === "monitors"
                        spacing: 14

                        Repeater {
                            model: Bridge.monitors
                            delegate: Group {
                                required property var modelData
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    // The block has no title any more, so the monitor's own name is
                                    // the first thing in its first row - without it the per-screen
                                    // numbers say nothing about which screen they belong to.
                                    Text { text: modelData.tag; color: th.text; font.family: root.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold }
                                    Text { text: modelData.px; color: th.muted; font.family: root.fontFamily; font.pixelSize: 12 }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: root.num(modelData.measured_fps, 1) + " / " + modelData.effective_fps
                                        color: Number(modelData.effective_fps) > 0 ? th.ok : th.warn
                                        font.family: root.fontFamily; font.pixelSize: 12
                                    }
                                    Text { text: "fps"; color: th.muted; font.family: root.fontFamily; font.pixelSize: 11 }
                                }
                                Text {
                                    text: modelData.wallpaperName + " · " + trs("state") + ": " + trs("state_" + modelData.state)
                                        + " · " + trs("avg_frame") + " " + modelData.avg_frame_ms + " ms"
                                        + " · " + trs("worst_frame") + " " + modelData.worst_frame_ms + " ms"
                                    color: th.muted; font.family: root.fontFamily; font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: trs("frames") + " " + modelData.frames + " · " + trs("corner_changed") + " "
                                        + modelData.corner_samples_changed + " · " + trs("corner_static") + " "
                                        + modelData.corner_samples_static + " · " + trs("draw_errors") + ": " + modelData.draw_errors
                                    color: Number(modelData.draw_errors) > 0 ? th.err : th.muted
                                    font.family: root.fontFamily; font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                                Text {
                                    visible: String(modelData.error).length > 0
                                    text: trs("error") + ": " + modelData.error
                                    color: th.err; font.family: root.fontFamily; font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Text {
                                        text: trs("monitor_fps_cap")
                                        color: th.muted; font.family: root.fontFamily; font.pixelSize: 11
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                    }
                                    NumField {
                                        value: modelData.assign_fps === undefined ? 0 : modelData.assign_fps
                                        enabled: Bridge.connected
                                        onCommitted: function(v) { Bridge.setMonitorFps(modelData.tag, v) }
                                    }
                                }
                            }
                        }
                    }

                    // ---------------------------------------------------------- 省电
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.settingsTab === "power"
                        spacing: 14

                        Text {
                            text: trs("power_hint")
                            color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Group {
                            Repeater {
                                model: Bridge.powerCaps
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Text {
                                        text: modelData.label
                                        color: th.text; font.family: root.fontFamily; font.pixelSize: 12
                                        Layout.fillWidth: true
                                    }
                                    NumField {
                                        value: modelData.value
                                        enabled: Bridge.connected
                                        onCommitted: function(v) { Bridge.setPowerCap(modelData.key, v) }
                                    }
                                }
                            }
                        }
                    }

                    // ---------------------------------------------------------- 壁纸
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.settingsTab === "wallpapers"
                        spacing: 14

                        Group {
                            // The two actions on one line, equal share of the width: stacked they read
                            // as two unrelated buttons, side by side as the one block they are.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                Btn { raised: true; Layout.fillWidth: true; text: trs("add_image"); enabled: Bridge.connected; onClicked: Bridge.addImageFile() }
                                Btn { raised: true; Layout.fillWidth: true; text: trs("add_folder"); enabled: Bridge.connected; onClicked: Bridge.addImageFolder() }
                                Btn { raised: true; Layout.fillWidth: true; text: trs("reload"); enabled: Bridge.connected; onClicked: Bridge.reload() }
                            }
                            Text {
                                visible: root.localImages.length === 0
                                text: trs("no_local_images")
                                color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                                Layout.fillWidth: true
                            }
                            // The imported copies, one at a time and as wide as the card: a 150 px tile
                            // cannot show what a picture looks like, which is the only reason to look
                            // at it here. The arrows wrap so the pair never goes grey mid-browse, and
                            // the dots are the position readout.
                            //
                            // It does not advance on its own. A rotation that moves the target under
                            // the pointer turns the cross into a roulette, and this list is the one
                            // place in the window that deletes files.
                            ColumnLayout {
                                id: car
                                visible: root.localImages.length > 0
                                Layout.fillWidth: true
                                spacing: 8
                                readonly property int n: root.localImages.length
                                property int wanted: 0
                                property bool armed: false
                                // Clamped on the way out: deleting the last item shortens the list
                                // under us, and an index past the end would read as a blank slide.
                                readonly property int sel: n > 0 ? Math.min(wanted, n - 1) : 0
                                readonly property var item: n > 0 ? root.localImages[sel] : null
                                function go(d) { if (n > 0) wanted = (sel + d + n) % n }
                                function to(i) { wanted = i }
                                onSelChanged: { car.armed = false; carDisarm.stop() }
                                // Arming does not stay armed: the pointer can drift, the mind can
                                // move on, and the second press is the one that deletes a file.
                                Timer { id: carDisarm; interval: 3000; onTriggered: car.armed = false }

                                Rectangle {
                                    id: slide
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 200
                                    radius: 6
                                    color: "#0A0D12"
                                    clip: true

                                    Image {
                                        anchors.fill: parent
                                        source: car.item && car.item.thumb !== undefined ? car.item.thumb : ""
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                    }
                                    MouseArea { id: overSlide; anchors.fill: parent; hoverEnabled: true }

                                    IconBtn {
                                        icon: "chevL"; tip: trs("prev"); flat: true
                                        box: 40; glyph: 30; stroke: 2.6; tint: th.text
                                        anchors.left: parent.left
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.leftMargin: 8
                                        enabled: car.n > 1
                                        onClicked: car.go(-1)
                                    }
                                    IconBtn {
                                        icon: "chevR"; tip: trs("next"); flat: true
                                        box: 40; glyph: 30; stroke: 2.6; tint: th.text
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.rightMargin: 8
                                        enabled: car.n > 1
                                        onClicked: car.go(1)
                                    }

                                    // The cross keeps the card's own rule: it exists only while the
                                    // pointer is on the slide, and the first press only arms it.
                                    Rectangle {
                                        id: badge
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.margins: 6
                                        width: badgeRow.width + 14
                                        height: 20
                                        radius: 4   // 20 px tall plate: 10 was a pill, 6 still read round
                                        visible: overSlide.containsMouse || car.armed
                                        color: car.armed ? th.errDim : "#CC1B2029"
                                        Row {
                                            id: badgeRow
                                            anchors.centerIn: parent
                                            spacing: 4
                                            Icon {
                                                visible: !car.armed
                                                name: "close"; px: 10; tint: "white"
                                                anchors.verticalCenter: parent.verticalCenter
                                            }
                                            Text {
                                                visible: car.armed
                                                text: trs("confirm_delete")
                                                color: "white"; font.family: root.fontFamily; font.pixelSize: 11
                                                anchors.verticalCenter: parent.verticalCenter
                                            }
                                        }
                                    }
                                    MouseArea {
                                            // A clickable place has to say so under the pointer, and taking the press also
                                            // drops the caret out of whichever field still holds it.
                                            cursorShape: Qt.PointingHandCursor
                                            onPressed: forceActiveFocus()   // takes the caret out of any field
                                        anchors.centerIn: badge
                                        width: Math.max(badge.width, 28)
                                        height: 28
                                        enabled: badge.visible
                                        onClicked: {
                                            if (!car.item) return
                                            if (car.armed) { Bridge.deleteImage(car.item.id); car.armed = false; carDisarm.stop() }
                                            else { car.armed = true; carDisarm.restart() }
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Text {
                                        text: car.item ? car.item.name : ""
                                        color: th.text; font.family: root.fontFamily; font.pixelSize: 11
                                        wrapMode: Text.WrapAnywhere
                                        Layout.fillWidth: true
                                    }
                                    Row {
                                        spacing: 6
                                        Layout.alignment: Qt.AlignVCenter
                                        Repeater {
                                            model: car.n
                                            delegate: Rectangle {
                                                required property int index
                                                width: 8; height: 8; radius: 4
                                                color: index === car.sel ? th.accent : th.surfaceAlt
                                                MouseArea {
                                                        // A clickable place has to say so under the pointer, and taking the press also
                                                        // drops the caret out of whichever field still holds it.
                                                        cursorShape: Qt.PointingHandCursor
                                                        onPressed: forceActiveFocus()   // takes the caret out of any field
                                                    anchors.fill: parent
                                                    anchors.margins: -4      // 8 px of dot is not a target
                                                    onClicked: car.to(index)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }


                        // The taskbar's opacity: the one setting here that changes something outside
                        // this program, and the reason it is a range rather than a switch is that the
                        // bar's window alpha moves how much of the wallpaper reaches it - which is
                        // worth something only while a wallpaper is what is behind the bar.
                        Group {
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("tray_alpha")
                                    color: th.muted; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Range {
                                    value: Bridge.trayAlpha
                                    enabled: Bridge.connected
                                    onCommitted: function (v) { Bridge.setTrayAlpha(v) }
                                }
                            }
                        }
                    }

                    // ---------------------------------------------------------- 渲染器
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.settingsTab === "renderer"
                        spacing: 14

                        Group {
                            StatLine { k: trs("host_layer"); v: Bridge.connected ? Bridge.hostMethod : trs("none") }
                            StatLine { k: trs("cpu"); v: root.num(Bridge.cpuPercent, 2) + " % · "
                                                    + trs("cpu_over") + " "
                                                    + root.num(Bridge.cpuWindowS, 1) + " s" }
                            StatLine { k: trs("working_set"); v: root.num(Bridge.workingSetMb, 1) + " MB" }
                            StatLine { visible: Bridge.gpuSelfPercent >= 0
                                k: trs("gpu_self")
                                v: root.num(Bridge.gpuSelfPercent, 1) + " % · " + trs("gpu_3d") + " "
                                  + root.num(Bridge.gpuSelf3d, 1) + " % · " + trs("gpu_decode") + " "
                                  + root.num(Bridge.gpuSelfDecode, 1) + " %" }
                            StatLine { visible: Bridge.gpuPercent >= 0
                                k: trs("gpu_machine"); v: root.num(Bridge.gpuPercent, 0) + " %" }
                            StatLine { k: trs("uptime"); v: root.num(Bridge.uptimeSeconds, 0) + " s" }
                        }

                        Group {
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("autostart")
                                    color: th.text; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Slide {
                                    checked: Bridge.autostart
                                    enabled: Bridge.connected
                                    onToggled: function (on) { Bridge.setAutostart(on) }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: trs("pause")
                                    color: th.text; font.family: root.fontFamily; font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Slide {
                                    checked: Bridge.paused
                                    enabled: Bridge.connected
                                    onToggled: function (on) { Bridge.setPaused(on) }
                                }
                            }
                            // One row of three: they are the same kind of action and each label is
                            // short enough to fit a third of the card.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Btn { raised: true; Layout.fillWidth: true; text: trs("start"); enabled: !Bridge.connected; onClicked: Bridge.startRenderer() }
                                Btn { raised: true; Layout.fillWidth: true; text: trs("quit"); enabled: Bridge.connected; onClicked: Bridge.quitRenderer() }
                                Btn { raised: true; Layout.fillWidth: true; text: trs("log_folder"); enabled: Bridge.connected; onClicked: Bridge.openLogFolder() }
                            }
                        }
                    }

                    // A command result belongs to the page that produced it and to three seconds. The
                    // actions are spread over four pages (import/delete on 壁纸, fps on 显示器, caps on
                    // 省电, start/quit on 渲染器), so hiding every message that was not born on 壁纸
                    // would leave those three with no feedback at all - hence "born here, dies here".
                    Rectangle {
                        visible: Bridge.lastMessage.length > 0 && root.msgLive
                                 && root.msgTab === root.settingsTab
                        Layout.fillWidth: true
                        // Layout.preferredHeight, not height: qmllint calls a bare `height` on a
                        // layout-managed item undefined behaviour, and this row's visibility now flips
                        // every three seconds, which is exactly when that would show.
                        Layout.preferredHeight: msgLbl.height + 16
                        radius: 6
                        color: Bridge.lastMessageIsError ? "#2A1A1C" : "#16231C"
                        Text {
                            id: msgLbl
                            x: 8
                            y: 8
                            width: parent.width - 16
                            text: Bridge.lastMessage
                            color: Bridge.lastMessageIsError ? th.err : th.ok
                            font.family: root.fontFamily; font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                        Connections {
                            target: Bridge
                            function onMessageChanged() {
                                root.msgTab = root.settingsTab
                                root.msgLive = true
                                msgHide.restart()
                            }
                        }
                        Timer {
                            id: msgHide
                            interval: 3000
                            onTriggered: root.msgLive = false
                        }
                    }
                }
            }
        }
    }

    // While the option list is up, a transparent layer under it takes the next click anywhere in the
    // window: that is what "clicking elsewhere hides the popup" means here, and the same press drops
    // the caret out of a field. The list itself sits above this layer, so its own rows still work.
    Rectangle {
        anchors.fill: parent
        visible: root.openField !== null
        color: "transparent"
        z: 900
        MouseArea {
            anchors.fill: parent
            onClicked: { root.openField = null; root.contentItem.forceActiveFocus() }
        }
    }

    // One option list shared by every Choice, parented to the window instead of to the field. An item
    // only receives the mouse inside its ancestors' rects, so a list hanging below a group card's own
    // rect was unreachable there - which read as "only the first option can be picked".
    Rectangle {
        id: dropList
        readonly property Item field: root.openField
        readonly property int listHeight: dropCol.implicitHeight + 8
        readonly property int belowY: field ? field.mapToItem(dropList.parent, 0, field.height + 4).y : 0
        readonly property bool flipUp: field !== null && belowY + listHeight > root.height - 12
        visible: field !== null && root.settingsOpen
        x: field ? field.mapToItem(dropList.parent, 0, 0).x : 0
        y: flipUp ? field.mapToItem(dropList.parent, 0, 0).y - listHeight - 4 : belowY
        width: field ? Math.max(field.width, 150) : 150
        height: listHeight
        z: 1000
        radius: 6
        color: th.surfaceAlt
        border.color: th.hover

        Column {
            id: dropCol
            x: 4
            y: 4
            width: dropList.width - 8
            Repeater {
                model: dropList.field ? dropList.field.items : []
                delegate: Rectangle {
                    required property var modelData
                    width: dropCol.width
                    height: 28
                    radius: 4
                    color: optHover.containsMouse ? th.hover
                         : (dropList.field && modelData.value === dropList.field.value ? th.surface : "transparent")
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: th.text; font.family: root.fontFamily; font.pixelSize: 12
                    }
                    MouseArea {
                            // A clickable place has to say so under the pointer, and taking the press also
                            // drops the caret out of whichever field still holds it.
                            cursorShape: Qt.PointingHandCursor
                            onPressed: forceActiveFocus()   // takes the caret out of any field
                        id: optHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: { if (dropList.field) dropList.field.choose(modelData.value); root.openField = null }
                    }
                }
            }
        }
    }

    Shortcut {
        // The handler below peels layers, which is what Escape means here. `StandardKey.Close`
        // alone never fires on it: on Windows that standard key is Ctrl+W / Alt+F4, not Esc.
        sequences: ["Esc", StandardKey.Close]
        onActivated: {
            if (root.openField !== null) root.openField = null
            else if (root.settingsOpen) root.settingsOpen = false
        }
    }

    // ---------------------------------------------------- window chrome (the frame is gone)
    // Three buttons where the OS title bar used to be, at the right end of the chip strip. Declared
    // after the dialog so they stay reachable while it is open.
    RowLayout {
        id: winChrome
        // 6, the same gap the category chips across the strip use between themselves, so the three
        // plates separate the way the tabs do. It widens the row leftwards, which is why the chip
        // column's right margin went 144 -> 152 to hold the search box's distance from them.
        spacing: 6
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 8
        // 14, the same inset the chip strip uses above itself: the chips and the search box are
        // 28 px tall starting at y=14, so their centre line is y=28 - these buttons are 28 px tall
        // too, and only this margin decides whether the three groups sit on one line.
        anchors.topMargin: 14

        WinBtn {
            name: "wmin"
            tip: trs("win_min")
            onClicked: Bridge.showCommand(6)          // SW_MINIMIZE
        }
        WinBtn {
            name: root.visibility === Window.Maximized ? "wrestore" : "wmax"
            tip: trs(root.visibility === Window.Maximized ? "win_restore" : "win_max")
            // 3 = SW_SHOWMAXIMIZED, 9 = SW_RESTORE: through the platform, so the zoom animation and
            // the work-area maths are Windows' own and not a two-step geometry change by Qt.
            onClicked: Bridge.showCommand(root.visibility === Window.Maximized ? 9 : 3)
        }
        WinBtn {
            name: "close"
            tip: trs("close")
            danger: true
            // Not close(): the process survives in the tray, which is the only way back once the
            // window is gone. Tray.attach() never ran if this session has no tray, and then
            // hideToTray() falls back to a real close.
            onClicked: Tray.hideToTray()
        }
    }

    // With no frame there is no border to grab, so these eight thin zones are the only way to
    // resize. Corners are declared after the edges so they win where the zones overlap.
    Edge { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
           width: 6; edges: Qt.LeftEdge; cursor: Qt.SizeHorCursor }
    Edge { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
           width: 6; edges: Qt.RightEdge; cursor: Qt.SizeHorCursor }
    Edge { anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
           height: 6; edges: Qt.TopEdge; cursor: Qt.SizeVerCursor }
    Edge { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
           height: 6; edges: Qt.BottomEdge; cursor: Qt.SizeVerCursor }
    Edge { anchors.left: parent.left; anchors.top: parent.top; width: 14; height: 14
           edges: Qt.LeftEdge | Qt.TopEdge; cursor: Qt.SizeFDiagCursor }
    Edge { anchors.right: parent.right; anchors.top: parent.top; width: 14; height: 14
           edges: Qt.RightEdge | Qt.TopEdge; cursor: Qt.SizeBDiagCursor }
    Edge { anchors.left: parent.left; anchors.bottom: parent.bottom; width: 14; height: 14
           edges: Qt.LeftEdge | Qt.BottomEdge; cursor: Qt.SizeBDiagCursor }
    Edge { anchors.right: parent.right; anchors.bottom: parent.bottom; width: 14; height: 14
           edges: Qt.RightEdge | Qt.BottomEdge; cursor: Qt.SizeFDiagCursor }

    // ------------------------------------------------------------------ components
    // Every mark that is not a letter is drawn here, on one 24-unit grid with one stroke weight.
    // The unicode glyphs these replace came out of whichever font happened to carry them: measured,
    // their ink boxes ranged from 9x8 to 17x18 px, and each one sat 4.5-6.5 px below
    // the middle of its 40 px cell, because a glyph is centred on its baseline and not on its box.
    // The card frame, redrawn to the HUD sheet's language. The brief he pasted is written for HTML/CSS
    // (clip-path, pseudo-elements, box-shadow, CSS variables); this is Qt Quick, so each part is
    // translated rather than copied: clip-path -> the Canvas path below, pseudo-elements -> the extra
    // strokes, box-shadow/drop-shadow -> the MultiEffect that blurs `seedOnly`, and
    // --primary/--secondary/--glow -> the three colour properties. Nothing is an image; the whole
    // frame is drawn at paint time from `cut` and three colours.
    //
    // Layers, outer to inner: faint silhouette line, primary rim, secondary inner line, broken side
    // lines, then the HUD marks (corner brackets, top ticks + centre pip, bottom energy diamond).
    // Only the HUD marks seed the glow - a frame that all glows is a frame with no focus.
    component TechFrame: Canvas {
        id: tech
        property real cut: 9
        // The echo line's inset. The card keeps 6; a 28 px chip has to pull it in to 5 or it lands
        // on the glyph (the ink of a 12 px label starts 8 px from the box edge).
        property real echo: 6
        // The broken side lines and the HUD marks need the room a card has: at 28 px tall the top
        // pip would sit on the first row of the label and the side ticks would run under it.
        property bool decor: true
        property color primary: th.accent
        property color secondary: th.accent
        property color glow: th.accent
        property color body: "transparent"
        property bool seedOnly: false
        onCutChanged: requestPaint()
        onEchoChanged: requestPaint()
        onDecorChanged: requestPaint()
        onPrimaryChanged: requestPaint()
        onSecondaryChanged: requestPaint()
        onGlowChanged: requestPaint()
        onBodyChanged: requestPaint()
        onSeedOnlyChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        function nothing(c) { return c.a < 0.01 }
        // The octagon: a rectangle with every corner cut at 45 degrees, `inset` in from the item's
        // own edge (a stroke is centred on its path, so the outermost ring has to come in by half of
        // it or half the line falls off the item).
        function ring(g, inset, k) {
            const x0 = inset, y0 = inset, x1 = width - inset, y1 = height - inset
            g.beginPath()
            g.moveTo(x0 + k, y0); g.lineTo(x1 - k, y0); g.lineTo(x1, y0 + k); g.lineTo(x1, y1 - k)
            g.lineTo(x1 - k, y1); g.lineTo(x0 + k, y1); g.lineTo(x0, y1 - k); g.lineTo(x0, y0 + k)
            g.closePath()
        }
        function poly(g, p) {
            g.beginPath(); g.moveTo(p[0][0], p[0][1])
            for (var i = 1; i < p.length; ++i) g.lineTo(p[i][0], p[i][1])
            g.stroke()
        }
        function diamond(g, cx, cy, rx, ry) {
            g.beginPath(); g.moveTo(cx, cy - ry); g.lineTo(cx + rx, cy); g.lineTo(cx, cy + ry)
            g.lineTo(cx - rx, cy); g.closePath()
        }
        // The four mechanical corners: arm along one edge, the chamfer itself, arm along the other.
        function brackets(g, i, k, arm) {
            const W = width, H = height
            poly(g, [[i + k + arm, i], [i + k, i], [i, i + k], [i, i + k + arm]])
            poly(g, [[W - i - k - arm, i], [W - i - k, i], [W - i, i + k], [W - i, i + k + arm]])
            poly(g, [[W - i, H - i - k - arm], [W - i, H - i - k], [W - i - k, H - i], [W - i - k - arm, H - i]])
            poly(g, [[i + k + arm, H - i], [i + k, H - i], [i, H - i - k], [i, H - i - k - arm]])
        }
        function hud(g, i, k) {
            const W = width, H = height, cx = W / 2
            // In the seed everything is the glow colour, because that is what gets blurred; on the
            // frame itself only the corners and the two gems carry it, so the light has a source.
            const mark = glow
            g.strokeStyle = mark; g.lineWidth = seedOnly ? 3.5 : 2.4
            brackets(g, i, k, 15)
            const tick = seedOnly ? glow : secondary
            g.strokeStyle = tick; g.lineWidth = 1
            poly(g, [[cx - 26, i + 3], [cx - 11, i + 3]])
            poly(g, [[cx + 11, i + 3], [cx + 26, i + 3]])
            g.fillStyle = mark
            diamond(g, cx, i + 3, 3.5, 3); g.fill()
            diamond(g, cx, H - i - 4, 5.5, 4.5); g.fill()
            g.strokeStyle = tick
            poly(g, [[cx - 30, H - i - 4], [cx - 13, H - i - 4]])
            poly(g, [[cx + 13, H - i - 4], [cx + 30, H - i - 4]])
        }

        onPaint: {
            const g = getContext("2d")
            g.reset()
            g.clearRect(0, 0, width, height)
            const k = cut
            if (!nothing(body)) { g.fillStyle = body; ring(g, 0, k); g.fill() }
            g.lineJoin = "miter"
            if (seedOnly) { hud(g, 2.5, k - 2.5); return }
            g.strokeStyle = Qt.alpha(primary, 0.22); g.lineWidth = 1; ring(g, 0.5, k - 0.5); g.stroke()
            g.strokeStyle = primary; g.lineWidth = 2; ring(g, 2.5, k - 2.5); g.stroke()
            g.strokeStyle = Qt.alpha(secondary, 0.7); g.lineWidth = 1; ring(g, echo, k - (echo - 1)); g.stroke()
            if (!decor) return
            // The broken lines: 2 on / 5 off, and stopped short of the corners so they read as
            // instrument ticks rather than a second box.
            g.strokeStyle = Qt.alpha(secondary, 0.62); g.lineWidth = 1; g.setLineDash([2, 5])
            g.beginPath(); g.moveTo(11, k + 9); g.lineTo(11, height - k - 9)
            g.moveTo(width - 11, k + 9); g.lineTo(width - 11, height - k - 9); g.stroke()
            g.setLineDash([])
            hud(g, 2.5, k - 2.5)
        }
    }

    component Icon: Canvas {
        id: ic
        property string name: ""
        property color tint: th.text
        property real px: 20
        property real stroke: 1.6
        implicitWidth: px
        implicitHeight: px
        antialiasing: true
        onNameChanged: requestPaint()
        onTintChanged: requestPaint()
        onPxChanged: requestPaint()
        onStrokeChanged: requestPaint()
        onEnabledChanged: requestPaint()

        onPaint: {
            var c = getContext("2d")
            c.reset()
            var k = ic.px / 24
            c.scale(k, k)
            c.lineWidth = ic.stroke / k
            c.lineCap = "round"
            c.lineJoin = "round"
            c.strokeStyle = ic.tint
            c.fillStyle = ic.tint
            var tau = Math.PI * 2

            function rr(x, y, w, h, r) {
                c.beginPath()
                c.moveTo(x + r, y)
                c.lineTo(x + w - r, y);      c.arc(x + w - r, y + r, r, -Math.PI / 2, 0)
                c.lineTo(x + w, y + h - r);  c.arc(x + w - r, y + h - r, r, 0, Math.PI / 2)
                c.lineTo(x + r, y + h);      c.arc(x + r, y + h - r, r, Math.PI / 2, Math.PI)
                c.lineTo(x, y + r);          c.arc(x + r, y + r, r, Math.PI, Math.PI * 1.5)
                c.closePath()
            }
            function spoke(a, r0, r1) {
                c.moveTo(12 + r0 * Math.cos(a), 12 + r0 * Math.sin(a))
                c.lineTo(12 + r1 * Math.cos(a), 12 + r1 * Math.sin(a))
            }

            switch (ic.name) {
            case "heart":
            case "heart_fill":
                c.beginPath()
                c.moveTo(12, 20.4)
                c.bezierCurveTo(4.6, 14.8, 3.4, 11.0, 3.4, 8.4)
                c.bezierCurveTo(3.4, 5.9, 5.2, 4.2, 7.3, 4.2)
                c.bezierCurveTo(9.1, 4.2, 10.7, 5.2, 12, 6.9)
                c.bezierCurveTo(13.3, 5.2, 14.9, 4.2, 16.7, 4.2)
                c.bezierCurveTo(18.8, 4.2, 20.6, 5.9, 20.6, 8.4)
                c.bezierCurveTo(20.6, 11.0, 19.4, 14.8, 12, 20.4)
                c.closePath()
                if (ic.name === "heart_fill") c.fill(); else c.stroke()
                break
            case "gear":
                c.beginPath(); c.arc(12, 12, 6.6, 0, tau); c.stroke()
                c.beginPath(); c.arc(12, 12, 2.5, 0, tau); c.stroke()
                c.beginPath()
                for (var gi = 0; gi < 8; ++gi) spoke(gi * Math.PI / 4 + Math.PI / 8, 6.9, 9.7)
                c.stroke()
                break
            case "monitor":
                rr(3.2, 4.4, 17.6, 12.0, 2.0); c.stroke()
                c.beginPath(); c.moveTo(12, 16.4); c.lineTo(12, 19.4); c.stroke()
                c.beginPath(); c.moveTo(7.6, 19.9); c.lineTo(16.4, 19.9); c.stroke()
                break
            case "bolt":
                c.beginPath(); c.moveTo(13.6, 2.8); c.lineTo(6.2, 13.4); c.lineTo(10.8, 13.4)
                c.lineTo(9.6, 21.2); c.lineTo(17.8, 10.2); c.lineTo(13.2, 10.2); c.closePath()
                c.stroke()
                break
            case "layers":
                c.beginPath(); c.moveTo(12, 3.4); c.lineTo(20.6, 8.4); c.lineTo(12, 13.4)
                c.lineTo(3.4, 8.4); c.closePath(); c.stroke()
                c.beginPath(); c.moveTo(3.4, 12.9); c.lineTo(12, 17.9); c.lineTo(20.6, 12.9); c.stroke()
                c.beginPath(); c.moveTo(3.4, 17.4); c.lineTo(12, 22.4)
                c.lineTo(20.6, 17.4); c.stroke()
                break
            case "cpu":
                rr(6.2, 6.2, 11.6, 11.6, 1.8); c.stroke()
                rr(9.8, 9.8, 4.4, 4.4, 1.0); c.stroke()
                c.beginPath()
                for (var pi = 0; pi < 3; ++pi) {
                    var p = 9.2 + pi * 2.8
                    c.moveTo(p, 6.2);  c.lineTo(p, 3.4)
                    c.moveTo(p, 17.8); c.lineTo(p, 20.6)
                    c.moveTo(6.2, p);  c.lineTo(3.4, p)
                    c.moveTo(17.8, p); c.lineTo(20.6, p)
                }
                c.stroke()
                break
            case "close":
                c.beginPath(); c.moveTo(5.4, 5.4); c.lineTo(18.6, 18.6)
                c.moveTo(18.6, 5.4); c.lineTo(5.4, 18.6); c.stroke()
                break
            // The three title-bar buttons of a frameless window. Same 13.2-wide box as the cross so
            // the three read as one family.
            case "wmin":
                c.beginPath(); c.moveTo(5.4, 12); c.lineTo(18.6, 12); c.stroke()
                break
            case "wmax":
                c.strokeRect(5.4, 5.4, 13.2, 13.2)
                break
            case "wrestore":
                c.beginPath(); c.moveTo(5.6, 9.2); c.lineTo(5.6, 5.4); c.lineTo(18.6, 5.4)
                c.lineTo(18.6, 15.4); c.stroke()
                c.strokeRect(8.6, 8.6, 10.8, 10.8)
                break
            case "search":
                c.beginPath(); c.arc(10.6, 10.6, 6.4, 0, tau); c.stroke()
                c.beginPath(); c.moveTo(15.3, 15.3); c.lineTo(20.2, 20.2); c.stroke()
                break
            case "caret":
                c.beginPath(); c.moveTo(5.6, 9.2); c.lineTo(12, 15.2); c.lineTo(18.4, 9.2); c.stroke()
                break
            case "chevL":
                // Square caps and a mitered corner: these two sit on the big picture as the
                // carousel's arrows, where the sheet he pasted shows a hard gold angle, not a soft one.
                c.lineCap = "square"; c.lineJoin = "miter"
                c.beginPath(); c.moveTo(13.6, 3.6); c.lineTo(8.0, 12); c.lineTo(13.6, 20.4); c.stroke()
                break
            case "chevR":
                c.lineCap = "square"; c.lineJoin = "miter"
                c.beginPath(); c.moveTo(10.4, 3.6); c.lineTo(16.0, 12); c.lineTo(10.4, 20.4); c.stroke()
                break
            case "pause":
                rr(7.2, 4.8, 2.9, 14.4, 1.4); c.fill()
                rr(13.9, 4.8, 2.9, 14.4, 1.4); c.fill()
                break
            case "play":
                c.beginPath(); c.moveTo(8.2, 4.6); c.lineTo(19.4, 12.0); c.lineTo(8.2, 19.4)
                c.closePath(); c.fill()
                break
            case "motion":
                // A particle with two trailing arcs: this is the wallpaper's own motion, not a video.
                c.beginPath(); c.arc(14.6, 12.0, 3.2, 0, tau); c.stroke()
                c.beginPath(); c.arc(14.6, 12.0, 7.2, 2.50, 3.85); c.stroke()
                c.beginPath(); c.arc(14.6, 12.0, 10.4, 2.62, 3.72); c.stroke()
                break
            }
        }
    }

    // Icon-only buttons are square. Btn's minimum width is 56 px because it is sized for a word,
    // and a 56 px hit box around a 12 px cross reads as "the whole row closed the dialog".
    component IconBtn: Rectangle {
        id: ib
        property string icon: ""
        property real glyph: 14
        property real stroke: 1.6
        property real box: 28
        property bool flat: false    // no plate behind the glyph, for the ones that sit on a picture
        property color tint: th.text
        property string tip: ""
        property bool checked: false
        // What a flat button shows under the pointer. The default is a dark veil, right for a button
        // sitting on a bright picture and invisible on the dark caption bar, which passes a light one.
        property color veil: "#40000000"
        readonly property alias hoverArea: hoverI
        signal clicked

        implicitWidth: box
        implicitHeight: box
        Layout.preferredWidth: box
        Layout.preferredHeight: box
        radius: 6
        opacity: enabled ? 1.0 : 0.45
        color: !flat ? (hoverI.containsMouse ? th.hover : checked ? th.surfaceAlt : th.surface)
                     : (hoverI.containsMouse ? veil : "transparent")
        border.color: checked ? th.accent : "transparent"
        border.width: 1

        Icon {
            name: ib.icon
            tint: ib.tint
            px: ib.glyph
            stroke: ib.stroke
            anchors.centerIn: parent
            enabled: ib.enabled
        }
        MouseArea {
                // A clickable place has to say so under the pointer, and taking the press also
                // drops the caret out of whichever field still holds it.
                cursorShape: Qt.PointingHandCursor
                onPressed: forceActiveFocus()   // takes the caret out of any field
            id: hoverI
            anchors.fill: parent
            hoverEnabled: true
            enabled: ib.enabled
            onClicked: ib.clicked()
        }

        Accessible.role: Accessible.Button
        Accessible.name: ib.tip
        Accessible.onPressAction: ib.clicked()
    }

    // Icon-only controls have no label to show, so the name shows in a bubble we draw: the style's
    // own ToolTip is a light box that does not belong anywhere near this palette.
    // A bubble that is a child of its button is painted under anything declared later in the same
    // container: the caption bar and the card strip both live inside the picture's clip box, the
    // strip comes last, and the bubble vanished behind a card. A Popup belongs to the window's
    // overlay instead - nothing is above it and no ancestor's clip reaches it. The coordinates are
    // mapped to the window because what we know is the button's own hover area.
    component Hint: Popup {
        id: hint
        property string label: ""
        property Item hovered: null
        property string place: "right"  // "right" | "left" | "top" | "bottom" - left/top for
                                        // controls near the right edge, where a bubble would be
                                        // clipped by the picture; bottom for the title-bar buttons
        padding: 0
        width: hintTxt.implicitWidth + 18
        height: 26
        visible: hint.hovered !== null && hint.hovered.containsMouse
        // Mapped into the Popup's own parent, not the window: a Popup's x/y are relative to whatever
        // it ends up parented to, and that is not guaranteed to be the window's content item.
        readonly property point at: hovered && parent ? hovered.mapToItem(parent, 0, 0) : Qt.point(0, 0)
        readonly property real srcW: hovered ? hovered.width : 0
        readonly property real srcH: hovered ? hovered.height : 0
        x: place === "right" ? at.x + srcW + 10
           : place === "left" ? at.x - width - 10
           : at.x + (srcW - width) / 2
        y: place === "top" ? at.y - height - 8
           : place === "bottom" ? at.y + srcH + 8
           : at.y + (srcH - height) / 2
        background: Rectangle {
            radius: 6
            color: th.surfaceAlt
            border.color: th.hover
        }
        Text {
            id: hintTxt
            anchors.centerIn: parent
            text: hint.label
            color: th.text
            font.family: root.fontFamily
            font.pixelSize: 12
        }
    }

    // The three buttons of a title bar we now draw ourselves. They now carry the same plate as the
    // search box beside them; the hover fills are what they always were.
    component WinBtn: Rectangle {
        id: wb
        property string name: ""
        property string tip: ""
        property bool danger: false
        signal clicked
        readonly property alias hoverArea: wbHover

        Layout.preferredWidth: 40
        Layout.preferredHeight: 28
        radius: 4
        color: wbHover.containsMouse ? (wb.danger ? Qt.alpha(th.err, 0.85) : "#26FFFFFF")
                                     : Qt.alpha(th.surface, root.plateAlpha)

        Icon {
            anchors.centerIn: parent
            name: wb.name
            px: 14
            tint: wb.danger && wbHover.containsMouse ? "white" : th.text
        }
        MouseArea {
            id: wbHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: wb.clicked()
        }
        Accessible.role: Accessible.Button
        Accessible.name: wb.tip
        Accessible.onPressAction: wb.clicked()
        Hint { label: wb.tip; hovered: wb.hoverArea; place: "bottom" }
    }

    // One resize zone of a frameless window: it hands the press to the platform, which is the only
    // way to get the system's own snap-to-edge behaviour back without a frame.
    component Edge: MouseArea {
        property int edges: 0
        property int cursor: Qt.ArrowCursor
        cursorShape: cursor
        acceptedButtons: Qt.LeftButton
        onPressed: root.startSystemResize(edges)
    }

    // A rounded block of rows, like the settings sheet he pasted: the block is one shade up from the
    // dialog, and every row carries a divider under it except the last one. It has no title - the
    // section names came out, so what a block holds has to read from its own rows.
    component Group: Rectangle {
        id: grp
        default property alias rows: body.data
        readonly property int pad: 12
        // Half way between the module hairline and the block's own fill: the row dividers are meant
        // to sit back from anything that separates modules.
        readonly property color divider: Qt.rgba((th.line.r + grp.color.r) / 2,
                                                 (th.line.g + grp.color.g) / 2,
                                                 (th.line.b + grp.color.b) / 2, 1.0)

        Layout.fillWidth: true
        implicitHeight: body.y + body.height + pad
        radius: 8
        color: th.surface

        // First child, so every row sits on top of it: a press that lands on the block's own
        // background is "somewhere else" and has to end whatever edit is still running. The dialog's
        // content is inside a Flickable, and that swallows the press before the card's parent can
        // see it - which is why the number field kept its caret when you clicked beside it.
        MouseArea {
            anchors.fill: parent
            onPressed: root.contentItem.forceActiveFocus()
        }

        // The dividers belong to the block, not to the rows: a row is a RowLayout and a layout child
        // cannot be anchored, so a line inside a row would be laid out as a cell of it. Drawing one
        // between consecutive rows is the same picture as "a bottom border on every row but the
        // last", and it needs no child counting.
        // Declared BEFORE the rows on purpose: an item stacked over a card both eats the clicks that
        // belong to the controls under it and paints its lines across an open dropdown list.
        Canvas {
            id: sep
            anchors.fill: parent
            // childrenRect, not body.height: a row can change height without the total changing, and
            // the first paint lands before the rows have their final geometry - the status card drew
            // one line in the middle of its first row and nothing else until something else resized.
            property rect rows: body.childrenRect
            onRowsChanged: requestPaint()
            onWidthChanged: requestPaint()
            Component.onCompleted: Qt.callLater(requestPaint)
            onPaint: {
                var c = getContext("2d")
                c.reset()
                c.fillStyle = grp.divider
                var kids = body.children
                var prev = -1
                for (var i = 0; i < kids.length; ++i) {
                    if (kids[i].visible !== true || !(kids[i].height > 0)) continue
                    if (prev >= 0) {
                        var mid = (kids[prev].y + kids[prev].height + kids[i].y) / 2
                        var y = Math.round(body.y + mid)
                        for (var x = 0; x < grp.width; x += 7)
                            c.fillRect(x, y, 4, 1)     // 4 on, 3 off
                    }
                    prev = i
                }
            }
        }

        ColumnLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: grp.pad
            anchors.rightMargin: grp.pad
            anchors.topMargin: grp.pad
            spacing: 12
        }
    }

    // An option that has exactly two states is a slider, not a button whose label flips.
    component Slide: Rectangle {
        id: slide
        property bool checked: false
        // `checked` is bound to the renderer's state, so the switch never owns it: the handler has
        // to be told the value to send. Reading `checked` here would report the old one and the
        // knob would never move.
        signal toggled(bool on)
        Layout.preferredWidth: 40
        Layout.preferredHeight: 22
        radius: 11
        opacity: enabled ? 1.0 : 0.45
        color: checked ? th.accent : th.surfaceAlt
        Behavior on color { ColorAnimation { duration: 110 } }
        Rectangle {
            width: 16; height: 16; radius: 8
            y: 3
            x: slide.checked ? 21 : 3
            color: "white"
            Behavior on x { NumberAnimation { duration: 110 } }
        }
        MouseArea {
                // A clickable place has to say so under the pointer, and taking the press also
                // drops the caret out of whichever field still holds it.
                cursorShape: Qt.PointingHandCursor
                onPressed: forceActiveFocus()   // takes the caret out of any field
            anchors.fill: parent
            enabled: slide.enabled
            onClicked: slide.toggled(!slide.checked)
        }
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: slide.checked
        Accessible.onPressAction: slide.toggled(!slide.checked)
        Accessible.onToggleAction: slide.toggled(!slide.checked)
    }

    // A value on a scale, not a button that cycles through its options. The whole track is the hit
    // area and pressing anywhere on it moves the number there: that is fewer parts than a knob you
    // can only drag, and it is the difference between a control a scripted click can prove works and
    // one it cannot. The value leaves on release, so dragging does not put a request per pixel on
    // the pipe.
    component Range: Rectangle {
        id: rng
        property int from: 0
        property int to: 100
        property int value: 0            // what the renderer holds
        property int held: -1            // what the pointer is on, only read while it is down
        readonly property int shown: trackHover.pressed ? (held >= 0 ? held : value) : value
        readonly property int pad: 10
        readonly property int labelW: 34
        readonly property int knob: 14
        readonly property real trackW: Math.max(0, width - pad * 2 - labelW - knob)
        readonly property real frac: to > from ? (shown - from) / (to - from) : 0
        signal committed(int v)

        function at(px) {
            if (trackW <= 0) return from
            const f = Math.min(1, Math.max(0, (px - pad - labelW) / trackW))
            return Math.round(from + f * (to - from))
        }

        Layout.preferredWidth: 190
        Layout.preferredHeight: 30
        radius: 6
        opacity: enabled ? 1.0 : 0.45
        color: th.surface
        border.color: th.line
        border.width: 1

        Text {
            x: rng.pad
            anchors.verticalCenter: parent.verticalCenter
            width: rng.labelW
            text: rng.shown === 0 ? trs("off") : rng.shown + "%"
            color: th.text; font.family: root.fontFamily; font.pixelSize: 11
        }
        Rectangle {
            x: rng.pad + rng.labelW
            width: rng.trackW
            height: 4
            radius: 2
            y: (rng.height - height) / 2
            color: trackHover.containsMouse ? th.line : th.surfaceAlt
        }
        Rectangle {
            x: rng.pad + rng.labelW
            width: rng.trackW * rng.frac
            height: 4
            radius: 2
            y: (rng.height - height) / 2
            color: th.accent
        }
        Rectangle {
            x: rng.pad + rng.labelW + rng.trackW * rng.frac
            width: rng.knob
            height: rng.knob
            radius: rng.knob / 2
            y: (rng.height - height) / 2
            color: "white"
            border.color: th.accent
            border.width: 1
        }
        MouseArea {
            id: trackHover
            anchors.fill: parent
            enabled: rng.enabled
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onPressed: function (mouse) {
                forceActiveFocus()       // takes the caret out of any field
                rng.held = rng.at(mouse.x)
            }
            onPositionChanged: function (mouse) { if (pressed) rng.held = rng.at(mouse.x) }
            onReleased: {
                if (rng.held >= 0) rng.committed(rng.held)
                rng.held = -1
            }
        }
        Accessible.role: Accessible.Slider
        Accessible.name: trs("tray_alpha")
    }

    // Anything with more than two options is one field with a list under it, not a row of chips.
    // The field is only the box: the option list is a single shared item at the window level
    // (see dropList below). A list that lives inside a group card only receives the mouse where it
    // overlaps that card, so everything hanging below the card's own rect was unhittable - which
    // looked exactly like "only the first option can be picked".
    component Choice: Rectangle {
        id: choice
        property var items: []
        property variant value: null
        signal picked(var v)
        function choose(v) { choice.picked(v) }
        property string currentLabel: {
            for (var i = 0; i < items.length; ++i)
                if (items[i].value === value) return items[i].label
            return items.length > 0 ? String(items[0].label) : ""
        }
        Layout.preferredWidth: 190
        Layout.preferredHeight: 30
        radius: 6
        opacity: enabled ? 1.0 : 0.45
        color: root.openField === choice ? th.surfaceAlt : th.surface
        // The field sits on a group card of its own colour, so the outline is what separates them -
        // same as the bordered dropdown in the sheet he pasted.
        border.color: th.line
        border.width: 1
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 8
            spacing: 6
            Text {
                text: choice.currentLabel
                color: th.text; font.family: root.fontFamily; font.pixelSize: 12
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Icon { name: "caret"; px: 12; tint: th.muted }
        }
        MouseArea {
                // A clickable place has to say so under the pointer, and taking the press also
                // drops the caret out of whichever field still holds it.
                cursorShape: Qt.PointingHandCursor
                onPressed: forceActiveFocus()   // takes the caret out of any field
            anchors.fill: parent
            enabled: choice.enabled
            onClicked: root.openField = root.openField === choice ? null : choice
        }
    }

    component SectionTitle: Text {
        property int topPad: 0
        Layout.fillWidth: true
        topPadding: topPad
        color: th.muted
        font.family: root.fontFamily
        font.pixelSize: 11
        font.letterSpacing: 0.6
    }

    component Btn: Rectangle {
        id: btn
        property string text: ""
        property bool accent: false
        property bool checked: false
        property bool compact: false
        // A button that sits on a group card has to be one shade above it. Outlining it instead put
        // a second hairline 7 px above the row's own divider, so the fill carries the step.
        property bool raised: false
        // The card's octagonal HUD rim instead of a 1 px rounded border. Only the chips ask for it.
        property bool frame: false
        signal clicked

        implicitWidth: Math.max(compact ? 56 : 92, lbl.implicitWidth + (compact ? 18 : 26))
        implicitHeight: compact ? 28 : 34
        Layout.preferredWidth: implicitWidth
        Layout.preferredHeight: implicitHeight
        radius: 6
        opacity: enabled ? 1.0 : 0.45
        // How see-through the plate is. 1.0 leaves every existing button exactly as it was; the top
        // tabs and the search field lower it so the wallpaper reads through them.
        property real alpha: 1.0
        readonly property color plate: Qt.alpha(
            !enabled ? (raised ? th.surfaceAlt : th.surface)
             : hoverC.containsMouse ? (accent ? th.accentDim : th.hover)
             : checked ? (raised ? th.hover : th.surfaceAlt)
             : accent ? th.accent : (raised ? th.surfaceAlt : th.surface), btn.alpha)
        // A rounded plate behind an octagonal rim pokes 1.75 px past each chamfer, so the framed
        // button hands its background to the frame, which fills the same octagon it outlines.
        color: frame ? "transparent" : plate
        border.color: (!frame && checked) ? th.accent : "transparent"
        border.width: 1

        TechFrame {
            anchors.fill: parent
            visible: btn.frame
            cut: 6
            echo: 5
            decor: false
            body: btn.plate
            primary: btn.checked ? th.accent : "transparent"
            secondary: btn.checked ? th.accentDim : "transparent"
            glow: th.accent
        }

        Text {
            id: lbl
            anchors.centerIn: parent
            text: btn.text
            color: btn.accent && btn.enabled ? "white" : th.text
            font.family: root.fontFamily
            font.pixelSize: btn.compact ? 12 : 13
        }
        MouseArea {
                // A clickable place has to say so under the pointer, and taking the press also
                // drops the caret out of whichever field still holds it.
                cursorShape: Qt.PointingHandCursor
                onPressed: forceActiveFocus()   // takes the caret out of any field
            id: hoverC
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.enabled
            onClicked: btn.clicked()
        }

        Accessible.role: Accessible.Button
        Accessible.name: btn.text
        Accessible.checkable: true
        Accessible.checked: btn.checked
        Accessible.onPressAction: btn.clicked()
        Accessible.onToggleAction: btn.clicked()
    }

    component StatLine: RowLayout {
        property string k: ""
        property string v: ""
        Layout.fillWidth: true
        spacing: 8
        Text { text: k; color: th.muted; font.family: root.fontFamily; font.pixelSize: 12 }
        Item { Layout.fillWidth: true }
        Text {
            text: v
            color: th.text
            font.family: root.fontFamily
            font.pixelSize: 12
            horizontalAlignment: Text.AlignRight
            wrapMode: Text.WordWrap
            Layout.maximumWidth: 210
        }
    }

    component NumField: Rectangle {
        id: nf
        property int value: 0
        signal committed(int v)
        Layout.preferredWidth: 74
        implicitWidth: 74
        implicitHeight: 28
        Layout.preferredHeight: 28
        radius: 6
        opacity: enabled ? 1 : 0.45
        color: edit.activeFocus ? th.surfaceAlt : th.surface
        border.color: edit.activeFocus ? th.accent : th.line
        border.width: 1

        function commit() {
            const n = parseInt(edit.text, 10);
            if (isNaN(n) || n < 0 || n > 1000) { edit.text = String(nf.value); return; }
            if (n !== nf.value) nf.committed(n);
        }

        TextField {
            id: edit
            anchors.fill: parent
            text: String(nf.value)
            enabled: nf.enabled
            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
            color: th.text
            background: null
            font.family: root.fontFamily
            font.pixelSize: 12
            selectByMouse: true
            validator: IntValidator { bottom: 0; top: 1000 }
            onEditingFinished: nf.commit()
            onAccepted: nf.commit()
            Accessible.role: Accessible.EditableText
            Accessible.name: String(nf.value)
        }
    }
}
