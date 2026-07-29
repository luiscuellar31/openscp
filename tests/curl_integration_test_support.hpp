#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openscp::testsupport {

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
        previous_.emplace_back(
            name, old ? std::optional<std::string>(old) : std::nullopt);
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

    std::vector<
        std::pair<std::string, std::optional<std::string>>>
        previous_;
};

inline void forceUnreachableEnvironmentProxies(ScopedEnvironment &environment) {
    static constexpr const char *proxyVariables[] = {
        "ALL_PROXY",  "all_proxy",  "FTP_PROXY", "ftp_proxy",
        "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
    };
    for (const char *name : proxyVariables)
        environment.set(name, "http://127.0.0.1:1");
    environment.set("NO_PROXY", "");
    environment.set("no_proxy", "");
}

} // namespace openscp::testsupport
