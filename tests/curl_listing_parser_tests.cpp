// Deterministic regression tests for hostile FTP and WebDAV listings.
#include "TestHarness.hpp"
#include "curl/CurlListingParser.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using openscp::curlparser::ListingParserLimits;
using openscp::curlparser::ListingParseStatus;

#if OPENSCP_HAS_CURL_FTP
OPENSCP_TEST(testFtpListingsWithLargeWhitespacePadding, test) {
    const std::string spaces(1024 * 1024, ' ');
    const std::string mlsx = spaces + "type=file;size=7; a file.txt\r\n";
    std::vector<openscp::FileInfo> entries;
    test.check(
        openscp::curlparser::parseFtpMlsdListing(
            spaces + "\r\n" + mlsx, entries) == ListingParseStatus::Success &&
            entries.size() == 1 && entries.front().name == "a file.txt" &&
            entries.front().has_size && entries.front().size == 7,
        "MLSD should skip large blank lines and padding before the facts "
        "while preserving the name and facts");
    openscp::FileInfo info;
    test.check(openscp::curlparser::parseFtpMlstReply(
                   "250-Listing\r\n" + mlsx + "250 End\r\n", info) ==
                       ListingParseStatus::Success &&
                   info.name == "a file.txt" && info.has_size && info.size == 7,
               "MLST should parse the same heavily padded entry");
    const std::string listing = spaces + "\r\n" + spaces +
                                "-rw-r--r-- 1 user group 7 Jan 01 2024 " +
                                spaces + "a file.txt \t\r\n";
    test.check(openscp::curlparser::parseFtpListListing(listing, entries) ==
                       ListingParseStatus::Success &&
                   entries.size() == 1 &&
                   entries.front().name == "a file.txt \t" &&
                   entries.front().has_size && entries.front().size == 7,
               "LIST should skip the padding before a name and keep the rest");
    test.check(openscp::curlparser::parseFtpMlsdListing(
                   spaces + "type=file; \t" + spaces + "\r\n", entries) ==
                       ListingParseStatus::Malformed &&
                   entries.empty(),
               "large padding must not turn a missing filename into a record");
}

OPENSCP_TEST(testFtpMlsxNamesKeepTheirSpaces, test) {
    // One space separates the facts from the name; any other space belongs to
    // the name, so these are three different files.
    std::vector<openscp::FileInfo> entries;
    test.check(openscp::curlparser::parseFtpMlsdListing(
                   "type=file;size=1;  foo\r\n"
                   "type=file;size=2; foo\r\n"
                   "type=file;size=3; foo \r\n",
                   entries) == ListingParseStatus::Success &&
                   entries.size() == 3 && entries[0].name == " foo" &&
                   entries[1].name == "foo" && entries[2].name == "foo ",
               "MLSD should keep spaces at the start and end of names");

    openscp::FileInfo info;
    test.check(openscp::curlparser::parseFtpMlstReply(
                   "250-Listing\r\n type=file;size=1; /dir/ spaced \r\n"
                   "250 End\r\n",
                   info) == ListingParseStatus::Success &&
                   info.name == "/dir/ spaced ",
               "MLST should keep spaces in the echoed pathname");
}

// Without MLSD, as on vsftpd, "notes -> old.txt" showed as "notes", the name
// of another file, and "trailing " lost its space.
OPENSCP_TEST(testFtpListNamesKeepArrowsAndTrailingSpaces, test) {
    std::vector<openscp::FileInfo> entries;
    test.check(openscp::curlparser::parseFtpListListing(
                   "-rw-r--r-- 1 u g 6 Oct 18 12:00 notes -> old.txt\r\n"
                   "drwxr-xr-x 2 u g 4096 Oct 18 12:00 a -> b\r\n"
                   "lrwxrwxrwx 1 u g 7 Oct 18 12:00 link -> target\r\n"
                   "-rw-r--r-- 1 u g 6 Oct 18 12:00 trailing \r\n",
                   entries) == ListingParseStatus::Success &&
                   entries.size() == 4,
               "LIST should parse every entry");
    if (entries.size() != 4)
        return;
    test.check(entries[0].name == "notes -> old.txt" && !entries[0].is_dir,
               "a file whose name holds an arrow should keep its whole name");
    test.check(entries[1].name == "a -> b" && entries[1].is_dir,
               "a folder whose name holds an arrow should keep its whole name");
    test.check(entries[2].name == "link",
               "a link should drop the target after its arrow");
    test.check(entries[3].name == "trailing ",
               "a name should keep its trailing space");
}

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

