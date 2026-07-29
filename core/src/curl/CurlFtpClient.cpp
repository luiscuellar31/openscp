// FTP/FTPS backend implementation based on libcurl.
#include "openscp/CurlFtpClient.hpp"

#include "CurlBackendCommon.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace openscp {
namespace {

bool isFtpFamilyProtocol(Protocol protocol) {
    return protocol == Protocol::Ftp || protocol == Protocol::Ftps;
}

const char *protocolLabel(Protocol protocol) {
    return protocol == Protocol::Ftps ? "FTPS" : "FTP";
}

bool unsupportedFtpOperation(const char *what, std::string &err) {
    err = std::string("FTP/FTPS backend does not support ") + what + ".";
    return false;
}

using curlcommon::ensureCurlInitialized;
using curlcommon::parseUnsignedDec;
using curlcommon::toLowerAscii;
using curlcommon::trimAscii;

std::string trimAsciiLeft(std::string s) {
    auto isWs = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isWs(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    return s;
}

std::string normalizeRemotePath(std::string path) {
    if (path.empty())
        return "/";
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    return path;
}

std::string normalizeRemoteDirPath(std::string path) {
    path = normalizeRemotePath(std::move(path));
    if (path != "/" && !path.empty() && path.back() != '/')
        path.push_back('/');
    return path;
}

class InterruptResetGuard {
    public:
    explicit InterruptResetGuard(std::atomic<bool> &interrupted)
        : interrupted_(interrupted) {}
    ~InterruptResetGuard() { interrupted_.store(false); }

    private:
    std::atomic<bool> &interrupted_;
};

bool rejectInterruptedBeforePerform(
    const std::atomic<bool> *interrupted, std::string &err,
    CURLcode *curlCodeOut = nullptr) {
    if (!interrupted || !interrupted->load())
        return false;
    if (curlCodeOut)
        *curlCodeOut = CURLE_ABORTED_BY_CALLBACK;
    err = "Interrupted";
    return true;
}

bool normalizeFtpCommandRoot(const char *entryPath, std::string &root,
                             std::string &err) {
    root = "/";
    if (!entryPath || !*entryPath)
        return true;
    std::string path(entryPath);
    if (!curlcommon::validateRemotePath(path, "FTP login root", err))
        return false;
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    root = std::move(path);
    return true;
}

std::uint32_t parseUnixPermBits(const std::string &perm) {
    if (perm.empty())
        return 0;
    std::uint32_t mode = 0;
    if (perm[0] == 'd')
        mode |= 0040000u;
    else if (perm[0] == 'l')
        mode |= 0120000u;
    else if (perm[0] == '-')
        mode |= 0100000u;

    if (perm.size() < 10)
        return mode;

    const std::uint32_t bits[9] = {0400u, 0200u, 0100u, 040u, 020u,
                                   010u,  04u,   02u,   01u};
    for (int i = 0; i < 9; ++i) {
        const char c = perm[1 + i];
        if (c != '-' && c != '\0')
            mode |= bits[i];
    }
    return mode;
}

bool parseMlsdUtcTimestamp(const std::string &raw, std::uint64_t &outEpoch) {
    if (raw.size() < 14)
        return false;
    std::uint64_t y = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
    if (!parseUnsignedDec(std::string_view(raw).substr(0, 4), y) ||
        !parseUnsignedDec(std::string_view(raw).substr(4, 2), mon) ||
        !parseUnsignedDec(std::string_view(raw).substr(6, 2), day) ||
        !parseUnsignedDec(std::string_view(raw).substr(8, 2), hh) ||
        !parseUnsignedDec(std::string_view(raw).substr(10, 2), mm) ||
        !parseUnsignedDec(std::string_view(raw).substr(12, 2), ss)) {
        return false;
    }
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 ||
        ss > 60 || y < 1970 || y > 9999) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = static_cast<int>(y - 1900);
    tm.tm_mon = static_cast<int>(mon - 1);
    tm.tm_mday = static_cast<int>(day);
    tm.tm_hour = static_cast<int>(hh);
    tm.tm_min = static_cast<int>(mm);
    tm.tm_sec = static_cast<int>(ss);
    tm.tm_isdst = 0;
    std::tm localCopy = tm;
#ifdef _WIN32
    // MLSD timestamps are UTC; _mkgmtime/timegm convert tm as UTC.
    const std::time_t tt = _mkgmtime(&localCopy);
#else
    const std::time_t tt = timegm(&localCopy);
#endif
    if (tt < 0)
        return false;
    outEpoch = static_cast<std::uint64_t>(tt);
    return true;
}

bool useImplicitFtps(const SessionOptions &opt) {
    if (opt.protocol != Protocol::Ftps)
        return false;
    switch (normalizeFtpsMode(opt.ftps_mode)) {
    case FtpsMode::ImplicitTls:
        return true;
    case FtpsMode::ExplicitTls:
        return false;
    case FtpsMode::Auto:
        return opt.port == defaultPortForProtocol(Protocol::Ftps);
    }
    return false;
}

const char *ftpUrlScheme(const SessionOptions &opt) {
    if (opt.protocol != Protocol::Ftps)
        return "ftp";
    return useImplicitFtps(opt) ? "ftps" : "ftp";
}

std::string buildFtpUrl(const SessionOptions &opt,
                        const std::string &remotePath) {
    const std::string host = curlcommon::normalizeHostAuthorityForUrl(opt.host);
    const std::string path =
        curlcommon::encodeUrlPath(normalizeRemotePath(remotePath));
    return std::string(ftpUrlScheme(opt)) + "://" + host + ":" +
           std::to_string(opt.port) +
           path;
}

std::string ftpDestinationKey(const SessionOptions &opt,
                              const std::string &remotePath) {
    return std::string("remote:") + protocolStorageName(opt.protocol) + ":" +
           toLowerAscii(opt.host) + ":" + std::to_string(opt.port) + ":" +
           opt.username + ":" + normalizeRemotePath(remotePath);
}

std::string formatCurlProbeFailure(Protocol protocol, CURLcode code,
                                   long responseCode) {
    std::string msg = std::string(protocolLabel(protocol)) +
                      " connect probe failed: " + curl_easy_strerror(code);
    if (responseCode > 0) {
        msg += " (server response ";
        msg += std::to_string(responseCode);
        msg += ")";
    }
    return msg;
}

RemoteError ftpErrorFromResult(CURLcode code, long responseCode,
                               const std::string &message,
                               bool commitUncertain = false) {
    RemoteError error = curlcommon::errorFromCurl(
        code, message, responseCode, commitUncertain);
    if (responseCode == 530) {
        error.kind = RemoteErrorKind::Authentication;
        error.transient = false;
    } else if (responseCode == 550 || responseCode == 553) {
        error.kind = RemoteErrorKind::PermissionDenied;
        error.transient = false;
    } else if (responseCode == 452 || responseCode == 552) {
        error.kind = RemoteErrorKind::InsufficientSpace;
        error.transient = false;
        error.commit_uncertain = false;
    } else if (responseCode == 450 || responseCode == 451) {
        error.kind = RemoteErrorKind::RemoteIo;
        error.transient = true;
    } else if (responseCode == 421 || responseCode == 425 ||
               responseCode == 426) {
        error.kind = RemoteErrorKind::Connection;
        error.transient = true;
    }
    return error;
}

bool configureCommonCurlHandle(CURL *curl, const SessionOptions &opt,
                               std::string &err) {
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L) != CURLE_OK) {
        err = "Could not configure FTP client timeouts.";
        return false;
    }

    const std::string username =
        opt.username.empty() ? std::string("anonymous") : opt.username;
    const std::string password =
        (opt.username.empty() && (!opt.password || opt.password->empty()))
            ? std::string("anonymous@openscp.local")
            : (opt.password ? *opt.password : std::string());

    if (curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str()) != CURLE_OK) {
        err = "Could not set FTP credentials.";
        return false;
    }

    if (opt.protocol == Protocol::Ftps) {
        if (curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL) != CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_FTPSSLAUTH, CURLFTPAUTH_TLS) !=
                CURLE_OK) {
            err = "Could not configure FTPS TLS mode.";
            return false;
        }
        if (!curlcommon::configureTlsVerification(
                curl, opt.ftps_verify_peer, opt.ftps_ca_cert_path,
                "Could not configure FTPS certificate verification.",
                "Could not configure FTPS CA certificate path.", err)) {
            return false;
        }
    } else {
        if (curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_NONE) !=
            CURLE_OK) {
            err = "Could not configure FTP TLS mode.";
            return false;
        }
    }

    return curlcommon::configureProxy(curl, opt, "FTP", "FTP/FTPS", err);
}

