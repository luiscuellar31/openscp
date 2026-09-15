#include "CurlBackendCommon.hpp"
#include "CurlListingParser.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace openscp::curlparser {
namespace {

using curlcommon::parseUnsignedDec;
using curlcommon::toLowerAscii;
using curlcommon::trimAscii;

std::string trimAsciiLeft(std::string value) {
    const auto isWhitespace = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    while (!value.empty() &&
           isWhitespace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    return value;
}

std::uint32_t parseUnixPermBits(const std::string &permissions) {
    if (permissions.empty())
        return 0;
    std::uint32_t mode = 0;
    if (permissions[0] == 'd')
        mode |= 0040000u;
    else if (permissions[0] == 'l')
        mode |= 0120000u;
    else if (permissions[0] == '-')
        mode |= 0100000u;

    if (permissions.size() < 10)
        return mode;

    const std::uint32_t bits[9] = {0400u, 0200u, 0100u, 040u, 020u,
                                   010u,  04u,   02u,   01u};
    for (std::size_t index = 0; index < 9; ++index) {
        const char character = permissions[1 + index];
        if (character != '-' && character != '\0')
            mode |= bits[index];
    }
    return mode;
}

bool parseMlsdUtcTimestamp(const std::string &raw, std::uint64_t &outEpoch) {
    if (raw.size() < 14)
        return false;
    std::uint64_t year = 0;
    std::uint64_t month = 0;
    std::uint64_t day = 0;
    std::uint64_t hour = 0;
    std::uint64_t minute = 0;
    std::uint64_t second = 0;
    if (!parseUnsignedDec(std::string_view(raw).substr(0, 4), year) ||
        !parseUnsignedDec(std::string_view(raw).substr(4, 2), month) ||
        !parseUnsignedDec(std::string_view(raw).substr(6, 2), day) ||
        !parseUnsignedDec(std::string_view(raw).substr(8, 2), hour) ||
        !parseUnsignedDec(std::string_view(raw).substr(10, 2), minute) ||
        !parseUnsignedDec(std::string_view(raw).substr(12, 2), second)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60 || year < 1970 || year > 9999) {
        return false;
    }
    std::tm timestamp{};
    timestamp.tm_year = static_cast<int>(year - 1900);
    timestamp.tm_mon = static_cast<int>(month - 1);
    timestamp.tm_mday = static_cast<int>(day);
    timestamp.tm_hour = static_cast<int>(hour);
    timestamp.tm_min = static_cast<int>(minute);
    timestamp.tm_sec = static_cast<int>(second);
    timestamp.tm_isdst = 0;
#ifdef _WIN32
    const std::time_t converted = _mkgmtime(&timestamp);
#else
    const std::time_t converted = timegm(&timestamp);
#endif
    if (converted < 0)
        return false;
    outEpoch = static_cast<std::uint64_t>(converted);
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

    const std::size_t separator = line.find_first_of(" \t");
    if (separator == std::string::npos)
        return false;

    const std::string factsPart = line.substr(0, separator);
    std::string name = trimAsciiLeft(line.substr(separator + 1));
    if (name.empty())
        return false;
    if (name == "." || name == "..")
        return true;

    FileInfo parsed{};
    parsed.name = name;
    std::string type;

    std::size_t start = 0;
    while (start < factsPart.size()) {
        const std::size_t end = factsPart.find(';', start);
        const std::string fact = end == std::string::npos
                                     ? factsPart.substr(start)
                                     : factsPart.substr(start, end - start);
        start = end == std::string::npos ? factsPart.size() : end + 1;
        if (fact.empty())
            continue;
        const std::size_t equals = fact.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = toLowerAscii(fact.substr(0, equals));
        const std::string value = fact.substr(equals + 1);
        if (key == "type") {
            type = toLowerAscii(value);
        } else if (key == "size") {
            std::uint64_t size = 0;
            if (parseUnsignedDec(value, size)) {
                parsed.size = size;
                parsed.has_size = true;
            }
        } else if (key == "modify") {
            std::uint64_t timestamp = 0;
            if (parseMlsdUtcTimestamp(value, timestamp))
                parsed.mtime = timestamp;
        } else if (key == "unix.mode") {
            char *endPointer = nullptr;
            errno = 0;
            const unsigned long mode =
                std::strtoul(value.c_str(), &endPointer, 8);
            if (errno == 0 && endPointer && *endPointer == '\0')
                parsed.mode = static_cast<std::uint32_t>(mode & 07777u);
        } else if (key == "unix.uid") {
            std::uint64_t uid = 0;
            if (parseUnsignedDec(value, uid)) {
                parsed.uid = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    uid, std::numeric_limits<std::uint32_t>::max()));
            }
        } else if (key == "unix.gid") {
            std::uint64_t gid = 0;
            if (parseUnsignedDec(value, gid)) {
                parsed.gid = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    gid, std::numeric_limits<std::uint32_t>::max()));
            }
        }
    }

    if (type.empty())
        return false;
    if (type == "cdir" || type == "pdir")
        return true;

    parsed.is_dir = type == "dir";
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

