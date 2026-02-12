// Application entry point: initialize Qt and show MainWindow.
#include <QApplication>
#include <QSettings>
#include <QTranslator>
#include <QLocale>
#include <QLibraryInfo>
#include <QDir>
#include <QFile>
#include "MainWindow.hpp"
#include "AppVersion.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("OpenSCP");
    QCoreApplication::setOrganizationName("OpenSCP");
    QCoreApplication::setApplicationVersion(QStringLiteral(OPEN_SCP_APP_VERSION));

    // Theme: use system default (no overrides)

    // Load translation if available (supports resources and disk)
    QSettings s("OpenSCP", "OpenSCP");
    const QString lang = s.value("UI/language", "en").toString().trimmed().toLower();
    static QTranslator translator; // static so it lives until app.exec()
    const QString base = QString("openscp_%1").arg(lang);
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString transDir1 = QDir(exeDir).filePath("translations");
    const QString transDir2 = QDir(QCoreApplication::applicationDirPath()).absolutePath();
    const QString resPath = ":/i18n/" + base + ".qm";
    // English is the source language: no app translator is needed for "en".
    if (lang != QStringLiteral("en")) {
        if (QFile::exists(resPath) ? translator.load(resPath)
                                   : (translator.load(base, transDir1) || translator.load(base, transDir2))) {
            app.installTranslator(&translator);
        }
    }
    static QTranslator qtTranslator;
    const QString qtBaseName = QString("qtbase_%1").arg(lang);
    const QString qtTransPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qtTranslator.load(qtBaseName, qtTransPath)) {
        app.installTranslator(&qtTranslator);
    }
    MainWindow w;
    w.show();
    return app.exec();
}