bool runDirectoryListingCommand(CURL *curl, const SessionOptions &opt,
                                const std::string &remotePath,
                                const char *command, std::string &payload,
                                const std::atomic<bool> *interrupted,
                                std::string &err, CURLcode *curlCodeOut = nullptr,
                                long *responseCodeOut = nullptr) {
    // Shared wire call for MLSD/LIST; parser choice is made by callers.
    payload.clear();
    if (curlCodeOut)
        *curlCodeOut = CURLE_OK;
    if (responseCodeOut)
        *responseCodeOut = 0;
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, err)) {
        return false;
    }

    curlcommon::TransferProgressContext cancelContext{
        {}, {}, interrupted, false};
    const std::string url = buildFtpUrl(opt, normalizeRemoteDirPath(remotePath));
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, command) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                          curlcommon::appendStringCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, &payload) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelContext) ==
         CURLE_OK);
    if (!configured) {
        err = std::string("Could not configure ") + protocolLabel(opt.protocol) +
              " listing command " + command + ".";
        return false;
    }
    if (rejectInterruptedBeforePerform(interrupted, err, curlCodeOut))
        return false;

    const CURLcode rc = curl_easy_perform(curl);
    long responseCode = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    if (curlCodeOut)
        *curlCodeOut = rc;
    if (responseCodeOut)
        *responseCodeOut = responseCode;
    if (interrupted && interrupted->load()) {
        if (curlCodeOut)
            *curlCodeOut = CURLE_ABORTED_BY_CALLBACK;
        err = "Interrupted";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string(protocolLabel(opt.protocol)) + " listing command " +
              command + " failed: " + curl_easy_strerror(rc);
        return false;
    }
    if (responseCode >= 400) {
        err = std::string(protocolLabel(opt.protocol)) + " listing command " +
              command + " was rejected (server response " +
              std::to_string(responseCode) + ").";
        return false;
    }
    return true;
}

