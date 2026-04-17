// MainWindow orchestrator: builds UI shell, wires actions, and applies
// preferences. Detailed local/remote/transfer logic lives in dedicated units.
#include "MainWindow.hpp"
#include "AboutDialog.hpp"
#include "ConnectionDialog.hpp"
#include "DragAwareTreeView.hpp"
#include "PermissionsDialog.hpp"
#include "RemoteModel.hpp"
#include "SecretStore.hpp"
#include "SettingsDialog.hpp"
#include "SiteManagerDialog.hpp"
#include "TransferManager.hpp"
#include "TransferQueueDialog.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QParallelAnimationGroup>
#include <QProcess>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStackedWidget>
#include <QTemporaryFile>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

static constexpr int NAME_COL = 0;

static QString normalizeRemotePathForMatch(const QString &rawPath) {
    QString normalized = rawPath.trimmed();
    if (normalized.isEmpty())
        normalized = QStringLiteral("/");
    if (!normalized.startsWith('/'))
        normalized.prepend('/');
    while (normalized.contains(QStringLiteral("//")))
        normalized.replace(QStringLiteral("//"), QStringLiteral("/"));
    if (normalized.size() > 1 && normalized.endsWith('/'))
        normalized.chop(1);
    return normalized;
}

static bool remotePathIsInsideRoot(const QString &candidatePath,
                                   const QString &rootPath) {
    const QString candidate = normalizeRemotePathForMatch(candidatePath);
    const QString root = normalizeRemotePathForMatch(rootPath);
    if (root == QStringLiteral("/"))
        return candidate.startsWith('/');
    return candidate == root || candidate.startsWith(root + QStringLiteral("/"));
}

static bool focusWithinWidget(QWidget *focus, QWidget *root) {
    if (!focus || !root)
        return false;
    for (QWidget *cur = focus; cur != nullptr; cur = cur->parentWidget()) {
        if (cur == root)
            return true;
    }
    return false;
}

static QString formatConnectionElapsed(qint64 totalSeconds) {
    if (totalSeconds < 0)
        totalSeconds = 0;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

constexpr int kRecentHistoryMaxEntries = 20;
constexpr const char *kRecentLocalPathsKey = "History/recentLocalPaths";
constexpr const char *kRecentRemotePathsKey = "History/recentRemotePaths";
constexpr const char *kRecentServersKey = "History/recentServers";
constexpr const char *kShortcutTransfersKey = "Shortcuts/openTransfers";
constexpr const char *kShortcutHistoryKey = "Shortcuts/openHistory";

static QString trimHistoryLabel(const QString &raw, int maxLen = 96) {
    QString out = raw.simplified();
    if (out.size() <= maxLen)
        return out;
    if (maxLen <= 3)
        return out.left(maxLen);
    return out.left(maxLen - 3) + QStringLiteral("...");
}

static void prependRecentValue(QStringList *list, const QString &value,
                               int maxEntries = kRecentHistoryMaxEntries) {
    if (!list)
        return;
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return;
    list->removeAll(trimmed);
    list->prepend(trimmed);
    while (list->size() > maxEntries)
        list->removeLast();
}

static QString normalizedLocalHistoryPath(const QString &raw) {
    QString normalized = QDir::fromNativeSeparators(raw.trimmed());
    if (normalized.isEmpty())
        return {};
    normalized = QDir::cleanPath(normalized);
    if (!QFileInfo(normalized).isAbsolute())
        normalized = QDir::current().absoluteFilePath(normalized);
    return QDir(normalized).absolutePath();
}

static QString encodeRecentServerEntry(const openscp::SessionOptions &opt) {
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("protocol"),
        QString::fromLatin1(openscp::protocolStorageName(opt.protocol)));
    query.addQueryItem(QStringLiteral("host"),
                       QString::fromStdString(opt.host).trimmed().toLower());
    query.addQueryItem(QStringLiteral("port"), QString::number(opt.port));
    query.addQueryItem(QStringLiteral("user"),
                       QString::fromStdString(opt.username).trimmed());
    if (opt.protocol == openscp::Protocol::WebDav) {
        query.addQueryItem(
            QStringLiteral("webdavScheme"),
            QString::fromLatin1(openscp::webDavSchemeStorageName(
                openscp::normalizeWebDavScheme(opt.webdav_scheme))));
    }
    return query.toString(QUrl::FullyEncoded);
}

static bool decodeRecentServerEntry(const QString &encoded,
                                    openscp::SessionOptions *optOut,
                                    QString *labelOut) {
    const QUrlQuery query(encoded);
    const QString host = query.queryItemValue(QStringLiteral("host"))
                             .trimmed()
                             .toLower();
    if (host.isEmpty())
        return false;
    const QString protocolStorage =
        query.queryItemValue(QStringLiteral("protocol"))
            .trimmed()
            .toLower();
    const openscp::Protocol protocol = openscp::protocolFromStorageName(
        protocolStorage.toStdString());
    bool portOk = false;
    int portRaw =
        query.queryItemValue(QStringLiteral("port")).trimmed().toInt(&portOk);
    if (!portOk || portRaw <= 0 || portRaw > 65535) {
        portRaw = static_cast<int>(openscp::defaultPortForProtocol(protocol));
    }
    const QString user =
        query.queryItemValue(QStringLiteral("user")).trimmed();

    if (optOut) {
        openscp::SessionOptions opt{};
        opt.protocol = protocol;
        opt.host = host.toStdString();
        opt.port = static_cast<std::uint16_t>(portRaw);
        opt.username = user.toStdString();
        if (protocol == openscp::Protocol::WebDav) {
            const QString rawScheme =
                query.queryItemValue(QStringLiteral("webdavScheme"))
                    .trimmed()
                    .toLower();
            if (!rawScheme.isEmpty()) {
                opt.webdav_scheme = openscp::webDavSchemeFromStorageName(
                    rawScheme.toStdString());
            } else if (opt.port ==
                       openscp::defaultPortForWebDavScheme(
                           openscp::WebDavScheme::Http)) {
                opt.webdav_scheme = openscp::WebDavScheme::Http;
            }
            if (opt.webdav_scheme == openscp::WebDavScheme::Http) {
                opt.webdav_verify_peer = false;
                opt.webdav_ca_cert_path.reset();
            }
        }
        *optOut = opt;
    }
    if (labelOut) {
        QString endpoint = host;
        if (static_cast<std::uint16_t>(portRaw) !=
            openscp::defaultPortForProtocol(protocol)) {
            endpoint += QStringLiteral(":%1").arg(portRaw);
        }
        if (!user.isEmpty())
            endpoint = QStringLiteral("%1@%2").arg(user, endpoint);
        *labelOut = QStringLiteral("%1  %2")
                        .arg(
                            QString::fromLatin1(
                                openscp::protocolDisplayName(protocol))
                                .toUpper(),
                            endpoint);
    }
    return true;
}

static bool hasRegexMetaBeyondWildcards(const QString &pattern) {
    static const QString kRegexMeta = QStringLiteral("\\.^$+()[]{}|");
    for (const QChar ch : pattern) {
        if (kRegexMeta.contains(ch))
            return true;
    }
    return false;
}

static QString wildcardPatternToRegex(const QString &wildcard) {
    QString regex;
    regex.reserve((wildcard.size() * 2) + 4);
    regex += QLatin1Char('^');
    for (const QChar ch : wildcard) {
        if (ch == QLatin1Char('*')) {
            regex += QStringLiteral(".*");
        } else if (ch == QLatin1Char('?')) {
            regex += QLatin1Char('.');
        } else {
            regex += QRegularExpression::escape(QString(ch));
        }
    }
    regex += QLatin1Char('$');
    return regex;
}

static QRegularExpression compilePanelSearchRegex(const QString &rawPattern,
                                                  QString *errorOut) {
    if (errorOut)
        errorOut->clear();
    const QString pattern = rawPattern.trimmed();
    if (pattern.isEmpty())
        return QRegularExpression();

    QString regexPattern;
    if (hasRegexMetaBeyondWildcards(pattern)) {
        regexPattern = pattern;
    } else if (pattern.contains(QLatin1Char('*')) ||
               pattern.contains(QLatin1Char('?'))) {
        regexPattern = wildcardPatternToRegex(pattern);
    } else {
        regexPattern = QStringLiteral(".*%1.*")
                           .arg(QRegularExpression::escape(pattern));
    }

    QRegularExpression regex(regexPattern,
                             QRegularExpression::CaseInsensitiveOption);
    if (!regex.isValid() && errorOut)
        *errorOut = regex.errorString();
    return regex;
}

struct PanelSearchPromptResult {
    QString pattern;
    bool recursive = false;
};

static bool promptPanelSearch(QWidget *parent, const QString &panelLabel,
                              PanelSearchPromptResult *out) {
    if (!out)
        return false;

    QDialog dlg(parent);
    dlg.setWindowTitle(
        QCoreApplication::translate("MainWindow", "Search items (%1)")
            .arg(panelLabel));
    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *help = new QLabel(
        QCoreApplication::translate(
            "MainWindow",
            "Pattern accepts wildcard (*, ?) or regex.\nExamples: *report*, "
            "report, ^report_.*\\.pdf$"),
        &dlg);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto *patternEdit = new QLineEdit(&dlg);
    patternEdit->setPlaceholderText(
        QCoreApplication::translate("MainWindow", "e.g. *report*"));
    patternEdit->setClearButtonEnabled(true);
    layout->addWidget(patternEdit);

    auto *recursiveCheck = new QCheckBox(
        QCoreApplication::translate(
            "MainWindow", "Search recursively in subfolders"),
        &dlg);
    recursiveCheck->setChecked(false);
    layout->addWidget(recursiveCheck);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel,
                                     &dlg);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    layout->addWidget(box);

    if (QAbstractButton *okBtn = box->button(QDialogButtonBox::Ok))
        okBtn->setEnabled(false);
    QObject::connect(patternEdit, &QLineEdit::textChanged, &dlg,
                     [box](const QString &text) {
                         if (QAbstractButton *okBtn =
                                 box->button(QDialogButtonBox::Ok)) {
                             okBtn->setEnabled(!text.trimmed().isEmpty());
                         }
                     });

    patternEdit->setFocus();
    dlg.adjustSize();
    const QSize minSearchSize = dlg.sizeHint();
    dlg.setMinimumSize(minSearchSize);
    dlg.resize(minSearchSize);
    if (dlg.exec() != QDialog::Accepted)
        return false;

    out->pattern = patternEdit->text().trimmed();
    out->recursive = recursiveCheck->isChecked();
    return !out->pattern.isEmpty();
}

