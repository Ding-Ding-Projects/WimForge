#pragma once

#include <QProcessEnvironment>
#include <QString>

class QProcess;

namespace wimforge {

// Keeps captured child processes in the background on Windows without
// changing QProcess' stdout/stderr pipe setup. This is intentionally not for
// detached provider consoles, which must remain visible to the user.
void configureProcessWithoutConsole(QProcess &process);

// Resolve a bare executable through trusted Windows locations first and PATH
// second. Git is deliberately restricted to a non-reparse installation below
// Program Files because application-owned history can run automatically in an
// elevated GUI. Oscdimg is likewise restricted to the protected Windows ADK
// amd64 deployment-tools location. Relative paths are never handed to
// CreateProcess.
[[nodiscard]] QString resolveExecutableForLaunch(const QString &executable);

// Windows PowerShell loads the desktop CLR before its command text runs. This
// environment removes user-controlled profiler/runtime injection variables and
// restricts PATH/module discovery to protected Windows locations. Use it only
// for WimForge-owned PowerShell children, not arbitrary operator tools.
[[nodiscard]] QProcessEnvironment sanitizedPowerShellEnvironment();

} // namespace wimforge