std::string remoteParentPath(const std::string &rawPath) {
    std::string path = normalizeRemotePath(rawPath);
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    const std::size_t slash = path.find_last_of('/');
    if (slash == 0 || slash == std::string::npos)
        return "/";
    return path.substr(0, slash);
}

std::string remoteBaseName(const std::string &rawPath) {
    std::string path = normalizeRemotePath(rawPath);
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path == "/")
        return "/";
    return path.substr(path.find_last_of('/') + 1);
}

bool runFtpCommands(CURL *curl, const SessionOptions &opt,
                    const std::vector<std::string> &commands,
                    const std::atomic<bool> *interrupted, std::string &err,
                    CURLcode &curlCodeOut, long &responseCodeOut) {
    curlCodeOut = CURLE_OK;
    responseCodeOut = 0;
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, err))
        return false;

    struct curl_slist *quote = nullptr;
    for (const std::string &command : commands)
        quote = curl_slist_append(quote, command.c_str());
    if (!quote) {
        err = "Could not allocate FTP command list.";
        return false;
    }

    std::string sink;
    curlcommon::TransferProgressContext cancelContext{
        {}, {}, interrupted, false};
    const std::string url = buildFtpUrl(opt, "/");
    const bool configured =
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_QUOTE, quote) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         curlcommon::appendStringCallback) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                         curlcommon::transferProgressCallback) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelContext) == CURLE_OK;
    if (!configured) {
        err = "Could not configure FTP command request.";
        curl_slist_free_all(quote);
        return false;
    }
    if (rejectInterruptedBeforePerform(interrupted, err, &curlCodeOut)) {
        curl_slist_free_all(quote);
        return false;
    }

    curlCodeOut = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCodeOut);
    curl_slist_free_all(quote);
    if (curlCodeOut != CURLE_OK) {
        err = std::string(protocolLabel(opt.protocol)) +
              " command failed: " + curl_easy_strerror(curlCodeOut);
        if (responseCodeOut > 0)
            err += " (server response " + std::to_string(responseCodeOut) + ")";
        return false;
    }
    if (responseCodeOut >= 400) {
        err = std::string(protocolLabel(opt.protocol)) +
              " command was rejected (server response " +
              std::to_string(responseCodeOut) + ").";
        return false;
    }
    return true;
}

bool parseMlsdLine(const std::string &raw, FileInfo &info, bool &emit) {
    emit = false;
    std::string line = raw;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    line = trimAscii(line);
    if (line.empty())
        return true;

    const std::size_t sep = line.find_first_of(" \t");
    if (sep == std::string::npos)
        return false;

    const std::string factsPart = line.substr(0, sep);
    std::string name = trimAsciiLeft(line.substr(sep + 1));
    if (name.empty())
        return false;
    if (name == "." || name == "..")
        return true;

    FileInfo parsed{};
    parsed.name = name;
    std::string type;

    std::size_t start = 0;
    // Parse MLSD "facts" section: key=value;key=value;...
    while (start < factsPart.size()) {
        const std::size_t end = factsPart.find(';', start);
        const std::string fact = (end == std::string::npos)
                                     ? factsPart.substr(start)
                                     : factsPart.substr(start, end - start);
        start = (end == std::string::npos) ? factsPart.size() : end + 1;
        if (fact.empty())
            continue;
        const std::size_t eq = fact.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = toLowerAscii(fact.substr(0, eq));
        const std::string value = fact.substr(eq + 1);
        if (key == "type") {
            type = toLowerAscii(value);
        } else if (key == "size") {
            std::uint64_t sz = 0;
            if (parseUnsignedDec(value, sz)) {
                parsed.size = sz;
                parsed.has_size = true;
            }
        } else if (key == "modify") {
            std::uint64_t ts = 0;
            if (parseMlsdUtcTimestamp(value, ts))
                parsed.mtime = ts;
        } else if (key == "unix.mode") {
            char *endp = nullptr;
            errno = 0;
            const unsigned long mode = std::strtoul(value.c_str(), &endp, 8);
            if (errno == 0 && endp && *endp == '\0') {
                parsed.mode = static_cast<std::uint32_t>(mode & 07777u);
            }
        } else if (key == "unix.uid") {
            std::uint64_t uid = 0;
            if (parseUnsignedDec(value, uid))
                parsed.uid = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    uid, std::numeric_limits<std::uint32_t>::max()));
        } else if (key == "unix.gid") {
            std::uint64_t gid = 0;
            if (parseUnsignedDec(value, gid))
                parsed.gid = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    gid, std::numeric_limits<std::uint32_t>::max()));
        }
    }

    if (type.empty())
        return false;
    if (type == "cdir" || type == "pdir")
        return true;

    parsed.is_dir = (type == "dir");
    if (parsed.is_dir) {
        parsed.has_size = false;
        parsed.size = 0;
        if ((parsed.mode & 0170000u) == 0)
            parsed.mode |= 0040000u;
    } else if ((parsed.mode & 0170000u) == 0) {
        parsed.mode |= 0100000u;
    }

    info = std::move(parsed);
    emit = true;
    return true;
}

