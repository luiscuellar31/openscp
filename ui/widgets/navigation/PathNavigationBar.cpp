#include "widgets/navigation/PathNavigationBar.hpp"

#include "widgets/common/InputModalityTracker.hpp"

#include <QDir>
#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QSizePolicy>
#include <QStyleOptionFocusRect>

namespace {

class PathLineEdit final : public QLineEdit {
    public:
    using QLineEdit::QLineEdit;

    void setKeyboardFocusVisible(bool visible) {
        if (keyboardFocusVisible_ == visible)
            return;
        keyboardFocusVisible_ = visible;
        setProperty("keyboardFocusVisible", visible);
        update();
    }

    protected:
    void paintEvent(QPaintEvent *event) override {
        QLineEdit::paintEvent(event);
        if (!keyboardFocusVisible_)
            return;

        QStyleOptionFocusRect option;
        option.initFrom(this);
        option.rect = rect().adjusted(1, 1, -1, -1);
        option.state |= QStyle::State_KeyboardFocusChange;
        option.backgroundColor = palette().color(QPalette::Base);

        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter,
                               this);
    }

    private:
    bool keyboardFocusVisible_ = false;
};

} // namespace

namespace openscpui {

PathNavigationBar::PathNavigationBar(PathFlavor flavor,
                                     const QString &initialPath,
                                     QWidget *parent)
    : QWidget(parent), flavor_(flavor) {
    setObjectName(QStringLiteral("pathNavigationBar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    display_ = new PathLineEdit(this);
    display_->setObjectName(QStringLiteral("pathDisplay"));
    display_->setReadOnly(true);
    display_->setFocusPolicy(Qt::StrongFocus);
    display_->setAttribute(Qt::WA_MacShowFocusRect, false);
    display_->setMouseTracking(true);
    display_->setAccessibleName(tr("Current folder path"));
    defaultToolTip_ =
        tr("Click the current path or press %1 to open a directory.")
            .arg(QKeySequence(QKeySequence::Open)
                     .toString(QKeySequence::NativeText));
    display_->setToolTip(defaultToolTip_);
    display_->installEventFilter(this);
    if (InputModalityTracker *tracker = inputModalityTracker()) {
        connect(tracker, &InputModalityTracker::modalityChanged, this,
                [this](InputModality modality) {
                    if (display_->hasFocus()) {
                        setKeyboardFocusVisible(modality ==
                                                InputModality::Keyboard);
                    }
                });
    }
    layout->addWidget(display_);

    setPath(initialPath);
}

void PathNavigationBar::setPath(const QString &path) {
    committedPath_ = path;
    rebuildDisplay();
}

QWidget *PathNavigationBar::keyboardFocusTarget() const {
    return display_;
}

void PathNavigationBar::setPathFlavor(PathFlavor flavor) {
    if (flavor_ == flavor)
        return;
    flavor_ = flavor;
    rebuildDisplay();
}

void PathNavigationBar::requestOpenDialog() {
    emit openDialogRequested();
}

bool PathNavigationBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched != display_)
        return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::FocusIn) {
        const auto *focusEvent = static_cast<QFocusEvent *>(event);
        const InputModalityTracker *tracker = inputModalityTracker();
        const bool keyboardInput = tracker && tracker->isKeyboardActive();
        setKeyboardFocusVisible(keyboardInput);
        if (keyboardInput || focusEvent->reason() == Qt::ShortcutFocusReason) {
            focusedSegment_ = segments_.isEmpty() ? -1 : segments_.size() - 1;
            updateSegmentPresentation(focusedSegment_, false);
        }
    } else if (event->type() == QEvent::FocusOut) {
        setKeyboardFocusVisible(false);
        if (hoveredSegment_ < 0)
            updateSegmentPresentation(-1, false);
    } else if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (segments_.isEmpty())
            return QWidget::eventFilter(watched, event);
        if (keyEvent->key() == Qt::Key_Left) {
            focusedSegment_ = qMax<qsizetype>(0, focusedSegment_ - 1);
            updateSegmentPresentation(focusedSegment_, false);
            return true;
        }
        if (keyEvent->key() == Qt::Key_Right) {
            focusedSegment_ =
                qMin<qsizetype>(segments_.size() - 1, focusedSegment_ + 1);
            updateSegmentPresentation(focusedSegment_, false);
            return true;
        }
        if (keyEvent->key() == Qt::Key_Home) {
            focusedSegment_ = 0;
            updateSegmentPresentation(focusedSegment_, false);
            return true;
        }
        if (keyEvent->key() == Qt::Key_End) {
            focusedSegment_ = segments_.size() - 1;
            updateSegmentPresentation(focusedSegment_, false);
            return true;
        }
        if (keyEvent->key() == Qt::Key_Enter ||
            keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Space) {
            activateSegment(focusedSegment_);
            return true;
        }
    } else if (event->type() == QEvent::MouseMove) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        hoveredSegment_ = segmentAt(mouseEvent->position().toPoint());
        if ((mouseEvent->buttons() & Qt::LeftButton) &&
            hoveredSegment_ != pressedSegment_) {
            pressedSegment_ = -1;
        }
        updateSegmentPresentation(hoveredSegment_, true);
    } else if (event->type() == QEvent::Leave) {
        pressedSegment_ = -1;
        hoveredSegment_ = -1;
        updateSegmentPresentation(display_->hasFocus() ? focusedSegment_ : -1,
                                  false);
    } else if (event->type() == QEvent::MouseButtonPress) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            suppressNextRelease_ = false;
            pressedSegment_ = segmentAt(mouseEvent->position().toPoint());
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            pressedSegment_ = -1;
            suppressNextRelease_ = true;
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            if (suppressNextRelease_) {
                suppressNextRelease_ = false;
                pressedSegment_ = -1;
                return true;
            }
            const qsizetype pressedSegment = pressedSegment_;
            pressedSegment_ = -1;
            const qsizetype releasedSegment =
                segmentAt(mouseEvent->position().toPoint());
            if (pressedSegment >= 0 && releasedSegment == pressedSegment) {
                focusedSegment_ = pressedSegment;
                activateSegment(focusedSegment_);
            }
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void PathNavigationBar::rebuildDisplay() {
    segments_ = buildPathSegments(committedPath_, flavor_);
    if (!segments_.isEmpty())
        committedPath_ = segments_.back().target;
    displayPath_ = committedPath_;
    if (flavor_ == PathFlavor::Local)
        displayPath_ = QDir::toNativeSeparators(displayPath_);
    display_->setText(displayPath_);
    display_->setCursorPosition(static_cast<int>(displayPath_.size()));
    hoveredSegment_ = -1;
    focusedSegment_ = segments_.isEmpty() ? -1 : segments_.size() - 1;
    pressedSegment_ = -1;
    suppressNextRelease_ = false;
    updateSegmentPresentation(display_->hasFocus() ? focusedSegment_ : -1,
                              false);
}

