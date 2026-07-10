#include "core/VmLab.h"
#include "core/VmLabProviders.h"
#include "core/VmLabVmx.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <optional>

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
            QTextStream(stdout) << "vm_lab_tests: all checks passed\n";
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

ProviderInfo availableVirtualBox(const QString &root, FakeRunner *runner = nullptr)
{
    const QString manage = makeFile(QDir(root).filePath(QStringLiteral("VBoxManage.exe")));
    const QString console = makeFile(QDir(root).filePath(QStringLiteral("VirtualBox.exe")));
    FakeRunner local;
    FakeRunner &probe = runner ? *runner : local;
    probe.responses.append(successResult("7.1.8r168469\r\n"));
    return VirtualBoxProvider::detect(manage, console, probe);
}

ProviderInfo availableVmware(const QString &root, const QString &providerId,
                             bool withDiskManager = true, FakeRunner *runner = nullptr)
{
    const QString vmrun = makeFile(QDir(root).filePath(QStringLiteral("vmrun.exe")));
    const QString consoleName = providerId == vmwarePlayerProviderId()
        ? QStringLiteral("vmplayer.exe") : QStringLiteral("vmware.exe");
    const QString console = makeFile(QDir(root).filePath(consoleName));
    const QString disk = withDiskManager
        ? makeFile(QDir(root).filePath(QStringLiteral("vmware-vdiskmanager.exe"))) : QString{};
    FakeRunner local;
    FakeRunner &probe = runner ? *runner : local;
    probe.responses.append(successResult("Total running VMs: 0\r\n"));
    ProcessResult banner;
    banner.started = true;
    banner.exitCode = 1;
    banner.standardError = QByteArray(
        "VMware vmrun version 17.5.2 build-23775571\n"
        "start stop reset suspend pause unpause listSnapshots snapshot "
        "deleteSnapshot revertToSnapshot");
    probe.responses.append(banner);
    return VmwareProvider::detect(providerId, vmrun, console, disk, probe);
}

Machine machineFor(const ProviderInfo &provider, const QString &id, const QString &name,
                   const QString &configPath, PowerState state = PowerState::PoweredOff,
                   Ownership ownership = Ownership::External)
{
    Machine machine;
    machine.ref = VmRef{provider.id, id, name};
    machine.configPath = configPath;
    machine.powerState = state;
    machine.ownership = ownership;
    machine.inventoryComplete = true;
    if (provider.id == vmwareWorkstationProviderId()
        || provider.id == vmwarePlayerProviderId()) {
        machine.stateRevision = commandEvidence(
            EvidenceFormat::VmwareRunningPathsSha256,
            QByteArray("Total running VMs: 0\n"));
    } else {
        machine.stateRevision = commandEvidence(
            EvidenceFormat::RawSha256, QByteArray("fixture-state"));
    }
    return machine;
}

