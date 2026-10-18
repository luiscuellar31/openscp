// Windows implementation of SafeLocalFile.hpp; POSIX uses SafeLocalFile.cpp.
//
// Paths arrive as UTF-8. Each component below the drive or share root is
// opened relative to the handle of the directory before it, with
// NtCreateFile, the Windows counterpart of openat(). A junction, symbolic
// link or mount point put in place of a parent therefore cannot redirect an
// operation, and the final step works on the opened handle: renames,
// deletions and timestamps never resolve the path a second time.
#include "common/Win32Text.hpp"
#include "openscp/SafeLocalFile.hpp"

// clang-format off
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>
// clang-format on

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// ntdll exports it; winternl.h leaves it out.
extern "C" NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(
    HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, FILE_INFORMATION_CLASS FileInformationClass);

namespace openscp::localfiles {
namespace {

// FILE_RENAME_IGNORE_READONLY_ATTRIBUTE from ntifs.h (Windows 10 1809): a
// rename replaces a read-only destination, as rename() does on POSIX.
constexpr DWORD kRenameIgnoreReadOnly = 0x00000040;
constexpr int kMaximumTrustedLinks = 8;
// 100-nanosecond intervals between 1601-01-01 and 1970-01-01.
constexpr std::int64_t kUnixEpochInFileTime = 116444736000000000LL;
constexpr std::int64_t kFileTimeUnitsPerSecond = 10000000LL;
constexpr ULONG kShareAll =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr ACCESS_MASK kDirectoryAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                                         FILE_READ_ATTRIBUTES | READ_CONTROL |
                                         SYNCHRONIZE;

class UniqueHandle {
    public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other)
            reset(std::exchange(other.handle_, nullptr));
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, nullptr);
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle_);
        handle_ = handle;
    }

    private:
    HANDLE handle_ = nullptr;
};

int errnoFromWin32(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
        return EBUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return EEXIST;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return ENOSPC;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_WRITE_PROTECT:
        return EROFS;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_PARAMETER:
        return EINVAL;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return ENOTSUP;
    case ERROR_CANT_ACCESS_FILE:
    case ERROR_CANT_RESOLVE_FILENAME:
    case ERROR_STOPPED_ON_SYMLINK:
        return ELOOP;
    default:
        return EIO;
    }
}

// "<operation>: <system message>", with errno set to match the error.
std::string win32Failure(const char *operation, DWORD code) {
    std::string message =
        std::string(operation) + ": " + win32::errorMessage(code);
    errno = errnoFromWin32(code);
    return message;
}

std::string errnoFailure(const char *operation, int code) {
    errno = code;
    return std::string(operation) + ": " + std::strerror(code);
}

bool succeeded(NTSTATUS status) {
    return status >= 0;
}

// The path below its root as names, never ".." and never empty.
struct LocalPath {
    std::wstring root;
    std::vector<std::wstring> names;
};

bool isSeparator(wchar_t character) {
    return character == L'\\' || character == L'/';
}

bool isDriveLetter(wchar_t character) {
    return (character >= L'A' && character <= L'Z') ||
           (character >= L'a' && character <= L'z');
}

// The length of the root of an absolute path, or 0 for any other path:
// "C:\", "\\server\share\", "\\?\C:\" and "\\?\UNC\server\share\". The root
// of a UNC path is its share, which cannot be opened without it.
std::size_t rootLength(std::wstring_view path) {
    if (path.size() >= 3 && isDriveLetter(path[0]) && path[1] == L':' &&
        isSeparator(path[2]))
        return 3;
    if (path.size() < 2 || !isSeparator(path[0]) || !isSeparator(path[1]))
        return 0;
    std::size_t start = 2;
    if (path.size() >= 4 && path[2] == L'?' && path[3] == L'\\') {
        const std::wstring_view rest = path.substr(4);
        if (rest.size() >= 3 && isDriveLetter(rest[0]) && rest[1] == L':' &&
            rest[2] == L'\\')
            return 7;
        if (rest.size() < 4 || ::CompareStringOrdinal(rest.data(), 4, L"UNC\\",
                                                      4, TRUE) != CSTR_EQUAL)
            return 0;
        start = 8;
    } else if (path.size() >= 3 && (path[2] == L'?' || path[2] == L'.')) {
        // Other device and verbatim paths are not local folders.
        return 0;
    }
    // Server, then share, each followed by a separator or the end.
    std::size_t position = start;
    for (int part = 0; part < 2; ++part) {
        const std::size_t begin = position;
        while (position < path.size() && !isSeparator(path[position]))
            ++position;
        if (position == begin)
            return 0;
        if (position < path.size())
            ++position;
    }
    return position;
}