qsizetype PathNavigationBar::segmentAt(const QPoint &position) const {
    if (segments_.isEmpty() || displayPath_.isEmpty() ||
        !display_->rect().contains(position)) {
        return -1;
    }

    const qsizetype cursorPosition = display_->cursorPositionAt(position);
    if (cursorPosition >= displayPath_.size())
        return segments_.size() - 1;

    for (qsizetype index = 0; index < segments_.size(); ++index) {
        const PathSegment &segment = segments_.at(index);
        if (cursorPosition >= segment.displayStart &&
            cursorPosition < segment.displayEnd) {
            return index;
        }
        if (cursorPosition < segment.displayStart)
            return qMax<qsizetype>(0, index - 1);
    }
    return segments_.size() - 1;
}

void PathNavigationBar::activateSegment(qsizetype segment) {
    if (segment >= 0 && segment + 1 < segments_.size()) {
        emit pathRequested(segments_.at(segment).target);
        return;
    }
    requestOpenDialog();
}

void PathNavigationBar::setKeyboardFocusVisible(bool visible) {
    static_cast<PathLineEdit *>(display_)->setKeyboardFocusVisible(visible);
}

void PathNavigationBar::updateSegmentPresentation(qsizetype segment,
                                                  bool hovered) {
    if (segment < 0 || segment >= segments_.size()) {
        display_->deselect();
        display_->setCursor(Qt::ArrowCursor);
        display_->setToolTip(defaultToolTip_);
        display_->setAccessibleDescription(
            tr("Use Left and Right to choose a path segment, then press Enter "
               "or Space to activate it."));
        return;
    }

    const PathSegment &part = segments_.at(segment);
    display_->setSelection(
        static_cast<int>(part.displayStart),
        static_cast<int>(part.displayEnd - part.displayStart));
    display_->setCursor(hovered ? Qt::PointingHandCursor : Qt::ArrowCursor);
    const bool current = segment + 1 == segments_.size();
    const QString action =
        current ? tr("Open directory…") : tr("Open %1").arg(part.target);
    display_->setToolTip(action);
    display_->setAccessibleDescription(
        current ? tr("Current folder %1. Press Enter or Space to open path "
                     "entry.")
                      .arg(part.target)
                : tr("Path segment %1. Press Enter or Space to navigate to "
                     "it.")
                      .arg(part.target));
}

} // namespace openscpui
