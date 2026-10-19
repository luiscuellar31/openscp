#include "PlatformTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/common/MainWindowSharedUtils.hpp"
#include "logic/common/RowSelection.hpp"
#include "logic/common/StagingStore.hpp"
#include "logic/common/UiFormatters.hpp"
#include "logic/navigation/RemotePath.hpp"
#include "logic/remote/RemoteModel.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

OPENSCP_TEST(testRemotePaths, test) {
    test.check(normalizeRemotePath(QString()) == QStringLiteral("/"),
               "an empty remote path should normalize to root");
    test.check(normalizeRemotePath(QStringLiteral("folder\\child")) ==
                   QStringLiteral("/folder/child"),
               "remote paths should normalize both separator styles");
    test.check(normalizeRemotePath(QStringLiteral("/one/./two/../three")) ==
                   QStringLiteral("/one/three"),
               "dot segments should be resolved");
    test.check(normalizeRemotePath(QStringLiteral("../../outside")) ==
                   QStringLiteral("/outside"),
               "remote paths should remain confined to the logical root");
    test.check(joinRemotePath(QStringLiteral("/base"), QStringLiteral("a/b")) ==
                   QStringLiteral("/base/a/b"),
               "joining should accept a safe relative path");
    test.check(joinRemotePath(QStringLiteral("/base"),
                              QStringLiteral("../b")) == QStringLiteral("/b"),
               "joining should normalize traversal segments");
}

OPENSCP_TEST(testRemoteEntryNameSafety, test) {
    test.check(isSafeRemoteEntryName(QStringLiteral("report.txt")),
               "ordinary remote entry names should be accepted");
    test.check(isSafeRemoteEntryName(QString::fromUtf8("résumé 2026")),
               "Unicode and spaces should be accepted in remote names");
    for (const QString &name :
         {QString(), QStringLiteral("."), QStringLiteral(".."),
          QStringLiteral("folder/file"), QStringLiteral("folder\\file"),
          QStringLiteral("line\nbreak"), QString(QChar(0x7f))}) {
        test.check(!isSafeRemoteEntryName(name),
                   "unsafe remote entry names should be rejected");
    }
}

OPENSCP_TEST(testByteFormatting, test) {
    test.check(formatByteSize(0) == QStringLiteral("0 B"),
               "zero bytes should use the byte unit");
    test.check(formatByteSize(1536) == QStringLiteral("1.5 KiB"),
               "binary byte sizes should use IEC units");
    test.check(formatTransferRate(1.5) == QStringLiteral("1.5 KiB/s"),
               "transfer rates should reuse byte-size formatting");
    test.check(formatTransferRate(0.0) == QString::fromUtf8("—"),
               "unknown transfer rates should use an em dash");
}

OPENSCP_TEST(testPathDepthOrdering, test) {
    const PathDepthComparator shallowestFirst{.deepestFirst = false};
    test.check(
        shallowestFirst(QStringLiteral("/root"), QStringLiteral("/root/child")),
        "shallow ordering should place parent paths first");
    test.check(
        shallowestFirst(QStringLiteral("/root/a"), QStringLiteral("/root/b")),
        "shallow ordering should break equal-depth ties ascending");

    const PathDepthComparator deepestFirst{.deepestFirst = true};
    test.check(
        deepestFirst(QStringLiteral("/root/child"), QStringLiteral("/root")),
        "deep ordering should place child paths first");
    test.check(
        deepestFirst(QStringLiteral("/root/b"), QStringLiteral("/root/a")),
        "deep ordering should break equal-depth ties descending");
}

