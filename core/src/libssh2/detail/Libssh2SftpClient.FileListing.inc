bool Libssh2SftpClient::list(const std::string &remote_path,
                             std::vector<FileInfo> &out, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    std::string path = remote_path.empty() ? "/" : remote_path;

    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp_, path.c_str());
    if (!dir) {
        err = "sftp_opendir failed for: " + path;
        return false;
    }

    out.clear();
    out.reserve(64);

    char filename[512];
    char longentry[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (true) {
        memset(&attrs, 0, sizeof(attrs));
        int rc = libssh2_sftp_readdir_ex(dir, filename, sizeof(filename),
                                         longentry, sizeof(longentry), &attrs);
        if (rc > 0) {
            // rc = name length
            FileInfo fi{};
            fi.name = std::string(filename, static_cast<std::size_t>(rc));
            fi.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                            ? ((attrs.permissions & LIBSSH2_SFTP_S_IFMT) ==
                               LIBSSH2_SFTP_S_IFDIR)
                            : false;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
                fi.size = attrs.filesize;
                fi.has_size = true;
            }
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                fi.mtime = attrs.mtime;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                fi.mode = static_cast<std::uint32_t>(attrs.permissions);
            if (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
                fi.uid = static_cast<std::uint32_t>(attrs.uid);
                fi.gid = static_cast<std::uint32_t>(attrs.gid);
            }
            if (fi.name == "." || fi.name == "..")
                continue;
            out.push_back(std::move(fi));
        } else if (rc == 0) {
            // end of directory
            break;
        } else {
            err = "sftp_readdir_ex failed";
            libssh2_sftp_closedir(dir);
            return false;
        }
    }

    libssh2_sftp_closedir(dir);
    return true;
}
