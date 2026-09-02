#include "KeyboardFocusIndicator.hpp"

#include <QEvent>
#include <QPainter>
#include <QStyle>

namespace {

constexpr auto FocusIndicatorObjectName = "keyboardFocusIndicator";

} // namespace

namespace openscpui {

KeyboardFocusIndicator::KeyboardFocusIndicator(QWidget *target)
    : QWidget(target), target_(target) {
    setObjectName(QString::fromLatin1(FocusIndicatorObjectName));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    target_->setProperty("keyboardFocusVisible", false);
    target_->installEventFilter(this);
    hide();
}

void KeyboardFocusIndicator::setKeyboardFocusVisible(bool visible) {
    target_->setProperty("keyboardFocusVisible", visible);
    if (!visible) {
        hide();
        return;
    }

    setGeometry(target_->rect());
    show();
    raise();
    update();
}

bool KeyboardFocusIndicator::eventFilter(QObject *watched, QEvent *event) {
    if (watched == target_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        setGeometry(target_->rect());
        if (isVisible())
            raise();
    }
    return QWidget::eventFilter(watched, event);
}

void KeyboardFocusIndicator::paintEvent(QPaintEvent *) {
    if (width() < 3 || height() < 3)
        return;

    const auto group = isActiveWindow() ? QPalette::Active : QPalette::Inactive;
    QColor outline = palette().color(group, QPalette::Highlight);
    if (!outline.isValid())
        outline = palette().color(group, QPalette::WindowText);

    const qreal penWidth = qMax(
        2, style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this));
    const qreal inset = penWidth / 2.0;
    const QRectF outlineRect =
        QRectF(rect()).adjusted(inset, inset, -inset, -inset);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(outline, penWidth));
    painter.drawRoundedRect(outlineRect, 3.0, 3.0);
}

} // namespace openscpui