bool parseMlsdListing(const std::string &payload, std::vector<FileInfo> &out) {
    out.clear();
    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string normalized = trimAscii(line);
        if (normalized.empty())
            continue;
        FileInfo info{};
        bool emit = false;
        if (!parseMlsdLine(line, info, emit))
            return false;
        if (emit)
            out.push_back(std::move(info));
    }
    return true;
}

bool parseUnixListLine(const std::string &line, FileInfo &info, bool &emit) {
    emit = false;
    std::istringstream iss(line);
    std::string perm, links, owner, group, sizeTok, month, day, timeOrYear;
    if (!(iss >> perm >> links >> owner >> group >> sizeTok >> month >> day >>
          timeOrYear)) {
        return false;
    }
    std::string name;
    std::getline(iss, name);
    name = trimAscii(name);
    if (name.empty())
        return false;
    const std::size_t arrowPos = name.find(" -> ");
    if (arrowPos != std::string::npos)
        name.erase(arrowPos);
    if (name == "." || name == "..")
        return true;

    FileInfo parsed{};
    parsed.name = name;
    parsed.mode = parseUnixPermBits(perm);
    parsed.is_dir = !perm.empty() && perm[0] == 'd';
    if (!parsed.is_dir) {
        std::uint64_t sz = 0;
        if (parseUnsignedDec(sizeTok, sz)) {
            parsed.size = sz;
            parsed.has_size = true;
        }
    }
    info = std::move(parsed);
    emit = true;
    return true;
}

bool parseDosListLine(const std::string &line, FileInfo &info, bool &emit) {
    emit = false;
    std::istringstream iss(line);
    std::string dateTok, timeTok, sizeOrDir;
    if (!(iss >> dateTok >> timeTok >> sizeOrDir))
        return false;
    std::string name;
    std::getline(iss, name);
    name = trimAscii(name);
    if (name.empty())
        return false;
    if (name == "." || name == "..")
        return true;

    FileInfo parsed{};
    parsed.name = name;
    const std::string kind = toLowerAscii(sizeOrDir);
    parsed.is_dir = (kind == "<dir>");
    if (parsed.is_dir) {
        parsed.mode = 0040000u;
    } else {
        std::string normalizedSize = sizeOrDir;
        normalizedSize.erase(
            std::remove(normalizedSize.begin(), normalizedSize.end(), ','),
            normalizedSize.end());
        std::uint64_t sz = 0;
        if (!parseUnsignedDec(normalizedSize, sz))
            return false;
        parsed.size = sz;
        parsed.has_size = true;
        parsed.mode = 0100000u;
    }
    info = std::move(parsed);
    emit = true;
    return true;
}

bool parseListListing(const std::string &payload, std::vector<FileInfo> &out) {
    out.clear();
    std::istringstream iss(payload);
    std::string line;
    bool sawContent = false;
    bool parsedAny = false;
    bool sawUnparsedLine = false;
    while (std::getline(iss, line)) {
        const std::string normalized = trimAscii(line);
        if (normalized.empty())
            continue;
        const std::string lowered = toLowerAscii(normalized);
        if (lowered.rfind("total ", 0) == 0)
            continue;
        sawContent = true;

        FileInfo info{};
        bool emit = false;
        bool ok = false;
        // Try UNIX style first, then DOS style as compatibility fallback.
        if (normalized.front() == 'd' || normalized.front() == '-' ||
            normalized.front() == 'l' || normalized.front() == 'c' ||
            normalized.front() == 'b' || normalized.front() == 's' ||
            normalized.front() == 'p') {
            ok = parseUnixListLine(normalized, info, emit);
        }
        if (!ok)
            ok = parseDosListLine(normalized, info, emit);
        if (!ok) {
            sawUnparsedLine = true;
            continue;
        }
        if (emit) {
            out.push_back(std::move(info));
            parsedAny = true;
        }
    }
    if (!sawContent)
        return true;
    return parsedAny || !sawUnparsedLine;
}