OPENSCP_TEST(testLocalRenameUsesTheFilesystemMove, test) {
    QTemporaryDir temporary;
    test.check(temporary.isValid(),
               "the local rename test should have a temporary directory");
    if (!temporary.isValid())
        return;

    const QString source = temporary.filePath(QStringLiteral("source.txt"));
    const QString destination =
        temporary.filePath(QStringLiteral("destination.txt"));
    QFile sourceFile(source);
    test.check(sourceFile.open(QIODevice::WriteOnly) &&
                   sourceFile.write("payload") == 7,
               "the local rename fixture should be writable");
    sourceFile.close();

    QString error;
    test.check(renameLocalEntry(source, destination, &error) ==
                       LocalRenameResult::Moved &&
                   !QFile::exists(source) && QFile::exists(destination),
               "a same-volume rename should move the entry without copying");

    QFile movedFile(destination);
    test.check(movedFile.open(QIODevice::ReadOnly) &&
                   movedFile.readAll() == QByteArrayLiteral("payload"),
               "a native rename should preserve the file contents");

    const QString missingParentTarget =
        temporary.filePath(QStringLiteral("missing/target.txt"));
    error.clear();
    test.check(renameLocalEntry(destination, missingParentTarget, &error) ==
                       LocalRenameResult::Failed &&
                   QFile::exists(destination) && !error.isEmpty(),
               "non-volume rename failures should preserve the source and "
               "report an error");
}

OPENSCP_TEST(testLocalRemoveKeepsLinkTargets, test) {
    QTemporaryDir temporary;
    test.check(temporary.isValid(),
               "the local remove test should have a temporary directory");
    if (!temporary.isValid())
        return;

    const QDir root(temporary.path());
    const QString target = root.filePath(QStringLiteral("target"));
    const QString targetFile = root.filePath(QStringLiteral("target/data.txt"));
    test.check(root.mkpath(QStringLiteral("target/sub")),
               "the local remove fixture should create a folder tree");
    QFile data(targetFile);
    test.check(data.open(QIODevice::WriteOnly) && data.write("payload") == 7,
               "the local remove fixture should be writable");
    data.close();

    const QString link = root.filePath(QStringLiteral("link"));
    test.check(openscp::testsupport::createLocalLink(target, link),
               "the local remove fixture should link to the folder");
    test.check(removeLocalEntry(link) && !QFileInfo::exists(link) &&
                   !QFileInfo(link).isSymLink(),
               "removing a folder link should remove the link itself");
    test.check(QFile::exists(targetFile) &&
                   QDir(root.filePath(QStringLiteral("target/sub"))).exists(),
               "removing a folder link should keep the linked folder intact");

    const QString dangling = root.filePath(QStringLiteral("dangling"));
    if (openscp::testsupport::createLocalLink(
            root.filePath(QStringLiteral("missing")), dangling)) {
        test.check(removeLocalEntry(dangling) &&
                       !QFileInfo(dangling).isSymLink(),
                   "removing a dangling link should remove the link");
    } else {
        test.skip("removing a dangling link needs symbolic links");
    }

    test.check(removeLocalEntry(target) && !QFileInfo::exists(target),
               "removing a folder should remove its whole tree");
    test.check(removeLocalEntry(target),
               "removing a missing entry should count as removed");
}

std::filesystem::path nativePath(const QString &path) {
    return openscp::testsupport::nativeTestPath(path);
}

bool writeLocalFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(contents) == contents.size();
}

QByteArray readLocalFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool createRelativeLink(const QString &linkPath, const char *target) {
    std::error_code error;
    std::filesystem::create_symlink(target, nativePath(linkPath), error);
    return !error;
}

QString linkText(const QString &linkPath) {
    std::error_code error;
    const std::filesystem::path target =
        std::filesystem::read_symlink(nativePath(linkPath), error);
    return error ? QString() : QString::fromStdString(target.string());
}

