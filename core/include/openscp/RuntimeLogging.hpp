// Runtime policy helpers for diagnostics/sensitive logging.
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>

namespace openscp {

inline std::string normalizedEnv(const char *name) {
    if (!name)
        return {};
    const char *raw = std::getenv(name);
    if (!raw || !*raw)
        return {};
    std::string out(raw);
    std::size_t start = 0;
    while (start < out.size() &&
           std::isspace(static_cast<unsigned char>(out[start]))) {
        ++start;
    }
    std::size_t end = out.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(out[end - 1]))) {
        --end;
    }
    out = out.substr(start, end - start);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool envFlagEnabled(const char *name) {
    const std::string v = normalizedEnv(name);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

inline bool isDevEnvironment() {
    const std::string env = normalizedEnv("OPEN_SCP_ENV");
    return env == "dev" || env == "development" || env == "local" ||
           env == "debug";
}

inline bool sensitiveLoggingEnabled() {
    return isDevEnvironment() && envFlagEnabled("OPEN_SCP_LOG_SENSITIVE");
}

} // namespace openscp
