#include "CurlBackendCommon.hpp"
#include "CurlListingParser.hpp"
#include "openscp/RemotePath.hpp"

#include <curl/curl.h>
#include <tinyxml2.h>

#include <cctype>
#include <cstring>
#include <ctime>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace openscp::curlparser {
namespace {

using curlcommon::parseUnsignedDec;
using curlcommon::trimAscii;

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
    tinyxml2::XMLDocument document;
    const tinyxml2::XMLError parseError =
        document.Parse(xml.c_str(), xml.size());
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
    std::vector<PendingElement> stack{{root, 1}};
    RemoteListingBudget budget(limits.maxEntries, limits.maxNameBytes);
    std::unordered_map<std::string, std::size_t> resourceIndex;
    while (!stack.empty()) {
        const PendingElement pending = stack.back();
        stack.pop_back();
        const tinyxml2::XMLElement *element = pending.element;

        if (pending.depth > limits.maxXmlNestingDepth) {
            resources.clear();
            error = "WebDAV PROPFIND response exceeded the XML nesting "
                    "safety limit.";
            return ListingParseStatus::ResourceLimitExceeded;
        }

        for (const tinyxml2::XMLElement *child = element->FirstChildElement();
             child; child = child->NextSiblingElement()) {
            stack.push_back({child, pending.depth + 1});
        }

        if (!xmlNameEquals(element, "response"))
            continue;

        WebDavResource parsed;
        const bool usable = parseResponseElement(element, options, parsed);
        // Count every DAV response record, including unusable and duplicate
        // records, so a hostile server cannot bypass the work budget by
        // omitting href or returning paths outside the configured base.
        if (!budget.tryConsume(usable ? parsed.path.size() : 0)) {
            resources.clear();
            error = "WebDAV PROPFIND response exceeded the directory listing "
                    "safety limit.";
            return ListingParseStatus::ResourceLimitExceeded;
        }
        if (!usable)
            continue;
        mergeResource(std::move(parsed), resources, resourceIndex);
    }

    if (resources.empty()) {
        error = "WebDAV PROPFIND response does not contain usable resources.";
        return ListingParseStatus::Malformed;
    }
    return ListingParseStatus::Success;
}

} // namespace openscp::curlparser