OPENSCP_TEST(testLocalCopyKeepsHiddenEntriesAndLinks, test) {
    QTemporaryDir temporary;
    test.check(temporary.isValid(),
               "the local copy test should have a temporary directory");
    if (!temporary.isValid())
        return;

    const QDir root(temporary.path());
    const QString source = root.filePath(QStringLiteral("source"));
    const QString copy = root.filePath(QStringLiteral("copy"));
    test.check(root.mkpath(QStringLiteral("source/.git")) &&
                   root.mkpath(QStringLiteral("source/sub")),
               "the local copy fixture should create its folders");
    test.check(
        writeLocalFile(source + QStringLiteral("/.env"), "hidden") &&
            writeLocalFile(source + QStringLiteral("/.git/config"), "git") &&
            writeLocalFile(source + QStringLiteral("/sub/inner.txt"), "inner"),
        "the local copy fixture should write its files");
    const bool linked =
        createRelativeLink(source + QStringLiteral("/folder-link"), "sub") &&
        createRelativeLink(source + QStringLiteral("/sub/file-link"),
                           "inner.txt") &&
        createRelativeLink(source + QStringLiteral("/dangling"), "missing");
#ifdef Q_OS_WIN
    // Only administrators, or Developer Mode, may make symbolic links.
    if (!linked) {
        test.skip("copying links needs symbolic links");
        return;
    }
#endif
    test.check(linked, "the local copy fixture should create relative links");

    QString error;
    test.check(copyLocalEntry(source, copy, error) && error.isEmpty(),
               "copying a folder with hidden entries and links should work");
    test.check(readLocalFile(copy + QStringLiteral("/.env")) == "hidden" &&
                   readLocalFile(copy + QStringLiteral("/.git/config")) ==
                       "git",
               "a folder copy should include hidden files and folders");
    test.check(QFileInfo(copy + QStringLiteral("/folder-link")).isSymLink() &&
                   linkText(copy + QStringLiteral("/folder-link")) ==
                       QStringLiteral("sub"),
               "a folder link should be copied as the same relative link");
    test.check(QFileInfo(copy + QStringLiteral("/sub/file-link")).isSymLink() &&
                   linkText(copy + QStringLiteral("/sub/file-link")) ==
                       QStringLiteral("inner.txt"),
               "a file link should be copied as a link, not as a file");
    test.check(QFileInfo(copy + QStringLiteral("/dangling")).isSymLink(),
               "a dangling link should be copied too");
    test.check(readLocalFile(copy + QStringLiteral("/sub/inner.txt")) ==
                   "inner",
               "regular files should be copied with their contents");

    const QString linkCopy = root.filePath(QStringLiteral("link-copy"));
    error.clear();
    test.check(copyLocalEntry(source + QStringLiteral("/folder-link"), linkCopy,
                              error) &&
                   QFileInfo(linkCopy).isSymLink(),
               "copying a selected link should copy the link itself");
}

#ifdef Q_OS_WIN
OPENSCP_TEST(testWindowsLocalCopyKeepsShortcutsAsFiles, test) {
    // Qt's isDir() and isFile() describe the entry a shortcut leads to, but
    // the shortcut is an ordinary file, which Explorer copies as it is.
    QTemporaryDir temporary;
    const QDir root(temporary.path());
    const QString source = root.filePath(QStringLiteral("source"));
    const QString target = root.filePath(QStringLiteral("target"));
    test.check(
        root.mkpath(QStringLiteral("source")) &&
            root.mkpath(QStringLiteral("target")) &&
            writeLocalFile(target + QStringLiteral("/inside.txt"), "inside") &&
            QFile::link(target, source + QStringLiteral("/folder.lnk")) &&
            QFile::link(target + QStringLiteral("/inside.txt"),
                        source + QStringLiteral("/file.lnk")) &&
            QFile::link(target + QStringLiteral("/missing.txt"),
                        source + QStringLiteral("/broken.lnk")),
        "the shortcut fixture should be created");

    const QString copy = root.filePath(QStringLiteral("copy"));
    QString error;
    const bool copied = copyLocalEntry(source, copy, error);
    test.check(copied, "a folder with shortcuts should be copied: " +
                           error.toStdString());
    for (const char *name : {"folder.lnk", "file.lnk", "broken.lnk"}) {
        const QString shortcut = QStringLiteral("/") + QLatin1String(name);
        test.check(QFileInfo(copy + shortcut).isShortcut() &&
                       readLocalFile(copy + shortcut) ==
                           readLocalFile(source + shortcut),
                   std::string("a shortcut should be copied as itself: ") +
                       name);
    }

    // Not a copy of the folder it leads to, so not one into itself either.
    const QString back = target + QStringLiteral("/back.lnk");
    error.clear();
    const bool copiedBack =
        copyLocalEntry(source + QStringLiteral("/folder.lnk"), back, error);
    test.check(copiedBack && QFileInfo(back).isShortcut(),
               "a folder shortcut should be copied into its own folder: " +
                   error.toStdString());
}
#endif

