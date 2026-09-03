#include "PaneController.hpp"

#include "LocalTreeDiscovery.hpp"
#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "RemotePath.hpp"
#include "UiAlerts.hpp"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStatusBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace openscpui {

namespace {

constexpr int kMaximumSearchMatches = 5000;

struct SearchRequest {
    QString pattern;
    bool recursive = false;
};

bool promptSearch(QWidget *parent, const QString &panelLabel,
                  SearchRequest *request) {
    if (!request)
        return false;

    QDialog dialog(parent);
    dialog.setWindowTitle(
        QCoreApplication::translate("MainWindow", "Search items (%1)")
            .arg(panelLabel));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *help = new QLabel(
        QCoreApplication::translate(
            "MainWindow",
            "Pattern accepts wildcard (*, ?) or regex.\nExamples: *report*, "
            "report, ^report_.*\\.pdf$"),
        &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto *pattern = new QLineEdit(&dialog);
    pattern->setPlaceholderText(
        QCoreApplication::translate("MainWindow", "e.g. *report*"));
    pattern->setClearButtonEnabled(true);
    layout->addWidget(pattern);

    auto *recursive =
        new QCheckBox(QCoreApplication::translate(
                          "MainWindow", "Search recursively in subfolders"),
                      &dialog);
    layout->addWidget(recursive);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    layout->addWidget(buttons);

    QAbstractButton *acceptButton = buttons->button(QDialogButtonBox::Ok);
    if (acceptButton)
        acceptButton->setEnabled(false);
    QObject::connect(pattern, &QLineEdit::textChanged, &dialog,
                     [acceptButton](const QString &text) {
                         if (acceptButton)
                             acceptButton->setEnabled(
                                 !text.trimmed().isEmpty());
                     });

    pattern->setFocus();
    dialog.adjustSize();
    dialog.setMinimumSize(dialog.sizeHint());
    dialog.resize(dialog.sizeHint());
    if (dialog.exec() != QDialog::Accepted)
        return false;

    request->pattern = pattern->text().trimmed();
    request->recursive = recursive->isChecked();
    return !request->pattern.isEmpty();
}

void showSearchResults(QWidget *parent, const QString &panelLabel,
                       const QString &basePath,
                       const QVector<QPair<QString, bool>> &rows,
                       int scanErrors, bool canceled, bool truncated) {
    QDialog dialog(parent);
    dialog.setWindowTitle(
        QCoreApplication::translate("MainWindow", "Search results (%1)")
            .arg(panelLabel));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    QString summary =
        QCoreApplication::translate("MainWindow", "Base: %1\nMatches: %2")
            .arg(basePath, QString::number(rows.size()));
    if (scanErrors > 0) {
        summary += QStringLiteral("\n") +
                   QCoreApplication::translate("MainWindow", "Scan errors: %1")
                       .arg(scanErrors);
    }
    if (canceled)
        summary += QStringLiteral("\n") +
                   QCoreApplication::translate("MainWindow",
                                               "Search canceled by user.");
    if (truncated) {
        summary += QStringLiteral("\n") +
                   QCoreApplication::translate(
                       "MainWindow", "Results truncated to safety limit.");
    }

    auto *summaryLabel = new QLabel(summary, &dialog);
    summaryLabel->setWordWrap(true);
    summaryLabel->setFrameStyle(static_cast<int>(QFrame::StyledPanel) |
                                static_cast<int>(QFrame::Plain));
    summaryLabel->setMargin(8);
    layout->addWidget(summaryLabel);

    auto *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setSelectionBehavior(QAbstractItemView::SelectRows);
    list->setAlternatingRowColors(true);
    list->setUniformItemSizes(true);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    for (const auto &[path, isDirectory] : rows) {
        QString label = path;
        if (isDirectory && !label.endsWith(QLatin1Char('/')))
            label += QLatin1Char('/');
        list->addItem(label);
    }
    layout->addWidget(list, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    layout->addWidget(buttons);

    dialog.adjustSize();
    dialog.setMinimumSize(dialog.sizeHint());
    dialog.resize(dialog.sizeHint());
    dialog.exec();
}

void showStatus(const PaneController::SearchContext &context,
                const QString &message, int timeoutMs) {
    if (context.statusBar)
        context.statusBar->showMessage(message, timeoutMs);
}

void reportResults(const PaneController::SearchContext &context,
                   const QString &basePath,
                   const QVector<QPair<QString, bool>> &matches, int scanErrors,
                   bool canceled, bool truncated) {
    if (matches.isEmpty()) {
        QString message =
            canceled ? QCoreApplication::translate("MainWindow",
                                                   "Search canceled in %1.")
                           .arg(context.panelLabel)
                     : QCoreApplication::translate(
                           "MainWindow", "No recursive matches found in %1.")
                           .arg(context.panelLabel);
        if (scanErrors > 0) {
            message += QStringLiteral("  ") +
                       QCoreApplication::translate("MainWindow",
                                                   "Folders with errors: %1")
                           .arg(scanErrors);
        }
        showStatus(context, message, 5000);
        return;
    }

    showSearchResults(context.dialogParent, context.panelLabel, basePath,
                      matches, scanErrors, canceled, truncated);
    QString message = QCoreApplication::translate(
                          "MainWindow", "Found %1 recursive match(es) in %2.")
                          .arg(matches.size())
                          .arg(context.panelLabel);
    if (truncated) {
        message +=
            QStringLiteral("  ") +
            QCoreApplication::translate("MainWindow", "Results limited to %1.")
                .arg(kMaximumSearchMatches);
    }
    if (scanErrors > 0) {
        message +=
            QStringLiteral("  ") +
            QCoreApplication::translate("MainWindow", "Folders with errors: %1")
                .arg(scanErrors);
    }
    if (canceled)
        message += QStringLiteral("  ") +
                   QCoreApplication::translate("MainWindow", "(Canceled)");
    showStatus(context, message, 6000);
}

int selectCurrentFolderMatches(const PaneController::SearchContext &context,
                               const QRegularExpression &expression) {
    QTreeView *view = context.view;
    if (!view || !view->model() || !view->selectionModel())
        return 0;

    QAbstractItemModel *model = view->model();
    QItemSelectionModel *selection = view->selectionModel();
    selection->clearSelection();
    QModelIndex firstMatch;
    int matches = 0;

    const QModelIndex root = view->rootIndex();
    const int rowCount = model->rowCount(root);
    for (int row = 0; row < rowCount; ++row) {
        const QModelIndex index = model->index(row, 0, root);
        QString name = context.isRemote && context.remoteModel
                           ? context.remoteModel->nameAt(index)
                           : model->data(index, Qt::DisplayRole).toString();
        if (!context.isRemote && name.endsWith(QLatin1Char('/')))
            name.chop(1);
        if (!expression.match(name).hasMatch())
            continue;

        selection->select(index, QItemSelectionModel::Select |
                                     QItemSelectionModel::Rows);
        if (!firstMatch.isValid())
            firstMatch = index;
        ++matches;
    }

    if (firstMatch.isValid()) {
        selection->setCurrentIndex(firstMatch, QItemSelectionModel::NoUpdate);
        view->scrollTo(firstMatch, QAbstractItemView::PositionAtCenter);
    }
    return matches;
}

struct RemoteSearchState {
    RemoteOperationController::JobId jobId = 0;
    PaneController::SearchContext context;
    QVector<QPair<QString, bool>> matches;
    QRegularExpression expression;
    QString basePath;
    bool canceledByUser = false;
    bool truncated = false;
    QPointer<QProgressDialog> progress;
    QMetaObject::Connection batchConnection;
    QMetaObject::Connection progressConnection;
    QMetaObject::Connection completionConnection;
};

struct LocalSearchState {
    PaneController::SearchContext context;
    QVector<QPair<QString, bool>> matches;
    QRegularExpression expression;
    QString basePath;
    bool canceledByUser = false;
    bool truncated = false;
    QPointer<QProgressDialog> progress;
    QPointer<LocalTreeDiscovery> discovery;
};

} // namespace

PaneController::PaneController(QObject *parent) : QObject(parent) {
}

PaneController::~PaneController() {
    cancelSearches();
}

void PaneController::search(const SearchContext &context) {
    if (!context.view || !context.view->model() ||
        !context.view->selectionModel()) {
        return;
    }

    SearchRequest request;
    if (!promptSearch(context.dialogParent, context.panelLabel, &request))
        return;

    QString expressionError;
    const QRegularExpression expression =
        compileSearchPattern(request.pattern, &expressionError);
    if (!expression.isValid()) {
        UiAlerts::warning(
            context.dialogParent,
            QCoreApplication::translate("MainWindow", "Invalid pattern"),
            QCoreApplication::translate("MainWindow",
                                        "The pattern is not valid.\n%1")
                .arg(expressionError.isEmpty()
                         ? QCoreApplication::translate("MainWindow",
                                                       "Unknown regex error.")
                         : expressionError));
        return;
    }

    if (!request.recursive) {
        const int matches = selectCurrentFolderMatches(context, expression);
        const QString message =
            matches > 0 ? QCoreApplication::translate(
                              "MainWindow", "Found %1 match(es) in %2.")
                              .arg(matches)
                              .arg(context.panelLabel)
                        : QCoreApplication::translate("MainWindow",
                                                      "No matches found in %1.")
                              .arg(context.panelLabel);
        showStatus(context, message, 4000);
        return;
    }

    if (context.isRemote) {
        if (!context.remoteModel || !context.remoteOperations ||
            !context.remoteOperations->hasRequestedSession()) {
            UiAlerts::warning(
                context.dialogParent,
                QCoreApplication::translate("MainWindow", "Remote"),
                QCoreApplication::translate("MainWindow",
                                            "No active remote session."));
            return;
        }

        auto state = std::make_shared<RemoteSearchState>();
        state->context = context;
        state->expression = expression;
        state->basePath =
            ::normalizeRemotePath(context.remoteModel->rootPath());
        state->matches.reserve(kMaximumSearchMatches);
        state->progress = new QProgressDialog(
            QCoreApplication::translate("MainWindow",
                                        "Searching recursively in %1...")
                .arg(context.panelLabel),
            QCoreApplication::translate("MainWindow", "Cancel"), 0, 0,
            context.dialogParent);
        state->progress->setWindowModality(Qt::NonModal);
        state->progress->setMinimumDuration(0);
        state->progress->setAutoClose(false);
        state->progress->show();

        state->batchConnection = connect(
            context.remoteOperations,
            &RemoteOperationController::entriesBatchReady, this,
            [state](const RemoteOperationController::EntryBatch &batch) {
                if (batch.job.id != state->jobId || state->truncated)
                    return;
                for (const auto &entry : batch.entries) {
                    const QString name =
                        QString::fromStdString(entry.info.name);
                    if (!state->expression.match(name).hasMatch())
                        continue;
                    state->matches.push_back(
                        {entry.relativePath, entry.info.is_dir});
                    if (state->matches.size() >= kMaximumSearchMatches) {
                        state->truncated = true;
                        if (state->context.remoteOperations) {
                            state->context.remoteOperations->cancel(
                                state->jobId);
                        }
                        break;
                    }
                }
            });
        state->progressConnection = connect(
            context.remoteOperations, &RemoteOperationController::jobProgress,
            this, [state](const RemoteOperationController::Progress &progress) {
                if (progress.job.id != state->jobId || !state->progress) {
                    return;
                }
                state->progress->setLabelText(
                    QCoreApplication::translate(
                        "MainWindow",
                        "Scanning %1\nVisited: %2  |  Matches: %3")
                        .arg(progress.currentPath)
                        .arg(progress.visitedEntries)
                        .arg(state->matches.size()));
            });
        state->completionConnection = connect(
            context.remoteOperations, &RemoteOperationController::jobFinished,
            this,
            [state](const RemoteOperationController::Completion &completion) {
                if (completion.result.job.id != state->jobId)
                    return;
                QObject::disconnect(state->batchConnection);
                QObject::disconnect(state->progressConnection);
                QObject::disconnect(state->completionConnection);
                if (state->progress) {
                    state->progress->hide();
                    state->progress->deleteLater();
                }

                const bool canceled =
                    state->canceledByUser ||
                    (completion.result.outcome ==
                         RemoteOperationController::Outcome::Canceled &&
                     !state->truncated);
                int errors = static_cast<int>(completion.failedEntries);
                if (completion.result.outcome ==
                        RemoteOperationController::Outcome::Failed &&
                    errors == 0) {
                    ++errors;
                }
                if (completion.result.outcome ==
                        RemoteOperationController::Outcome::Succeeded &&
                    state->context.remoteActivitySucceeded) {
                    state->context.remoteActivitySucceeded();
                }
                reportResults(state->context, state->basePath, state->matches,
                              errors, canceled, state->truncated);
            });
        connect(state->progress, &QProgressDialog::canceled, this, [state] {
            state->canceledByUser = true;
            if (state->context.remoteOperations && state->jobId != 0) {
                state->context.remoteOperations->cancel(state->jobId);
            }
        });

        RemoteOperationController::TraverseRequest traversal;
        traversal.rootPath = state->basePath;
        traversal.includeDirectories = true;
        traversal.traversal.includeHidden = context.includeHidden;
        traversal.traversal.skipSymlinks = true;
        traversal.traversal.maxDepth = 32;
        traversal.traversal.batchSize = 250;
        state->jobId = context.remoteOperations->submit(traversal);
        return;
    }

    const QString basePath = QDir::cleanPath(context.localBasePath.trimmed());
    if (basePath.isEmpty() || !QDir(basePath).exists()) {
        UiAlerts::warning(
            context.dialogParent,
            QCoreApplication::translate("MainWindow", "Invalid folder"),
            QCoreApplication::translate("MainWindow",
                                        "The current folder does not exist."));
        return;
    }

    auto *discovery = new LocalTreeDiscovery(this);
    localSearches_.insert(discovery);
    auto state = std::make_shared<LocalSearchState>();
    state->context = context;
    state->expression = expression;
    state->basePath = basePath;
    state->matches.reserve(kMaximumSearchMatches);
    state->discovery = discovery;
    state->progress =
        new QProgressDialog(QCoreApplication::translate(
                                "MainWindow", "Searching recursively in %1...")
                                .arg(context.panelLabel),
                            QCoreApplication::translate("MainWindow", "Cancel"),
                            0, 0, context.dialogParent);
    state->progress->setWindowModality(Qt::NonModal);
    state->progress->setMinimumDuration(0);
    state->progress->setAutoClose(false);
    state->progress->show();

    connect(
        discovery, &LocalTreeDiscovery::batchReady, this,
        [state](const LocalTreeDiscoveryBatch &batch) {
            if (state->truncated)
                return;
            for (const auto &entry : batch.entries) {
                if (entry.relativePath.isEmpty())
                    continue;
                const QString name = QFileInfo(entry.localPath).fileName();
                if (!state->expression.match(name).hasMatch())
                    continue;
                state->matches.push_back(
                    {entry.relativePath,
                     entry.type == LocalTreeDiscoveryEntry::Type::Directory});
                if (state->matches.size() >= kMaximumSearchMatches) {
                    state->truncated = true;
                    if (state->discovery)
                        state->discovery->cancel();
                    break;
                }
            }
        });
    connect(
        discovery, &LocalTreeDiscovery::progressChanged, this,
        [state](const LocalTreeDiscoveryCounters &counters,
                const QString &currentPath) {
            if (!state->progress)
                return;
            state->progress->setLabelText(
                QCoreApplication::translate(
                    "MainWindow", "Scanning %1\nVisited: %2  |  Matches: %3")
                    .arg(QDir::fromNativeSeparators(currentPath))
                    .arg(counters.itemCount)
                    .arg(state->matches.size()));
        });

    const auto finalize = [this,
                           state](const LocalTreeDiscoveryCounters &counters,
                                  bool canceled) {
        if (state->progress) {
            state->progress->hide();
            state->progress->deleteLater();
        }
        if (state->discovery)
            localSearches_.remove(state->discovery);
        const int scanErrors = static_cast<int>(counters.inaccessibleEntries +
                                                counters.depthLimits);
        reportResults(state->context, state->basePath, state->matches,
                      scanErrors,
                      state->canceledByUser || (canceled && !state->truncated),
                      state->truncated);
        if (state->discovery)
            state->discovery->deleteLater();
    };
    connect(discovery, &LocalTreeDiscovery::finished, this,
            [finalize](const LocalTreeDiscoveryCounters &counters) {
                finalize(counters, false);
            });
    connect(discovery, &LocalTreeDiscovery::canceled, this,
            [finalize](const LocalTreeDiscoveryCounters &counters) {
                finalize(counters, true);
            });
    connect(
        discovery, &LocalTreeDiscovery::largeTreeConfirmationRequired, this,
        [state](const LocalTreeDiscoveryCounters &counters) {
            if (!state->discovery)
                return;
            const auto decision = UiAlerts::question(
                state->context.dialogParent,
                QCoreApplication::translate("MainWindow", "Large search"),
                QCoreApplication::translate(
                    "MainWindow", "The search has reached %1 items.\nContinue?")
                    .arg(counters.itemCount),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (decision == QMessageBox::Yes)
                state->discovery->continueAfterLargeTreeConfirmation();
            else
                state->discovery->cancel();
        });
    connect(state->progress, &QProgressDialog::canceled, this, [state] {
        state->canceledByUser = true;
        if (state->discovery)
            state->discovery->cancel();
    });

    LocalTreeDiscoveryOptions options;
    options.roots = {{basePath}};
    options.batchSize = 250;
    options.maximumDepth = 32;
    discovery->start(options);
}

void PaneController::cancelSearches() {
    const auto searches = localSearches_;
    for (LocalTreeDiscovery *search : searches) {
        if (search)
            search->cancel();
    }
}

QRegularExpression
PaneController::compileSearchPattern(const QString &rawPattern,
                                     QString *error) {
    if (error)
        error->clear();
    const QString pattern = rawPattern.trimmed();
    if (pattern.isEmpty())
        return {};

    // A plain dot is common in wildcard file names (for example *.pdf), so it
    // must not force regex mode. Anchors, escapes, groups and character
    // classes are unambiguous regex intent.
    static const QString regexMetadata = QStringLiteral("\\^$+()[]{}|");
    const bool containsRegexMetadata =
        std::any_of(pattern.cbegin(), pattern.cend(), [&](QChar character) {
            return regexMetadata.contains(character);
        });

    QString expression;
    if (containsRegexMetadata) {
        expression = pattern;
    } else if (pattern.contains(QLatin1Char('*')) ||
               pattern.contains(QLatin1Char('?'))) {
        expression.reserve((pattern.size() * 2) + 4);
        expression += QLatin1Char('^');
        for (QChar character : pattern) {
            if (character == QLatin1Char('*'))
                expression += QStringLiteral(".*");
            else if (character == QLatin1Char('?'))
                expression += QLatin1Char('.');
            else
                expression += QRegularExpression::escape(QString(character));
        }
        expression += QLatin1Char('$');
    } else {
        expression =
            QStringLiteral(".*%1.*").arg(QRegularExpression::escape(pattern));
    }

    QRegularExpression compiled(expression,
                                QRegularExpression::CaseInsensitiveOption);
    if (!compiled.isValid() && error)
        *error = compiled.errorString();
    return compiled;
}

} // namespace openscpui
