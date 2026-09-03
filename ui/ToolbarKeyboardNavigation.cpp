#include "ToolbarKeyboardNavigation.hpp"

#include "KeyboardFocusIndicator.hpp"

#include <QAction>
#include <QHash>
#include <QKeyEvent>
#include <QList>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>
#include <QWidget>

#include <algorithm>

namespace {

class ToolbarKeyboardController final : public QObject {
    public:
    explicit ToolbarKeyboardController(QToolBar *toolbar)
        : QObject(toolbar), toolbar_(toolbar) {
        toolbar_->installEventFilter(this);
    }

    void refreshButtons();

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == toolbar_ && event->type() == QEvent::ActionAdded) {
            QTimer::singleShot(0, this, [this] { refreshButtons(); });
            return QObject::eventFilter(watched, event);
        }

        auto *button = qobject_cast<QToolButton *>(watched);
        if (!button || !indicators_.contains(button))
            return QObject::eventFilter(watched, event);

        if (event->type() == QEvent::FocusOut) {
            resetPopupExtensionState(button);
        } else if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const bool enterPressed = keyEvent->key() == Qt::Key_Enter ||
                                      keyEvent->key() == Qt::Key_Return;
            const auto disallowedModifiers =
                Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
            if (enterPressed &&
                !(keyEvent->modifiers() & disallowedModifiers)) {
                if (!keyEvent->isAutoRepeat())
                    button->click();
                keyEvent->accept();
                return true;
            }
        }

        return QObject::eventFilter(watched, event);
    }

    private:
    [[nodiscard]] bool isPopupExtensionButton(QToolButton *button) const {
        return toolbar_ &&
               button == openscpui::toolbarPopupExtensionButton(toolbar_);
    }

    void resetPopupExtensionState(QToolButton *button) const {
        if (!isPopupExtensionButton(button))
            return;
        button->setChecked(false);
        button->setDown(false);
        button->update();
    }

    void attachButton(QToolButton *button) {
        if (!button || indicators_.contains(button))
            return;

        button->setFocusPolicy(Qt::TabFocus);
        if (button->accessibleName().trimmed().isEmpty() &&
            button->defaultAction()) {
            button->setAccessibleName(button->defaultAction()->text());
        }

        auto *indicator = new openscpui::KeyboardFocusIndicator(button);
        indicators_.insert(button, indicator);
        button->installEventFilter(this);
        if (isPopupExtensionButton(button)) {
            // Qt's toolbar extension is checkable because a QMainWindow
            // toolbar can expand in place. Pane toolbars use a popup menu
            // instead, where retaining that checked state leaves a stale dark
            // background after a menu action opens and cancels a dialog.
            resetPopupExtensionState(button);
            button->setCheckable(false);
            QPointer<QToolButton> guardedButton(button);
            connect(button->menu(), &QMenu::aboutToHide, this,
                    [this, guardedButton] {
                        if (!guardedButton)
                            return;
                        QTimer::singleShot(0, this, [this, guardedButton] {
                            resetPopupExtensionState(guardedButton);
                        });
                    });
        }
        connect(button, &QObject::destroyed, this,
                [this, button] { indicators_.remove(button); });
    }

    QPointer<QToolBar> toolbar_;
    QHash<QToolButton *, openscpui::KeyboardFocusIndicator *> indicators_;
};

} // namespace

namespace openscpui {

void configureToolbarKeyboardNavigation(QToolBar *toolbar) {
    if (!toolbar)
        return;

    constexpr auto ControllerProperty = "openscpToolbarKeyboardController";
    QObject *controllerObject =
        toolbar->property(ControllerProperty).value<QObject *>();
    auto *controller =
        static_cast<ToolbarKeyboardController *>(controllerObject);
    if (!controller) {
        controller = new ToolbarKeyboardController(toolbar);
        toolbar->setProperty(ControllerProperty,
                             QVariant::fromValue<QObject *>(controller));
    }
    controller->refreshButtons();
}

QToolButton *toolbarPopupExtensionButton(QToolBar *toolbar) {
    if (!toolbar)
        return nullptr;

    const auto directButtons =
        toolbar->findChildren<QToolButton *>(Qt::FindDirectChildrenOnly);
    for (QToolButton *button : directButtons) {
        if (!button || !button->menu())
            continue;

        const QList<QAction *> actions = toolbar->actions();
        const bool representsAction =
            std::any_of(actions.cbegin(), actions.cend(),
                        [toolbar, button](QAction *action) {
                            return toolbar->widgetForAction(action) == button;
                        });
        if (!representsAction)
            return button;
    }
    return nullptr;
}

QList<QWidget *> toolbarKeyboardFocusWidgets(QToolBar *toolbar) {
    QList<QWidget *> widgets;
    if (!toolbar)
        return widgets;

    for (QAction *action : toolbar->actions()) {
        QWidget *widget = toolbar->widgetForAction(action);
        if (qobject_cast<QToolButton *>(widget))
            widgets.push_back(widget);
    }

    if (QToolButton *extension = toolbarPopupExtensionButton(toolbar);
        extension && !widgets.contains(extension)) {
        widgets.push_back(extension);
    }
    return widgets;
}

bool moveKeyboardFocus(const QList<QWidget *> &order, QWidget *current,
                       bool forward) {
    if (order.isEmpty())
        return false;

    qsizetype currentIndex = -1;
    for (qsizetype index = 0; index < order.size(); ++index) {
        QWidget *candidate = order.at(index);
        if (candidate == current ||
            (candidate && current && candidate->isAncestorOf(current))) {
            currentIndex = index;
            break;
        }
    }
    if (currentIndex < 0 && !forward)
        currentIndex = 0;

    const qsizetype count = order.size();
    for (qsizetype offset = 1; offset <= count; ++offset) {
        qsizetype index = 0;
        if (forward)
            index = (currentIndex + offset + count) % count;
        else
            index = (currentIndex - offset + count * 2) % count;

        QWidget *candidate = order.at(index);
        if (!candidate || !candidate->isVisible() || !candidate->isEnabled() ||
            !(candidate->focusPolicy() & Qt::TabFocus)) {
            continue;
        }

        candidate->setFocus(forward ? Qt::TabFocusReason
                                    : Qt::BacktabFocusReason);
        if (candidate->hasFocus())
            return true;
    }
    return false;
}

} // namespace openscpui

void ToolbarKeyboardController::refreshButtons() {
    if (!toolbar_)
        return;

    for (QWidget *widget : openscpui::toolbarKeyboardFocusWidgets(toolbar_)) {
        attachButton(qobject_cast<QToolButton *>(widget));
    }
}
