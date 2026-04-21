// Protect transfer workers from indefinite kernel-level socket blocking when
// the peer/network disappears without a clean TCP close.
static void apply_transfer_socket_timeouts(int sock) {
    if (sock < 0)
        return;
#ifdef _WIN32
    DWORD tvMs = 15000;
    (void)::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&tvMs), sizeof(tvMs));
    (void)::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char *>(&tvMs), sizeof(tvMs));
#else
    struct timeval tv{};
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    (void)::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static bool seek_local_file(FILE *f, std::uint64_t off, std::string *why) {
#ifdef _WIN32
    if (_fseeki64(f, (__int64)off, SEEK_SET) != 0) {
        if (why)
            *why = "local seek failed";
        return false;
    }
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) {
        if (why)
            *why = posix_err("fseeko(local)");
        return false;
    }
#endif
    return true;
}

static bool get_local_file_size(const std::string &path, std::uint64_t &out,
                                std::string *why) {
#ifndef _WIN32
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        if (why)
            *why = posix_err("stat(local)");
        return false;
    }
    out = (std::uint64_t)st.st_size;
    return true;
#else
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (why)
            *why = win_err("CreateFile(local-size)", GetLastError());
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        if (why)
            *why = win_err("GetFileSizeEx(local)", GetLastError());
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    out = (std::uint64_t)sz.QuadPart;
    return true;
#endif
}

static bool flush_local_file(FILE *f, std::string *why) {
    if (std::fflush(f) != 0) {
        if (why)
            *why = "fflush(local) failed";
        return false;
    }
#ifdef _WIN32
    const int fd = _fileno(f);
    if (fd >= 0 && _commit(fd) != 0) {
        if (why)
            *why = "commit(local) failed";
        return false;
    }
#else
    const int fd = fileno(f);
    if (fd >= 0 && ::fsync(fd) != 0) {
        if (why)
            *why = posix_err("fsync(local)");
        return false;
    }
#endif
    return true;
}