static void showRecursiveSearchResultsDialog(
    QWidget *parent, const QString &panelLabel, const QString &basePath,
    const QVector<QPair<QString, bool>> &rows, int scanErrors, bool canceled,
    bool truncated) {
    QDialog dlg(parent);
    dlg.setWindowTitle(
        QCoreApplication::translate("MainWindow", "Search results (%1)")
            .arg(panelLabel));

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    QString summary =
        QCoreApplication::translate("MainWindow",
                                    "Base: %1\nMatches: %2")
            .arg(basePath, QString::number(rows.size()));
    if (scanErrors > 0) {
        summary += QStringLiteral("\n") +
                   QCoreApplication::translate("MainWindow",
                                               "Scan errors: %1")
                       .arg(QString::number(scanErrors));
    }
    if (canceled) {
        summary += QStringLiteral("\n") +
                   QCoreApplication::translate("MainWindow",
                                               "Search canceled by user.");
    }
    if (truncated) {
        summary += QStringLiteral("\n") +
                   QCoreApplication::translate(
                       "MainWindow", "Results truncated to safety limit.");
    }
    auto *summaryLabel = new QLabel(summary, &dlg);
    summaryLabel->setWordWrap(true);
    summaryLabel->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    summaryLabel->setMargin(8);
    layout->addWidget(summaryLabel);

    auto *list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setSelectionBehavior(QAbstractItemView::SelectRows);
    list->setAlternatingRowColors(true);
    list->setUniformItemSizes(true);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    for (const auto &row : rows) {
        QString label = row.first;
        if (row.second && !label.endsWith('/'))
            label += QLatin1Char('/');
        list->addItem(label);
    }
    layout->addWidget(list, 1);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    layout->addWidget(box);

    dlg.adjustSize();
    const QSize minResultsSize = dlg.sizeHint();
    dlg.setMinimumSize(minResultsSize);
    dlg.resize(minResultsSize);
    dlg.exec();
}

