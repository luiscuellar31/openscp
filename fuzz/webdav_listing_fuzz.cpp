#include "curl/CurlListingParser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    const std::string payload(reinterpret_cast<const char *>(data), size);
    openscp::SessionOptions options;
    options.protocol = openscp::Protocol::WebDav;
    options.webdav_base_path = "/";

    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    openscp::curlparser::ListingParserLimits limits;
    limits.maxEntries = 1'024;
    limits.maxNameBytes = 64 * 1'024;
    limits.maxXmlNestingDepth = 32;
    (void)openscp::curlparser::parseWebDavPropfindResponse(
        options, payload, resources, error, limits);
    return 0;
}
