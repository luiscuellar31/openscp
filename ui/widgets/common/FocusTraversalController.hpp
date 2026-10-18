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

// Disables `widget` without passing its keyboard focus to the next widget, as
// setEnabled(false) does, so that no other widget has the focus meanwhile.
// Returns whether `widget` had it, for enableRestoringFocus().
bool disableWithoutPassingFocus(QWidget *widget);

// Enables `widget` and, if it had the focus and no widget of its window took
// the focus since, gives it back.
void enableRestoringFocus(QWidget *widget, bool hadFocus);

} // namespace openscpui
