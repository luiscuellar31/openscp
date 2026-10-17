// UTF-8 and UTF-16 conversion for the Windows API, which reads narrow strings
// in the system code page rather than as UTF-8. Windows only.
#pragma once

#ifdef _WIN32

#include <string>
#include <string_view>

namespace openscp::win32 {

// For text such as messages: an invalid sequence becomes U+FFFD.
std::wstring toWide(std::string_view utf8);
std::string toUtf8(std::wstring_view wide);

// For paths, which must name exactly the file meant: fails on invalid UTF-8
// instead of replacing it, since two different invalid names could otherwise
// reach the same file.
bool toWidePath(std::string_view utf8, std::wstring &wide);

// The system's description of a Win32 or Winsock error code, in UTF-8 and
// without the trailing line break, or the bare number when there is none.
std::string errorMessage(unsigned long code);

} // namespace openscp::win32

#endif
