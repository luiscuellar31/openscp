// Deterministic regression tests for hostile FTP and WebDAV listings.
#include "TestHarness.hpp"
#include "curl/CurlListingParser.hpp"

#include <string>
#include <vector>

namespace {

using openscp::curlparser::ListingParserLimits;
using openscp::curlparser::ListingParseStatus;

#if OPENSCP_HAS_CURL_FTP
OPENSCP_TEST(testFtpMlsdListingLimitsAreTransactional, test) {
    const std::string payload = "type=file;size=1; first\r\n"
                                "type=dir; second\r\n"
                                "type=file;size=3; third\r\n";
    std::vector<openscp::FileInfo> entries;
    ListingParserLimits limits;
    limits.maxEntries = 2;

    const ListingParseStatus status =
        openscp::curlparser::parseFtpMlsdListing(payload, entries, limits);
    test.check(status == ListingParseStatus::ResourceLimitExceeded,
               "MLSD should reject entries beyond the shared limit");
    test.check(entries.empty(),
               "a rejected MLSD payload must not expose partial results");

    limits.maxEntries = 3;
    limits.maxNameBytes = 16;
    test.check(openscp::curlparser::parseFtpMlsdListing(
                   payload, entries, limits) == ListingParseStatus::Success &&
                   entries.size() == 3,
               "MLSD should accept payloads exactly within both limits");
}

OPENSCP_TEST(testFtpListListingLimitsAndMalformedInput, test) {
    const std::string payload = "-rw-r--r-- 1 user group 1 Jan 01 2024 alpha\n"
                                "01-01-24  12:00PM       <DIR> beta\n";
    std::vector<openscp::FileInfo> entries;
    ListingParserLimits limits;
    limits.maxNameBytes = 8;

    test.check(
        openscp::curlparser::parseFtpListListing(payload, entries, limits) ==
            ListingParseStatus::ResourceLimitExceeded,
        "LIST should enforce cumulative filename bytes");
    test.check(entries.empty(),
               "a rejected LIST payload must not expose partial results");

    entries.push_back({});
    test.check(openscp::curlparser::parseFtpMlsdListing(
                   "type=file;missing-name-separator", entries) ==
                   ListingParseStatus::Malformed,
               "malformed MLSD should be reported distinctly");
    test.check(entries.empty(),
               "malformed MLSD must clear caller-owned output");
    test.check(openscp::curlparser::parseFtpListListing(
                   "not a listing", entries) == ListingParseStatus::Malformed,
               "fully malformed LIST output should be rejected");
}

OPENSCP_TEST(testFtpMlstReplyReadsTheEntryLine, test) {
    openscp::FileInfo info;
    test.check(openscp::curlparser::parseFtpMlstReply(
                   "250-Listing /dir/a file.txt\r\n"
                   " modify=20240102030405;size=42;type=file;UNIX.mode=0640; "
                   "/dir/a file.txt\r\n"
                   "250 End\r\n",
                   info) == ListingParseStatus::Success,
               "MLST should parse a file entry");
    test.check(!info.is_dir && info.has_size && info.size == 42 &&
                   info.mtime == 1704164645u && info.mode == 0100640u &&
                   info.name == "/dir/a file.txt",
               "MLST should keep the file facts and echoed pathname");

    for (const char *type : {"dir", "cdir", "pdir"}) {
        info = {};
        test.check(openscp::curlparser::parseFtpMlstReply(
                       std::string("250-Listing /dir\r\n type=") + type +
                           ";size=4096; /dir\r\n250 End\r\n",
                       info) == ListingParseStatus::Success &&
                       info.is_dir && !info.has_size,
                   std::string("MLST should report type=") + type +
                       " as a directory");
    }

    test.check(openscp::curlparser::parseFtpMlstReply(
                   "250-Listing /x\r\n size=1; /x\r\n250 End\r\n", info) ==
                   ListingParseStatus::Malformed,
               "MLST without a type fact should be rejected");
    test.check(openscp::curlparser::parseFtpMlstReply("250 End\r\n", info) ==
                   ListingParseStatus::Malformed,
               "MLST without an entry line should be rejected");
}
#endif

#if OPENSCP_HAS_CURL_WEBDAV
std::string webDavResponse(const std::string &path,
                           const std::string &property) {
    return "<d:response><d:href>" + path +
           "</d:href><d:propstat><d:status>HTTP/1.1 200 OK</d:status>"
           "<d:prop>" +
           property + "</d:prop></d:propstat></d:response>";
}

openscp::SessionOptions webDavOptions() {
    openscp::SessionOptions options;
    options.protocol = openscp::Protocol::WebDav;
    options.webdav_base_path = "/dav";
    return options;
}

OPENSCP_TEST(testWebDavDeduplicatesAndMergesProperties, test) {
    const std::string xml =
        "<d:multistatus xmlns:d=\"DAV:\">" +
        webDavResponse("/dav/file.txt", "<d:getcontentlength>7"
                                        "</d:getcontentlength>") +
        webDavResponse("/dav/file.txt", "<d:getlastmodified>Wed, 21 Oct "
                                        "2015 07:28:00 GMT"
                                        "</d:getlastmodified>") +
        "</d:multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;

    const ListingParseStatus status =
        openscp::curlparser::parseWebDavPropfindResponse(webDavOptions(), xml,
                                                         resources, error);
    test.check(status == ListingParseStatus::Success && error.empty(),
               "valid WebDAV duplicates should parse successfully");
    test.check(resources.size() == 1 && resources.front().hasSize &&
                   resources.front().size == 7 && resources.front().hasMtime,
               "duplicate WebDAV resources should merge in one entry");
}

OPENSCP_TEST(testWebDavCountsDuplicateRecordsAgainstBudget, test) {
    const std::string response = webDavResponse("/dav/repeated", "");
    const std::string xml = "<d:multistatus xmlns:d=\"DAV:\">" + response +
                            response + response + "</d:multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    ListingParserLimits limits;
    limits.maxEntries = 2;

    const ListingParseStatus status =
        openscp::curlparser::parseWebDavPropfindResponse(
            webDavOptions(), xml, resources, error, limits);
    test.check(status == ListingParseStatus::ResourceLimitExceeded,
               "duplicate response records must still consume work budget");
    test.check(resources.empty(),
               "a rejected WebDAV payload must not expose partial results");
    test.check(error.find("safety limit") != std::string::npos,
               "WebDAV limit errors should be actionable");

    limits.maxEntries = 3;
    limits.maxNameBytes = 18;
    error.clear();
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "duplicate WebDAV paths must consume the filename-byte budget");

