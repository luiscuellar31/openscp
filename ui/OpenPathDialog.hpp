#pragma once

#include <QDialog>
#include <QStringList>

class QComboBox;
class QIcon;
class QListWidget;
class QPushButton;

namespace openscpui {

class OpenPathDialog final : public QDialog {
    Q_OBJECT

    public:
    explicit OpenPathDialog(const QString &currentPath,
                            const QStringList &recentPaths,
                            const QStringList &favorites,
                            Qt::CaseSensitivity pathCaseSensitivity,
                            const QIcon &headingIcon,
                            QWidget *parent = nullptr);

    [[nodiscard]] QString selectedPath() const;
    void setFavorites(const QStringList &favorites);

    signals:
    void addFavoriteRequested(const QString &path);
    void removeFavoriteRequested(const QString &path);

    private:
    [[nodiscard]] QString selectedFavorite() const;
    [[nodiscard]] bool containsFavorite(const QString &path) const;
    void updateActions();

    Qt::CaseSensitivity pathCaseSensitivity_ = Qt::CaseSensitive;
    QComboBox *pathCombo_ = nullptr;
    QListWidget *favoritesList_ = nullptr;
    QPushButton *addFavoriteButton_ = nullptr;
    QPushButton *removeFavoriteButton_ = nullptr;
    QPushButton *openButton_ = nullptr;
};

} // namespace openscpui