static bool replace_local_file_atomic(const std::string &from,
                                      const std::string &to, std::string *why) {
#ifndef _WIN32
    if (::rename(from.c_str(), to.c_str()) != 0) {
        if (why)
            *why = posix_err("rename(.part->dest)");
        return false;
    }
    if (!fsync_parent_dir(to, why))
        return false;
    return true;
#else
    if (!MoveFileExA(from.c_str(), to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        if (why)
            *why = win_err("MoveFileEx(.part->dest)", GetLastError());
        return false;
    }
    return true;
#endif
}

static bool rename_remote_with_fallback(LIBSSH2_SFTP *sftp,
                                        const std::string &from,
                                        const std::string &to, bool overwrite,
                                        std::string *why) {
    if (!sftp) {
        if (why)
            *why = "SFTP handle is null";
        return false;
    }

    struct Attempt {
        long flags;
        const char *name;
    };
    const std::vector<Attempt> attempts = overwrite
                                              ? std::vector<Attempt>{
                                                    {LIBSSH2_SFTP_RENAME_ATOMIC |
                                                         LIBSSH2_SFTP_RENAME_NATIVE |
                                                         LIBSSH2_SFTP_RENAME_OVERWRITE,
                                                     "atomic+native+overwrite"},
                                                    {LIBSSH2_SFTP_RENAME_ATOMIC |
                                                         LIBSSH2_SFTP_RENAME_OVERWRITE,
                                                     "atomic+overwrite"},
                                                    {LIBSSH2_SFTP_RENAME_NATIVE |
                                                         LIBSSH2_SFTP_RENAME_OVERWRITE,
                                                     "native+overwrite"},
                                                    {LIBSSH2_SFTP_RENAME_OVERWRITE,
                                                     "overwrite"},
                                                    {0, "plain"}}
                                              : std::vector<Attempt>{
                                                    {LIBSSH2_SFTP_RENAME_ATOMIC |
                                                         LIBSSH2_SFTP_RENAME_NATIVE,
                                                     "atomic+native"},
                                                    {LIBSSH2_SFTP_RENAME_ATOMIC,
                                                     "atomic"},
                                                    {LIBSSH2_SFTP_RENAME_NATIVE,
                                                     "native"},
                                                    {0, "plain"}};

    int lastRc = 0;
    unsigned long lastSftpErr = 0;
    const char *lastAttempt = "none";
    for (const auto &a : attempts) {
        const int rc = libssh2_sftp_rename_ex(
            sftp, from.c_str(), (unsigned)from.size(), to.c_str(),
            (unsigned)to.size(), a.flags);
        if (rc == 0)
            return true;
        lastRc = rc;
        lastSftpErr = libssh2_sftp_last_error(sftp);
        lastAttempt = a.name;
    }

    // Some servers reject RENAME flags but accept plain rename after removing
    // destination first.
    if (overwrite) {
        int urc = libssh2_sftp_unlink_ex(sftp, to.c_str(), (unsigned)to.size());
        unsigned long unlinkErr = libssh2_sftp_last_error(sftp);
        if (urc == 0 || unlinkErr == LIBSSH2_FX_NO_SUCH_FILE) {
            const int rc = libssh2_sftp_rename_ex(
                sftp, from.c_str(), (unsigned)from.size(), to.c_str(),
                (unsigned)to.size(), 0);
            if (rc == 0)
                return true;
            lastRc = rc;
            lastSftpErr = libssh2_sftp_last_error(sftp);
            lastAttempt = "plain-after-unlink";
        }
    }

    if (why) {
        std::ostringstream oss;
        oss << "sftp_rename_ex failed after fallback (attempt=" << lastAttempt
            << ", rc=" << lastRc << ", sftp_err=" << lastSftpErr << ")";
        if (lastSftpErr == LIBSSH2_FX_OP_UNSUPPORTED)
            oss << " [server does not support requested rename mode]";
        else if (lastSftpErr == LIBSSH2_FX_PERMISSION_DENIED)
            oss << " [permission denied]";
        else if (lastSftpErr == LIBSSH2_FX_FAILURE)
            oss << " [generic failure]";
        *why = oss.str();
    }
    return false;
}

static bool transfer_cancel_requested(
    const std::function<bool()> *shouldCancel) {
    return shouldCancel && *shouldCancel && (*shouldCancel)();
}

static bool ensure_sftp_ready(bool connected, LIBSSH2_SFTP *sftp,
                              std::string &err) {
    if (connected && sftp)
        return true;
    err = "Not connected";
    return false;
}

static bool fail_if_transfer_canceled(
    const std::function<bool()> *shouldCancel, std::string &err) {
    if (!transfer_cancel_requested(shouldCancel))
        return false;
    err = "Canceled by user";
    return true;
}

static bool hash_local_range(const std::string &path, std::uint64_t offset,
                             std::uint64_t length, Sha256Digest &out,
                             std::string *why,
                             const std::function<bool()> *shouldCancel =
                                 nullptr) {
    FILE *f = ::fopen(path.c_str(), "rb");
    if (!f) {
        if (why)
            *why = "Could not open local file for hashing";
        return false;
    }
    if (!seek_local_file(f, offset, why)) {
        std::fclose(f);
        return false;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        if (why)
            *why = "Could not initialize local hash context";
        std::fclose(f);
        return false;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (why)
            *why = "EVP_DigestInit_ex(local) failed";
        EVP_MD_CTX_free(ctx);
        std::fclose(f);
        return false;
    }
    std::array<unsigned char, 64 * 1024> buf{};
    std::uint64_t remain = length;
    while (remain > 0) {
        if (transfer_cancel_requested(shouldCancel)) {
            if (why)
                *why = "Canceled by user";
            EVP_MD_CTX_free(ctx);
            std::fclose(f);
            return false;
        }
        const std::size_t want =
            (std::size_t)std::min<std::uint64_t>(remain, buf.size());
        const std::size_t n = std::fread(buf.data(), 1, want, f);
        if (n == 0) {
            if (why)
                *why = "Insufficient local read while hashing";
            EVP_MD_CTX_free(ctx);
            std::fclose(f);
            return false;
        }
        if (EVP_DigestUpdate(ctx, buf.data(), n) != 1) {
            if (why)
                *why = "EVP_DigestUpdate(local) failed";
            EVP_MD_CTX_free(ctx);
            std::fclose(f);
            return false;
        }
        remain -= (std::uint64_t)n;
    }
    unsigned int outLen = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &outLen) != 1 ||
        outLen != SHA256_DIGEST_LENGTH) {
        if (why)
            *why = "EVP_DigestFinal_ex(local) failed";
        EVP_MD_CTX_free(ctx);
        std::fclose(f);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    std::fclose(f);
    return true;
}

static bool hash_local_full(const std::string &path, Sha256Digest &out,
                            std::string *why,
                            const std::function<bool()> *shouldCancel =
                                nullptr) {
    std::uint64_t sz = 0;
    if (!get_local_file_size(path, sz, why))
        return false;
    return hash_local_range(path, 0, sz, out, why, shouldCancel);
}

