// MainWindow orchestrator: builds UI shell, wires actions, and applies
// preferences. Detailed local/remote/transfer logic lives in dedicated units.
#include "MainWindow.hpp"
#include "AboutDialog.hpp"
#include "ConnectionDialog.hpp"
#include "DragAwareTreeView.hpp"
#include "MainWindowSharedUtils.hpp"
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
    return normalizeRemotePath(rawPath);
}

static QIcon mainWindowActionIcon(const char *name) {
    return QIcon(QStringLiteral(":/assets/icons/") + QLatin1String(name));
}

static void setActionIconAndTooltip(QAction *action, const QIcon &icon) {
    if (!action)
        return;
    action->setIcon(icon);
    action->setToolTip(action->text());
}

static void bindActionToPanelShortcut(QAction *action, QWidget *panel,
                                      const QKeySequence &shortcut) {
    if (!action)
        return;
    action->setShortcut(shortcut);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (panel)
        panel->addAction(action);
}

static QListWidget *createHistoryTabList(QTabWidget *tabs,
                                         const QString &title) {
    if (!tabs)
        return nullptr;
    auto *list = new QListWidget(tabs);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setAlternatingRowColors(true);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tabs->addTab(list, title);
    return list;
}

static int selectRowsMatchingPattern(
    QTreeView *view, QAbstractItemModel *model, const QModelIndex &root,
    const QRegularExpression &regex,
    const std::function<QString(const QModelIndex &)> &nameForIndex) {
    if (!view || !model || !view->selectionModel())
        return 0;

    QItemSelectionModel *selection = view->selectionModel();
    selection->clearSelection();
    QModelIndex firstMatch;
    int matches = 0;

    const int rows = model->rowCount(root);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex idx = model->index(row, NAME_COL, root);
        const QString itemName = nameForIndex ? nameForIndex(idx) : QString();
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
    }
    return matches;
}

static void configurePanelTreeView(QTreeView *view, QAbstractItemModel *model,
                                   const QModelIndex &rootIndex) {
    if (!view || !model)
        return;
    view->setModel(model);
    view->setExpandsOnDoubleClick(false);
    view->setRootIndex(rootIndex);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSortingEnabled(true);
    view->sortByColumn(0, Qt::AscendingOrder);
    view->header()->setStretchLastSection(true);
    view->setColumnWidth(0, 280);
    view->setDragEnabled(true);
}

static void configurePanelDropTarget(QTreeView *view, QObject *eventFilterOwner) {
    if (!view)
        return;
    view->setAcceptDrops(true);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->viewport()->setAcceptDrops(true);
    view->setDefaultDropAction(Qt::CopyAction);
    if (eventFilterOwner)
        view->viewport()->installEventFilter(eventFilterOwner);
}

static QToolBar *createPaneIconToolbar(const QString &title, QWidget *parent) {
    auto *bar = new QToolBar(title, parent);
    bar->setIconSize(QSize(18, 18));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    return bar;
}

static QToolBar *createBreadcrumbToolbar(const QString &title, QWidget *parent) {
    auto *bar = new QToolBar(title, parent);
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    bar->setIconSize(QSize(14, 14));
    bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return bar;
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
    const QString home = preferredLocalHomePath();
    initializePanels(home);
    initializeMainToolbar();
    initializeMenuBarActions();
    initializePanelInteractions();
    initializeRuntimeState();
}

