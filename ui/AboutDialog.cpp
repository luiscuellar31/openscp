// Implementation of the "About" dialog for OpenSCP.
#include "AboutDialog.hpp"
#include "AppVersion.hpp"
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStringConverter>
#include <QStringList>
#include <QSysInfo>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QStringList aboutSearchBases() {
    return {
        QDir::currentPath(), QCoreApplication::applicationDirPath(),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."),
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath("../Resources") // macOS bundle Resources
    };
}

QString findFromCandidates(const QStringList &relativeCandidates,
                           bool wantDirectory) {
    for (const QString &base : aboutSearchBases()) {
        QDir dir(base);
        for (int i = 0; i < 5; ++i) { // search up to 5 levels up
            for (const QString &rel : relativeCandidates) {
                const QString candidate = dir.filePath(rel);
                const QFileInfo info(candidate);
                if (!info.exists()) {
                    continue;
                }
                if (wantDirectory && !info.isDir()) {
                    continue;
                }
                if (!wantDirectory && !info.isFile()) {
                    continue;
                }
                return wantDirectory ? QDir(candidate).absolutePath()
                                     : info.absoluteFilePath();
            }
            if (!dir.cdUp()) {
                break;
            }
        }
    }
    return {};
}

QString findDocsFile(const QStringList &fileNames) {
    QStringList docsCandidates;
    docsCandidates.reserve(fileNames.size());
    for (const QString &fileName : fileNames) {
        docsCandidates << QStringLiteral("docs/%1").arg(fileName);
    }
    return findFromCandidates(docsCandidates, false);
}

QString findLicensesDir() {
    return findFromCandidates(
        {QStringLiteral("docs/credits/LICENSES"),
         QStringLiteral("docs/licenses"), QStringLiteral("usr/share/licenses"),
         QStringLiteral("share/licenses"), QStringLiteral("LICENSES"),
         QStringLiteral("licenses"), QStringLiteral("Licenses"),
         QStringLiteral("Resources/licenses"),
         QStringLiteral("Resources/LICENSES")},
        true);
}

