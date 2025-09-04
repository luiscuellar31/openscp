// Utilidad mínima de registro (solo encabezado) para el núcleo de OpenSCP.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

namespace openscp {

inline bool logEnabled() {
    const char* v = std::getenv("OPEN_SCP_LOG");
    return v && *v && *v != '0';
}

inline void logf(const char* level, const char* fmt, ...) {
    if (!logEnabled()) return;
    std::fprintf(stderr, "[OpenSCP][%s] ", level);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
}

} // namespace openscp

#define LOGI(fmt, ...) \
    do { \
        if (openscp::logEnabled()) \
            openscp::logf("INFO", fmt, ##__VA_ARGS__); \
    } while (0)

#define LOGE(fmt, ...) \
    do { \
        if (openscp::logEnabled()) \
            openscp::logf("ERROR", fmt, ##__VA_ARGS__); \
    } while (0)
	