void MainWindow::initializePanels(const QString &home) {
    // Models
    leftModel_ = new QFileSystemModel(this);
    rightLocalModel_ = new QFileSystemModel(this);

    leftModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                          QDir::AllDirs);
    rightLocalModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                                QDir::AllDirs);

    // Initial paths: local home (fallback to root if HOME is unavailable)
    leftModel_->setRootPath(home);
    rightLocalModel_->setRootPath(home);

    // Views
    leftView_ = new DragAwareTreeView(this);
    rightView_ = new DragAwareTreeView(this);

    configurePanelTreeView(leftView_, leftModel_, leftModel_->index(home));
    configurePanelTreeView(rightView_, rightLocalModel_,
                           rightLocalModel_->index(home));
    configurePanelDropTarget(rightView_, this);
    configurePanelDropTarget(leftView_, this);

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
    leftPaneBar_ = createPaneIconToolbar(QStringLiteral("LeftBar"), leftPane);
    leftBreadcrumbsBar_ =
        createBreadcrumbToolbar(QStringLiteral("LeftBreadcrumbs"), leftPane);
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
    setActionIconAndTooltip(actUpLeft_, resIcon("action-go-up.svg"));
    // Button "Open left folder" next to Up
    actChooseLeft_ = leftPaneBar_->addAction(tr("Open left folder"), this,
                                             &MainWindow::chooseLeftDir);
    setActionIconAndTooltip(actChooseLeft_, resIcon("action-open-folder.svg"));
    actHomeLeft_ =
        leftPaneBar_->addAction(tr("Home"), this, &MainWindow::goHomeLeft);
    setActionIconAndTooltip(actHomeLeft_, resIcon("action-go-home.svg"));
    actSearchLeft_ =
        leftPaneBar_->addAction(tr("Search items"), this,
                                [this, leftSearchLabel] {
                                    searchItemsInCurrentFolder(
                                        leftView_, leftSearchLabel());
                                });
    setActionIconAndTooltip(actSearchLeft_, resIcon("action-search-item.svg"));
    leftPaneBar_->addSeparator();
    actCopyF5_ =
        leftPaneBar_->addAction(tr("Copy"), this, &MainWindow::copyLeftToRight);
    setActionIconAndTooltip(actCopyF5_, resIcon("action-copy.svg"));
    // Shortcut F5 on left panel (scope: left view and its children)
    bindActionToPanelShortcut(actCopyF5_, leftView_, QKeySequence(Qt::Key_F5));
    actMoveF6_ =
        leftPaneBar_->addAction(tr("Move"), this, &MainWindow::moveLeftToRight);
    setActionIconAndTooltip(actMoveF6_, resIcon("action-move-to-right.svg"));
    // Shortcut F6 on left panel
    bindActionToPanelShortcut(actMoveF6_, leftView_, QKeySequence(Qt::Key_F6));
    actDelete_ = leftPaneBar_->addAction(tr("Delete"), this,
                                         &MainWindow::deleteFromLeft);
    setActionIconAndTooltip(actDelete_, resIcon("action-delete.svg"));
    bindActionToPanelShortcut(actDelete_, leftView_, QKeySequence(Qt::Key_Delete));
    // Action: copy from right panel to left (remote/local -> left)
    actCopyRight_ = new QAction(tr("Copy to left panel"), this);
    connect(actCopyRight_, &QAction::triggered, this,
            &MainWindow::copyRightToLeft);
    setActionIconAndTooltip(
        actCopyRight_, QIcon(QLatin1String(":/assets/icons/action-copy.svg")));
    // Action: move from right panel to left
    actMoveRight_ = new QAction(tr("Move to left panel"), this);
    connect(actMoveRight_, &QAction::triggered, this,
            &MainWindow::moveRightToLeft);
    setActionIconAndTooltip(
        actMoveRight_,
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
    setActionIconAndTooltip(actRenameLeft_, resIcon("action-rename.svg"));
    setActionIconAndTooltip(actNewDirLeft_, resIcon("action-new-folder.svg"));
    setActionIconAndTooltip(actNewFileLeft_, resIcon("action-new-file.svg"));
    // Shortcuts (left panel scope)
    bindActionToPanelShortcut(actRenameLeft_, leftView_, QKeySequence(Qt::Key_F2));
    bindActionToPanelShortcut(actNewDirLeft_, leftView_, QKeySequence(Qt::Key_F9));
    bindActionToPanelShortcut(actNewFileLeft_, leftView_,
                              QKeySequence(Qt::Key_F10));
    leftPaneBar_->addAction(actRenameLeft_);
    leftPaneBar_->addAction(actNewFileLeft_);
    leftPaneBar_->addAction(actNewDirLeft_);
    leftLayout->addWidget(leftPaneBar_);

    // Left panel: toolbar -> breadcrumbs -> path -> view
    leftLayout->addWidget(leftBreadcrumbsBar_);
    leftLayout->addWidget(leftPath_);
    leftLayout->addWidget(leftView_);

    // Right pane sub‑toolbar
    rightPaneBar_ = createPaneIconToolbar(QStringLiteral("RightBar"), rightPane);
    rightBreadcrumbsBar_ =
        createBreadcrumbToolbar(QStringLiteral("RightBreadcrumbs"), rightPane);
    actUpRight_ =
        rightPaneBar_->addAction(tr("Up"), this, &MainWindow::goUpRight);
    setActionIconAndTooltip(actUpRight_, resIcon("action-go-up.svg"));
    // Button "Open right folder" next to Up
    actChooseRight_ = rightPaneBar_->addAction(tr("Open right folder"), this,
                                               &MainWindow::chooseRightDir);
    setActionIconAndTooltip(actChooseRight_, resIcon("action-open-folder.svg"));
    actHomeRight_ =
        rightPaneBar_->addAction(tr("Home"), this, &MainWindow::goHomeRight);
    setActionIconAndTooltip(actHomeRight_, resIcon("action-go-home.svg"));
    actSearchRight_ =
        rightPaneBar_->addAction(tr("Search items"), this,
                                 [this, rightSearchLabel] {
                                     searchItemsInCurrentFolder(
                                         rightView_, rightSearchLabel());
                                 });
    setActionIconAndTooltip(actSearchRight_, resIcon("action-search-item.svg"));

    // Right panel actions (create first, then add in requested order)
    actDownloadF7_ = new QAction(tr("Download"), this);
    connect(actDownloadF7_, &QAction::triggered, this,
            &MainWindow::downloadRightToLeft);
    actDownloadF7_->setEnabled(false); // starts disabled on local
    setActionIconAndTooltip(actDownloadF7_, resIcon("action-download.svg"));

    actUploadRight_ = new QAction(tr("Upload…"), this);
    connect(actUploadRight_, &QAction::triggered, this,
            &MainWindow::uploadViaDialog);
    setActionIconAndTooltip(actUploadRight_, resIcon("action-upload.svg"));
    // Shortcut F8 on right panel to upload via dialog (remote only)
    bindActionToPanelShortcut(actUploadRight_, rightView_,
                              QKeySequence(Qt::Key_F8));

    actRefreshRight_ = new QAction(tr("Refresh"), this);
    connect(actRefreshRight_, &QAction::triggered, this,
            &MainWindow::refreshRightRemotePanel);
    setActionIconAndTooltip(actRefreshRight_, resIcon("action-refresh.svg"));

    actOpenTerminalRight_ = new QAction(tr("Open in terminal"), this);
    connect(actOpenTerminalRight_, &QAction::triggered, this,
            &MainWindow::openRightRemoteTerminal);
    setActionIconAndTooltip(actOpenTerminalRight_,
                            resIcon("action-open-terminal.svg"));

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
    setActionIconAndTooltip(actNewDirRight_, resIcon("action-new-folder.svg"));
    setActionIconAndTooltip(actRenameRight_, resIcon("action-rename.svg"));
    setActionIconAndTooltip(actDeleteRight_, resIcon("action-delete.svg"));
    setActionIconAndTooltip(actNewFileRight_, resIcon("action-new-file.svg"));
    // Shortcuts (right panel scope)
    bindActionToPanelShortcut(actRenameRight_, rightView_,
                              QKeySequence(Qt::Key_F2));
    bindActionToPanelShortcut(actNewDirRight_, rightView_,
                              QKeySequence(Qt::Key_F9));
    bindActionToPanelShortcut(actNewFileRight_, rightView_,
                              QKeySequence(Qt::Key_F10));

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
    setActionIconAndTooltip(actCopyRightTb_, resIcon("action-copy.svg"));
    setActionIconAndTooltip(actMoveRightTb_, resIcon("action-move-to-left.svg"));
    // Shortcuts F5/F6 on right panel (scope: right view)
    bindActionToPanelShortcut(actCopyRightTb_, rightView_,
                              QKeySequence(Qt::Key_F5));
    bindActionToPanelShortcut(actMoveRightTb_, rightView_,
                              QKeySequence(Qt::Key_F6));
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
        bindActionToPanelShortcut(actDeleteRight_, rightView_,
                                  QKeySequence(Qt::Key_Delete));
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

}

