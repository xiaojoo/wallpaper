#pragma once
// Engine/Graphics/FFmpegPlayer.hpp - the video backend's entry point.
//
// A factory and a probe rather than an includable class: this is the only translation unit that sees
// libav* headers, so nothing outside it can accidentally take on an FFmpeg type (the engine proper
// talks to Engine/Graphics/VideoSource.hpp). CMake makes the LGPL shared build a requirement
// (build/ffm-lgpl, see tools/get-ffmpeg.sh) - a tree without it fails to configure rather than
// quietly losing video.
#include "Engine/Graphics/VideoSource.hpp"

#include <memory>
#include <string>

namespace sw {

// Null only when the decoder cannot be created at all; a file this backend cannot open is an error
// from Open(), not a null player. preferNvdec asks the hardware decoder first (h264_cuvid and its
// twins) and falls back to libavcodec's native decoder when the driver refuses.
std::unique_ptr<VideoSource> MakeFFmpegPlayer(bool preferNvdec);

// Which FFmpeg is actually loaded, read out of the library at run time: release, libavcodec version
// and the license string the build itself reports. LGPL compliance is a claim about a specific build,
// so it has to come from the artifact rather than from whatever the repo thought when it fetched it.
std::string FfmpegBuildNote();

// What the import step needs to know about a candidate file before it becomes a package. The numbers
// go into `wallpaper.json` so the browser can label and lay out a card without opening a decoder for
// every row - and the probe is the *gate*: whatever it cannot name a decoder for is refused at import
// rather than failing later when a wallpaper slot tries to open it.
struct VideoProbe {
    unsigned w = 0, h = 0;
    double fps = 0.0, duration_s = 0.0;
    bool bt601 = false;      // only a declared 601-family matrix; unspecified stays BT.709
    std::string codec;       // the decoder libavcodec picked, for the log
};

bool FfmpegProbeVideo(const std::wstring& file, VideoProbe& out, std::string& error);

} // namespace sw