MainWindow::~MainWindow() = default; // define the destructor here

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // Models
    leftModel_ = new QFileSystemModel(this);
    rightLocalModel_ = new QFileSystemModel(this);

    leftModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                          QDir::AllDirs);
    rightLocalModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                                QDir::AllDirs);

    // Initial paths: local home (fallback to root if HOME is unavailable)
    const QString home = preferredLocalHomePath();
    leftModel_->setRootPath(home);
    rightLocalModel_->setRootPath(home);

    // Views
    leftView_ = new DragAwareTreeView(this);
    rightView_ = new DragAwareTreeView(this);

    leftView_->setModel(leftModel_);
    rightView_->setModel(rightLocalModel_);
    // Avoid expanding subtrees on double‑click; navigate by changing root
    leftView_->setExpandsOnDoubleClick(false);
    rightView_->setExpandsOnDoubleClick(false);
    leftView_->setRootIndex(leftModel_->index(home));
    rightView_->setRootIndex(rightLocalModel_->index(home));

    // Basic view tuning
    auto tuneView = [](QTreeView *v) {
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setSelectionBehavior(QAbstractItemView::SelectRows);
        v->setSortingEnabled(true);
        v->sortByColumn(0, Qt::AscendingOrder);
        v->header()->setStretchLastSection(true);
        v->setColumnWidth(0, 280);
    };
    tuneView(leftView_);
    tuneView(rightView_);
    leftView_->setDragEnabled(true);
    rightView_->setDragEnabled(true); // allow starting drags from right pane

    // Accept drops on both panes
    rightView_->setAcceptDrops(true);
    rightView_->setDragDropMode(QAbstractItemView::DragDrop);
    rightView_->viewport()->setAcceptDrops(true);
    rightView_->setDefaultDropAction(Qt::CopyAction);
    leftView_->setAcceptDrops(true);
    leftView_->setDragDropMode(QAbstractItemView::DragDrop);
    leftView_->viewport()->setAcceptDrops(true);
    leftView_->setDefaultDropAction(Qt::CopyAction);
    // Install event filters on viewports to receive drag/drop
    rightView_->viewport()->installEventFilter(this);
    leftView_->viewport()->installEventFilter(this);

    // Path entries (top)
    leftPath_ = new QLineEdit(home, this);
    rightPath_ = new QLineEdit(home, this);
    leftPath_->setClearButtonEnabled(true);
    rightPath_->setClearButtonEnabled(true);
    connect(leftPath_, &QLineEdit::returnPressed, this,
            &MainWindow::leftPathEntered);
    connect(rightPath_, &QLineEdit::returnPressed, this,
            &MainWindow::rightPathEntered);

    // Central splitter with two panes
    mainSplitter_ = new QSplitter(this);
    auto *leftPane = new QWidget(this);
    auto *rightPane = new QWidget(this);

    auto *leftLayout = new QVBoxLayout(leftPane);
    auto *rightLayout = new QVBoxLayout(rightPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // Right content stack:
    // - index 0: standard file tree view (local/SFTP)
    // - index 1: SCP transfer-only panel (no remote listing)
    rightContentStack_ = new QStackedWidget(rightPane);
    rightContentStack_->addWidget(rightView_);
    scpTransferPanel_ = new QWidget(rightPane);
    auto *scpLay = new QVBoxLayout(scpTransferPanel_);
    scpLay->setContentsMargins(12, 12, 12, 12);
    scpLay->setSpacing(8);
    scpModeHintLabel_ = new QLabel(
        tr("This protocol works in transfer-only mode.\nUse the remote path "
           "above as the target folder for uploads.\nFor downloads, choose a "
           "remote file path explicitly."),
        scpTransferPanel_);
    scpModeHintLabel_->setWordWrap(true);
    scpQuickUploadBtn_ = new QPushButton(tr("Upload local files…"), scpTransferPanel_);
    scpQuickDownloadBtn_ =
        new QPushButton(tr("Download remote file…"), scpTransferPanel_);
    scpLay->addWidget(scpModeHintLabel_);
    scpLay->addWidget(scpQuickUploadBtn_);
    scpLay->addWidget(scpQuickDownloadBtn_);
    scpLay->addStretch(1);
    connect(scpQuickUploadBtn_, &QPushButton::clicked, this,
            &MainWindow::uploadViaDialog);
    connect(scpQuickDownloadBtn_, &QPushButton::clicked, this,
            &MainWindow::downloadRightToLeft);
    rightContentStack_->addWidget(scpTransferPanel_);
    rightContentStack_->setCurrentWidget(rightView_);

    // Left pane sub‑toolbar
    leftPaneBar_ = new QToolBar("LeftBar", leftPane);
    leftPaneBar_->setIconSize(QSize(18, 18));
    leftPaneBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    leftBreadcrumbsBar_ = new QToolBar("LeftBreadcrumbs", leftPane);
    leftBreadcrumbsBar_->setMovable(false);
    leftBreadcrumbsBar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    leftBreadcrumbsBar_->setIconSize(QSize(14, 14));
    leftBreadcrumbsBar_->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Fixed);
    // Helper for icons from local resources
    auto resIcon = [](const char *fname) -> QIcon {
        return QIcon(QStringLiteral(":/assets/icons/") + QLatin1String(fname));
    };
    auto leftSearchLabel = [this]() {
        return rightIsRemote_ ? tr("Local panel") : tr("Local panel - left");
    };
    auto rightSearchLabel = [this]() {
        return rightIsRemote_ ? tr("Remote panel")
                              : tr("Local panel - right");
    };
    // Left sub‑toolbar: Up, Open, Home, Search, Copy/Move/Delete, Rename/New
    actUpLeft_ = leftPaneBar_->addAction(tr("Up"), this, &MainWindow::goUpLeft);
    actUpLeft_->setIcon(resIcon("action-go-up.svg"));
    actUpLeft_->setToolTip(actUpLeft_->text());
    // Button "Open left folder" next to Up
    actChooseLeft_ = leftPaneBar_->addAction(tr("Open left folder"), this,
                                             &MainWindow::chooseLeftDir);
    actChooseLeft_->setIcon(resIcon("action-open-folder.svg"));
    actChooseLeft_->setToolTip(actChooseLeft_->text());
    actHomeLeft_ =
        leftPaneBar_->addAction(tr("Home"), this, &MainWindow::goHomeLeft);
    actHomeLeft_->setIcon(resIcon("action-go-home.svg"));
    actHomeLeft_->setToolTip(actHomeLeft_->text());
    actSearchLeft_ =
        leftPaneBar_->addAction(tr("Search items"), this,
                                [this, leftSearchLabel] {
                                    searchItemsInCurrentFolder(
                                        leftView_, leftSearchLabel());
                                });
    actSearchLeft_->setIcon(resIcon("action-search-item.svg"));
    actSearchLeft_->setToolTip(actSearchLeft_->text());
    leftPaneBar_->addSeparator();
    actCopyF5_ =
        leftPaneBar_->addAction(tr("Copy"), this, &MainWindow::copyLeftToRight);
    actCopyF5_->setIcon(resIcon("action-copy.svg"));
    actCopyF5_->setToolTip(actCopyF5_->text());
    // Shortcut F5 on left panel (scope: left view and its children)
    actCopyF5_->setShortcut(QKeySequence(Qt::Key_F5));
    actCopyF5_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actCopyF5_);
    actMoveF6_ =
        leftPaneBar_->addAction(tr("Move"), this, &MainWindow::moveLeftToRight);
    actMoveF6_->setIcon(resIcon("action-move-to-right.svg"));
    actMoveF6_->setToolTip(actMoveF6_->text());
    // Shortcut F6 on left panel
    actMoveF6_->setShortcut(QKeySequence(Qt::Key_F6));
    actMoveF6_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actMoveF6_);
    actDelete_ = leftPaneBar_->addAction(tr("Delete"), this,
                                         &MainWindow::deleteFromLeft);
    actDelete_->setIcon(resIcon("action-delete.svg"));
    actDelete_->setToolTip(actDelete_->text());
    actDelete_->setShortcut(QKeySequence(Qt::Key_Delete));
    actDelete_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actDelete_);
    // Action: copy from right panel to left (remote/local -> left)
    actCopyRight_ = new QAction(tr("Copy to left panel"), this);
    connect(actCopyRight_, &QAction::triggered, this,
            &MainWindow::copyRightToLeft);
    actCopyRight_->setIcon(
        QIcon(QLatin1String(":/assets/icons/action-copy.svg")));
    // Action: move from right panel to left
    actMoveRight_ = new QAction(tr("Move to left panel"), this);
    connect(actMoveRight_, &QAction::triggered, this,
            &MainWindow::moveRightToLeft);
    actMoveRight_->setIcon(
        QIcon(QLatin1String(":/assets/icons/action-move-to-left.svg")));
    // Additional local actions (also in toolbar)
    actNewDirLeft_ = new QAction(tr("New folder"), this);
    connect(actNewDirLeft_, &QAction::triggered, this, &MainWindow::newDirLeft);
    actRenameLeft_ = new QAction(tr("Rename"), this);
    connect(actRenameLeft_, &QAction::triggered, this,
            &MainWindow::renameLeftSelected);
    actNewFileLeft_ = new QAction(tr("New file"), this);
    connect(actNewFileLeft_, &QAction::triggered, this,
            &MainWindow::newFileLeft);
    actRenameLeft_->setIcon(resIcon("action-rename.svg"));
    actRenameLeft_->setToolTip(actRenameLeft_->text());
    actNewDirLeft_->setIcon(resIcon("action-new-folder.svg"));
    actNewDirLeft_->setToolTip(actNewDirLeft_->text());
    actNewFileLeft_->setIcon(resIcon("action-new-file.svg"));
    actNewFileLeft_->setToolTip(actNewFileLeft_->text());
    // Shortcuts (left panel scope)
    actRenameLeft_->setShortcut(QKeySequence(Qt::Key_F2));
    actRenameLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actRenameLeft_);
    actNewDirLeft_->setShortcut(QKeySequence(Qt::Key_F9));
    actNewDirLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actNewDirLeft_);
    actNewFileLeft_->setShortcut(QKeySequence(Qt::Key_F10));
    actNewFileLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actNewFileLeft_);
    leftPaneBar_->addAction(actRenameLeft_);
    leftPaneBar_->addAction(actNewFileLeft_);
    leftPaneBar_->addAction(actNewDirLeft_);
    leftLayout->addWidget(leftPaneBar_);

    // Left panel: toolbar -> breadcrumbs -> path -> view
    leftLayout->addWidget(leftBreadcrumbsBar_);
    leftLayout->addWidget(leftPath_);
    leftLayout->addWidget(leftView_);

    // Right pane sub‑toolbar
    rightPaneBar_ = new QToolBar("RightBar", rightPane);
    rightPaneBar_->setIconSize(QSize(18, 18));
    rightPaneBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    rightBreadcrumbsBar_ = new QToolBar("RightBreadcrumbs", rightPane);
    rightBreadcrumbsBar_->setMovable(false);
    rightBreadcrumbsBar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    rightBreadcrumbsBar_->setIconSize(QSize(14, 14));
    rightBreadcrumbsBar_->setSizePolicy(QSizePolicy::Expanding,
                                        QSizePolicy::Fixed);
    actUpRight_ =
        rightPaneBar_->addAction(tr("Up"), this, &MainWindow::goUpRight);
    actUpRight_->setIcon(resIcon("action-go-up.svg"));
    actUpRight_->setToolTip(actUpRight_->text());
    // Button "Open right folder" next to Up
    actChooseRight_ = rightPaneBar_->addAction(tr("Open right folder"), this,
                                               &MainWindow::chooseRightDir);
    actChooseRight_->setIcon(resIcon("action-open-folder.svg"));
    actChooseRight_->setToolTip(actChooseRight_->text());
    actHomeRight_ =
        rightPaneBar_->addAction(tr("Home"), this, &MainWindow::goHomeRight);
    actHomeRight_->setIcon(resIcon("action-go-home.svg"));
    actHomeRight_->setToolTip(actHomeRight_->text());
    actSearchRight_ =
        rightPaneBar_->addAction(tr("Search items"), this,
                                 [this, rightSearchLabel] {
                                     searchItemsInCurrentFolder(
                                         rightView_, rightSearchLabel());
                                 });
    actSearchRight_->setIcon(resIcon("action-search-item.svg"));
    actSearchRight_->setToolTip(actSearchRight_->text());

    // Right panel actions (create first, then add in requested order)
    actDownloadF7_ = new QAction(tr("Download"), this);
    connect(actDownloadF7_, &QAction::triggered, this,
            &MainWindow::downloadRightToLeft);
    actDownloadF7_->setEnabled(false); // starts disabled on local
    actDownloadF7_->setIcon(resIcon("action-download.svg"));
    actDownloadF7_->setToolTip(actDownloadF7_->text());

    actUploadRight_ = new QAction(tr("Upload…"), this);
    connect(actUploadRight_, &QAction::triggered, this,
            &MainWindow::uploadViaDialog);
    actUploadRight_->setIcon(resIcon("action-upload.svg"));
    actUploadRight_->setToolTip(actUploadRight_->text());
    // Shortcut F8 on right panel to upload via dialog (remote only)
    actUploadRight_->setShortcut(QKeySequence(Qt::Key_F8));
    actUploadRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actUploadRight_);

    actRefreshRight_ = new QAction(tr("Refresh"), this);
    connect(actRefreshRight_, &QAction::triggered, this,
            &MainWindow::refreshRightRemotePanel);
    actRefreshRight_->setIcon(resIcon("action-refresh.svg"));
    actRefreshRight_->setToolTip(actRefreshRight_->text());

    actOpenTerminalRight_ = new QAction(tr("Open in terminal"), this);
    connect(actOpenTerminalRight_, &QAction::triggered, this,
            &MainWindow::openRightRemoteTerminal);
    actOpenTerminalRight_->setIcon(resIcon("action-open-terminal.svg"));
    actOpenTerminalRight_->setToolTip(actOpenTerminalRight_->text());

    actNewDirRight_ = new QAction(tr("New folder"), this);
    connect(actNewDirRight_, &QAction::triggered, this,
            &MainWindow::newDirRight);
    actRenameRight_ = new QAction(tr("Rename"), this);
    connect(actRenameRight_, &QAction::triggered, this,
            &MainWindow::renameRightSelected);
    actDeleteRight_ = new QAction(tr("Delete"), this);
    connect(actDeleteRight_, &QAction::triggered, this,
            &MainWindow::deleteRightSelected);
    actNewFileRight_ = new QAction(tr("New file"), this);
    connect(actNewFileRight_, &QAction::triggered, this,
            &MainWindow::newFileRight);
    actNewDirRight_->setIcon(resIcon("action-new-folder.svg"));
    actNewDirRight_->setToolTip(actNewDirRight_->text());
    actRenameRight_->setIcon(resIcon("action-rename.svg"));
    actRenameRight_->setToolTip(actRenameRight_->text());
    actDeleteRight_->setIcon(resIcon("action-delete.svg"));
    actDeleteRight_->setToolTip(actDeleteRight_->text());
    actNewFileRight_->setIcon(resIcon("action-new-file.svg"));
    actNewFileRight_->setToolTip(actNewFileRight_->text());
    // Shortcuts (right panel scope)
    actRenameRight_->setShortcut(QKeySequence(Qt::Key_F2));
    actRenameRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actRenameRight_);
    actNewDirRight_->setShortcut(QKeySequence(Qt::Key_F9));
    actNewDirRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actNewDirRight_);
    actNewFileRight_->setShortcut(QKeySequence(Qt::Key_F10));
    actNewFileRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actNewFileRight_);

    // Order: Copy, Move, Delete, Rename, New folder, then
    // Download/Upload/Refresh
    rightPaneBar_->addSeparator();
    // Toolbar buttons with generic texts (Copy/Move)
    actCopyRightTb_ = new QAction(tr("Copy"), this);
    connect(actCopyRightTb_, &QAction::triggered, this,
            &MainWindow::copyRightToLeft);
    actMoveRightTb_ = new QAction(tr("Move"), this);
    connect(actMoveRightTb_, &QAction::triggered, this,
            &MainWindow::moveRightToLeft);
    actCopyRightTb_->setIcon(resIcon("action-copy.svg"));
    actCopyRightTb_->setToolTip(actCopyRightTb_->text());
    actMoveRightTb_->setIcon(resIcon("action-move-to-left.svg"));
    actMoveRightTb_->setToolTip(actMoveRightTb_->text());
    // Shortcuts F5/F6 on right panel (scope: right view)
    actCopyRightTb_->setShortcut(QKeySequence(Qt::Key_F5));
    actCopyRightTb_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actCopyRightTb_);
    actMoveRightTb_->setShortcut(QKeySequence(Qt::Key_F6));
    actMoveRightTb_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actMoveRightTb_);
    rightPaneBar_->addAction(actCopyRightTb_);
    rightPaneBar_->addAction(actMoveRightTb_);
    rightPaneBar_->addAction(actDeleteRight_);
    rightPaneBar_->addAction(actRenameRight_);
    rightPaneBar_->addAction(actNewFileRight_);
    rightPaneBar_->addAction(actNewDirRight_);
    rightPaneBar_->addSeparator();
    rightPaneBar_->addAction(actDownloadF7_);
    rightPaneBar_->addAction(actUploadRight_);
    rightPaneBar_->addSeparator();
    rightPaneBar_->addAction(actRefreshRight_);
    rightPaneBar_->addAction(actOpenTerminalRight_);
    // Delete shortcut also on right panel (limited to right panel widget)
    if (actDeleteRight_) {
        actDeleteRight_->setShortcut(QKeySequence(Qt::Key_Delete));
        actDeleteRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        if (rightView_)
            rightView_->addAction(actDeleteRight_);
    }
    // Keyboard shortcut F7 on right panel: only acts when remote and with
    // selection
    if (rightView_) {
        auto *scF7 = new QShortcut(QKeySequence(Qt::Key_F7), rightView_);
        scF7->setContext(Qt::WidgetWithChildrenShortcut);
        connect(scF7, &QShortcut::activated, this, [this] {
            if (!rightIsRemote_)
                return; // only when remote
            auto sel = rightView_->selectionModel();
            if (!sel || sel->selectedRows(NAME_COL).isEmpty()) {
                statusBar()->showMessage(tr("Select items to download"), 2000);
                return;
            }
            downloadRightToLeft();
        });
    }
    // Ctrl+F / Cmd+F: open search dialog for the focused panel.
    auto *scFind = new QShortcut(QKeySequence::Find, this);
    scFind->setContext(Qt::WindowShortcut);
    connect(scFind, &QShortcut::activated, this,
            [this, leftSearchLabel, rightSearchLabel] {
        QWidget *focus = QApplication::focusWidget();
        const bool inRightPanel =
            focusWithinWidget(focus, rightView_) ||
            focusWithinWidget(focus, rightPath_) ||
            focusWithinWidget(focus, rightPaneBar_) ||
            focusWithinWidget(focus, rightBreadcrumbsBar_);
        if (inRightPanel) {
            searchItemsInCurrentFolder(rightView_, rightSearchLabel());
            return;
        }

        const bool inLeftPanel =
            focusWithinWidget(focus, leftView_) ||
            focusWithinWidget(focus, leftPath_) ||
            focusWithinWidget(focus, leftPaneBar_) ||
            focusWithinWidget(focus, leftBreadcrumbsBar_);
        if (inLeftPanel) {
            searchItemsInCurrentFolder(leftView_, leftSearchLabel());
            return;
        }

        if (rightIsRemote_)
            searchItemsInCurrentFolder(rightView_, rightSearchLabel());
        else
            searchItemsInCurrentFolder(leftView_, leftSearchLabel());
    });
    // Disable strictly-remote actions at startup
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(false);
    actUploadRight_->setEnabled(false);
    if (actRefreshRight_)
        actRefreshRight_->setEnabled(false);
    if (actOpenTerminalRight_)
        actOpenTerminalRight_->setEnabled(false);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(false);

    // Right panel: toolbar -> breadcrumbs -> path -> view
    rightLayout->addWidget(rightPaneBar_);
    rightLayout->addWidget(rightBreadcrumbsBar_);
    rightLayout->addWidget(rightPath_);
    rightLayout->addWidget(rightContentStack_);

    // Mount panes into the splitter
    mainSplitter_->addWidget(leftPane);
    mainSplitter_->addWidget(rightPane);
    setCentralWidget(mainSplitter_);

    // Main toolbar (top)
    auto *tb = addToolBar("Main");
    tb->setObjectName("mainToolbar");
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setMovable(false);
    // Keep the system default size for the main toolbar and make sub‑toolbars
    // slightly smaller
    const int mainIconPx =
        tb->style()->pixelMetric(QStyle::PM_ToolBarIconSize, nullptr, tb);
    const int subIconPx =
        qMax(16, mainIconPx - 4); // sub‑toolbars slightly smaller
    leftPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    rightPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    // Copy/move/delete actions now live in the left sub‑toolbar
    actConnect_ =
        tb->addAction(tr("Connect"), this, &MainWindow::connectSftp);
    actConnect_->setIcon(resIcon("action-connect.svg"));
    actConnect_->setToolTip(actConnect_->text());
    tb->addSeparator();
    actDisconnect_ =
        tb->addAction(tr("Disconnect"), this, &MainWindow::disconnectSftp);
    actDisconnect_->setIcon(resIcon("action-disconnect.svg"));
    actDisconnect_->setToolTip(actDisconnect_->text());
    actDisconnect_->setEnabled(false);

    // Show text to the LEFT of the icon for Connect/Disconnect buttons only
    if (QWidget *w = tb->widgetForAction(actConnect_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Connect"));
        }
    }
    if (QWidget *w = tb->widgetForAction(actDisconnect_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Disconnect"));
        }
    }
    tb->addSeparator();
    actSites_ = tb->addAction(tr("Saved sites"), [this] {
        SiteManagerDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            openscp::SessionOptions opt{};
            if (dlg.selectedOptions(opt)) {
                startSftpConnect(opt);
            }
        }
    });
    actSites_->setIcon(resIcon("action-open-saved-sites.svg"));
    actSites_->setToolTip(actSites_->text());
    tb->addSeparator();
    actShowQueue_ =
        tb->addAction(tr("Transfers"), [this] { showTransferQueue(); });
    actShowQueue_->setIcon(resIcon("action-open-transfer-queue.svg"));
    actShowQueue_->setToolTip(actShowQueue_->text());
    tb->addSeparator();
    actShowHistory_ =
        tb->addAction(tr("History"), this, &MainWindow::showHistoryMenu);
    actShowHistory_->setIcon(resIcon("action-open-history.svg"));
    actShowHistory_->setToolTip(actShowHistory_->text());
    // Show text beside icon for Sites and Queue too
    if (QWidget *w = tb->widgetForAction(actSites_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Saved sites"));
        }
    }
    if (QWidget *w = tb->widgetForAction(actShowQueue_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Transfers"));
        }
    }
    if (QWidget *w = tb->widgetForAction(actShowHistory_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("History"));
        }
    }
    // Global shortcut to open the transfer queue
    actShowQueue_->setShortcut(QKeySequence(Qt::Key_F12));
    actShowQueue_->setShortcutContext(Qt::ApplicationShortcut);
    this->addAction(actShowQueue_);
    // Global shortcut to open recent history
    actShowHistory_->setShortcut(
        QKeySequence::fromString(QStringLiteral("Ctrl+Shift+H"),
                                 QKeySequence::PortableText));
    actShowHistory_->setShortcutContext(Qt::ApplicationShortcut);
    this->addAction(actShowHistory_);

    // Global fullscreen toggle (standard platform shortcut)
    // macOS: Ctrl+Cmd+F, Linux: F11
    {
        QAction *actToggleFs = new QAction(tr("Full screen"), this);
        actToggleFs->setShortcut(QKeySequence::FullScreen);
        actToggleFs->setShortcutContext(Qt::ApplicationShortcut);
        connect(actToggleFs, &QAction::triggered, this, [this] {
            const bool fs = (windowState() & Qt::WindowFullScreen);
            if (fs)
                setWindowState(windowState() & ~Qt::WindowFullScreen);
            else
                setWindowState(windowState() | Qt::WindowFullScreen);
        });
        this->addAction(actToggleFs);
    }
    // Separator to the right of the history button
    tb->addSeparator();
    // Queue is always enabled by default; no toggle

    // Spacer to push next action to the far right
    {
        QWidget *spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tb->addWidget(spacer);
    }
    // Visual separator before the right-side buttons
    tb->addSeparator();
    // About button (to the left of Settings)
    actAboutToolbar_ =
        tb->addAction(resIcon("action-open-about-us.svg"), tr("About OpenSCP"),
                      this, &MainWindow::showAboutDialog);
    if (actAboutToolbar_)
        actAboutToolbar_->setToolTip(actAboutToolbar_->text());
    // Settings button (far right)
    actPrefsToolbar_ =
        tb->addAction(resIcon("action-open-settings.svg"), tr("Settings"), this,
                      &MainWindow::showSettingsDialog);
    actPrefsToolbar_->setToolTip(actPrefsToolbar_->text());

    // Global shortcuts were already added to their respective actions

    // Menu bar (native on macOS)
    // Duplicate actions so users who prefer the classic menu can use it.
    appMenu_ = menuBar()->addMenu(tr("OpenSCP"));
    actAbout_ = appMenu_->addAction(tr("About OpenSCP"), this,
                                    &MainWindow::showAboutDialog);
    actAbout_->setMenuRole(QAction::AboutRole);
    actPrefs_ = appMenu_->addAction(tr("Settings…"), this,
                                    &MainWindow::showSettingsDialog);
    actPrefs_->setMenuRole(QAction::PreferencesRole);
    // Standard cross‑platform shortcut (Cmd+, on macOS; Ctrl+, on
    // Linux/Windows)
    actPrefs_->setShortcut(QKeySequence::Preferences);
    appMenu_->addSeparator();
    actQuit_ = appMenu_->addAction(tr("Quit"), qApp, &QApplication::quit);
    actQuit_->setMenuRole(QAction::QuitRole);
    // Standard quit shortcut (Cmd+Q / Ctrl+Q)
    actQuit_->setShortcut(QKeySequence::Quit);

    fileMenu_ = menuBar()->addMenu(tr("File"));
    fileMenu_->addAction(actChooseLeft_);
    fileMenu_->addAction(actChooseRight_);
    fileMenu_->addSeparator();
    fileMenu_->addAction(actConnect_);
    fileMenu_->addAction(actDisconnect_);
    fileMenu_->addAction(actSites_);
    fileMenu_->addAction(actShowQueue_);
    fileMenu_->addAction(actShowHistory_);
    // On non‑macOS platforms, also show Preferences and Quit under "File"
    // to provide a familiar UX on Linux/Windows while keeping the "OpenSCP" app
    // menu.
