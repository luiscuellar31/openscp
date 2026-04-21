// Download a remote file to local. Reports progress and supports cooperative
// cancellation.
bool Libssh2SftpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    apply_transfer_socket_timeouts(sock_);

    const TransferIntegrityPolicy policy = transferIntegrityPolicy_;
    const std::string localPart = local + ".part";

    // Remote size (for progress and sanity checks)
    LIBSSH2_SFTP_ATTRIBUTES st{};
    if (libssh2_sftp_stat_ex(sftp_, remote.c_str(), (unsigned)remote.size(),
                             LIBSSH2_SFTP_STAT, &st) != 0) {
        err = "Could not stat remote path";
        return false;
    }
    const bool hasTotal = (st.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0;
    std::size_t total = hasTotal ? (std::size_t)st.filesize : 0;

    // Open remote for reading
    LIBSSH2_SFTP_HANDLE *rh =
        libssh2_sftp_open_ex(sftp_, remote.c_str(), (unsigned)remote.size(),
                             LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!rh) {
        err = "Could not open remote file for reading";
        return false;
    }

    std::size_t offset = 0;
    if (resume) {
        std::uint64_t partSize = 0;
        std::string sizeErr;
        if (get_local_file_size(localPart, partSize, &sizeErr)) {
            offset = (std::size_t)partSize;
        }
        if (offset > 0 && hasTotal && offset > total) {
            if (policy == TransferIntegrityPolicy::Required) {
                libssh2_sftp_close(rh);
                err = "Invalid resume: local .part is larger than remote file";
                return false;
            }
            offset = 0; // fallback: restart from scratch
        }
        if (offset > 0 && hasTotal && offset < total &&
            policy != TransferIntegrityPolicy::Off) {
            const std::uint64_t window =
                std::min<std::uint64_t>(offset, 64 * 1024);
            const std::uint64_t start = (std::uint64_t)offset - window;
            Sha256Digest lh{}, rhh{};
            std::string hErr;
            const bool okLocal =
                hash_local_range(localPart, start, window, lh, &hErr,
                                 &shouldCancel);
            const bool okRemote =
                okLocal && hash_remote_range(sftp_, remote, start, window, rhh,
                                             &hErr, &shouldCancel);
            if (shouldCancel && shouldCancel()) {
                libssh2_sftp_close(rh);
                err = "Canceled by user";
                return false;
            }
            if (!okLocal || !okRemote) {
                if (policy == TransferIntegrityPolicy::Required) {
                    libssh2_sftp_close(rh);
                    err = std::string("Could not validate resume integrity "
                                      "(download): ") +
                          hErr;
                    return false;
                }
                offset = 0; // optional: restart
            } else if (lh != rhh) {
                if (policy == TransferIntegrityPolicy::Required) {
                    libssh2_sftp_close(rh);
                    err = "Integrity check failed in resume (download): local "
                          "prefix does not match remote";
                    return false;
                }
                offset = 0; // optional: restart
            }
        }
    }

    if (offset > 0) {
        libssh2_sftp_seek64(rh, (libssh2_uint64_t)offset);
    }

    // Open local .part for writing
    FILE *lf = ::fopen(localPart.c_str(), offset > 0 ? "ab" : "wb");
    if (!lf) {
        libssh2_sftp_close(rh);
        err = "Could not open local file (.part) for writing";
        return false;
    }

    const std::size_t CHUNK = 64 * 1024;
    std::vector<char> buf(CHUNK);
    std::size_t done = offset;

    while (true) {
        if (shouldCancel && shouldCancel()) {
            err = "Canceled by user";
            std::fclose(lf);
            // Avoid a potentially blocking per-handle close on cancellation;
            // the worker session teardown will release pending handles.
            return false;
        }
        ssize_t n = libssh2_sftp_read(rh, buf.data(), (size_t)buf.size());
        if (n > 0) {
            if (std::fwrite(buf.data(), 1, (size_t)n, lf) != (size_t)n) {
                err = "Local write failed";
                std::fclose(lf);
                libssh2_sftp_close(rh);
                return false;
            }
            done = done + (std::size_t)n;
            if (progress && total)
                progress(done, total);
        } else if (n == 0) {
            break; // EOF
        } else {
            const bool canceledNow = (shouldCancel && shouldCancel());
            err = canceledNow ? "Canceled by user" : "Remote read failed";
            std::fclose(lf);
            if (!canceledNow) {
                (void)libssh2_sftp_close(rh);
            }
            return false;
        }
    }

    std::string syncErr;
    if (!flush_local_file(lf, &syncErr)) {
        std::fclose(lf);
        libssh2_sftp_close(rh);
        err = std::string("Could not sync local file (.part): ") + syncErr;
        return false;
    }
    std::fclose(lf);
    libssh2_sftp_close(rh);

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    if (!verify_final_transfer_integrity(sftp_, localPart, remote, policy,
                                         "download", err, &shouldCancel)) {
        return false;
    }

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    std::string replaceErr;
    if (!replace_local_file_atomic(localPart, local, &replaceErr)) {
        err = std::string("Could not finalize atomic download: ") + replaceErr;
        return false;
    }
    return true;
}