bool fetchFtpListing(CURL *curl, const SessionOptions &opt,
                     const std::string &remotePath,
                     const std::atomic<bool> *interrupted,
                     std::vector<FileInfo> &out, std::string &err,
                     CURLcode *lastCurlCode = nullptr,
                     long *lastResponseCode = nullptr) {
    std::string mlsdPayload;
    std::string mlsdErr;
    CURLcode mlsdCode = CURLE_OK;
    long mlsdResponse = 0;
    const bool mlsdOk = runDirectoryListingCommand(
        curl, opt, remotePath, "MLSD", mlsdPayload, interrupted, mlsdErr,
        &mlsdCode, &mlsdResponse);
    if (mlsdOk && parseMlsdListing(mlsdPayload, out))
        return true;
    if (interrupted && interrupted->load()) {
        if (lastCurlCode)
            *lastCurlCode = CURLE_ABORTED_BY_CALLBACK;
        if (lastResponseCode)
            *lastResponseCode = mlsdResponse;
        err = "Interrupted";
        return false;
    }

    std::string listPayload;
    std::string listErr;
    CURLcode listCode = CURLE_OK;
    long listResponse = 0;
    const bool listOk = runDirectoryListingCommand(
        curl, opt, remotePath, "LIST", listPayload, interrupted, listErr,
        &listCode, &listResponse);
    if (lastCurlCode)
        *lastCurlCode = listOk ? CURLE_OK : listCode;
    if (lastResponseCode)
        *lastResponseCode = listResponse;
    if (listOk && parseListListing(listPayload, out))
        return true;

    if (!mlsdOk && !listOk) {
        err = std::string(protocolLabel(opt.protocol)) +
              " directory listing failed. MLSD: " + mlsdErr +
              " | LIST: " + listErr;
        return false;
    }
    if (mlsdOk && !listOk) {
        err = std::string(protocolLabel(opt.protocol)) +
              " directory listing parse failed for MLSD output, and LIST "
              "fallback failed: " +
              listErr;
        return false;
    }
    if (lastCurlCode)
        *lastCurlCode = CURLE_WEIRD_SERVER_REPLY;
    err = std::string(protocolLabel(opt.protocol)) +
          " directory listing parse failed for MLSD and LIST output.";
    return false;
}

} // namespace

CurlFtpClient::CurlFtpClient(Protocol protocol) : protocol_(protocol) {
    if (!isFtpFamilyProtocol(protocol_))
        protocol_ = Protocol::Ftp;
    options_.protocol = protocol_;
    options_.port = defaultPortForProtocol(protocol_);
}

CurlFtpClient::~CurlFtpClient() { disconnect(); }

bool CurlFtpClient::connect(const SessionOptions &opt, std::string &err) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateUrlHost(opt.host, "FTP host", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (!isFtpFamilyProtocol(opt.protocol)) {
        err = "CurlFtpClient only supports FTP and FTPS protocols.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (opt.protocol != protocol_) {
        err = std::string("CurlFtpClient protocol mismatch: expected ") +
              protocolLabel(protocol_) + ", got " + protocolLabel(opt.protocol) +
              ".";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (opt.jump_host.has_value() && !opt.jump_host->empty()) {
        err = "FTP/FTPS backend does not support SSH jump host.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    SessionOptions normalized = opt;
    normalized.protocol = protocol_;
    normalized.ftps_mode = normalizeFtpsMode(normalized.ftps_mode);
    if (normalized.port == 0)
        normalized.port = defaultPortForProtocol(protocol_);

    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        connected_ = false;
    }
    if (easyHandle_) {
        curl_easy_cleanup(static_cast<CURL *>(easyHandle_));
        easyHandle_ = nullptr;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, normalized, err)) {
        curl_easy_cleanup(curl);
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    const std::string url = buildFtpUrl(normalized, "/");
    // Authenticate and obtain the login directory without opening a data
    // connection or downloading a potentially huge root listing.
    struct curl_slist *probeCommands = curl_slist_append(nullptr, "PWD");
    if (!probeCommands) {
        err = "Could not allocate FTP connection probe command.";
        curl_easy_cleanup(curl);
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }
    curlcommon::TransferProgressContext cancelContext{
        {}, {}, &interrupted_, false};
    if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_QUOTE, probeCommands) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                         curlcommon::transferProgressCallback) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelContext) !=
            CURLE_OK) {
        err = "Could not configure FTP connection probe.";
        curl_slist_free_all(probeCommands);
        curl_easy_cleanup(curl);
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    CURLcode rc = CURLE_OK;
    if (rejectInterruptedBeforePerform(&interrupted_, err, &rc)) {
        curl_slist_free_all(probeCommands);
        curl_easy_cleanup(curl);
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(rc));
        return false;
    }
    rc = curl_easy_perform(curl);
    long responseCode = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    curl_slist_free_all(probeCommands);
    if (interrupted_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        curl_easy_cleanup(curl);
        return false;
    }
    if (rc != CURLE_OK) {
        err = formatCurlProbeFailure(normalized.protocol, rc, responseCode);
        setLastOperationError(
            ftpErrorFromResult(rc, responseCode, err));
        curl_easy_cleanup(curl);
        return false;
    }

    char *entryPath = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_FTP_ENTRY_PATH, &entryPath) !=
        CURLE_OK) {
        err = "Could not determine the FTP login directory.";
        setLastOperationError(RemoteErrorKind::Protocol, err);
        curl_easy_cleanup(curl);
        return false;
    }
    std::string commandRoot;
    if (!normalizeFtpCommandRoot(entryPath, commandRoot, err)) {
        setLastOperationError(RemoteErrorKind::Protocol, err);
        curl_easy_cleanup(curl);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        options_ = normalized;
        commandRoot_ = std::move(commandRoot);
        connected_ = true;
    }
    easyHandle_ = curl;
    return true;
}

