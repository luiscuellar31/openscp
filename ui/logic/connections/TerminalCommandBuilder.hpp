#pragma once

#include "openscp/SessionOptions.hpp"

#include <QString>

#include <functional>

namespace openscpui {

struct TerminalCommandResult {
    QString command;
    QString error;
    bool hasSftpFallback = false;

    bool isValid() const { return !command.isEmpty() && error.isEmpty(); }
};

class TerminalCommandBuilder {
    public:
    using ExecutableLookup = std::function<QString(const QString &)>;

    explicit TerminalCommandBuilder(ExecutableLookup executableLookup = {});

    TerminalCommandResult prepare(const openscp::SessionOptions &session,
                                  const QString &remotePath,
                                  bool forceInteractiveLogin,
                                  bool enableSftpCliFallback) const;

    bool launch(const QString &shellCommand, QString *error = nullptr) const;

    private:
    ExecutableLookup executableLookup_;
};

} // namespace openscpui
