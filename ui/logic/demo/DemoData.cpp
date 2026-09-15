#include "logic/demo/DemoData.hpp"

#include "logic/navigation/NavigationScope.hpp"
#include "logic/navigation/NavigationStore.hpp"
#include "logic/persistence/SavedSitesPersistence.hpp"
#include "openscp/SessionOptions.hpp"

#include <QDir>
#include <QStringList>

namespace openscpui::demo {
namespace {

// Hosts use the reserved .example domain and the accounts are generic on
// purpose: these end up in screenshots published on stores and in the README.
SiteEntry makeSite(const QString &name, const char *host, const char *user,
                   openscp::Protocol protocol, const QString &remotePath) {
    SiteEntry site;
    site.siteId = SavedSitesPersistence::createSiteId();
    site.name = name;
    site.opt.protocol = protocol;
    site.opt.host = host;
    site.opt.port = 22;
    site.opt.username = user;
    site.initialRemotePath = remotePath;
    site.rememberLastPaths = true;
    return site;
}

QVector<SiteEntry> sampleSites() {
    return {
        makeSite(QStringLiteral("Staging server"), "staging.example", "deploy",
                 openscp::Protocol::Sftp,
                 QStringLiteral("/home/demo/projects")),
        makeSite(QStringLiteral("Build archive"), "builds.example", "ci",
                 openscp::Protocol::Sftp, QStringLiteral("/var/log")),
        makeSite(QStringLiteral("Media library"), "media.example", "editor",
                 openscp::Protocol::Sftp, QStringLiteral("/home/demo")),
        makeSite(QStringLiteral("Backup host"), "backup.example", "operator",
                 openscp::Protocol::Scp, QStringLiteral("/")),
    };
}

} // namespace

void seedSampleDataIfEmpty() {
    const QVector<SiteEntry> sites = sampleSites();
    if (SavedSitesPersistence::loadSites().sites.isEmpty())
        SavedSitesPersistence::saveSites(sites);

    NavigationStore navigation;
    if (navigation.recentServers().isEmpty()) {
        // Seeded oldest first: each entry is prepended, so the list reads in
        // the order a person would have visited them.
        for (auto site = sites.crbegin(); site != sites.crend(); ++site)
            navigation.addRecentServer(site->opt);
    }

    if (navigation.recentLocalPaths().isEmpty()) {
        const QString home = QDir::homePath();
        for (const QString &relative :
             {QStringLiteral("Documents"), QStringLiteral("Downloads"),
              QStringLiteral("Pictures")}) {
            navigation.addRecentLocalPath(QDir(home).filePath(relative));
        }
    }

    // Connecting through a saved site scopes its history by site id, so seed
    // the same scope the application will read back.
    for (const SiteEntry &site : sites) {
        const QString scope = savedSiteNavigationScope(site.siteId);
        if (scope.isEmpty() || !navigation.recentRemotePaths(scope).isEmpty())
            continue;
        navigation.addRecentRemotePath(scope, QStringLiteral("/"));
        navigation.addRecentRemotePath(scope, site.initialRemotePath);
    }
}

} // namespace openscpui::demo
