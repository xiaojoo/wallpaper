#pragma once
// Engine/App/IPCClient.hpp - one-shot client for the renderer's control pipe. Shared by the
// renderer's own --ctl mode and by the Qt settings window.
#include "Engine/Core/Platform.hpp"

namespace sw {

struct IpcReply {
    bool connected = false;
    std::string response;
    std::string error;
};

IpcReply IPCCall(const std::wstring& pipeName, const std::string& request, DWORD timeoutMs);
std::string TrimCrLf(std::string s);

} // namespace sw
