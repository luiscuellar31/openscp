#pragma once

#include <QObject>

class QApplication;
class QEvent;

namespace openscpui {

enum class InputModality { Pointer, Keyboard };
enum class UserInputKind { FocusTraversal, Keyboard, Pointer };

class InputModalityTracker final : public QObject {
    Q_OBJECT

    public:
    explicit InputModalityTracker(QApplication *application);

    [[nodiscard]] InputModality modality() const noexcept { return modality_; }
    [[nodiscard]] bool isKeyboardActive() const noexcept {
        return modality_ == InputModality::Keyboard;
    }

    signals:
    void modalityChanged(openscpui::InputModality modality);
    void inputObserved(openscpui::UserInputKind kind, QObject *target);

    protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    private:
    void setModality(InputModality modality);

    InputModality modality_ = InputModality::Pointer;
};

[[nodiscard]] InputModalityTracker *inputModalityTracker();

} // namespace openscpui