OPENSCP_TEST(testFtpMlsdReportsSymbolicLinks, test) {
    // ProFTPD's default form, and the RFC 3659 example form that ProFTPD
    // with UseSlink and Pure-FTPd send.
    std::vector<openscp::FileInfo> entries;
    test.check(openscp::curlparser::parseFtpMlsdListing(
                   "type=OS.unix=symlink;UNIX.mode=0777; current\r\n"
                   "type=OS.unix=slink:/srv/releases/42; previous\r\n"
                   "type=file;size=3; notes.txt\r\n",
                   entries) == ListingParseStatus::Success &&
                   entries.size() == 3,
               "MLSD with links should parse");
    if (entries.size() != 3)
        return;
    for (std::size_t index = 0; index < 2; ++index) {
        test.check(!entries[index].is_dir &&
                       (entries[index].mode & 0170000u) == 0120000u,
                   "MLSD should report " + entries[index].name +
                       " as a symbolic link");
    }
    test.check(entries[0].mode == 0120777u,
               "a link should keep its permission bits");
    test.check((entries[2].mode & 0170000u) == 0100000u,
               "a file next to the links should stay a regular file");
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

    info = {};
    test.check(openscp::curlparser::parseFtpMlstReply(
                   "250-Listing /current\r\n type=OS.unix=symlink;"
                   "UNIX.mode=0777; /current\r\n250 End\r\n",
                   info) == ListingParseStatus::Success &&
                   !info.is_dir && info.mode == 0120777u,
               "MLST should report a symbolic link as a link");

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

OPENSCP_TEST(testWebDavRejectsNonResponseNodeFlood, test) {
    std::string xml = "<multistatus>";
    xml.reserve(2'000'100);
    for (std::size_t index = 0; index < 500'000; ++index)
        xml += "<x/>";
    xml += "</multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources{{}};
    std::string error;

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "non-response nodes must be rejected by the XML budget");
    test.check(resources.empty() &&
                   error.find("safety limit") != std::string::npos,
               "XML budget failures must clear results and explain rejection");
}

OPENSCP_TEST(testWebDavXmlBudgetExactBoundaries, test) {
    const std::string xml = "<multistatus><response><href>/dav/file</href>"
                            "</response></multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    ListingParserLimits limits;
    limits.maxXmlBytes = xml.size();
    // The response record holds two opening tags, two closing tags and one
    // text segment; the multistatus tags around it take two more nodes.
    limits.maxXmlNodes = 5;
    limits.maxXmlNestingDepth = 3;
    limits.maxXmlAttributesPerElement = 0;

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                       ListingParseStatus::Success &&
                   resources.size() == 1 && resources.front().path == "/file",
               "XML exactly at every budget should preserve WebDAV results");

    --limits.maxXmlBytes;
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "XML bytes must be checked even for direct parser callers");
    ++limits.maxXmlBytes;
    --limits.maxXmlNodes;
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "one work item above the XML budget must be rejected");
    ++limits.maxXmlNodes;
    --limits.maxXmlNestingDepth;
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "XML depth must be checked before document construction");
}

