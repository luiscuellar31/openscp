// Implementation of the "About" dialog for OpenSCP.
#include "AboutDialog.hpp"
#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTextEdit>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>
#include <QCoreApplication>
#include <QSettings>
#include <QStringList>
#include <QStringConverter>

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Acerca de OpenSCP"));

    auto* lay = new QVBoxLayout(this);

    auto* title = new QLabel(QString("<b>OpenSCP v%1</b>").arg(QCoreApplication::applicationVersion()), this);
    title->setTextFormat(Qt::RichText);
    lay->addWidget(title);

    auto* author = new QLabel(tr("Autor: <a href=\"https://github.com/luiscuellar31\">luiscuellar31</a>"), this);
    author->setTextFormat(Qt::RichText);
    author->setOpenExternalLinks(true);
    lay->addWidget(author);

    auto* libsTitle = new QLabel(tr("Librerías utilizadas:"), this);
    lay->addWidget(libsTitle);

    // Scrollable text area to show long credits/licenses text loaded from docs/*.txt
    auto* libsText = new QTextEdit(this);
    libsText->setReadOnly(true);
    libsText->setLineWrapMode(QTextEdit::WidgetWidth);
    libsText->setMinimumHeight(220);

    // Try to locate the text file under a 'docs' directory near the app/build tree
    auto findDocsFile = [](const QStringList& fileNames) -> QString {
        const QStringList bases = {
            QDir::currentPath(),
            QCoreApplication::applicationDirPath(),
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."),
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../Resources") // macOS bundle Resources
        };
        for (const QString& base : bases) {
            QDir dir(base);
            for (int i = 0; i < 5; ++i) { // search up to 5 levels up
                for (const QString& fn : fileNames) {
                    const QString candidate = dir.filePath(QStringLiteral("docs/%1").arg(fn));
                    if (QFileInfo::exists(candidate)) return candidate;
                }
                if (!dir.cdUp()) break;
            }
        }
        return {};
    };

    // Decide which file to load based on UI language (settings: UI/language)
    QSettings s("OpenSCP", "OpenSCP");
    const QString lang = s.value("UI/language", "es").toString().toLower();
    const QString suffix = lang.startsWith("en") ? QStringLiteral("EN") : QStringLiteral("ES");

    const QStringList candidates = {
        QStringLiteral("ABOUT_LIBRARIES_%1.txt").arg(suffix),
        QStringLiteral("ABOUT_LIBRARIES_%1").arg(suffix),
        QStringLiteral("ABOUT_LIBRARIES.txt"),
        QStringLiteral("ABOUT_LIBRARIES")
    };
    const QString path = findDocsFile(candidates);
    QString content;
    if (!path.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            content = ts.readAll();
        }
    }
    if (content.isEmpty()) {
        const QString expected = QStringLiteral("ABOUT_LIBRARIES_%1(.txt)").arg(suffix);
        content = tr(
            "No se encontró el archivo de créditos en docs/%1.\n"
            "Crea ese archivo y añade las librerías/licencias a listar."
        ).arg(expected);
    }
    libsText->setPlainText(content);
    lay->addWidget(libsText);

    // Report an issue link at the bottom (opens Issues page)
    {
        const QString linkText = tr("Informar de un error");
        auto* report = new QLabel(QString("<a href=\"https://github.com/luiscuellar31/openscp/issues\">%1</a>").arg(linkText), this);
        report->setTextFormat(Qt::RichText);
        report->setOpenExternalLinks(true);
        report->setWordWrap(true);
        lay->addWidget(report);
    }

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(btns, &QDialogButtonBox::rejected, this, &AboutDialog::reject);
    connect(btns, &QDialogButtonBox::accepted, this, &AboutDialog::accept);
    lay->addWidget(btns);
}
