#include "PathNavigationBar.hpp"

#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPoint>
#include <QSizePolicy>

namespace openscpui {

PathNavigationBar::PathNavigationBar(PathFlavor flavor,
                                     const QString &initialPath,
                                     QWidget *parent)
    : QWidget(parent), flavor_(flavor) {
    setObjectName(QStringLiteral("pathNavigationBar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    display_ = new QLineEdit(this);
    display_->setObjectName(QStringLiteral("pathDisplay"));
    display_->setReadOnly(true);
    display_->setFocusPolicy(Qt::NoFocus);
    display_->setMouseTracking(true);
    display_->setAccessibleName(tr("Current folder path"));
    defaultToolTip_ =
        tr("Click the current path or press %1 to open a directory.")
            .arg(QKeySequence(QKeySequence::Open)
                     .toString(QKeySequence::NativeText));
    display_->setToolTip(defaultToolTip_);
    display_->installEventFilter(this);
    layout->addWidget(display_);

    setPath(initialPath);
}

void PathNavigationBar::setPath(const QString &path) {
    committedPath_ = path;
    rebuildDisplay();
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

    if (event->type() == QEvent::MouseMove) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        updateHoveredSegment(segmentAt(mouseEvent->position().toPoint()));
    } else if (event->type() == QEvent::Leave) {
        updateHoveredSegment(-1);
    } else if (event->type() == QEvent::MouseButtonPress) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton)
            return true;
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            suppressNextRelease_ = true;
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            if (suppressNextRelease_) {
                suppressNextRelease_ = false;
                return true;
            }
            const qsizetype segment =
                segmentAt(mouseEvent->position().toPoint());
            if (segment >= 0 && segment + 1 < segments_.size())
                emit pathRequested(segments_.at(segment).target);
            else
                requestOpenDialog();
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
    hoveredSegment_ = -2;
    updateHoveredSegment(-1);
}

qsizetype PathNavigationBar::segmentAt(const QPoint &position) const {
    if (segments_.isEmpty() || displayPath_.isEmpty())
        return -1;

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

void PathNavigationBar::updateHoveredSegment(qsizetype segment) {
    if (hoveredSegment_ == segment)
        return;
    hoveredSegment_ = segment;

    if (segment < 0 || segment >= segments_.size()) {
        display_->deselect();
        display_->setCursor(Qt::ArrowCursor);
        display_->setToolTip(defaultToolTip_);
        return;
    }

    const PathSegment &part = segments_.at(segment);
    display_->setSelection(
        static_cast<int>(part.displayStart),
        static_cast<int>(part.displayEnd - part.displayStart));
    display_->setCursor(Qt::PointingHandCursor);
    display_->setToolTip(segment + 1 == segments_.size()
                             ? tr("Open directory…")
                             : tr("Open %1").arg(part.target));
}

} // namespace openscpui