void testTypesAndCommandSafety(TestRun &test, const QString &root)
{
    test.check(virtualBoxProviderId() == QStringLiteral("virtualbox")
                   && vmwareWorkstationProviderId() == QStringLiteral("vmware-workstation")
                   && vmwarePlayerProviderId() == QStringLiteral("vmware-player"),
               QStringLiteral("provider IDs are exact and stable"));
    test.check(powerStateName(PowerState::Inaccessible) == QStringLiteral("inaccessible")
                   && powerStateName(PowerState::Suspended) == QStringLiteral("suspended")
                   && ownershipName(Ownership::Managed) == QStringLiteral("managed")
                   && riskName(Risk::Destructive) == QStringLiteral("destructive"),
               QStringLiteral("typed state, ownership, and risk values serialize deterministically"));
    test.check(isSafeMachineFileStem(QStringLiteral("Windows 11 - QA"))
                   && !isSafeMachineFileStem(QStringLiteral("..\\escape"))
                   && !isSafeMachineFileStem(QStringLiteral("CON"))
                   && !isSafeMachineFileStem(QStringLiteral("trailing.")),
               QStringLiteral("provider-created VM filenames reject traversal and Windows device names"));

    QString error;
    Command relative{QStringLiteral("VBoxManage.exe"), {}, {}, 1000};
    test.check(!relative.valid(&error) && error.contains(QStringLiteral("absolute")),
               QStringLiteral("relative provider executable is rejected"));
    const QString shell = makeFile(QDir(root).filePath(QStringLiteral("cmd.exe")));
    Command shellCommand{shell, {QStringLiteral("/c"), QStringLiteral("echo unsafe")}, {}, 1000};
    test.check(!shellCommand.valid(&error) && error.contains(QStringLiteral("shell")),
               QStringLiteral("shell executable is rejected even when absolute"));
    const QString provider = makeFile(QDir(root).filePath(QStringLiteral("provider.exe")));
    const QString hostile = QDir(root).filePath(QStringLiteral("VMs/one & $(touch nope).vmx"));
    Command structured{provider, {QStringLiteral("start"), hostile}, root, 1000};
    test.check(structured.valid(&error) && structured.arguments.size() == 2
                   && structured.arguments.at(1) == hostile,
               QStringLiteral("hostile-looking path remains one structured argument"));
    ProcessCommandRunner processRunner;
    const QString childArgument = QStringLiteral("argument & $(still-one-token)");
    const ProcessResult child = processRunner.run(Command{
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--runner-child"), childArgument}, {}, 10000});
    test.check(child.ok() && QString::fromUtf8(child.standardOutput).trimmed() == childArgument,
               QStringLiteral("real command runner forwards QStringList arguments without a shell"));

    FakeRunner failingRunner;
    ProcessResult providerFailure;
    providerFailure.started = true;
    providerFailure.exitCode = 7;
    providerFailure.standardError = QByteArray(
        "provider says the VM is locked by another process");
    failingRunner.responses.append(providerFailure);
    const QDateTime now = QDateTime::fromString(
        QStringLiteral("2026-07-10T12:00:00Z"), Qt::ISODate);
    Plan failedPlan;
    failedPlan.preview = makePreview(
        QStringLiteral("inspect"),
        VmRef{vmwareWorkstationProviderId(),
              QDir(root).filePath(QStringLiteral("failure.vmx")),
              QStringLiteral("Failure fixture")},
        Risk::ReadOnly, {QStringLiteral("Inspect provider failure output.")}, {},
        {Command{provider, {QStringLiteral("inspect")}, {}, 1000}},
        QStringLiteral("failure-rev"), now);
    const Result failed = Executor::execute(
        failedPlan, QStringLiteral("failure-rev"), {}, now, failingRunner);
    test.check(!failed.success && failed.error.contains(QStringLiteral("VM is locked"))
                   && failed.processes.size() == 1,
               QStringLiteral("provider stderr remains actionable in the primary result"));
}

void testDetectionAndCapabilityDegrade(TestRun &test, const QString &root)
{
    FakeRunner vboxRunner;
    const ProviderInfo vbox = availableVirtualBox(QDir(root).filePath(QStringLiteral("vbox")),
                                                  &vboxRunner);
    test.check(vbox.available && vbox.version.startsWith(QStringLiteral("7.1.8"))
                   && vbox.supports(capability::create())
                   && vbox.supports(capability::registerMachine())
                   && vbox.supports(capability::tpm())
                   && !vbox.supports(capability::secureBoot())
                   && vboxRunner.commands.size() == 1
                   && vboxRunner.commands.first().arguments
                          == QStringList{QStringLiteral("--version")},
               QStringLiteral("VirtualBox detection uses exact version evidence and degrades risky capabilities"));

    FakeRunner workstationRunner;
    const ProviderInfo workstation = availableVmware(
        QDir(root).filePath(QStringLiteral("workstation")), vmwareWorkstationProviderId(),
        true, &workstationRunner);
    test.check(workstation.available && workstation.version == QStringLiteral("17.5.2")
                   && workstation.supports(capability::create())
                   && workstation.supports(capability::openConsole())
                   && workstation.supports(capability::secureBoot())
                   && !workstation.supports(capability::registerMachine())
                   && !workstation.supports(capability::tpm())
                   && workstationRunner.commands.first().arguments
                          == QStringList{QStringLiteral("-T"), QStringLiteral("ws"),
                                         QStringLiteral("list")},
               QStringLiteral("Workstation detection reports evidence without inventing register or TPM"));

    FakeRunner playerRunner;
    const ProviderInfo player = availableVmware(
        QDir(root).filePath(QStringLiteral("player")), vmwarePlayerProviderId(),
        false, &playerRunner);
    test.check(player.available && !player.supports(capability::create())
                   && player.supports(capability::lifecycle())
                   && player.warnings.join(QLatin1Char(' ')).contains(QStringLiteral("vdiskmanager"))
                   && playerRunner.commands.first().arguments.value(1) == QStringLiteral("player"),
               QStringLiteral("Player remains manageable when disk creation capability is absent"));

    const QString dispatchRoot = QDir(root).filePath(QStringLiteral("dispatch"));
    const ProviderProbePaths vboxCandidate{
        virtualBoxProviderId(), makeFile(QDir(dispatchRoot).filePath(QStringLiteral("VBoxManage.exe"))),
        makeFile(QDir(dispatchRoot).filePath(QStringLiteral("VirtualBox.exe"))), {}};
    const ProviderProbePaths vmwareCandidate{
        vmwareWorkstationProviderId(), makeFile(QDir(dispatchRoot).filePath(QStringLiteral("vmrun.exe"))),
        makeFile(QDir(dispatchRoot).filePath(QStringLiteral("vmware.exe"))),
        makeFile(QDir(dispatchRoot).filePath(QStringLiteral("vmware-vdiskmanager.exe")))};
    FakeRunner dispatchRunner;
    dispatchRunner.responses = {
        successResult("7.0.0\n"),
        successResult("Total running VMs: 0\n"),
        ProcessResult{true, false, 1, {}, QByteArray("vmrun version 17.0.0"), {}},
    };
    const QList<ProviderInfo> detected = ProviderDetector::detect(
        {vboxCandidate, vmwareCandidate}, dispatchRunner);
    test.check(detected.size() == 2 && detected.at(0).id == virtualBoxProviderId()
                   && detected.at(1).id == vmwareWorkstationProviderId()
                   && dispatchRunner.commands.size() == 3,
               QStringLiteral("provider detector dispatches injected absolute candidates without shell discovery"));
    const QList<ProviderProbePaths> defaults = ProviderDetector::defaultWindowsCandidates();
    test.check(std::all_of(defaults.cbegin(), defaults.cend(), [](const ProviderProbePaths &candidate) {
        return isKnownProviderId(candidate.providerId) && QFileInfo(candidate.executable).isAbsolute();
    }), QStringLiteral("default provider candidates use known IDs and absolute executable paths"));
}

void testProviderParsers(TestRun &test, const QString &root)
{
    QString error;
    const QByteArray vboxList(
        "\"Normal VM\" {11111111-1111-1111-1111-111111111111}\r\n"
        "\"Hostile \\\"name\\\" & $(x)\" {22222222-2222-2222-2222-222222222222}\r\n");
    const QList<VmRef> refs = VirtualBoxProvider::parseMachineList(vboxList, &error);
    test.check(error.isEmpty() && refs.size() == 2
                   && refs.at(1).name == QStringLiteral("Hostile \"name\" & $(x)")
                   && refs.at(1).id == QStringLiteral("22222222-2222-2222-2222-222222222222"),
               QStringLiteral("VirtualBox inventory parser preserves escaped hostile names"));

    const QString cfg = QDir(root).filePath(QStringLiteral("VMs/test.vbox"));
    const QString storage = QDir(root).filePath(QStringLiteral("VMs/test.vdi"));
    const QByteArray info = QStringLiteral(
        "name=\"Parser VM\"\r\nUUID=\"33333333-3333-3333-3333-333333333333\"\r\n"
        "CfgFile=\"%1\"\r\nSATA-0-0=\"%2\"\r\nVMState=\"poweroff\"\r\n"
        "VMState=\"paused\"\r\naccessible=\"true\"\r\n")
                                .arg(cfg, storage).toUtf8();
    const std::optional<Machine> parsed = VirtualBoxProvider::parseMachineInfo(info, &error);
    test.check(parsed && parsed->powerState == PowerState::Paused && parsed->configPath == cfg
                   && parsed->storagePaths == QStringList{storage},
               QStringLiteral("machine-readable duplicate keys use last value and expose storage containment evidence"));

    const QByteArray inaccessible(
        "name=\"Broken\"\nUUID=\"44444444-4444-4444-4444-444444444444\"\n"
        "VMState=\"inaccessible\"\naccessible=\"false\"\naccessError=\"missing disk\"\n");
    const std::optional<Machine> broken = VirtualBoxProvider::parseMachineInfo(inaccessible, &error);
    test.check(broken && broken->powerState == PowerState::Inaccessible
                   && broken->inaccessibleReason == QStringLiteral("missing disk"),
               QStringLiteral("VirtualBox inaccessible evidence is retained"));

    const QByteArray snapshots(
        "SnapshotName=\"base\"\nSnapshotUUID=\"55555555-5555-5555-5555-555555555555\"\n"
        "SnapshotName-1=\"patched\"\nSnapshotUUID-1=\"66666666-6666-6666-6666-666666666666\"\n"
        "CurrentSnapshotUUID=\"66666666-6666-6666-6666-666666666666\"\n");
    const QList<Snapshot> vboxSnapshots = VirtualBoxProvider::parseSnapshotList(snapshots, &error);
    test.check(vboxSnapshots.size() == 2 && vboxSnapshots.at(1).current,
               QStringLiteral("VirtualBox snapshot parser identifies current snapshot"));

    const QString runningOne = QDir(root).filePath(QStringLiteral("VMs/one & two.vmx"));
    const QString runningTwo = QDir(root).filePath(QStringLiteral("VMs/second.vmx"));
    const QByteArray vmrunList = QStringLiteral("Total running VMs: 2\r\n%1\r\n%2\r\n")
                                       .arg(runningOne, runningTwo).toLocal8Bit();
    const QStringList running = VmwareProvider::parseRunningVmList(vmrunList, &error);
    test.check(running == QStringList{QFileInfo(runningOne).absoluteFilePath(),
                                      QFileInfo(runningTwo).absoluteFilePath()},
               QStringLiteral("vmrun inventory parser preserves each VMX as one absolute path"));
    const QList<Snapshot> vmwareSnapshots = VmwareProvider::parseSnapshotList(
        QByteArray("Total snapshots: 2\nBefore updates\nName & symbols\n"), &error);
    test.check(vmwareSnapshots.size() == 2
                   && vmwareSnapshots.at(1).name == QStringLiteral("Name & symbols"),
               QStringLiteral("vmrun snapshot parser preserves provider names"));
    test.check(VmwareProvider::parseRunningVmList(
                   QStringLiteral("Total running VMs: 2\n%1\n").arg(runningOne).toLocal8Bit(),
                   &error).isEmpty()
                   && error.contains(QStringLiteral("count")),
               QStringLiteral("vmrun parser rejects inconsistent provider count evidence"));

    ProviderInfo inspectorInfo;
    inspectorInfo.id = vmwareWorkstationProviderId();
    inspectorInfo.available = true;
    inspectorInfo.executable = makeFile(QDir(root).filePath(QStringLiteral("vmrun.exe")));
    const VmwareProvider inspector(inspectorInfo);
    const QString vmx = makeFile(QDir(root).filePath(QStringLiteral("VMs/inspect.vmx")),
                                 QByteArray("displayName = \"Inspected VM\"\n"));
    Machine inspected = inspector.inspectMachine(vmx, {vmx}, Ownership::External, &error);
    test.check(error.isEmpty() && inspected.ref.name == QStringLiteral("Inspected VM")
                   && inspected.powerState == PowerState::Running,
               QStringLiteral("VMware catalog VMX and live vmrun path reconcile to running state"));
    makeFile(QDir(QFileInfo(vmx).absolutePath()).filePath(QStringLiteral("inspect.vmss")));
    inspected = inspector.inspectMachine(vmx, {}, Ownership::External, &error);
    test.check(inspected.powerState == PowerState::Suspended,
               QStringLiteral("VMware VMX inventory recognizes suspended state evidence"));
    inspected = inspector.inspectMachine(QDir(root).filePath(QStringLiteral("missing.vmx")), {},
                                         Ownership::External, &error);
    test.check(inspected.powerState == PowerState::Inaccessible
                   && !inspected.inaccessibleReason.isEmpty(),
               QStringLiteral("missing external VMX remains visible as inaccessible"));
}

void testVirtualBoxPlansAndDeletion(TestRun &test, const QString &root)
{
    const ProviderInfo info = availableVirtualBox(QDir(root).filePath(QStringLiteral("provider")));
    const VirtualBoxProvider provider(info);
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-07-10T12:00:00Z"), Qt::ISODate);
    const QString managedRoot = QDir(root).filePath(QStringLiteral("managed"));
    QDir().mkpath(managedRoot);
    const QString iso = makeFile(QDir(root).filePath(QStringLiteral("media/Windows & test.iso")));
    CreateSpec spec;
    spec.providerId = virtualBoxProviderId();
    spec.id = QStringLiteral("77777777-7777-7777-7777-777777777777");
    spec.name = QStringLiteral("VM & $(hostile)");
    spec.directory = managedRoot;
    spec.guestType = QStringLiteral("Windows11_64");
    spec.cpuCount = 4;
    spec.memoryMiB = 8192;
    spec.isoPath = iso;
    spec.unattendedBoot = true;
    const Plan create = provider.create(spec, QStringLiteral("rev-1"), now);
    bool isoIsOneArgument = false;
    bool uuidTargeted = true;
    for (const Command &command : create.preview.commands) {
        QString error;
        test.check(command.valid(&error), QStringLiteral("VirtualBox plan emits valid structured command: %1").arg(error));
        if (command.arguments.contains(iso))
            isoIsOneArgument = command.arguments.count(iso) == 1;
        if (command.arguments.first() != QStringLiteral("createvm")
            && command.arguments.first() != QStringLiteral("createmedium"))
            uuidTargeted = uuidTargeted && command.arguments.contains(spec.id);
    }
    test.check(create.ok() && create.preview.commands.size() == 6
                   && isoIsOneArgument && uuidTargeted,
               QStringLiteral("VirtualBox create uses UUID targets and preserves hostile ISO as one argument"));
    CreateSpec unsupported = spec;
    unsupported.secureBoot = true;
    test.check(!provider.create(unsupported, QStringLiteral("rev"), now).ok(),
               QStringLiteral("VirtualBox Secure Boot degrades instead of being fabricated"));

    const QString vmDirectory = QDir(managedRoot).filePath(QStringLiteral("managed-vm"));
    const QString config = makeFile(QDir(vmDirectory).filePath(QStringLiteral("managed.vbox")));
    Machine managed = machineFor(info, spec.id, QStringLiteral("Managed VM"), config,
                                 PowerState::PoweredOff, Ownership::Managed);
    const Plan deletion = provider.deleteMachine(managed, managedRoot, {managed},
                                                  QStringLiteral("rev-2"), now);
    test.check(deletion.ok() && deletion.preview.risk == Risk::Destructive
                   && deletion.preview.confirmation == QStringLiteral("DELETE Managed VM")
                   && deletion.preview.commands.first().arguments
                          == QStringList{QStringLiteral("unregistervm"), spec.id}
                   && deletion.managedDeletionAfterCommands.has_value(),
               QStringLiteral("managed VirtualBox delete unregisters first, then revalidates guarded local deletion"));
    Machine external = managed;
    external.ownership = Ownership::External;
    test.check(!provider.deleteMachine(external, managedRoot, {external}, QStringLiteral("rev"), now).ok()
                   && provider.unregisterMachine(external, QStringLiteral("rev"), now)
                          .preview.commands.first().arguments
                          == QStringList{QStringLiteral("unregistervm"), spec.id},
               QStringLiteral("external VirtualBox VM may unregister but never receives --delete"));
    Machine shared = managed;
    shared.ref.id = QStringLiteral("88888888-8888-8888-8888-888888888888");
    shared.ref.name = QStringLiteral("Shared");
    shared.configPath = QDir(vmDirectory).filePath(QStringLiteral("other.vbox"));
    makeFile(shared.configPath);
    test.check(!provider.deleteMachine(managed, managedRoot, {managed, shared},
                                       QStringLiteral("rev"), now).ok(),
               QStringLiteral("shared managed directory cannot be deleted"));
    Machine borrower = machineFor(
        info, QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"), QStringLiteral("Borrower"),
        makeFile(QDir(root).filePath(QStringLiteral("borrower/borrower.vbox"))),
        PowerState::PoweredOff, Ownership::External);
    borrower.storagePaths = {makeFile(QDir(vmDirectory).filePath(QStringLiteral("borrowed.vdi")))};
    test.check(!provider.deleteMachine(managed, managedRoot, {managed, borrower},
                                       QStringLiteral("rev"), now).ok(),
               QStringLiteral("managed directory containing another catalog VM's storage cannot be deleted"));
    Machine externalStorage = managed;
    externalStorage.storagePaths = {
        makeFile(QDir(root).filePath(QStringLiteral("outside/shared-disk.vdi")))};
    test.check(!provider.deleteMachine(externalStorage, managedRoot, {externalStorage},
                                       QStringLiteral("rev"), now).ok(),
               QStringLiteral("VirtualBox --delete is refused when attached storage is outside VM directory"));
    test.check(!provider.pause(managed, QStringLiteral("rev"), now).ok(),
               QStringLiteral("provider planner rejects lifecycle action in incompatible power state"));
    Machine running = managed;
    running.powerState = PowerState::Running;
    test.check(provider.pause(running, QStringLiteral("rev"), now).ok()
                   && !provider.configure(running, ConfigPatch{}, QStringLiteral("rev"), now).ok(),
               QStringLiteral("provider planner gates actions by live state"));

    const QString registerPath = makeFile(
        QDir(root).filePath(QStringLiteral("external/registered.vbox")),
        QByteArray("<VirtualBox><Machine uuid=\"{99999999-9999-9999-9999-999999999999}\" name=\"Registered\"/></VirtualBox>"));
    const Plan registration = provider.registerMachine(registerPath, QStringLiteral("Registered"),
                                                        QStringLiteral("rev"), now);
    test.check(registration.ok()
                   && registration.preview.target.id
                          == QStringLiteral("99999999-9999-9999-9999-999999999999")
                   && registration.preview.commands.first().arguments.last() == registerPath,
               QStringLiteral("VirtualBox register parses and previews the provider UUID"));

    const Snapshot hostileSnapshot{QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"),
                                   QStringLiteral("snap & one"), {}, {}, false};
    test.check(provider.restoreSnapshot(managed, hostileSnapshot, QStringLiteral("rev"), now)
                       .preview.commands.first().arguments.last() == hostileSnapshot.id
                   && provider.takeSnapshot(managed, hostileSnapshot.name, QStringLiteral("desc & x"),
                                            QStringLiteral("rev"), now)
                          .preview.commands.first().arguments.contains(hostileSnapshot.name),
               QStringLiteral("VirtualBox snapshot targets remain structured arguments"));
}

void testVmxEditing(TestRun &test, const QString &root)
{
    QString error;
    const QByteArray source(
        "# retained comment\r\n"
        "unknown.vendor.key = \"keep me\"\r\n"
        "displayName = \"first\"\r\n"
        "displayName = \"second\"\r\n"
        "sata0:0.present = \"TRUE\"\r\n"
        "sata0:0.fileName = \"disk.vmdk\"\r\n"
        "sata0:2.present = \"TRUE\"\r\n"
        "sata0:2.deviceType = \"cdrom-image\"\r\n"
        "sata0:2.fileName = \"installer.iso\"\r\n"
        "sata0:1.fileName = \"old.iso\"\r\n");
    std::optional<VmxDocument> document = VmxDocument::parse(source, &error);
    test.check(document && document->value(QStringLiteral("displayName")) == QStringLiteral("second"),
               QStringLiteral("VMX duplicate keys use last-key-wins semantics"));
    if (!document)
        return;
    document->setValue(QStringLiteral("displayName"), QStringLiteral("updated \"name\""), &error);
    document->remove(QStringLiteral("sata0:1.fileName"));
    const QByteArray serialized = document->serialize();
    test.check(serialized.contains("# retained comment")
                   && serialized.contains("unknown.vendor.key = \"keep me\"")
                   && serialized.contains("displayName = \"first\"")
                   && serialized.contains("displayName = \"updated \\\"name\\\"\"")
                   && !serialized.contains("old.iso"),
               QStringLiteral("VMX editor preserves comments/unknown keys and updates only last duplicate"));
    test.check(document->storagePaths(root)
                       == QStringList{QFileInfo(QDir(root).filePath(QStringLiteral("disk.vmdk")))
                                          .absoluteFilePath()},
               QStringLiteral("VMX inventory resolves disk storage and excludes optical media"));
    test.check(!VmxDocument::parse(QByteArray("broken = unquoted\n"), &error)
                   && error.contains(QStringLiteral("Malformed")),
               QStringLiteral("strict VMX parser rejects malformed assignments"));
    const std::optional<VmxDocument> windowsPath = VmxDocument::parse(
        QByteArray("sata0:0.fileName = \"C:\\VMs\\Windows 11\\disk.vmdk\"\r\n"), &error);
    test.check(windowsPath
                   && windowsPath->value(QStringLiteral("sata0:0.fileName"))
                          == QStringLiteral("C:\\VMs\\Windows 11\\disk.vmdk"),
               QStringLiteral("VMX parser preserves literal Windows path separators"));

    CreateSpec spec;
    spec.providerId = vmwareWorkstationProviderId();
    spec.name = QStringLiteral("VMX fixture");
    spec.directory = QDir(root).filePath(QStringLiteral("vmx"));
    spec.guestType = QStringLiteral("windows11-64");
    spec.secureBoot = true;
    spec.virtualHardwareVersion = 20;
    spec.isoPath = QDir(root).filePath(QStringLiteral("media/one & two.iso"));
    const QString disk = QDir(spec.directory).filePath(QStringLiteral("disk & data.vmdk"));
    document = VmxDocument::fromCreateSpec(spec, disk, &error);
    test.check(document && document->value(QStringLiteral("uefi.secureBoot.enabled"))
                               == QStringLiteral("TRUE")
                   && document->value(QStringLiteral("sata0:1.fileName")) == spec.isoPath,
               QStringLiteral("strict generated VMX carries reviewed firmware and ISO values"));
    ConfigPatch patch;
    patch.cpuCount = 8;
    patch.isoPath = QString{};
    test.check(document && applyConfigPatch(*document, patch, &error)
                   && document->value(QStringLiteral("numvcpus")) == QStringLiteral("8")
                   && document->value(QStringLiteral("sata0:1.present")) == QStringLiteral("FALSE")
                   && !document->contains(QStringLiteral("sata0:1.fileName")),
               QStringLiteral("VMX patch updates config and detaches ISO without stale duplicate keys"));
    ConfigPatch tpm;
    tpm.tpm = true;
    test.check(document && !applyConfigPatch(*document, tpm, &error)
                   && error.contains(QStringLiteral("TPM")),
               QStringLiteral("VMX editor refuses unproven encrypted-VM TPM mutation"));
    const QString vmxPath = QDir(spec.directory).filePath(QStringLiteral("atomic.vmx"));
    test.check(document && document->saveAtomic(vmxPath, &error) && QFileInfo::exists(vmxPath),
               QStringLiteral("VMX saves atomically to an absolute path: %1").arg(error));
}

void testVmwarePlansAndExecutor(TestRun &test, const QString &root)
{
    const ProviderInfo info = availableVmware(QDir(root).filePath(QStringLiteral("provider")),
                                               vmwareWorkstationProviderId());
    const VmwareProvider provider(info);
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-07-10T12:00:00Z"), Qt::ISODate);
    const QString managedRoot = QDir(root).filePath(QStringLiteral("managed"));
    const QString vmDirectory = QDir(managedRoot).filePath(QStringLiteral("Test & VM"));
    QDir().mkpath(vmDirectory);
    const QString iso = makeFile(QDir(root).filePath(QStringLiteral("media/Win & Test.iso")));

    CreateSpec spec;
    spec.providerId = vmwareWorkstationProviderId();
    spec.name = QStringLiteral("Test & VM");
    spec.directory = vmDirectory;
    spec.guestType = QStringLiteral("windows11-64");
    spec.isoPath = iso;
    const Plan create = provider.create(spec, QStringLiteral("catalog-rev"), now);
    const QString diskPath = QDir(vmDirectory).filePath(spec.name + QStringLiteral(".vmdk"));
    const QString vmxPath = QDir(vmDirectory).filePath(spec.name + QStringLiteral(".vmx"));
    test.check(create.ok() && create.preview.commands.size() == 1
                   && create.preview.commands.first().executable == info.diskManagerExecutable
                   && create.preview.commands.first().arguments.last() == diskPath
                   && create.atomicWritesAfterCommands.size() == 1
                   && create.atomicWritesAfterCommands.first().path == vmxPath
                   && create.atomicWritesAfterCommands.first().contents.contains(iso.toUtf8()),
               QStringLiteral("VMware create plans one structured vdisk command plus atomic strict VMX"));
    test.check(!provider.registerMachine(vmxPath, spec.name, QStringLiteral("rev"), now).ok(),
               QStringLiteral("VMware registration is explicitly unsupported rather than fabricated"));

    FakeRunner createRunner;
    createRunner.responses.append(successResult());
    const Result created = Executor::execute(create, QStringLiteral("catalog-rev"), {}, now, createRunner);
    test.check(created.success && QFileInfo::exists(vmxPath)
                   && createRunner.commands.first().arguments.last() == diskPath,
               QStringLiteral("preview executor revalidates and executes structured VMware create"));

    Machine machine = machineFor(info, vmxPath, spec.name, vmxPath,
                                 PowerState::PoweredOff, Ownership::Managed);
    const Plan attached = provider.attachIso(machine, iso, QStringLiteral("rev-2"), now);
    test.check(attached.ok() && attached.preview.commands.isEmpty()
                   && attached.atomicWritesAfterCommands.size() == 1
                   && attached.atomicWritesAfterCommands.first().contents.contains("sata0:1.fileName"),
               QStringLiteral("VMware ISO attach uses atomic VMX sata0:1 edits, not a shell"));
    QFile mutate(vmxPath);
    mutate.open(QIODevice::WriteOnly | QIODevice::Append);
    mutate.write("# changed after preview\n");
    mutate.close();
    FakeRunner staleRunner;
    const Result stale = Executor::execute(attached, QStringLiteral("rev-2"), {}, now, staleRunner);
    test.check(!stale.success && stale.error.contains(QStringLiteral("changed after preview"))
                   && staleRunner.commands.isEmpty(),
               QStringLiteral("VMX file revision is revalidated before execution"));

    const Plan start = provider.start(machine, true, QStringLiteral("rev-3"), now);
    test.check(start.preview.commands.first().arguments
                   == QStringList{QStringLiteral("-T"), QStringLiteral("ws"),
                                  QStringLiteral("start"), vmxPath, QStringLiteral("nogui")},
               QStringLiteral("VMware lifecycle targets the absolute VMX with exact vmrun vector"));
    const Plan open = provider.openConsole(machine, QStringLiteral("rev"), now);
    test.check(open.ok() && open.preview.commands.first().arguments == QStringList{vmxPath},
               QStringLiteral("provider console receives VMX as one argument"));

    const Plan deletion = provider.deleteMachine(machine, managedRoot, {machine},
                                                  QStringLiteral("delete-rev"), now);
    test.check(deletion.ok() && deletion.preview.commands.isEmpty()
                   && deletion.managedDeletionAfterCommands.has_value(),
               QStringLiteral("VMware managed delete uses guarded local deletion, never vmrun register/delete fiction"));
    FakeRunner deleteRunner;
    Result denied = Executor::execute(deletion, QStringLiteral("delete-rev"),
                                      QStringLiteral("wrong"), now, deleteRunner);
    test.check(!denied.success && QFileInfo::exists(vmDirectory),
               QStringLiteral("destructive delete requires exact typed token"));
    denied = Executor::execute(deletion, QStringLiteral("stale-revision"),
                               deletion.preview.confirmation, now, deleteRunner);
    test.check(!denied.success && denied.error.contains(QStringLiteral("changed after preview"))
                   && QFileInfo::exists(vmDirectory),
               QStringLiteral("destructive delete revalidates catalog revision"));
    denied = Executor::execute(deletion, QStringLiteral("delete-rev"),
                               deletion.preview.confirmation, now.addSecs(301), deleteRunner);
    test.check(!denied.success && denied.error.contains(QStringLiteral("expired"))
                   && QFileInfo::exists(vmDirectory),
               QStringLiteral("destructive token expires after five minutes"));
    FakeRunner startedAfterPreview;
    startedAfterPreview.responses.append(successResult(
        QStringLiteral("Total running VMs: 1\n%1\n").arg(vmxPath).toLocal8Bit()));
    const Result startedDenied = Executor::execute(
        deletion, QStringLiteral("delete-rev"), deletion.preview.confirmation,
        now, startedAfterPreview);
    test.check(!startedDenied.success
                   && startedDenied.error.contains(QStringLiteral("state changed"))
                   && QFileInfo::exists(vmDirectory),
               QStringLiteral("VM started after preview is never deleted from stale state"));
    deleteRunner.responses.append(successResult("Total running VMs: 0\n"));
    const Result deleted = Executor::execute(deletion, QStringLiteral("delete-rev"),
                                             deletion.preview.confirmation, now, deleteRunner);
    test.check(deleted.success && deleted.processes.size() == 1
                   && !QFileInfo::exists(vmDirectory),
               QStringLiteral("fresh provider evidence permits confirmed managed VMware deletion"));

    Machine outside = machine;
    outside.configPath = makeFile(QDir(root).filePath(QStringLiteral("outside/outside.vmx")));
    test.check(!provider.deleteMachine(outside, managedRoot, {outside},
                                       QStringLiteral("rev"), now).ok(),
               QStringLiteral("managed flag cannot authorize deletion outside canonical root"));
    outside.ownership = Ownership::External;
    test.check(!provider.deleteMachine(outside, managedRoot, {outside},
                                       QStringLiteral("rev"), now).ok(),
               QStringLiteral("external VMware files are never deleted"));

    const QString swappedDirectory = QDir(managedRoot).filePath(QStringLiteral("swapped"));
    Machine swapped = machineFor(
        info, QDir(swappedDirectory).filePath(QStringLiteral("swapped.vmx")),
        QStringLiteral("Swapped VM"),
        makeFile(QDir(swappedDirectory).filePath(QStringLiteral("swapped.vmx"))),
        PowerState::PoweredOff, Ownership::Managed);
    const Plan swappedDeletion = provider.deleteMachine(
        swapped, managedRoot, {swapped}, QStringLiteral("swap-rev"), now);
    QDir(swappedDirectory).removeRecursively();
    const QString replacement = makeFile(
        QDir(swappedDirectory).filePath(QStringLiteral("replacement.txt")),
        QByteArray("must survive"));
    const Result swappedResult = Executor::execute(
        swappedDeletion, QStringLiteral("swap-rev"),
        swappedDeletion.preview.confirmation, now, deleteRunner);
    test.check(!swappedResult.success
                   && swappedResult.error.contains(QStringLiteral("identity changed"),
                                                   Qt::CaseInsensitive)
                   && QFileInfo::exists(replacement),
               QStringLiteral("same-path directory replacement after preview is never deleted"));
}

void testCatalogPersistence(TestRun &test, const QString &root)
{
    const QString catalogPath = QDir(root).filePath(QStringLiteral("state/vm-catalog.json"));
    Catalog catalog(catalogPath);
    Machine machine;
    machine.ref = VmRef{vmwareWorkstationProviderId(),
                        QDir(root).filePath(QStringLiteral("vm/external.vmx")),
                        QStringLiteral("Portable entry")};
    machine.configPath = machine.ref.id;
    machine.ownership = Ownership::External;
    machine.powerState = PowerState::Running;
    machine.inaccessibleReason = QStringLiteral("secret-runtime-evidence");
    QString error;
    test.check(catalog.upsert(machine, &error) && catalog.save(&error),
               QStringLiteral("catalog saves atomically: %1").arg(error));
    QFile file(catalogPath);
    const bool opened = file.open(QIODevice::ReadOnly);
    const QByteArray bytes = opened ? file.readAll() : QByteArray{};
    file.close();
    const QJsonObject object = QJsonDocument::fromJson(bytes).object();
    test.check(opened && object.value(QStringLiteral("schema")).toString()
                              == QStringLiteral("wimforge.vm-catalog")
                   && object.value(QStringLiteral("version")).toInt() == 1
                   && !bytes.contains("powerState") && !bytes.contains("running")
                   && !bytes.contains("secret-runtime-evidence"),
               QStringLiteral("catalog schema v1 persists no live state or secret/error evidence"));

    Catalog loaded(catalogPath);
    test.check(loaded.load(&error) && loaded.machines().size() == 1
                   && loaded.machines().first().powerState == PowerState::Unknown
                   && loaded.machines().first().inaccessibleReason.isEmpty(),
               QStringLiteral("catalog reload restores provider-neutral metadata with unknown live state"));
    const QString before = loaded.revision();
    Machine second = machine;
    second.ref.id = QDir(root).filePath(QStringLiteral("vm/second.vmx"));
    second.ref.name = QStringLiteral("Second");
    second.configPath = second.ref.id;
    test.check(loaded.upsert(second, &error) && loaded.revision() != before,
               QStringLiteral("catalog revision changes with reviewed metadata"));

    Catalog concurrentA(catalogPath);
    Catalog concurrentB(catalogPath);
    test.check(concurrentA.load(&error) && concurrentB.load(&error),
               QStringLiteral("two catalog writers can load the same baseline"));
    Machine third = machine;
    third.ref.id = QDir(root).filePath(QStringLiteral("vm/third.vmx"));
    third.ref.name = QStringLiteral("Third");
    third.configPath = third.ref.id;
    Machine fourth = machine;
    fourth.ref.id = QDir(root).filePath(QStringLiteral("vm/fourth.vmx"));
    fourth.ref.name = QStringLiteral("Fourth");
    fourth.configPath = fourth.ref.id;
    const bool aUpdated = concurrentA.upsert(third, &error);
    const bool aSaved = aUpdated && concurrentA.save(&error);
    const bool bUpdated = concurrentB.upsert(fourth, &error);
    const bool bRejected = bUpdated && !concurrentB.save(&error);
    const QString staleWriterError = error;
    test.check(aUpdated && aSaved && bUpdated && bRejected
                   && staleWriterError.contains(QStringLiteral("changed on disk")),
               QStringLiteral("catalog compare-and-swap refuses a stale concurrent writer: %1")
                   .arg(staleWriterError));

    makeFile(catalogPath, QByteArray("{ broken"));
    test.check(!loaded.load(&error) && loaded.machines().size() == 2,
               QStringLiteral("failed catalog parse does not replace in-memory catalog"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeVmLabTests"));
    if (application.arguments().value(1) == QStringLiteral("--runner-child")) {
        QTextStream(stdout) << application.arguments().value(2) << '\n';
        return 0;
    }

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    testTypesAndCommandSafety(test, QDir(temporary.path()).filePath(QStringLiteral("types")));
    testDetectionAndCapabilityDegrade(test, QDir(temporary.path()).filePath(QStringLiteral("detect")));
    testProviderParsers(test, QDir(temporary.path()).filePath(QStringLiteral("parse")));
    testVirtualBoxPlansAndDeletion(test, QDir(temporary.path()).filePath(QStringLiteral("vbox")));
    testVmxEditing(test, QDir(temporary.path()).filePath(QStringLiteral("vmx")));
    testVmwarePlansAndExecutor(test, QDir(temporary.path()).filePath(QStringLiteral("vmware")));
    testCatalogPersistence(test, QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    return test.result();
}
