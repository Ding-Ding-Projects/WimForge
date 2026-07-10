#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace wimforge {

enum class CliExitCode
{
    Success = 0,
    Usage = 2,
    Validation = 3,
    NotFound = 4,
    ConfirmationRequired = 5,
    ExternalProcessFailed = 6,
    Conflict = 7,
    IoError = 8,
    InternalError = 10,
};

struct CliProcessResult
{
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;

    [[nodiscard]] bool ok() const
    {
        return started && finished && exitCode == 0;
    }
};

struct CliResult
{
    CliExitCode code = CliExitCode::Success;
    QString standardOutput;
    QString standardError;

    [[nodiscard]] int exitCode() const { return static_cast<int>(code); }
    [[nodiscard]] bool ok() const { return code == CliExitCode::Success; }
};

// CliRunner owns no UI state. Callers may replace processInvoker to route
// servicing operations through JobEngine or to provide a deterministic test
// double. The default invoker runs one executable directly through QProcess;
// command strings are never evaluated by a shell.
struct CliDependencies
{
    using ProcessInvoker = std::function<CliProcessResult(
        const QString &executable,
        const QStringList &arguments,
        const QString &workingDirectory)>;

    ProcessInvoker processInvoker;
};

class CliRunner
{
public:
    explicit CliRunner(CliDependencies dependencies = {});

    // arguments excludes argv[0]. The returned strings always end in a single
    // newline when non-empty, which makes both terminal and test output stable.
    [[nodiscard]] CliResult run(const QStringList &arguments) const;

    [[nodiscard]] static QString helpText();

private:
    CliDependencies m_dependencies;
};

} // namespace wimforge
