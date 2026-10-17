#include "libssh2/detail/KnownHostsFile.hpp"

#include "common/UniqueFile.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include "common/Win32Text.hpp"

#include <windows.h>
#endif

namespace openscp::libssh2detail {
namespace {

std::string base64(const unsigned char *data, std::size_t length) {
    std::string out(4 * ((length + 2) / 3) + 1, '\0');
    const int written =
        EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), data,
                        static_cast<int>(length));
    out.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return out;
}

#ifndef _WIN32
std::string systemError(const char *where) {
    return std::string(where) + ": " + std::strerror(errno);
}
#endif

} // namespace

KnownHostsReadStatus readKnownHosts(const std::string &path,
                                    std::vector<std::string> &lines) {
    lines.clear();
    UniqueFile file(openLocalFile(path, "rb"));
    if (!file) {
        return errno == ENOENT ? KnownHostsReadStatus::Missing
                               : KnownHostsReadStatus::Unreadable;
    }
    std::string content;
    char buffer[8192];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file.get())) > 0)
        content.append(buffer, count);
    const bool readFailed = std::ferror(file.get()) != 0;
    file.reset();
    if (readFailed)
        return KnownHostsReadStatus::Unreadable;

    std::size_t start = 0;
    while (start < content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string::npos)
            end = content.size();
        std::string line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return KnownHostsReadStatus::Read;
}

std::string knownHostsLine(const std::string &hostToken,
                           const std::string &keyType, const unsigned char *key,
                           std::size_t keyLength, bool hashName) {
    std::string name = hostToken;
    if (hashName) {
        // |1|base64(salt)|base64(HMAC-SHA1(salt, host)), as ssh-keygen -H.
        unsigned char salt[20];
        if (RAND_bytes(salt, sizeof(salt)) != 1)
            return {};
        unsigned char mac[EVP_MAX_MD_SIZE];
        unsigned int macLength = 0;
        if (!HMAC(EVP_sha1(), salt, static_cast<int>(sizeof(salt)),
                  reinterpret_cast<const unsigned char *>(hostToken.data()),
                  hostToken.size(), mac, &macLength)) {
            return {};
        }
        name =
            "|1|" + base64(salt, sizeof(salt)) + "|" + base64(mac, macLength);
    }
    return name + " " + keyType + " " + base64(key, keyLength);
}

std::string
hostKeyAlgorithmPreference(const std::vector<std::string> &recordedKeyTypes) {
    struct Algorithm {
        const char *name;
        // The key type known_hosts records for keys of this algorithm.
        const char *keyType;
    };
    static constexpr Algorithm kAlgorithms[] = {
        {"ssh-ed25519", "ssh-ed25519"},
        {"ecdsa-sha2-nistp256", "ecdsa-sha2-nistp256"},
        {"rsa-sha2-512", "ssh-rsa"},
        {"rsa-sha2-256", "ssh-rsa"},
        {"ecdsa-sha2-nistp384", "ecdsa-sha2-nistp384"},
        {"ecdsa-sha2-nistp521", "ecdsa-sha2-nistp521"},
    };
    const auto recorded = [&recordedKeyTypes](const Algorithm &algorithm) {
        return std::find(recordedKeyTypes.begin(), recordedKeyTypes.end(),
                         algorithm.keyType) != recordedKeyTypes.end();
    };
    std::string preference;
    for (const bool recordedPass : {true, false}) {
        for (const Algorithm &algorithm : kAlgorithms) {
            if (recorded(algorithm) != recordedPass)
                continue;
            if (!preference.empty())
                preference.push_back(',');
            preference += algorithm.name;
        }
    }
    return preference;
}

bool appendKnownHostsLine(const std::string &path, const std::string &line,
                          std::string &error) {
    error.clear();
    if (path.empty()) {
        error = "empty known_hosts path";
        return false;
    }
#ifndef _WIN32
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string folder = path.substr(0, slash);
        if (::mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST) {
            error = systemError("mkdir");
            return false;
        }
    }
    const int fd =
        ::open(path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = systemError("open(known_hosts)");
        return false;
    }
    // A last line without its newline would run into the new one.
    std::string text;
    struct ::stat st {};
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        char last = '\n';
        if (::pread(fd, &last, 1, st.st_size - 1) == 1 && last != '\n')
            text.push_back('\n');
    }
    text += line;
    text.push_back('\n');
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n =
            ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            error = systemError("write(known_hosts)");
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0 && errno != EINVAL) {
        error = systemError("fsync(known_hosts)");
        ::close(fd);
        return false;
    }
    if (::close(fd) != 0) {
        error = systemError("close(known_hosts)");
        return false;
    }
    return true;
#else
    std::wstring widePath;
    if (!win32::toWidePath(path, widePath)) {
        error = "known_hosts path is not valid UTF-8";
        return false;
    }
    const std::size_t separator = widePath.find_last_of(L"/\\");
    if (separator != std::wstring::npos && separator > 0) {
        const std::wstring folder = widePath.substr(0, separator);
        if (!::CreateDirectoryW(folder.c_str(), nullptr) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS) {
            error = "CreateDirectory: " + win32::errorMessage(::GetLastError());
            return false;
        }
    }
    // With append access only, every write lands at the end of the file,
    // even while another program appends to it.
    HANDLE handle = ::CreateFileW(
        widePath.c_str(), FILE_READ_DATA | FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "open(known_hosts): " + win32::errorMessage(::GetLastError());
        return false;
    }
    // A last line without its newline would run into the new one.
    std::string text;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle, &size) && size.QuadPart > 0) {
        OVERLAPPED lastByte{};
        lastByte.Offset = static_cast<DWORD>(size.QuadPart - 1);
        lastByte.OffsetHigh = static_cast<DWORD>((size.QuadPart - 1) >> 32);
        char last = '\n';
        DWORD read = 0;
        if (::ReadFile(handle, &last, 1, &read, &lastByte) && read == 1 &&
            last != '\n')
            text.push_back('\n');
    }
    text += line;
    text.push_back('\n');
    DWORD written = 0;
    const bool ok =
        ::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()),
                    &written, nullptr) &&
        written == text.size() && ::FlushFileBuffers(handle);
    if (!ok)
        error = "write(known_hosts): " + win32::errorMessage(::GetLastError());
    ::CloseHandle(handle);
    return ok;
#endif
}

} // namespace openscp::libssh2detail
