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
    return opt.protocol == Protocol::Ftps &&
           opt.port == defaultPortForProtocol(Protocol::Ftps);
}

const char *ftpUrlScheme(const SessionOptions &opt) {
    if (opt.protocol != Protocol::Ftps)
        return "ftp";
    return useImplicitFtps(opt) ? "ftps" : "ftp";
}

std::string buildFtpUrl(const SessionOptions &opt,
                        const std::string &remotePath) {
    const std::string host = curlcommon::normalizeHostAuthorityForUrl(opt.host);
    const std::string path = normalizeRemotePath(remotePath);
    return std::string(ftpUrlScheme(opt)) + "://" + host + ":" +
           std::to_string(opt.port) +
           path;
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

bool configureCommonCurlHandle(CURL *curl, const SessionOptions &opt,
                               std::string &err) {
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L) != CURLE_OK) {
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

bool runDirectoryListingCommand(const SessionOptions &opt,
                                const std::string &remotePath,
                                const char *command, std::string &payload,
                                std::string &err) {
    // Shared wire call for MLSD/LIST; parser choice is made by callers.
    payload.clear();
    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        curl_easy_cleanup(curl);
        return false;
    }

    const std::string url = buildFtpUrl(opt, normalizeRemoteDirPath(remotePath));
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, command) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                          curlcommon::appendStringCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, &payload) == CURLE_OK);
    if (!configured) {
        err = std::string("Could not configure ") + protocolLabel(opt.protocol) +
              " listing command " + command + ".";
        curl_easy_cleanup(curl);
        return false;
    }

    const CURLcode rc = curl_easy_perform(curl);
    long responseCode = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    curl_easy_cleanup(curl);
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

} // namespace

CurlFtpClient::CurlFtpClient(Protocol protocol) : protocol_(protocol) {
    if (!isFtpFamilyProtocol(protocol_))
        protocol_ = Protocol::Ftp;
    options_.protocol = protocol_;
    options_.port = defaultPortForProtocol(protocol_);
}

bool CurlFtpClient::connect(const SessionOptions &opt, std::string &err) {
    err.clear();
    interrupted_.store(false);
    if (opt.host.empty()) {
        err = "Host is required.";
        return false;
    }
    if (!isFtpFamilyProtocol(opt.protocol)) {
        err = "CurlFtpClient only supports FTP and FTPS protocols.";
        return false;
    }
    if (opt.protocol != protocol_) {
        err = std::string("CurlFtpClient protocol mismatch: expected ") +
              protocolLabel(protocol_) + ", got " + protocolLabel(opt.protocol) +
              ".";
        return false;
    }
    if (opt.jump_host.has_value() && !opt.jump_host->empty()) {
        err = "FTP/FTPS backend does not support SSH jump host.";
        return false;
    }
    if (!ensureCurlInitialized(err))
        return false;

    SessionOptions normalized = opt;
    normalized.protocol = protocol_;
    if (normalized.port == 0)
        normalized.port = defaultPortForProtocol(protocol_);

    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, normalized, err)) {
        curl_easy_cleanup(curl);
        return false;
    }

    const std::string url = buildFtpUrl(normalized, "/");
    std::string sink;
    // Probe with lightweight directory listing to validate credentials/session.
    if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         curlcommon::appendStringCallback) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink) != CURLE_OK) {
        err = "Could not configure FTP connection probe.";
        curl_easy_cleanup(curl);
        return false;
    }

    const CURLcode rc = curl_easy_perform(curl);
    long responseCode = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        err = formatCurlProbeFailure(normalized.protocol, rc, responseCode);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        options_ = normalized;
        connected_ = true;
    }
    return true;
}

void CurlFtpClient::disconnect() {
    interrupted_.store(false);
    std::lock_guard<std::mutex> lk(stateMutex_);
    connected_ = false;
    options_ = SessionOptions{};
    options_.protocol = protocol_;
    options_.port = defaultPortForProtocol(protocol_);
}

void CurlFtpClient::interrupt() { interrupted_.store(true); }

bool CurlFtpClient::isConnected() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return connected_;
}

bool CurlFtpClient::list(const std::string &remote_path,
                         std::vector<FileInfo> &out, std::string &err) {
    err.clear();
    out.clear();

    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    if (!ensureCurlInitialized(err))
        return false;

    std::string mlsdPayload;
    std::string mlsdErr;
    // Prefer MLSD (structured), then fallback to LIST (text parsing).
    const bool mlsdOk =
        runDirectoryListingCommand(opt, remote_path, "MLSD", mlsdPayload, mlsdErr);
    if (mlsdOk && parseMlsdListing(mlsdPayload, out))
        return true;

    std::string listPayload;
    std::string listErr;
    const bool listOk =
        runDirectoryListingCommand(opt, remote_path, "LIST", listPayload, listErr);
    if (listOk && parseListListing(listPayload, out))
        return true;

    // Surface both command/parsing outcomes so diagnosis is actionable.
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
    err = std::string(protocolLabel(opt.protocol)) +
          " directory listing parse failed for MLSD and LIST output.";
    return false;
}

