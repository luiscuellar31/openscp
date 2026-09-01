#pragma once

#include <QString>

class QWidget;

namespace openscpui::detail {

enum class UploadSourceType { Canceled, Files, Folders };

[[nodiscard]] UploadSourceType chooseUploadSourceType(QWidget *parent,
                                                      const QString &title);

} // namespace openscpui::detail
