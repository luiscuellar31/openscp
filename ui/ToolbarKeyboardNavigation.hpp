// Keyboard-focus configuration for action buttons created by QToolBar.
#pragma once

#include <QList>

class QToolBar;
class QToolButton;
class QWidget;

namespace openscpui {

// Makes current and subsequently added action buttons reachable through
// Tab/Shift+Tab. Keyboard focus receives a temporary visible outline, and
// Enter/Return activates the focused action just like Space. Disabled actions
// remain in the chain but Qt skips them until they become enabled.
void configureToolbarKeyboardNavigation(QToolBar *toolbar);

// Returns Qt's generated popup-extension button, if this toolbar currently
// uses one. Detection is intentionally based on public toolbar/action
// relationships rather than Qt's private object names.
[[nodiscard]] QToolButton *toolbarPopupExtensionButton(QToolBar *toolbar);

// Returns the toolbar buttons in visual action order, followed by any visible
// auxiliary button such as the overflow control.
[[nodiscard]] QList<QWidget *> toolbarKeyboardFocusWidgets(QToolBar *toolbar);

// Moves focus through an explicit logical order. This intentionally bypasses
// platform style filtering after the application has opted controls into the
// order, while still skipping hidden and disabled widgets.
bool moveKeyboardFocus(const QList<QWidget *> &order, QWidget *current,
                       bool forward);

} // namespace openscpui
