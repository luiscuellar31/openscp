// Application entry point: initialize Qt and show MainWindow.
#include "AppVersion.hpp"
#include "MainWindow.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("OpenSCP");
    QCoreApplication::setOrganizationName("OpenSCP");
    QCoreApplication::setApplicationVersion(
        QStringLiteral(OPENSCP_APP_VERSION));

    // Theme: use system default (no overrides)

    // Load translation if available (supports resources and disk)
    QSettings settings("OpenSCP", "OpenSCP");
    const QString languageCode = settings.value("UI/language", "en")
                                     .toString()
                                     .trimmed()
                                     .toLower();
    static QTranslator translator; // static so it lives until app.exec()
    const QString translationBaseName = QString("openscp_%1").arg(languageCode);
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString transDir1 = QDir(exeDir).filePath("translations");
    const QString transDir2 =
        QDir(QCoreApplication::applicationDirPath()).absolutePath();
    const QString resourceTranslationPath = ":/i18n/" + translationBaseName + ".qm";
    // English is the source language: no app translator is needed for "en".
    if (languageCode != QStringLiteral("en")) {
        if (QFile::exists(resourceTranslationPath)
                ? translator.load(resourceTranslationPath)
                : (translator.load(translationBaseName, transDir1) ||
                   translator.load(translationBaseName, transDir2))) {
            app.installTranslator(&translator);
        }
    }
    static QTranslator qtTranslator;
    const QString qtBaseName = QString("qtbase_%1").arg(languageCode);
    const QString qtTransPath =
        QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qtTranslator.load(qtBaseName, qtTransPath)) {
        app.installTranslator(&qtTranslator);
    }
    MainWindow w;
    w.show();
    return app.exec();
}