OPENSCP_TEST(testLocalCopyStopsWhenCanceled, test) {
    QTemporaryDir root;
    const QString source = root.filePath(QStringLiteral("source"));
    const QString destination = root.filePath(QStringLiteral("destination"));
    test.check(QDir().mkpath(source + QStringLiteral("/nested")),
               "the copy source should be created");
    for (int index = 0; index < 5; ++index) {
        QFile file(QDir(source).filePath(QStringLiteral("file-%1").arg(index)));
        test.check(file.open(QIODevice::WriteOnly) && file.write("x") == 1,
                   "a source file should be written");
    }
    QFile existing(destination);
    test.check(existing.open(QIODevice::WriteOnly) &&
                   existing.write("keep") == 4,
               "the existing destination should be written");
    existing.close();

    int asked = 0;
    QString error;
    test.check(!copyLocalEntry(source, destination, error,
                               [&asked] { return ++asked > 3; }) &&
                   !error.isEmpty(),
               "a canceled copy should fail");
    QFile kept(destination);
    test.check(kept.open(QIODevice::ReadOnly) && kept.readAll() == "keep",
               "a canceled copy should leave the destination as it was");
    const QStringList leftovers =
        QDir(root.path())
            .entryList({QStringLiteral(".openscp-*")},
                       QDir::AllEntries | QDir::Hidden);
    test.check(leftovers.isEmpty(),
               "a canceled copy should remove what it staged: " +
                   leftovers.join(QLatin1Char(',')).toStdString());
}

OPENSCP_TEST(testLocalCopyRejectsUnsafeSources, test) {
    QTemporaryDir temporary;
    test.check(temporary.isValid(),
               "the unsafe copy test should have a temporary directory");
    if (!temporary.isValid())
        return;

    const QDir root(temporary.path());
    const QString source = root.filePath(QStringLiteral("source"));
    test.check(root.mkpath(QStringLiteral("source/sub")) &&
                   writeLocalFile(source + QStringLiteral("/data.txt"), "data"),
               "the unsafe copy fixture should create a folder tree");

    QString error;
    const QString nested = source + QStringLiteral("/sub/copy");
    test.check(!copyLocalEntry(source, nested, error) && !error.isEmpty(),
               "copying a folder into itself should fail");
    test.check(!QFileInfo::exists(nested) &&
                   QDir(source + QStringLiteral("/sub"))
                       .entryList(QDir::AllEntries | QDir::Hidden |
                                  QDir::System | QDir::NoDotAndDotDot)
                       .isEmpty(),
               "a rejected copy into itself should leave nothing behind");

#ifndef _WIN32
    const QString fifoSource = root.filePath(QStringLiteral("fifo-source"));
    const QString fifoCopy = root.filePath(QStringLiteral("fifo-copy"));
    const QString fifo = fifoSource + QStringLiteral("/pipe");
    test.check(root.mkpath(QStringLiteral("fifo-source")) &&
                   ::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0,
               "the unsafe copy fixture should create a FIFO");

    // A regression must fail instead of hanging the test worker.
    struct AlarmGuard {
        ~AlarmGuard() { ::alarm(0); }
    } alarmGuard;
    ::alarm(5);
    error.clear();
    test.check(!copyLocalEntry(fifoSource, fifoCopy, error) &&
                   !error.isEmpty() && !QFileInfo::exists(fifoCopy),
               "copying a folder with a FIFO should fail without blocking");
    test.check(QFileInfo(fifo).exists(),
               "a failed copy should keep the source FIFO");
#endif
}

OPENSCP_TEST(testStagingBatchNames, test) {
    const QString name = openscpui::newStagingBatchName();
    test.check(openscpui::isStagingBatchName(name),
               "the startup cleanup should recognize new staging batches: " +
                   name.toStdString());
    test.check(openscpui::isStagingBatchName(QStringLiteral("20260101-120000")),
               "batches named by older releases should still be recognized");
    test.check(!openscpui::isStagingBatchName(QStringLiteral("Documents")) &&
                   !openscpui::isStagingBatchName(
                       QStringLiteral("20260101-120000-123")),
               "other folders in the staging root should be left alone");
}