std::wstring nameProblem(std::wstring_view name);

bool parseLocalPath(const std::string &utf8, LocalPath &path,
                    std::string &error) {
    std::wstring wide;
    if (!win32::toWidePath(utf8, wide)) {
        error = errnoFailure("Local path is not valid UTF-8", EINVAL);
        return false;
    }
    const std::size_t root = rootLength(wide);
    if (root == 0) {
        errno = EINVAL;
        error = "Local path must be an absolute path on a drive or share.";
        return false;
    }
    path.root = wide.substr(0, root);
    for (wchar_t &character : path.root) {
        if (character == L'/')
            character = L'\\';
    }
    if (!isSeparator(path.root.back()))
        path.root.push_back(L'\\');
    path.names.clear();
    std::size_t position = root;
    while (position < wide.size()) {
        std::size_t end = position;
        while (end < wide.size() && !isSeparator(wide[end]))
            ++end;
        const std::wstring_view name(wide.data() + position, end - position);
        position = end + 1;
        if (name.empty() || name == L".")
            continue;
        if (name == L"..") {
            errno = EINVAL;
            error = "Local destination path contains a parent traversal.";
            return false;
        }
        const std::wstring problem = nameProblem(name);
        if (!problem.empty()) {
            errno = EINVAL;
            error = "Local path has a name Windows cannot use: " +
                    win32::toUtf8(problem);
            return false;
        }
        path.names.emplace_back(name);
    }
    return true;
}

bool sameName(std::wstring_view left, std::wstring_view right) {
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                  right.data(), static_cast<int>(right.size()),
                                  TRUE) == CSTR_EQUAL;
}

// Opens `name` relative to the directory `parent`, as openat() does.
NTSTATUS openRelative(HANDLE parent, std::wstring_view name, ACCESS_MASK access,
                      ULONG share, ULONG disposition, ULONG options,
                      UniqueHandle &opened, ULONG_PTR *outcome = nullptr) {
    UNICODE_STRING unicodeName;
    unicodeName.Buffer = const_cast<PWSTR>(name.data());
    unicodeName.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicodeName.MaximumLength = unicodeName.Length;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &unicodeName, OBJ_CASE_INSENSITIVE,
                               parent, nullptr);
    IO_STATUS_BLOCK status{};
    HANDLE handle = nullptr;
    const NTSTATUS result =
        ::NtCreateFile(&handle, access | SYNCHRONIZE, &attributes, &status,
                       nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                       options | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (succeeded(result)) {
        opened.reset(handle);
        if (outcome)
            *outcome = status.Information;
    }
    return result;
}

std::string ntFailure(const char *operation, NTSTATUS status) {
    return win32Failure(operation, ::RtlNtStatusToDosError(status));
}

bool isNotFound(NTSTATUS status) {
    const ULONG code = ::RtlNtStatusToDosError(status);
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

bool attributeTags(HANDLE handle, FILE_ATTRIBUTE_TAG_INFO &tags,
                   std::string &error) {
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tags,
                                       sizeof(tags)))
        return true;
    error = win32Failure("Could not inspect local path", ::GetLastError());
    return false;
}

