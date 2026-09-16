// Implementation of the "About" dialog for OpenSCP.
#include "widgets/dialogs/AboutDialog.hpp"

#include "AppVersion.hpp"
#include "logic/common/UiAlerts.hpp"
#include "widgets/platform/PlatformPathActions.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QStringConverter>
#include <QStringList>
#include <QSysInfo>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

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
        QDir candidateBaseDir(base);
        for (int depth = 0; depth < 5; ++depth) { // search up to 5 levels up
            for (const QString &rel : relativeCandidates) {
                const QString candidate = candidateBaseDir.filePath(rel);
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
            if (!candidateBaseDir.cdUp()) {
                break;
            }
        }
    }
    return {};
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
        .arg(QString::fromUtf8(OPENSCP_APP_VERSION),
             QString::fromUtf8(qVersion()), QSysInfo::prettyProductName(),
             QSysInfo::currentCpuArchitecture(),
             QString::fromUtf8(OPENSCP_BUILD_TYPE),
             QString::fromUtf8(OPENSCP_GIT_COMMIT),
             QString::fromUtf8(OPENSCP_REPOSITORY_URL));
}

struct CreditSection {
    QString title;
    QString details;
};

QVector<CreditSection> splitCreditSections(const QString &markdown) {
    QVector<CreditSection> sections;
    CreditSection current;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("## "))) {
            if (!current.title.isEmpty()) {
                current.details = current.details.trimmed();
                sections.push_back(current);
            }
            current = {line.mid(3).trimmed(), QString()};
        } else if (!current.title.isEmpty()) {
            current.details += line;
            current.details += QLatin1Char('\n');
        }
    }
    if (!current.title.isEmpty()) {
        current.details = current.details.trimmed();
        sections.push_back(current);
    }
    return sections;
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("About OpenSCP"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *topRow = new QHBoxLayout();
    auto *leftCol = new QVBoxLayout();

    auto *title = new QLabel(QString("<b>OpenSCP v%1</b>")
                                 .arg(QCoreApplication::applicationVersion()),
                             this);
    title->setTextFormat(Qt::RichText);
    leftCol->addWidget(title);

    const QString authorName = QString::fromUtf8(OPENSCP_AUTHOR_NAME);
    const QString authorUrl = QString::fromUtf8(OPENSCP_AUTHOR_URL);
    const QString issuesUrl = QString::fromUtf8(OPENSCP_ISSUES_URL);

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
    QPixmap iconPix(QStringLiteral(":/assets/icons/app-openscp.png"));
    if (!iconPix.isNull()) {
        iconLabel->setPixmap(iconPix.scaled(96, 96, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation));
    }
    iconLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    topRow->addWidget(iconLabel, 0);
    root->addLayout(topRow);
    root->addStretch(1);

    QString creditsContent;
    QFile creditsFile(QStringLiteral(":/credits/CREDITS.md"));
    QUrl creditsBaseUrl(QStringLiteral("qrc:/credits/"));
    if (!creditsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString creditsPath = findFromCandidates(
            {QStringLiteral("docs/credits/CREDITS.md")}, false);
        if (!creditsPath.isEmpty()) {
            creditsFile.setFileName(creditsPath);
            creditsFile.open(QIODevice::ReadOnly | QIODevice::Text);
            creditsBaseUrl = QUrl::fromLocalFile(
                QFileInfo(creditsPath).absolutePath() + QDir::separator());
        }
    }
    if (creditsFile.isOpen()) {
        QTextStream creditsStream(&creditsFile);
        creditsStream.setEncoding(QStringConverter::Utf8);
        creditsContent = creditsStream.readAll();
    }
    if (creditsContent.isEmpty()) {
        creditsContent =
            tr("No third-party license details were found in this "
               "installation.\n"
               "Use an official package for full license information.");
    }
    QVector<CreditSection> creditSections = splitCreditSections(creditsContent);
    if (creditSections.isEmpty())
        creditSections.push_back({tr("Credits"), creditsContent});
    const QString licensesDir = findLicensesDir();

    auto *actionsRow = new QHBoxLayout();
    auto *creditsButton = new QPushButton(tr("Credits…"), this);
    creditsButton->setObjectName(QStringLiteral("aboutCreditsButton"));
    connect(
        creditsButton, &QPushButton::clicked, this,
        [this, creditsBaseUrl, creditSections, licensesDir] {
            QDialog creditsDialog(this);
            creditsDialog.setObjectName(QStringLiteral("creditsDialog"));
            creditsDialog.setWindowTitle(tr("Credits"));

            auto *creditsLayout = new QVBoxLayout(&creditsDialog);
            creditsLayout->setContentsMargins(12, 12, 12, 12);
            creditsLayout->setSpacing(10);

            auto *splitter = new QSplitter(Qt::Horizontal, &creditsDialog);
            auto *componentList = new QListWidget(splitter);
            componentList->setObjectName(QStringLiteral("creditComponentList"));
            componentList->setAccessibleName(tr("Credits"));
            componentList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            componentList->setTextElideMode(Qt::ElideRight);
            componentList->setMinimumWidth(170);
            componentList->setMaximumWidth(230);

            auto *details = new QTextBrowser(splitter);
            details->setObjectName(QStringLiteral("creditDetailsBrowser"));
            details->setAccessibleName(tr("Credits"));
            details->setOpenExternalLinks(true);
            details->document()->setDocumentMargin(12.0);
            details->document()->setBaseUrl(creditsBaseUrl);

            for (const CreditSection &section : creditSections) {
                auto *item = new QListWidgetItem(section.title, componentList);
                item->setToolTip(section.title);
            }

            connect(componentList, &QListWidget::currentRowChanged,
                    &creditsDialog, [details, creditSections](int row) {
                        if (row < 0 || row >= creditSections.size()) {
                            details->clear();
                            return;
                        }
                        details->setMarkdown(creditSections.at(row).details);
                    });

            splitter->setStretchFactor(0, 0);
            splitter->setStretchFactor(1, 1);
            splitter->setSizes({190, 410});
            creditsLayout->addWidget(splitter, 1);

            auto *buttons =
                new QDialogButtonBox(QDialogButtonBox::Close, &creditsDialog);
            auto *openLicensesButton = buttons->addButton(
                tr("Open Licenses Folder"), QDialogButtonBox::ActionRole);
            openLicensesButton->setEnabled(!licensesDir.isEmpty());
            openLicensesButton->setToolTip(
                licensesDir.isEmpty()
                    ? tr("License files are not available in this "
                         "installation.")
                    : tr("Open the folder that contains third-party "
                         "licenses."));
            connect(openLicensesButton, &QPushButton::clicked, &creditsDialog,
                    [&creditsDialog, licensesDir] {
                        if (!licensesDir.isEmpty() &&
                            QFileInfo(licensesDir).isDir()) {
                            const openscpui::PathActionResult result =
                                openscpui::PlatformPathActions::openFolder(
                                    licensesDir);
                            if (result.failed()) {
                                UiAlerts::warning(&creditsDialog,
                                                  tr("Open location"),
                                                  result.error);
                            }
                            return;
                        }
                        UiAlerts::information(
                            &creditsDialog, tr("Licenses folder not found"),
                            tr("No license files were found in this "
                               "installation."));
                    });
            connect(buttons, &QDialogButtonBox::rejected, &creditsDialog,
                    &QDialog::reject);
            creditsLayout->addWidget(buttons);

            componentList->setCurrentRow(0);
            const QSize creditsMinimum =
                QSize(520, 340).expandedTo(creditsDialog.minimumSizeHint());
            creditsDialog.setMinimumSize(creditsMinimum);
            creditsDialog.resize(QSize(560, 360).expandedTo(creditsMinimum));
            creditsDialog.exec();
        });
    actionsRow->addWidget(creditsButton);

    auto *copyDiagnosticsBtn = new QPushButton(tr("Copy diagnostics"), this);
    copyDiagnosticsBtn->setToolTip(
        tr("Copy version and environment details for support."));
    connect(copyDiagnosticsBtn, &QPushButton::clicked, this, [this] {
        if (QGuiApplication::clipboard() == nullptr) {
            UiAlerts::information(this, tr("Diagnostics unavailable"),
                                  tr("Could not access the system clipboard."));
            return;
        }
        QGuiApplication::clipboard()->setText(buildDiagnosticsText());
        UiAlerts::information(
            this, tr("Diagnostics copied"),
            tr("Diagnostic information was copied to your clipboard."));
    });
    actionsRow->addWidget(copyDiagnosticsBtn);
    actionsRow->addStretch(1);
    root->addLayout(actionsRow);

    const QString linkText = tr("Report an issue");
    auto *report = new QLabel(
        QString("<a href=\"%1\">%2</a>")
            .arg(issuesUrl.toHtmlEscaped(), linkText.toHtmlEscaped()),
        this);
    report->setTextFormat(Qt::RichText);
    report->setOpenExternalLinks(true);
    report->setWordWrap(true);
    root->addWidget(report);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(btns, &QDialogButtonBox::rejected, this, &AboutDialog::reject);
    connect(btns, &QDialogButtonBox::accepted, this, &AboutDialog::accept);
    root->addWidget(btns);

    setMinimumSize(QSize(440, 260).expandedTo(minimumSizeHint()));
    resize(QSize(460, 270).expandedTo(minimumSize()));
}
