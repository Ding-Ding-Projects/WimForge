#include "core/VmLabCliService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>

#include <memory>
#include <stdexcept>

using namespace wimforge::vmlab;

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
            QTextStream(stdout) << "vm_lab_cli_service_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

QString makeFile(const QString &path, const QByteArray &contents = QByteArray("fixture"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        return {};
    file.close();
    return QFileInfo(file).absoluteFilePath();
}

ProcessResult successResult(const QByteArray &output = {})
{
    ProcessResult result;
    result.started = true;
    result.exitCode = 0;
    result.standardOutput = output;
    return result;
}

class FakeRunner final : public CommandRunner
{
public:
    QList<ProcessResult> responses;
    QList<Command> commands;

    ProcessResult run(const Command &command) override
    {
        commands.append(command);
        if (responses.isEmpty())
            return successResult();
        return responses.takeFirst();
    }
};

ProviderInfo fullProvider(const QString &executable)
{
    ProviderInfo provider;
    provider.id = virtualBoxProviderId();
    provider.displayName = QStringLiteral("Fake VirtualBox");
    provider.executable = executable;
    provider.consoleExecutable = executable;
    provider.diskManagerExecutable = executable;
    provider.version = QStringLiteral("7.1-test");
    provider.available = true;
    provider.evidence = {QStringLiteral("deterministic fake evidence")};
    provider.capabilities = {
        capability::inventory(), capability::create(), capability::registerMachine(),
        capability::openConsole(), capability::lifecycle(), capability::configure(),
        capability::media(), capability::snapshots(), capability::unregisterMachine(),
        capability::deleteMachine(), capability::secureBoot(), capability::tpm(),
    };
    return provider;
}

Machine liveMachine(const QString &id = QStringLiteral("vm-1"),
                    PowerState state = PowerState::PoweredOff)
{
    Machine machine;
    machine.ref = VmRef{virtualBoxProviderId(), id, QStringLiteral("Fixture VM")};
    machine.configPath = QDir::temp().filePath(QStringLiteral("fixture.vbox"));
    machine.powerState = state;
    machine.ownership = Ownership::External;
    machine.inventoryComplete = true;
    machine.stateRevision = QStringLiteral("live-revision");
    return machine;
}

class FakeAdapter final : public VmLabProviderAdapter
{
public:
    QList<ProviderInfo> detected;
    InventoryRefreshResult inventory;
    QList<OperationRequest> planned;
    int detectCalls = 0;
    int inventoryCalls = 0;
    int snapshotParseCalls = 0;
    bool throwOnDetect = false;
    bool throwOnInventory = false;
    bool throwOnPlan = false;
    bool snapshotParseFails = false;

    QList<ProviderInfo> detect(const QList<ProviderProbePaths> &,
                               CommandRunner &) const override
    {
        FakeAdapter *self = const_cast<FakeAdapter *>(this);
        ++self->detectCalls;
        if (throwOnDetect)
            throw std::runtime_error("fake detection exception");
        return detected;
    }

    InventoryRefreshResult refreshInventory(const QList<ProviderInfo> &,
                                            const QList<Machine> &,
                                            CommandRunner &) const override
    {
        FakeAdapter *self = const_cast<FakeAdapter *>(this);
        ++self->inventoryCalls;
        if (throwOnInventory)
            throw std::runtime_error("fake inventory exception");
        return inventory;
    }

    Plan plan(const ProviderInfo &provider,
              const OperationRequest &request) const override
    {
        FakeAdapter *self = const_cast<FakeAdapter *>(this);
        self->planned.append(request);
        if (throwOnPlan)
            throw std::runtime_error("fake planning exception");

        VmRef target;
        if (request.action == ManagerAction::Create) {
            target = VmRef{request.createSpec.providerId, request.createSpec.id,
                           request.createSpec.name};
        } else if (request.action == ManagerAction::Register && request.machine) {
            target = request.machine->ref;
        } else if (request.machine) {
            target = request.machine->ref;
        }

        Risk risk = Risk::Reversible;
        if (request.action == ManagerAction::ListSnapshots
            || request.action == ManagerAction::OpenConsole) {
            risk = Risk::ReadOnly;
        } else if (request.action == ManagerAction::Delete
                   || request.action == ManagerAction::RestoreSnapshot
                   || request.action == ManagerAction::DeleteSnapshot) {
            risk = Risk::Destructive;
        }
        const Command command{
            provider.executable,
            {managerActionName(request.action), target.id},
            {}, 30000};
        Plan result;
        result.preview = makePreview(
            managerActionName(request.action), target, risk,
            {QStringLiteral("Exercise synchronous CLI routing.")}, {},
            {command}, request.revision, request.now);
        return result;
    }

    QList<Snapshot> parseSnapshots(const QString &providerId,
                                   const QByteArray &standardOutput,
                                   QString *error) const override
    {
        FakeAdapter *self = const_cast<FakeAdapter *>(this);
        ++self->snapshotParseCalls;
        if (snapshotParseFails) {
            if (error)
                *error = QStringLiteral("fake snapshot parser failure");
            return {};
        }
        if (error)
            error->clear();
        return {
            Snapshot{providerId + QStringLiteral("-snapshot"),
                     QString::fromUtf8(standardOutput).trimmed(),
                     QStringLiteral("fake snapshot"), {}, true, {}},
        };
    }
};

struct Fixture
{
    explicit Fixture(const QString &root)
        : managedRoot(QDir(root).filePath(QStringLiteral("managed"))),
          catalogPath(QDir(root).filePath(QStringLiteral("state/catalog.json"))),
          executable(makeFile(QDir(root).filePath(QStringLiteral("provider/fake-provider.exe")))),
          adapter(std::make_shared<FakeAdapter>()),
          service(catalogPath, managedRoot, adapter)
    {
        QDir().mkpath(managedRoot);
        adapter->detected = {fullProvider(executable)};
        adapter->inventory.success = true;
        adapter->inventory.complete = true;
        adapter->inventory.machines = {liveMachine()};
    }

    QString managedRoot;
    QString catalogPath;
    QString executable;
    std::shared_ptr<FakeAdapter> adapter;
    VmLabCliService service;
};

VmLabCliRequest requestFor(const QString &action)
{
    VmLabCliRequest request;
    request.action = action;
    request.now = QDateTime::fromString(QStringLiteral("2026-07-10T18:00:00Z"), Qt::ISODate);
    request.parameters = QJsonObject{
        {QStringLiteral("providerId"), virtualBoxProviderId()},
        {QStringLiteral("vmId"), QStringLiteral("vm-1")},
    };
    return request;
}

void testInspectionSurfaces(TestRun &test, const QString &root)
{
    Fixture fixture(root);
    FakeRunner runner;

    VmLabCliRequest paths;
    paths.action = QStringLiteral("paths");
    const VmLabCliResult pathResult = fixture.service.handle(paths, runner);
    test.check(pathResult.success && fixture.adapter->detectCalls == 0
                   && pathResult.output.value(QStringLiteral("paths")).toObject()
                          .value(QStringLiteral("catalog")).toString() == fixture.catalogPath,
               QStringLiteral("paths returns exact catalog/managed roots without probing providers"));

    VmLabCliRequest detect;
    detect.action = QStringLiteral("detect");
    const VmLabCliResult detectResult = fixture.service.handle(detect, runner);
    test.check(detectResult.success && fixture.adapter->detectCalls == 1
                   && detectResult.output.value(QStringLiteral("providers")).toArray().size() == 1
                   && detectResult.output.value(QStringLiteral("providerCount")).toInt() == 1,
               QStringLiteral("detect routes synchronously through the injected provider adapter"));

    VmLabCliRequest inventory;
    inventory.action = QStringLiteral("inventory");
    const VmLabCliResult inventoryResult = fixture.service.handle(inventory, runner);
    const QJsonObject inventoryObject = inventoryResult.output
                                            .value(QStringLiteral("inventory")).toObject();
    test.check(inventoryResult.success && fixture.adapter->inventoryCalls == 1
                   && inventoryObject.value(QStringLiteral("complete")).toBool()
                   && inventoryObject.value(QStringLiteral("machineCount")).toInt() == 1,
               QStringLiteral("inventory exposes complete live state and counts"));

    VmLabCliRequest catalog;
    catalog.action = QStringLiteral("catalog");
    const VmLabCliResult catalogResult = fixture.service.handle(catalog, runner);
    test.check(catalogResult.success
                   && catalogResult.output.value(QStringLiteral("catalogRevision")).isString(),
               QStringLiteral("catalog inspection is JSON-friendly and revisioned"));
}

QJsonObject parametersForAction(const QString &action, const Fixture &fixture)
{
    QJsonObject parameters{
        {QStringLiteral("providerId"), virtualBoxProviderId()},
        {QStringLiteral("vmId"), QStringLiteral("vm-1")},
    };
    if (action == QStringLiteral("create")) {
        parameters.remove(QStringLiteral("vmId"));
        parameters.insert(QStringLiteral("id"), QStringLiteral("11111111-1111-1111-1111-111111111111"));
        parameters.insert(QStringLiteral("name"), QStringLiteral("Created VM"));
        parameters.insert(QStringLiteral("isoPath"),
                          makeFile(QDir(fixture.managedRoot).filePath(QStringLiteral("media.iso"))));
        parameters.insert(QStringLiteral("cpuCount"), 4);
        parameters.insert(QStringLiteral("memoryMiB"), 8192);
    } else if (action == QStringLiteral("register")) {
        parameters.remove(QStringLiteral("vmId"));
        parameters.insert(QStringLiteral("configPath"),
                          makeFile(QDir(fixture.managedRoot).filePath(QStringLiteral("external.vbox"))));
        parameters.insert(QStringLiteral("name"), QStringLiteral("Imported VM"));
    } else if (action == QStringLiteral("configure")) {
        parameters.insert(QStringLiteral("cpuCount"), 8);
        parameters.insert(QStringLiteral("firmware"), QStringLiteral("efi"));
    } else if (action == QStringLiteral("attach-iso")) {
        parameters.insert(QStringLiteral("isoPath"),
                          makeFile(QDir(fixture.managedRoot).filePath(QStringLiteral("attached.iso"))));
    } else if (action == QStringLiteral("attach-storage")) {
        parameters.insert(QStringLiteral("path"),
                          makeFile(QDir(fixture.managedRoot).filePath(QStringLiteral("disk.vdi"))));
        parameters.insert(QStringLiteral("bus"), QStringLiteral("sata"));
        parameters.insert(QStringLiteral("port"), 2);
    } else if (action == QStringLiteral("detach-storage")) {
        parameters.insert(QStringLiteral("bus"), QStringLiteral("sata"));
        parameters.insert(QStringLiteral("port"), 2);
    } else if (action == QStringLiteral("attach-network")) {
        parameters.insert(QStringLiteral("slot"), 2);
        parameters.insert(QStringLiteral("networkMode"), QStringLiteral("bridged"));
        parameters.insert(QStringLiteral("interfaceName"), QStringLiteral("Ethernet 2"));
    } else if (action == QStringLiteral("detach-network")) {
        parameters.insert(QStringLiteral("slot"), 2);
    } else if (action == QStringLiteral("take-snapshot")) {
        parameters.insert(QStringLiteral("name"), QStringLiteral("Before updates"));
        parameters.insert(QStringLiteral("description"), QStringLiteral("Review fixture"));
    } else if (action == QStringLiteral("restore-snapshot")
               || action == QStringLiteral("delete-snapshot")) {
        parameters.insert(QStringLiteral("snapshotId"),
                          virtualBoxProviderId() + QStringLiteral("-snapshot"));
        parameters.insert(QStringLiteral("snapshotName"), QStringLiteral("Before updates"));
    } else if (action == QStringLiteral("start")) {
        parameters.insert(QStringLiteral("headless"), true);
    }
    return parameters;
}

void testEveryOperationRoutesToReview(TestRun &test, const QString &root)
{
    Fixture fixture(root);
    FakeRunner runner;
    const QStringList actions{
        QStringLiteral("create"), QStringLiteral("register"),
        QStringLiteral("open-console"), QStringLiteral("start"),
        QStringLiteral("graceful-shutdown"), QStringLiteral("power-off"),
        QStringLiteral("pause"), QStringLiteral("resume"), QStringLiteral("reset"),
        QStringLiteral("save-state"), QStringLiteral("configure"),
        QStringLiteral("attach-iso"), QStringLiteral("detach-iso"),
        QStringLiteral("attach-storage"), QStringLiteral("detach-storage"),
        QStringLiteral("attach-network"), QStringLiteral("detach-network"),
        QStringLiteral("list-snapshots"), QStringLiteral("take-snapshot"),
        QStringLiteral("restore-snapshot"), QStringLiteral("delete-snapshot"),
        QStringLiteral("unregister"), QStringLiteral("delete"),
    };

    for (const QString &action : actions) {
        VmLabCliRequest request = requestFor(action);
        request.parameters = parametersForAction(action, fixture);
        const VmLabCliResult result = fixture.service.handle(request, runner);
        const QJsonObject preview = result.output.value(QStringLiteral("preview")).toObject();
        test.check(result.success && preview.value(QStringLiteral("action")).toString() == action
                       && preview.value(QStringLiteral("commands")).toArray().size() == 1
                       && result.output.value(QStringLiteral("mode")).toString()
                              == QStringLiteral("review"),
                   QStringLiteral("action '%1' produces a reviewed plan").arg(action));
    }
    test.check(fixture.adapter->planned.size() == actions.size() + 2,
                QStringLiteral("every supported action plus two mandatory snapshot refresh plans reached the adapter"));
}

void testCapabilityAndStateGates(TestRun &test, const QString &root)
{
    Fixture fixture(root);
    FakeRunner runner;

    fixture.adapter->detected[0].capabilities.remove(capability::snapshots());
    VmLabCliRequest snapshot = requestFor(QStringLiteral("take-snapshot"));
    snapshot.parameters.insert(QStringLiteral("name"), QStringLiteral("blocked"));
    const VmLabCliResult missingCapability = fixture.service.handle(snapshot, runner);
    test.check(!missingCapability.success
                   && missingCapability.error.contains(QStringLiteral("capability"))
                   && fixture.adapter->planned.isEmpty(),
               QStringLiteral("unproven provider capabilities fail before planning"));

    fixture.adapter->detected[0].capabilities.insert(capability::snapshots());
    fixture.adapter->inventory.machines = {liveMachine(QStringLiteral("vm-1"), PowerState::Running)};
    VmLabCliRequest configure = requestFor(QStringLiteral("configure"));
    configure.parameters.insert(QStringLiteral("cpuCount"), 6);
    const VmLabCliResult runningEdit = fixture.service.handle(configure, runner);
    test.check(!runningEdit.success
                   && runningEdit.error.contains(QStringLiteral("powered-off"))
                   && fixture.adapter->planned.isEmpty(),
               QStringLiteral("configuration and device edits require powered-off live state"));

    Machine incomplete = liveMachine();
    incomplete.inventoryComplete = false;
    incomplete.stateRevision.clear();
    fixture.adapter->inventory.machines = {incomplete};
    const VmLabCliResult incompleteState = fixture.service.handle(
        requestFor(QStringLiteral("start")), runner);
    test.check(!incompleteState.success
                   && incompleteState.error.contains(QStringLiteral("Complete live state")),
               QStringLiteral("stale catalog-only state cannot authorize lifecycle review"));

    fixture.adapter->detected[0].available = false;
    const VmLabCliResult unavailable = fixture.service.handle(
        requestFor(QStringLiteral("start")), runner);
    test.check(!unavailable.success
                   && unavailable.error.contains(QStringLiteral("not available")),
               QStringLiteral("unavailable providers cannot be routed"));
}

void testConfirmationAndCatalogSemantics(TestRun &test, const QString &root)
{
    Fixture fixture(root);
    FakeRunner runner;
    VmLabCliRequest create = requestFor(QStringLiteral("create"));
    create.parameters = parametersForAction(QStringLiteral("create"), fixture);
    create.execute = true;

    const VmLabCliResult noYes = fixture.service.handle(create, runner);
    test.check(!noYes.success && noYes.exitCode == VmLabCliResult::ConfirmationRequired
                   && runner.commands.isEmpty()
                   && noYes.output.value(QStringLiteral("preview")).isObject(),
               QStringLiteral("execution never occurs without explicit --yes after preview"));

    create.yes = true;
    const VmLabCliResult created = fixture.service.handle(create, runner);
    test.check(created.success && runner.commands.size() == 1
                   && QFileInfo::exists(fixture.catalogPath),
               QStringLiteral("confirmed create executes and persists the catalog"));
    Catalog catalog(fixture.catalogPath);
    QString error;
    test.check(catalog.load(&error) && catalog.machines().size() == 1
                   && catalog.machines().first().ownership == Ownership::Managed
                   && QFileInfo(catalog.machines().first().configPath).isAbsolute(),
               QStringLiteral("create catalog entry has stable ownership and absolute config path"));

    runner.commands.clear();
    VmLabCliRequest deletion = requestFor(QStringLiteral("delete"));
    deletion.execute = true;
    const VmLabCliResult deleteNoYes = fixture.service.handle(deletion, runner);
    deletion.yes = true;
    const VmLabCliResult deleteNoToken = fixture.service.handle(deletion, runner);
    deletion.typedConfirmation = QStringLiteral("DELETE Fixture VM");
    const VmLabCliResult deleted = fixture.service.handle(deletion, runner);
    test.check(!deleteNoYes.success && !deleteNoToken.success && deleted.success
                   && deleteNoToken.exitCode == VmLabCliResult::ConfirmationRequired
                   && runner.commands.size() == 1,
               QStringLiteral("destructive execution needs --yes plus the exact typed token"));
}

void testSnapshotExecutionAndProviderFailures(TestRun &test, const QString &root)
{
    Fixture fixture(root);
    FakeRunner runner;
    VmLabCliRequest snapshots = requestFor(QStringLiteral("list-snapshots"));
    snapshots.execute = true;
    snapshots.yes = true;
    runner.responses.append(successResult("Snapshot from fake provider\n"));
    const VmLabCliResult listed = fixture.service.handle(snapshots, runner);
    test.check(listed.success && fixture.adapter->snapshotParseCalls == 1
                   && listed.output.value(QStringLiteral("snapshotCount")).toInt() == 1
                   && listed.output.value(QStringLiteral("snapshots")).toArray().first()
                          .toObject().value(QStringLiteral("name")).toString()
                              == QStringLiteral("Snapshot from fake provider"),
               QStringLiteral("reviewed snapshot inventory executes synchronously and parses provider output"));

    runner.commands.clear();
    const QString hostileId = QStringLiteral("vm & $(still-one-argument)");
    fixture.adapter->inventory.machines = {liveMachine(hostileId)};
    VmLabCliRequest start = requestFor(QStringLiteral("start"));
    start.parameters.insert(QStringLiteral("vmId"), hostileId);
    start.execute = true;
    start.yes = true;
    ProcessResult failed;
    failed.started = true;
    failed.exitCode = 17;
    failed.standardError = QByteArray(9000, 'E');
    runner.responses.append(failed);
    const VmLabCliResult providerFailure = fixture.service.handle(start, runner);
    const QJsonObject evidence = providerFailure.output
                                     .value(QStringLiteral("evidence")).toObject();
    const QJsonObject entry = evidence.value(QStringLiteral("commands"))
                                  .toArray().first().toObject();
    const QJsonObject command = entry.value(QStringLiteral("command")).toObject();
    const QJsonObject standardError = entry.value(QStringLiteral("standardError")).toObject();
    test.check(!providerFailure.success
                   && providerFailure.exitCode == VmLabCliResult::ProviderFailure
                   && command.value(QStringLiteral("arguments")).toArray().at(1).toString() == hostileId
                   && !command.contains(QStringLiteral("commandLine"))
                   && standardError.value(QStringLiteral("byteCount")).toInteger() == 9000
                   && standardError.value(QStringLiteral("truncated")).toBool()
                   && standardError.value(QStringLiteral("text")).toString().size() == 4096,
               QStringLiteral("provider failures retain bounded evidence and never flatten arguments into a shell string"));

    const QByteArray json = QJsonDocument(providerFailure.output).toJson(QJsonDocument::Compact);
    test.check(!json.contains("cmd.exe") && !json.contains("powershell")
                   && runner.commands.first().arguments.size() == 2
                   && runner.commands.first().arguments.at(1) == hostileId,
               QStringLiteral("hostile-looking values remain one structured provider argument"));

    fixture.adapter->throwOnDetect = true;
    VmLabCliRequest detect;
    detect.action = QStringLiteral("detect");
    const VmLabCliResult exception = fixture.service.handle(detect, runner);
    test.check(!exception.success
                   && exception.error.contains(QStringLiteral("fake detection exception")),
               QStringLiteral("provider adapter exceptions become bounded CLI failures"));
}

void testInvalidRequests(TestRun &test, const QString &root)
{
    Fixture fixture(root);
    FakeRunner runner;

    VmLabCliRequest unsupported;
    unsupported.action = QStringLiteral("run-arbitrary-shell");
    const VmLabCliResult unsupportedResult = fixture.service.handle(unsupported, runner);
    test.check(!unsupportedResult.success
                   && unsupportedResult.exitCode == VmLabCliResult::InvalidRequest
                   && unsupportedResult.output.value(QStringLiteral("supportedActions"))
                          .toArray().contains(QStringLiteral("attach-storage")),
               QStringLiteral("unknown actions are rejected with an explicit allowlist"));

    VmLabCliRequest badInteger = requestFor(QStringLiteral("configure"));
    badInteger.parameters.insert(QStringLiteral("cpuCount"), 2.5);
    const VmLabCliResult badIntegerResult = fixture.service.handle(badInteger, runner);
    test.check(!badIntegerResult.success
                   && badIntegerResult.error.contains(QStringLiteral("integer")),
               QStringLiteral("JSON numeric parameters must be bounded integers"));

    const VmLabCliService relative(QStringLiteral("relative.json"), fixture.managedRoot,
                                   fixture.adapter);
    VmLabCliRequest paths;
    paths.action = QStringLiteral("paths");
    const VmLabCliResult badPath = relative.handle(paths, runner);
    test.check(!badPath.success
                   && badPath.error.contains(QStringLiteral("absolute")),
               QStringLiteral("catalog and managed roots cannot depend on process working directory"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeVmLabCliServiceTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    testInspectionSurfaces(test, QDir(temporary.path()).filePath(QStringLiteral("inspection")));
    testEveryOperationRoutesToReview(test, QDir(temporary.path()).filePath(QStringLiteral("routing")));
    testCapabilityAndStateGates(test, QDir(temporary.path()).filePath(QStringLiteral("gates")));
    testConfirmationAndCatalogSemantics(test, QDir(temporary.path()).filePath(QStringLiteral("confirm")));
    testSnapshotExecutionAndProviderFailures(test, QDir(temporary.path()).filePath(QStringLiteral("failure")));
    testInvalidRequests(test, QDir(temporary.path()).filePath(QStringLiteral("invalid")));
    return test.result();
}