bool CurlFtpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    err.clear();
    if (resume) {
        err = std::string(protocolLabel(protocol_)) +
              " backend does not support resume.";
        return false;
    }

    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    interrupted_.store(false);
    if (!ensureCurlInitialized(err))
        return false;

    std::FILE *localFile = std::fopen(local.c_str(), "wb");
    if (!localFile) {
        err = "Could not open local file for writing.";
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(localFile);
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        std::fclose(localFile);
        curl_easy_cleanup(curl);
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
        curl_easy_cleanup(curl);
        err = "Could not configure FTP download.";
        return false;
    }

    const CURLcode rc = curl_easy_perform(curl);
    std::fclose(localFile);
    curl_easy_cleanup(curl);
    // Progress callback returns CURLE_ABORTED_BY_CALLBACK on cancel/interrupt.
    if (rc == CURLE_ABORTED_BY_CALLBACK && (shouldCancel && shouldCancel())) {
        err = "Canceled by user";
        return false;
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load()) {
        err = "Interrupted";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string(protocolLabel(opt.protocol)) +
              " download failed: " + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool CurlFtpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    err.clear();
    if (resume) {
        err = std::string(protocolLabel(protocol_)) +
              " backend does not support resume.";
        return false;
    }

    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    interrupted_.store(false);
    if (!ensureCurlInitialized(err))
        return false;

    std::uint64_t total = 0;
    try {
        total = std::filesystem::file_size(local);
    } catch (...) {
        err = "Could not determine local file size.";
        return false;
    }

    std::FILE *localFile = std::fopen(local.c_str(), "rb");
    if (!localFile) {
        err = "Could not open local file for reading.";
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(localFile);
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        std::fclose(localFile);
        curl_easy_cleanup(curl);
        return false;
    }

    curlcommon::TransferProgressContext progressContext{
        progress, shouldCancel, &interrupted_, false};
    const std::string url = buildFtpUrl(opt, remote);
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
        curl_easy_cleanup(curl);
        err = "Could not configure FTP upload.";
        return false;
    }

    const CURLcode rc = curl_easy_perform(curl);
    std::fclose(localFile);
    curl_easy_cleanup(curl);
    // Match get() semantics so UI can treat cancellation uniformly.
    if (rc == CURLE_ABORTED_BY_CALLBACK && (shouldCancel && shouldCancel())) {
        err = "Canceled by user";
        return false;
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load()) {
        err = "Interrupted";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string(protocolLabel(opt.protocol)) +
              " upload failed: " + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool CurlFtpClient::exists(const std::string &remote_path, bool &isDir,
                           std::string &err) {
    (void)remote_path;
    isDir = false;
    return unsupportedFtpOperation("path existence checks", err);
}

bool CurlFtpClient::stat(const std::string &remote_path, FileInfo &info,
                         std::string &err) {
    (void)remote_path;
    info = FileInfo{};
    return unsupportedFtpOperation("metadata stat", err);
}

bool CurlFtpClient::chmod(const std::string &remote_path, std::uint32_t mode,
                          std::string &err) {
    (void)remote_path;
    (void)mode;
    return unsupportedFtpOperation("chmod", err);
}

bool CurlFtpClient::chown(const std::string &remote_path, std::uint32_t uid,
                          std::uint32_t gid, std::string &err) {
    (void)remote_path;
    (void)uid;
    (void)gid;
    return unsupportedFtpOperation("chown", err);
}

bool CurlFtpClient::setTimes(const std::string &remote_path, std::uint64_t atime,
                             std::uint64_t mtime, std::string &err) {
    (void)remote_path;
    (void)atime;
    (void)mtime;
    return unsupportedFtpOperation("timestamp updates", err);
}

bool CurlFtpClient::mkdir(const std::string &remote_dir, std::string &err,
                          unsigned int mode) {
    (void)remote_dir;
    (void)mode;
    return unsupportedFtpOperation("mkdir", err);
}

bool CurlFtpClient::removeFile(const std::string &remote_path,
                               std::string &err) {
    (void)remote_path;
    return unsupportedFtpOperation("file deletion", err);
}

bool CurlFtpClient::removeDir(const std::string &remote_dir, std::string &err) {
    (void)remote_dir;
    return unsupportedFtpOperation("directory deletion", err);
}

bool CurlFtpClient::rename(const std::string &from, const std::string &to,
                           std::string &err, bool overwrite) {
    (void)from;
    (void)to;
    (void)overwrite;
    return unsupportedFtpOperation("rename", err);
}

std::unique_ptr<SftpClient>
CurlFtpClient::newConnectionLike(const SessionOptions &opt, std::string &err) {
    const Protocol nextProtocol =
        isFtpFamilyProtocol(opt.protocol) ? opt.protocol : protocol_;
    auto ptr = std::make_unique<CurlFtpClient>(nextProtocol);
    if (!ptr->connect(opt, err))
        return nullptr;
    return ptr;
}

} // namespace openscp
