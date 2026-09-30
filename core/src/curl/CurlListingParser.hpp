// Internal, side-effect-free parsers for remote directory listing payloads.
#pragma once

#include "common/RemoteListingLimits.hpp"
#include "openscp/RemoteFileInfo.hpp"
#include "openscp/SessionOptions.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace openscp::curlparser {

inline constexpr std::size_t kMaxWebDavXmlNestingDepth = 64;
inline constexpr std::size_t kMaxWebDavXmlBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxWebDavXmlNodes = 100'000;
inline constexpr std::size_t kMaxWebDavXmlAttributesPerElement = 64;

struct ListingParserLimits {
    std::size_t maxEntries = kMaxRemoteListingEntries;
    std::size_t maxNameBytes = kMaxRemoteListingNameBytes;
    std::size_t maxXmlNestingDepth = kMaxWebDavXmlNestingDepth;
    std::size_t maxXmlBytes = kMaxWebDavXmlBytes;
    // Conservative allocation/work budget, including attributes, closing
    // tags (temporary nodes in tinyxml2), and whitespace-only text segments.
    std::size_t maxXmlNodes = kMaxWebDavXmlNodes;
    std::size_t maxXmlAttributesPerElement = kMaxWebDavXmlAttributesPerElement;
};

enum class ListingParseStatus {
    Success,
    Malformed,
    ResourceLimitExceeded,
};

struct WebDavResource {
    std::string path;
    bool isDir = false;
    bool hasSize = false;
    std::uint64_t size = 0;
    bool hasMtime = false;
    std::uint64_t mtime = 0;
};

[[nodiscard]] ListingParseStatus
parseFtpMlsdListing(const std::string &payload, std::vector<FileInfo> &out,
                    const ListingParserLimits &limits = {});
// Reads the entry of a successful MLST reply. The name is the pathname the
// server echoed, so callers that need a base name set it themselves.
[[nodiscard]] ListingParseStatus parseFtpMlstReply(const std::string &reply,
                                                   FileInfo &out);
[[nodiscard]] ListingParseStatus
parseFtpListListing(const std::string &payload, std::vector<FileInfo> &out,
                    const ListingParserLimits &limits = {});
[[nodiscard]] ListingParseStatus parseWebDavPropfindResponse(
    const SessionOptions &options, const std::string &xml,
    std::vector<WebDavResource> &resources, std::string &error,
    const ListingParserLimits &limits = {});

} // namespace openscp::curlparser