#ifndef Q_OS_MAC
    fileMenu_->addSeparator();
    fileMenu_->addAction(actPrefs_);
    fileMenu_->addAction(actQuit_);
#endif

    // Help (avoid native help menu to skip the search box)
    auto *helpMenu = menuBar()->addMenu(tr("Help"));
    // On macOS, a menu titled exactly "Help" triggers the native search bar.
    // Keep visible label "Help" but avoid detection by inserting a zero‑width
    // space.
#ifdef Q_OS_MAC
    {
        const QString t = helpMenu->title();
        if (t.compare(QStringLiteral("Help"), Qt::CaseInsensitive) == 0) {
            helpMenu->setTitle(QStringLiteral("Hel") + QChar(0x200B) +
                               QStringLiteral("p"));
        }
    }
#endif
    helpMenu->menuAction()->setMenuRole(QAction::NoRole);
    // Prevent macOS from moving actions to the app menu: force NoRole
    {
        QAction *helpAboutAct = new QAction(tr("About OpenSCP"), this);
        helpAboutAct->setMenuRole(QAction::NoRole);
        connect(helpAboutAct, &QAction::triggered, this,
                &MainWindow::showAboutDialog);
        helpMenu->addAction(helpAboutAct);
    }
    {
        QAction *reportAct = new QAction(tr("Report a bug"), this);
        reportAct->setMenuRole(QAction::NoRole);
        connect(reportAct, &QAction::triggered, this, [] {
            QDesktopServices::openUrl(
                QUrl("https://github.com/luiscuellar31/openscp/issues"));
        });
        helpMenu->addAction(reportAct);
    }

    // Double click/Enter navigation on both panes
    connect(rightView_, &QTreeView::activated, this,
            &MainWindow::rightItemActivated);
    connect(leftView_, &QTreeView::activated, this,
            &MainWindow::leftItemActivated);

    // Context menu on right pane
    rightView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rightView_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showRightContextMenu);
    if (rightView_->selectionModel()) {
        connect(rightView_->selectionModel(),
                &QItemSelectionModel::selectionChanged, this,
                [this] { updateDeleteShortcutEnables(); });
    }

    // Context menu on left pane (local)
    leftView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(leftView_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showLeftContextMenu);

    // Enable delete shortcut only when there is a selection on the left pane
    if (leftView_->selectionModel()) {
        connect(leftView_->selectionModel(),
                &QItemSelectionModel::selectionChanged, this,
                [this] { updateDeleteShortcutEnables(); });
    }

    {
        QSettings s("OpenSCP", "OpenSCP");
        downloadDir_ = defaultDownloadDirFromSettings(s);
    }
    QDir().mkpath(downloadDir_);

    initializeConnectionSessionIndicators();
    statusBar()->showMessage(tr("Ready"));
    setWindowTitle(tr("OpenSCP — local/local (click Connect for remote)"));
    resize(1100, 650);
    refreshLeftBreadcrumbs();
    refreshRightBreadcrumbs();
    restoreMainWindowUiState();

    // Transfer queue
    transferMgr_ = new TransferManager(this);
    connect(transferMgr_, &TransferManager::tasksChanged, this,
            [this] {
                maybeRefreshRemoteAfterCompletedUploads();
                maybeNotifyCompletedTransfers();
            });
    // Provide transfer manager to views (for async remote drag-out staging)
    if (auto *lv = qobject_cast<DragAwareTreeView *>(leftView_))
        lv->setTransferManager(transferMgr_);
    if (auto *rv = qobject_cast<DragAwareTreeView *>(rightView_))
        rv->setTransferManager(transferMgr_);

    // Startup cleanup (deferred): remove old staging batches if
    // autoCleanStaging is enabled
    QTimer::singleShot(0, this, [this] {
        QSettings s("OpenSCP", "OpenSCP");
        const bool autoClean =
            s.value("Advanced/autoCleanStaging", true).toBool();
        if (!autoClean)
            return;
        QString root = s.value("Advanced/stagingRoot").toString();
        if (root.isEmpty())
            root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
        QDir r(root);
        if (!r.exists())
            return;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const int retentionDays =
            qBound(1, s.value("Advanced/stagingRetentionDays", 7).toInt(),
                   365);
        const qint64 maxAgeMs = qint64(retentionDays) * 24 * 60 * 60 * 1000;
        // Match timestamp batches: yyyyMMdd-HHmmss
        QRegularExpression re("^\\d{8}-\\d{6}$");
        const auto entries = r.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot |
                                             QDir::NoSymLinks);
        for (const QFileInfo &fi : entries) {
            if (fi.isSymLink())
                continue; // do not follow symlinks
            if (!re.match(fi.fileName()).hasMatch())
                continue; // only batches
            const QDateTime m = fi.lastModified().toUTC();
            if (m.isValid() && m.msecsTo(now) > maxAgeMs) {
                QDir(fi.absoluteFilePath()).removeRecursively();
            }
        }
    });

    // Warn if insecure storage is active (non‑Apple only when explicitly
    // enabled)
    if (SecretStore::insecureFallbackActive()) {
        auto *warn = new QLabel(
            tr("Warning: unencrypted secrets storage active (fallback)"), this);
        warn->setStyleSheet(
            "QLabel{ color:#b00020; font-weight:bold; padding:2px 6px; }");
        warn->setToolTip(
            tr("You are using unencrypted credentials storage enabled via "
               "environment variable. Disable "
               "OPENSCP_ENABLE_INSECURE_FALLBACK to hide this warning."));
        statusBar()->addPermanentWidget(warn, 0);
    }

    // Apply user preferences (hidden files, click mode, etc.)
    applyPreferences();
    updateDeleteShortcutEnables();

    // Startup preferences and migration
    {
        QSettings s("OpenSCP", "OpenSCP");
        // One-shot migration: if only showConnOnStart exists, copy to
        // openSiteManagerOnDisconnect
        if (!s.contains("UI/openSiteManagerOnDisconnect") &&
            s.contains("UI/showConnOnStart")) {
            const bool v = s.value("UI/showConnOnStart", true).toBool();
            s.setValue("UI/openSiteManagerOnDisconnect", v);
            s.sync();
        }
        m_openSiteManagerOnStartup =
            s.value("UI/showConnOnStart", true).toBool();
        m_openSiteManagerOnDisconnect =
            s.value("UI/openSiteManagerOnDisconnect", true).toBool();
        if (m_openSiteManagerOnStartup && !QCoreApplication::closingDown() &&
            !sftp_) {
            QTimer::singleShot(0, this, [this] { showSiteManagerNonModal(); });
        }
    }
}

