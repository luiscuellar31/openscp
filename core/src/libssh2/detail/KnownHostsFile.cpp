#include "libssh2/detail/KnownHostsFile.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
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

std::string systemError(const char *where) {
    return std::string(where) + ": " + std::strerror(errno);
}

} // namespace

KnownHostsReadResult
readKnownHosts(const std::string &path,
               const std::function<bool(const std::string &line)> &addLine) {
    KnownHostsReadResult result;
    std::error_code statusError;
    if (std::filesystem::status(path, statusError).type() ==
        std::filesystem::file_type::not_found) {
        result.status = KnownHostsReadStatus::Missing;
        return result;
    }

    result.status = KnownHostsReadStatus::Unreadable;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return result;
    std::string content;
    char buffer[8192];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        content.append(buffer, count);
    const bool readFailed = std::ferror(file) != 0;
    std::fclose(file);
    if (readFailed)
        return result;

    result.status = KnownHostsReadStatus::Read;
    std::size_t start = 0;
    while (start < content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string::npos)
            end = content.size();
        std::string line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!addLine(line))
            ++result.skippedLines;
        start = end + 1;
    }
    return result;
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
    std::FILE *file = std::fopen(path.c_str(), "a+b");
    if (!file) {
        error = systemError("fopen(known_hosts)");
        return false;
    }
    std::string text;
    if (std::fseek(file, -1, SEEK_END) == 0 && std::fgetc(file) != '\n')
        text.push_back('\n');
    text += line;
    text.push_back('\n');
    const bool ok =
        std::fseek(file, 0, SEEK_END) == 0 &&
        std::fwrite(text.data(), 1, text.size(), file) == text.size() &&
        std::fflush(file) == 0;
    if (!ok)
        error = systemError("write(known_hosts)");
    if (std::fclose(file) != 0 && ok) {
        error = systemError("close(known_hosts)");
        return false;
    }
    return ok;
#endif
}

} // namespace openscp::libssh2detail
