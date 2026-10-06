#pragma once
// Engine/App/IPCServer.hpp - named pipe control channel (JSON lines), shared with the CLI for now
// and with the Qt settings UI later.
#include "Engine/App/IPCClient.hpp"
#include "Engine/Core/Platform.hpp"
#include <atomic>

namespace sw {

class Application;

class IPCServer {
public:
    ~IPCServer();
    bool Start(const std::wstring& pipeName, Application* app, std::string& error);
    void Stop();
    const std::wstring& pipeName() const { return pipe_; }

private:
    static DWORD WINAPI ThreadEntry(LPVOID param);
    void Loop();

    HANDLE thread_ = nullptr;
    std::wstring pipe_;
    Application* app_ = nullptr;
    std::atomic<bool> stop_{false};
};

} // namespace sw