QString buildDiagnosticsText() {
    return QStringLiteral("OpenSCP version: %1\n"
                          "Qt version: %2\n"
                          "OS: %3\n"
                          "CPU architecture: %4\n"
                          "Build type: %5\n"
                          "Git commit: %6\n"
                          "Repository: %7")
        .arg(QString::fromUtf8(OPEN_SCP_APP_VERSION),
             QString::fromUtf8(qVersion()), QSysInfo::prettyProductName(),
             QSysInfo::currentCpuArchitecture(),
             QString::fromUtf8(OPEN_SCP_BUILD_TYPE),
             QString::fromUtf8(OPEN_SCP_GIT_COMMIT),
             QString::fromUtf8(OPEN_SCP_REPOSITORY_URL));
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("About OpenSCP"));
    // Layout-driven sizing: we'll compute the minimum size after creating
    // widgets

    auto *lay = new QVBoxLayout(this);

    // Top row: title/author on the left, app icon on the right
    auto *topRow = new QHBoxLayout();
    auto *leftCol = new QVBoxLayout();

    auto *title = new QLabel(QString("<b>OpenSCP v%1</b>")
                                 .arg(QCoreApplication::applicationVersion()),
                             this);
    title->setTextFormat(Qt::RichText);
    leftCol->addWidget(title);

    const QString authorName = QString::fromUtf8(OPEN_SCP_AUTHOR_NAME);
    const QString authorUrl = QString::fromUtf8(OPEN_SCP_AUTHOR_URL);
    const QString issuesUrl = QString::fromUtf8(OPEN_SCP_ISSUES_URL);

    auto *author = new QLabel(
        tr("Author: <a href=\"%1\">%2</a>")
            .arg(authorUrl.toHtmlEscaped(), authorName.toHtmlEscaped()),
        this);
    author->setTextFormat(Qt::RichText);
    author->setOpenExternalLinks(true);
    leftCol->addWidget(author);

    leftCol->addStretch(1);
    topRow->addLayout(leftCol, 1);

    auto *iconLabel = new QLabel(this);
    QPixmap iconPix(QStringLiteral(":/assets/program/icon-openscp-2048.png"));
    if (iconPix.isNull()) {
        // Fallback for dev environments: search alongside source/build folders.
        const QString iconPath = findFromCandidates(
            {QStringLiteral("assets/program/icon-openscp-2048.png")}, false);
        if (!iconPath.isEmpty()) {
            iconPix.load(iconPath);
        }
    }
    if (!iconPix.isNull()) {
        iconLabel->setPixmap(iconPix.scaled(96, 96, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation));
    }
    iconLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    topRow->addWidget(iconLabel, 0);

    lay->addLayout(topRow);

    auto *libsTitle = new QLabel(tr("Used libraries:"), this);
    lay->addWidget(libsTitle);

    // Scrollable text area to show long credits/licenses text loaded from
    // docs/*.txt
    auto *libsText = new QPlainTextEdit(this);
    libsText->setReadOnly(true);
    libsText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    libsText->setMinimumHeight(180);

    // Decide which file to load based on UI language (settings: UI/language)
    QSettings s("OpenSCP", "OpenSCP");
    const QString lang = s.value("UI/language", "en").toString().toLower();
    const QString suffix =
        lang.startsWith("en") ? QStringLiteral("EN") : QStringLiteral("ES");

    const QStringList candidates = {
        QStringLiteral("ABOUT_LIBRARIES_%1.txt").arg(suffix),
        QStringLiteral("ABOUT_LIBRARIES_%1").arg(suffix),
        QStringLiteral("ABOUT_LIBRARIES.txt"),
        QStringLiteral("ABOUT_LIBRARIES")};
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
        content = tr("No third-party license details were found in this "
                     "installation.\n"
                     "Use an official package for full license information.");
    }
    libsText->setPlainText(content);
    lay->addWidget(libsText);

    const QString licensesDir = findLicensesDir();

    auto *actionsRow = new QHBoxLayout();

    auto *openLicensesBtn = new QPushButton(tr("Open Licenses Folder"), this);
    openLicensesBtn->setEnabled(!licensesDir.isEmpty());
    openLicensesBtn->setToolTip(
        licensesDir.isEmpty()
            ? tr("License files are not available in this installation.")
            : tr("Open the folder that contains third-party licenses."));
    connect(openLicensesBtn, &QPushButton::clicked, this, [this, licensesDir] {
        if (!licensesDir.isEmpty() && QFileInfo(licensesDir).isDir()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(licensesDir));
            return;
        }
        QMessageBox::information(this, tr("Licenses folder not found"),
                                 tr("No license files were found in this "
                                    "installation."));
    });
    actionsRow->addWidget(openLicensesBtn);

    auto *copyDiagnosticsBtn = new QPushButton(tr("Copy diagnostics"), this);
    copyDiagnosticsBtn->setToolTip(
        tr("Copy version and environment details for support."));
    connect(copyDiagnosticsBtn, &QPushButton::clicked, this, [this] {
        if (QGuiApplication::clipboard() == nullptr) {
            QMessageBox::information(
                this, tr("Diagnostics unavailable"),
                tr("Could not access the system clipboard."));
            return;
        }
        QGuiApplication::clipboard()->setText(buildDiagnosticsText());
        QMessageBox::information(
            this, tr("Diagnostics copied"),
            tr("Diagnostic information was copied to your clipboard."));
    });
    actionsRow->addWidget(copyDiagnosticsBtn);
    actionsRow->addStretch(1);
    lay->addLayout(actionsRow);

    // Report an issue link at the bottom (opens Issues page)
    {
        const QString linkText = tr("Report an issue");
        auto *report = new QLabel(
            QString("<a href=\"%1\">%2</a>")
                .arg(issuesUrl.toHtmlEscaped(), linkText.toHtmlEscaped()),
            this);
        report->setTextFormat(Qt::RichText);
        report->setOpenExternalLinks(true);
        report->setWordWrap(true);
        lay->addWidget(report);
    }

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(btns, &QDialogButtonBox::rejected, this, &AboutDialog::reject);
    connect(btns, &QDialogButtonBox::accepted, this, &AboutDialog::accept);
    lay->addWidget(btns);

    // Enforce a dynamic minimum size so controls never overlap when shrinking
    lay->setSizeConstraint(QLayout::SetMinimumSize);
    const QSize layMin = lay->minimumSize();
    const int minW = qMax(560, layMin.width());
    const int minH = qMax(360, layMin.height());
    this->setMinimumSize(minW, minH);
    this->resize(qMax(620, minW), qMax(460, minH));
}