    limits.maxNameBytes = 27;
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                       ListingParseStatus::Success &&
                   resources.size() == 1,
               "WebDAV should accept duplicate records exactly at the budget");
}

OPENSCP_TEST(testWebDavCountsUnusableResponseRecordsAgainstBudget, test) {
    const std::string xml = "<d:multistatus xmlns:d=\"DAV:\">"
                            "<d:response/><d:response/><d:response/>"
                            "</d:multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    ListingParserLimits limits;
    limits.maxEntries = 2;

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "unusable WebDAV records must not bypass the work budget");
    test.check(resources.empty(),
               "rejected unusable records must not expose partial results");
}

OPENSCP_TEST(testWebDavRejectsExcessiveNestingAndMalformedXml, test) {
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    ListingParserLimits limits;
    limits.maxXmlNestingDepth = 3;
    const std::string nested = "<d:multistatus xmlns:d=\"DAV:\"><a><b><c>" +
                               webDavResponse("/dav/file", "") +
                               "</c></b></a></d:multistatus>";

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), nested, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "WebDAV XML nesting should have an explicit safety limit");

    error.clear();
    resources.push_back({});
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), "<d:multistatus>", resources, error) ==
                   ListingParseStatus::Malformed,
               "malformed WebDAV XML should be classified separately");
    test.check(resources.empty() && !error.empty(),
               "malformed WebDAV XML should clear output and explain failure");
}
#endif

} // namespace

int main() {
    openscp::test::TestHarness harness("curl listing parser");
    return harness.run();
}