void CurlFtpClient::disconnect() {
    disconnecting_.store(true);
    interrupted_.store(true);
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (easyHandle_) {
        curl_easy_cleanup(static_cast<CURL *>(easyHandle_));
        easyHandle_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        connected_ = false;
        options_ = SessionOptions{};
        options_.protocol = protocol_;
        options_.port = defaultPortForProtocol(protocol_);
        commandRoot_ = "/";
    }
    interrupted_.store(false);
    disconnecting_.store(false);
}

void CurlFtpClient::interrupt() { interrupted_.store(true); }

bool CurlFtpClient::isConnected() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return connected_;
}

bool CurlFtpClient::list(const std::string &remote_path,
                         std::vector<FileInfo> &out, std::string &err) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    out.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_path, protocolLabel(protocol_),
                                        err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    CURLcode rc = CURLE_OK;
    long responseCode = 0;
    const bool ok = fetchFtpListing(static_cast<CURL *>(easyHandle_), opt,
                                    remote_path, &interrupted_, out, err, &rc,
                                    &responseCode);
    if (!ok) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load())
            err = "Interrupted";
        setLastOperationError(
            ftpErrorFromResult(rc, responseCode, err));
    }
    return ok;
}

bool CurlFtpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (resume) {
        err = std::string(protocolLabel(protocol_)) +
              " backend does not support resume.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote, protocolLabel(protocol_), err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (normalizeRemotePath(remote) == "/" || local.empty()) {
        err = "FTP download requires a file path and local destination.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    const std::string partial = curlcommon::localPartialPath(local);
    curlcommon::ActiveDestinationLease destinationLease(
        curlcommon::localDestinationKey(local));
    curlcommon::ActiveDestinationLease partialLease(
        curlcommon::localDestinationKey(partial));
    if (!destinationLease.acquired() || !partialLease.acquired()) {
        err = "Another transfer is already using this local destination.";
        setLastOperationError(RemoteErrorKind::Conflict, err);
        return false;
    }
    std::FILE *localFile = std::fopen(partial.c_str(), "wb");
    if (!localFile) {
        err = "Could not open local partial file for writing.";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }

    CURL *curl = static_cast<CURL *>(easyHandle_);
    if (!curl) {
        std::fclose(localFile);
        err = "Could not create CURL handle.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, err)) {
        std::fclose(localFile);
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    curlcommon::TransferProgressContext progressContext{
        progress, shouldCancel, &interrupted_, false};
    const std::string url = buildFtpUrl(opt, remote);
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                          curlcommon::writeFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, localFile) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured) {
        std::fclose(localFile);
        err = "Could not configure FTP download.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    const bool canceledBeforeTransfer = shouldCancel && shouldCancel();
    if (canceledBeforeTransfer || interrupted_.load()) {
        std::fclose(localFile);
        err = canceledBeforeTransfer ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    const CURLcode rc = curl_easy_perform(curl);
    long responseCode = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    const bool userCanceled = shouldCancel && shouldCancel();
    if (userCanceled || interrupted_.load()) {
        std::fclose(localFile);
        err = userCanceled ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    if (rc != CURLE_OK) {
        std::fclose(localFile);
        err = std::string(protocolLabel(opt.protocol)) +
              " download failed: " + curl_easy_strerror(rc);
        setLastOperationError(
            ftpErrorFromResult(rc, responseCode, err));
        return false;
    }
    if (responseCode >= 400) {
        std::fclose(localFile);
        err = std::string(protocolLabel(opt.protocol)) +
              " download was rejected (server response " +
              std::to_string(responseCode) + ").";
        setLastOperationError(curlcommon::errorFromCurl(
            CURLE_REMOTE_FILE_NOT_FOUND, err, responseCode));
        return false;
    }
    if (!curlcommon::flushAndSyncFile(localFile, err)) {
        std::fclose(localFile);
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    if (std::fclose(localFile) != 0) {
        err = "Could not close local partial file after download.";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    const bool canceledBeforeCommit = shouldCancel && shouldCancel();
    if (canceledBeforeCommit || interrupted_.load()) {
        err = canceledBeforeCommit ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    if (!curlcommon::atomicReplaceLocalFile(partial, local, err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    return true;
}

bool CurlFtpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (resume) {
        err = std::string(protocolLabel(protocol_)) +
              " backend does not support resume.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote, protocolLabel(protocol_), err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (normalizeRemotePath(remote) == "/" || local.empty()) {
        err = "FTP upload requires a local file and remote file path.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    SessionOptions opt;
    std::string commandRoot;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
        commandRoot = commandRoot_;
    }
    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    const std::string remotePartial = normalizeRemotePath(remote) + ".part";
    curlcommon::ActiveDestinationLease destinationLease(
        ftpDestinationKey(opt, remote));
    curlcommon::ActiveDestinationLease partialLease(
        ftpDestinationKey(opt, remotePartial));
    if (!destinationLease.acquired() || !partialLease.acquired()) {
        err = "Another transfer is already using this remote destination.";
        setLastOperationError(RemoteErrorKind::Conflict, err);
        return false;
    }

    std::uint64_t total = 0;
    try {
        total = std::filesystem::file_size(local);
    } catch (...) {
        err = "Could not determine local file size.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    std::FILE *localFile = std::fopen(local.c_str(), "rb");
    if (!localFile) {
        err = "Could not open local file for reading.";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }

    CURL *curl = static_cast<CURL *>(easyHandle_);
    if (!curl) {
        std::fclose(localFile);
        err = "Could not create CURL handle.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, err)) {
        std::fclose(localFile);
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    curlcommon::TransferProgressContext progressContext{
        progress, shouldCancel, &interrupted_, true};
    const std::string url = buildFtpUrl(opt, remotePartial);
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                          curlcommon::readFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READDATA, localFile) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                          static_cast<curl_off_t>(total)) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS,
                          CURLFTP_CREATE_DIR_RETRY) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured) {
        std::fclose(localFile);
        err = "Could not configure FTP upload.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    const bool canceledBeforeTransfer = shouldCancel && shouldCancel();
    if (canceledBeforeTransfer || interrupted_.load()) {
        std::fclose(localFile);
        err = canceledBeforeTransfer ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    const CURLcode rc = curl_easy_perform(curl);
    std::fclose(localFile);
    long responseCode = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    const bool userCanceled = shouldCancel && shouldCancel();
    if (userCanceled || interrupted_.load()) {
        err = userCanceled ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string(protocolLabel(opt.protocol)) +
              " upload failed: " + curl_easy_strerror(rc);
        setLastOperationError(
            ftpErrorFromResult(rc, responseCode, err));
        return false;
    }
    if (responseCode >= 400) {
        err = std::string(protocolLabel(opt.protocol)) +
              " upload was rejected (server response " +
              std::to_string(responseCode) + ").";
        setLastOperationError(curlcommon::errorFromCurl(
            CURLE_UPLOAD_FAILED, err, responseCode));
        return false;
    }

    const bool canceledBeforeCommit = shouldCancel && shouldCancel();
    if (canceledBeforeCommit || interrupted_.load()) {
        err = canceledBeforeCommit ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }

    CURLcode renameCode = CURLE_OK;
    long renameResponse = 0;
    const std::vector<std::string> renameCommands = {
        "RNFR " + curlcommon::ftpCommandPath(commandRoot, remotePartial),
        "RNTO " + curlcommon::ftpCommandPath(commandRoot, remote),
    };
    if (!runFtpCommands(curl, opt, renameCommands, &interrupted_, err,
                        renameCode, renameResponse)) {
        setLastOperationError(
            ftpErrorFromResult(renameCode, renameResponse, err, true));
        return false;
    }
    return true;
}

bool CurlFtpClient::exists(const std::string &remote_path, bool &isDir,
                           std::string &err) {
    isDir = false;
    FileInfo info{};
    if (!stat(remote_path, info, err))
        return false;
    isDir = info.is_dir;
    return true;
}

bool CurlFtpClient::stat(const std::string &remote_path, FileInfo &info,
                         std::string &err) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    info = FileInfo{};
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_path, protocolLabel(protocol_),
                                        err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    const std::string target = normalizeRemotePath(remote_path);
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    if (target == "/") {
        info.name = "/";
        info.is_dir = true;
        info.mode = 0040000u;
        return true;
    }
    std::vector<FileInfo> parentEntries;
    CURLcode rc = CURLE_OK;
    long responseCode = 0;
    if (!fetchFtpListing(static_cast<CURL *>(easyHandle_), opt,
                         remoteParentPath(target), &interrupted_, parentEntries,
                         err, &rc, &responseCode)) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load())
            err = "Interrupted";
        setLastOperationError(
            ftpErrorFromResult(rc, responseCode, err));
        return false;
    }

    const std::string name = remoteBaseName(target);
    const auto it = std::find_if(
        parentEntries.begin(), parentEntries.end(),
        [&name](const FileInfo &entry) { return entry.name == name; });
    if (it == parentEntries.end()) {
        err.clear(); // "not found" remains source-compatible and non-exceptional.
        setLastOperationError(RemoteErrorKind::NotFound,
                              "Remote path was not found.", 550);
        return false;
    }
    info = *it;
    return true;
}

bool CurlFtpClient::chmod(const std::string &remote_path, std::uint32_t mode,
                          std::string &err) {
    clearLastOperationError();
    (void)remote_path;
    (void)mode;
    const bool ok = unsupportedFtpOperation("chmod", err);
    setLastOperationError(RemoteErrorKind::Unsupported, err);
    return ok;
}

bool CurlFtpClient::chown(const std::string &remote_path, std::uint32_t uid,
                          std::uint32_t gid, std::string &err) {
    clearLastOperationError();
    (void)remote_path;
    (void)uid;
    (void)gid;
    const bool ok = unsupportedFtpOperation("chown", err);
    setLastOperationError(RemoteErrorKind::Unsupported, err);
    return ok;
}

bool CurlFtpClient::setTimes(const std::string &remote_path, std::uint64_t atime,
                             std::uint64_t mtime, std::string &err) {
    clearLastOperationError();
    (void)remote_path;
    (void)atime;
    (void)mtime;
    const bool ok = unsupportedFtpOperation("timestamp updates", err);
    setLastOperationError(RemoteErrorKind::Unsupported, err);
    return ok;
}

bool CurlFtpClient::mkdir(const std::string &remote_dir, std::string &err,
                          unsigned int mode) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    (void)mode;
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_dir, protocolLabel(protocol_),
                                        err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    const std::string target = normalizeRemotePath(remote_dir);
    if (target == "/")
        return true;

    SessionOptions opt;
    std::string commandRoot;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
        commandRoot = commandRoot_;
    }
    CURLcode rc = CURLE_OK;
    long responseCode = 0;
    const bool ok = runFtpCommands(
        static_cast<CURL *>(easyHandle_), opt,
        {"MKD " + curlcommon::ftpCommandPath(commandRoot, target)},
        &interrupted_, err, rc, responseCode);
    if (!ok)
        setLastOperationError(ftpErrorFromResult(rc, responseCode, err, true));
    return ok;
}

bool CurlFtpClient::removeFile(const std::string &remote_path,
                               std::string &err) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_path, protocolLabel(protocol_),
                                        err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    const std::string target = normalizeRemotePath(remote_path);
    if (target == "/") {
        err = "Refusing to delete the FTP server root.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    std::string commandRoot;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
        commandRoot = commandRoot_;
    }
    CURLcode rc = CURLE_OK;
    long responseCode = 0;
    const bool ok = runFtpCommands(
        static_cast<CURL *>(easyHandle_), opt,
        {"DELE " + curlcommon::ftpCommandPath(commandRoot, target)},
        &interrupted_, err, rc, responseCode);
    if (!ok)
        setLastOperationError(ftpErrorFromResult(rc, responseCode, err, true));
    return ok;
}

