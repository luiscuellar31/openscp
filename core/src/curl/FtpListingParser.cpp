#include "CurlBackendCommon.hpp"
#include "CurlListingParser.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
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
    std::size_t first = 0;
    while (first < value.size() &&
           isWhitespace(static_cast<unsigned char>(value[first])))
        ++first;
    value.erase(0, first);
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

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar, after
// Howard Hinnant's days_from_civil, for years from 1970. timegm took most of
// the time of a large listing, microseconds per entry on macOS. As with
// timegm, a day past the end of its month counts on into the next one.
std::uint64_t daysSinceEpoch(std::uint64_t year, std::uint64_t month,
                             std::uint64_t day) {
    year -= month <= 2 ? 1 : 0;
    const std::uint64_t era = year / 400;
    const std::uint64_t yearOfEra = year - era * 400;
    const std::uint64_t dayOfYear =
        (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const std::uint64_t dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

bool parseMlsdUtcTimestamp(std::string_view raw, std::uint64_t &outEpoch) {
    if (raw.size() < 14)
        return false;
    std::uint64_t year = 0;
    std::uint64_t month = 0;
    std::uint64_t day = 0;
    std::uint64_t hour = 0;
    std::uint64_t minute = 0;
    std::uint64_t second = 0;
    if (!parseUnsignedDec(raw.substr(0, 4), year) ||
        !parseUnsignedDec(raw.substr(4, 2), month) ||
        !parseUnsignedDec(raw.substr(6, 2), day) ||
        !parseUnsignedDec(raw.substr(8, 2), hour) ||
        !parseUnsignedDec(raw.substr(10, 2), minute) ||
        !parseUnsignedDec(raw.substr(12, 2), second)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60 || year < 1970 || year > 9999) {
        return false;
    }
    outEpoch = daysSinceEpoch(year, month, day) * 86400 + hour * 3600 +
               minute * 60 + second;
    return true;
}

bool isAsciiWhitespace(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::string_view trimAsciiView(std::string_view value) {
    while (!value.empty() && isAsciiWhitespace(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && isAsciiWhitespace(value.back()))
        value.remove_suffix(1);
    return value;
}

// Fact names ignore case (RFC 3659); `lowercase` is the name to match.
bool isFact(std::string_view name, std::string_view lowercase) {
    return name.size() == lowercase.size() &&
           std::equal(name.begin(), name.end(), lowercase.begin(),
                      [](char character, char expected) {
                          return std::tolower(static_cast<unsigned char>(
                                     character)) == expected;
                      });
}

// Splits an MLSD or MLST entry line ("fact=value;... name") and reads its
// facts. The lowercased type fact is left for the caller to interpret. A
// listing can hold many thousands of entries, so the line is read in place:
// only the name and the type are copied.
bool parseMlsxEntry(std::string_view line, FileInfo &parsed,
                    std::string &type) {
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    // RFC 3659 puts one space between the facts and the pathname, which can
    // itself start or end with spaces, so only padding before the facts goes.
    while (!line.empty() && isAsciiWhitespace(line.front()))
        line.remove_prefix(1);

    const std::size_t separator = line.find_first_of(" \t");
    if (separator == std::string_view::npos)
        return false;

    const std::string_view factsPart = line.substr(0, separator);
    const std::string_view name = line.substr(separator + 1);
    if (trimAsciiView(name).empty())
        return false;
    parsed = FileInfo{};
    parsed.name.assign(name.data(), name.size());
    type.clear();

    std::size_t start = 0;
    while (start < factsPart.size()) {
        const std::size_t end = factsPart.find(';', start);
        const std::string_view fact =
            end == std::string_view::npos
                ? factsPart.substr(start)
                : factsPart.substr(start, end - start);
        start = end == std::string_view::npos ? factsPart.size() : end + 1;
        if (fact.empty())
            continue;
        const std::size_t equals = fact.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = fact.substr(0, equals);
        const std::string_view value = fact.substr(equals + 1);
        if (isFact(key, "type")) {
            type = toLowerAscii(std::string(value));
        } else if (isFact(key, "size")) {
            std::uint64_t size = 0;
            if (parseUnsignedDec(value, size)) {
                parsed.size = size;
                parsed.has_size = true;
            }
        } else if (isFact(key, "modify")) {
            std::uint64_t timestamp = 0;
            if (parseMlsdUtcTimestamp(value, timestamp))
                parsed.mtime = timestamp;
        } else if (isFact(key, "unix.mode")) {
            // strtoul needs a terminated string; a mode fits in the
            // string's own buffer.
            const std::string mode(value);
            char *endPointer = nullptr;
            errno = 0;
            const unsigned long modeBits =
                std::strtoul(mode.c_str(), &endPointer, 8);
            if (errno == 0 && endPointer && *endPointer == '\0')
                parsed.mode = static_cast<std::uint32_t>(modeBits & 07777u);
        } else if (isFact(key, "unix.uid")) {
            std::uint64_t uid = 0;
            if (parseUnsignedDec(value, uid)) {
                parsed.uid = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    uid, std::numeric_limits<std::uint32_t>::max()));
            }
        } else if (isFact(key, "unix.gid")) {
            std::uint64_t gid = 0;
            if (parseUnsignedDec(value, gid)) {
                parsed.gid = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    gid, std::numeric_limits<std::uint32_t>::max()));
            }
        }
    }

    return true;
}