OPENSCP_TEST(testStagingRootFollowsTheDownloadsFolder, test) {
    const QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    test.check(
        downloads.isEmpty() ||
            openscpui::defaultStagingRootPath() ==
                QDir(downloads).filePath(QStringLiteral("OpenSCP-Dragged")),
        "the default staging root should be in the Downloads folder");
#ifdef Q_OS_LINUX
    // XDG can name that folder in the user's language; releases before 1.3.0
    // used ~/Downloads anyway.
    QTemporaryDir config;
    test.check(config.isValid(), "the XDG test needs a folder");
    if (!config.isValid())
        return;
    QFile userDirs(
        QDir(config.path()).filePath(QStringLiteral("user-dirs.dirs")));
    test.check(userDirs.open(QIODevice::WriteOnly) &&
                   userDirs.write("XDG_DOWNLOAD_DIR=\"$HOME/Descargas\"\n") > 0,
               "the XDG folder names should be written");
    userDirs.close();
    const bool hadConfigHome = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const QByteArray configHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(config.path()));
    const QDir home(QDir::homePath());
    test.check(openscpui::defaultStagingRootPath() ==
                   home.filePath(QStringLiteral("Descargas/OpenSCP-Dragged")),
               "the default staging root should follow the XDG Downloads "
               "folder");
    QSettings unset(QDir(config.path()).filePath(QStringLiteral("unset.ini")),
                    QSettings::IniFormat);
    test.check(openscpui::stagingRootsForStartupCleanup(unset).contains(
                   home.filePath(QStringLiteral("Downloads/OpenSCP-Dragged"))),
               "the startup cleanup should still go through the previous "
               "default root");
    QSettings chosen(QDir(config.path()).filePath(QStringLiteral("chosen.ini")),
                     QSettings::IniFormat);
    chosen.setValue(openscpui::settingskeys::kStagingRoot, config.path());
    test.check(openscpui::stagingRootsForStartupCleanup(chosen) ==
                   QStringList{config.path()},
               "a chosen staging root should be the only one cleaned");
    if (hadConfigHome)
        qputenv("XDG_CONFIG_HOME", configHome);
    else
        qunsetenv("XDG_CONFIG_HOME");
#endif
}

OPENSCP_TEST(testStagingBatchRemovalTakesAnEmptyRoot, test) {
    QTemporaryDir directory;
    test.check(directory.isValid(), "the staging test needs a folder");
    if (!directory.isValid())
        return;
    const QString root =
        QDir(directory.path()).filePath(QStringLiteral("staging"));
    const QString first = QDir(root).filePath(openscpui::newStagingBatchName());
    const QString second =
        QDir(root).filePath(QStringLiteral("20260101-120000"));
    QFile payload(QDir(first).filePath(QStringLiteral("payload.txt")));
    test.check(QDir().mkpath(first) && QDir().mkpath(second) &&
                   payload.open(QIODevice::WriteOnly),
               "the staging batches should be created");
    payload.close();

    openscpui::removeStagingBatch(first);
    test.check(!QFileInfo::exists(first) && QFileInfo::exists(root),
               "a root with another batch should stay");
    openscpui::removeStagingBatch(second);
    test.check(!QFileInfo::exists(root),
               "a root left empty should be removed with its last batch");
}

OPENSCP_TEST(testExpiredStagingBatchesAreRemoved, test) {
    QTemporaryDir directory;
    test.check(directory.isValid(), "the staging test needs a folder");
    if (!directory.isValid())
        return;
    const QDir base(directory.path());
    const QString root = base.filePath(QStringLiteral("staging"));
    const QString oldBatch =
        QDir(root).filePath(QStringLiteral("20200101-120000"));
    const QString newBatch =
        QDir(root).filePath(openscpui::newStagingBatchName());
    const QString other = QDir(root).filePath(QStringLiteral("Documents"));
    const QString outside = base.filePath(QStringLiteral("outside"));
    test.check(QDir().mkpath(oldBatch) && QDir().mkpath(newBatch) &&
                   QDir().mkpath(other) && QDir().mkpath(outside),
               "the staging folders should be created");
    const auto age = [](const QString &path) {
        std::error_code error;
        std::filesystem::last_write_time(
            QFile::encodeName(path).toStdString(),
            std::filesystem::file_time_type::clock::now() -
                std::chrono::hours(24 * 30),
            error);
        return !error;
    };
    test.check(age(oldBatch) && age(other) && age(outside),
               "the old folders should be dated back");
    const QString link = QDir(root).filePath(QStringLiteral("20200101-130000"));
    test.check(openscp::testsupport::createLocalLink(outside, link),
               "a batch-named link should exist");

    const int removed = openscpui::removeStagingBatchesOlderThan(
        root, QDateTime::currentDateTimeUtc().addDays(-7));
    test.check(removed == 1 && !QFileInfo::exists(oldBatch) &&
                   QFileInfo::exists(newBatch) && QFileInfo::exists(other) &&
                   QFileInfo::exists(outside),
               "only batches older than the retention should be removed");
}