OPENSCP_TEST(testWebDavXmlBudgetUnderstandsNonElementContent, test) {
    const std::string xml =
        "<?xml version=\"1.0\"?><!DOCTYPE multistatus>"
        "<multistatus a=\"<x>= >\" b='<!-- > ='>\n"
        "<!--<a><b>=--><![CDATA[<a><b>=>]]>"
        "<response><href>/dav/file</href></response></multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    ListingParserLimits limits;
    limits.maxXmlNestingDepth = 3;
    // Around the five-node record: the declaration, the doctype, the
    // multistatus tags with two attributes, the comment and the CDATA
    // section. The blank text between them is not parsed and costs nothing.
    limits.maxXmlNodes = 8;
    limits.maxXmlAttributesPerElement = 2;

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::Success,
               "quotes, comments and CDATA must not create phantom markup");
    --limits.maxXmlNodes;
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "all node types and attributes must consume the XML budget");
    ++limits.maxXmlNodes;
    --limits.maxXmlAttributesPerElement;
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "attribute values must not hide excess attributes");
}

OPENSCP_TEST(testWebDavXmlBudgetCountsEachNonElementNode, test) {
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    ListingParserLimits limits;
    // Without the repeated content, the record and the multistatus element
    // around it take two nodes each.
    limits.maxXmlNodes = 3;
    for (const std::string node :
         {"<!-- ignored -->", "<![CDATA[text]]>", "<!extension>", "text<x/>"}) {
        // The node twice, between `before` and `after`.
        const auto twice = [&node](std::string_view before,
                                   std::string_view after) {
            std::string xml(before);
            xml += node;
            xml += node;
            xml += after;
            return xml;
        };
        for (const std::string &xml :
             {twice("<multistatus>", "<response></response></multistatus>"),
              twice("<multistatus><response>", "</response></multistatus>")}) {
            test.check(
                openscp::curlparser::parseWebDavPropfindResponse(
                    webDavOptions(), xml, resources, error, limits) ==
                    ListingParseStatus::ResourceLimitExceeded,
                "repeated non-element content must not bypass the budget");
        }
    }
}

OPENSCP_TEST(testWebDavXmlBudgetAppliesToEachRecord, test) {
    std::string xml = "<multistatus>";
    for (int index = 0; index < 100; ++index)
        xml += "<response><href>/dav/file" + std::to_string(index) +
               "</href></response>";
    xml += "</multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    ListingParserLimits limits;
    // Each record takes five nodes, and the whole listing 502.
    limits.maxXmlNodes = 5;

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                       ListingParseStatus::Success &&
                   resources.size() == 100,
               "the XML budget should bound each record, not the listing");
    --limits.maxXmlNodes;
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error, limits) ==
                       ListingParseStatus::ResourceLimitExceeded &&
                   resources.empty(),
               "a record over the XML budget must reject the listing");
}

// Apache mod_dav's layout, which takes about 33 nodes per file and 44 per
// folder.
std::string apacheResponse(std::size_t index, bool folder) {
    std::string response = "<D:response xmlns:lp1=\"DAV:\" "
                           "xmlns:lp2=\"http://apache.org/dav/props/\">\n"
                           "<D:href>/dav/entry" +
                           std::to_string(index) + (folder ? "/" : ".txt") +
                           "</D:href>\n<D:propstat>\n<D:prop>\n";
    if (folder) {
        response += "<lp1:resourcetype><D:collection/></lp1:resourcetype>\n";
    } else {
        response += "<lp1:resourcetype/>\n"
                    "<lp1:getcontentlength>1234</lp1:getcontentlength>\n";
    }
    response += "<lp1:getlastmodified>Mon, 01 Jan 2024 00:00:00 GMT"
                "</lp1:getlastmodified>\n</D:prop>\n"
                "<D:status>HTTP/1.1 200 OK</D:status>\n</D:propstat>\n";
    if (folder) {
        response += "<D:propstat>\n<D:prop>\n<g0:getcontentlength/>\n"
                    "</D:prop>\n<D:status>HTTP/1.1 404 Not Found</D:status>\n"
                    "</D:propstat>\n";
    }
    return response + "</D:response>\n";
}