// ProFTPD reports a link as "OS.unix=symlink"; with its UseSlink option, as
// Pure-FTPd does, as "OS.unix=slink:<target>". Either way the type of the
// target is unknown.
bool isMlsxLinkType(const std::string &type) {
    return type == "os.unix=symlink" || type == "os.unix=slink" ||
           type.rfind("os.unix=slink:", 0) == 0;
}

void finishMlsxEntry(FileInfo &parsed, bool isDir, bool isLink) {
    parsed.is_dir = isDir;
    if (isLink) {
        parsed.mode = (parsed.mode & 07777u) | 0120000u;
    } else if (parsed.is_dir) {
        parsed.has_size = false;
        parsed.size = 0;
        if ((parsed.mode & 0170000u) == 0)
            parsed.mode |= 0040000u;
    } else if ((parsed.mode & 0170000u) == 0) {
        parsed.mode |= 0100000u;
    }
}

bool parseMlsdLine(std::string_view raw, FileInfo &info, bool &emit) {
    emit = false;
    if (trimAsciiView(raw).empty())
        return true;

    FileInfo parsed{};
    std::string type;
    if (!parseMlsxEntry(raw, parsed, type))
        return false;
    if (parsed.name == "." || parsed.name == "..")
        return true;
    if (type.empty())
        return false;
    if (type == "cdir" || type == "pdir")
        return true;

    finishMlsxEntry(parsed, type == "dir", isMlsxLinkType(type));
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
    name = trimAsciiLeft(name);
    if (trimAscii(name).empty())
        return false;
    // Only a link's line names its target after " -> "; elsewhere it is part
    // of the name.
    if (permissions.front() == 'l') {
        const std::size_t arrowPosition = name.find(" -> ");
        if (arrowPosition != std::string::npos)
            name.erase(arrowPosition);
    }
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
    std::string_view rest(payload);
    while (!rest.empty()) {
        const std::size_t newline = rest.find('\n');
        const std::string_view line = rest.substr(0, newline);
        rest = newline == std::string_view::npos ? std::string_view()
                                                 : rest.substr(newline + 1);
        if (trimAsciiView(line).empty())
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

ListingParseStatus parseFtpMlstReply(const std::string &reply, FileInfo &out) {
    // RFC 3659 marks the entry line of a multi-line MLST reply with a leading
    // space; the other lines carry the reply code.
    std::istringstream input(reply);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() != ' ')
            continue;
        FileInfo parsed{};
        std::string type;
        if (!parseMlsxEntry(line, parsed, type) || type.empty())
            return ListingParseStatus::Malformed;
        finishMlsxEntry(parsed,
                        type == "dir" || type == "cdir" || type == "pdir",
                        isMlsxLinkType(type));
        out = std::move(parsed);
        return ListingParseStatus::Success;
    }
    return ListingParseStatus::Malformed;
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
            // The name ends the line, so its trailing spaces are its own.
            std::string entry = trimAsciiLeft(line);
            if (!entry.empty() && entry.back() == '\r')
                entry.pop_back();
            parsed = parseUnixListLine(entry, info, emit);
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