void MainWindow::initializeConnectionSessionIndicators() {
    if (!statusBar())
        return;

    if (!m_connectionTypeLabel_) {
        m_connectionTypeLabel_ = new QLabel(this);
        m_connectionTypeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_connectionTypeLabel_->setMinimumWidth(
            m_connectionTypeLabel_->fontMetrics().horizontalAdvance(
                tr("Type: HttpConnect")) +
            12);
        m_connectionTypeLabel_->setToolTip(
            tr("Active connection method for this session"));
        statusBar()->addPermanentWidget(m_connectionTypeLabel_);
    }

    if (!m_connectionElapsedLabel_) {
        m_connectionElapsedLabel_ = new QLabel(this);
        m_connectionElapsedLabel_->setAlignment(Qt::AlignRight |
                                                Qt::AlignVCenter);
        m_connectionElapsedLabel_->setMinimumWidth(
            m_connectionElapsedLabel_->fontMetrics().horizontalAdvance(
                tr("Session: 000:00:00")) +
            12);
        m_connectionElapsedLabel_->setToolTip(
            tr("Elapsed time for the current connection session"));
        statusBar()->addPermanentWidget(m_connectionElapsedLabel_);
    }

    if (!m_connectionElapsedTimer_) {
        m_connectionElapsedTimer_ = new QTimer(this);
        m_connectionElapsedTimer_->setInterval(1000);
        connect(m_connectionElapsedTimer_, &QTimer::timeout, this,
                &MainWindow::updateConnectionSessionIndicators);
    }

    resetConnectionSessionIndicators();
}

void MainWindow::startConnectionSessionIndicators(
    const QString &connectionType) {
    if (!m_connectionTypeLabel_ || !m_connectionElapsedLabel_ ||
        !m_connectionElapsedTimer_) {
        initializeConnectionSessionIndicators();
    }

    const QString normalizedType = connectionType.trimmed();
    m_activeConnectionType_ =
        normalizedType.isEmpty() ? tr("Unknown") : normalizedType;
    m_connectionStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    if (m_connectionElapsedTimer_)
        m_connectionElapsedTimer_->start();
    updateConnectionSessionIndicators();
}

void MainWindow::resetConnectionSessionIndicators() {
    m_activeConnectionType_.clear();
    m_connectionStartedAtMs_ = 0;
    if (m_connectionElapsedTimer_)
        m_connectionElapsedTimer_->stop();
    updateConnectionSessionIndicators();
}

void MainWindow::updateConnectionSessionIndicators() {
    if (m_connectionTypeLabel_) {
        const QString typeLabel =
            m_activeConnectionType_.isEmpty() ? tr("None")
                                              : m_activeConnectionType_;
        m_connectionTypeLabel_->setText(tr("Type: %1").arg(typeLabel));
    }

    if (m_connectionElapsedLabel_) {
        if (m_connectionStartedAtMs_ <= 0) {
            m_connectionElapsedLabel_->setText(tr("Session: --:--:--"));
        } else {
            const qint64 elapsedSeconds =
                qMax<qint64>(0, (QDateTime::currentMSecsSinceEpoch() -
                                 m_connectionStartedAtMs_) /
                                    1000);
            m_connectionElapsedLabel_->setText(
                tr("Session: %1")
                    .arg(formatConnectionElapsed(elapsedSeconds)));
        }
    }
}

// Show the application About dialog.
void MainWindow::showAboutDialog() {
    AboutDialog dlg(this);
    dlg.exec();
}

