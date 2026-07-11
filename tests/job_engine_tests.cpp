#include "core/JobEngine.h"
#include "core/StructuredLogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

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
            QTextStream(stdout) << "job_engine_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

ProjectConfig executableProject(const QString &root)
{
    ProjectConfig project;
    project.projectDirectory = QDir(root).filePath(QStringLiteral("project"));
    project.projectName = QStringLiteral("Job engine fixture");
    project.sourcePath = QDir(root).filePath(QStringLiteral("input/install.wim"));
    project.imagePath = project.sourcePath;
    project.mountPath = QDir(project.projectDirectory).filePath(QStringLiteral("mount"));
    project.outputPath = QDir(project.projectDirectory).filePath(QStringLiteral("output/result.wim"));
    project.outputFormat = QStringLiteral("wim");
    project.selectedImageIndex = 1;
    QDir().mkpath(QFileInfo(project.sourcePath).absolutePath());
    QDir().mkpath(project.projectDirectory);
    QFile source(project.sourcePath);
    if (source.open(QIODevice::WriteOnly)) {
        source.write("fixture");
        source.close();
    }
    return project;
}

ServicingOperation childOperation(const QString &id,
                                  int exitCode,
                                  const QStringList &dependsOn,
                                  const QString &markerPath,
                                  OperationState state = OperationState::Queued)
{
    ServicingOperation operation;
    operation.id = id;
    operation.kind = OperationKind::Inspect;
    operation.titleEn = id;
    operation.titleZh = id;
    operation.executable = QCoreApplication::applicationFilePath();
    operation.arguments = {
        QStringLiteral("--job-operation"),
        QString::number(exitCode),
        markerPath,
        id,
    };
    operation.dependsOn = dependsOn;
    operation.state = state;
    return operation;
}

struct EngineRunResult
{
    bool started = false;
    bool finished = false;
    bool success = false;
    bool timedOut = false;
    int finishedSignals = 0;
    QString startError;
    QString message;
    QString statusText;
    QList<ServicingOperation> operations;
};

EngineRunResult runEngine(const ProjectConfig &project,
                          const QList<ServicingOperation> &operations,
                          int maximumParallel = 2)
{
    EngineRunResult result;
    JobEngine engine;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        result.timedOut = true;
        engine.cancel();
        loop.quit();
    });
    QObject::connect(&engine, &JobEngine::finished, &loop,
                     [&](bool success, const QString &message) {
        result.finished = true;
        result.success = success;
        result.message = message;
        ++result.finishedSignals;
        loop.quit();
    });

    result.started = engine.start(project, operations, maximumParallel, &result.startError);
    if (result.started) {
        timeout.start(10000);
        loop.exec();
        timeout.stop();
    }
    result.statusText = engine.statusText();
    result.operations = engine.operations();
    return result;
}

const ServicingOperation *operationById(const QList<ServicingOperation> &operations,
                                        const QString &id)
{
    for (const ServicingOperation &operation : operations)
        if (operation.id == id)
            return &operation;
    return nullptr;
}

