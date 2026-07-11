#include "core/VmLabManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <deque>
#include <mutex>

using namespace wimforge::vmlab;

namespace {

class TestRun
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++failures;
        qCritical().noquote() << QStringLiteral("FAIL: %1").arg(message);
    }

    int failures = 0;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

ProcessResult successResult(const QByteArray &output = {}, const QByteArray &error = {})
{
    ProcessResult result;
    result.started = true;
    result.exitCode = 0;
    result.standardOutput = output;
    result.standardError = error;
    return result;
}

QString fakeExecutable(const QString &name)
{
    return QDir(QDir::rootPath()).filePath(QStringLiteral("fake/") + name);
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

struct FakeRunnerState
{
    mutable std::mutex mutex;
    std::deque<ProcessResult> responses;
    QList<Command> commands;
    std::atomic_bool cancelled = false;
    std::atomic_bool block = false;
};

class FakeCancelableRunner final : public CancelableCommandRunner
{
public:
    explicit FakeCancelableRunner(std::shared_ptr<FakeRunnerState> state)
        : m_state(std::move(state))
    {
    }

    ProcessResult run(const Command &command) override
    {
        {
            const std::lock_guard lock(m_state->mutex);
            m_state->commands.append(command);
        }
        while (m_state->block.load() && !m_state->cancelled.load())
            QThread::msleep(2);
        if (m_state->cancelled.load()) {
            ProcessResult cancelled;
            cancelled.error = QStringLiteral("cancelled by fake runner");
            return cancelled;
        }
        const std::lock_guard lock(m_state->mutex);
        if (m_state->responses.empty())
            return successResult();
        const ProcessResult result = m_state->responses.front();
        m_state->responses.pop_front();
        return result;
    }

    void requestCancel() override { m_state->cancelled.store(true); }
    bool cancellationRequested() const override { return m_state->cancelled.load(); }

private:
    std::shared_ptr<FakeRunnerState> m_state;
};

class FakeRunnerFactory final : public CommandRunnerFactory
{
public:
    explicit FakeRunnerFactory(std::shared_ptr<FakeRunnerState> state)
        : m_state(std::move(state))
    {
    }

    std::shared_ptr<CancelableCommandRunner> create() const override
    {
        m_state->cancelled.store(false);
        return std::make_shared<FakeCancelableRunner>(m_state);
    }

private:
    std::shared_ptr<FakeRunnerState> m_state;
};

ProviderInfo fakeProvider()
{
    ProviderInfo provider;
    provider.id = virtualBoxProviderId();
    provider.displayName = QStringLiteral("Fake VirtualBox");
    provider.executable = fakeExecutable(QStringLiteral("VBoxManage.exe"));
    provider.consoleExecutable = fakeExecutable(QStringLiteral("VirtualBox.exe"));
    provider.version = QStringLiteral("7.1.0");
    provider.available = true;
    provider.capabilities = {
        capability::inventory(), capability::create(), capability::registerMachine(),
        capability::openConsole(), capability::lifecycle(), capability::configure(),
        capability::media(), capability::snapshots(), capability::unregisterMachine(),
        capability::deleteMachine(), capability::tpm()};
    return provider;
}

Machine fakeMachine(const QString &configPath,
                    PowerState state = PowerState::PoweredOff)
{
    Machine machine;
    machine.ref = VmRef{virtualBoxProviderId(), QStringLiteral("vm-1"),
                        QStringLiteral("Windows Test")};
    machine.configPath = configPath;
    machine.powerState = state;
    machine.ownership = Ownership::Managed;
    machine.inventoryComplete = true;
    machine.hardwareInventoryComplete = true;
    machine.stateRevision = QStringLiteral("live-revision");
    return machine;
}

class FakeProviderAdapter final : public VmLabProviderAdapter
{
public:
    QList<ProviderInfo> detect(const QList<ProviderProbePaths> &,
                               CommandRunner &runner) const override
    {
        runner.run(Command{fakeExecutable(QStringLiteral("detect.exe")), {}, {}, 1000});
        return {providerInfo};
    }

    InventoryRefreshResult refreshInventory(const QList<ProviderInfo> &,
                                             const QList<Machine> &,
                                             CommandRunner &runner) const override
    {
        runner.run(Command{fakeExecutable(QStringLiteral("inventory.exe")), {}, {}, 1000});
        FakeProviderAdapter *self = const_cast<FakeProviderAdapter *>(this);
        const int refreshIndex = self->refreshCalls++;
        InventoryRefreshResult result;
        result.success = true;
        result.complete = true;
        result.machines = refreshIndex < inventorySequence.size()
            ? inventorySequence.at(refreshIndex) : inventory;
        return result;
    }

    Plan plan(const ProviderInfo &provider,
              const OperationRequest &request) const override
    {
        lastRequest = request;
        if (!provider.available)
            return errorPlan(QStringLiteral("provider unavailable"));
        VmRef target;
        if (request.action == ManagerAction::Create) {
            target = VmRef{provider.id, QStringLiteral("created-id"),
                           request.createSpec.name};
        } else if (request.action == ManagerAction::Register) {
            target = VmRef{provider.id, QStringLiteral("registered-id"),
                           request.machine ? request.machine->ref.name : request.name};
        } else if (request.machine) {
            target = request.machine->ref;
        }
        if (!target.valid())
            return errorPlan(QStringLiteral("missing target"));
        const Risk risk = request.action == ManagerAction::Delete
                || request.action == ManagerAction::DeleteSnapshot
                || request.action == ManagerAction::RestoreSnapshot
            ? Risk::Destructive : Risk::Reversible;
        Plan result;
        result.preview = makePreview(
            managerActionName(request.action), target, risk,
            {QStringLiteral("fake reviewed effect")}, {},
            {Command{fakeExecutable(QStringLiteral("provider.exe")),
                     {managerActionName(request.action)}, {}, 1000}},
            request.revision, request.now);
        if (guardedDeletion && request.action == ManagerAction::Delete
            && request.machine) {
            const DeletionGuard guard = PathPolicy::managedDeletionGuard(
                *request.machine, guardedRoot, request.allMachines);
            if (!guard.allowed)
                return errorPlan(guard.error);
            result.managedDeletionAfterCommands = ManagedDeletion{
                *request.machine, guardedRoot, request.allMachines,
                guard.identity, guard.rootIdentity, true};
        }
        return result;
    }

    QList<Snapshot> parseSnapshots(const QString &,
                                   const QByteArray &standardOutput,
                                   QString *error) const override
    {
        if (!standardOutput.contains("snapshot-one")) {
            if (error)
                *error = QStringLiteral("missing fake snapshot output");
            return {};
        }
        if (error)
            error->clear();
        return {
            Snapshot{QStringLiteral("one"), QStringLiteral("snapshot-one"),
                     {}, {}, false, {}},
            Snapshot{QStringLiteral("two"), QStringLiteral("snapshot-two"),
                     {}, {}, false, {}}};
    }

    static Plan errorPlan(const QString &error)
    {
        Plan result;
        result.errors.append(error);
        return result;
    }

    ProviderInfo providerInfo = fakeProvider();
    QList<Machine> inventory;
    QList<QList<Machine>> inventorySequence;
    mutable int refreshCalls = 0;
    bool guardedDeletion = false;
    QString guardedRoot;
    mutable OperationRequest lastRequest;
};

void testDetectionInventoryAndSelection(TestRun &test, const QString &root)
{
    const auto state = std::make_shared<FakeRunnerState>();
    state->responses.push_back(successResult("detected\n"));
    state->responses.push_back(successResult("inventory\n"));
    const auto adapter = std::make_shared<FakeProviderAdapter>();
    adapter->inventory = {fakeMachine(QDir(root).filePath(QStringLiteral("vm/one.vbox")))};
    VmLabManager manager(QDir(root).filePath(QStringLiteral("catalog.json")), root,
                         adapter, std::make_shared<FakeRunnerFactory>(state));
    manager.setAutoRefresh(false);
    test.check(manager.load(), QStringLiteral("manager loads an empty catalog"));
    test.check(manager.detectProviders(), QStringLiteral("provider detection starts asynchronously"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("provider detection completes"));
    test.check(manager.providers().size() == 1
                   && manager.providers().first().available,
               QStringLiteral("detected provider evidence is retained"));
    test.check(manager.refreshInventory(), QStringLiteral("inventory refresh starts"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("inventory refresh completes"));
    test.check(manager.machines().size() == 1
                   && manager.machines().first().inventoryComplete,
               QStringLiteral("complete live inventory replaces stale catalog state"));
    test.check(manager.selectMachine(virtualBoxProviderId(), QStringLiteral("vm-1"))
                   && manager.selectedMachine().has_value(),
               QStringLiteral("provider-neutral selected VM is stable by provider and ID"));
    test.check(manager.evidenceHistory().size() == 2
                   && manager.evidenceHistory().first().commands.size() == 1,
               QStringLiteral("detection and inventory keep structured command evidence"));
}

void prepareSelectedManager(VmLabManager &manager,
                            FakeProviderAdapter &adapter,
                            TestRun &test)
{
    manager.setAutoRefresh(false);
    test.check(manager.load(), QStringLiteral("prepared manager loads"));
    test.check(manager.detectProviders(), QStringLiteral("prepared manager detects"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("prepared detection completes"));
    test.check(manager.refreshInventory(), QStringLiteral("prepared manager refreshes"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("prepared refresh completes"));
    test.check(manager.selectMachine(adapter.inventory.first().ref.providerId,
                                     adapter.inventory.first().ref.id),
               QStringLiteral("prepared manager selects VM"));
}

void testReviewedExecutionAndEvidence(TestRun &test, const QString &root)
{
    const auto state = std::make_shared<FakeRunnerState>();
    state->responses.push_back(successResult());
    state->responses.push_back(successResult());
    state->responses.push_back(successResult("provider stdout\n", "provider warning\n"));
    const auto adapter = std::make_shared<FakeProviderAdapter>();
    adapter->inventory = {fakeMachine(QDir(root).filePath(QStringLiteral("vm/two.vbox")))};
    VmLabManager manager(QDir(root).filePath(QStringLiteral("catalog.json")), root,
                         adapter, std::make_shared<FakeRunnerFactory>(state));
    prepareSelectedManager(manager, *adapter, test);
    const std::optional<OperationPreview> preview = manager.reviewStart(true);
    test.check(preview.has_value() && preview->action == QStringLiteral("start")
                   && adapter->lastRequest.headless,
               QStringLiteral("lifecycle review preserves selected VM and headless setting"));
    test.check(!manager.executeReviewed(QUuid::createUuid()),
               QStringLiteral("a different preview ID cannot execute"));
    test.check(manager.executeReviewed(preview->id),
               QStringLiteral("the exact reviewed lifecycle preview executes"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("reviewed lifecycle execution completes"));
    const OperationEvidence evidence = manager.evidenceHistory().constLast();
    test.check(evidence.success && evidence.commands.size() == 1
                   && evidence.commands.first().result.standardOutput.contains("provider stdout")
                   && evidence.commands.first().result.standardError.contains("provider warning"),
               QStringLiteral("stdout, stderr, exit status, and structured command are retained"));
    test.check(!manager.reviewedPlan().has_value(),
               QStringLiteral("a completed preview cannot be replayed"));
}

void testDestructiveConfirmation(TestRun &test, const QString &root)
{
    const auto state = std::make_shared<FakeRunnerState>();
    state->responses.push_back(successResult());
    state->responses.push_back(successResult());
    state->responses.push_back(successResult());
    const auto adapter = std::make_shared<FakeProviderAdapter>();
    adapter->inventory = {fakeMachine(QDir(root).filePath(QStringLiteral("vm/three.vbox")))};
    VmLabManager manager(QDir(root).filePath(QStringLiteral("catalog.json")), root,
                         adapter, std::make_shared<FakeRunnerFactory>(state));
    prepareSelectedManager(manager, *adapter, test);
    const std::optional<OperationPreview> preview = manager.reviewDeleteSnapshot(
        Snapshot{QStringLiteral("snapshot-id"), QStringLiteral("Before update"),
                 {}, {}, false, {}});
    test.check(preview && preview->risk == Risk::Destructive
                   && !preview->confirmation.isEmpty(),
               QStringLiteral("destructive snapshot operation has an exact token"));
    test.check(!manager.executeReviewed(preview->id, QStringLiteral("DELETE wrong")),
               QStringLiteral("incorrect destructive token is refused before process launch"));
    test.check(manager.executeReviewed(preview->id, preview->confirmation),
               QStringLiteral("exact destructive token authorizes the reviewed plan"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("destructive fake operation completes"));
}

void testSnapshotsAndCancellation(TestRun &test, const QString &root)
{
    const auto state = std::make_shared<FakeRunnerState>();
    state->responses.push_back(successResult());
    state->responses.push_back(successResult());
    state->responses.push_back(successResult("snapshot-one\nsnapshot-two\n"));
    const auto adapter = std::make_shared<FakeProviderAdapter>();
    adapter->inventory = {fakeMachine(QDir(root).filePath(QStringLiteral("vm/four.vbox")))};
    VmLabManager manager(QDir(root).filePath(QStringLiteral("catalog.json")), root,
                         adapter, std::make_shared<FakeRunnerFactory>(state));
    prepareSelectedManager(manager, *adapter, test);
    test.check(manager.refreshSnapshots(), QStringLiteral("snapshot refresh starts"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("snapshot refresh completes"));
    test.check(manager.snapshots().size() == 2,
               QStringLiteral("provider snapshot output populates manager inventory"));

    state->block.store(true);
    test.check(manager.detectProviders(), QStringLiteral("blocking provider detection starts"));
    test.check(waitUntil([state] {
        const std::lock_guard lock(state->mutex);
        return !state->commands.isEmpty();
    }), QStringLiteral("blocking fake command entered runner"));
    test.check(manager.cancel() && manager.state() == ManagerState::Cancelling,
               QStringLiteral("active provider task accepts cancellation"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("cancelled task exits without blocking UI thread"));
    const OperationEvidence cancelled = manager.evidenceHistory().constLast();
    test.check(cancelled.cancelled && !cancelled.success,
               QStringLiteral("cancellation is explicit in operation evidence"));
    state->block.store(false);
}

void testCatalogMutation(TestRun &test, const QString &root)
{
    const QString catalogPath = QDir(root).filePath(QStringLiteral("created-catalog.json"));
    const QString isoPath = QDir(root).filePath(QStringLiteral("install.iso"));
    test.check(writeFile(isoPath, "fake iso"), QStringLiteral("test ISO is created"));
    const auto state = std::make_shared<FakeRunnerState>();
    state->responses.push_back(successResult());
    state->responses.push_back(successResult());
    const auto adapter = std::make_shared<FakeProviderAdapter>();
    VmLabManager manager(catalogPath, root, adapter,
                         std::make_shared<FakeRunnerFactory>(state));
    manager.setAutoRefresh(false);
    test.check(manager.load(), QStringLiteral("catalog mutation manager loads"));
    test.check(manager.detectProviders(), QStringLiteral("catalog mutation detects provider"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("catalog mutation detection completes"));
    CreateSpec spec;
    spec.providerId = virtualBoxProviderId();
    spec.name = QStringLiteral("Created VM");
    spec.directory = root;
    spec.guestType = QStringLiteral("Windows11_64");
    spec.isoPath = isoPath;
    const std::optional<OperationPreview> preview = manager.reviewCreate(spec);
    test.check(preview.has_value(), QStringLiteral("create-from-ISO settings are reviewable"));
    test.check(manager.executeReviewed(preview->id), QStringLiteral("reviewed create executes"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("reviewed create and catalog save complete"));
    Catalog catalog(catalogPath);
    QString error;
    test.check(catalog.load(&error) && catalog.machines().size() == 1
                   && catalog.machines().first().ownership == Ownership::Managed,
               QStringLiteral("successful create is persisted as a managed catalog VM"));
}

void testManagedDeleteRefreshesAfterUnregister(TestRun &test, const QString &root)
{
    const QString managedRoot = QDir(root).filePath(QStringLiteral("managed"));
    const QString directory = QDir(managedRoot).filePath(QStringLiteral("delete-me"));
    const QString config = QDir(directory).filePath(QStringLiteral("delete-me.vbox"));
    test.check(writeFile(config, QByteArray("fixture")),
               QStringLiteral("guarded manager delete fixture is created"));
    Machine machine = fakeMachine(config);
    machine.ownershipToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    test.check(writeFile(
                   QDir(directory).filePath(managedOwnershipMarkerFileName()),
                   managedOwnershipMarkerContents(machine.ref, machine.ownershipToken)),
               QStringLiteral("guarded manager delete marker is created"));

    const auto state = std::make_shared<FakeRunnerState>();
    for (int index = 0; index < 5; ++index)
        state->responses.push_back(successResult());
    const auto adapter = std::make_shared<FakeProviderAdapter>();
    adapter->inventory = {machine};
    adapter->inventorySequence = {{machine}, {machine}, {}};
    adapter->guardedDeletion = true;
    adapter->guardedRoot = managedRoot;
    VmLabManager manager(QDir(root).filePath(QStringLiteral("catalog.json")),
                         managedRoot, adapter,
                         std::make_shared<FakeRunnerFactory>(state));
    prepareSelectedManager(manager, *adapter, test);
    const std::optional<OperationPreview> preview = manager.reviewDelete();
    test.check(preview && preview->risk == Risk::Destructive,
               QStringLiteral("guarded manager delete is reviewable"));
    test.check(preview && manager.executeReviewed(preview->id, preview->confirmation),
               QStringLiteral("guarded manager delete starts after exact confirmation"));
    test.check(waitUntil([&manager] { return !manager.busy(); }),
               QStringLiteral("guarded manager delete completes"));
    test.check(adapter->refreshCalls == 3 && manager.lastError().isEmpty()
                   && !QFileInfo::exists(directory),
               QStringLiteral(
                   "manager performs pre-delete and post-unregister full inventories before local removal"));
}

void testTypedReviewSurface(TestRun &test, const QString &root)
{
    const QString iso = QDir(root).filePath(QStringLiteral("review.iso"));
    const QString disk = QDir(root).filePath(QStringLiteral("review.vdi"));
    const QString registration = QDir(root).filePath(QStringLiteral("review.vbox"));
    test.check(writeFile(iso, "iso") && writeFile(disk, "disk")
                   && writeFile(registration, "registration"),
               QStringLiteral("typed review fixtures are created"));
    const auto state = std::make_shared<FakeRunnerState>();
    state->responses.push_back(successResult());
    state->responses.push_back(successResult());
    const auto adapter = std::make_shared<FakeProviderAdapter>();
    adapter->inventory = {fakeMachine(QDir(root).filePath(QStringLiteral("selected.vbox")))};
    VmLabManager manager(QDir(root).filePath(QStringLiteral("catalog.json")), root,
                         adapter, std::make_shared<FakeRunnerFactory>(state));
    prepareSelectedManager(manager, *adapter, test);
    const auto expect = [&test, &manager](const std::optional<OperationPreview> &preview,
                                         const QString &action) {
        test.check(preview && preview->action == action,
                   QStringLiteral("typed review builds '%1' preview").arg(action));
        manager.clearReviewedPlan();
    };
    expect(manager.reviewOpenConsole(), QStringLiteral("open-console"));
    expect(manager.reviewStart(false), QStringLiteral("start"));
    expect(manager.reviewGracefulShutdown(), QStringLiteral("graceful-shutdown"));
    expect(manager.reviewPowerOff(), QStringLiteral("power-off"));
    expect(manager.reviewPause(), QStringLiteral("pause"));
    expect(manager.reviewResume(), QStringLiteral("resume"));
    expect(manager.reviewReset(), QStringLiteral("reset"));
    expect(manager.reviewSaveState(), QStringLiteral("save-state"));
    ConfigPatch patch;
    patch.cpuCount = 4;
    patch.memoryMiB = 8192;
    patch.networkMode = NetworkMode::Nat;
    expect(manager.reviewConfigure(patch), QStringLiteral("configure"));
    expect(manager.reviewAttachIso(iso), QStringLiteral("attach-iso"));
    expect(manager.reviewDetachIso(), QStringLiteral("detach-iso"));
    StorageDeviceSpec storage;
    storage.path = disk;
    storage.port = 3;
    expect(manager.reviewAttachStorage(storage), QStringLiteral("attach-storage"));
    expect(manager.reviewDetachStorage(storage), QStringLiteral("detach-storage"));
    NetworkAdapterSpec network;
    network.slot = 2;
    network.mode = NetworkMode::Bridged;
    expect(manager.reviewAttachNetwork(network), QStringLiteral("attach-network"));
    expect(manager.reviewDetachNetwork(2), QStringLiteral("detach-network"));
    expect(manager.reviewTakeSnapshot(QStringLiteral("Before changes"),
                                      QStringLiteral("reviewed description")),
           QStringLiteral("take-snapshot"));
    const Snapshot snapshot{QStringLiteral("snapshot-id"), QStringLiteral("Before changes"),
                            {}, {}, false, {}};
    expect(manager.reviewRestoreSnapshot(snapshot), QStringLiteral("restore-snapshot"));
    expect(manager.reviewDeleteSnapshot(snapshot), QStringLiteral("delete-snapshot"));
    expect(manager.reviewUnregister(), QStringLiteral("unregister"));
    expect(manager.reviewDelete(), QStringLiteral("delete"));
    expect(manager.reviewRegister(virtualBoxProviderId(), registration,
                                  QStringLiteral("Registered VM")),
           QStringLiteral("register"));
    CreateSpec create;
    create.providerId = virtualBoxProviderId();
    create.name = QStringLiteral("Reviewed Create");
    create.directory = root;
    create.guestType = QStringLiteral("Windows11_64");
    create.isoPath = iso;
    create.firmware = Firmware::Efi;
    create.tpm = true;
    create.cpuCount = 8;
    create.memoryMiB = 16384;
    create.diskMiB = 131072;
    create.networkMode = NetworkMode::Bridged;
    create.bridgedInterface = QStringLiteral("Ethernet");
    create.unattendedBoot = true;
    expect(manager.reviewCreate(create), QStringLiteral("create"));
    test.check(adapter->lastRequest.createSpec.cpuCount == 8
                   && adapter->lastRequest.createSpec.memoryMiB == 16384
                   && adapter->lastRequest.createSpec.diskMiB == 131072
                   && adapter->lastRequest.createSpec.tpm
                   && adapter->lastRequest.createSpec.unattendedBoot,
               QStringLiteral("every reviewed create setting reaches the provider adapter"));
}

void testNativeTopologyAndGates(TestRun &test, const QString &root)
{
    NativeVmLabProviderAdapter adapter;
    ProviderInfo vbox = fakeProvider();
    const QString config = QDir(root).filePath(QStringLiteral("native/machine.vbox"));
    const QString disk = QDir(root).filePath(QStringLiteral("native/data.vdi"));
    test.check(writeFile(config, "<VirtualBox/>") && writeFile(disk, "disk"),
               QStringLiteral("native VirtualBox fixture files are created"));
    Machine machine = fakeMachine(config);
    OperationRequest request;
    request.action = ManagerAction::AttachStorage;
    request.machine = machine;
    request.storage.path = disk;
    request.storage.bus = StorageBus::Sata;
    request.storage.port = 2;
    request.revision = QStringLiteral("catalog-revision");
    request.now = QDateTime::currentDateTimeUtc();
    Plan plan = adapter.plan(vbox, request);
    test.check(plan.ok() && plan.preview.commands.size() == 1
                   && plan.preview.commands.first().executable == vbox.executable
                   && plan.preview.commands.first().arguments.contains(disk),
               QStringLiteral("VirtualBox storage uses structured provider arguments"));
    machine.powerState = PowerState::Running;
    request.machine = machine;
    test.check(!adapter.plan(vbox, request).ok(),
               QStringLiteral("running VM storage edits are state-gated"));
    machine.powerState = PowerState::PoweredOff;
    request.machine = machine;
    vbox.capabilities.remove(capability::media());
    test.check(!adapter.plan(vbox, request).ok(),
               QStringLiteral("missing detected media capability blocks storage edits"));

    ProviderInfo vmware;
    vmware.id = vmwareWorkstationProviderId();
    vmware.displayName = QStringLiteral("Fake VMware");
    vmware.executable = fakeExecutable(QStringLiteral("vmrun.exe"));
    vmware.available = true;
    vmware.capabilities = {capability::inventory(), capability::configure(), capability::media()};
    const QString vmx = QDir(root).filePath(QStringLiteral("native/vmware.vmx"));
    test.check(writeFile(vmx, QByteArray(
        ".encoding = \"UTF-8\"\r\n"
        "displayName = \"VMware Test\"\r\n"
        "sata0.present = \"TRUE\"\r\n")),
               QStringLiteral("native VMware fixture is created"));
    Machine vmwareMachine;
    vmwareMachine.ref = VmRef{vmware.id, vmx, QStringLiteral("VMware Test")};
    vmwareMachine.configPath = vmx;
    vmwareMachine.powerState = PowerState::PoweredOff;
    vmwareMachine.inventoryComplete = true;
    vmwareMachine.hardwareInventoryComplete = true;
    vmwareMachine.stateRevision = QStringLiteral("running-inventory-revision");
    request.machine = vmwareMachine;
    request.storage.path = disk;
    request.storage.port = 1;
    plan = adapter.plan(vmware, request);
    test.check(plan.ok() && plan.atomicWritesAfterCommands.size() == 1
                   && plan.preview.commands.isEmpty()
                   && plan.preflight.size() == 1,
               QStringLiteral("VMware storage is an atomic VMX edit guarded by live inventory evidence"));

    request.action = ManagerAction::AttachNetwork;
    request.network.slot = 2;
    request.network.mode = NetworkMode::Bridged;
    plan = adapter.plan(vmware, request);
    test.check(plan.ok() && plan.atomicWritesAfterCommands.size() == 1,
               QStringLiteral("VMware powered-off network topology is atomically reviewable"));
    request.network.mode = NetworkMode::Internal;
    test.check(!adapter.plan(vmware, request).ok(),
               QStringLiteral("unsupported provider-neutral VMware network fiction is refused"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        qCritical() << "Could not create test directory.";
        return 1;
    }
    TestRun test;
    testDetectionInventoryAndSelection(
        test, QDir(temporary.path()).filePath(QStringLiteral("selection")));
    testReviewedExecutionAndEvidence(
        test, QDir(temporary.path()).filePath(QStringLiteral("execution")));
    testDestructiveConfirmation(
        test, QDir(temporary.path()).filePath(QStringLiteral("destructive")));
    testSnapshotsAndCancellation(
        test, QDir(temporary.path()).filePath(QStringLiteral("snapshots")));
    testCatalogMutation(
        test, QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    testManagedDeleteRefreshesAfterUnregister(
        test, QDir(temporary.path()).filePath(QStringLiteral("guarded-delete")));
    testTypedReviewSurface(
        test, QDir(temporary.path()).filePath(QStringLiteral("review-surface")));
    testNativeTopologyAndGates(
        test, QDir(temporary.path()).filePath(QStringLiteral("native")));
    if (test.failures == 0)
        qInfo() << "All VM Lab manager tests passed.";
    return test.failures == 0 ? 0 : 1;
}
