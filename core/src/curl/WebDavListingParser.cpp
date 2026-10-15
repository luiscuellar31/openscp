#include "CurlBackendCommon.hpp"
#include "CurlListingParser.hpp"
#include "openscp/RemotePath.hpp"

#include <curl/curl.h>
#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace openscp::curlparser {
namespace {

using curlcommon::parseUnsignedDec;
using curlcommon::trimAscii;

// The isspace() characters, which tinyxml2's SkipWhiteSpace skips.
constexpr std::string_view kParserWhitespace = " \t\n\v\f\r";

// Same characters as tinyxml2's XMLUtil::IsNameChar.
bool isXmlNameByte(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x80 || std::isalnum(byte) || character == ':' ||
           character == '_' || character == '.' || character == '-';
}

// The element name at the start of `text`, as tinyxml2's ParseName reads it.
std::string_view leadingXmlName(std::string_view text) {
    std::size_t nameEnd = 0;
    while (nameEnd < text.size() && isXmlNameByte(text[nameEnd]))
        ++nameEnd;
    return text.substr(0, nameEnd);
}

// Uses the local-name rule of xmlNameEquals.
bool isResponseName(std::string_view name) {
    const std::size_t colon = name.find(':');
    if (colon != std::string_view::npos)
        name.remove_prefix(colon + 1);
    return name == "response";
}

// This is a resource preflight, not an XML validator. Keep delimiters aligned
// with tinyxml2's Identify/ParseDeep rules; tinyxml2 still validates each
// record. The cursor only advances, with no input-sized allocation.
//
// Each outermost response element of the first top-level element goes to
// onRecord as soon as it closes, together with its nesting depth, so records
// are parsed one at a time. The node budget therefore bounds each record, and
// separately the content around them, rather than the whole listing. That
// content is only checked here: its elements must nest and close properly.
template <typename RecordHandler>
ListingParseStatus scanXml(std::string_view xml,
                           const ListingParserLimits &limits,
                           RecordHandler &&onRecord) {
    if (xml.size() > limits.maxXmlBytes)
        return ListingParseStatus::ResourceLimitExceeded;
    if (xml.find('\0') != std::string_view::npos)
        return ListingParseStatus::Malformed;

    std::size_t outsideNodes = 0;
    std::size_t recordNodes = 0;
    std::size_t recordStart = std::string_view::npos;
    std::size_t recordDepth = 0;
    bool rootClosed = false;
    bool strayClosingTag = false;
    // Names of the open elements; maxXmlNestingDepth bounds its size.
    std::vector<std::string_view> openElements;
    std::size_t position = 0;
    const auto consumeNode = [&] {
        std::size_t &nodes =
            recordStart == std::string_view::npos ? outsideNodes : recordNodes;
        if (nodes >= limits.maxXmlNodes)
            return false;
        ++nodes;
        return true;
    };
    while (position < xml.size()) {
        if (xml[position] != '<') {
            const std::size_t next = xml.find('<', position);
            const std::size_t textEnd =
                next == std::string_view::npos ? xml.size() : next;
            // Nothing outside the records is parsed, so the whitespace
            // between them, one segment per record, is free.
            const bool blank =
                xml.find_first_not_of(kParserWhitespace, position) >= textEnd;
            if ((recordStart != std::string_view::npos || !blank) &&
                !consumeNode())
                return ListingParseStatus::ResourceLimitExceeded;
            position = textEnd;
            continue;
        }

        const std::string_view remaining = xml.substr(position);
        std::string_view terminator;
        std::size_t prefixLength = 0;
        if (remaining.starts_with("<!--")) {
            terminator = "-->";
            prefixLength = 4;
        } else if (remaining.starts_with("<![CDATA[")) {
            terminator = "]]>";
            prefixLength = 9;
        } else if (remaining.starts_with("<?")) {
            terminator = "?>";
            prefixLength = 2;
        } else if (remaining.starts_with("<!")) {
            terminator = ">";
            prefixLength = 2;
        }
        if (!terminator.empty()) {
            if (!consumeNode())
                return ListingParseStatus::ResourceLimitExceeded;
            const std::size_t end =
                xml.find(terminator, position + prefixLength);
            if (end == std::string_view::npos)
                return ListingParseStatus::Malformed;
            position = end + terminator.size();
            continue;
        }

        std::size_t attributes = 0;
        std::size_t end = position + 1;
        for (; end < xml.size() && xml[end] != '>'; ++end) {
            if (xml[end] == '\'' || xml[end] == '"') {
                end = xml.find(xml[end], end + 1);
                if (end == std::string_view::npos)
                    return ListingParseStatus::Malformed;
            } else if (xml[end] == '=') {
                if (attributes >= limits.maxXmlAttributesPerElement)
                    return ListingParseStatus::ResourceLimitExceeded;
                ++attributes;
            } else if (xml[end] == '<') {
                return ListingParseStatus::Malformed;
            }
        }
        if (end == xml.size())
            return ListingParseStatus::Malformed;

        // Read the tag as tinyxml2 does: whitespace, an optional '/' and the
        // name. A tag that ends in "/>" is a complete element, even after '/'.
        const std::string_view tag =
            xml.substr(position + 1, end - position - 1);
        std::string_view name = tag.substr(
            std::min(tag.find_first_not_of(kParserWhitespace), tag.size()));
        const bool slash = name.starts_with('/');
        if (slash)
            name.remove_prefix(1);
        const std::string_view elementName = leadingXmlName(name);
        const bool selfClosing = tag.ends_with('/');
        const bool closing = slash && !selfClosing;
        if (!closing && !rootClosed && recordStart == std::string_view::npos &&
            isResponseName(elementName)) {
            recordStart = position;
            recordDepth = openElements.size();
            recordNodes = 0;
        }
        // The tag and each of its attributes.
        for (std::size_t node = 0; node <= attributes; ++node) {
            if (!consumeNode())
                return ListingParseStatus::ResourceLimitExceeded;
        }

        if (closing) {
            if (openElements.empty()) {
                strayClosingTag = true;
            } else {
                // After a stray closing tag, tinyxml2 has stopped and checks
                // no further end tags.
                if (!strayClosingTag && openElements.back() != elementName)
                    return ListingParseStatus::Malformed;
                openElements.pop_back();
            }
        } else {
            // A self-closing element still occupies the next nesting level.
            if (openElements.size() >= limits.maxXmlNestingDepth)
                return ListingParseStatus::ResourceLimitExceeded;
            if (!selfClosing)
                openElements.push_back(elementName);
        }
        position = end + 1;
        // Like tinyxml2, ignore records after the first top-level element or
        // after a stray closing tag, where tinyxml2 stops.
        if (openElements.empty())
            rootClosed = true;
        if (recordStart != std::string_view::npos &&
            openElements.size() == recordDepth) {
            const ListingParseStatus status = onRecord(
                xml.substr(recordStart, position - recordStart), recordDepth);
            if (status != ListingParseStatus::Success)
                return status;
            recordStart = std::string_view::npos;
        }
    }
    // An unclosed element means a truncated listing, unless tinyxml2 would
    // have stopped at a stray closing tag before it.
    return openElements.empty() || strayClosingTag
               ? ListingParseStatus::Success
               : ListingParseStatus::Malformed;
}

const char *xmlLocalName(const char *name) {
    if (!name)
        return "";
    const char *colon = std::strchr(name, ':');
    return colon ? colon + 1 : name;
}

bool xmlNameEquals(const tinyxml2::XMLElement *element, const char *local) {
    return element && std::strcmp(xmlLocalName(element->Name()), local) == 0;
}

const tinyxml2::XMLElement *
firstChildByLocal(const tinyxml2::XMLElement *parent, const char *local) {
    if (!parent || !local)
        return nullptr;
    for (const tinyxml2::XMLElement *child = parent->FirstChildElement(); child;
         child = child->NextSiblingElement()) {
        if (xmlNameEquals(child, local))
            return child;
    }
    return nullptr;
}

int parseHttpStatusCode(const std::string &statusLine) {
    for (std::size_t index = 0; index + 2 < statusLine.size(); ++index) {
        const unsigned char first =
            static_cast<unsigned char>(statusLine[index]);
        const unsigned char second =
            static_cast<unsigned char>(statusLine[index + 1]);
        const unsigned char third =
            static_cast<unsigned char>(statusLine[index + 2]);
        if (std::isdigit(first) && std::isdigit(second) &&
            std::isdigit(third)) {
            return int((first - '0') * 100 + (second - '0') * 10 +
                       (third - '0'));
        }
    }
    return 0;
}

std::string extractPathFromHref(std::string href) {
    href = trimAscii(std::move(href));
    if (href.empty())
        return "/";
    const std::size_t hashPosition = href.find('#');
    if (hashPosition != std::string::npos)
        href.erase(hashPosition);
    const std::size_t queryPosition = href.find('?');
    if (queryPosition != std::string::npos)
        href.erase(queryPosition);
    const std::size_t schemePosition = href.find("://");
    if (schemePosition != std::string::npos) {
        const std::size_t pathPosition = href.find('/', schemePosition + 3);
        if (pathPosition == std::string::npos)
            return "/";
        return href.substr(pathPosition);
    }
    return href;
}

std::string decodePercent(const std::string &raw) {
    int decodedLength = 0;
    char *decoded = curl_easy_unescape(
        nullptr, raw.c_str(), static_cast<int>(raw.size()), &decodedLength);
    if (!decoded)
        return raw;
    std::string output(decoded, static_cast<std::size_t>(decodedLength));
    curl_free(decoded);
    return output;
}

void parsePropElement(const tinyxml2::XMLElement *property,
                      WebDavResource &out) {
    if (!property)
        return;
    if (const tinyxml2::XMLElement *resourceType =
            firstChildByLocal(property, "resourcetype")) {
        for (const tinyxml2::XMLElement *child =
                 resourceType->FirstChildElement();
             child; child = child->NextSiblingElement()) {
            if (xmlNameEquals(child, "collection")) {
                out.isDir = true;
                break;
            }
        }
    }
    if (const tinyxml2::XMLElement *length =
            firstChildByLocal(property, "getcontentlength")) {
        if (const char *text = length->GetText()) {
            std::uint64_t value = 0;
            if (parseUnsignedDec(trimAscii(text), value)) {
                out.hasSize = true;
                out.size = value;
            }
        }
    }
    if (const tinyxml2::XMLElement *modified =
            firstChildByLocal(property, "getlastmodified")) {
        if (const char *text = modified->GetText()) {
            const std::time_t timestamp = curl_getdate(text, nullptr);
            if (timestamp >= 0) {
                out.hasMtime = true;
                out.mtime = static_cast<std::uint64_t>(timestamp);
            }
        }
    }
}

bool parseResponseElement(const tinyxml2::XMLElement *element,
                          const SessionOptions &options,
                          WebDavResource &parsed) {
    const tinyxml2::XMLElement *hrefElement =
        firstChildByLocal(element, "href");
    const char *hrefText = hrefElement ? hrefElement->GetText() : nullptr;
    if (!hrefText || !*hrefText)
        return false;

    const std::string rawHref(hrefText);
    const std::string serverPath =
        normalizeRemotePath(decodePercent(extractPathFromHref(rawHref)));
    if (!curlcommon::webDavLogicalPath(options.webdav_base_path, serverPath,
                                       parsed.path)) {
        return false;
    }
    if (rawHref.back() == '/')
        parsed.isDir = true;

    bool consumedPropertyStatus = false;
    for (const tinyxml2::XMLElement *propertyStatus =
             element->FirstChildElement();
         propertyStatus;
         propertyStatus = propertyStatus->NextSiblingElement()) {
        if (!xmlNameEquals(propertyStatus, "propstat"))
            continue;
        const tinyxml2::XMLElement *status =
            firstChildByLocal(propertyStatus, "status");
        const char *statusText = status ? status->GetText() : nullptr;
        const int statusCode = statusText ? parseHttpStatusCode(statusText) : 0;
        if (statusCode < 200 || statusCode >= 300)
            continue;
        const tinyxml2::XMLElement *property =
            firstChildByLocal(propertyStatus, "prop");
        parsePropElement(property, parsed);
        consumedPropertyStatus = true;
    }

    if (!consumedPropertyStatus) {
        const tinyxml2::XMLElement *property =
            firstChildByLocal(element, "prop");
        parsePropElement(property, parsed);
    }
    return true;
}

void mergeResource(WebDavResource parsed,
                   std::vector<WebDavResource> &resources,
                   std::unordered_map<std::string, std::size_t> &index) {
    const auto [position, inserted] =
        index.emplace(parsed.path, resources.size());
    if (inserted) {
        resources.push_back(std::move(parsed));
        return;
    }

    WebDavResource &existing = resources[position->second];
    existing.isDir = existing.isDir || parsed.isDir;
    if (!existing.hasSize && parsed.hasSize) {
        existing.hasSize = true;
        existing.size = parsed.size;
    }
    if (!existing.hasMtime && parsed.hasMtime) {
        existing.hasMtime = true;
        existing.mtime = parsed.mtime;
    }
}

} // namespace