void MainWindow::initializeMainToolbar() {
    auto *mainToolbar = addToolBar("Main");
    mainToolbar->setObjectName("mainToolbar");
    mainToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    mainToolbar->setMovable(false);
    // Keep the system default size for the main toolbar and make sub‑toolbars
    // slightly smaller
    const int mainIconPx =
        mainToolbar->style()->pixelMetric(QStyle::PM_ToolBarIconSize, nullptr,
                                          mainToolbar);
    const int subIconPx =
        qMax(16, mainIconPx - 4); // sub‑toolbars slightly smaller
    leftPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    rightPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    // Copy/move/delete actions now live in the left sub‑toolbar
    actConnect_ =
        mainToolbar->addAction(tr("Connect"), this, &MainWindow::connectSftp);
    actConnect_->setIcon(mainWindowActionIcon("action-connect.svg"));
    actConnect_->setToolTip(actConnect_->text());
    mainToolbar->addSeparator();
    actDisconnect_ =
        mainToolbar->addAction(tr("Disconnect"), this,
                               &MainWindow::disconnectSftp);
    actDisconnect_->setIcon(mainWindowActionIcon("action-disconnect.svg"));
    actDisconnect_->setToolTip(actDisconnect_->text());
    actDisconnect_->setEnabled(false);

    auto setTextBesideIcon = [mainToolbar](QAction *action, const QString &text) {
        if (!mainToolbar || !action)
            return;
        if (QWidget *widget = mainToolbar->widgetForAction(action)) {
            if (auto *button = qobject_cast<QToolButton *>(widget)) {
                button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                button->setText(text);
            }
        }
    };
    // Show text to the LEFT of the icon for Connect/Disconnect buttons only
    setTextBesideIcon(actConnect_, tr("Connect"));
    setTextBesideIcon(actDisconnect_, tr("Disconnect"));
    mainToolbar->addSeparator();
    actSites_ = mainToolbar->addAction(tr("Saved sites"), [this] {
        SiteManagerDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            openscp::SessionOptions opt{};
            if (dlg.selectedOptions(opt)) {
                startSftpConnect(opt);
            }
        }
    });
    actSites_->setIcon(mainWindowActionIcon("action-open-saved-sites.svg"));
    actSites_->setToolTip(actSites_->text());
    mainToolbar->addSeparator();
    actShowQueue_ =
        mainToolbar->addAction(tr("Transfers"),
                               [this] { showTransferQueue(); });
    actShowQueue_->setIcon(
        mainWindowActionIcon("action-open-transfer-queue.svg"));
    actShowQueue_->setToolTip(actShowQueue_->text());
    mainToolbar->addSeparator();
    actShowHistory_ =
        mainToolbar->addAction(tr("History"), this, &MainWindow::showHistoryMenu);
    actShowHistory_->setIcon(mainWindowActionIcon("action-open-history.svg"));
    actShowHistory_->setToolTip(actShowHistory_->text());

    // Show text beside icon for Sites and Queue too
    setTextBesideIcon(actSites_, tr("Saved sites"));
    setTextBesideIcon(actShowQueue_, tr("Transfers"));
    setTextBesideIcon(actShowHistory_, tr("History"));

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
            const bool isFullScreen = (windowState() & Qt::WindowFullScreen);
            if (isFullScreen)
                setWindowState(windowState() & ~Qt::WindowFullScreen);
            else
                setWindowState(windowState() | Qt::WindowFullScreen);
        });
        this->addAction(actToggleFs);
    }

    // Separator to the right of the history button
    mainToolbar->addSeparator();

    // Spacer to push next action to the far right
    {
        QWidget *spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        mainToolbar->addWidget(spacer);
    }

    // Visual separator before the right-side buttons
    mainToolbar->addSeparator();
    // About button (to the left of Settings)
    actAboutToolbar_ = mainToolbar->addAction(
        mainWindowActionIcon("action-open-about-us.svg"), tr("About OpenSCP"),
        this, &MainWindow::showAboutDialog);
    if (actAboutToolbar_)
        actAboutToolbar_->setToolTip(actAboutToolbar_->text());
    // Settings button (far right)
    actPrefsToolbar_ = mainToolbar->addAction(
        mainWindowActionIcon("action-open-settings.svg"), tr("Settings"), this,
        &MainWindow::showSettingsDialog);
    actPrefsToolbar_->setToolTip(actPrefsToolbar_->text());
}