OPENSCP_TEST(testWebDavParsesListingsOfManyThousandEntries, test) {
    constexpr std::size_t kEntries = 10'000;
    std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                      "<D:multistatus xmlns:D=\"DAV:\" xmlns:g0=\"DAV:\">\n";
    for (std::size_t index = 0; index < kEntries; ++index)
        xml += apacheResponse(index, index % 2 == 0);
    xml += "</D:multistatus>\n";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;

    test.checkWithError(openscp::curlparser::parseWebDavPropfindResponse(
                            webDavOptions(), xml, resources, error) ==
                                ListingParseStatus::Success &&
                            resources.size() == kEntries,
                        "default limits should accept listings far above a few "
                        "thousand entries: ",
                        error);
    test.check(!resources.empty() && resources.front().isDir &&
                   !resources.back().isDir && resources.back().hasSize &&
                   resources.back().size == 1234 && resources.back().hasMtime,
               "large listings should keep each entry's properties");
}

OPENSCP_TEST(testWebDavRejectsTruncatedListings, test) {
    const std::string response = "<response><href>/dav/file</href></response>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;

    for (const std::string &xml :
         {"<multistatus>" + response,
          "<multistatus>" + response + "<response><href>/dav/other</href>"}) {
        resources.push_back({});
        test.check(openscp::curlparser::parseWebDavPropfindResponse(
                       webDavOptions(), xml, resources, error) ==
                           ListingParseStatus::Malformed &&
                       resources.empty() && !error.empty(),
                   "a truncated listing must not pass as a shorter one");
    }

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(),
                   "<multistatus>" + response +
                       "</multistatus><response>"
                       "<href>/dav/after</href></response>",
                   resources, error) == ListingParseStatus::Success &&
                   resources.size() == 1 && resources.front().path == "/file",
               "records after the multistatus element should be ignored, as "
               "the document parser did");
}

OPENSCP_TEST(testWebDavRejectsMismatchedTagsAroundRecords, test) {
    std::vector<openscp::curlparser::WebDavResource> resources{{}};
    std::string error;

    // A damaged start tag must not turn a record into ignored content.
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(),
                   "<multistatus><response><href>/dav/a</href></response>"
                   "<respo><href>/dav/b</href></response></multistatus>",
                   resources, error) == ListingParseStatus::Malformed &&
                   resources.empty() && !error.empty(),
               "a listing with mismatched tags must not lose entries silently");
}

OPENSCP_TEST(testWebDavXmlBudgetRejectsAttributeFloods, test) {
    std::string xml = "<multistatus";
    for (std::size_t index = 0;
         index <= openscp::curlparser::kMaxWebDavXmlAttributesPerElement;
         ++index)
        xml += " a" + std::to_string(index) + "='value'";
    xml += "><response><href>/dav/file</href></response></multistatus>";
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;

    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml, resources, error) ==
                   ListingParseStatus::ResourceLimitExceeded,
               "one element cannot cause unbounded attribute-name scans");
}

OPENSCP_TEST(testWebDavXmlPreflightLeavesValidationToTinyXml, test) {
    std::vector<openscp::curlparser::WebDavResource> resources;
    std::string error;
    for (const std::string xml :
         {"<multistatus><response></multistatus>", "<multistatus a='1' a='2'/>",
          "<multistatus a='unfinished>", "<multistatus><!-- unfinished",
          "<multistatus><![CDATA[text", "<?xml version='1.0'",
          "<!DOCTYPE unfinished"}) {
        resources.push_back({});
        test.check(openscp::curlparser::parseWebDavPropfindResponse(
                       webDavOptions(), xml, resources, error) ==
                       ListingParseStatus::Malformed,
                   "malformed XML must remain rejected without partial output");
        test.check(resources.empty() && !error.empty(),
                   "malformed XML must clear prior output and report an error");
    }
    const std::string xml = "<multistatus><response><href>/dav/file</href>"
                            "</response></multistatus>";
    test.check(openscp::curlparser::parseWebDavPropfindResponse(
                   webDavOptions(), xml + std::string(1, '\0') + "<x/>",
                   resources, error) == ListingParseStatus::Malformed,
               "embedded null bytes must not silently truncate XML parsing");
}
#endif

} // namespace

int main() {
    openscp::test::TestHarness harness("curl listing parser");
    return harness.run();
}
