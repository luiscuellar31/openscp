#pragma once

#include "openscp/RemoteError.hpp"

#include <string>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;

namespace openscp::libssh2detail {

RemoteError classifyFailure(const std::string &message,
                            _LIBSSH2_SESSION *session,
                            _LIBSSH2_SFTP *sftp = nullptr,
                            bool mutation = false,
                            const RemoteError *fallback = nullptr);

} // namespace openscp::libssh2detail
