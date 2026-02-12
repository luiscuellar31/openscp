// Implementation of the "About" dialog for OpenSCP.
#include "AboutDialog.hpp"
#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTextEdit>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>
#include <QCoreApplication>
#include <QSettings>
#include <QStringList>
#include <QStringConverter>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QPixmap>

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("About OpenSCP"));
    // Layout-driven sizing: we'll compute the minimum size after creating widgets

    auto* lay = new QVBoxLayout(this);

    // Top row: title/author on the left, app icon on the right
    auto* topRow = new QHBoxLayout();
    auto* leftCol = new QVBoxLayout();

    auto* title = new QLabel(QString("<b>OpenSCP v%1</b>").arg(QCoreApplication::applicationVersion()), this);
    title->setTextFormat(Qt::RichText);
    leftCol->addWidget(title);

    auto* author = new QLabel(tr("Author: <a href=\"https://github.com/luiscuellar31\">luiscuellar31</a>"), this);
    author->setTextFormat(Qt::RichText);
    author->setOpenExternalLinks(true);
    leftCol->addWidget(author);

    leftCol->addStretch(1);
    topRow->addLayout(leftCol, 1);

    auto* iconLabel = new QLabel(this);
    QPixmap iconPix(QStringLiteral(":/assets/program/icon-openscp-2048.png"));
    if (iconPix.isNull()) {
        // Fallback: try to find the PNG on disk in dev environments
        const QStringList bases = {
            QDir::currentPath(),
            QCoreApplication::applicationDirPath(),
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."),
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../Resources")
        };
        for (const QString& base : bases) {
            QDir dir(base);
            for (int i = 0; i < 5; ++i) {
                const QString candidate = dir.filePath(QStringLiteral("assets/program/icon-openscp-2048.png"));
                if (QFileInfo::exists(candidate)) { iconPix.load(candidate); break; }
                if (!dir.cdUp()) break;
            }
            if (!iconPix.isNull()) break;
        }
    }
    if (!iconPix.isNull()) {
        iconLabel->setPixmap(iconPix.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    iconLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    topRow->addWidget(iconLabel, 0);

    lay->addLayout(topRow);

    auto* libsTitle = new QLabel(tr("Used libraries:"), this);
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
    const QString lang = s.value("UI/language", "en").toString().toLower();
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
            "Could not find the credits file in docs/%1.\n"
            "Create that file and list the libraries/licenses there."
        ).arg(expected);
    }
    libsText->setPlainText(content);
    lay->addWidget(libsText);

    // Button: open licenses folder (packaged alongside the app)
    auto* openLicensesBtn = new QPushButton(tr("Open Licenses Folder"), this);
    lay->addWidget(openLicensesBtn);
    connect(openLicensesBtn, &QPushButton::clicked, this, [this]{
        auto findLicensesDir = []() -> QString {
            const QStringList bases = {
                QDir::currentPath(),
                QCoreApplication::applicationDirPath(),
                QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."),
                QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../Resources") // macOS bundle Resources
            };
            const QStringList candidates = {
                QStringLiteral("docs/credits/LICENSES"),
                QStringLiteral("docs/licenses"),
                QStringLiteral("usr/share/licenses"),
                QStringLiteral("share/licenses"),
                QStringLiteral("LICENSES"),
                QStringLiteral("licenses"),
                QStringLiteral("Licenses"),
                QStringLiteral("Resources/licenses"),
                QStringLiteral("Resources/LICENSES")
            };
            for (const QString& base : bases) {
                for (const QString& rel : candidates) {
                    const QString p = QDir(base).filePath(rel);
                    if (QFileInfo::exists(p) && QFileInfo(p).isDir()) return QDir(p).absolutePath();
                }
            }
            return {};
        };
        const QString dir = findLicensesDir();
        if (!dir.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        } else {
            QMessageBox::information(this,
                                    tr("Licenses folder not found"),
                                    tr("Could not find the licenses folder. Ensure the package includes the license texts (e.g., inside the app or next to the AppImage)."));
        }
    });

    // Report an issue link at the bottom (opens Issues page)
    {
        const QString linkText = tr("Report an issue");
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

    // Enforce a dynamic minimum size so controls never overlap when shrinking
    lay->setSizeConstraint(QLayout::SetMinimumSize);
    const QSize layMin = lay->minimumSize();
    const int minW = qMax(700, layMin.width());
    const int minH = qMax(440, layMin.height());
    this->setMinimumSize(minW, minH);
    // Start at a comfortable size, but never below min
    this->resize(qMax(680, minW), qMax(520, minH));
}
