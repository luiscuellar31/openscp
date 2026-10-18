#include "widgets/common/FocusTraversalController.hpp"

#include "widgets/common/InputModalityTracker.hpp"
#include "widgets/common/ToolbarKeyboardNavigation.hpp"

#include <QWidget>

namespace openscpui {

FocusTraversalController::FocusTraversalController(QWidget *scope)
    : QObject(scope), scope_(scope) {
    if (InputModalityTracker *tracker = inputModalityTracker()) {
        connect(tracker, &InputModalityTracker::inputObserved, this,
                [this](UserInputKind kind, QObject *target) {
                    if (initialNavigationPending_ &&
                        kind != UserInputKind::FocusTraversal &&
                        contains(target)) {
                        initialNavigationPending_ = false;
                    }
                });
    }
}

bool FocusTraversalController::moveFocus(const QList<QWidget *> &order,
                                         QWidget *current, bool forward) {
    if (initialNavigationPending_)
        current = nullptr;
    initialNavigationPending_ = false;
    return moveKeyboardFocus(order, current, forward);
}

bool FocusTraversalController::contains(QObject *target) const {
    if (!scope_ || !target)
        return false;
    auto *widget = qobject_cast<QWidget *>(target);
    return widget && (widget == scope_ || scope_->isAncestorOf(widget));
}

bool disableWithoutPassingFocus(QWidget *widget) {
    // A widget without the focus has none to pass on when disabled.
    const bool hadFocus = widget->hasFocus();
    if (hadFocus)
        widget->clearFocus();
    widget->setEnabled(false);
    return hadFocus;
}

void enableRestoringFocus(QWidget *widget, bool hadFocus) {
    widget->setEnabled(true);
    if (hadFocus && !widget->window()->focusWidget())
        widget->setFocus(Qt::OtherFocusReason);
}

} // namespace openscpui
