#include "local_bridge_client.h"

#include "logger.h"

#include <Windows.h>
#include <winhttp.h>

#include <sstream>
#include <utility>

namespace
{
struct WinHttpHandle
{
    HINTERNET handle = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET value) : handle(value) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~WinHttpHandle() { Reset(); }

    void Reset()
    {
        if (handle)
        {
            WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }

    explicit operator bool() const { return handle != nullptr; }
};

std::string MakeEventBody(const std::string& eventType)
{
    std::ostringstream stream;
    stream << "{\"type\":\"";
    for (const char c : eventType)
    {
        if (c == '\\' || c == '"')
        {
            stream << '\\';
        }
        stream << c;
    }
    stream << "\"}";
    return stream.str();
}
}

namespace fh6rb
{
LocalBridgeClient::LocalBridgeClient(std::wstring host, unsigned short port) :
    host_(std::move(host)),
    port_(port)
{
}

bool LocalBridgeClient::PostEvent(const std::string& eventType) const
{
    const auto body = MakeEventBody(eventType);
    WinHttpHandle session(WinHttpOpen(
        L"FH6RadioBridgeHook/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session)
    {
        return false;
    }

    WinHttpSetTimeouts(session.handle, 200, 500, 500, 500);

    WinHttpHandle connect(WinHttpConnect(session.handle, host_.c_str(), port_, 0));
    if (!connect)
    {
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connect.handle,
        L"POST",
        L"/api/hook/event",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0));
    if (!request)
    {
        return false;
    }

    constexpr wchar_t headers[] = L"Content-Type: application/json\r\n";
    const BOOL sent = WinHttpSendRequest(
        request.handle,
        headers,
        static_cast<DWORD>(-1L),
        const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);
    if (!sent)
    {
        return false;
    }

    if (!WinHttpReceiveResponse(request.handle, nullptr))
    {
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(
            request.handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) &&
        status >= 200 && status < 300)
    {
        return true;
    }

    return false;
}
}
