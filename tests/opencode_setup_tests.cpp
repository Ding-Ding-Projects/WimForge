#include "core/OpenCodeSetup.h"

#include <QCoreApplication>
#include <QTextStream>

#include <utility>
#include <vector>

using namespace wimforge;

namespace {

class TestRun
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int result() const
    {
        if (m_failures == 0)
            QTextStream(stdout) << "opencode_setup_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

class FakeRunner final : public OpenCodeProcessRunner
{
public:
    struct Pending
    {
        quint64 id = 0;
        OpenCodeProcessCommand command;
        Completion completed;
        bool completedAlready = false;
    };

    quint64 start(const OpenCodeProcessCommand &command, Completion completed) override
    {
        const quint64 id = ++m_nextId;
        m_pending.push_back({id, command, std::move(completed), false});
        return id;
    }

    void shutdown() override
    {
        ++shutdownCalls;
        for (Pending &pending : m_pending)
            pending.completedAlready = true;
    }

    [[nodiscard]] const Pending &at(qsizetype index) const
    {
        return m_pending.at(static_cast<std::size_t>(index));
    }

    [[nodiscard]] qsizetype count() const
    {
        return static_cast<qsizetype>(m_pending.size());
    }

    void complete(qsizetype index, const OpenCodeProcessResult &result)
    {
        Pending &pending = m_pending.at(static_cast<std::size_t>(index));
        if (pending.completedAlready)
            return;
        pending.completedAlready = true;
        Completion completed = pending.completed;
        completed(result);
    }

    int shutdownCalls = 0;

private:
    quint64 m_nextId = 0;
    std::vector<Pending> m_pending;
};

struct Fixture
{
    Fixture()
    {
        auto runner = std::make_unique<FakeRunner>();
        fakeRunner = runner.get();
        OpenCodeSetupEnvironment environment{
            [this] { ++executableLookups; return executable; },
            [this] { ++npmLookups; return npm; },
            [this] { ++wingetLookups; return winget; },
            [this] { return now; },
        };
        setup = std::make_unique<OpenCodeSetup>(std::move(environment), std::move(runner));
    }

    QString executable;
    OpenCodeToolCommand npm;
    QString winget;
    qint64 now = 100;
    int executableLookups = 0;
    int npmLookups = 0;
    int wingetLookups = 0;
    FakeRunner *fakeRunner = nullptr;
    std::unique_ptr<OpenCodeSetup> setup;
};

OpenCodeProcessResult success(const QString &output = {})
{
    OpenCodeProcessResult result;
    result.outcome = OpenCodeProcessResult::Outcome::Finished;
    result.exitCode = 0;
    result.normalExit = true;
    result.output = output;
    return result;
}

void passiveReadinessNeverLaunchesUserProfileTools(TestRun &test)
{
    Fixture fixture;
    fixture.executable = QStringLiteral("C:/Users/example/AppData/Roaming/npm/opencode.exe");
    int deniedCallbacks = 0;
    fixture.setup->ensureReady([&](bool ready, const QString &error) {
        if (!ready && error.contains(QStringLiteral("explicit approval")))
            ++deniedCallbacks;
    });
    test.check(fixture.executableLookups == 0 && fixture.npmLookups == 0
                   && fixture.wingetLookups == 0 && fixture.fakeRunner->count() == 0
                   && deniedCallbacks == 1,
               QStringLiteral("passive readiness cannot resolve or launch a user-profile tool"));

    fixture.setup->retry();
    test.check(fixture.executableLookups > 0 && fixture.fakeRunner->count() == 1,
               QStringLiteral("the explicit setup action authorizes one bounded verification"));
}

void reportsAbsentToolsWithoutStartingAProcess(TestRun &test)
{
    Fixture fixture;
    test.check(fixture.setup->stateName() == QStringLiteral("absent")
                   && fixture.setup->canRetry(),
               QStringLiteral("the initial absent state is explicit and actionable"));
    int failedCallbacks = 0;
    fixture.setup->retry([&](bool ready, const QString &error) {
        if (!ready && error.contains(QStringLiteral("neither npm nor WinGet")))
            ++failedCallbacks;
    });
    test.check(fixture.setup->state() == OpenCodeSetupState::Failed
                   && fixture.fakeRunner->count() == 0 && failedCallbacks == 1,
               QStringLiteral("missing prerequisites fail once without spawning a process"));
}

void verifiesExistingInstallation(TestRun &test)
{
    Fixture fixture;
    fixture.executable = QStringLiteral("C:/npm/opencode.exe");
    int completions = 0;
    fixture.setup->retry([&](bool ready, const QString &) {
        if (ready)
            ++completions;
    });

    test.check(fixture.setup->state() == OpenCodeSetupState::Verifying,
               QStringLiteral("an existing executable enters verifying"));
    test.check(fixture.fakeRunner->count() == 1,
               QStringLiteral("existing installation starts one process"));
    const OpenCodeProcessCommand command = fixture.fakeRunner->at(0).command;
    test.check(command.program == fixture.executable
                   && command.arguments == QStringList{QStringLiteral("--version")},
               QStringLiteral("verification uses the resolved native executable"));
    test.check(command.timeoutMs == OpenCodeSetup::VerifyTimeoutMs,
               QStringLiteral("verification has a bounded timeout"));

    fixture.now = 240;
    fixture.fakeRunner->complete(0, success(QStringLiteral("opencode 1.2.3\n")));
    test.check(fixture.setup->state() == OpenCodeSetupState::Ready
                   && fixture.setup->version() == QStringLiteral("opencode 1.2.3")
                   && completions == 1,
               QStringLiteral("a version response completes the ready transition"));
    test.check(fixture.setup->lastTransitionMs() == 240,
               QStringLiteral("state transitions use the injected clock"));
}

void installsWithNpmAndQueuesCallers(TestRun &test)
{
    Fixture fixture;
    fixture.npm = {QStringLiteral("C:/Node/node.exe"),
                   {QStringLiteral("C:/Node/node_modules/npm/bin/npm-cli.js")}};
    int readyCallbacks = 0;
    int failedCallbacks = 0;
    for (int index = 0; index < 3; ++index) {
        fixture.setup->retry([&](bool ready, const QString &) {
            ready ? ++readyCallbacks : ++failedCallbacks;
        });
    }

    test.check(fixture.fakeRunner->count() == 1,
               QStringLiteral("concurrent callers share one npm installation"));
    const OpenCodeProcessCommand npmCommand = fixture.fakeRunner->at(0).command;
    test.check(npmCommand.program == fixture.npm.program
                   && npmCommand.arguments
                          == QStringList{fixture.npm.prefixArguments.first(),
                                         QStringLiteral("install"), QStringLiteral("-g"),
                                         QStringLiteral("opencode-ai@latest")},
               QStringLiteral("npm is invoked with structured arguments"));
    test.check(npmCommand.timeoutMs == OpenCodeSetup::NpmInstallTimeoutMs,
               QStringLiteral("npm installation has a bounded timeout"));

    fixture.executable = QStringLiteral("C:/npm/opencode.exe");
    fixture.fakeRunner->complete(0, success());
    test.check(fixture.fakeRunner->count() == 2
                   && fixture.setup->state() == OpenCodeSetupState::Verifying,
               QStringLiteral("successful npm install advances directly to verification"));
    fixture.fakeRunner->complete(1, success(QStringLiteral("1.4.0")));
    test.check(readyCallbacks == 3 && failedCallbacks == 0,
               QStringLiteral("all queued callers complete exactly once"));
}

void installsNodeBeforeNpm(TestRun &test)
{
    Fixture fixture;
    fixture.winget = QStringLiteral("C:/Windows/winget.exe");
    fixture.setup->retry();
    test.check(fixture.fakeRunner->count() == 1,
               QStringLiteral("missing npm starts one WinGet operation"));
    const OpenCodeProcessCommand winGetCommand = fixture.fakeRunner->at(0).command;
    test.check(winGetCommand.program == fixture.winget
                   && winGetCommand.arguments.contains(QStringLiteral("OpenJS.NodeJS.LTS"))
                   && winGetCommand.arguments.contains(QStringLiteral("--disable-interactivity")),
               QStringLiteral("WinGet install is exact and non-interactive"));
    test.check(winGetCommand.timeoutMs == OpenCodeSetup::WinGetInstallTimeoutMs,
               QStringLiteral("WinGet installation has a bounded timeout"));

    fixture.npm = {QStringLiteral("C:/Node/node.exe"),
                   {QStringLiteral("C:/Node/npm-cli.js")}};
    fixture.fakeRunner->complete(0, success());
    test.check(fixture.fakeRunner->count() == 2
                   && fixture.fakeRunner->at(1).command.program == fixture.npm.program,
               QStringLiteral("WinGet success re-resolves npm before installing OpenCode"));
    fixture.executable = QStringLiteral("C:/npm/opencode.exe");
    fixture.fakeRunner->complete(1, success());
    fixture.fakeRunner->complete(2, success(QStringLiteral("2.0.0")));
    test.check(fixture.setup->ready(),
               QStringLiteral("WinGet, npm, and verification form one attempt"));
}

void failureAndRetryAreDeterministic(TestRun &test)
{
    Fixture fixture;
    fixture.executable = QStringLiteral("C:/npm/opencode.exe");
    int failedCallbacks = 0;
    fixture.setup->retry([&](bool ready, const QString &error) {
        if (!ready && error.contains(QStringLiteral("timed out")))
            ++failedCallbacks;
    });
    OpenCodeProcessResult timeout;
    timeout.outcome = OpenCodeProcessResult::Outcome::TimedOut;
    fixture.fakeRunner->complete(0, timeout);
    test.check(fixture.setup->state() == OpenCodeSetupState::Failed
                   && fixture.setup->canRetry() && failedCallbacks == 1,
               QStringLiteral("timeout becomes an observable retryable failure"));

    int retryCallbacks = 0;
    fixture.setup->retry([&](bool ready, const QString &) {
        if (ready)
            ++retryCallbacks;
    });
    fixture.setup->retry([&](bool ready, const QString &) {
        if (ready)
            ++retryCallbacks;
    });
    test.check(fixture.fakeRunner->count() == 2,
               QStringLiteral("concurrent retries start one fresh attempt"));
    fixture.fakeRunner->complete(1, success(QStringLiteral("2.1.0")));
    test.check(fixture.setup->ready() && retryCallbacks == 2,
               QStringLiteral("every retry waiter observes the shared success"));
}

void failedStartAndShutdownAreObservable(TestRun &test)
{
    Fixture failedFixture;
    failedFixture.executable = QStringLiteral("C:/npm/opencode.exe");
    failedFixture.setup->retry();
    OpenCodeProcessResult failedStart;
    failedStart.outcome = OpenCodeProcessResult::Outcome::FailedToStart;
    failedStart.error = QStringLiteral("access denied");
    failedFixture.fakeRunner->complete(0, failedStart);
    test.check(failedFixture.setup->state() == OpenCodeSetupState::Failed
                   && failedFixture.setup->error().contains(QStringLiteral("access denied")),
               QStringLiteral("failed-to-start diagnostics reach the public error"));

    Fixture shutdownFixture;
    shutdownFixture.npm = {QStringLiteral("npm"), {}};
    int cancelledCallbacks = 0;
    shutdownFixture.setup->retry([&](bool ready, const QString &error) {
        if (!ready && error.contains(QStringLiteral("shutdown")))
            ++cancelledCallbacks;
    });
    shutdownFixture.setup->shutdown();
    shutdownFixture.setup->shutdown();
    test.check(shutdownFixture.fakeRunner->shutdownCalls == 1
                   && cancelledCallbacks == 1,
               QStringLiteral("shutdown cancels the runner and waiters exactly once"));
    const qsizetype commandCount = shutdownFixture.fakeRunner->count();
    shutdownFixture.setup->ensureReady([&](bool ready, const QString &) {
        if (!ready)
            ++cancelledCallbacks;
    });
    test.check(shutdownFixture.fakeRunner->count() == commandCount
                   && cancelledCallbacks == 2,
               QStringLiteral("setup cannot restart after clean shutdown"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);

    TestRun test;
    passiveReadinessNeverLaunchesUserProfileTools(test);
    reportsAbsentToolsWithoutStartingAProcess(test);
    verifiesExistingInstallation(test);
    installsWithNpmAndQueuesCallers(test);
    installsNodeBeforeNpm(test);
    failureAndRetryAreDeterministic(test);
    failedStartAndShutdownAreObservable(test);
    return test.result();
}
