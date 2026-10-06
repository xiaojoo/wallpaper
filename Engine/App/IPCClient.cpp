#include "Engine/App/IPCClient.hpp"

namespace sw {

std::string TrimCrLf(std::string s) {
    const size_t b = s.find_first_not_of(" \t");
    const size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos || e == std::string::npos || e < b) return {};
    return s.substr(b, e - b + 1);
}

// ------------------------------------------------------------------ client

IpcReply IPCCall(const std::wstring& pipeName, const std::string& request, DWORD timeoutMs) {
    IpcReply r;
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;) {
        h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            r.error = "open pipe: " + HResultToString(HRESULT_FROM_WIN32(err));
            return r;
        }
        if (GetTickCount64() >= deadline) {
            r.error = err == ERROR_PIPE_BUSY ? "pipe busy" : "no renderer is running on this pipe";
            return r;
        }
        if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(pipeName.c_str(), 200);
        else Sleep(100);
    }
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    DWORD wrote = 0;
    std::string line = request + "\n";
    if (!WriteFile(h, line.data(), DWORD(line.size()), &wrote, nullptr)) {
        r.error = "write: " + HResultToString(HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(h);
        return r;
    }
    std::string buf;
    char chunk[4096];
    DWORD got = 0;
    // The server answers one command per connection and then disconnects, so the end of a reply is
    // EOF. A newline cannot terminate it: `status` is pretty-printed and full of newlines, and
    // stopping at the first one silently dropped everything past the first chunk.
    for (;;) {
        if (!ReadFile(h, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
        buf.append(chunk, got);
        if (buf.size() > 4000000) break;
    }
    CloseHandle(h);
    r.connected = true;
    r.response = TrimCrLf(buf);
    if (r.response.empty()) r.error = "empty reply";
    return r;
}

} // namespace sw
