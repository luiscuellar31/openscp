#pragma once

#include <QList>
#include <QObject>
#include <QPointer>

class QWidget;

namespace openscpui {

class FocusTraversalController final : public QObject {
    public:
    explicit FocusTraversalController(QWidget *scope);

    bool moveFocus(const QList<QWidget *> &order, QWidget *current,
                   bool forward);

    private:
    [[nodiscard]] bool contains(QObject *target) const;

    QPointer<QWidget> scope_;
    bool initialNavigationPending_ = true;
};

} // namespace openscpui