OPENSCP_TEST(testOnlyStagingBatchesAreCleanedAfterADrop, test) {
    QTemporaryDir directory;
    test.check(directory.isValid(), "the staging test needs a folder");
    if (!directory.isValid())
        return;
    const QDir base(directory.path());
    const QString root = base.filePath(QStringLiteral("staging"));
    const QString batch = QDir(root).filePath(openscpui::newStagingBatchName());
    const QString outside = base.filePath(QStringLiteral("20260101-120000"));
    test.check(
        QDir().mkpath(batch) && QDir().mkpath(outside) &&
            QDir().mkpath(QDir(root).filePath(QStringLiteral("Documents"))) &&
            QDir().mkpath(QDir(batch).filePath(QStringLiteral("nested"))),
        "the staging fixture should be created");
    QSettings settings(base.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(openscpui::settingskeys::kStagingRoot, root);

    test.check(openscpui::stagingBatchPathForCleanup(settings, batch) ==
                   QFileInfo(batch).canonicalFilePath(),
               "a batch in the staging root should be cleaned");
    const QStringList refused = {
        QString(),
        root,
        outside,
        QDir(root).filePath(QStringLiteral("Documents")),
        QDir(batch).filePath(QStringLiteral("nested")),
        QDir(root).filePath(QStringLiteral("../20260101-120000")),
        QDir(root).filePath(QStringLiteral("20991231-235959")),
    };
    for (const QString &path : refused) {
        test.check(
            openscpui::stagingBatchPathForCleanup(settings, path).isEmpty(),
            "only a batch directly in the staging root may be cleaned: " +
                path.toStdString());
    }
    const QString link = QDir(root).filePath(QStringLiteral("20260101-130000"));
    test.check(
        openscp::testsupport::createLocalLink(outside, link) &&
            openscpui::stagingBatchPathForCleanup(settings, link).isEmpty(),
        "a link named like a batch should not be followed");
}

OPENSCP_TEST(testSettingsReadOnlyTheUsersOwnFile, test) {
    // With IniFormat, QSettings would read a key missing from the user's file
    // from the organization-wide file and from the machine-wide one, which on
    // Windows sits under C:\ProgramData, where any user may write.
    QTemporaryDir user;
    QTemporaryDir machine;
    test.check(user.isValid() && machine.isValid(),
               "the settings folders should be created");
    if (!user.isValid() || !machine.isValid())
        return;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, user.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       machine.path());

    QString organization;
    QString application;
    {
        const openscpui::AppSettings store;
        organization = store.organizationName();
        application = store.applicationName();
    }
    const QString planted = QStringLiteral("/planted/by/another/account");
    {
        QSettings machineWide(QSettings::IniFormat, QSettings::SystemScope,
                              organization, application);
        machineWide.setValue(openscpui::settingskeys::kStagingRoot, planted);
        QSettings organizationWide(QSettings::IniFormat, QSettings::UserScope,
                                   organization);
        organizationWide.setValue(
            openscpui::settingskeys::kDefaultKnownHostsPolicy,
            QStringLiteral("off"));
    }

    const openscpui::AppSettings store;
    test.check(!store.contains(openscpui::settingskeys::kStagingRoot) &&
                   openscpui::effectiveStagingRootPath(store) != planted,
               "a machine-wide file must not supply the user's settings");
    test.check(
        !store.contains(openscpui::settingskeys::kDefaultKnownHostsPolicy),
        "an organization-wide file must not supply the user's settings");
    const openscpui::AppSettings named(organization, application);
    test.check(!named.contains(openscpui::settingskeys::kStagingRoot),
               "settings opened by name should read only the user's file");
}

