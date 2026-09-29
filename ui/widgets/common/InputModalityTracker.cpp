#include "widgets/common/InputModalityTracker.hpp"

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>

namespace {

constexpr auto TrackerObjectName = "openscpInputModalityTracker";

} // namespace

namespace openscpui {

InputModalityTracker::InputModalityTracker(QApplication *application)
    : QObject(application) {
    setObjectName(QString::fromLatin1(TrackerObjectName));
    application->installEventFilter(this);
}

bool InputModalityTracker::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress: {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool focusTraversalKey =
            (keyEvent->key() == Qt::Key_Tab &&
             (keyEvent->modifiers() == Qt::NoModifier ||
              keyEvent->modifiers() == Qt::ShiftModifier)) ||
            (keyEvent->key() == Qt::Key_Backtab &&
             (keyEvent->modifiers() == Qt::NoModifier ||
              keyEvent->modifiers() == Qt::ShiftModifier));
        const bool focusCueKey =
            focusTraversalKey || (keyEvent->key() == Qt::Key_Escape &&
                                  keyEvent->modifiers() == Qt::ShiftModifier);
        explicitInputObserved_ = true;
        if (focusCueKey)
            setModality(InputModality::Keyboard);
        emit inputObserved(focusTraversalKey ? UserInputKind::FocusTraversal
                                             : UserInputKind::Keyboard,
                           watched);
        break;
    }
    case QEvent::FocusIn: {
        if (explicitInputObserved_)
            break;
        const auto *focusEvent = static_cast<QFocusEvent *>(event);
        if (focusEvent->reason() == Qt::TabFocusReason ||
            focusEvent->reason() == Qt::BacktabFocusReason) {
            setModality(InputModality::Keyboard);
        } else if (focusEvent->reason() == Qt::MouseFocusReason) {
            setModality(InputModality::Pointer);
        }
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::NonClientAreaMouseButtonPress:
    case QEvent::NonClientAreaMouseButtonDblClick:
    case QEvent::TabletPress:
    case QEvent::TouchBegin:
    case QEvent::Wheel:
        explicitInputObserved_ = true;
        setModality(InputModality::Pointer);
        emit inputObserved(UserInputKind::Pointer, watched);
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void InputModalityTracker::setModality(InputModality modality) {
    if (modality_ == modality)
        return;
    modality_ = modality;
    emit modalityChanged(modality_);
}

InputModalityTracker *inputModalityTracker() {
    if (!qApp)
        return nullptr;

    auto *tracker = qApp->findChild<InputModalityTracker *>(
        QString::fromLatin1(TrackerObjectName), Qt::FindDirectChildrenOnly);
    if (!tracker)
        tracker = new InputModalityTracker(qApp);
    return tracker;
}

} // namespace openscpui