QStringList markerLines(const QString &path)
{
    QFile marker(path);
    if (!marker.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(marker.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

void testIntentionalSkipIsTransparent(TestRun &test, const QString &root)
{
    const ProjectConfig project = executableProject(root);
    const QString marker = QDir(root).filePath(QStringLiteral("order.txt"));
    const ServicingOperation downstream = childOperation(
        QStringLiteral("downstream"), 0, {QStringLiteral("optional")}, marker);
    const ServicingOperation optional = childOperation(
        QStringLiteral("optional"), 0, {QStringLiteral("prerequisite")}, marker,
        OperationState::Skipped);
    const ServicingOperation prerequisite = childOperation(
        QStringLiteral("prerequisite"), 0, {}, marker);

    // Deliberately reverse the dependency order. A reviewed skip must inherit
    // its prerequisites rather than allowing downstream to launch early.
    const EngineRunResult result = runEngine(project, {downstream, optional, prerequisite});
    test.check(result.started && result.finished && !result.timedOut,
               QStringLiteral("intentional-skip run finishes: %1").arg(result.startError));
    test.check(result.success && result.finishedSignals == 1,
               QStringLiteral("intentional skip is a successful reviewed omission: %1")
                   .arg(result.message));
    test.check(markerLines(marker)
                   == QStringList{QStringLiteral("prerequisite"), QStringLiteral("downstream")},
               QStringLiteral("skipped node preserves prerequisite order and is not executed"));
    const ServicingOperation *skipped = operationById(result.operations, QStringLiteral("optional"));
    const ServicingOperation *consumer = operationById(result.operations, QStringLiteral("downstream"));
    test.check(skipped && skipped->state == OperationState::Skipped,
               QStringLiteral("reviewed omission remains explicitly skipped"));
    test.check(consumer && consumer->state == OperationState::Succeeded
                   && result.statusText.contains(QStringLiteral("intentionally skipped")),
               QStringLiteral("downstream succeeds and final status names the intentional skip"));
}

void testFailureCascadesAcrossIntentionalSkip(TestRun &test, const QString &root)
{
    const ProjectConfig project = executableProject(root);
    const QString marker = QDir(root).filePath(QStringLiteral("failure-order.txt"));
    const ServicingOperation leaf = childOperation(
        QStringLiteral("leaf"), 0, {QStringLiteral("optional")}, marker);
    const ServicingOperation optional = childOperation(
        QStringLiteral("optional"), 0, {QStringLiteral("middle")}, marker,
        OperationState::Skipped);
    const ServicingOperation middle = childOperation(
        QStringLiteral("middle"), 0, {QStringLiteral("root")}, marker);
    const ServicingOperation rootFailure = childOperation(
        QStringLiteral("root"), 7, {}, marker);

    const EngineRunResult result = runEngine(project, {leaf, optional, middle, rootFailure});
    test.check(result.started && result.finished && !result.timedOut,
               QStringLiteral("failed dependency run reaches a terminal result: %1")
                   .arg(result.startError));
    test.check(!result.success && result.finishedSignals == 1,
               QStringLiteral("failed/cascaded dependencies can never report success"));
    const ServicingOperation *rootOperation = operationById(result.operations, QStringLiteral("root"));
    const ServicingOperation *middleOperation = operationById(result.operations, QStringLiteral("middle"));
    const ServicingOperation *optionalOperation = operationById(result.operations, QStringLiteral("optional"));
    const ServicingOperation *leafOperation = operationById(result.operations, QStringLiteral("leaf"));
    test.check(rootOperation && rootOperation->state == OperationState::Failed,
               QStringLiteral("root process records the real failure"));
    test.check(middleOperation && middleOperation->state == OperationState::Blocked
                   && leafOperation && leafOperation->state == OperationState::Blocked,
               QStringLiteral("reverse-ordered descendants reach an explicit blocked state"));
    test.check(optionalOperation && optionalOperation->state == OperationState::Skipped,
               QStringLiteral("manual skip remains distinct while carrying prerequisite failure"));
    test.check(markerLines(marker) == QStringList{QStringLiteral("root")}
                   && result.message.contains(QStringLiteral("2 blocked")),
               QStringLiteral("blocked operations never launch and final message reports cascade"));

    QFile journal(JobEngine::journalPathForProject(project.projectDirectory));
    const bool opened = journal.open(QIODevice::ReadOnly);
    const QJsonObject journalObject = opened
        ? QJsonDocument::fromJson(journal.readAll()).object() : QJsonObject{};
    test.check(opened && journalObject.value(QStringLiteral("status")).toString()
                              == QStringLiteral("failed"),
               QStringLiteral("recovery journal records failed, never succeeded"));
}

void testInvalidGraphsAreRejected(TestRun &test, const QString &root)
{
    const ProjectConfig project = executableProject(root);
    const QString marker = QDir(root).filePath(QStringLiteral("invalid.txt"));

    ServicingOperation missing = childOperation(
        QStringLiteral("consumer"), 0, {QStringLiteral("absent")}, marker);
    EngineRunResult result = runEngine(project, {missing});
    test.check(!result.started && result.startError.contains(QStringLiteral("missing operation")),
               QStringLiteral("engine rejects a missing dependency instead of hanging"));

    ServicingOperation first = childOperation(
        QStringLiteral("first"), 0, {QStringLiteral("second")}, marker);
    ServicingOperation second = childOperation(
        QStringLiteral("second"), 0, {QStringLiteral("first")}, marker);
    result = runEngine(project, {first, second});
    test.check(!result.started && result.startError.contains(QStringLiteral("cycle")),
               QStringLiteral("engine rejects a dependency cycle instead of hanging"));
}

void testStructuredLogPrivacy(TestRun &test,
                              const QString &root,
                              const QString &structuredLogPath)
{
    const ProjectConfig project = executableProject(root);
    const QString workingDirectory = QDir(root).filePath(
        QStringLiteral("JOB_WORKING_DIRECTORY_SENTINEL"));
    QDir().mkpath(workingDirectory);

    ServicingOperation operation;
    operation.id = QStringLiteral("privacy-operation");
    operation.kind = OperationKind::Inspect;
    operation.titleEn = QStringLiteral("Privacy operation");
    operation.titleZh = QStringLiteral("私隱測試操作");
    operation.executable = QCoreApplication::applicationFilePath();
    operation.arguments = {
        QStringLiteral("--job-privacy-operation"),
        QStringLiteral("JOB_OUTPUT_SECRET_SENTINEL"),
        QStringLiteral("--token=JOB_TOKEN_SECRET_SENTINEL"),
        QStringLiteral("C:\\JOB_ARGUMENT_PATH_SENTINEL\\payload.bin"),
    };
    operation.workingDirectory = workingDirectory;

    const EngineRunResult result = runEngine(project, {operation}, 1);
    test.check(result.started && result.finished && result.success
                   && !result.timedOut,
               QStringLiteral("structured-log privacy operation completes: %1")
                   .arg(result.startError));

    QFile log(structuredLogPath);
    test.check(log.open(QIODevice::ReadOnly),
               QStringLiteral("structured JobEngine log opens"));
    const QByteArray encoded = log.readAll();
    const QList<QByteArray> forbidden{
        QByteArrayLiteral("JOB_WORKING_DIRECTORY_SENTINEL"),
        QByteArrayLiteral("JOB_OUTPUT_SECRET_SENTINEL"),
        QByteArrayLiteral("JOB_TOKEN_SECRET_SENTINEL"),
        QByteArrayLiteral("JOB_ARGUMENT_PATH_SENTINEL"),
        QByteArrayLiteral("--job-privacy-operation"),
        QByteArrayLiteral("--token="),
    };
    for (const QByteArray &sentinel : forbidden) {
        test.check(!encoded.contains(sentinel),
                   QStringLiteral("JobEngine JSONL omits sentinel %1")
                       .arg(QString::fromLatin1(sentinel)));
    }

    bool foundStart = false;
    bool foundOutput = false;
    const QList<QByteArray> lines = encoded.split('\n');
    for (const QByteArray &line : lines) {
        const QJsonObject record = QJsonDocument::fromJson(line).object();
        const QString event = record.value(QStringLiteral("event")).toString();
        const QJsonObject data = record.value(QStringLiteral("data")).toObject();
        if (event == QStringLiteral("operation.process_starting")) {
            foundStart = true;
            test.check(data.value(QStringLiteral("argumentCount")).toInt() > 0
                           && !data.contains(QStringLiteral("program"))
                           && !data.contains(QStringLiteral("requestedProgram"))
                           && !data.contains(QStringLiteral("arguments"))
                           && !data.contains(QStringLiteral("workingDirectory"))
                           && !data.contains(QStringLiteral("operationLogPath")),
                       QStringLiteral("process-start event contains metadata only"));
        } else if (event == QStringLiteral("operation.output")) {
            foundOutput = true;
            test.check(data.value(QStringLiteral("bytes")).toInt() > 0
                           && !data.contains(QStringLiteral("output")),
                       QStringLiteral("process-output event contains byte count only"));
        }
    }
    test.check(foundStart && foundOutput,
               QStringLiteral("privacy fixture exercised process start and output logging"));

    const QString operationLogPath = QDir(project.projectDirectory).filePath(
        QStringLiteral(".wimforge/logs"));
    QDir operationLogs(operationLogPath);
    const QFileInfoList runDirectories = operationLogs.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    test.check(!runDirectories.isEmpty(),
               QStringLiteral("per-operation diagnostic directory exists"));
    QByteArray persistedOperationLog;
    if (!runDirectories.isEmpty()) {
        QFile operationLog(QDir(runDirectories.constFirst().absoluteFilePath())
                               .filePath(QStringLiteral("privacy-operation.log")));
        test.check(operationLog.open(QIODevice::ReadOnly),
                   QStringLiteral("per-operation diagnostic log opens"));
        persistedOperationLog = operationLog.readAll();
    }

    QFile journal(JobEngine::journalPathForProject(project.projectDirectory));
    test.check(journal.open(QIODevice::ReadOnly),
               QStringLiteral("recovery journal opens for privacy audit"));
    const QByteArray persistedJournal = journal.readAll();
    for (const QByteArray &sentinel : forbidden) {
        test.check(!persistedOperationLog.contains(sentinel),
                   QStringLiteral("per-operation log omits sentinel %1")
                       .arg(QString::fromLatin1(sentinel)));
        test.check(!persistedJournal.contains(sentinel),
                   QStringLiteral("recovery journal omits sentinel %1")
                       .arg(QString::fromLatin1(sentinel)));
    }
    const QJsonObject journalObject =
        QJsonDocument::fromJson(persistedJournal).object();
    const QJsonArray journalOperations =
        journalObject.value(QStringLiteral("operations")).toArray();
    test.check(!journalOperations.isEmpty()
                   && !journalOperations.at(0).toObject().contains(
                       QStringLiteral("command"))
                   && journalOperations.at(0).toObject()
                          .value(QStringLiteral("commandOmitted")).toBool()
                   && journalOperations.at(0).toObject()
                          .value(QStringLiteral("argumentCount")).toInt()
                          == operation.arguments.size(),
               QStringLiteral("recovery journal retains metadata but omits command values"));
}

int runChildOperation(const QStringList &arguments)
{
    if (arguments.size() < 5)
        return 90;
    bool validExitCode = false;
    const int exitCode = arguments.at(2).toInt(&validExitCode);
    if (!validExitCode)
        return 91;
    QFile marker(arguments.at(3));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Append))
        return 92;
    const QByteArray line = (arguments.at(4) + QLatin1Char('\n')).toUtf8();
    if (marker.write(line) != line.size() || !marker.flush())
        return 93;
    return exitCode;
}

int runPrivacyChild(const QStringList &arguments)
{
    // Deliberately echo every sensitive value. JobEngine may stream this to
    // its UI/per-operation diagnostic file, but structured JSONL must retain
    // only byte counts and never the contents.
    QTextStream output(stdout);
    output << arguments.mid(2).join(QStringLiteral(" | ")) << '\n';
    output.flush();
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeJobEngineTests"));
    if (application.arguments().value(1) == QStringLiteral("--job-operation"))
        return runChildOperation(application.arguments());
    if (application.arguments().value(1)
        == QStringLiteral("--job-privacy-operation")) {
        return runPrivacyChild(application.arguments());
    }

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    StructuredLoggerOptions loggingOptions;
    loggingOptions.directory = QDir(temporary.path()).filePath(
        QStringLiteral("structured-logs"));
    QString loggingError;
    StructuredLogger &logger = StructuredLogger::instance();
    test.check(logger.initialize(loggingOptions, &loggingError),
               QStringLiteral("structured logger initializes: %1")
                   .arg(loggingError));

    testIntentionalSkipIsTransparent(
        test, QDir(temporary.path()).filePath(QStringLiteral("intentional-skip")));
    testFailureCascadesAcrossIntentionalSkip(
        test, QDir(temporary.path()).filePath(QStringLiteral("failed-cascade")));
    testInvalidGraphsAreRejected(
        test, QDir(temporary.path()).filePath(QStringLiteral("invalid-graphs")));
    testStructuredLogPrivacy(
        test, QDir(temporary.path()).filePath(QStringLiteral("privacy")),
        logger.logPath());
    logger.shutdown();
    return test.result();
}
