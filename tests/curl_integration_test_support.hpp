#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openscp::testsupport {

inline std::optional<std::string> envValue(const char *key) {
    const char *raw = std::getenv(key);
    if (!raw || !*raw)
        return std::nullopt;
    return std::string(raw);
}

inline bool parsePort(const std::optional<std::string> &raw, std::uint16_t &out,
                      std::uint16_t fallback) {
    if (!raw.has_value()) {
        out = fallback;
        return true;
    }
    try {
        const int parsed = std::stoi(*raw);
        if (parsed < 1 || parsed > 65535)
            return false;
        out = static_cast<std::uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool parseBool(const std::optional<std::string> &raw, bool fallback) {
    if (!raw.has_value())
        return fallback;
    const std::string &value = *raw;
    if (value == "1" || value == "true" || value == "TRUE" || value == "yes" ||
        value == "YES") {
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" || value == "no" ||
        value == "NO") {
        return false;
    }
    return fallback;
}

inline std::string uniqueToken() {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<long long>(now));
}

inline std::string joinRemotePath(const std::string &base,
                                  const std::string &name) {
    if (base.empty())
        return std::string("/") + name;
    if (base.back() == '/')
        return base + name;
    return base + "/" + name;
}

inline bool writeFile(const std::filesystem::path &path,
                      const std::string &content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
        return false;
    output << content;
    return output.good();
}

inline bool readFile(const std::filesystem::path &path, std::string &output) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
        return false;
    output.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return true;
}

class ScopedEnvironment {
    public:
    ScopedEnvironment() = default;
    ~ScopedEnvironment() {
        for (auto it = previous_.rbegin(); it != previous_.rend(); ++it)
            restore(it->first, it->second);
    }

    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

    void set(const std::string &name, const std::string &value) {
        const char *old = std::getenv(name.c_str());
        previous_.emplace_back(name, old ? std::optional<std::string>(old)
                                         : std::nullopt);
#ifdef _WIN32
        (void)_putenv_s(name.c_str(), value.c_str());
#else
        (void)::setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    private:
    static void restore(const std::string &name,
                        const std::optional<std::string> &value) {
#ifdef _WIN32
        (void)_putenv_s(name.c_str(), value ? value->c_str() : "");
#else
        if (value)
            (void)::setenv(name.c_str(), value->c_str(), 1);
        else
            (void)::unsetenv(name.c_str());
#endif
    }

    std::vector<std::pair<std::string, std::optional<std::string>>> previous_;
};

inline void forceUnreachableEnvironmentProxies(ScopedEnvironment &environment) {
    static constexpr const char *proxyVariables[] = {
        "ALL_PROXY",  "all_proxy",  "FTP_PROXY",   "ftp_proxy",
        "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
    };
    for (const char *name : proxyVariables)
        environment.set(name, "http://127.0.0.1:1");
    environment.set("NO_PROXY", "");
    environment.set("no_proxy", "");
}

} // namespace openscp::testsupport