ListingParseStatus parseWebDavPropfindResponse(
    const SessionOptions &options, const std::string &xml,
    std::vector<WebDavResource> &resources, std::string &error,
    const ListingParserLimits &limits) {
    resources.clear();
    error.clear();

    RemoteListingBudget budget(limits.maxEntries, limits.maxNameBytes);
    std::unordered_map<std::string, std::size_t> resourceIndex;
    const auto parseRecord = [&](std::string_view record,
                                 std::size_t recordDepth) {
        tinyxml2::XMLDocument document;
        const tinyxml2::XMLError parseError =
            document.Parse(record.data(), record.size());
        if (parseError != tinyxml2::XML_SUCCESS) {
            std::ostringstream message;
            message << "Could not parse WebDAV PROPFIND response (XML error "
                    << static_cast<int>(parseError) << ").";
            error = message.str();
            return ListingParseStatus::Malformed;
        }
        const tinyxml2::XMLElement *root = document.RootElement();
        if (!root) {
            error = "WebDAV PROPFIND response is empty.";
            return ListingParseStatus::Malformed;
        }

        struct PendingElement {
            const tinyxml2::XMLElement *element = nullptr;
            std::size_t depth = 0;
        };
        std::vector<PendingElement> stack{{root, recordDepth + 1}};
        while (!stack.empty()) {
            const PendingElement pending = stack.back();
            stack.pop_back();
            const tinyxml2::XMLElement *element = pending.element;

            if (pending.depth > limits.maxXmlNestingDepth) {
                error = "WebDAV PROPFIND response exceeded the XML nesting "
                        "safety limit.";
                return ListingParseStatus::ResourceLimitExceeded;
            }

            for (const tinyxml2::XMLElement *child =
                     element->FirstChildElement();
                 child; child = child->NextSiblingElement()) {
                stack.push_back({child, pending.depth + 1});
            }

            if (!xmlNameEquals(element, "response"))
                continue;

            WebDavResource parsed;
            const bool usable = parseResponseElement(element, options, parsed);
            // Count every DAV response record, including unusable and
            // duplicate records, so a hostile server cannot bypass the work
            // budget by omitting href or returning paths outside the
            // configured base.
            if (!budget.tryConsume(usable ? parsed.path.size() : 0)) {
                error = "WebDAV PROPFIND response exceeded the directory "
                        "listing safety limit.";
                return ListingParseStatus::ResourceLimitExceeded;
            }
            if (!usable)
                continue;
            mergeResource(std::move(parsed), resources, resourceIndex);
        }
        return ListingParseStatus::Success;
    };

    const ListingParseStatus status = scanXml(xml, limits, parseRecord);
    if (status != ListingParseStatus::Success) {
        resources.clear();
        if (error.empty()) {
            error = status == ListingParseStatus::ResourceLimitExceeded
                        ? "WebDAV PROPFIND response exceeded the XML parsing "
                          "safety limit."
                        : "Could not parse WebDAV PROPFIND response (malformed "
                          "XML).";
        }
        return status;
    }
    if (resources.empty()) {
        error = "WebDAV PROPFIND response does not contain usable resources.";
        return ListingParseStatus::Malformed;
    }
    return ListingParseStatus::Success;
}

} // namespace openscp::curlparser
