#include "Engine/App/IPCServer.hpp"
#include "Engine/App/Application.hpp"
#include "Engine/App/IPCClient.hpp"
#include "Engine/Core/Log.hpp"

namespace sw {
static constexpr const char* MOD = "ipc";

namespace {

bool WriteAll(HANDLE h, std::string_view data) {
    size_t off = 0;
    while (off < data.size()) {
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + off, DWORD(data.size() - off), &wrote, nullptr) || wrote == 0) return false;
        off += wrote;
    }
    return true;
}

} // namespace

bool IPCServer::Start(const std::wstring& pipeName, Application* app, std::string& error) {
    pipe_ = pipeName;
    app_ = app;
    stop_ = false;
    thread_ = CreateThread(nullptr, 0, &IPCServer::ThreadEntry, this, 0, nullptr);
    if (!thread_) {
        error = "CreateThread: " + HResultToString(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    Info(MOD, "control pipe listening on {}", ToUtf8(pipe_));
    return true;
}

DWORD WINAPI IPCServer::ThreadEntry(LPVOID p) {
    reinterpret_cast<IPCServer*>(p)->Loop();
    return 0;
}

void IPCServer::Loop() {
    for (;;) {
        if (stop_) break;
        HANDLE h = CreateNamedPipeW(pipe_.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 8192, 65536, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Error(MOD, "CreateNamedPipe: {}", HResultToString(HRESULT_FROM_WIN32(GetLastError())));
            return;
        }
        BOOL connected = ConnectNamedPipe(h, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            DWORD err = GetLastError();
            CloseHandle(h);
            if (stop_ || err == ERROR_OPERATION_ABORTED) return;
            Warn(MOD, "ConnectNamedPipe failed (err {})", err);
            Sleep(200);
            continue;
        }

        std::string buf;
        char chunk[4096];
        DWORD got = 0;
        while (!stop_) {
            if (!ReadFile(h, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
            buf.append(chunk, got);
            if (buf.find('\n') != std::string::npos) break;
            if (buf.size() > 200000) break; // a client that never sends a newline must not grow us
        }

        std::string line = TrimCrLf(buf.substr(0, buf.find('\n')));
        if (!line.empty() && app_) {
            std::string resp = app_->Command(line);
            if (!WriteAll(h, resp + "\n"))
                Warn(MOD, "reply truncated after {} bytes", resp.size());
            FlushFileBuffers(h);
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
}

void IPCServer::Stop() {
    if (!thread_) return;
    stop_ = true;
    CancelSynchronousIo(thread_);
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
    Info(MOD, "control pipe stopped");
}

IPCServer::~IPCServer() { Stop(); }

} // namespace sw
