#include "widgets/navigation/OpenPathDialog.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace openscpui {

namespace {

constexpr int kFavoritePathRole = Qt::UserRole;

} // namespace

OpenPathDialog::OpenPathDialog(const QString &currentPath,
                               const QStringList &recentPaths,
                               const QStringList &favorites,
                               Qt::CaseSensitivity pathCaseSensitivity,
                               const QIcon &headingIcon, QWidget *parent)
    : QDialog(parent), pathCaseSensitivity_(pathCaseSensitivity) {
    setObjectName(QStringLiteral("openPathDialog"));
    setWindowTitle(tr("Open directory"));
    resize(620, 390);
    setMinimumSize(480, 320);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *headingLayout = new QHBoxLayout;
    auto *iconLabel = new QLabel(this);
    const QIcon effectiveIcon =
        headingIcon.isNull() ? style()->standardIcon(QStyle::SP_DirOpenIcon)
                             : headingIcon;
    iconLabel->setPixmap(effectiveIcon.pixmap(32, 32));
    auto *headingLabel = new QLabel(tr("Open directory"), this);
    QFont headingFont = headingLabel->font();
    headingFont.setBold(true);
    headingLabel->setFont(headingFont);
    headingLayout->addWidget(iconLabel);
    headingLayout->addWidget(headingLabel, 1);
    layout->addLayout(headingLayout);

    pathCombo_ = new QComboBox(this);
    pathCombo_->setObjectName(QStringLiteral("openPathCombo"));
    pathCombo_->setEditable(true);
    pathCombo_->setInsertPolicy(QComboBox::NoInsert);
    pathCombo_->setMaxVisibleItems(14);
    pathCombo_->addItem(currentPath);
    for (const QString &recentPath : recentPaths) {
        const QString candidate = recentPath.trimmed();
        bool alreadyAdded = candidate.isEmpty();
        for (int index = 0; !alreadyAdded && index < pathCombo_->count();
             ++index) {
            alreadyAdded = pathCombo_->itemText(index).compare(
                               candidate, pathCaseSensitivity_) == 0;
        }
        if (alreadyAdded) {
            continue;
        }
        pathCombo_->addItem(candidate);
    }
    pathCombo_->setCurrentText(currentPath);
    layout->addWidget(pathCombo_);

    auto *favoritesGroup = new QGroupBox(tr("Favorites"), this);
    auto *favoritesLayout = new QHBoxLayout(favoritesGroup);
    favoritesList_ = new QListWidget(favoritesGroup);
    favoritesList_->setObjectName(QStringLiteral("openPathFavoritesList"));
    favoritesList_->setAlternatingRowColors(true);
    favoritesList_->setSelectionMode(QAbstractItemView::SingleSelection);
    favoritesLayout->addWidget(favoritesList_, 1);

    auto *favoriteButtonsLayout = new QVBoxLayout;
    addFavoriteButton_ = new QPushButton(tr("Add"), favoritesGroup);
    removeFavoriteButton_ = new QPushButton(tr("Remove"), favoritesGroup);
    addFavoriteButton_->setObjectName(QStringLiteral("addPathFavoriteButton"));
    removeFavoriteButton_->setObjectName(
        QStringLiteral("removePathFavoriteButton"));
    favoriteButtonsLayout->addWidget(addFavoriteButton_);
    favoriteButtonsLayout->addWidget(removeFavoriteButton_);
    favoriteButtonsLayout->addStretch(1);
    favoritesLayout->addLayout(favoriteButtonsLayout);
    layout->addWidget(favoritesGroup, 1);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    openButton_ = buttons->button(QDialogButtonBox::Open);
    openButton_->setText(tr("Open"));
    openButton_->setDefault(true);
    layout->addWidget(buttons);

    connect(pathCombo_->lineEdit(), &QLineEdit::textChanged, this,
            [this] { updateActions(); });
    connect(favoritesList_, &QListWidget::currentRowChanged, this, [this](int) {
        const QString favorite = selectedFavorite();
        if (!favorite.isEmpty())
            pathCombo_->setCurrentText(favorite);
        updateActions();
    });
    connect(favoritesList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) {
                if (!selectedFavorite().isEmpty())
                    accept();
            });
    connect(addFavoriteButton_, &QPushButton::clicked, this, [this] {
        const QString path = selectedPath();
        if (!path.isEmpty() && !containsFavorite(path))
            emit addFavoriteRequested(path);
    });
    connect(removeFavoriteButton_, &QPushButton::clicked, this, [this] {
        const QString path = selectedFavorite();
        if (!path.isEmpty())
            emit removeFavoriteRequested(path);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!selectedPath().isEmpty())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    setFavorites(favorites);
    QTimer::singleShot(0, this, [this] {
        pathCombo_->setFocus(Qt::OtherFocusReason);
        pathCombo_->lineEdit()->selectAll();
    });
}

QString OpenPathDialog::selectedPath() const {
    return pathCombo_ ? pathCombo_->currentText().trimmed() : QString();
}

void OpenPathDialog::setFavorites(const QStringList &favorites) {
    favoritesList_->clear();
    for (const QString &favorite : favorites) {
        const QString path = favorite.trimmed();
        if (path.isEmpty() || containsFavorite(path))
            continue;
        auto *item = new QListWidgetItem(path, favoritesList_);
        item->setData(kFavoritePathRole, path);
        item->setToolTip(path);
    }
    if (favoritesList_->count() == 0) {
        auto *emptyItem =
            new QListWidgetItem(tr("No favorites"), favoritesList_);
        emptyItem->setFlags(Qt::NoItemFlags);
    }
    updateActions();
}

QString OpenPathDialog::selectedFavorite() const {
    const QListWidgetItem *item = favoritesList_->currentItem();
    return item ? item->data(kFavoritePathRole).toString() : QString();
}

bool OpenPathDialog::containsFavorite(const QString &path) const {
    for (int row = 0; row < favoritesList_->count(); ++row) {
        const QString favorite =
            favoritesList_->item(row)->data(kFavoritePathRole).toString();
        if (!favorite.isEmpty() &&
            favorite.compare(path, pathCaseSensitivity_) == 0) {
            return true;
        }
    }
    return false;
}

void OpenPathDialog::updateActions() {
    const QString path = selectedPath();
    openButton_->setEnabled(!path.isEmpty());
    addFavoriteButton_->setEnabled(!path.isEmpty() && !containsFavorite(path));
    removeFavoriteButton_->setEnabled(!selectedFavorite().isEmpty());
}

} // namespace openscpui
