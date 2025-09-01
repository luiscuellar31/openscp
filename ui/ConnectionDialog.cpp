#include "ConnectionDialog.hpp"
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QFileDialog>

ConnectionDialog::ConnectionDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Conectar (SFTP)");
  auto* lay = new QFormLayout(this);

  host_ = new QLineEdit(this);
  port_ = new QSpinBox(this);
  user_ = new QLineEdit(this);
  pass_ = new QLineEdit(this);

  // NUEVO: campos de clave privada (opcionales)
  keyPath_ = new QLineEdit(this);
  keyPass_ = new QLineEdit(this);

  // Defaults útiles
  host_->setText("localhost");
  port_->setRange(1, 65535);
  port_->setValue(22);
  user_->setText(QString::fromLocal8Bit(qgetenv("USER")));

  pass_->setEchoMode(QLineEdit::Password);
  keyPass_->setEchoMode(QLineEdit::Password);

  // Layout
  lay->addRow("Host:", host_);
  lay->addRow("Puerto:", port_);
  lay->addRow("Usuario:", user_);
  lay->addRow("Contraseña:", pass_);
  lay->addRow("Ruta clave privada:", keyPath_);
  lay->addRow("Passphrase clave:", keyPass_);

  // (Opcional) botón para elegir archivo de clave privada
  // Si lo quieres, descomenta estas 6 líneas:
  /*
  auto* browseBtn = new QPushButton("Elegir clave…", this);
  lay->addRow("", browseBtn);
  connect(browseBtn, &QPushButton::clicked, this, [this]{
    const QString f = QFileDialog::getOpenFileName(this, "Selecciona clave privada", QDir::homePath() + "/.ssh");
    if (!f.isEmpty()) keyPath_->setText(f);
  });
  */

  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  lay->addRow(bb);
  connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

openscp::SessionOptions ConnectionDialog::options() const {
  openscp::SessionOptions o;
  o.host     = host_->text().toStdString();
  o.port     = static_cast<std::uint16_t>(port_->value());
  o.username = user_->text().toStdString();

  // Password (si se escribió)
  if (!pass_->text().isEmpty())
    o.password = pass_->text().toStdString();

  // Clave privada (si se escribió)
  if (!keyPath_->text().isEmpty())
    o.private_key_path = keyPath_->text().toStdString();

  // Passphrase de la clave (si se escribió)
  if (!keyPass_->text().isEmpty())
    o.private_key_passphrase = keyPass_->text().toStdString();

  return o;
}