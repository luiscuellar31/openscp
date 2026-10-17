#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace openscpui {
// Sparkle owns downloading, signature verification, installation and relaunch.
// The application owns the decision that its current work can safely finish.
class MacUpdater : public QObject {
    Q_OBJECT
    public:
    MacUpdater(std::function<bool()> canRestart,
               std::function<void()> prepareRestart, QObject *parent = nullptr);
    ~MacUpdater() override;
    bool available() const;
    bool check();

    signals:
    void finished();

    private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace openscpui
