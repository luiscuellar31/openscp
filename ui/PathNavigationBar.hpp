// Flat, clickable path field shared by the local and remote file panels.
#pragma once

#include "PathNavigationModel.hpp"

#include <QWidget>

class QEvent;
class QLineEdit;
class QPoint;

namespace openscpui {

class PathNavigationBar final : public QWidget {
    Q_OBJECT

    public:
    explicit PathNavigationBar(PathFlavor flavor, const QString &initialPath,
                               QWidget *parent = nullptr);

    [[nodiscard]] QString path() const { return committedPath_; }
    [[nodiscard]] PathFlavor pathFlavor() const { return flavor_; }

    void setPath(const QString &path);
    void setPathFlavor(PathFlavor flavor);
    void requestOpenDialog();

    signals:
    void pathRequested(const QString &path);
    void openDialogRequested();

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    private:
    void rebuildDisplay();
    [[nodiscard]] qsizetype segmentAt(const QPoint &position) const;
    void activateSegment(qsizetype segment);
    void setKeyboardFocusVisible(bool visible);
    void updateSegmentPresentation(qsizetype segment, bool hovered);

    PathFlavor flavor_ = PathFlavor::Local;
    QString committedPath_;
    QString displayPath_;
    QString defaultToolTip_;
    QVector<PathSegment> segments_;
    QLineEdit *display_ = nullptr;
    qsizetype hoveredSegment_ = -1;
    qsizetype focusedSegment_ = -1;
    bool suppressNextRelease_ = false;
};

} // namespace openscpui