// A junction, symbolic link or mount point: an entry that names another one.
// Other reparse points, such as OneDrive placeholders or deduplicated files,
// store the entry's own data differently and are used as they are.
bool isLink(const FILE_ATTRIBUTE_TAG_INFO &tags) {
    return (tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           IsReparseTagNameSurrogate(tags.ReparseTag);
}

std::vector<BYTE> tokenSid(TOKEN_INFORMATION_CLASS kind) {
    std::vector<BYTE> sid;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return sid;
    UniqueHandle tokenOwner(token);
    DWORD size = 0;
    (void)::GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (size == 0 ||
        !::GetTokenInformation(token, kind, buffer.data(), size, &size))
        return sid;
    PSID source = kind == TokenUser
                      ? reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid
                      : reinterpret_cast<TOKEN_OWNER *>(buffer.data())->Owner;
    const DWORD length = ::GetLengthSid(source);
    sid.resize(length);
    if (!::CopySid(length, sid.data(), source))
        sid.clear();
    return sid;
}

enum class Ownership { CurrentUser, OtherAccount, Unknown };

// Whether the current user, or the default owner of what it creates (the
// Administrators group in an elevated process), owns the entry.
Ownership ownership(HANDLE handle) {
    static const std::vector<BYTE> user = tokenSid(TokenUser);
    static const std::vector<BYTE> defaultOwner = tokenSid(TokenOwner);
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                          &owner, nullptr, nullptr, nullptr,
                          &descriptor) != ERROR_SUCCESS)
        return Ownership::Unknown;
    Ownership result = Ownership::Unknown;
    if (owner && ::IsValidSid(owner)) {
        const auto matches = [owner](const std::vector<BYTE> &sid) {
            return !sid.empty() &&
                   ::EqualSid(owner, const_cast<BYTE *>(sid.data()));
        };
        result = matches(user) || matches(defaultOwner)
                     ? Ownership::CurrentUser
                     : Ownership::OtherAccount;
    }
    ::LocalFree(descriptor);
    return result;
}

bool isRemote(HANDLE handle) {
    FILE_REMOTE_PROTOCOL_INFO protocol{};
    return ::GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo,
                                          &protocol, sizeof(protocol)) != 0;
}

// Whether the current user may add or remove entries in directory, and so
// make or replace a link there: any one of these rights is enough, as making
// a folder is all a junction needs. ReOpenFile refuses directory handles
// whatever the access, so the directory is opened again relative to itself.
bool canChangeEntries(HANDLE directory) {
    for (const ACCESS_MASK right :
         {FILE_ADD_FILE, FILE_ADD_SUBDIRECTORY, FILE_DELETE_CHILD}) {
        UniqueHandle reopened;
        const NTSTATUS status =
            openRelative(directory, {}, right, kShareAll, FILE_OPEN,
                         FILE_DIRECTORY_FILE, reopened);
        if (succeeded(status) ||
            ::RtlNtStatusToDosError(status) != ERROR_ACCESS_DENIED)
            return true;
    }
    return false;
}

// A directory the current user can neither change nor owns, such as C:\Users:
// a link in it was put there by the system, like the links POSIX trusts in
// root-owned directories. C:\ and C:\ProgramData are not: any user may make
// folders in them.
bool isTrustedSystemDirectory(HANDLE directory) {
    // An owner can always grant itself more access.
    return !canChangeEntries(directory) &&
           ownership(directory) == Ownership::OtherAccount;
}

