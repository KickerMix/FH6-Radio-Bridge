#pragma once

#include <string>

namespace fh6rb
{
void SetLoggerBaseDirectory(const std::wstring& baseDirectory);
void InitializeLogger();
void LogInfo(const std::string& message);
void LogWarn(const std::string& message);
void LogError(const std::string& message);
}