// Upload a local file to remote (create/truncate). Reports progress and
// supports cancellation.
bool Libssh2SftpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    apply_transfer_socket_timeouts(sock_);

    const TransferIntegrityPolicy policy = transferIntegrityPolicy_;
    const std::string remotePart = remote + ".part";

    // Open local for reading
    FILE *lf = ::fopen(local.c_str(), "rb");
    if (!lf) {
        err = "Could not open local file for reading";
        return false;
    }

    // Local size
    std::fseek(lf, 0, SEEK_END);
    long fsz = std::ftell(lf);
    std::fseek(lf, 0, SEEK_SET);
    std::size_t total = fsz > 0 ? (std::size_t)fsz : 0;

    // Resume against remote .part (final destination is set via atomic rename).
    long startOffset = 0;
    if (resume) {
        LIBSSH2_SFTP_ATTRIBUTES stR{};
        if (libssh2_sftp_stat_ex(sftp_, remotePart.c_str(),
                                 (unsigned)remotePart.size(), LIBSSH2_SFTP_STAT,
                                 &stR) == 0) {
            if (stR.flags & LIBSSH2_SFTP_ATTR_SIZE)
                startOffset = (long)stR.filesize;
        }
    }

    if (startOffset > 0 && (std::size_t)startOffset > total) {
        if (policy == TransferIntegrityPolicy::Required) {
            std::fclose(lf);
            err = "Invalid resume: remote .part is larger than local file";
            return false;
        }
        startOffset = 0;
    }

    if (startOffset > 0 && (std::size_t)startOffset < total &&
        policy != TransferIntegrityPolicy::Off) {
        const std::uint64_t window =
            std::min<std::uint64_t>((std::uint64_t)startOffset, 64 * 1024);
        const std::uint64_t start = (std::uint64_t)startOffset - window;
        Sha256Digest lsum{}, rsum{};
        std::string herr;
        const bool lok = hash_local_range(local, start, window, lsum, &herr,
                                          &shouldCancel);
        const bool rok = lok && hash_remote_range(sftp_, remotePart, start,
                                                  window, rsum, &herr,
                                                  &shouldCancel);
        if (shouldCancel && shouldCancel()) {
            std::fclose(lf);
            err = "Canceled by user";
            return false;
        }
        if (!lok || !rok) {
            if (policy == TransferIntegrityPolicy::Required) {
                std::fclose(lf);
                err = std::string(
                          "Could not validate resume integrity (upload): ") +
                      herr;
                return false;
            }
            startOffset = 0;
        } else if (lsum != rsum) {
            if (policy == TransferIntegrityPolicy::Required) {
                std::fclose(lf);
                err = "Integrity check failed in resume (upload): local/remote "
                      "prefix does not match";
                return false;
            }
            startOffset = 0;
        }
    }

    unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT |
                          ((startOffset > 0) ? 0 : LIBSSH2_FXF_TRUNC);
    LIBSSH2_SFTP_HANDLE *wh = libssh2_sftp_open_ex(
        sftp_, remotePart.c_str(), (unsigned)remotePart.size(), flags, 0644,
        LIBSSH2_SFTP_OPENFILE);
    if (!wh) {
        std::fclose(lf);
        err = "Could not open remote (.part) for writing";
        return false;
    }

    const std::size_t CHUNK = 64 * 1024;
    std::vector<char> buf(CHUNK);
    std::size_t done = 0;

    // If resuming, advance local and remote
    if (resume && startOffset > 0 && (std::size_t)startOffset < total) {
        libssh2_sftp_seek64(wh, (libssh2_uint64_t)startOffset);
        if (std::fseek(lf, startOffset, SEEK_SET) != 0) {
            err = "Could not seek local file";
            libssh2_sftp_close(wh);
            std::fclose(lf);
            return false;
        }
        done = (std::size_t)startOffset;
    }

    while (true) {
        size_t n = std::fread(buf.data(), 1, buf.size(), lf);
        if (n > 0) {
            char *p = buf.data();
            size_t remain = n;
            while (remain > 0) {
                if (shouldCancel && shouldCancel()) {
                    err = "Canceled by user";
                    std::fclose(lf);
                    // Avoid a potentially blocking per-handle close on
                    // cancellation; worker disconnect will release it.
                    return false;
                }
                ssize_t w = libssh2_sftp_write(wh, p, remain);
                if (w < 0) {
                    const bool canceledNow = (shouldCancel && shouldCancel());
                    err = canceledNow ? "Canceled by user" : "Remote write failed";
                    if (!canceledNow) {
                        (void)libssh2_sftp_close(wh);
                    }
                    std::fclose(lf);
                    return false;
                }
                remain = remain - (size_t)w;
                p = p + w;
                done = done + (size_t)w;
                if (progress && total)
                    progress(done, total);
            }
        } else {
            if (std::ferror(lf)) {
                err = "Local read failed";
                libssh2_sftp_close(wh);
                std::fclose(lf);
                return false;
            }
            break; // EOF
        }
    }

    libssh2_sftp_close(wh);
    std::fclose(lf);

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    if (!verify_final_transfer_integrity(sftp_, local, remotePart, policy,
                                         "upload", err, &shouldCancel)) {
        return false;
    }

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    std::string rnErr;
    if (!rename_remote_with_fallback(sftp_, remotePart, remote, true, &rnErr)) {
        err = std::string("Could not finalize upload (.part -> destination): ") +
              rnErr;
        return false;
    }

    return true;
}
