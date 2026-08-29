#include "openscp/RemotePath.hpp"

#include <vector>

namespace openscp {

std::string normalizeRemotePath(std::string_view rawPath) {
    std::vector<std::string_view> segments;
    std::size_t segmentStart = 0;

    const auto consumeSegment = [&](std::size_t segmentEnd) {
        const std::string_view segment =
            rawPath.substr(segmentStart, segmentEnd - segmentStart);
        if (segment.empty() || segment == ".")
            return;
        if (segment == "..") {
            if (!segments.empty())
                segments.pop_back();
            return;
        }
        segments.push_back(segment);
    };

    for (std::size_t index = 0; index < rawPath.size(); ++index) {
        if (rawPath[index] != '/' && rawPath[index] != '\\')
            continue;
        consumeSegment(index);
        segmentStart = index + 1;
    }
    consumeSegment(rawPath.size());

    std::size_t outputSize = 1;
    for (const std::string_view segment : segments)
        outputSize += segment.size() + 1;
    if (!segments.empty())
        --outputSize;

    std::string normalized;
    normalized.reserve(outputSize);
    normalized.push_back('/');
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index != 0)
            normalized.push_back('/');
        normalized.append(segments[index]);
    }
    return normalized;
}

} // namespace openscp
