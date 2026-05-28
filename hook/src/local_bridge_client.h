#pragma once

#include <string>

namespace fh6rb
{
class LocalBridgeClient
{
public:
    LocalBridgeClient(std::wstring host = L"127.0.0.1", unsigned short port = 8420);

    bool PostEvent(const std::string& eventType) const;

private:
    std::wstring host_;
    unsigned short port_;
};
}
