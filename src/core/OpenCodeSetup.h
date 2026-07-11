#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace wimforge {

enum class OpenCodeSetupState
{
    Absent,
    Installing,
    Verifying,
    Ready,
    Failed,
};

struct OpenCodeToolCommand
{
    QString program;
    QStringList prefixArguments;

    [[nodiscard]] bool isEmpty() const { return program.trimmed().isEmpty(); }
};

struct OpenCodeProcessCommand
{
    QString program;
    QStringList arguments;
    int timeoutMs = 0;
};

struct OpenCodeProcessResult
{
    enum class Outcome
    {
        Finished,
        FailedToStart,
        TimedOut,
    };

    Outcome outcome = Outcome::Finished;
    int exitCode = -1;
    bool normalExit = false;
    QString output;
    QString error;

    [[nodiscard]] bool succeeded() const
    {
        return outcome == Outcome::Finished && normalExit && exitCode == 0;
    }
};

class OpenCodeProcessRunner
{
public:
    using Completion = std::function<void(const OpenCodeProcessResult &)>;

    virtual ~OpenCodeProcessRunner() = default;
    virtual quint64 start(const OpenCodeProcessCommand &command, Completion completed) = 0;
    virtual void shutdown() = 0;
};

struct OpenCodeSetupEnvironment
{
    std::function<QString()> openCodeExecutable;
    std::function<OpenCodeToolCommand()> npmCommand;
    std::function<QString()> wingetExecutable;
    std::function<qint64()> nowMs;
};

class OpenCodeSetup final : public QObject
{
    Q_OBJECT

public:
    using Completion = std::function<void(bool ready, const QString &error)>;

    static constexpr int VerifyTimeoutMs = 20'000;
    static constexpr int NpmInstallTimeoutMs = 300'000;
    static constexpr int WinGetInstallTimeoutMs = 600'000;

    explicit OpenCodeSetup(QObject *parent = nullptr);
    OpenCodeSetup(OpenCodeSetupEnvironment environment,
                  std::unique_ptr<OpenCodeProcessRunner> runner,
                  QObject *parent = nullptr);
    ~OpenCodeSetup() override;

    OpenCodeSetup(const OpenCodeSetup &) = delete;
    OpenCodeSetup &operator=(const OpenCodeSetup &) = delete;

    [[nodiscard]] OpenCodeSetupState state() const { return m_state; }
    [[nodiscard]] QString stateName() const;
    [[nodiscard]] QString status() const { return m_status; }
    [[nodiscard]] QString error() const { return m_error; }
    [[nodiscard]] QString version() const { return m_version; }
    [[nodiscard]] QString executablePath() const { return m_executablePath; }
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool ready() const { return m_state == OpenCodeSetupState::Ready; }
    [[nodiscard]] bool installed() const;
    [[nodiscard]] bool canRetry() const;
    [[nodiscard]] qint64 lastTransitionMs() const { return m_lastTransitionMs; }

    // Passive readiness checks never locate, install, or launch host tools.
    // retry() is the explicit operator-authorized entry point; calls made once
    // that approval exists join the same bounded attempt.
    void ensureReady(Completion completed = {});
    void retry(Completion completed = {});
    void shutdown();

signals:
    void changed();
    void becameReady(bool installedDuringAttempt);
    void failed(const QString &error);

private:
    void startAttempt();
    void startWinGetInstall();
    void startNpmInstall();
    void startVerification();
    void handleWinGetResult(quint64 generation, const OpenCodeProcessResult &result);
    void handleNpmResult(quint64 generation, const OpenCodeProcessResult &result);
    void handleVerificationResult(quint64 generation, const OpenCodeProcessResult &result);
    void transition(OpenCodeSetupState state,
                    const QString &status,
                    const QString &error = {});
    void finishReady(const QString &version);
    void finishFailed(const QString &message);
    void completePending(bool ready, const QString &error);
    [[nodiscard]] QString processFailure(const QString &operation,
                                         const OpenCodeProcessResult &result) const;

    OpenCodeSetupEnvironment m_environment;
    std::unique_ptr<OpenCodeProcessRunner> m_runner;
    OpenCodeSetupState m_state = OpenCodeSetupState::Absent;
    QString m_status;
    QString m_error;
    QString m_version;
    QString m_executablePath;
    qint64 m_lastTransitionMs = 0;
    quint64 m_generation = 0;
    bool m_installedDuringAttempt = false;
    bool m_explicitSetupAuthorized = false;
    bool m_shuttingDown = false;
    std::vector<Completion> m_pending;
};

[[nodiscard]] OpenCodeSetupEnvironment systemOpenCodeEnvironment();
[[nodiscard]] std::unique_ptr<OpenCodeProcessRunner> createSystemOpenCodeProcessRunner();

} // namespace wimforge
