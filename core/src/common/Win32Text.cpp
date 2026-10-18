#include "common/Win32Text.hpp"

#include <climits>
#include <cstddef>
#include <utility>
#include <windows.h>

namespace openscp::win32 {

namespace {

int checkedLength(std::size_t length) {
    return length > static_cast<std::size_t>(INT_MAX)
               ? -1
               : static_cast<int>(length);
}

bool startsWith(std::wstring_view text, std::wstring_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// Paths in the \\?\ form, or naming a device (\\.\), go to the file system
// as they are written.
bool isVerbatim(std::wstring_view path) {
    return startsWith(path, L"\\\\?\\") || startsWith(path, L"\\\\.\\");
}

} // namespace

std::wstring toWide(std::string_view utf8) {
    const int length = checkedLength(utf8.size());
    if (length <= 0)
        return std::wstring();
    const int required =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
    if (required <= 0)
        return std::wstring();
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, wide.data(),
                          required);
    return wide;
}

bool toWidePath(std::string_view utf8, std::wstring &wide) {
    wide.clear();
    const int length = checkedLength(utf8.size());
    if (length < 0)
        return false;
    if (length == 0)
        return true;
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               utf8.data(), length, nullptr, 0);
    if (required <= 0)
        return false;
    wide.resize(static_cast<std::size_t>(required));
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                 length, wide.data(), required) == required;
}

bool toExtendedLengthPath(std::string_view utf8, std::wstring &wide) {
    if (!toWidePath(utf8, wide))
        return false;
    // Resolving a verbatim path could change which file it names.
    if (wide.empty() || isVerbatim(wide))
        return true;
    // When the path cannot be resolved, it is left for the file function to
    // refuse.
    const DWORD required =
        ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (required == 0)
        return true;
    std::wstring full(required, L'\0');
    const DWORD length =
        ::GetFullPathNameW(wide.c_str(), required, full.data(), nullptr);
    if (length == 0 || length >= required)
        return true;
    full.resize(length);
    if (isVerbatim(full)) // A device name such as NUL becomes \\.\NUL.
        wide = std::move(full);
    else if (startsWith(full, L"\\\\"))
        wide = L"\\\\?\\UNC\\" + full.substr(2);
    else
        wide = L"\\\\?\\" + full;
    return true;
}

std::string toUtf8(std::wstring_view wide) {
    const int length = checkedLength(wide.size());
    if (length <= 0)
        return std::string();
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length,
                                               nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return std::string();
    std::string utf8(static_cast<std::size_t>(required), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(),
                          required, nullptr, nullptr);
    return utf8;
}

std::string errorMessage(unsigned long code) {
    wchar_t *buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::wstring text;
    if (length > 0 && buffer)
        text.assign(buffer, length);
    if (buffer)
        ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' '))
        text.pop_back();
    if (text.empty())
        return "error " + std::to_string(code);
    return toUtf8(text);
}

} // namespace openscp::win32