void MainWindow::initializeMenuBarActions() {
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
        const QString helpMenuTitle = helpMenu->title();
        if (helpMenuTitle.compare(QStringLiteral("Help"),
                                  Qt::CaseInsensitive) == 0) {
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
}

void MainWindow::initializePanelInteractions() {
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
}

void MainWindow::initializeRuntimeState() {
    {
        QSettings settings("OpenSCP", "OpenSCP");
        downloadDir_ = defaultDownloadDirFromSettings(settings);
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
    if (auto *leftDragView = qobject_cast<DragAwareTreeView *>(leftView_))
        leftDragView->setTransferManager(transferMgr_);
    if (auto *rightDragView = qobject_cast<DragAwareTreeView *>(rightView_))
        rightDragView->setTransferManager(transferMgr_);

    // Startup cleanup (deferred): remove old staging batches if
    // autoCleanStaging is enabled
    QTimer::singleShot(0, this, [this] {
        QSettings settings("OpenSCP", "OpenSCP");
        const bool autoClean =
            settings.value("Advanced/autoCleanStaging", true).toBool();
        if (!autoClean)
            return;
        QString root = settings.value("Advanced/stagingRoot").toString();
        if (root.isEmpty())
            root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
        QDir stagingRootDir(root);
        if (!stagingRootDir.exists())
            return;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const int retentionDays =
            qBound(1, settings.value("Advanced/stagingRetentionDays", 7).toInt(),
                   365);
        const qint64 maxAgeMs = qint64(retentionDays) * 24 * 60 * 60 * 1000;
        // Match timestamp batches: yyyyMMdd-HHmmss
        QRegularExpression re("^\\d{8}-\\d{6}$");
        const auto entries =
            stagingRootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot |
                                         QDir::NoSymLinks);
        for (const QFileInfo &entryInfo : entries) {
            if (entryInfo.isSymLink())
                continue; // do not follow symlinks
            if (!re.match(entryInfo.fileName()).hasMatch())
                continue; // only batches
            const QDateTime lastModifiedUtc = entryInfo.lastModified().toUTC();
            if (lastModifiedUtc.isValid() &&
                lastModifiedUtc.msecsTo(now) > maxAgeMs) {
                QDir(entryInfo.absoluteFilePath()).removeRecursively();
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
        QSettings settings("OpenSCP", "OpenSCP");
        // One-shot migration: if only showConnOnStart exists, copy to
        // openSiteManagerOnDisconnect
        if (!settings.contains("UI/openSiteManagerOnDisconnect") &&
            settings.contains("UI/showConnOnStart")) {
            const bool showSiteManagerOnStart =
                settings.value("UI/showConnOnStart", true).toBool();
            settings.setValue("UI/openSiteManagerOnDisconnect",
                              showSiteManagerOnStart);
            settings.sync();
        }
        m_openSiteManagerOnStartup =
            settings.value("UI/showConnOnStart", true).toBool();
        m_openSiteManagerOnDisconnect =
            settings.value("UI/openSiteManagerOnDisconnect", true).toBool();
        if (m_openSiteManagerOnStartup && !QCoreApplication::closingDown() &&
            !sftp_) {
            QTimer::singleShot(0, this, [this] { showSiteManagerNonModal(); });
        }
    }
}

void MainWindow::rebuildContextMenu(QMenu *menu,
                                    const QVector<QAction *> &entries) const {
    if (!menu)
        return;

    menu->clear();
    bool lastWasSeparator = true;
    for (QAction *entry : entries) {
        if (!entry) {
            if (!lastWasSeparator && !menu->actions().isEmpty()) {
                menu->addSeparator();
                lastWasSeparator = true;
            }
            continue;
        }
        menu->addAction(entry);
        lastWasSeparator = false;
    }

    const QList<QAction *> actions = menu->actions();
    if (!actions.isEmpty() && actions.back()->isSeparator())
        menu->removeAction(actions.back());
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
        else if (auto primaryScreen = QGuiApplication::primaryScreen())
            avail = primaryScreen->availableGeometry();
        if (avail.isValid()) {
            const QSize sz = size();
            const int centeredX = avail.center().x() - sz.width() / 2;
            const int centeredY = avail.center().y() - sz.height() / 2;
            move(centeredX, centeredY);
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
        QSettings settings("OpenSCP", "OpenSCP");
        settings.remove("UI/mainWindow/geometry");
        settings.remove("UI/mainWindow/windowState");
        settings.remove("UI/mainWindow/splitterState");
        settings.remove("UI/mainWindow/leftHeaderState");
        settings.remove("UI/mainWindow/rightHeaderLocal");
        settings.remove("UI/mainWindow/rightHeaderRemote");
        settings.sync();
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
    QSettings settings("OpenSCP", "OpenSCP");
    const QString key = remoteMode ? QStringLiteral("UI/mainWindow/rightHeaderRemote")
                                   : QStringLiteral("UI/mainWindow/rightHeaderLocal");
    settings.setValue(key, rightView_->header()->saveState());
}

bool MainWindow::restoreRightHeaderState(bool remoteMode) {
    if (!rightView_ || !rightView_->header())
        return false;
    QSettings settings("OpenSCP", "OpenSCP");
    const QString key = remoteMode ? QStringLiteral("UI/mainWindow/rightHeaderRemote")
                                   : QStringLiteral("UI/mainWindow/rightHeaderLocal");
    const QByteArray state = settings.value(key).toByteArray();
    if (state.isEmpty())
        return false;
    return rightView_->header()->restoreState(state);
}

void MainWindow::saveMainWindowUiState() const {
    QSettings settings("OpenSCP", "OpenSCP");
    settings.setValue("UI/mainWindow/geometry", saveGeometry());
    settings.setValue("UI/mainWindow/windowState", saveState());
    if (mainSplitter_)
        settings.setValue("UI/mainWindow/splitterState", mainSplitter_->saveState());
    if (leftView_ && leftView_->header())
        settings.setValue("UI/mainWindow/leftHeaderState",
                          leftView_->header()->saveState());
    saveRightHeaderState(rightIsRemote_);
    settings.sync();
}

void MainWindow::restoreMainWindowUiState() {
    QSettings settings("OpenSCP", "OpenSCP");
    const QByteArray geometry =
        settings.value("UI/mainWindow/geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
        m_restoredWindowGeometry_ = true;
    }
    const QByteArray winState =
        settings.value("UI/mainWindow/windowState").toByteArray();
    if (!winState.isEmpty())
        restoreState(winState);
    if (mainSplitter_) {
        const QByteArray splitterState =
            settings.value("UI/mainWindow/splitterState").toByteArray();
        if (!splitterState.isEmpty())
            mainSplitter_->restoreState(splitterState);
    }
    if (leftView_ && leftView_->header()) {
        const QByteArray leftHeader =
            settings.value("UI/mainWindow/leftHeaderState").toByteArray();
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

    for (int crumbIndex = 0; crumbIndex < crumbs.size(); ++crumbIndex) {
        const QString label = crumbs[crumbIndex].first;
        const QString target = crumbs[crumbIndex].second;
        QAction *act = bar->addAction(label);
        act->setToolTip(target);
        connect(act, &QAction::triggered, this, [this, rightPane, target] {
            if (rightPane)
                setRightRoot(target);
            else
                setLeftRoot(target);
        });
        if (crumbIndex + 1 < crumbs.size())
            bar->addSeparator();
    }
}

void MainWindow::rebuildRemoteBreadcrumbs(const QString &path) {
    if (!rightBreadcrumbsBar_)
        return;
    rightBreadcrumbsBar_->clear();

    const QString normalized = normalizeRemotePath(path);

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

    for (int crumbIndex = 0; crumbIndex < crumbs.size(); ++crumbIndex) {
        const QString label = crumbs[crumbIndex].first;
        const QString target = crumbs[crumbIndex].second;
        QAction *act = rightBreadcrumbsBar_->addAction(label);
        act->setToolTip(target);
        connect(act, &QAction::triggered, this,
                [this, target] { setRightRemoteRoot(target); });
        if (crumbIndex + 1 < crumbs.size())
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

    if (!req.recursive) {
        const QModelIndex root = view->rootIndex();
        const int matches = selectRowsMatchingPattern(
            view, model, root, regex, [&](const QModelIndex &idx) {
                QString itemName = model->data(idx, Qt::DisplayRole).toString();
                if (view == rightView_ && rightIsRemote_ && rightRemoteModel_) {
                    itemName = rightRemoteModel_->nameAt(idx);
                } else if (itemName.endsWith('/')) {
                    itemName.chop(1);
                }
                return itemName;
            });

        if (matches > 0) {
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

        QDirIterator dirIterator(baseLocal, filters, QDirIterator::Subdirectories);
        qint64 pumped = 0;
        while (dirIterator.hasNext()) {
            if (progress.wasCanceled()) {
                canceled = true;
                break;
            }

            const QString absPath = dirIterator.next();
            const QFileInfo fileInfo = dirIterator.fileInfo();
            const QString name = fileInfo.fileName();
            if (name.isEmpty() || name == QStringLiteral(".") ||
                name == QStringLiteral("..")) {
                continue;
            }

            if (regex.match(name).hasMatch()) {
                QString rel = QDir::fromNativeSeparators(
                    baseDir.relativeFilePath(absPath));
                if (rel.isEmpty())
                    rel = name;
                recursiveMatches.push_back({rel, fileInfo.isDir()});
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
                                                  fileInfo.absoluteFilePath()))));
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

    for (const auto &task : tasks) {
        if (task.type != TransferTask::Type::Upload)
            continue;

        activeUploadIds.insert(task.taskId);
        if (task.status != TransferTask::Status::Done)
            continue;
        if (m_seenCompletedUploadTaskIds_.contains(task.taskId))
            continue;

        m_seenCompletedUploadTaskIds_.insert(task.taskId);
        if (rightIsRemote_ && rightRemoteModel_ &&
            remotePathIsInsideRoot(task.dst, currentRoot)) {
            shouldRefresh = true;
        }
    }

    for (auto completedIdIt = m_seenCompletedUploadTaskIds_.begin();
         completedIdIt != m_seenCompletedUploadTaskIds_.end();) {
        if (!activeUploadIds.contains(*completedIdIt))
            completedIdIt = m_seenCompletedUploadTaskIds_.erase(completedIdIt);
        else
            ++completedIdIt;
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
    for (const auto &task : tasks) {
        activeTaskIds.insert(task.taskId);
        if (task.status != TransferTask::Status::Done)
            continue;
        if (m_seenCompletedTransferNoticeTaskIds_.contains(task.taskId))
            continue;

        m_seenCompletedTransferNoticeTaskIds_.insert(task.taskId);
        ++newlyCompleted;
        if (newlyCompleted == 1) {
            const bool upload = (task.type == TransferTask::Type::Upload);
            const QString path = upload ? task.src : task.dst;
            QString name = QFileInfo(path).fileName();
            if (name.isEmpty())
                name = path;
            message = upload ? tr("Upload completed: %1").arg(name)
                             : tr("Download completed: %1").arg(name);
        }
    }

    for (auto completedIdIt = m_seenCompletedTransferNoticeTaskIds_.begin();
         completedIdIt != m_seenCompletedTransferNoticeTaskIds_.end();) {
        if (!activeTaskIds.contains(*completedIdIt))
            completedIdIt =
                m_seenCompletedTransferNoticeTaskIds_.erase(completedIdIt);
        else
            ++completedIdIt;
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
    QSettings settings("OpenSCP", "OpenSCP");
    const bool showHidden = settings.value("UI/showHidden", false).toBool();
    const bool singleClick = settings.value("UI/singleClick", false).toBool();
    QString openBehaviorMode =
        settings.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openBehaviorMode.isEmpty()) {
        const bool revealLegacy =
            settings.value("UI/openRevealInFolder", false).toBool();
        openBehaviorMode =
            revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    if (openBehaviorMode != QStringLiteral("ask") &&
        openBehaviorMode != QStringLiteral("reveal") &&
        openBehaviorMode != QStringLiteral("open")) {
        openBehaviorMode = QStringLiteral("ask");
    }
    prefOpenBehaviorMode_ = openBehaviorMode;
    prefShowQueueOnEnqueue_ =
        settings.value("UI/showQueueOnEnqueue", true).toBool();
    prefNoHostVerificationTtlMin_ = qBound(
        1, settings.value("Security/noHostVerificationTtlMin", 15).toInt(), 120);
    m_remoteSessionHealthIntervalMs_ =
        qBound(60, settings.value("Network/sessionHealthIntervalSec", 600).toInt(),
               86400) *
        1000;
    m_remoteWriteabilityTtlMs_ = qBound(
        1000, settings.value("Network/remoteWriteabilityTtlMs", 15000).toInt(),
        120000);
    if (m_remoteSessionHealthTimer_) {
        m_remoteSessionHealthTimer_->setInterval(m_remoteSessionHealthIntervalMs_);
    }
    downloadDir_ = defaultDownloadDirFromSettings(settings);
    QDir().mkpath(downloadDir_);
    if (actShowQueue_) {
        const QString queueShortcutText =
            settings.value(kShortcutTransfersKey, QStringLiteral("F12"))
                .toString()
                .trimmed();
        actShowQueue_->setShortcut(
            QKeySequence::fromString(queueShortcutText,
                                     QKeySequence::PortableText));
        actShowQueue_->setShortcutContext(Qt::ApplicationShortcut);
    }
    if (actShowHistory_) {
        const QString historyShortcutText =
            settings.value(kShortcutHistoryKey, QStringLiteral("Ctrl+Shift+H"))
                .toString()
                .trimmed();
        actShowHistory_->setShortcut(
            QKeySequence::fromString(historyShortcutText,
                                     QKeySequence::PortableText));
        actShowHistory_->setShortcutContext(Qt::ApplicationShortcut);
    }
    // Keep Site Manager auto-open preference up to date
    m_openSiteManagerOnDisconnect =
        settings.value("UI/openSiteManagerOnDisconnect", true).toBool();
    applyTransferPreferences();

    // Local: model filters (hidden on/off)
    auto applyLocalFilters = [&](QFileSystemModel *fileModel) {
        if (!fileModel)
            return;
        QDir::Filters filters =
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
        if (showHidden)
            filters = filters | QDir::Hidden | QDir::System;
        fileModel->setFilter(filters);
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
    QSettings settings("OpenSCP", "OpenSCP");
    const int maxConcurrent =
        qBound(1, settings.value("Transfer/maxConcurrent", 2).toInt(), 8);
    const int globalSpeed =
        qMax(0, settings.value("Transfer/globalSpeedKBps", 0).toInt());
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
    QSettings settings("OpenSCP", "OpenSCP");
    QStringList recent = settings.value(kRecentLocalPathsKey).toStringList();
    prependRecentValue(&recent, normalized);
    settings.setValue(kRecentLocalPathsKey, recent);
}

void MainWindow::addRecentRemotePath(const QString &path) {
    const QString normalized = normalizeRemotePathForMatch(path);
    if (normalized.isEmpty())
        return;
    QSettings settings("OpenSCP", "OpenSCP");
    QStringList recent = settings.value(kRecentRemotePathsKey).toStringList();
    prependRecentValue(&recent, normalized);
    settings.setValue(kRecentRemotePathsKey, recent);
}

void MainWindow::addRecentServer(const openscp::SessionOptions &opt) {
    const QString host = QString::fromStdString(opt.host).trimmed().toLower();
    if (host.isEmpty())
        return;
    const QString encoded = encodeRecentServerEntry(opt);
    if (encoded.isEmpty())
        return;
    QSettings settings("OpenSCP", "OpenSCP");
    QStringList recent = settings.value(kRecentServersKey).toStringList();
    prependRecentValue(&recent, encoded);
    settings.setValue(kRecentServersKey, recent);
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

    auto *localList = createHistoryTabList(tabs, tr("Recent local paths"));
    auto *remoteList = createHistoryTabList(tabs, tr("Recent remote paths"));
    auto *serverList = createHistoryTabList(tabs, tr("Recent servers"));

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

        QSettings settings("OpenSCP", "OpenSCP");
        const QStringList localPaths =
            settings.value(kRecentLocalPathsKey).toStringList();
        const QStringList remotePaths =
            settings.value(kRecentRemotePathsKey).toStringList();
        const QStringList recentServers =
            settings.value(kRecentServersKey).toStringList();

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
        QSettings settings("OpenSCP", "OpenSCP");
        settings.remove(kRecentLocalPathsKey);
        settings.remove(kRecentRemotePathsKey);
        settings.remove(kRecentServersKey);
        statusBar()->showMessage(tr("History cleared"), 3000);
        populate();
    });

    for (QListWidget *list : {localList, remoteList, serverList}) {
        connect(list, &QListWidget::itemDoubleClicked, &dlg,
                [openSelected](QListWidgetItem *) { openSelected(); });
        connect(list, &QListWidget::currentRowChanged, &dlg,
                [updateOpenEnabled](int) { updateOpenEnabled(); });
    }
    connect(tabs, &QTabWidget::currentChanged, &dlg,
            [updateOpenEnabled](int) { updateOpenEnabled(); });

    populate();
    dlg.exec();
}

void MainWindow::updateDeleteShortcutEnables() {
    auto hasColSel = [&](QTreeView *treeView) -> bool {
        if (!treeView || !treeView->selectionModel())
            return false;
        return !treeView->selectionModel()->selectedRows(NAME_COL).isEmpty();
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
        QDir leftDir(leftPath_ ? leftPath_->text() : QString());
        bool canUp = leftDir.cdUp();
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
            const QString cur =
                normalizeRemotePath(rightPath_ ? rightPath_->text() : QString());
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
            cur = normalizeRemotePath(cur);
            actUpRight_->setEnabled(!cur.isEmpty() && cur != "/");
        } else {
            QDir currentDir(cur);
            actUpRight_->setEnabled(currentDir.cdUp());
        }
    }
}
