#include "curl/CurlListingParser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    if (size == 0)
        return 0;

    const bool parseMlsd = (data[0] & 1u) == 0;
    const std::string payload(reinterpret_cast<const char *>(data + 1),
                              size - 1);
    std::vector<openscp::FileInfo> entries;
    openscp::curlparser::ListingParserLimits limits;
    limits.maxEntries = 1'024;
    limits.maxNameBytes = 64 * 1'024;

    if (parseMlsd) {
        (void)openscp::curlparser::parseFtpMlsdListing(payload, entries,
                                                       limits);
    } else {
        (void)openscp::curlparser::parseFtpListListing(payload, entries,
                                                       limits);
    }
    return 0;
}
