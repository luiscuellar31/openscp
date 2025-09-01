#pragma once
#include <QDialog>
#include "openscp/SftpTypes.hpp"

class QLineEdit;
class QSpinBox;

class ConnectionDialog : public QDialog {
  Q_OBJECT
public:
  explicit ConnectionDialog(QWidget* parent = nullptr);
  openscp::SessionOptions options() const;

private:
  QLineEdit* host_ = nullptr;
  QSpinBox*  port_ = nullptr;
  QLineEdit* user_ = nullptr;
  QLineEdit* pass_ = nullptr;
};
