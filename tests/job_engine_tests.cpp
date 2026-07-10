#include "core/JobEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeJobEngineTests"));
    if (application.arguments().value(1) == QStringLiteral("--job-operation"))
        return runChildOperation(application.arguments());

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    testIntentionalSkipIsTransparent(
        test, QDir(temporary.path()).filePath(QStringLiteral("intentional-skip")));
    testFailureCascadesAcrossIntentionalSkip(
        test, QDir(temporary.path()).filePath(QStringLiteral("failed-cascade")));
    testInvalidGraphsAreRejected(
        test, QDir(temporary.path()).filePath(QStringLiteral("invalid-graphs")));
    return test.result();
}