OPENSCP_TEST(testLocalEntryNamesFollowThisComputer, test) {
    test.check(isValidLocalEntryName(QStringLiteral("report.txt")),
               "an ordinary name should be a valid local name");
    test.check(!isValidLocalEntryName(QStringLiteral("..")),
               "a relative reference is never a local name");
#ifdef Q_OS_WIN
    // REPORT~1.TXT and DOCUME~1 have the form of the short names Windows
    // makes for long ones, which open the long-named entry instead.
    for (const char *name :
         {"a:b", "CON", "nul.txt", "COM1.log", "trailing.", "trailing ",
          "what?", "pipe|name", "REPORT~1.TXT", "docume~1", "RE3A5F~1.TXT"}) {
        QString why;
        test.check(!isValidLocalEntryName(QString::fromUtf8(name), &why) &&
                       !why.isEmpty(),
                   std::string("Windows should refuse the local name ") + name);
    }
    // No short name Windows makes has a space, a longer base or extension,
    // or no number after the tilde.
    for (const char *name : {"README.TXT", "a b~1.txt", "FILE~1.HTML",
                             "notes~2026.txt", "draft.txt~", "~$draft.docx"}) {
        test.check(isValidLocalEntryName(QString::fromUtf8(name)),
                   std::string("Windows should accept the local name ") + name);
    }
#else
    test.check(isValidLocalEntryName(QStringLiteral("a:b")) &&
                   isValidLocalEntryName(QStringLiteral("CON")) &&
                   isValidLocalEntryName(QStringLiteral("trailing.")) &&
                   isValidLocalEntryName(QStringLiteral("REPORT~1.TXT")),
               "POSIX should accept the names only Windows refuses");
#endif
}

OPENSCP_TEST(testTerminalTransferStatuses, test) {
    using Status = TransferTask::Status;
    for (const Status status : {Status::Done, Status::Error, Status::Canceled,
                                Status::Skipped, Status::Warning}) {
        test.check(isTerminalTransferStatus(status),
                   "every completed outcome should be terminal");
    }
    for (const Status status :
         {Status::Queued, Status::Running, Status::Paused,
          Status::WaitingForConnection, Status::RetryWaiting}) {
        test.check(!isTerminalTransferStatus(status),
                   "an actionable transfer state should not be terminal");
    }
}

void fillModel(RemoteModel &model, int rows) {
    std::vector<openscp::FileInfo> entries;
    entries.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        openscp::FileInfo info;
        info.name = "entry-" + std::to_string(row);
        info.has_size = true;
        info.mode = 0100644u;
        entries.push_back(info);
    }
    model.setEntries(QStringLiteral("/"), entries);
}

OPENSCP_TEST(testRowSelectionMergesConsecutiveRows, test) {
    RemoteModel model;
    fillModel(model, 10);
    const QItemSelection selection =
        openscpui::rowSelection(model, {}, {4, 1, 2, 3, 7, 1});
    test.check(selection.size() == 2,
               "consecutive rows should become one range each");
    if (selection.size() != 2)
        return;
    test.check(selection[0].top() == 1 && selection[0].bottom() == 4 &&
                   selection[1].top() == 7 && selection[1].bottom() == 7,
               "ranges should cover the rows that were asked for, in order");
    test.check(selection[0].left() == 0 &&
                   selection[0].right() == model.columnCount() - 1,
               "each range should cover the whole row");
    QVector<int> selectedRows;
    for (const QModelIndex &index : selection.indexes()) {
        if (index.column() == 0)
            selectedRows.push_back(index.row());
    }
    std::sort(selectedRows.begin(), selectedRows.end());
    test.check(selectedRows == QVector<int>({1, 2, 3, 4, 7}),
               "the selection should hold every requested row once");
}

OPENSCP_TEST(testRowSelectionIgnoresRowsOutsideTheModel, test) {
    RemoteModel model;
    fillModel(model, 3);
    test.check(openscpui::rowSelection(model, {}, {}).isEmpty(),
               "no rows should give an empty selection");
    const QItemSelection selection =
        openscpui::rowSelection(model, {}, {-1, 0, 1, 3, 99});
    test.check(selection.size() == 1 && selection[0].top() == 0 &&
                   selection[0].bottom() == 1,
               "rows outside the model should be left out");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("UI utility");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
