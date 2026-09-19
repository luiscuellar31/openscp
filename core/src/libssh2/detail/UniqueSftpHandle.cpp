#include "UniqueSftpHandle.hpp"

#include <libssh2.h>
#include <libssh2_sftp.h>

namespace openscp::libssh2detail {

int defaultCloseSftpHandle(LIBSSH2_SFTP_HANDLE *handle) noexcept {
    return handle ? libssh2_sftp_close_handle(handle) : 0;
}

} // namespace openscp::libssh2detail
