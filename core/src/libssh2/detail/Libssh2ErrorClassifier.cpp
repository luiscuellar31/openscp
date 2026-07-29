#include "Libssh2ErrorClassifier.hpp"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>

namespace openscp::libssh2detail {
namespace {

bool isUncertain(RemoteErrorKind kind) {
    return kind == RemoteErrorKind::Connection ||
           kind == RemoteErrorKind::Timeout ||
           kind == RemoteErrorKind::RemoteIo;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool containsAny(const std::string &value,
                 std::initializer_list<const char *> needles) {
    return std::any_of(needles.begin(), needles.end(),
                       [&value](const char *needle) {
                           return value.find(needle) != std::string::npos;
                       });
}

std::optional<RemoteError> classifyMessage(const std::string &message,
                                           const std::string &lower) {
    RemoteError error;
    error.message = message;
    if (containsAny(lower, {"cancel", "interrupt"})) {
        error.kind = RemoteErrorKind::Canceled;
    } else if (containsAny(lower, {"no space", "disk full", "quota exceeded",
                                   "enospc"})) {
        error.kind = RemoteErrorKind::InsufficientSpace;
        error.native_code = ENOSPC;
    } else if (containsAny(lower, {"host key", "hostkey", "known_hosts",
                                   "fingerprint", "unknown host"})) {
        error.kind = RemoteErrorKind::Certificate;
    } else if (containsAny(lower,
                           {"authentication", "password", "publickey",
                            "private key", "sin credenciales", "credential"})) {
        error.kind = RemoteErrorKind::Authentication;
    } else if (containsAny(lower, {"permission denied", "write protect",
                                   "access denied"})) {
        error.kind = RemoteErrorKind::PermissionDenied;
    } else if (containsAny(
                   lower, {"integrity", "checksum", "does not match remote"})) {
        error.kind = RemoteErrorKind::Integrity;
    } else if (containsAny(lower, {"does not support", "not supported"})) {
        error.kind = RemoteErrorKind::Unsupported;
    } else if (containsAny(lower,
                           {"invalid ", "missing session options", " is empty",
                            "already connected", "cannot be used together"})) {
        error.kind = RemoteErrorKind::InvalidRequest;
    } else if (containsAny(lower,
                           {"local file", "local .part", "local read",
                            "local write", "local seek", "sync local",
                            "finalize local", "finalize atomic download"})) {
        error.kind = errno == ENOSPC ? RemoteErrorKind::InsufficientSpace
                                     : RemoteErrorKind::LocalIo;
        error.native_code = errno;
    } else if (lower.find("not connected") != std::string::npos) {
        error.kind = RemoteErrorKind::Connection;
        error.transient = true;
    } else {
        return std::nullopt;
    }
    return error;
}

std::optional<RemoteError> classifySftpCode(const std::string &message,
                                            unsigned long code, bool mutation) {
    RemoteError error;
    error.message = message;
    error.native_code = static_cast<std::int64_t>(code);
    switch (code) {
    case LIBSSH2_FX_NO_SUCH_FILE:
    case LIBSSH2_FX_NO_SUCH_PATH:
        error.kind = RemoteErrorKind::NotFound;
        break;
    case LIBSSH2_FX_PERMISSION_DENIED:
    case LIBSSH2_FX_WRITE_PROTECT:
    case LIBSSH2_FX_UNKNOWN_PRINCIPAL:
        error.kind = RemoteErrorKind::PermissionDenied;
        break;
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
    case LIBSSH2_FX_QUOTA_EXCEEDED:
        error.kind = RemoteErrorKind::InsufficientSpace;
        break;
    case LIBSSH2_FX_NO_CONNECTION:
    case LIBSSH2_FX_CONNECTION_LOST:
        error.kind = RemoteErrorKind::Connection;
        error.transient = true;
        break;
    case LIBSSH2_FX_OP_UNSUPPORTED:
        error.kind = RemoteErrorKind::Unsupported;
        break;
    case LIBSSH2_FX_FILE_ALREADY_EXISTS:
    case LIBSSH2_FX_LOCK_CONFLICT:
    case LIBSSH2_FX_DIR_NOT_EMPTY:
        error.kind = RemoteErrorKind::Conflict;
        break;
    case LIBSSH2_FX_BAD_MESSAGE:
        error.kind = RemoteErrorKind::Protocol;
        break;
    case LIBSSH2_FX_FAILURE:
    case LIBSSH2_FX_NO_MEDIA:
        error.kind = RemoteErrorKind::RemoteIo;
        break;
    case LIBSSH2_FX_INVALID_HANDLE:
    case LIBSSH2_FX_NOT_A_DIRECTORY:
    case LIBSSH2_FX_INVALID_FILENAME:
    case LIBSSH2_FX_LINK_LOOP:
        error.kind = RemoteErrorKind::InvalidRequest;
        break;
    default:
        return std::nullopt;
    }
    error.commit_uncertain = mutation && isUncertain(error.kind);
    return error;
}

} // namespace

RemoteError classifyFailure(const std::string &message,
                            _LIBSSH2_SESSION *session, _LIBSSH2_SFTP *sftp,
                            bool mutation, const RemoteError *fallback) {
    const std::string lower = lowercase(message);
    if (auto messageError = classifyMessage(message, lower))
        return *messageError;

    const unsigned long sftpCode =
        sftp ? libssh2_sftp_last_error(sftp) : LIBSSH2_FX_OK;
    if (auto sftpError = classifySftpCode(message, sftpCode, mutation))
        return *sftpError;

    if (fallback != nullptr && fallback->kind != RemoteErrorKind::None)
        return *fallback;

    RemoteError error;
    error.message = message;
    const int sessionCode =
        session ? libssh2_session_last_errno(session) : LIBSSH2_ERROR_NONE;
    error.native_code = sessionCode != LIBSSH2_ERROR_NONE
                            ? sessionCode
                            : static_cast<std::int64_t>(sftpCode);
    switch (sessionCode) {
    case LIBSSH2_ERROR_AUTHENTICATION_FAILED:
    case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED:
    case LIBSSH2_ERROR_KEYFILE_AUTH_FAILED:
    case LIBSSH2_ERROR_PASSWORD_EXPIRED:
    case LIBSSH2_ERROR_PUBLICKEY_PROTOCOL:
        error.kind = RemoteErrorKind::Authentication;
        break;
    case LIBSSH2_ERROR_KNOWN_HOSTS:
    case LIBSSH2_ERROR_HOSTKEY_INIT:
    case LIBSSH2_ERROR_HOSTKEY_SIGN:
        error.kind = RemoteErrorKind::Certificate;
        break;
    case LIBSSH2_ERROR_TIMEOUT:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
    case LIBSSH2_ERROR_EAGAIN:
        error.kind = RemoteErrorKind::Timeout;
        error.transient = true;
        break;
    case LIBSSH2_ERROR_SOCKET_NONE:
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_BAD_SOCKET:
    case LIBSSH2_ERROR_CHANNEL_CLOSED:
        error.kind = RemoteErrorKind::Connection;
        error.transient = true;
        break;
    case LIBSSH2_ERROR_METHOD_NOT_SUPPORTED:
    case LIBSSH2_ERROR_ALGO_UNSUPPORTED:
    case LIBSSH2_ERROR_KEX_FAILURE:
    case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE:
        error.kind = RemoteErrorKind::Unsupported;
        break;
    case LIBSSH2_ERROR_REQUEST_DENIED:
    case LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED:
        error.kind = RemoteErrorKind::PermissionDenied;
        break;
    case LIBSSH2_ERROR_PROTO:
    case LIBSSH2_ERROR_SCP_PROTOCOL:
    case LIBSSH2_ERROR_SFTP_PROTOCOL:
        error.kind = RemoteErrorKind::Protocol;
        break;
    case LIBSSH2_ERROR_INVAL:
    case LIBSSH2_ERROR_BAD_USE:
        error.kind = RemoteErrorKind::InvalidRequest;
        break;
    case LIBSSH2_ERROR_ALLOC:
        error.kind = RemoteErrorKind::LocalIo;
        break;
    case LIBSSH2_ERROR_FILE:
        error.kind = errno == ENOSPC ? RemoteErrorKind::InsufficientSpace
                                     : RemoteErrorKind::LocalIo;
        if (errno != 0)
            error.native_code = errno;
        break;
    case LIBSSH2_ERROR_NONE:
    default:
        if (containsAny(lower, {"not found", "no such"})) {
            error.kind = RemoteErrorKind::NotFound;
        } else if (containsAny(lower, {"timeout", "timed out"})) {
            error.kind = RemoteErrorKind::Timeout;
            error.transient = true;
        } else if (containsAny(lower,
                               {"connect", "connection", "socket", "closed"})) {
            error.kind = RemoteErrorKind::Connection;
            error.transient = true;
        } else if (containsAny(lower, {"protocol", "handshake"})) {
            error.kind = RemoteErrorKind::Protocol;
        } else {
            error.kind = RemoteErrorKind::RemoteIo;
        }
        break;
    }
    error.commit_uncertain =
        (mutation || lower.find("finalize upload") != std::string::npos) &&
        isUncertain(error.kind);
    return error;
}

} // namespace openscp::libssh2detail
