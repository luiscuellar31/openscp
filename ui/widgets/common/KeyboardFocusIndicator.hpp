// Palette-aware focus outline shown only for keyboard traversal.
#pragma once

#include <QWidget>

namespace openscpui {

class KeyboardFocusIndicator final : public QWidget {
    public:
    explicit KeyboardFocusIndicator(QWidget *target);

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    private:
    void setKeyboardFocusVisible(bool visible);

    QWidget *target_ = nullptr;
};

} // namespace openscpui