// Open the Settings dialog and apply changes when accepted.
void MainWindow::showSettingsDialog() {
    SettingsDialog dlg(this);
    connect(&dlg, &SettingsDialog::settingsApplied, this,
            &MainWindow::applyPreferences);
    dlg.exec();
    // Reflect any applied changes in the running UI
    applyPreferences();
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    if (firstShow_) {
        firstShow_ = false;
        if (m_restoredWindowGeometry_)
            return;
        QRect avail;
        if (this->screen())
            avail = this->screen()->availableGeometry();
        else if (auto ps = QGuiApplication::primaryScreen())
            avail = ps->availableGeometry();
        if (avail.isValid()) {
            const QSize sz = size();
            const int x = avail.center().x() - sz.width() / 2;
            const int y = avail.center().y() - sz.height() / 2;
            move(x, y);
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (m_isDisconnecting) {
        m_pendingCloseAfterDisconnect_ = true;
        e->ignore();
        return;
    }
    if (rightIsRemote_) {
        m_pendingCloseAfterDisconnect_ = true;
        disconnectSftp();
        e->ignore();
        return;
    }
    saveMainWindowUiState();
    QMainWindow::closeEvent(e);
}

void MainWindow::resetMainWindowLayoutToDefaults() {
    {
        QSettings s("OpenSCP", "OpenSCP");
        s.remove("UI/mainWindow/geometry");
        s.remove("UI/mainWindow/windowState");
        s.remove("UI/mainWindow/splitterState");
        s.remove("UI/mainWindow/leftHeaderState");
        s.remove("UI/mainWindow/rightHeaderLocal");
        s.remove("UI/mainWindow/rightHeaderRemote");
        s.sync();
    }

    m_restoredWindowGeometry_ = false;

    resize(1100, 650);
    if (mainSplitter_) {
        const int half = qMax(220, width() / 2);
        mainSplitter_->setSizes({half, half});
    }

    if (leftView_) {
        if (leftView_->header())
            leftView_->header()->setStretchLastSection(true);
        leftView_->setColumnWidth(0, 280);
    }
    if (rightView_) {
        if (rightIsRemote_) {
            if (rightView_->header())
                rightView_->header()->setStretchLastSection(false);
            rightView_->setColumnWidth(0, 300);
            rightView_->setColumnWidth(1, 120);
            rightView_->setColumnWidth(2, 180);
            rightView_->setColumnWidth(3, 120);
        } else {
            if (rightView_->header())
                rightView_->header()->setStretchLastSection(true);
            rightView_->setColumnWidth(0, 280);
        }
    }

    statusBar()->showMessage(tr("Window layout restored to defaults"), 3500);
}

void MainWindow::saveRightHeaderState(bool remoteMode) const {
    if (!rightView_ || !rightView_->header())
        return;
    QSettings s("OpenSCP", "OpenSCP");
    const QString key = remoteMode ? QStringLiteral("UI/mainWindow/rightHeaderRemote")
                                   : QStringLiteral("UI/mainWindow/rightHeaderLocal");
    s.setValue(key, rightView_->header()->saveState());
}

bool MainWindow::restoreRightHeaderState(bool remoteMode) {
    if (!rightView_ || !rightView_->header())
        return false;
    QSettings s("OpenSCP", "OpenSCP");
    const QString key = remoteMode ? QStringLiteral("UI/mainWindow/rightHeaderRemote")
                                   : QStringLiteral("UI/mainWindow/rightHeaderLocal");
    const QByteArray state = s.value(key).toByteArray();
    if (state.isEmpty())
        return false;
    return rightView_->header()->restoreState(state);
}

void MainWindow::saveMainWindowUiState() const {
    QSettings s("OpenSCP", "OpenSCP");
    s.setValue("UI/mainWindow/geometry", saveGeometry());
    s.setValue("UI/mainWindow/windowState", saveState());
    if (mainSplitter_)
        s.setValue("UI/mainWindow/splitterState", mainSplitter_->saveState());
    if (leftView_ && leftView_->header())
        s.setValue("UI/mainWindow/leftHeaderState",
                   leftView_->header()->saveState());
    saveRightHeaderState(rightIsRemote_);
    s.sync();
}

void MainWindow::restoreMainWindowUiState() {
    QSettings s("OpenSCP", "OpenSCP");
    const QByteArray geometry = s.value("UI/mainWindow/geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
        m_restoredWindowGeometry_ = true;
    }
    const QByteArray winState =
        s.value("UI/mainWindow/windowState").toByteArray();
    if (!winState.isEmpty())
        restoreState(winState);
    if (mainSplitter_) {
        const QByteArray splitterState =
            s.value("UI/mainWindow/splitterState").toByteArray();
        if (!splitterState.isEmpty())
            mainSplitter_->restoreState(splitterState);
    }
    if (leftView_ && leftView_->header()) {
        const QByteArray leftHeader =
            s.value("UI/mainWindow/leftHeaderState").toByteArray();
        if (!leftHeader.isEmpty())
            leftView_->header()->restoreState(leftHeader);
    }
    restoreRightHeaderState(false);
}

void MainWindow::rebuildLocalBreadcrumbs(QToolBar *bar, const QString &path,
                                         bool rightPane) {
    if (!bar)
        return;
    bar->clear();

    QString normalized = QDir::fromNativeSeparators(path.trimmed());
    if (normalized.isEmpty())
        normalized = QDir::homePath();
    if (!QFileInfo(normalized).isAbsolute()) {
        normalized = QDir::current().absoluteFilePath(normalized);
    }
    normalized = QDir::cleanPath(normalized);

    QVector<QPair<QString, QString>> crumbs;
#ifdef Q_OS_WIN
    if (normalized.size() >= 2 && normalized[1] == QLatin1Char(':')) {
        const QString drive = normalized.left(2);
        QString acc = drive + QLatin1Char('/');
        crumbs.push_back({drive, acc});
        const QStringList parts =
            normalized.mid(2).split('/', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (!acc.endsWith('/'))
                acc += QLatin1Char('/');
            acc += part;
            crumbs.push_back({part, acc});
        }
    } else
#endif
    {
        QString acc = QStringLiteral("/");
        crumbs.push_back({QStringLiteral("/"), acc});
        const QStringList parts = normalized.split('/', Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (!acc.endsWith('/'))
                acc += QLatin1Char('/');
            acc += part;
            crumbs.push_back({part, acc});
        }
    }

    for (int i = 0; i < crumbs.size(); ++i) {
        const QString label = crumbs[i].first;
        const QString target = crumbs[i].second;
        QAction *act = bar->addAction(label);
        act->setToolTip(target);
        connect(act, &QAction::triggered, this, [this, rightPane, target] {
            if (rightPane)
                setRightRoot(target);
            else
                setLeftRoot(target);
        });
        if (i + 1 < crumbs.size())
            bar->addSeparator();
    }
}

void MainWindow::rebuildRemoteBreadcrumbs(const QString &path) {
    if (!rightBreadcrumbsBar_)
        return;
    rightBreadcrumbsBar_->clear();

    QString normalized = path.trimmed();
    if (normalized.isEmpty())
        normalized = QStringLiteral("/");
    if (!normalized.startsWith('/'))
        normalized.prepend('/');
    if (normalized.size() > 1 && normalized.endsWith('/'))
        normalized.chop(1);

    QVector<QPair<QString, QString>> crumbs;
    QString acc = QStringLiteral("/");
    crumbs.push_back({QStringLiteral("/"), acc});
    const QStringList parts = normalized.split('/', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (!acc.endsWith('/'))
            acc += QLatin1Char('/');
        acc += part;
        crumbs.push_back({part, acc});
    }

    for (int i = 0; i < crumbs.size(); ++i) {
        const QString label = crumbs[i].first;
        const QString target = crumbs[i].second;
        QAction *act = rightBreadcrumbsBar_->addAction(label);
        act->setToolTip(target);
        connect(act, &QAction::triggered, this,
                [this, target] { setRightRemoteRoot(target); });
        if (i + 1 < crumbs.size())
            rightBreadcrumbsBar_->addSeparator();
    }
}

void MainWindow::refreshLeftBreadcrumbs() {
    rebuildLocalBreadcrumbs(leftBreadcrumbsBar_,
                            leftPath_ ? leftPath_->text() : QString(), false);
}

void MainWindow::refreshRightBreadcrumbs() {
    const QString path = rightPath_ ? rightPath_->text() : QString();
    if (rightIsRemote_)
        rebuildRemoteBreadcrumbs(path);
    else
        rebuildLocalBreadcrumbs(rightBreadcrumbsBar_, path, true);
}

void MainWindow::searchItemsInCurrentFolder(QTreeView *view,
                                            const QString &panelLabel) {
    if (!view || !view->model() || !view->selectionModel())
        return;

    PanelSearchPromptResult req;
    if (!promptPanelSearch(this, panelLabel, &req))
        return;

    QString regexError;
    const QRegularExpression regex = compilePanelSearchRegex(req.pattern, &regexError);
    if (!regex.isValid()) {
        QMessageBox::warning(
            this, tr("Invalid pattern"),
            tr("The pattern is not valid.\n%1")
                .arg(regexError.isEmpty() ? tr("Unknown regex error.")
                                          : regexError));
        return;
    }

    QAbstractItemModel *model = view->model();
    QItemSelectionModel *selection = view->selectionModel();

    if (!req.recursive) {
        const QModelIndex root = view->rootIndex();
        const int rows = model->rowCount(root);

        selection->clearSelection();
        QModelIndex firstMatch;
        int matches = 0;

        for (int row = 0; row < rows; ++row) {
            const QModelIndex idx = model->index(row, NAME_COL, root);
            QString itemName = model->data(idx, Qt::DisplayRole).toString();
            if (view == rightView_ && rightIsRemote_ && rightRemoteModel_) {
                itemName = rightRemoteModel_->nameAt(idx);
            } else if (itemName.endsWith('/')) {
                itemName.chop(1);
            }
            if (!regex.match(itemName).hasMatch())
                continue;

            selection->select(idx,
                              QItemSelectionModel::Select |
                                  QItemSelectionModel::Rows);
            if (!firstMatch.isValid())
                firstMatch = idx;
            ++matches;
        }

        if (firstMatch.isValid()) {
            selection->setCurrentIndex(firstMatch, QItemSelectionModel::NoUpdate);
            view->scrollTo(firstMatch, QAbstractItemView::PositionAtCenter);
            statusBar()->showMessage(
                tr("Found %1 match(es) in %2.")
                    .arg(QString::number(matches), panelLabel),
                4000);
        } else {
            statusBar()->showMessage(
                tr("No matches found in %1.").arg(panelLabel), 4000);
        }
        return;
    }

    static constexpr int kRecursiveSearchMaxMatches = 5000;
    static constexpr int kRecursiveSearchPumpEvery = 128;
    QVector<QPair<QString, bool>> recursiveMatches; // path, isDir
    int scanErrors = 0;
    bool canceled = false;
    bool truncated = false;

    if (view == rightView_ && rightIsRemote_ && (!rightRemoteModel_ || !sftp_)) {
        QMessageBox::warning(this, tr("Remote"),
                             tr("No active remote session."));
        return;
    }

    const bool isRemotePanelSearch =
        (view == rightView_ && rightIsRemote_ && rightRemoteModel_ && sftp_);
    QString basePathForSummary;

    if (isRemotePanelSearch) {
        QString baseRemote = rightRemoteModel_->rootPath().trimmed();
        if (baseRemote.isEmpty())
            baseRemote = QStringLiteral("/");
        baseRemote = normalizeRemotePathForMatch(baseRemote);
        basePathForSummary = baseRemote;

        QProgressDialog progress(
            tr("Searching recursively in %1...").arg(panelLabel), tr("Cancel"),
            0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);

        QSet<QString> visited;
        QVector<QString> stack;
        stack.push_back(baseRemote);
        qint64 pumped = 0;

        while (!stack.isEmpty()) {
            if (progress.wasCanceled()) {
                canceled = true;
                break;
            }

            const QString current = stack.back();
            stack.pop_back();
            const QString currentNorm = normalizeRemotePathForMatch(current);
            if (visited.contains(currentNorm))
                continue;
            visited.insert(currentNorm);

            progress.setLabelText(tr("Scanning %1").arg(currentNorm));
            QCoreApplication::processEvents();
            if (progress.wasCanceled()) {
                canceled = true;
                break;
            }

            std::vector<openscp::FileInfo> out;
            std::string err;
            if (!sftp_->list(currentNorm.toStdString(), out, err)) {
                ++scanErrors;
                continue;
            }

            for (const auto &entry : out) {
                const QString name = QString::fromStdString(entry.name);
                if (name.isEmpty() || name == QStringLiteral(".") ||
                    name == QStringLiteral("..")) {
                    continue;
                }
                if (!prefShowHidden_ && name.startsWith('.'))
                    continue;

                const bool isSymlink = (entry.mode & 0120000u) == 0120000u;
                const bool isDir = entry.is_dir;
                const QString child =
                    (currentNorm == QStringLiteral("/")
                         ? (QStringLiteral("/") + name)
                         : (currentNorm + QStringLiteral("/") + name));
                const QString childNorm = normalizeRemotePathForMatch(child);

                QString rel;
                if (baseRemote == QStringLiteral("/")) {
                    rel = childNorm.mid(1);
                } else if (childNorm.startsWith(baseRemote + QStringLiteral("/"))) {
                    rel = childNorm.mid(baseRemote.size() + 1);
                } else {
                    rel = childNorm;
                }
                if (rel.isEmpty())
                    rel = name;

                if (regex.match(name).hasMatch()) {
                    recursiveMatches.push_back({rel, isDir});
                    if (recursiveMatches.size() >= kRecursiveSearchMaxMatches) {
                        truncated = true;
                        break;
                    }
                }

                if (isDir && !isSymlink)
                    stack.push_back(childNorm);

                ++pumped;
                if ((pumped % kRecursiveSearchPumpEvery) == 0) {
                    QCoreApplication::processEvents();
                    if (progress.wasCanceled()) {
                        canceled = true;
                        break;
                    }
                }
            }

            if (canceled || truncated)
                break;
        }
    } else {
        QString baseLocal =
            (view == leftView_) ? (leftPath_ ? leftPath_->text() : QString())
                                : (rightPath_ ? rightPath_->text() : QString());
        baseLocal = QDir::cleanPath(baseLocal.trimmed());
        if (baseLocal.isEmpty() || !QDir(baseLocal).exists()) {
            QMessageBox::warning(this, tr("Invalid folder"),
                                 tr("The current folder does not exist."));
            return;
        }
        basePathForSummary = baseLocal;
        QDir baseDir(baseLocal);

        QDir::Filters filters =
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
        if (prefShowHidden_)
            filters |= QDir::Hidden;

        QProgressDialog progress(
            tr("Searching recursively in %1...").arg(panelLabel), tr("Cancel"),
            0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);

        QDirIterator it(baseLocal, filters, QDirIterator::Subdirectories);
        qint64 pumped = 0;
        while (it.hasNext()) {
            if (progress.wasCanceled()) {
                canceled = true;
                break;
            }

            const QString absPath = it.next();
            const QFileInfo fi = it.fileInfo();
            const QString name = fi.fileName();
            if (name.isEmpty() || name == QStringLiteral(".") ||
                name == QStringLiteral("..")) {
                continue;
            }

            if (regex.match(name).hasMatch()) {
                QString rel = QDir::fromNativeSeparators(
                    baseDir.relativeFilePath(absPath));
                if (rel.isEmpty())
                    rel = name;
                recursiveMatches.push_back({rel, fi.isDir()});
                if (recursiveMatches.size() >= kRecursiveSearchMaxMatches) {
                    truncated = true;
                    break;
                }
            }

            ++pumped;
            if ((pumped % kRecursiveSearchPumpEvery) == 0) {
                progress.setLabelText(tr("Scanning %1")
                                          .arg(QDir::fromNativeSeparators(
                                              baseDir.relativeFilePath(
                                                  fi.absoluteFilePath()))));
                QCoreApplication::processEvents();
                if (progress.wasCanceled()) {
                    canceled = true;
                    break;
                }
            }
        }
    }

    if (recursiveMatches.isEmpty()) {
        QString msg = canceled ? tr("Search canceled in %1.").arg(panelLabel)
                               : tr("No recursive matches found in %1.")
                                     .arg(panelLabel);
        if (scanErrors > 0) {
            msg += QStringLiteral("  ") +
                   tr("Folders with errors: %1")
                       .arg(QString::number(scanErrors));
        }
        statusBar()->showMessage(msg, 5000);
        return;
    }

    showRecursiveSearchResultsDialog(this, panelLabel, basePathForSummary,
                                     recursiveMatches, scanErrors, canceled,
                                     truncated);

    QString msg = tr("Found %1 recursive match(es) in %2.")
                      .arg(QString::number(recursiveMatches.size()), panelLabel);
    if (truncated) {
        msg += QStringLiteral("  ") +
               tr("Results limited to %1.")
                   .arg(QString::number(kRecursiveSearchMaxMatches));
    }
    if (scanErrors > 0) {
        msg += QStringLiteral("  ") +
               tr("Folders with errors: %1").arg(QString::number(scanErrors));
    }
    if (canceled)
        msg += QStringLiteral("  ") + tr("(Canceled)");
    statusBar()->showMessage(msg, 6000);
}

void MainWindow::maybeRefreshRemoteAfterCompletedUploads() {
    if (!transferMgr_) {
        m_seenCompletedUploadTaskIds_.clear();
        m_pendingRemoteRefreshFromUpload_ = false;
        return;
    }

    const auto tasks = transferMgr_->tasksSnapshot();
    QSet<quint64> activeUploadIds;
    activeUploadIds.reserve(tasks.size());

    bool shouldRefresh = false;
    const QString currentRoot =
        rightRemoteModel_ ? rightRemoteModel_->rootPath() : QString();

    for (const auto &t : tasks) {
        if (t.type != TransferTask::Type::Upload)
            continue;

        activeUploadIds.insert(t.id);
        if (t.status != TransferTask::Status::Done)
            continue;
        if (m_seenCompletedUploadTaskIds_.contains(t.id))
            continue;

        m_seenCompletedUploadTaskIds_.insert(t.id);
        if (rightIsRemote_ && rightRemoteModel_ &&
            remotePathIsInsideRoot(t.dst, currentRoot)) {
            shouldRefresh = true;
        }
    }

    for (auto it = m_seenCompletedUploadTaskIds_.begin();
         it != m_seenCompletedUploadTaskIds_.end();) {
        if (!activeUploadIds.contains(*it))
            it = m_seenCompletedUploadTaskIds_.erase(it);
        else
            ++it;
    }

    if (!shouldRefresh || m_pendingRemoteRefreshFromUpload_ || !rightIsRemote_ ||
        !rightRemoteModel_) {
        return;
    }

    m_pendingRemoteRefreshFromUpload_ = true;
    QTimer::singleShot(150, this, [this] {
        m_pendingRemoteRefreshFromUpload_ = false;
        if (!rightIsRemote_ || !rightRemoteModel_)
            return;
        QString err;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &err,
                                       true);
    });
}

void MainWindow::maybeNotifyCompletedTransfers() {
    if (!transferMgr_) {
        m_seenCompletedTransferNoticeTaskIds_.clear();
        return;
    }

    const auto tasks = transferMgr_->tasksSnapshot();
    QSet<quint64> activeTaskIds;
    activeTaskIds.reserve(tasks.size());

    QString message;
    int newlyCompleted = 0;
    for (const auto &t : tasks) {
        activeTaskIds.insert(t.id);
        if (t.status != TransferTask::Status::Done)
            continue;
        if (m_seenCompletedTransferNoticeTaskIds_.contains(t.id))
            continue;

        m_seenCompletedTransferNoticeTaskIds_.insert(t.id);
        ++newlyCompleted;
        if (newlyCompleted == 1) {
            const bool upload = (t.type == TransferTask::Type::Upload);
            const QString path = upload ? t.src : t.dst;
            QString name = QFileInfo(path).fileName();
            if (name.isEmpty())
                name = path;
            message = upload ? tr("Upload completed: %1").arg(name)
                             : tr("Download completed: %1").arg(name);
        }
    }

    for (auto it = m_seenCompletedTransferNoticeTaskIds_.begin();
         it != m_seenCompletedTransferNoticeTaskIds_.end();) {
        if (!activeTaskIds.contains(*it))
            it = m_seenCompletedTransferNoticeTaskIds_.erase(it);
        else
            ++it;
    }

    if (newlyCompleted == 0)
        return;
    if (newlyCompleted > 1)
        message = tr("%1 transfers completed").arg(newlyCompleted);
    statusBar()->showMessage(message, 5000);
}

bool MainWindow::isScpTransferMode() const {
    if (!rightIsRemote_ || !m_activeSessionOptions_.has_value())
        return false;
    const openscp::ProtocolCapabilities caps =
        openscp::capabilitiesForProtocol(m_activeSessionOptions_->protocol);
    return caps.supports_file_transfers && !caps.supports_listing;
}

void MainWindow::activateScpTransferModeUi(bool enabled) {
    if (!rightContentStack_ || !rightView_)
        return;
    if (enabled && scpTransferPanel_) {
        rightContentStack_->setCurrentWidget(scpTransferPanel_);
        if (rightPath_) {
            rightPath_->setPlaceholderText(tr("/remote/folder"));
            if (rightPath_->text().trimmed().isEmpty())
                rightPath_->setText(QStringLiteral("/"));
        }
        if (scpModeHintLabel_) {
            scpModeHintLabel_->setText(
                tr("This protocol works in transfer-only mode.\n"
                   "Uploads use the remote folder path above.\n"
                   "Downloads require entering a remote file path."));
        }
        return;
    }

    rightContentStack_->setCurrentWidget(rightView_);
    if (rightPath_)
        rightPath_->setPlaceholderText(QString());
}

void MainWindow::applyPreferences() {
    QSettings s("OpenSCP", "OpenSCP");
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    QString openBehaviorMode =
        s.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openBehaviorMode.isEmpty()) {
        const bool revealLegacy =
            s.value("UI/openRevealInFolder", false).toBool();
        openBehaviorMode =
            revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    if (openBehaviorMode != QStringLiteral("ask") &&
        openBehaviorMode != QStringLiteral("reveal") &&
        openBehaviorMode != QStringLiteral("open")) {
        openBehaviorMode = QStringLiteral("ask");
    }
    prefOpenBehaviorMode_ = openBehaviorMode;
    prefShowQueueOnEnqueue_ = s.value("UI/showQueueOnEnqueue", true).toBool();
    prefNoHostVerificationTtlMin_ = qBound(
        1, s.value("Security/noHostVerificationTtlMin", 15).toInt(), 120);
    m_remoteSessionHealthIntervalMs_ =
        qBound(60, s.value("Network/sessionHealthIntervalSec", 600).toInt(),
               86400) *
        1000;
    m_remoteWriteabilityTtlMs_ = qBound(
        1000, s.value("Network/remoteWriteabilityTtlMs", 15000).toInt(),
        120000);
    if (m_remoteSessionHealthTimer_) {
        m_remoteSessionHealthTimer_->setInterval(m_remoteSessionHealthIntervalMs_);
    }
    downloadDir_ = defaultDownloadDirFromSettings(s);
    QDir().mkpath(downloadDir_);
    if (actShowQueue_) {
        const QString queueShortcutText =
            s.value(kShortcutTransfersKey, QStringLiteral("F12"))
                .toString()
                .trimmed();
        actShowQueue_->setShortcut(
            QKeySequence::fromString(queueShortcutText,
                                     QKeySequence::PortableText));
        actShowQueue_->setShortcutContext(Qt::ApplicationShortcut);
    }
    if (actShowHistory_) {
        const QString historyShortcutText =
            s.value(kShortcutHistoryKey, QStringLiteral("Ctrl+Shift+H"))
                .toString()
                .trimmed();
        actShowHistory_->setShortcut(
            QKeySequence::fromString(historyShortcutText,
                                     QKeySequence::PortableText));
        actShowHistory_->setShortcutContext(Qt::ApplicationShortcut);
    }
    // Keep Site Manager auto-open preference up to date
    m_openSiteManagerOnDisconnect =
        s.value("UI/openSiteManagerOnDisconnect", true).toBool();
    applyTransferPreferences();

    // Local: model filters (hidden on/off)
    auto applyLocalFilters = [&](QFileSystemModel *m) {
        if (!m)
            return;
        QDir::Filters f =
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
        if (showHidden)
            f = f | QDir::Hidden | QDir::System;
        m->setFilter(f);
    };
    applyLocalFilters(leftModel_);
    applyLocalFilters(rightLocalModel_);

    // Remote: re-list with hidden filter
    prefShowHidden_ = showHidden;
    if (rightRemoteModel_) {
        rightRemoteModel_->setShowHidden(showHidden);
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
    }

    // Single-click activation (connect/disconnect to clicked())
    if (prefSingleClick_ != singleClick) {
        // Disconnect previous connections if they existed
        if (leftClickConn_) {
            QObject::disconnect(leftClickConn_);
            leftClickConn_ = QMetaObject::Connection();
        }
        if (rightClickConn_) {
            QObject::disconnect(rightClickConn_);
            rightClickConn_ = QMetaObject::Connection();
        }
        if (singleClick) {
            if (leftView_)
                leftClickConn_ = connect(leftView_, &QTreeView::clicked, this,
                                         &MainWindow::leftItemActivated);
            if (rightView_)
                rightClickConn_ = connect(rightView_, &QTreeView::clicked, this,
                                          &MainWindow::rightItemActivated);
        }
        prefSingleClick_ = singleClick;
    }
}

void MainWindow::applyTransferPreferences() {
    if (!transferMgr_)
        return;
    QSettings s("OpenSCP", "OpenSCP");
    const int maxConcurrent =
        qBound(1, s.value("Transfer/maxConcurrent", 2).toInt(), 8);
    const int globalSpeed =
        qMax(0, s.value("Transfer/globalSpeedKBps", 0).toInt());
    transferMgr_->setMaxConcurrent(maxConcurrent);
    transferMgr_->setGlobalSpeedLimitKBps(globalSpeed);
    if (transferDlg_)
        QMetaObject::invokeMethod(transferDlg_, "refresh",
                                  Qt::QueuedConnection);
}

void MainWindow::addRecentLocalPath(const QString &path) {
    const QString normalized = normalizedLocalHistoryPath(path);
    if (normalized.isEmpty())
        return;
    QSettings s("OpenSCP", "OpenSCP");
    QStringList recent = s.value(kRecentLocalPathsKey).toStringList();
    prependRecentValue(&recent, normalized);
    s.setValue(kRecentLocalPathsKey, recent);
}

void MainWindow::addRecentRemotePath(const QString &path) {
    const QString normalized = normalizeRemotePathForMatch(path);
    if (normalized.isEmpty())
        return;
    QSettings s("OpenSCP", "OpenSCP");
    QStringList recent = s.value(kRecentRemotePathsKey).toStringList();
    prependRecentValue(&recent, normalized);
    s.setValue(kRecentRemotePathsKey, recent);
}

void MainWindow::addRecentServer(const openscp::SessionOptions &opt) {
    const QString host = QString::fromStdString(opt.host).trimmed().toLower();
    if (host.isEmpty())
        return;
    const QString encoded = encodeRecentServerEntry(opt);
    if (encoded.isEmpty())
        return;
    QSettings s("OpenSCP", "OpenSCP");
    QStringList recent = s.value(kRecentServersKey).toStringList();
    prependRecentValue(&recent, encoded);
    s.setValue(kRecentServersKey, recent);
}

void MainWindow::showHistoryMenu() {
    QWidget *focusBeforeDialog = QApplication::focusWidget();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("History"));
    dlg.resize(720, 460);
    dlg.setMinimumSize(560, 360);

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *tabs = new QTabWidget(&dlg);
    layout->addWidget(tabs, 1);

    auto *localList = new QListWidget(tabs);
    localList->setSelectionMode(QAbstractItemView::SingleSelection);
    localList->setAlternatingRowColors(true);
    localList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tabs->addTab(localList, tr("Recent local paths"));

    auto *remoteList = new QListWidget(tabs);
    remoteList->setSelectionMode(QAbstractItemView::SingleSelection);
    remoteList->setAlternatingRowColors(true);
    remoteList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tabs->addTab(remoteList, tr("Recent remote paths"));

    auto *serverList = new QListWidget(tabs);
    serverList->setSelectionMode(QAbstractItemView::SingleSelection);
    serverList->setAlternatingRowColors(true);
    serverList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tabs->addTab(serverList, tr("Recent servers"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto *openBtn =
        buttons->addButton(tr("Open selected"), QDialogButtonBox::ActionRole);
    auto *clearBtn =
        buttons->addButton(tr("Clear history"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);

    auto addEmptyPlaceholder = [](QListWidget *list, const QString &text) {
        if (!list)
            return;
        auto *item = new QListWidgetItem(text, list);
        item->setFlags(Qt::NoItemFlags);
    };

    auto activeList = [tabs, localList, remoteList,
                       serverList]() -> QListWidget * {
        switch (tabs->currentIndex()) {
        case 0:
            return localList;
        case 1:
            return remoteList;
        case 2:
            return serverList;
        default:
            return nullptr;
        }
    };

    auto updateOpenEnabled = [activeList, openBtn]() {
        if (!openBtn)
            return;
        QListWidget *list = activeList();
        if (!list || !list->currentItem()) {
            openBtn->setEnabled(false);
            return;
        }
        const QString value = list->currentItem()->data(Qt::UserRole).toString();
        openBtn->setEnabled(!value.trimmed().isEmpty());
    };

    auto populate = [=]() {
        localList->clear();
        remoteList->clear();
        serverList->clear();

        QSettings s("OpenSCP", "OpenSCP");
        const QStringList localPaths = s.value(kRecentLocalPathsKey).toStringList();
        const QStringList remotePaths =
            s.value(kRecentRemotePathsKey).toStringList();
        const QStringList recentServers = s.value(kRecentServersKey).toStringList();

        bool hasEntries = false;

        for (const QString &rawPath : localPaths) {
            const QString normalized = normalizedLocalHistoryPath(rawPath);
            if (normalized.isEmpty())
                continue;
            auto *item = new QListWidgetItem(
                trimHistoryLabel(QDir::toNativeSeparators(normalized)), localList);
            item->setToolTip(normalized);
            item->setData(Qt::UserRole, normalized);
            hasEntries = true;
        }
        if (localList->count() == 0)
            addEmptyPlaceholder(localList, tr("No recent history"));

        for (const QString &rawPath : remotePaths) {
            const QString normalized = normalizeRemotePathForMatch(rawPath);
            if (normalized.isEmpty())
                continue;
            auto *item =
                new QListWidgetItem(trimHistoryLabel(normalized), remoteList);
            item->setToolTip(normalized);
            item->setData(Qt::UserRole, normalized);
            hasEntries = true;
        }
        if (remoteList->count() == 0)
            addEmptyPlaceholder(remoteList, tr("No recent history"));

        for (const QString &encoded : recentServers) {
            openscp::SessionOptions preset;
            QString label;
            if (!decodeRecentServerEntry(encoded, &preset, &label))
                continue;
            auto *item = new QListWidgetItem(trimHistoryLabel(label), serverList);
            item->setToolTip(label);
            item->setData(Qt::UserRole, encoded);
            hasEntries = true;
        }
        if (serverList->count() == 0)
            addEmptyPlaceholder(serverList, tr("No recent history"));

        if (clearBtn)
            clearBtn->setEnabled(hasEntries);
        updateOpenEnabled();
    };

    auto openSelected = [&, focusBeforeDialog]() {
        QListWidget *list = activeList();
        if (!list || !list->currentItem())
            return;
        const QString value = list->currentItem()->data(Qt::UserRole).toString();
        if (value.trimmed().isEmpty())
            return;

        switch (tabs->currentIndex()) {
        case 0: {
            const bool inRightPanel =
                focusWithinWidget(focusBeforeDialog, rightView_) ||
                focusWithinWidget(focusBeforeDialog, rightPath_) ||
                focusWithinWidget(focusBeforeDialog, rightPaneBar_) ||
                focusWithinWidget(focusBeforeDialog, rightBreadcrumbsBar_);
            if (!rightIsRemote_ && inRightPanel)
                setRightRoot(value);
            else
                setLeftRoot(value);
            dlg.accept();
            break;
        }
        case 1:
            if (!rightIsRemote_) {
                statusBar()->showMessage(
                    tr("Connect to a remote server to open remote path history."),
                    3500);
                return;
            }
            setRightRemoteRoot(value);
            dlg.accept();
            break;
        case 2: {
            if (rightIsRemote_) {
                statusBar()->showMessage(
                    tr("Disconnect the current remote session before opening "
                       "another server."),
                    4000);
                return;
            }
            openscp::SessionOptions preset;
            if (!decodeRecentServerEntry(value, &preset, nullptr))
                return;
            dlg.accept();
            openConnectDialogWithPreset(preset);
            break;
        }
        default:
            break;
        }
    };

    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(openBtn, &QPushButton::clicked, &dlg, openSelected);
    connect(clearBtn, &QPushButton::clicked, &dlg, [=, this]() {
        const auto ret = QMessageBox::question(
            this, tr("Clear history"),
            tr("Remove all recent paths and servers from history?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;
        QSettings s("OpenSCP", "OpenSCP");
        s.remove(kRecentLocalPathsKey);
        s.remove(kRecentRemotePathsKey);
        s.remove(kRecentServersKey);
        statusBar()->showMessage(tr("History cleared"), 3000);
        populate();
    });

    connect(localList, &QListWidget::itemDoubleClicked, &dlg,
            [openSelected](QListWidgetItem *) { openSelected(); });
    connect(remoteList, &QListWidget::itemDoubleClicked, &dlg,
            [openSelected](QListWidgetItem *) { openSelected(); });
    connect(serverList, &QListWidget::itemDoubleClicked, &dlg,
            [openSelected](QListWidgetItem *) { openSelected(); });

    connect(localList, &QListWidget::currentRowChanged, &dlg,
            [updateOpenEnabled](int) { updateOpenEnabled(); });
    connect(remoteList, &QListWidget::currentRowChanged, &dlg,
            [updateOpenEnabled](int) { updateOpenEnabled(); });
    connect(serverList, &QListWidget::currentRowChanged, &dlg,
            [updateOpenEnabled](int) { updateOpenEnabled(); });
    connect(tabs, &QTabWidget::currentChanged, &dlg,
            [updateOpenEnabled](int) { updateOpenEnabled(); });

    populate();
    dlg.exec();
}

void MainWindow::updateDeleteShortcutEnables() {
    auto hasColSel = [&](QTreeView *v) -> bool {
        if (!v || !v->selectionModel())
            return false;
        return !v->selectionModel()->selectedRows(NAME_COL).isEmpty();
    };
    const bool leftHasSel = hasColSel(leftView_);
    const bool rightHasSel = hasColSel(rightView_);
    const bool scpMode = isScpTransferMode();
    const bool rightWrite =
        (!rightIsRemote_) || (rightIsRemote_ && rightRemoteWritable_);

    // Left: enable according to selection (exception: Up)
    if (actCopyF5_)
        actCopyF5_->setEnabled(leftHasSel);
    if (actMoveF6_)
        actMoveF6_->setEnabled(leftHasSel && !scpMode);
    if (actDelete_)
        actDelete_->setEnabled(leftHasSel);
    if (actRenameLeft_)
        actRenameLeft_->setEnabled(leftHasSel);
    if (actNewDirLeft_)
        actNewDirLeft_->setEnabled(true); // always enabled on local
    if (actNewFileLeft_)
        actNewFileLeft_->setEnabled(true); // always enabled on local
    if (actUpLeft_) {
        QDir d(leftPath_ ? leftPath_->text() : QString());
        bool canUp = d.cdUp();
        actUpLeft_->setEnabled(canUp);
    }

    // Right: enable according to selection + permissions (exceptions: Up,
    // Upload, Download)
    if (scpMode) {
        if (actCopyRightTb_)
            actCopyRightTb_->setEnabled(false);
        if (actMoveRightTb_)
            actMoveRightTb_->setEnabled(false);
        if (actDeleteRight_)
            actDeleteRight_->setEnabled(false);
        if (actRenameRight_)
            actRenameRight_->setEnabled(false);
        if (actNewDirRight_)
            actNewDirRight_->setEnabled(false);
        if (actNewFileRight_)
            actNewFileRight_->setEnabled(false);
        if (actUploadRight_)
            actUploadRight_->setEnabled(true);
        if (actDownloadF7_)
            actDownloadF7_->setEnabled(true);
        if (actRefreshRight_)
            actRefreshRight_->setEnabled(false);
        if (actOpenTerminalRight_)
            actOpenTerminalRight_->setEnabled(true);
        if (actSearchRight_)
            actSearchRight_->setEnabled(false);
        if (actMoveRight_)
            actMoveRight_->setEnabled(false);
        if (actCopyRight_)
            actCopyRight_->setEnabled(false);
        if (actUpRight_) {
            QString cur = rightPath_ ? rightPath_->text().trimmed() : QString();
            if (cur.isEmpty())
                cur = QStringLiteral("/");
            if (cur.endsWith('/') && cur.size() > 1)
                cur.chop(1);
            actUpRight_->setEnabled(cur != "/");
        }
        return;
    }

    if (actCopyRightTb_)
        actCopyRightTb_->setEnabled(rightHasSel);
    if (actMoveRightTb_)
        actMoveRightTb_->setEnabled(rightHasSel && rightWrite);
    if (actDeleteRight_)
        actDeleteRight_->setEnabled(rightHasSel && rightWrite);
    if (actRenameRight_)
        actRenameRight_->setEnabled(rightHasSel && rightWrite);
    if (actNewDirRight_)
        actNewDirRight_->setEnabled(
            rightWrite); // enabled if local or remote is writable
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(rightWrite);
    if (actUploadRight_)
        actUploadRight_->setEnabled(rightIsRemote_ &&
                                    rightRemoteWritable_); // exception
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(
            rightIsRemote_); // exception: enabled without selection
    if (actRefreshRight_)
        actRefreshRight_->setEnabled(
            rightIsRemote_); // exception: enabled without selection
    if (actOpenTerminalRight_)
        actOpenTerminalRight_->setEnabled(
            rightIsRemote_ && m_activeSessionOptions_.has_value());
    if (actSearchRight_)
        actSearchRight_->setEnabled(true);
    if (actUpRight_) {
        QString cur = rightRemoteModel_ ? rightRemoteModel_->rootPath()
                                        : rightPath_->text();
        if (rightIsRemote_) {
            if (cur.endsWith('/'))
                cur.chop(1);
            actUpRight_->setEnabled(!cur.isEmpty() && cur != "/");
        } else {
            QDir d(cur);
            actUpRight_->setEnabled(d.cdUp());
        }
    }
}
