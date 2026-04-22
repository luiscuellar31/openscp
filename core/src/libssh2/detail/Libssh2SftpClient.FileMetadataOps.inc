// Lightweight existence check using sftp_stat.
bool Libssh2SftpClient::exists(const std::string &remote_path, bool &isDir,
                               std::string &err) {
    isDir = false;
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    LIBSSH2_SFTP_ATTRIBUTES st{};
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  (unsigned)remote_path.size(),
                                  LIBSSH2_SFTP_STAT, &st);

    if (rc == 0) {
        if (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            isDir = ((st.permissions & LIBSSH2_SFTP_S_IFMT) ==
                     LIBSSH2_SFTP_S_IFDIR);
        }
        return true; // exists
    }

    unsigned long sftp_err = libssh2_sftp_last_error(sftp_);
    if (sftp_err == LIBSSH2_FX_NO_SUCH_FILE || sftp_err == LIBSSH2_FX_FAILURE) {
        err.clear();
        return false; // does not exist
    }

    err = "remote stat failed";
    return false;
}

// Detailed remote metadata (stat-like). Returns false if the path does not
// exist.
bool Libssh2SftpClient::stat(const std::string &remote_path, FileInfo &info,
                             std::string &err) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    LIBSSH2_SFTP_ATTRIBUTES st{};
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  (unsigned)remote_path.size(),
                                  LIBSSH2_SFTP_STAT, &st);
    if (rc != 0) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp_);
        if (sftp_err == LIBSSH2_FX_NO_SUCH_FILE ||
            sftp_err == LIBSSH2_FX_FAILURE) {
            err.clear();
            return false; // missing path
        }
        err = "remote stat failed";
        return false;
    }
    info.name.clear();
    info.is_dir =
        (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
            ? ((st.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR)
            : false;
    if (st.flags & LIBSSH2_SFTP_ATTR_SIZE) {
        info.size = (std::uint64_t)st.filesize;
        info.has_size = true;
    } else {
        info.size = 0;
        info.has_size = false;
    }
    info.mtime =
        (st.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (std::uint64_t)st.mtime : 0;
    info.mode = (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? st.permissions : 0;
    if (st.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        info.uid = st.uid;
        info.gid = st.gid;
    }
    return true;
}

bool Libssh2SftpClient::chmod(const std::string &remote_path,
                              std::uint32_t mode, std::string &err) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
#ifdef LIBSSH2_SFTP
    // libssh2 exposes chmod through SETSTAT.
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    a.permissions = mode;
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  (unsigned)remote_path.size(),
                                  LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "Remote chmod failed";
        return false;
    }
    return true;
#else
    (void)remote_path;
    (void)mode;
    err = "chmod not supported";
    return false;
#endif
}

bool Libssh2SftpClient::setTimes(const std::string &remote_path,
                                 std::uint64_t atime, std::uint64_t mtime,
                                 std::string &err) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
    a.atime = (unsigned long)atime;
    a.mtime = (unsigned long)mtime;
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  (unsigned)remote_path.size(),
                                  LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "Remote setTimes failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::chown(const std::string &remote_path, std::uint32_t uid,
                              std::uint32_t gid, std::string &err) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = 0;
    if (uid != (std::uint32_t)-1) {
        a.flags |= LIBSSH2_SFTP_ATTR_UIDGID;
        a.uid = uid;
    }
    if (gid != (std::uint32_t)-1) {
        a.flags |= LIBSSH2_SFTP_ATTR_UIDGID;
        a.gid = gid;
    }
    if (a.flags == 0) {
        // Nothing requested to change.
        err.clear();
        return true;
    }
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  (unsigned)remote_path.size(),
                                  LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "Remote chown failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::mkdir(const std::string &remote_dir, std::string &err,
                              unsigned int mode) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    int rc = libssh2_sftp_mkdir(sftp_, remote_dir.c_str(), mode);
    if (rc != 0) {
        err = "sftp_mkdir failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::removeFile(const std::string &remote_path,
                                   std::string &err) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    int rc = libssh2_sftp_unlink(sftp_, remote_path.c_str());
    if (rc != 0) {
        err = "sftp_unlink failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::removeDir(const std::string &remote_dir,
                                  std::string &err) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    int rc = libssh2_sftp_rmdir(sftp_, remote_dir.c_str());
    if (rc != 0) {
        err = "sftp_rmdir failed (directory not empty?)";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::rename(const std::string &from, const std::string &to,
                               std::string &err, bool overwrite) {
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    if (!rename_remote_with_fallback(sftp_, from, to, overwrite, &err)) {
        if (err.empty())
            err = "sftp_rename_ex failed";
        return false;
    }
    return true;
}

bool RemoveKnownHostEntry(const std::string &khPath, const std::string &host,
                          std::uint16_t port, std::string &err) {
    err.clear();
    if (khPath.empty()) {
        err = "known_hosts path is empty";
        return false;
    }
    if (host.empty()) {
        err = "host is empty";
        return false;
    }

    std::ifstream in(khPath, std::ios::binary);
    if (!in.is_open()) {
        err = "Could not open known_hosts";
        return false;
    }

    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        err = "Could not read known_hosts";
        return false;
    }

    std::vector<std::string> hostTokens;
    if (port == 22) {
        hostTokens.push_back(host);
        // Support this uncommon notation if present in existing files.
        hostTokens.push_back("[" + host + "]:22");
    } else {
        hostTokens.push_back("[" + host + "]:" + std::to_string(port));
    }

    const bool hadTrailingLf = !content.empty() && content.back() == '\n';
    std::istringstream inLines(content);
    std::vector<std::string> keptLines;
    std::string line;
    bool removedAny = false;

    while (std::getline(inLines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line_matches_site_host(line, hostTokens)) {
            removedAny = true;
            continue;
        }
        keptLines.push_back(line);
    }

    if (!removedAny)
        return true;

    std::string rewritten;
    for (std::size_t i = 0; i < keptLines.size(); ++i) {
        if (i > 0)
            rewritten.push_back('\n');
        rewritten += keptLines[i];
    }
    if (hadTrailingLf && !keptLines.empty())
        rewritten.push_back('\n');

    std::string persistErr;
    if (!persist_text_atomic(khPath, rewritten, &persistErr)) {
        err = std::string("Could not write known_hosts: ") + persistErr;
        return false;
    }
    return true;
}

std::unique_ptr<SftpClient>
Libssh2SftpClient::newConnectionLike(const SessionOptions &opt,
                                     std::string &err) {
    auto ptr = std::make_unique<Libssh2SftpClient>();
    if (!ptr->connect(opt, err))
        return nullptr;
    return ptr;
}
} // namespace openscp
