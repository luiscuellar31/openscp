#pragma once

#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QString>

#include <functional>

class LocalTreeDiscovery;
class QStatusBar;
class QTreeView;
class QWidget;
class RemoteModel;
class RemoteOperationController;

namespace openscpui {

class PaneController final : public QObject {
    public:
    struct SearchContext {
        QWidget *dialogParent = nullptr;
        QStatusBar *statusBar = nullptr;
        QTreeView *view = nullptr;
        RemoteModel *remoteModel = nullptr;
        RemoteOperationController *remoteOperations = nullptr;
        QString panelLabel;
        QString localBasePath;
        bool isRemote = false;
        bool includeHidden = false;
        std::function<void()> remoteActivitySucceeded;
    };

    explicit PaneController(QObject *parent = nullptr);
    ~PaneController() override;

    void search(const SearchContext &context);
    void cancelSearches();

    static QRegularExpression compileSearchPattern(const QString &pattern,
                                                   QString *error = nullptr);

    private:
    QSet<LocalTreeDiscovery *> localSearches_;
};

} // namespace openscpui
