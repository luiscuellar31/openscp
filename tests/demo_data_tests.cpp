#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/demo/DemoData.hpp"
#include "logic/persistence/SavedSitesPersistence.hpp"

#include <QCoreApplication>
#include <QSettings>

#include <iostream>

namespace {

// NavigationStore() keeps history in the platform's native store for the
// application's name, apart from the isolated settings the sites use.
QSettings &historyStore() {
    static QSettings store(QCoreApplication::organizationName(),
                           QCoreApplication::applicationName());
    return store;
}

void clearAll() {
    QSettings().clear();
    historyStore().clear();
    historyStore().sync();
}

QStringList remoteScopes() {
    historyStore().sync();
    historyStore().beginGroup(openscpui::settingskeys::kRemoteScopes);
    const QStringList scopes = historyStore().childGroups();
    historyStore().endGroup();
    return scopes;
}

OPENSCP_TEST(testSeedingTwiceAddsNoHistory, test) {
    clearAll();
    openscpui::demo::seedSampleDataIfEmpty();
    const QStringList seeded = remoteScopes();
    test.check(seeded.size() == 4 &&
                   SavedSitesPersistence::loadSites().sites.size() == 4,
               "the first start should seed four sites and their history");

    // Each start makes new sample sites; their history would belong to ids
    // that were never saved.
    openscpui::demo::seedSampleDataIfEmpty();
    test.check(remoteScopes() == seeded,
               "a later start should not add history for unsaved sites");
}

OPENSCP_TEST(testExistingSitesGetNoSampleHistory, test) {
    clearAll();
    SiteEntry own;
    own.siteId = SavedSitesPersistence::createSiteId();
    own.name = QStringLiteral("Own site");
    own.opt.host = "own.example";
    own.opt.username = "me";
    test.check(SavedSitesPersistence::saveSites({own}).ok,
               "the user's site should be saved first");
    openscpui::demo::seedSampleDataIfEmpty();
    test.check(SavedSitesPersistence::loadSites().sites.size() == 1 &&
                   remoteScopes().isEmpty(),
               "sample history should only come with the sample sites");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("demo-data-tests"));

    openscp::testsupport::IsolatedSettings settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    openscp::test::TestHarness harness("demo data");
    const int result = harness.run();
    clearAll();
    return result;
}