UniqueHandle openRoot(const std::wstring &root, std::string &error) {
    UniqueHandle handle(::CreateFileW(root.c_str(), kDirectoryAccess, kShareAll,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle)
        error = win32Failure("Could not open local destination directory",
                             ::GetLastError());
    return handle;
}

// Opens the directory `path` names, never following a link unless the
// directory that holds it is one the current user cannot change. With
// `create`, missing directories are made, inheriting their parent's
// permissions.
bool openSafeDirectory(const LocalPath &path, bool create,
                       UniqueHandle &directory, std::string &error) {
    UniqueHandle current = openRoot(path.root, error);
    if (!current)
        return false;
    int followedLinks = 0;
    for (const std::wstring &name : path.names) {
        UniqueHandle next;
        NTSTATUS status =
            openRelative(current.get(), name, kDirectoryAccess, kShareAll,
                         create ? FILE_OPEN_IF : FILE_OPEN,
                         FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, next);
        if (!succeeded(status)) {
            error = ntFailure("Could not safely open local destination "
                              "directory",
                              status);
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO tags{};
        if (!attributeTags(next.get(), tags, error))
            return false;
        if ((tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (isLink(tags) && (++followedLinks > kMaximumTrustedLinks ||
                                 !isTrustedSystemDirectory(current.get()))) {
                errno = ELOOP;
                error = "Local destination path contains an untrusted "
                        "symbolic link.";
                return false;
            }
            // Open it again so the system resolves it: a trusted link leads
            // to its target, and storage filters such as OneDrive's see an
            // ordinary open of their folders.
            status =
                openRelative(current.get(), name, kDirectoryAccess, kShareAll,
                             FILE_OPEN, FILE_DIRECTORY_FILE, next);
            if (!succeeded(status)) {
                error = ntFailure("Could not resolve system directory link",
                                  status);
                return false;
            }
        }
        current = std::move(next);
    }
    directory = std::move(current);
    return true;
}

// Splits `path` into its parent directory, opened safely, and the final
// name. Fails with ENOENT when the parent does not exist.
bool openParent(const std::string &path, const char *withoutName,
                UniqueHandle &parent, std::wstring &name, std::string &error) {
    LocalPath parsed;
    if (!parseLocalPath(path, parsed, error))
        return false;
    if (parsed.names.empty()) {
        errno = EINVAL;
        error = withoutName;
        return false;
    }
    name = std::move(parsed.names.back());
    parsed.names.pop_back();
    return openSafeDirectory(parsed, false, parent, error);
}

void toUnixTime(LARGE_INTEGER fileTime, std::int64_t &seconds,
                std::int64_t &nanoseconds) {
    const std::int64_t units = fileTime.QuadPart - kUnixEpochInFileTime;
    seconds = units / kFileTimeUnitsPerSecond;
    std::int64_t remainder = units % kFileTimeUnitsPerSecond;
    if (remainder < 0) {
        --seconds;
        remainder += kFileTimeUnitsPerSecond;
    }
    nanoseconds = remainder * 100;
}

bool readRegularFileIdentity(HANDLE file, LocalFileIdentity &identity,
                             std::string &error) {
    FILE_ATTRIBUTE_TAG_INFO tags{};
    if (!attributeTags(file, tags, error))
        return false;
    if ((tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || isLink(tags) ||
        ::GetFileType(file) != FILE_TYPE_DISK) {
        errno = EINVAL;
        error = "Local source is not a regular file.";
        return false;
    }
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                        sizeof(standard)) ||
        !::GetFileInformationByHandleEx(file, FileBasicInfo, &basic,
                                        sizeof(basic))) {
        error = win32Failure("Could not inspect local source file",
                             ::GetLastError());
        return false;
    }
    LocalFileIdentity result;
    FILE_ID_INFO fileId{};
    if (::GetFileInformationByHandleEx(file, FileIdInfo, &fileId,
                                       sizeof(fileId))) {
        result.device = fileId.VolumeSerialNumber;
        std::memcpy(&result.inode, fileId.FileId.Identifier,
                    sizeof(result.inode));
        std::memcpy(&result.inodeHigh,
                    fileId.FileId.Identifier + sizeof(result.inode),
                    sizeof(result.inodeHigh));
    } else {
        // File systems without 128-bit IDs, such as FAT.
        BY_HANDLE_FILE_INFORMATION information{};
        if (!::GetFileInformationByHandle(file, &information)) {
            error = win32Failure("Could not inspect local source file",
                                 ::GetLastError());
            return false;
        }
        result.device = information.dwVolumeSerialNumber;
        result.inode =
            (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
            information.nFileIndexLow;
    }
    result.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    toUnixTime(basic.LastWriteTime, result.modifiedSeconds,
               result.modifiedNanoseconds);
    toUnixTime(basic.ChangeTime, result.changedSeconds,
               result.changedNanoseconds);
    identity = result;
    return true;
}

// Deletes the entry `handle` refers to: the name goes at once, even while
// other programs that allowed it still have the entry open. File systems
// without POSIX semantics, such as FAT, delete it when the last handle closes.
bool deleteOpenedEntry(HANDLE handle, std::string &error) {
    FILE_DISPOSITION_INFO_EX disposition{
        FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (::SetFileInformationByHandle(handle, FileDispositionInfoEx,
                                     &disposition, sizeof(disposition)))
        return true;
    DWORD code = ::GetLastError();
    if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED ||
        code == ERROR_INVALID_FUNCTION) {
        FILE_BASIC_INFO basic{};
        if (::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                           sizeof(basic)) &&
            (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            FILE_BASIC_INFO writable{};
            writable.FileAttributes =
                (basic.FileAttributes & ~FILE_ATTRIBUTE_READONLY) |
                FILE_ATTRIBUTE_NORMAL;
            (void)::SetFileInformationByHandle(handle, FileBasicInfo, &writable,
                                               sizeof(writable));
        }
        FILE_DISPOSITION_INFO legacy{TRUE};
        if (::SetFileInformationByHandle(handle, FileDispositionInfo, &legacy,
                                         sizeof(legacy)))
            return true;
        code = ::GetLastError();
    }
    error = win32Failure("Could not safely remove local path", code);
    return false;
}

bool isTransientSharingError(DWORD code) {
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
           code == ERROR_ACCESS_DENIED;
}

// Renames the opened file to `name` in `directory`, replacing what is there.
// SetFileInformationByHandle would resolve a bare name against the process's
// current directory and refuses a directory handle, so this goes to the NT
// call, which resolves the name relative to the handle. Antivirus scanners,
// search indexers and preview handlers often hold a just-written file for a
// moment, so sharing errors are retried briefly.
DWORD renameOpenedFile(HANDLE file, HANDLE directory, std::wstring_view name) {
    // FILE_RENAME_INFORMATION in ntifs.h has the layout of FILE_RENAME_INFO.
    // winternl.h declares no rename class, and its enumeration of a single
    // value cannot take these by a cast, so they go in by their bits.
    const auto kRenameInformation = std::bit_cast<FILE_INFORMATION_CLASS>(10);
    const auto kRenameInformationEx = std::bit_cast<FILE_INFORMATION_CLASS>(65);
    const std::size_t nameBytes = name.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto *info = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
    info->RootDirectory = directory;
    info->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(info->FileName, name.data(), nameBytes);
    const auto bufferSize = static_cast<ULONG>(buffer.size());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool extendedRename = true;
    for (;;) {
        IO_STATUS_BLOCK status{};
        if (extendedRename) {
            info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                          FILE_RENAME_FLAG_POSIX_SEMANTICS |
                          kRenameIgnoreReadOnly;
        } else {
            info->Flags = 0;
            info->ReplaceIfExists = TRUE;
        }
        const NTSTATUS result = ::NtSetInformationFile(
            file, &status, info, bufferSize,
            extendedRename ? kRenameInformationEx : kRenameInformation);
        if (succeeded(result))
            return ERROR_SUCCESS;
        const DWORD code = ::RtlNtStatusToDosError(result);
        // FAT and many network file systems only know the plain rename.
        if (extendedRename &&
            (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED ||
             code == ERROR_INVALID_FUNCTION)) {
            extendedRename = false;
            continue;
        }
        if (!isTransientSharingError(code) ||
            std::chrono::steady_clock::now() >= deadline)
            return code;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool isReservedDeviceName(std::wstring_view name) {
    // The device names, matched without the extension and the spaces before
    // it, as Windows does: "nul.txt" and "COM1 .log" are devices too.
    const std::size_t dot = name.find(L'.');
    std::wstring_view base = name.substr(0, dot);
    while (!base.empty() && base.back() == L' ')
        base.remove_suffix(1);
    static constexpr std::array<std::wstring_view, 6> kNames{
        L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (const std::wstring_view reserved : kNames) {
        if (sameName(base, reserved))
            return true;
    }
    if (base.size() == 4 && (sameName(base.substr(0, 3), L"COM") ||
                             sameName(base.substr(0, 3), L"LPT"))) {
        const wchar_t digit = base[3];
        // Superscript one, two and three count as digits too.
        return (digit >= L'0' && digit <= L'9') || digit == L'\u00B9' ||
               digit == L'\u00B2' || digit == L'\u00B3';
    }
    return false;
}

std::wstring nameProblem(std::wstring_view name) {
    if (name.empty() || name == L"." || name == L"..")
        return L"A name cannot be empty, '.' or '..'.";
    if (name.size() > 255)
        return L"A name cannot be longer than 255 characters.";
    for (const wchar_t character : name) {
        if (character < 0x20 ||
            std::wstring_view(L"<>:\"/\\|?*").find(character) !=
                std::wstring_view::npos)
            return L"A name cannot contain <>:\"/\\|?* or control characters.";
    }
    if (name.back() == L'.' || name.back() == L' ')
        return L"A name cannot end with a dot or a space.";
    if (isReservedDeviceName(name))
        return L"A name cannot be a Windows device name such as CON, NUL or "
               L"COM1.";
    return std::wstring();
}

// Whether `name` has the form of the short names Windows makes for long ones,
// such as REPORT~1.TXT for "Report for 2026.txt" or DOCUME~1: NTFS and FAT
// open the long-named entry by its short name, so a file or folder given
// that name would replace or fill another.
bool looksLikeShortName(std::wstring_view name) {
    const std::wstring_view base = name.substr(0, name.find(L'.'));
    const std::size_t tilde = base.rfind(L'~');
    if (tilde == std::wstring_view::npos || tilde + 1 == base.size())
        return false;
    for (const wchar_t character : base.substr(tilde + 1)) {
        if (character < L'0' || character > L'9')
            return false;
    }
    // A legal 8.3 name in the system's OEM code page, without the spaces no
    // generated short name has.
    UNICODE_STRING unicodeName;
    unicodeName.Buffer = const_cast<PWSTR>(name.data());
    unicodeName.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicodeName.MaximumLength = unicodeName.Length;
    BOOLEAN spaces = FALSE;
    return ::RtlIsNameLegalDOS8Dot3(&unicodeName, nullptr, &spaces) && !spaces;
}

} // namespace

std::string localNameProblem(const std::string &name) {
    std::wstring wide;
    if (!win32::toWidePath(name, wide))
        return "A name must be valid UTF-8.";
    std::wstring problem = nameProblem(wide);
    // Checked for a single name, such as one from a server, and not for each
    // name of a whole path, which may reach a folder by its short name.
    if (problem.empty() && looksLikeShortName(wide))
        problem = L"A name cannot have the form of a Windows short name, such "
                  L"as REPORT~1.TXT, which can stand for another entry.";
    return win32::toUtf8(problem);
}

std::string localEntryName(const std::string &path) {
    std::wstring wide;
    if (!win32::toExtendedLengthPath(path, wide) || wide.empty())
        return {};
    // A name has no wildcard: localNameProblem() refuses * and ?.
    WIN32_FIND_DATAW entry;
    const HANDLE search =
        ::FindFirstFileExW(wide.c_str(), FindExInfoBasic, &entry,
                           FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE)
        return {};
    ::FindClose(search);
    return win32::toUtf8(entry.cFileName);
}

bool ensureLocalDirectories(const std::string &path, std::string &error) {
    error.clear();
    LocalPath parsed;
    if (!parseLocalPath(path, parsed, error))
        return false;
    UniqueHandle directory;
    return openSafeDirectory(parsed, true, directory, error);
}

bool removeLocalPath(const std::string &path, bool directory,
                     std::string &error) {
    error.clear();
    UniqueHandle parent;
    std::wstring name;
    if (!openParent(path, "Local removal path does not identify an entry.",
                    parent, name, error)) {
        if (errno == ENOENT) {
            error.clear();
            return true;
        }
        return false;
    }
    UniqueHandle entry;
    const NTSTATUS status =
        openRelative(parent.get(), name,
                     DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                     kShareAll, FILE_OPEN, FILE_OPEN_REPARSE_POINT, entry);
    if (!succeeded(status)) {
        if (isNotFound(status))
            return true;
        error = ntFailure("Could not safely remove local path", status);
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tags{};
    if (!attributeTags(entry.get(), tags, error))
        return false;
    // A link goes as a link, as unlinkat() removes a symbolic link to a
    // directory; only real entries must match the kind asked for.
    if (!isLink(tags)) {
        const bool isDirectory =
            (tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directory != isDirectory) {
            error = errnoFailure("Could not safely remove local path",
                                 directory ? ENOTDIR : EISDIR);
            return false;
        }
    }
    return deleteOpenedEntry(entry.get(), error);
}

bool localFileIdentity(const std::string &path, LocalFileIdentity &identity,
                       std::string &error) {
    error.clear();
    UniqueHandle parent;
    std::wstring name;
    if (!openParent(path, "Local source path does not identify a file.", parent,
                    name, error))
        return false;
    // Attribute access only: it never conflicts with other programs' sharing
    // modes and never downloads an online-only OneDrive file.
    UniqueHandle file;
    const NTSTATUS status =
        openRelative(parent.get(), name, FILE_READ_ATTRIBUTES, kShareAll,
                     FILE_OPEN, FILE_OPEN_REPARSE_POINT, file);
    if (!succeeded(status)) {
        error = ntFailure("Could not inspect local source file", status);
        return false;
    }
    return readRegularFileIdentity(file.get(), identity, error);
}

bool removeLocalFileIfUnchanged(const std::string &path,
                                const LocalFileIdentity &identity,
                                std::string &error) {
    error.clear();
    UniqueHandle parent;
    std::wstring name;
    if (!openParent(path, "Local source path does not identify a file.", parent,
                    name, error)) {
        if (errno == ENOENT) {
            error.clear();
            return true;
        }
        return false;
    }
    // Sharing only reads: while this handle is open, no other program can
    // write, rename or delete the file, so it cannot change between the
    // comparison and the deletion, unlike on POSIX.
    UniqueHandle file;
    const NTSTATUS status =
        openRelative(parent.get(), name,
                     DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                     FILE_SHARE_READ, FILE_OPEN, FILE_OPEN_REPARSE_POINT, file);
    if (!succeeded(status)) {
        if (isNotFound(status))
            return true;
        error = ntFailure("Could not inspect local source file", status);
        return false;
    }
    LocalFileIdentity current;
    if (!readRegularFileIdentity(file.get(), current, error))
        return false;
    if (current != identity) {
        errno = EAGAIN;
        error = "Local source changed after transfer; it was not removed.";
        return false;
    }
    return deleteOpenedEntry(file.get(), error);
}

bool setLocalModificationTime(const std::string &path,
                              std::int64_t modifiedSeconds,
                              std::string &error) {
    error.clear();
    constexpr std::int64_t kEarliest =
        -kUnixEpochInFileTime / kFileTimeUnitsPerSecond + 1;
    constexpr std::int64_t kLatest =
        (std::numeric_limits<std::int64_t>::max() - kUnixEpochInFileTime) /
        kFileTimeUnitsPerSecond;
    if (modifiedSeconds < kEarliest || modifiedSeconds > kLatest) {
        error = errnoFailure("Could not set local modification time", EINVAL);
        return false;
    }
    UniqueHandle parent;
    std::wstring name;
    if (!openParent(path, "Local timestamp path does not identify a file.",
                    parent, name, error))
        return false;
    UniqueHandle file;
    const NTSTATUS status = openRelative(
        parent.get(), name, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        kShareAll, FILE_OPEN, FILE_OPEN_REPARSE_POINT, file);
    if (!succeeded(status)) {
        error =
            ntFailure("Could not safely open local file for timestamp", status);
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tags{};
    if (!attributeTags(file.get(), tags, error))
        return false;
    if ((tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || isLink(tags)) {
        errno = EINVAL;
        error = "Local timestamp path is not a regular file.";
        return false;
    }
    // Zero leaves the other times and the attributes as they are.
    FILE_BASIC_INFO basic{};
    basic.LastWriteTime.QuadPart =
        modifiedSeconds * kFileTimeUnitsPerSecond + kUnixEpochInFileTime;
    if (!::SetFileInformationByHandle(file.get(), FileBasicInfo, &basic,
                                      sizeof(basic))) {
        error = win32Failure("Could not set local modification time",
                             ::GetLastError());
        return false;
    }
    return true;
}

std::FILE *openRegularFileForWrite(const std::string &path, WriteMode mode,
                                   std::string &error) {
    error.clear();
    UniqueHandle parent;
    std::wstring name;
    if (!openParent(path, "Local partial path does not identify a file.",
                    parent, name, error))
        return nullptr;
    // Other programs may read the file while it is written, but not change,
    // rename or delete it.
    UniqueHandle file;
    ULONG_PTR outcome = 0;
    const NTSTATUS status = openRelative(
        parent.get(), name, FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, FILE_OPEN_IF,
        FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, file, &outcome);
    if (!succeeded(status)) {
        error =
            ntFailure("Could not safely open local file for writing", status);
        return nullptr;
    }
    FILE_ATTRIBUTE_TAG_INFO tags{};
    FILE_STANDARD_INFO standard{};
    if (!attributeTags(file.get(), tags, error))
        return nullptr;
    if (!::GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard,
                                        sizeof(standard))) {
        error = win32Failure("Could not inspect local partial file",
                             ::GetLastError());
        return nullptr;
    }
    // Another account's file, which it could still read, matters only where
    // the owner is recorded reliably: network shares map owners their own way.
    const bool foreign = outcome == FILE_OPENED && !isRemote(file.get()) &&
                         ownership(file.get()) == Ownership::OtherAccount;
    if ((tags.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        ::GetFileType(file.get()) != FILE_TYPE_DISK ||
        standard.NumberOfLinks != 1 || foreign) {
        errno = EINVAL;
        error = "Local partial path must be a user-owned regular file without "
                "additional hard links.";
        return nullptr;
    }
    if (mode == WriteMode::Truncate) {
        FILE_END_OF_FILE_INFO end{};
        if (!::SetFileInformationByHandle(file.get(), FileEndOfFileInfo, &end,
                                          sizeof(end))) {
            error = win32Failure("Could not truncate local partial file",
                                 ::GetLastError());
            return nullptr;
        }
    }

    const int descriptor = ::_open_osfhandle(
        reinterpret_cast<intptr_t>(file.get()),
        _O_BINARY | _O_WRONLY | (mode == WriteMode::Append ? _O_APPEND : 0));
    if (descriptor < 0) {
        error = errnoFailure("Could not create local file descriptor", errno);
        return nullptr;
    }
    (void)file.release();
    std::FILE *stream =
        ::_fdopen(descriptor, mode == WriteMode::Append ? "ab" : "wb");
    if (!stream) {
        const int savedError = errno;
        ::_close(descriptor);
        error = errnoFailure("Could not create local file stream", savedError);
        return nullptr;
    }
    return stream;
}

std::uint32_t uploadPermissions(std::FILE *) {
    // Windows has no permission bits; 0644 is what a umask of 022 gives.
    return 0644;
}

bool flushAndSync(std::FILE *file, std::string &error,
                  LocalFileDurability durability) {
    if (!file) {
        error = "Invalid local file handle.";
        errno = EINVAL;
        return false;
    }
    if (std::fflush(file) != 0) {
        error = errnoFailure("Could not flush local file", errno);
        return false;
    }
    durability = normalizeLocalFileDurability(durability);
    if (durability == LocalFileDurability::Buffered)
        return true;
    const intptr_t osHandle = ::_get_osfhandle(::_fileno(file));
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the CRT gives an integer.
    const auto handle = reinterpret_cast<HANDLE>(osHandle);
    if (handle == INVALID_HANDLE_VALUE || !::FlushFileBuffers(handle)) {
        error =
            win32Failure("Could not synchronize local file",
                         handle == INVALID_HANDLE_VALUE ? ERROR_INVALID_HANDLE
                                                        : ::GetLastError());
        return false;
    }
    return true;
}

bool atomicReplace(const std::string &temporary, const std::string &destination,
                   std::string &error, LocalFileDurability durability) {
    // Windows has no directory sync; NTFS journals the rename itself.
    (void)durability;
    LocalPath temporaryPath;
    LocalPath destinationPath;
    if (!parseLocalPath(temporary, temporaryPath, error) ||
        !parseLocalPath(destination, destinationPath, error))
        return false;
    const auto sameParent = [&]() {
        if (temporaryPath.names.empty() || destinationPath.names.empty() ||
            temporaryPath.names.size() != destinationPath.names.size() ||
            !sameName(temporaryPath.root, destinationPath.root))
            return false;
        for (std::size_t index = 0; index + 1 < temporaryPath.names.size();
             ++index) {
            if (!sameName(temporaryPath.names[index],
                          destinationPath.names[index]))
                return false;
        }
        return true;
    };
    if (!sameParent()) {
        errno = EINVAL;
        error = "Local atomic replacement requires sibling file paths.";
        return false;
    }
    const std::wstring temporaryName = temporaryPath.names.back();
    const std::wstring destinationName = destinationPath.names.back();
    temporaryPath.names.pop_back();
    UniqueHandle parent;
    if (!openSafeDirectory(temporaryPath, false, parent, error))
        return false;
    UniqueHandle file;
    const NTSTATUS status = openRelative(
        parent.get(), temporaryName, DELETE | FILE_READ_ATTRIBUTES, kShareAll,
        FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, file);
    if (!succeeded(status)) {
        error = ntFailure("Could not atomically finalize local file", status);
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tags{};
    if (!attributeTags(file.get(), tags, error))
        return false;
    if ((tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        errno = EINVAL;
        error = "Local partial path is not a regular file.";
        return false;
    }
    const DWORD code =
        renameOpenedFile(file.get(), parent.get(), destinationName);
    if (code != ERROR_SUCCESS) {
        error = win32Failure("Could not atomically finalize local file", code);
        return false;
    }
    return true;
}

} // namespace openscp::localfiles
