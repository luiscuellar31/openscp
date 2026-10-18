// Stands in for a program that OpenSCP starts on Windows, such as ssh.exe,
// sftp.exe or an update setup, in tests that check how it is started. Each
// run appends its name and the arguments that the C runtime split from its
// command line to the file in OPENSCP_ARGV_PROBE_LOG, and exits with the code
// in OPENSCP_ARGV_PROBE_EXIT_<NAME>, where NAME is its file name in capitals
// without the extension, such as SSH. With OPENSCP_ARGV_PROBE_ECHO set, it
// first copies its input to its output until the input ends, as ssh -W
// relays a connection, and then writes that variable's value to stderr.

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <windows.h>

namespace {

std::string toUtf8(const std::wstring &text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(),
                            static_cast<int>(text.size()), utf8.data(), size,
                            nullptr, nullptr);
    }
    return utf8;
}

std::wstring programName() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring name(path, length);
    name.erase(0, name.find_last_of(L"\\/") + 1);
    name.erase(name.find_last_of(L'.'));
    for (wchar_t &ch : name)
        ch = static_cast<wchar_t>(std::towupper(ch));
    return name;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    const std::wstring name = programName();
    if (const wchar_t *logPath = _wgetenv(L"OPENSCP_ARGV_PROBE_LOG")) {
        if (std::FILE *log = _wfopen(logPath, L"ab")) {
            // Unit separators between the fields of a run, and a record
            // separator after it.
            std::string record = toUtf8(name);
            for (int i = 1; i < argc; ++i)
                record += '\x1f' + toUtf8(argv[i]);
            record += '\x1e';
            std::fwrite(record.data(), 1, record.size(), log);
            std::fclose(log);
        }
    }
    if (const wchar_t *echo = _wgetenv(L"OPENSCP_ARGV_PROBE_ECHO")) {
        // ReadFile returns what a pipe holds without waiting for more.
        char buffer[4096];
        DWORD read = 0;
        DWORD written = 0;
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer),
                        &read, nullptr) &&
               read > 0 &&
               WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, read,
                         &written, nullptr)) {
        }
        const std::string reason = toUtf8(echo);
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), reason.data(),
                  static_cast<DWORD>(reason.size()), &written, nullptr);
    }
    const std::wstring exitVariable = L"OPENSCP_ARGV_PROBE_EXIT_" + name;
    const wchar_t *exitCode = _wgetenv(exitVariable.c_str());
    return exitCode ? _wtoi(exitCode) : 0;
}
