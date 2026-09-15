#pragma once

#include "openscp/RemoteClient.hpp"

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QStatusBar;
class QWidget;
class RemoteOperationController;

namespace openscpui {

class RemoteActionController final : public QObject {
    public:
    struct Entry {
        QString name;
        bool isDirectory = false;
    };

    struct PermissionSelection {
        std::uint32_t mode = 0644;
        bool recursive = false;
    };

    struct Availability {
        bool canMutate = false;
        bool canUpload = false;
        bool canCreateDirectory = false;
        bool canCreateFile = false;
        bool canRename = false;
        bool canDelete = false;
        bool canMoveToLocal = false;
        bool canSetPermissions = false;
    };

    struct Context {
        QWidget *dialogParent = nullptr;
        QStatusBar *statusBar = nullptr;
        RemoteOperationController *operations = nullptr;
        std::function<bool()> isSessionActive;
        std::function<void(const QString &)> refreshPath;
        std::function<void()> remoteActivitySucceeded;
        std::function<std::optional<PermissionSelection>(
            std::uint32_t currentMode, bool isDirectory)>
            requestPermissions;
    };

    explicit RemoteActionController(QObject *parent = nullptr);

    void setContext(Context context);

    void createDirectory(const QString &basePath, const QString &name);
    void createFile(const QString &basePath, const QString &name);
    void rename(const QString &basePath, const QString &oldName,
                const QString &newName);
    void remove(const QString &basePath, const QVector<Entry> &entries);
    void changePermissions(const QString &basePath, const Entry &entry);

    static Availability
    availability(const openscp::ProtocolCapabilities &capabilities,
                 bool sessionActive = true);

    private:
    bool isReady() const;
    void showStatus(const QString &message, int timeoutMs = 0) const;
    void reportRemoteActivity() const;
    void refresh(const QString &path) const;
    std::shared_ptr<quint64> watchMutation(const QString &basePath,
                                           const QString &failureMessage,
                                           const QString &successMessage = {});

    Context context_;
};

} // namespace openscpui
