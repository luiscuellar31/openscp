#include "ConnectionDialog.hpp"
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSpinBox>

ConnectionDialog::ConnectionDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Conectar (SFTP - Mock)");
  auto* lay = new QFormLayout(this);

  host_ = new QLineEdit(this);
  port_ = new QSpinBox(this);
  user_ = new QLineEdit(this);
  pass_ = new QLineEdit(this);

  port_->setRange(1, 65535);
  port_->setValue(22);
  pass_->setEchoMode(QLineEdit::Password);

  lay->addRow("Host:", host_);
  lay->addRow("Puerto:", port_);
  lay->addRow("Usuario:", user_);
  lay->addRow("Contraseña:", pass_);

  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  lay->addRow(bb);
  connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

openscp::SessionOptions ConnectionDialog::options() const {
  openscp::SessionOptions o;
  o.host = host_->text().toStdString();
  o.port = static_cast<std::uint16_t>(port_->value());
  o.username = user_->text().toStdString();
  if (!pass_->text().isEmpty())
    o.password = pass_->text().toStdString();
  return o;
}