static bool hash_remote_range(LIBSSH2_SFTP *sftp, const std::string &remote,
                              std::uint64_t offset, std::uint64_t length,
                              Sha256Digest &out, std::string *why,
                              const std::function<bool()> *shouldCancel =
                                  nullptr) {
    LIBSSH2_SFTP_HANDLE *rh =
        libssh2_sftp_open_ex(sftp, remote.c_str(), (unsigned)remote.size(),
                             LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!rh) {
        if (why)
            *why = "Could not open remote file for hashing";
        return false;
    }
    libssh2_sftp_seek64(rh, (libssh2_uint64_t)offset);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        if (why)
            *why = "Could not initialize remote hash context";
        libssh2_sftp_close(rh);
        return false;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (why)
            *why = "EVP_DigestInit_ex(remote) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    std::array<unsigned char, 64 * 1024> buf{};
    std::uint64_t remain = length;
    while (remain > 0) {
        if (transfer_cancel_requested(shouldCancel)) {
            if (why)
                *why = "Canceled by user";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        const std::size_t want =
            (std::size_t)std::min<std::uint64_t>(remain, buf.size());
        ssize_t n = libssh2_sftp_read(rh, reinterpret_cast<char *>(buf.data()),
                                      want);
        if (n <= 0) {
            if (why)
                *why = "Insufficient remote read while hashing";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        if (EVP_DigestUpdate(ctx, buf.data(), (std::size_t)n) != 1) {
            if (why)
                *why = "EVP_DigestUpdate(remote) failed";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        remain -= (std::uint64_t)n;
    }
    unsigned int outLen = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &outLen) != 1 ||
        outLen != SHA256_DIGEST_LENGTH) {
        if (why)
            *why = "EVP_DigestFinal_ex(remote) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    libssh2_sftp_close(rh);
    return true;
}

static bool hash_remote_full(LIBSSH2_SFTP *sftp, const std::string &remote,
                             Sha256Digest &out, std::string *why,
                             const std::function<bool()> *shouldCancel =
                                 nullptr) {
    LIBSSH2_SFTP_HANDLE *rh =
        libssh2_sftp_open_ex(sftp, remote.c_str(), (unsigned)remote.size(),
                             LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!rh) {
        if (why)
            *why = "Could not open remote file for full hashing";
        return false;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        if (why)
            *why = "Could not initialize full remote hash context";
        libssh2_sftp_close(rh);
        return false;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (why)
            *why = "EVP_DigestInit_ex(remote-full) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    std::array<unsigned char, 64 * 1024> buf{};
    while (true) {
        if (transfer_cancel_requested(shouldCancel)) {
            if (why)
                *why = "Canceled by user";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        ssize_t n = libssh2_sftp_read(rh, reinterpret_cast<char *>(buf.data()),
                                      buf.size());
        if (n > 0) {
            if (EVP_DigestUpdate(ctx, buf.data(), (std::size_t)n) != 1) {
                if (why)
                    *why = "EVP_DigestUpdate(remote-full) failed";
                EVP_MD_CTX_free(ctx);
                libssh2_sftp_close(rh);
                return false;
            }
            continue;
        }
        if (n == 0)
            break;
        if (why)
            *why = "Remote read failed during full hash";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    unsigned int outLen = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &outLen) != 1 ||
        outLen != SHA256_DIGEST_LENGTH) {
        if (why)
            *why = "EVP_DigestFinal_ex(remote-full) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    libssh2_sftp_close(rh);
    return true;
}

static bool verify_final_transfer_integrity(
    LIBSSH2_SFTP *sftp, const std::string &localPath,
    const std::string &remotePath, TransferIntegrityPolicy policy,
    const char *transferKind, std::string &err,
    const std::function<bool()> *shouldCancel) {
    if (policy == TransferIntegrityPolicy::Off)
        return true;

    Sha256Digest localDigest{}, remoteDigest{};
    std::string hashErr;
    const bool localOk =
        hash_local_full(localPath, localDigest, &hashErr, shouldCancel);
    const bool remoteOk =
        localOk && hash_remote_full(sftp, remotePath, remoteDigest, &hashErr,
                                    shouldCancel);
    if (fail_if_transfer_canceled(shouldCancel, err))
        return false;
    if (!localOk || !remoteOk) {
        if (policy != TransferIntegrityPolicy::Required)
            return true;
        err = std::string("Could not verify final integrity (") +
              transferKind + "): " + hashErr;
        return false;
    }
    if (localDigest != remoteDigest) {
        err = std::string("Final integrity check failed (") + transferKind +
              "): local/remote checksum mismatch";
        return false;
    }
    return true;
}