bool CurlFtpClient::removeDir(const std::string &remote_dir, std::string &err) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_dir, protocolLabel(protocol_),
                                        err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    const std::string target = normalizeRemotePath(remote_dir);
    if (target == "/") {
        err = "Refusing to delete the FTP server root.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    std::string commandRoot;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
        commandRoot = commandRoot_;
    }
    CURLcode rc = CURLE_OK;
    long responseCode = 0;
    const bool ok = runFtpCommands(
        static_cast<CURL *>(easyHandle_), opt,
        {"RMD " + curlcommon::ftpCommandPath(commandRoot, target)},
        &interrupted_, err, rc, responseCode);
    if (!ok)
        setLastOperationError(ftpErrorFromResult(rc, responseCode, err, true));
    return ok;
}

bool CurlFtpClient::rename(const std::string &from, const std::string &to,
                           std::string &err, bool overwrite) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(from, protocolLabel(protocol_), err) ||
        !curlcommon::validateRemotePath(to, protocolLabel(protocol_), err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    const std::string source = normalizeRemotePath(from);
    const std::string destination = normalizeRemotePath(to);
    if (source == "/" || destination == "/") {
        err = "Refusing to rename the FTP server root.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    std::string commandRoot;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
        commandRoot = commandRoot_;
    }
    if (!overwrite) {
        std::vector<FileInfo> entries;
        CURLcode listCode = CURLE_OK;
        long listResponse = 0;
        if (!fetchFtpListing(static_cast<CURL *>(easyHandle_), opt,
                             remoteParentPath(destination), &interrupted_,
                             entries, err, &listCode, &listResponse)) {
            setLastOperationError(
                ftpErrorFromResult(listCode, listResponse, err));
            return false;
        }
        const std::string destinationName = remoteBaseName(destination);
        const bool existsAlready = std::any_of(
            entries.begin(), entries.end(), [&](const FileInfo &entry) {
                return entry.name == destinationName;
            });
        if (existsAlready) {
            err = "FTP rename destination already exists.";
            setLastOperationError(RemoteErrorKind::Conflict, err, 550);
            return false;
        }
    }

    CURLcode rc = CURLE_OK;
    long responseCode = 0;
    const bool ok = runFtpCommands(
        static_cast<CURL *>(easyHandle_), opt,
        {"RNFR " + curlcommon::ftpCommandPath(commandRoot, source),
         "RNTO " + curlcommon::ftpCommandPath(commandRoot, destination)},
        &interrupted_, err, rc, responseCode);
    if (!ok)
        setLastOperationError(ftpErrorFromResult(rc, responseCode, err, true));
    return ok;
}

std::unique_ptr<RemoteClient>
CurlFtpClient::newConnectionLike(const SessionOptions &opt, std::string &err) {
    const Protocol nextProtocol =
        isFtpFamilyProtocol(opt.protocol) ? opt.protocol : protocol_;
    auto ptr = std::make_unique<CurlFtpClient>(nextProtocol);
    if (!ptr->connect(opt, err))
        return nullptr;
    return ptr;
}

} // namespace openscp
