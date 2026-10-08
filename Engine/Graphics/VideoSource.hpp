#pragma once
// Engine/Graphics/VideoSource.hpp - what the wallpaper engine needs from "a video playing".
//
// One implementation today, FFmpegPlayer (libavformat + libavcodec, hardware decoder asked first);
// the Media Foundation decoder that used to sit beside it was deleted on 2026-10-07. The seam is kept
// because it is doing a job independent of how many classes implement it: it is the boundary past
// which no libav* type travels, so WallpaperInstance, the thumbnailer and the control surface compile
// against this header and nothing knows what decodes the picture. Pixels from whichever backend exists
// go through the same Nv12Uploader, because the plane geometry - coded height larger than display
// height, chroma halved on both axes - is where the measured bugs live.
//
// The surface deliberately mirrors what the deleted backend had to do, so a future one cannot silently
// drop a capability: pause must stop decoding, ShowFrameAt must park on a named moment for the
// thumbnails, and delivered() must keep counting or the "is it actually moving" instrument in
// --ctl status goes blind.
#include "Engine/Core/Platform.hpp"

#include <functional>
#include <string>

namespace sw {

class VideoSource {
public:
    virtual ~VideoSource() = default;

    // Opening is two halves on purpose. `OpenFile` touches nothing but libav* and the disk, so it can
    // run on a worker thread while the desktop keeps drawing whatever wallpaper is already up; `Attach`
    // builds the plane textures on our device and starts the decode thread, which belongs on the thread
    // that owns the immediate context. Doing all of it inline held the real composited picture for
    // 100-180 ms on every switch to a video (measured 2026-10-08: the container probe alone costs
    // 16-93 ms depending on the clip), which is the hitch a user calls 卡顿一下 when a clip starts over.
    // A source that is not attached publishes no frame and decodes nothing.
    virtual bool OpenFile(const std::wstring& file, bool loop, std::string& error) = 0;
    virtual bool Attach(ID3D11Device* dev, std::string& error) = 0;

    // The current picture as two planes. FrameY uploads the newest decoded frame and returns the luma
    // view, so it must be called first; null before the first frame.
    virtual ID3D11ShaderResourceView* FrameY(ID3D11DeviceContext* ctx) = 0;
    virtual ID3D11ShaderResourceView* FrameUV() = 0;

    // False means "nobody is looking at it": the source must then stop decoding, not just stop
    // publishing, or a paused wallpaper keeps burning a decoder and its frame buffers.
    virtual void SetActive(bool active) = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual bool paused() const = 0;
    virtual void Seek(double seconds) = 0;
    // Park on the frame at this time and return it: the thumbnails need named moments. `idle` is called
    // once per poll while this waits, which for a clip whose previous keyframe is far back means several
    // hundred milliseconds - and the only caller is the card builder, running on the thread that presents
    // the desktop. Without the hook that wait is a freeze; with it the desktop keeps drawing.
    virtual bool ShowFrameAt(double seconds, ID3D11DeviceContext* ctx,
                             const std::function<void()>& idle = {}) = 0;

    virtual double position_s() const = 0;
    virtual double duration_s() const = 0;
    virtual double fps() const = 0;
    virtual unsigned width() const = 0;
    virtual unsigned height() const = 0;
    virtual unsigned long long delivered() const = 0;
    virtual bool ended() const = 0;
    // Which decoder this machine actually gave us, for `status` and for the log line.
    virtual const std::string& note() const = 0;
};

} // namespace sw