bool parseUnixListLine(const std::string &line, FileInfo &info, bool &emit) {
    emit = false;
    std::istringstream input(line);
    std::string permissions;
    std::string links;
    std::string owner;
    std::string group;
    std::string sizeToken;
    std::string month;
    std::string day;
    std::string timeOrYear;
    if (!(input >> permissions >> links >> owner >> group >> sizeToken >>
          month >> day >> timeOrYear)) {
        return false;
    }
    std::string name;
    std::getline(input, name);
    name = trimAscii(name);
    if (name.empty())
        return false;
    const std::size_t arrowPosition = name.find(" -> ");
    if (arrowPosition != std::string::npos)
        name.erase(arrowPosition);
    if (name == "." || name == "..")
        return true;

    FileInfo parsed{};
    parsed.name = name;
    parsed.mode = parseUnixPermBits(permissions);
    parsed.is_dir = !permissions.empty() && permissions[0] == 'd';
    if (!parsed.is_dir) {
        std::uint64_t size = 0;
        if (parseUnsignedDec(sizeToken, size)) {
            parsed.size = size;
            parsed.has_size = true;
        }
    }
    info = std::move(parsed);
    emit = true;
    return true;
}

bool parseDosListLine(const std::string &line, FileInfo &info, bool &emit) {
    emit = false;
    std::istringstream input(line);
    std::string dateToken;
    std::string timeToken;
    std::string sizeOrDirectory;
    if (!(input >> dateToken >> timeToken >> sizeOrDirectory))
        return false;
    std::string name;
    std::getline(input, name);
    name = trimAscii(name);
    if (name.empty())
        return false;
    if (name == "." || name == "..")
        return true;

    FileInfo parsed{};
    parsed.name = name;
    const std::string kind = toLowerAscii(sizeOrDirectory);
    parsed.is_dir = kind == "<dir>";
    if (parsed.is_dir) {
        parsed.mode = 0040000u;
    } else {
        sizeOrDirectory.erase(
            std::remove(sizeOrDirectory.begin(), sizeOrDirectory.end(), ','),
            sizeOrDirectory.end());
        std::uint64_t size = 0;
        if (!parseUnsignedDec(sizeOrDirectory, size))
            return false;
        parsed.size = size;
        parsed.has_size = true;
        parsed.mode = 0100000u;
    }
    info = std::move(parsed);
    emit = true;
    return true;
}

} // namespace

ListingParseStatus parseFtpMlsdListing(const std::string &payload,
                                       std::vector<FileInfo> &out,
                                       const ListingParserLimits &limits) {
    out.clear();
    RemoteListingBudget budget(limits.maxEntries, limits.maxNameBytes);
    std::istringstream input(payload);
    std::string line;
    while (std::getline(input, line)) {
        const std::string normalized = trimAscii(line);
        if (normalized.empty())
            continue;
        FileInfo info{};
        bool emit = false;
        if (!parseMlsdLine(line, info, emit)) {
            out.clear();
            return ListingParseStatus::Malformed;
        }
        if (emit) {
            if (!budget.tryConsume(info.name.size())) {
                out.clear();
                return ListingParseStatus::ResourceLimitExceeded;
            }
            out.push_back(std::move(info));
        }
    }
    return ListingParseStatus::Success;
}

ListingParseStatus parseFtpListListing(const std::string &payload,
                                       std::vector<FileInfo> &out,
                                       const ListingParserLimits &limits) {
    out.clear();
    RemoteListingBudget budget(limits.maxEntries, limits.maxNameBytes);
    std::istringstream input(payload);
    std::string line;
    bool sawContent = false;
    bool parsedAny = false;
    bool sawUnparsedLine = false;
    while (std::getline(input, line)) {
        const std::string normalized = trimAscii(line);
        if (normalized.empty())
            continue;
        const std::string lowered = toLowerAscii(normalized);
        if (lowered.rfind("total ", 0) == 0)
            continue;
        sawContent = true;

        FileInfo info{};
        bool emit = false;
        bool parsed = false;
        if (normalized.front() == 'd' || normalized.front() == '-' ||
            normalized.front() == 'l' || normalized.front() == 'c' ||
            normalized.front() == 'b' || normalized.front() == 's' ||
            normalized.front() == 'p') {
            parsed = parseUnixListLine(normalized, info, emit);
        }
        if (!parsed)
            parsed = parseDosListLine(normalized, info, emit);
        if (!parsed) {
            sawUnparsedLine = true;
            continue;
        }
        if (emit) {
            if (!budget.tryConsume(info.name.size())) {
                out.clear();
                return ListingParseStatus::ResourceLimitExceeded;
            }
            out.push_back(std::move(info));
            parsedAny = true;
        }
    }
    if (!sawContent)
        return ListingParseStatus::Success;
    if (parsedAny || !sawUnparsedLine)
        return ListingParseStatus::Success;
    out.clear();
    return ListingParseStatus::Malformed;
}

} // namespace openscp::curlparser
