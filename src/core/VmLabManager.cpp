#include "VmLabManager.h"

#include "ProcessLaunch.h"
#include "StructuredLogger.h"
#include "VmLabVmx.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace wimforge::vmlab {
namespace {

constexpr qsizetype MaxProviderStreamBytes = 4 * 1024 * 1024;

void appendBounded(QByteArray &target, const QByteArray &chunk, bool *truncated)
{
    const qsizetype remaining = std::max<qsizetype>(
        0, MaxProviderStreamBytes - target.size());
    target.append(chunk.constData(), std::min(remaining, chunk.size()));
    if (chunk.size() > remaining && truncated)
        *truncated = true;
}

void drainProcess(QProcess &process, ProcessResult &result)
{
    appendBounded(result.standardOutput, process.readAllStandardOutput(),
                  &result.standardOutputTruncated);
    appendBounded(result.standardError, process.readAllStandardError(),
                  &result.standardErrorTruncated);
}

class ProviderCommandTelemetry final
{
public:
    ProviderCommandTelemetry(const Command &command, const ProcessResult &result)
        : m_result(result)
    {
        m_elapsed.start();
        StructuredLogger::instance().log(
            LogSeverity::Debug, QStringLiteral("vmlab"),
            QStringLiteral("vmlab.provider-command.started"),
            QStringLiteral("A VM provider command started. / VM 提供者命令已經開始。"),
            QJsonObject{
                // Executable names, paths, arguments, working directories,
                // and stream contents are deliberately excluded.
                {QStringLiteral("argumentCount"), command.arguments.size()},
                {QStringLiteral("detached"), command.detached},
                {QStringLiteral("interruptible"), command.interruptible},
                {QStringLiteral("timeoutMs"), command.timeoutMs},
                {QStringLiteral("workingDirectoryProvided"),
                 !command.workingDirectory.isEmpty()},
            });
    }

    ~ProviderCommandTelemetry()
    {
        StructuredLogger::instance().log(
            m_result.ok() ? LogSeverity::Debug : LogSeverity::Error,
            QStringLiteral("vmlab"),
            QStringLiteral("vmlab.provider-command.completed"),
            m_result.ok()
                ? QStringLiteral("A VM provider command completed. / VM 提供者命令已經完成。")
                : QStringLiteral("A VM provider command failed. / VM 提供者命令失敗。"),
            QJsonObject{
                {QStringLiteral("started"), m_result.started},
                {QStringLiteral("succeeded"), m_result.ok()},
                {QStringLiteral("timedOut"), m_result.timedOut},
                {QStringLiteral("deadlineExceeded"),
                 m_result.deadlineExceeded},
                {QStringLiteral("exitCode"), m_result.exitCode},
                {QStringLiteral("elapsedMs"), m_elapsed.elapsed()},
                {QStringLiteral("errorPresent"), !m_result.error.isEmpty()},
                {QStringLiteral("standardOutputBytes"),
                 static_cast<qint64>(m_result.standardOutput.size())},
                {QStringLiteral("standardErrorBytes"),
                 static_cast<qint64>(m_result.standardError.size())},
                {QStringLiteral("standardOutputTruncated"),
                 m_result.standardOutputTruncated},
                {QStringLiteral("standardErrorTruncated"),
                 m_result.standardErrorTruncated},
            });
    }

private:
    const ProcessResult &m_result;
    QElapsedTimer m_elapsed;
};

class BoundedProcessRunner final : public CancelableCommandRunner
{
public:
    ProcessResult run(const Command &command) override
    {
        ProcessResult result;
        [[maybe_unused]] const ProviderCommandTelemetry telemetry(command, result);
        if (!command.valid(&result.error))
            return result;
        if (m_cancelled.load(std::memory_order_acquire)
            && (command.interruptible || !m_mutationStarted)) {
            result.error = QStringLiteral("VM operation was cancelled.");
            return result;
        }
        if (!command.interruptible)
            m_mutationStarted = true;

        QProcess process;
        process.setProgram(resolveExecutableForLaunch(command.executable));
        process.setArguments(command.arguments);
        process.setProcessChannelMode(QProcess::SeparateChannels);
        if (!command.workingDirectory.isEmpty())
            process.setWorkingDirectory(command.workingDirectory);

        if (command.detached) {
            qint64 processId = 0;
            if (!process.startDetached(&processId)) {
                result.error = process.errorString();
                return result;
            }
            result.started = true;
            result.exitCode = 0;
            return result;
        }

        configureProcessWithoutConsole(process);
        QElapsedTimer elapsed;
        elapsed.start();
        process.start();
        const int startLimit = std::min(command.timeoutMs, 10000);
        while (process.state() == QProcess::Starting && elapsed.elapsed() < startLimit) {
            if (command.interruptible
                && m_cancelled.load(std::memory_order_acquire)) {
                process.kill();
                process.waitForFinished(1000);
                result.error = QStringLiteral("VM operation was cancelled.");
                return result;
            }
            process.waitForStarted(50);
        }
        if (process.state() == QProcess::NotRunning) {
            result.error = process.errorString();
            return result;
        }
        result.started = true;

        bool deadlineExceeded = false;
        while (process.state() != QProcess::NotRunning) {
            if (command.interruptible
                && m_cancelled.load(std::memory_order_acquire)) {
                process.terminate();
                if (!process.waitForFinished(500)) {
                    process.kill();
                    process.waitForFinished(2000);
                }
                drainProcess(process, result);
                result.exitCode = process.exitCode();
                result.error = QStringLiteral("VM operation was cancelled.");
                return result;
            }
            if (elapsed.elapsed() >= command.timeoutMs) {
                deadlineExceeded = true;
                if (command.interruptible)
                    break;
            }
            drainProcess(process, result);
            process.waitForFinished(50);
        }

        if (process.state() != QProcess::NotRunning) {
            result.timedOut = true;
            result.error = QStringLiteral("Provider command timed out after %1 ms.")
                               .arg(command.timeoutMs);
            process.kill();
            process.waitForFinished(2000);
        }
        drainProcess(process, result);
        if (!result.timedOut) {
            result.exitCode = process.exitCode();
            if (process.exitStatus() != QProcess::NormalExit)
                result.error = QStringLiteral("Provider command crashed.");
            else if (deadlineExceeded)
                result.deadlineExceeded = true;
        }
        return result;
    }

    void requestCancel() override
    {
        m_cancelled.store(true, std::memory_order_release);
    }

    bool cancellationRequested() const override
    {
        return m_cancelled.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool m_cancelled = false;
    bool m_mutationStarted = false;
};

class RecordingRunner final : public CommandRunner
{
public:
    explicit RecordingRunner(CommandRunner &inner) : m_inner(inner) {}

    ProcessResult run(const Command &command) override
    {
        ProcessResult result = m_inner.run(command);
        m_transcripts.append(CommandTranscript{command, result});
        return result;
    }

    [[nodiscard]] QList<CommandTranscript> transcripts() const { return m_transcripts; }

private:
    CommandRunner &m_inner;
    QList<CommandTranscript> m_transcripts;
};

Plan invalidPlan(const QString &error)
{
    Plan result;
    result.errors.append(error);
    return result;
}

bool sameProviderPath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return QDir::fromNativeSeparators(QFileInfo(left).absoluteFilePath()).compare(
               QDir::fromNativeSeparators(QFileInfo(right).absoluteFilePath()), sensitivity) == 0;
}

const Machine *catalogMatch(const QList<Machine> &catalog, const VmRef &reference)
{
    const auto found = std::find_if(catalog.cbegin(), catalog.cend(),
                                    [&reference](const Machine &machine) {
        return machine.ref.providerId == reference.providerId
            && machine.ref.id == reference.id;
    });
    return found == catalog.cend() ? nullptr : &*found;
}

const Machine *catalogPathMatch(const QList<Machine> &catalog,
                                const QString &providerId,
                                const QString &path)
{
    const auto found = std::find_if(catalog.cbegin(), catalog.cend(),
                                    [&providerId, &path](const Machine &machine) {
        return machine.ref.providerId == providerId
            && sameProviderPath(machine.configPath, path);
    });
    return found == catalog.cend() ? nullptr : &*found;
}

Machine staleMachine(Machine machine, const QString &reason)
{
    machine.powerState = PowerState::Unknown;
    machine.storagePaths.clear();
    machine.storageDevices.clear();
    machine.networkDevices.clear();
    machine.cpuCount.reset();
    machine.memoryMiB.reset();
    machine.firmware.reset();
    machine.secureBoot.reset();
    machine.tpm.reset();
    machine.stateRevision.clear();
    machine.inventoryComplete = false;
    machine.hardwareInventoryComplete = false;
    machine.warnings.append(reason);
    return machine;
}

QString providerFailure(const ProcessResult &result, const QString &context)
{
    if (!result.error.isEmpty())
        return QStringLiteral("%1: %2").arg(context, result.error);
    const QString detail = QString::fromLocal8Bit(result.standardError).trimmed();
    return QStringLiteral("%1 failed with exit code %2%3")
        .arg(context)
        .arg(result.exitCode)
        .arg(detail.isEmpty() ? QStringLiteral(".")
                              : QStringLiteral(": %1").arg(detail.left(1024)));
}

QString defaultControllerName(const StorageDeviceSpec &storage)
{
    if (!storage.controllerName.trimmed().isEmpty())
        return storage.controllerName.trimmed();
    if (storage.controller != 0)
        return {};
    switch (storage.bus) {
    case StorageBus::Ide: return QStringLiteral("IDE");
    case StorageBus::Sata: return QStringLiteral("SATA");
    case StorageBus::Scsi: return QStringLiteral("SCSI");
    case StorageBus::Nvme: return QStringLiteral("NVMe");
    }
    return {};
}

QString vmxBusName(StorageBus bus)
{
    switch (bus) {
    case StorageBus::Ide: return QStringLiteral("ide");
    case StorageBus::Sata: return QStringLiteral("sata");
    case StorageBus::Scsi: return QStringLiteral("scsi");
    case StorageBus::Nvme: return QStringLiteral("nvme");
    }
    return {};
}

bool validateStorageSlot(const StorageDeviceSpec &storage, bool attaching, QString *error)
{
    const int maximumPort = storage.bus == StorageBus::Ide ? 1 : 29;
    const int maximumDevice = storage.bus == StorageBus::Ide ? 1 : 0;
    if (storage.controller < 0 || storage.controller > 3
        || storage.port < 0 || storage.port > maximumPort
        || storage.device < 0 || storage.device > maximumDevice) {
        if (error)
            *error = QStringLiteral("Storage controller, port, or device is outside safe topology limits.");
        return false;
    }
    if (attaching) {
        const QFileInfo path(storage.path);
        if (!path.isAbsolute() || !path.exists() || !path.isFile()) {
            if (error)
                *error = QStringLiteral("Attached storage must be an existing absolute file.");
            return false;
        }
        if (storage.optical
            && path.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) != 0) {
            if (error)
                *error = QStringLiteral("Optical media must be an ISO file.");
            return false;
        }
    }
    if (error)
        error->clear();
    return true;
}

bool machineReadyForEdit(const ProviderInfo &provider,
                         const Machine &machine,
                         const QString &requiredCapability,
                         QString *error)
{
    if (!provider.available || machine.ref.providerId != provider.id
        || !machine.ref.valid() || !machine.inventoryComplete
        || !machine.hardwareInventoryComplete
        || machine.stateRevision.isEmpty()) {
        if (error)
            *error = QStringLiteral("Refresh complete live state for this provider and VM first.");
        return false;
    }
    if (!requiredCapability.isEmpty() && !provider.supports(requiredCapability)) {
        if (error)
            *error = QStringLiteral("Detected provider evidence does not prove the required capability.");
        return false;
    }
    if (machine.powerState != PowerState::PoweredOff) {
        if (error)
            *error = QStringLiteral("Storage, network, and configuration changes require a powered-off VM.");
        return false;
    }
    if (error)
        error->clear();
    return true;
}

void addLivePreflight(Plan &plan, const ProviderInfo &provider, const Machine &machine)
{
    if (provider.id == virtualBoxProviderId()) {
        plan.preflight.append(CommandEvidence{
            Command{provider.executable,
                    {QStringLiteral("showvminfo"), machine.ref.id,
                     QStringLiteral("--machinereadable")}, {}, 30000, false, true},
            EvidenceFormat::RawSha256, machine.stateRevision,
            QStringLiteral("VirtualBox machine state/configuration")});
    } else {
        const QString token = provider.id == vmwarePlayerProviderId()
            ? QStringLiteral("player") : QStringLiteral("ws");
        plan.preflight.append(CommandEvidence{
            Command{provider.executable,
                    {QStringLiteral("-T"), token, QStringLiteral("list")}, {}, 30000,
                    false, true},
            EvidenceFormat::VmwareRunningPathsSha256, machine.stateRevision,
            QStringLiteral("VMware running-machine inventory")});
    }
    QString fileError;
    if (!addFileEvidence(plan, machine.configPath,
                         QStringLiteral("Selected VM configuration"), &fileError))
        plan.errors.append(fileError);
}

Plan virtualBoxStoragePlan(const ProviderInfo &provider,
                           const OperationRequest &request,
                           bool attach)
{
    if (!request.machine)
        return invalidPlan(QStringLiteral("Select a VM before editing storage."));
    QString error;
    if (!machineReadyForEdit(provider, *request.machine, capability::media(), &error)
        || !validateStorageSlot(request.storage, attach, &error)) {
        return invalidPlan(error);
    }
    const QString controller = defaultControllerName(request.storage);
    if (controller.isEmpty())
        return invalidPlan(QStringLiteral("A reviewed controller name is required for non-default VirtualBox controllers."));
    const auto occupied = std::find_if(
        request.machine->storageDevices.cbegin(), request.machine->storageDevices.cend(),
        [&request, &controller](const StorageAttachment &device) {
            return device.controllerName.compare(controller, Qt::CaseInsensitive) == 0
                && device.port == request.storage.port
                && device.device == request.storage.device;
        });
    if (attach && occupied != request.machine->storageDevices.cend()) {
        return invalidPlan(QStringLiteral(
            "The reviewed VirtualBox controller/port/device is already occupied; detach it explicitly first."));
    }
    if (!attach && occupied == request.machine->storageDevices.cend()) {
        return invalidPlan(QStringLiteral(
            "The reviewed VirtualBox controller/port/device is already empty."));
    }
    if (!attach && occupied != request.machine->storageDevices.cend()) {
        if (occupied->optical != request.storage.optical) {
            return invalidPlan(QStringLiteral(
                "The live VirtualBox device type no longer matches the reviewed detach request."));
        }
        if (!request.storage.path.trimmed().isEmpty()
            && !sameProviderPath(occupied->path, request.storage.path)) {
            return invalidPlan(QStringLiteral(
                "The live VirtualBox medium path no longer matches the reviewed detach request."));
        }
    }
    const QString medium = attach
        ? QFileInfo(request.storage.path).absoluteFilePath() : QStringLiteral("none");
    const Command command{
        provider.executable,
        {QStringLiteral("storageattach"), request.machine->ref.id,
         QStringLiteral("--storagectl"), controller,
         QStringLiteral("--port"), QString::number(request.storage.port),
         QStringLiteral("--device"), QString::number(request.storage.device),
         QStringLiteral("--type"), request.storage.optical ? QStringLiteral("dvddrive")
                                                           : QStringLiteral("hdd"),
         QStringLiteral("--medium"), medium},
        {}, 10 * 60 * 1000};
    Plan plan;
    plan.preview = makePreview(
        attach ? QStringLiteral("attach-storage") : QStringLiteral("detach-storage"),
        request.machine->ref, Risk::Reversible,
        {attach ? QStringLiteral("Attach %1 at %2 port %3 device %4.")
                      .arg(medium, controller)
                      .arg(request.storage.port)
                      .arg(request.storage.device)
                : QStringLiteral("Detach media from %1 port %2 device %3 without deleting it.")
                      .arg(controller)
                      .arg(request.storage.port)
                      .arg(request.storage.device)},
        {}, {command}, request.revision, request.now);
    addLivePreflight(plan, provider, *request.machine);
    if (attach && !addFileEvidence(plan, request.storage.path,
                                   request.storage.optical
                                       ? QStringLiteral("Attached optical image")
                                       : QStringLiteral("Attached virtual disk"),
                                   &error))
        plan.errors.append(error);
    return plan;
}

Plan vmwareStoragePlan(const ProviderInfo &provider,
                       const OperationRequest &request,
                       bool attach)
{
    if (!request.machine)
        return invalidPlan(QStringLiteral("Select a VM before editing storage."));
    QString error;
    if (!machineReadyForEdit(provider, *request.machine, capability::media(), &error)
        || !validateStorageSlot(request.storage, attach, &error)) {
        return invalidPlan(error);
    }
    std::optional<VmxDocument> document = VmxDocument::load(
        request.machine->configPath, &error);
    if (!document)
        return invalidPlan(error);

    const QString prefix = QStringLiteral("%1%2:%3")
                               .arg(vmxBusName(request.storage.bus))
                               .arg(request.storage.controller)
                               .arg(request.storage.port);
    const QString presentKey = prefix + QStringLiteral(".present");
    const bool currentlyPresent = document->value(presentKey)
                                      .compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0;
    if (attach && (currentlyPresent
                   || !document->value(prefix + QStringLiteral(".fileName")).isEmpty())) {
        return invalidPlan(QStringLiteral("The reviewed VMware storage slot is already occupied."));
    }
    if (!attach && !currentlyPresent)
        return invalidPlan(QStringLiteral("The reviewed VMware storage slot is already empty."));

    if (attach) {
        if (!document->setValue(
                QStringLiteral("%1%2.present").arg(vmxBusName(request.storage.bus))
                    .arg(request.storage.controller), QStringLiteral("TRUE"), &error)
            || !document->setValue(presentKey, QStringLiteral("TRUE"), &error)
            || !document->setValue(prefix + QStringLiteral(".fileName"),
                                   QFileInfo(request.storage.path).absoluteFilePath(), &error)) {
            return invalidPlan(error);
        }
        if (request.storage.optical) {
            if (!document->setValue(prefix + QStringLiteral(".deviceType"),
                                    QStringLiteral("cdrom-image"), &error))
                return invalidPlan(error);
        } else {
            document->remove(prefix + QStringLiteral(".deviceType"));
        }
    } else {
        if (!document->setValue(presentKey, QStringLiteral("FALSE"), &error))
            return invalidPlan(error);
        document->remove(prefix + QStringLiteral(".fileName"));
        document->remove(prefix + QStringLiteral(".deviceType"));
    }
    const QString expected = fileSha256(request.machine->configPath, &error);
    if (!error.isEmpty())
        return invalidPlan(error);
    Plan plan;
    plan.preview = makePreview(
        attach ? QStringLiteral("attach-storage") : QStringLiteral("detach-storage"),
        request.machine->ref, Risk::Reversible,
        {attach ? QStringLiteral("Atomically attach storage to powered-off VMX slot %1.").arg(prefix)
                : QStringLiteral("Atomically detach VMX slot %1 without deleting media.").arg(prefix)},
        {}, {}, request.revision, request.now);
    plan.atomicWritesAfterCommands.append(
        AtomicWrite{request.machine->configPath, document->serialize(), expected});
    addLivePreflight(plan, provider, *request.machine);
    if (attach && !addFileEvidence(plan, request.storage.path,
                                   request.storage.optical
                                       ? QStringLiteral("Attached optical image")
                                       : QStringLiteral("Attached virtual disk"),
                                   &error))
        plan.errors.append(error);
    return plan;
}

QString virtualBoxNetworkMode(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::Nat: return QStringLiteral("nat");
    case NetworkMode::Bridged: return QStringLiteral("bridged");
    case NetworkMode::HostOnly: return QStringLiteral("hostonly");
    case NetworkMode::Internal: return QStringLiteral("intnet");
    case NetworkMode::Disconnected: return QStringLiteral("none");
    }
    return {};
}

Plan virtualBoxNetworkPlan(const ProviderInfo &provider,
                           const OperationRequest &request,
                           bool attach)
{
    if (!request.machine)
        return invalidPlan(QStringLiteral("Select a VM before editing networking."));
    QString error;
    if (!machineReadyForEdit(provider, *request.machine, capability::configure(), &error))
        return invalidPlan(error);
    if (request.network.slot < 1 || request.network.slot > 8)
        return invalidPlan(QStringLiteral("VirtualBox network adapter slot must be between 1 and 8."));
    const NetworkMode mode = attach ? request.network.mode : NetworkMode::Disconnected;
    if (attach && (mode == NetworkMode::Bridged || mode == NetworkMode::HostOnly
                   || mode == NetworkMode::Internal)
        && request.network.interfaceName.trimmed().isEmpty()) {
        return invalidPlan(QStringLiteral(
            "VirtualBox bridged, host-only, and internal adapters require an explicit reviewed network name."));
    }
    QStringList arguments{
        QStringLiteral("modifyvm"), request.machine->ref.id,
        QStringLiteral("--nic%1").arg(request.network.slot), virtualBoxNetworkMode(mode)};
    if (attach && mode != NetworkMode::Disconnected) {
        arguments.append({QStringLiteral("--cableconnected%1").arg(request.network.slot),
                          request.network.connected ? QStringLiteral("on") : QStringLiteral("off")});
        if (!request.network.interfaceName.trimmed().isEmpty()) {
            QString option;
            if (mode == NetworkMode::Bridged)
                option = QStringLiteral("--bridgeadapter%1").arg(request.network.slot);
            else if (mode == NetworkMode::HostOnly)
                option = QStringLiteral("--hostonlyadapter%1").arg(request.network.slot);
            else if (mode == NetworkMode::Internal)
                option = QStringLiteral("--intnet%1").arg(request.network.slot);
            else
                return invalidPlan(QStringLiteral("This network mode does not accept an interface name."));
            arguments.append({option, request.network.interfaceName.trimmed()});
        }
    }
    Plan plan;
    plan.preview = makePreview(
        attach ? QStringLiteral("attach-network") : QStringLiteral("detach-network"),
        request.machine->ref, Risk::Reversible,
        {attach ? QStringLiteral("Configure powered-off network adapter %1 as %2.")
                      .arg(request.network.slot)
                      .arg(networkModeName(mode))
                : QStringLiteral("Disconnect powered-off network adapter %1.")
                      .arg(request.network.slot)},
        {}, {Command{provider.executable, arguments, {}, 30000}},
        request.revision, request.now);
    addLivePreflight(plan, provider, *request.machine);
    return plan;
}

Plan vmwareNetworkPlan(const ProviderInfo &provider,
                       const OperationRequest &request,
                       bool attach)
{
    if (!request.machine)
        return invalidPlan(QStringLiteral("Select a VM before editing networking."));
    QString error;
    if (!machineReadyForEdit(provider, *request.machine, capability::configure(), &error))
        return invalidPlan(error);
    if (request.network.slot < 1 || request.network.slot > 10)
        return invalidPlan(QStringLiteral("VMware network adapter slot must be between 1 and 10."));
    const QString interfaceName = request.network.interfaceName.trimmed();
    static const QRegularExpression vmnetName(QStringLiteral("^VMnet[0-9]{1,3}$"),
                                               QRegularExpression::CaseInsensitiveOption);
    if (attach && request.network.mode == NetworkMode::Internal
        && !vmnetName.match(interfaceName).hasMatch()) {
        return invalidPlan(QStringLiteral(
            "VMware custom networking requires an explicit VMnet name such as VMnet2."));
    }
    if ((!attach || request.network.mode != NetworkMode::Internal)
        && !interfaceName.isEmpty()) {
        return invalidPlan(QStringLiteral(
            "VMware named networks are accepted only for explicit custom VMnet adapters."));
    }

    std::optional<VmxDocument> document = VmxDocument::load(
        request.machine->configPath, &error);
    if (!document)
        return invalidPlan(error);
    const QString prefix = QStringLiteral("ethernet%1").arg(request.network.slot - 1);
    if (!document->setValue(prefix + QStringLiteral(".present"),
                            attach ? QStringLiteral("TRUE") : QStringLiteral("FALSE"), &error))
        return invalidPlan(error);
    if (attach) {
        QString connection;
        switch (request.network.mode) {
        case NetworkMode::Nat: connection = QStringLiteral("nat"); break;
        case NetworkMode::Bridged: connection = QStringLiteral("bridged"); break;
        case NetworkMode::HostOnly: connection = QStringLiteral("hostonly"); break;
        case NetworkMode::Disconnected: connection = QStringLiteral("nat"); break;
        case NetworkMode::Internal: connection = QStringLiteral("custom"); break;
        }
        if (!document->setValue(prefix + QStringLiteral(".connectionType"), connection, &error)
            || !document->setValue(
                prefix + QStringLiteral(".startConnected"),
                request.network.connected && request.network.mode != NetworkMode::Disconnected
                    ? QStringLiteral("TRUE") : QStringLiteral("FALSE"), &error)) {
            return invalidPlan(error);
        }
        if (request.network.mode == NetworkMode::Internal) {
            if (!document->setValue(prefix + QStringLiteral(".vnet"), interfaceName, &error))
                return invalidPlan(error);
        } else {
            document->remove(prefix + QStringLiteral(".vnet"));
        }
    }
    const QString expected = fileSha256(request.machine->configPath, &error);
    if (!error.isEmpty())
        return invalidPlan(error);
    Plan plan;
    plan.preview = makePreview(
        attach ? QStringLiteral("attach-network") : QStringLiteral("detach-network"),
        request.machine->ref, Risk::Reversible,
        {attach ? QStringLiteral("Atomically configure powered-off VMware adapter %1.")
                      .arg(request.network.slot)
                : QStringLiteral("Atomically remove powered-off VMware adapter %1.")
                      .arg(request.network.slot)},
        {}, {}, request.revision, request.now);
    plan.atomicWritesAfterCommands.append(
        AtomicWrite{request.machine->configPath, document->serialize(), expected});
    addLivePreflight(plan, provider, *request.machine);
    return plan;
}

bool pathContainedForCreate(const QString &root, const QString &target)
{
    const QFileInfo rootInfo(root);
    if (!rootInfo.isAbsolute() || !rootInfo.exists() || !rootInfo.isDir())
        return false;
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || !QFileInfo(target).isAbsolute())
        return false;
    const QString absoluteTarget = QDir::cleanPath(QFileInfo(target).absoluteFilePath());
    const QString relative = QDir(canonicalRoot).relativeFilePath(absoluteTarget);
    if (relative == QStringLiteral(".") || relative == QStringLiteral("..")
        || relative.startsWith(QStringLiteral("../"))
        || relative.startsWith(QStringLiteral("..\\"))
        || QDir::isAbsolutePath(relative)) {
        return false;
    }
    const QFileInfo targetInfo(absoluteTarget);
    if (targetInfo.exists())
        return false;
    const QFileInfo targetParent(targetInfo.absolutePath());
    if (!targetParent.exists() || !targetParent.isDir() || targetParent.isSymLink()
#ifdef Q_OS_WIN
        || targetParent.isJunction()
#endif
    ) {
        return false;
    }
    QFileInfo ancestor(absoluteTarget);
    while (!ancestor.exists() && ancestor.absolutePath() != ancestor.absoluteFilePath())
        ancestor.setFile(ancestor.absolutePath());
    const QString canonicalAncestor = ancestor.canonicalFilePath();
    if (canonicalAncestor.isEmpty())
        return false;
    QFileInfo cursor(absoluteTarget);
    while (!sameProviderPath(cursor.absoluteFilePath(), canonicalRoot)) {
        if (cursor.exists() && (cursor.isSymLink()
#ifdef Q_OS_WIN
                                || cursor.isJunction()
#endif
                                )) {
            return false;
        }
        const QString parent = cursor.absolutePath();
        if (sameProviderPath(parent, cursor.absoluteFilePath()))
            return false;
        cursor.setFile(parent);
    }
    const QString ancestorRelative = QDir(canonicalRoot).relativeFilePath(canonicalAncestor);
    return ancestorRelative != QStringLiteral("..")
        && !ancestorRelative.startsWith(QStringLiteral("../"))
        && !ancestorRelative.startsWith(QStringLiteral("..\\"))
        && !QDir::isAbsolutePath(ancestorRelative);
}

} // namespace

std::shared_ptr<CancelableCommandRunner> ProcessCommandRunnerFactory::create() const
{
    return std::make_shared<BoundedProcessRunner>();
}

QList<ProviderInfo> NativeVmLabProviderAdapter::detect(
    const QList<ProviderProbePaths> &candidates,
    CommandRunner &runner) const
{
    const QList<ProviderInfo> detected = ProviderDetector::detect(candidates, runner);
    QList<ProviderInfo> result;
    for (const ProviderInfo &candidate : detected) {
        const auto existing = std::find_if(result.begin(), result.end(),
                                           [&candidate](const ProviderInfo &provider) {
            return provider.id == candidate.id;
        });
        if (existing == result.end()) {
            result.append(candidate);
        } else if ((!existing->available && candidate.available)
                   || (existing->available == candidate.available
                       && candidate.capabilities.size() > existing->capabilities.size())) {
            *existing = candidate;
        } else {
            existing->warnings.append(candidate.warnings);
            existing->evidence.append(candidate.evidence);
            existing->warnings.removeDuplicates();
            existing->evidence.removeDuplicates();
        }
    }
    return result;
}

InventoryRefreshResult NativeVmLabProviderAdapter::refreshInventory(
    const QList<ProviderInfo> &providers,
    const QList<Machine> &catalogMachines,
    CommandRunner &runner) const
{
    InventoryRefreshResult refresh;
    refresh.complete = true;
    QSet<QString> visitedCatalogIdentities;
    const auto markCatalog = [&visitedCatalogIdentities](const Machine &machine) {
        visitedCatalogIdentities.insert(machine.ref.providerId + QChar::Null + machine.ref.id);
    };

    for (const ProviderInfo &info : providers) {
        if (!info.available || !info.supports(capability::inventory())) {
            const bool ownsCatalogMachine = std::any_of(
                catalogMachines.cbegin(), catalogMachines.cend(),
                [&info](const Machine &machine) {
                    return machine.ref.providerId == info.id;
                });
            if (ownsCatalogMachine) {
                refresh.complete = false;
                refresh.warnings.append(QStringLiteral(
                    "%1 inventory is unavailable for a catalog VM.")
                                            .arg(info.displayName.isEmpty()
                                                     ? info.id : info.displayName));
            }
            continue;
        }
        if (info.id == virtualBoxProviderId()) {
            const VirtualBoxProvider provider(info);
            const ProcessResult listed = runner.run(provider.inventoryCommand());
            if (!listed.ok()) {
                refresh.complete = false;
                refresh.warnings.append(providerFailure(listed, QStringLiteral("VirtualBox inventory")));
                continue;
            }
            QString parseError;
            const QList<VmRef> references = VirtualBoxProvider::parseMachineList(
                listed.standardOutput, &parseError);
            if (!parseError.isEmpty()) {
                refresh.complete = false;
                refresh.warnings.append(parseError);
                continue;
            }
            for (const VmRef &reference : references) {
                const ProcessResult inspected = runner.run(provider.machineInfoCommand(reference));
                if (!inspected.ok()) {
                    refresh.complete = false;
                    refresh.warnings.append(providerFailure(
                        inspected, QStringLiteral("VirtualBox machine '%1'").arg(reference.name)));
                    if (const Machine *catalog = catalogMatch(catalogMachines, reference)) {
                        refresh.machines.append(staleMachine(
                            *catalog, QStringLiteral("VirtualBox live inspection failed.")));
                        markCatalog(*catalog);
                    }
                    continue;
                }
                std::optional<Machine> machine = VirtualBoxProvider::parseMachineInfo(
                    inspected.standardOutput, &parseError);
                if (!machine) {
                    refresh.complete = false;
                    refresh.warnings.append(parseError);
                    continue;
                }
                if (const Machine *catalog = catalogMatch(catalogMachines, machine->ref)) {
                    machine->ownership = catalog->ownership;
                    machine->ownershipToken = catalog->ownershipToken;
                    markCatalog(*catalog);
                } else {
                    machine->ownership = Ownership::External;
                }
                refresh.machines.append(*machine);
            }
        } else if (info.id == vmwareWorkstationProviderId()
                   || info.id == vmwarePlayerProviderId()) {
            const VmwareProvider provider(info);
            const ProcessResult listed = runner.run(provider.inventoryCommand());
            if (!listed.ok()) {
                refresh.complete = false;
                refresh.warnings.append(providerFailure(
                    listed, QStringLiteral("%1 inventory").arg(info.displayName)));
                continue;
            }
            QString parseError;
            const QStringList running = VmwareProvider::parseRunningVmList(
                listed.standardOutput, &parseError);
            if (!parseError.isEmpty()) {
                refresh.complete = false;
                refresh.warnings.append(parseError);
                continue;
            }
            QStringList paths = running;
            for (const Machine &catalog : catalogMachines) {
                if (catalog.ref.providerId == info.id)
                    paths.append(catalog.configPath);
            }
            QStringList uniquePaths;
            for (const QString &path : std::as_const(paths)) {
                if (std::none_of(uniquePaths.cbegin(), uniquePaths.cend(),
                                 [&path](const QString &known) {
                    return sameProviderPath(known, path);
                })) {
                    uniquePaths.append(QFileInfo(path).absoluteFilePath());
                }
            }
            for (const QString &path : std::as_const(uniquePaths)) {
                const Machine *catalog = catalogPathMatch(catalogMachines, info.id, path);
                Machine machine = provider.inspectMachine(
                    path, running, catalog ? catalog->ownership : Ownership::External,
                    &parseError);
                if (catalog)
                    machine.ownershipToken = catalog->ownershipToken;
                if (!parseError.isEmpty()) {
                    refresh.complete = false;
                    refresh.warnings.append(parseError);
                    continue;
                }
                if (catalog)
                    markCatalog(*catalog);
                refresh.machines.append(machine);
                if (!machine.inventoryComplete)
                    refresh.complete = false;
            }
        }
    }

    for (const Machine &catalog : catalogMachines) {
        const QString identity = catalog.ref.providerId + QChar::Null + catalog.ref.id;
        if (!visitedCatalogIdentities.contains(identity)) {
            refresh.machines.append(staleMachine(
                catalog, QStringLiteral("Provider inventory did not return this catalog VM.")));
            refresh.complete = false;
        }
    }
    std::sort(refresh.machines.begin(), refresh.machines.end(),
              [](const Machine &left, const Machine &right) {
        const int providerOrder = left.ref.providerId.compare(
            right.ref.providerId, Qt::CaseInsensitive);
        return providerOrder == 0
            ? left.ref.name.compare(right.ref.name, Qt::CaseInsensitive) < 0
            : providerOrder < 0;
    });
    refresh.success = !refresh.machines.isEmpty() || refresh.complete;
    if (!refresh.success)
        refresh.error = refresh.warnings.isEmpty()
            ? QStringLiteral("No VM provider inventory could be refreshed.")
            : refresh.warnings.join(QStringLiteral("\n"));
    return refresh;
}

Plan NativeVmLabProviderAdapter::plan(const ProviderInfo &provider,
                                      const OperationRequest &request) const
{
    if (!isKnownProviderId(provider.id))
        return invalidPlan(QStringLiteral("The selected VM provider is unsupported."));
    if (!request.now.isValid() || request.revision.isEmpty())
        return invalidPlan(QStringLiteral("Refresh catalog state before reviewing a VM operation."));

    if (request.action == ManagerAction::ForgetCatalog) {
        if (!request.machine || request.machine->ref.providerId != provider.id)
            return invalidPlan(QStringLiteral("Select the catalog VM entry to forget."));
        return makeForgetCatalogPlan(*request.machine, request.revision, request.now);
    }
    if (!provider.available)
        return invalidPlan(QStringLiteral("The selected VM provider is unavailable."));

    const bool virtualBox = provider.id == virtualBoxProviderId();
    const bool vmware = provider.id == vmwareWorkstationProviderId()
        || provider.id == vmwarePlayerProviderId();
    if (!virtualBox && !vmware)
        return invalidPlan(QStringLiteral("The selected VM provider is unsupported."));

    if (request.action == ManagerAction::Create) {
        if (!provider.supports(capability::create()))
            return invalidPlan(QStringLiteral("Detected provider evidence does not prove VM creation support."));
        const QString requestedNetwork = request.createSpec.bridgedInterface.trimmed();
        if (virtualBox
            && (request.createSpec.networkMode == NetworkMode::Bridged
                || request.createSpec.networkMode == NetworkMode::HostOnly
                || request.createSpec.networkMode == NetworkMode::Internal)
            && requestedNetwork.isEmpty()) {
            return invalidPlan(QStringLiteral(
                "VirtualBox bridged, host-only, and internal creation requires an explicit network name."));
        }
        static const QRegularExpression vmnetName(
            QStringLiteral("^VMnet[0-9]{1,3}$"),
            QRegularExpression::CaseInsensitiveOption);
        if (vmware && request.createSpec.networkMode == NetworkMode::Internal
            && !vmnetName.match(requestedNetwork).hasMatch()) {
            return invalidPlan(QStringLiteral(
                "VMware custom networking requires an explicit VMnet name such as VMnet2."));
        }
        if (vmware && request.createSpec.networkMode != NetworkMode::Internal
            && !requestedNetwork.isEmpty()) {
            return invalidPlan(QStringLiteral(
                "VMware physical bridged-interface selection is configured in the provider, not per VM."));
        }
        if (request.ownership == Ownership::Managed) {
            const QString target = virtualBox
                ? QDir(request.createSpec.directory).filePath(request.createSpec.name)
                : request.createSpec.directory;
            if (!pathContainedForCreate(request.managedRoot, target))
                return invalidPlan(QStringLiteral("Managed VM creation must remain under the configured managed root without reparse traversal."));
        }
        const QFileInfo iso(request.createSpec.isoPath);
        if (request.createSpec.isoPath.isEmpty() || !iso.isAbsolute()
            || !iso.exists() || !iso.isFile()
            || iso.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) != 0) {
            return invalidPlan(QStringLiteral("Create-from-ISO requires an existing absolute ISO file."));
        }
        Plan plan = virtualBox
            ? VirtualBoxProvider(provider).create(request.createSpec, request.revision, request.now)
            : VmwareProvider(provider).create(request.createSpec, request.revision, request.now);
        if (plan.ok() && request.ownership == Ownership::Managed) {
            const QString directory = virtualBox
                ? QDir(request.createSpec.directory).filePath(request.createSpec.name)
                : request.createSpec.directory;
            const CreationGuard creation = PathPolicy::managedCreateGuard(
                request.managedRoot, directory);
            if (!creation.allowed) {
                plan.errors.append(creation.error);
                return plan;
            }
            plan.managedOwnershipToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
            plan.managedCreateReservation = ManagedCreateReservation{
                creation.canonicalRoot, creation.targetDirectory,
                creation.rootIdentity};
            plan.atomicWritesAfterCommands.append(AtomicWrite{
                QDir(directory).filePath(managedOwnershipMarkerFileName()),
                managedOwnershipMarkerContents(plan.preview.target,
                                               plan.managedOwnershipToken),
                {}});
        }
        return plan;
    }

    if (request.action == ManagerAction::Register) {
        const QFileInfo configuration(request.path);
        if (!configuration.isAbsolute() || !configuration.exists() || !configuration.isFile())
            return invalidPlan(QStringLiteral("Registration requires an existing absolute provider configuration file."));
        if (virtualBox) {
            if (!provider.supports(capability::registerMachine())
                || configuration.suffix().compare(QStringLiteral("vbox"), Qt::CaseInsensitive) != 0) {
                return invalidPlan(QStringLiteral("VirtualBox registration requires a reviewed .vbox file and proven provider support."));
            }
            return VirtualBoxProvider(provider).registerMachine(
                configuration.absoluteFilePath(),
                request.name.trimmed().isEmpty() && request.machine
                    ? request.machine->ref.name : request.name,
                request.revision, request.now);
        }
        if (configuration.suffix().compare(QStringLiteral("vmx"), Qt::CaseInsensitive) != 0)
            return invalidPlan(QStringLiteral("VMware import requires a .vmx file."));
        QString error;
        const std::optional<VmxDocument> document = VmxDocument::load(
            configuration.absoluteFilePath(), &error);
        if (!document)
            return invalidPlan(error);
        QString displayName = request.name.trimmed();
        if (displayName.isEmpty())
            displayName = document->value(QStringLiteral("displayName")).trimmed();
        if (displayName.isEmpty())
            displayName = configuration.completeBaseName();
        if (!isSafeMachineFileStem(displayName))
            return invalidPlan(QStringLiteral("VMware display name is not a safe catalog identity."));
        const VmRef target{provider.id, configuration.absoluteFilePath(), displayName};
        Plan import;
        import.preview = makePreview(
            QStringLiteral("import"), target, Risk::ReadOnly,
            {QStringLiteral("Add the existing VMX to WimForge without claiming a VMware registration API or moving provider files.")},
            {}, {}, request.revision, request.now);
        return import;
    }

    if (!request.machine || request.machine->ref.providerId != provider.id)
        return invalidPlan(QStringLiteral("Select a VM owned by the requested provider."));
    if (!request.machine->inventoryComplete || request.machine->stateRevision.isEmpty())
        return invalidPlan(QStringLiteral("Refresh complete live VM state before reviewing this operation."));

    if (request.action == ManagerAction::AttachStorage)
        return virtualBox ? virtualBoxStoragePlan(provider, request, true)
                          : vmwareStoragePlan(provider, request, true);
    if (request.action == ManagerAction::DetachStorage)
        return virtualBox ? virtualBoxStoragePlan(provider, request, false)
                          : vmwareStoragePlan(provider, request, false);
    if (request.action == ManagerAction::AttachNetwork)
        return virtualBox ? virtualBoxNetworkPlan(provider, request, true)
                          : vmwareNetworkPlan(provider, request, true);
    if (request.action == ManagerAction::DetachNetwork)
        return virtualBox ? virtualBoxNetworkPlan(provider, request, false)
                          : vmwareNetworkPlan(provider, request, false);

    if (virtualBox) {
        const VirtualBoxProvider adapter(provider);
        switch (request.action) {
        case ManagerAction::OpenConsole:
            return adapter.openConsole(*request.machine, request.revision, request.now);
        case ManagerAction::Start:
            return adapter.start(*request.machine, request.headless, request.revision, request.now);
        case ManagerAction::GracefulShutdown:
            return adapter.gracefulShutdown(*request.machine, request.revision, request.now);
        case ManagerAction::PowerOff:
            return adapter.powerOff(*request.machine, request.revision, request.now);
        case ManagerAction::Pause:
            return adapter.pause(*request.machine, request.revision, request.now);
        case ManagerAction::Resume:
            return adapter.resume(*request.machine, request.revision, request.now);
        case ManagerAction::Reset:
            return adapter.reset(*request.machine, request.revision, request.now);
        case ManagerAction::SaveState:
            return adapter.saveState(*request.machine, request.revision, request.now);
        case ManagerAction::Configure:
            return adapter.configure(*request.machine, request.configPatch,
                                     request.revision, request.now);
        case ManagerAction::AttachIso:
            return adapter.attachIso(*request.machine, request.path,
                                     request.revision, request.now);
        case ManagerAction::DetachIso:
            return adapter.detachIso(*request.machine, request.revision, request.now);
        case ManagerAction::ListSnapshots:
            return adapter.listSnapshots(*request.machine, request.revision, request.now);
        case ManagerAction::TakeSnapshot:
            return adapter.takeSnapshot(*request.machine, request.name, request.description,
                                        request.revision, request.now);
        case ManagerAction::RestoreSnapshot:
            return adapter.restoreSnapshot(*request.machine, request.snapshot,
                                           request.revision, request.now);
        case ManagerAction::DeleteSnapshot:
            return adapter.deleteSnapshot(*request.machine, request.snapshot,
                                          request.revision, request.now);
        case ManagerAction::Unregister:
            return adapter.unregisterMachine(*request.machine, request.revision, request.now);
        case ManagerAction::Delete:
            return adapter.deleteMachine(*request.machine, request.managedRoot,
                                         request.allMachines, request.revision, request.now);
        case ManagerAction::Create:
        case ManagerAction::Register:
        case ManagerAction::ForgetCatalog:
        case ManagerAction::AttachStorage:
        case ManagerAction::DetachStorage:
        case ManagerAction::AttachNetwork:
        case ManagerAction::DetachNetwork:
            break;
        }
    } else {
        const VmwareProvider adapter(provider);
        switch (request.action) {
        case ManagerAction::OpenConsole:
            return adapter.openConsole(*request.machine, request.revision, request.now);
        case ManagerAction::Start:
            return adapter.start(*request.machine, request.headless, request.revision, request.now);
        case ManagerAction::GracefulShutdown:
            return adapter.gracefulShutdown(*request.machine, request.revision, request.now);
        case ManagerAction::PowerOff:
            return adapter.powerOff(*request.machine, request.revision, request.now);
        case ManagerAction::Pause:
            return adapter.pause(*request.machine, request.revision, request.now);
        case ManagerAction::Resume:
            return adapter.resume(*request.machine, request.revision, request.now);
        case ManagerAction::Reset:
            return adapter.reset(*request.machine, request.revision, request.now);
        case ManagerAction::SaveState:
            return adapter.saveState(*request.machine, request.revision, request.now);
        case ManagerAction::Configure:
            return adapter.configure(*request.machine, request.configPatch,
                                     request.revision, request.now);
        case ManagerAction::AttachIso:
            return adapter.attachIso(*request.machine, request.path,
                                     request.revision, request.now);
        case ManagerAction::DetachIso:
            return adapter.detachIso(*request.machine, request.revision, request.now);
        case ManagerAction::ListSnapshots:
            return adapter.listSnapshots(*request.machine, request.revision, request.now);
        case ManagerAction::TakeSnapshot:
            return adapter.takeSnapshot(*request.machine, request.name, request.description,
                                        request.revision, request.now);
        case ManagerAction::RestoreSnapshot:
            return adapter.restoreSnapshot(*request.machine, request.snapshot,
                                           request.revision, request.now);
        case ManagerAction::DeleteSnapshot:
            return adapter.deleteSnapshot(*request.machine, request.snapshot,
                                          request.revision, request.now);
        case ManagerAction::Unregister:
            return adapter.unregisterMachine(*request.machine, request.revision, request.now);
        case ManagerAction::Delete:
            return adapter.deleteMachine(*request.machine, request.managedRoot,
                                         request.allMachines, request.revision, request.now);
        case ManagerAction::Create:
        case ManagerAction::Register:
        case ManagerAction::ForgetCatalog:
        case ManagerAction::AttachStorage:
        case ManagerAction::DetachStorage:
        case ManagerAction::AttachNetwork:
        case ManagerAction::DetachNetwork:
            break;
        }
    }
    return invalidPlan(QStringLiteral("The requested provider operation is unsupported."));
}

QList<Snapshot> NativeVmLabProviderAdapter::parseSnapshots(
    const QString &providerId,
    const QByteArray &standardOutput,
    QString *error) const
{
    if (providerId == virtualBoxProviderId())
        return VirtualBoxProvider::parseSnapshotList(standardOutput, error);
    if (providerId == vmwareWorkstationProviderId()
        || providerId == vmwarePlayerProviderId()) {
        return VmwareProvider::parseSnapshotList(standardOutput, error);
    }
    if (error)
        *error = QStringLiteral("Unknown provider snapshot output cannot be parsed.");
    return {};
}

struct VmLabManager::AsyncResult
{
    enum class Kind { Detect, Inventory, Snapshots, Execute };

    Kind kind = Kind::Detect;
    bool success = false;
    bool cancelled = false;
    QString error;
    QList<ProviderInfo> providers;
    InventoryRefreshResult inventory;
    QList<Snapshot> snapshots;
    std::optional<VmRef> snapshotTarget;
    Result execution;
    OperationRequest request;
    Plan plan;
    OperationEvidence evidence;
};

struct VmLabManager::Private
{
    Private(QString catalogPath,
            QString root,
            std::shared_ptr<VmLabProviderAdapter> adapter,
            std::shared_ptr<CommandRunnerFactory> factory)
        : catalog(std::move(catalogPath)), managedRoot(std::move(root)),
          providerAdapter(std::move(adapter)), runnerFactory(std::move(factory))
    {
    }

    Catalog catalog;
    QString managedRoot;
    std::shared_ptr<VmLabProviderAdapter> providerAdapter;
    std::shared_ptr<CommandRunnerFactory> runnerFactory;
    ManagerState state = ManagerState::Idle;
    QString lastError;
    QList<ProviderProbePaths> candidates;
    QList<ProviderInfo> providers;
    QList<Machine> machines;
    QList<Snapshot> snapshots;
    QList<OperationEvidence> history;
    std::optional<VmRef> selected;
    std::optional<Plan> reviewed;
    std::optional<OperationRequest> reviewedRequest;
    bool loaded = false;
    bool autoRefresh = true;
    std::jthread worker;
    mutable std::mutex runnerMutex;
    std::shared_ptr<CancelableCommandRunner> activeRunner;
};

VmLabManager::VmLabManager(
    QString catalogPath,
    QString managedRoot,
    std::shared_ptr<VmLabProviderAdapter> providerAdapter,
    std::shared_ptr<CommandRunnerFactory> runnerFactory,
    QObject *parent)
    : QObject(parent),
      d(std::make_unique<Private>(
          std::move(catalogPath), std::move(managedRoot),
          providerAdapter ? std::move(providerAdapter)
                          : std::make_shared<NativeVmLabProviderAdapter>(),
          runnerFactory ? std::move(runnerFactory)
                        : std::make_shared<ProcessCommandRunnerFactory>()))
{
    qRegisterMetaType<ManagerState>();
    qRegisterMetaType<OperationEvidence>();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.lifecycle.created"),
        QStringLiteral("VM Lab manager was created. / VM Lab 管理員已經建立。"),
        QJsonObject{{QStringLiteral("state"), managerStateName(d->state)}});
}

VmLabManager::~VmLabManager()
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.lifecycle.stopping"),
        QStringLiteral("VM Lab manager is stopping. / VM Lab 管理員而家停止。"),
        QJsonObject{{QStringLiteral("state"), managerStateName(d->state)},
                    {QStringLiteral("busy"), busy()}});
    {
        const std::lock_guard lock(d->runnerMutex);
        if (d->activeRunner)
            d->activeRunner->requestCancel();
    }
    if (d->worker.joinable())
        d->worker.join();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.lifecycle.stopped"),
        QStringLiteral("VM Lab manager stopped. / VM Lab 管理員已經停止。"));
}

ManagerState VmLabManager::state() const { return d->state; }

bool VmLabManager::busy() const
{
    return d->state == ManagerState::DetectingProviders
        || d->state == ManagerState::RefreshingInventory
        || d->state == ManagerState::RefreshingSnapshots
        || d->state == ManagerState::Executing
        || d->state == ManagerState::Cancelling;
}

QString VmLabManager::lastError() const { return d->lastError; }
QString VmLabManager::managedRoot() const { return d->managedRoot; }
QList<ProviderInfo> VmLabManager::providers() const { return d->providers; }
QList<Machine> VmLabManager::machines() const { return d->machines; }
QList<Snapshot> VmLabManager::snapshots() const { return d->snapshots; }
QList<OperationEvidence> VmLabManager::evidenceHistory() const { return d->history; }

std::optional<Machine> VmLabManager::selectedMachine() const
{
    if (!d->selected)
        return std::nullopt;
    const auto found = std::find_if(d->machines.cbegin(), d->machines.cend(),
                                    [this](const Machine &machine) {
        return machine.ref.providerId == d->selected->providerId
            && machine.ref.id == d->selected->id;
    });
    return found == d->machines.cend() ? std::nullopt : std::optional<Machine>(*found);
}

std::optional<Plan> VmLabManager::reviewedPlan() const { return d->reviewed; }

void VmLabManager::setProbeCandidates(QList<ProviderProbePaths> candidates)
{
    if (busy()) {
        setError(QStringLiteral("Provider probes cannot change while a VM task is running."));
        return;
    }
    d->candidates = std::move(candidates);
}

void VmLabManager::setAutoRefresh(bool enabled) { d->autoRefresh = enabled; }
bool VmLabManager::autoRefresh() const { return d->autoRefresh; }

bool VmLabManager::load(QString *error)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.catalog.load.started"),
        QStringLiteral("VM Lab catalog loading started. / VM Lab 目錄載入已經開始。"),
        QJsonObject{{QStringLiteral("busy"), busy()},
                    {QStringLiteral("managedRootAbsolute"),
                     QFileInfo(d->managedRoot).isAbsolute()}});
    if (busy()) {
        const QString message = QStringLiteral("The VM manager is busy.");
        if (error)
            *error = message;
        setError(message);
        return false;
    }
    const QFileInfo root(d->managedRoot);
    if (!root.isAbsolute()) {
        const QString message = QStringLiteral("Managed VM root must be absolute.");
        if (error)
            *error = message;
        setError(message);
        return false;
    }
    if (!root.exists() && !QDir().mkpath(root.absoluteFilePath())) {
        const QString message = QStringLiteral("Could not create the managed VM root.");
        if (error)
            *error = message;
        setError(message);
        return false;
    }
    if (!QFileInfo(d->managedRoot).isDir()) {
        const QString message = QStringLiteral("Managed VM root is not a directory.");
        if (error)
            *error = message;
        setError(message);
        return false;
    }
    QString catalogError;
    if (!d->catalog.load(&catalogError)) {
        if (error)
            *error = catalogError;
        setError(catalogError);
        return false;
    }
    d->machines.clear();
    for (const Machine &machine : d->catalog.machines())
        d->machines.append(staleMachine(machine, QStringLiteral("Refresh provider inventory.")));
    d->selected.reset();
    d->snapshots.clear();
    clearReviewedPlan();
    d->lastError.clear();
    d->loaded = true;
    setState(ManagerState::Idle);
    emit machinesChanged();
    emit selectionChanged();
    emit snapshotsChanged();
    if (error)
        error->clear();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.catalog.load.completed"),
        QStringLiteral("VM Lab catalog loaded. / VM Lab 目錄已經載入。"),
        QJsonObject{{QStringLiteral("machineCount"), d->machines.size()}});
    return true;
}

bool VmLabManager::selectMachine(const QString &providerId, const QString &id)
{
    StructuredLogger::instance().log(
        LogSeverity::Debug, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.selection.requested"),
        QStringLiteral("A VM selection was requested. / 已經要求選取一部 VM。"),
        QJsonObject{{QStringLiteral("busy"), busy()}});
    if (busy()) {
        setError(QStringLiteral("Wait for the current VM Lab task before changing selection."));
        return false;
    }
    const auto found = std::find_if(d->machines.cbegin(), d->machines.cend(),
                                    [&providerId, &id](const Machine &machine) {
        return machine.ref.providerId == providerId && machine.ref.id == id;
    });
    if (found == d->machines.cend()) {
        setError(QStringLiteral("The requested VM is absent from current inventory."));
        return false;
    }
    if (d->selected && d->selected->providerId == providerId && d->selected->id == id)
        return true;
    d->selected = found->ref;
    d->snapshots.clear();
    clearReviewedPlan();
    emit selectionChanged();
    emit snapshotsChanged();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.selection.changed"),
        QStringLiteral("The selected VM changed. / 已選 VM 已經改變。"));
    return true;
}

void VmLabManager::clearSelection()
{
    if (!d->selected)
        return;
    d->selected.reset();
    d->snapshots.clear();
    clearReviewedPlan();
    emit selectionChanged();
    emit snapshotsChanged();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.selection.cleared"),
        QStringLiteral("The VM selection was cleared. / VM 選取已經清除。"));
}

bool VmLabManager::startAsync(const QString &action,
                              ManagerState stateValue,
                              AsyncWork work)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.action.requested"),
        QStringLiteral("A VM Lab action was requested. / 已經要求執行 VM Lab 動作。"),
        QJsonObject{{QStringLiteral("action"), action},
                    {QStringLiteral("requestedState"),
                     managerStateName(stateValue)},
                    {QStringLiteral("busy"), busy()}});
    if (busy()) {
        setError(QStringLiteral("A VM Lab task is already running."));
        return false;
    }
    if (!d->runnerFactory || !d->providerAdapter) {
        setError(QStringLiteral("VM Lab execution dependencies are unavailable."));
        return false;
    }
    if (d->worker.joinable())
        d->worker.join();
    const std::shared_ptr<CancelableCommandRunner> runner = d->runnerFactory->create();
    if (!runner) {
        setError(QStringLiteral("VM Lab command runner creation failed."));
        return false;
    }
    {
        const std::lock_guard lock(d->runnerMutex);
        d->activeRunner = runner;
    }
    d->lastError.clear();
    setState(stateValue);
    const QUuid evidenceId = QUuid::createUuid();
    const QDateTime startedAt = QDateTime::currentDateTimeUtc();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.action.started"),
        QStringLiteral("A VM Lab action started. / VM Lab 動作已經開始。"),
        QJsonObject{{QStringLiteral("action"), action},
                    {QStringLiteral("evidenceId"),
                     evidenceId.toString(QUuid::WithoutBraces)},
                    {QStringLiteral("state"), managerStateName(stateValue)}});
    d->worker = std::jthread(
        [this, runner, work = std::move(work), action, evidenceId, startedAt] {
            RecordingRunner recording(*runner);
            std::shared_ptr<AsyncResult> result;
            try {
                result = work(recording);
            } catch (const std::exception &exception) {
                result = std::make_shared<AsyncResult>();
                result->error = QStringLiteral("VM task failed unexpectedly: %1")
                                    .arg(QString::fromUtf8(exception.what()));
            } catch (...) {
                result = std::make_shared<AsyncResult>();
                result->error = QStringLiteral("VM task failed with an unknown exception.");
            }
            if (!result)
                result = std::make_shared<AsyncResult>();
            const QList<CommandTranscript> transcripts = recording.transcripts();
            const bool cancellationObserved = result->error.contains(
                QStringLiteral("cancel"), Qt::CaseInsensitive)
                || std::any_of(transcripts.cbegin(), transcripts.cend(),
                               [](const CommandTranscript &transcript) {
                    return transcript.result.error.contains(
                        QStringLiteral("cancel"), Qt::CaseInsensitive);
                });
            result->cancelled = runner->cancellationRequested()
                && cancellationObserved;
            if (result->cancelled) {
                result->success = false;
                if (result->error.isEmpty())
                    result->error = QStringLiteral("VM operation was cancelled.");
            }
            result->evidence.id = evidenceId;
            result->evidence.action = action;
            result->evidence.startedAt = startedAt;
            result->evidence.finishedAt = QDateTime::currentDateTimeUtc();
            result->evidence.success = result->success;
            result->evidence.cancelled = result->cancelled;
            result->evidence.error = result->error;
            result->evidence.commands = transcripts;
            result->evidence.files = result->execution.verifiedFiles;
            QMetaObject::invokeMethod(
                this, [this, result] { finishAsync(result); }, Qt::QueuedConnection);
        });
    return true;
}

bool VmLabManager::detectProviders()
{
    QList<ProviderProbePaths> candidates = d->candidates;
    if (candidates.isEmpty())
        candidates = ProviderDetector::defaultWindowsCandidates();
    const std::shared_ptr<VmLabProviderAdapter> adapter = d->providerAdapter;
    return startAsync(
        QStringLiteral("detect-providers"), ManagerState::DetectingProviders,
        [adapter, candidates](CommandRunner &runner) {
            auto result = std::make_shared<AsyncResult>();
            result->kind = AsyncResult::Kind::Detect;
            result->providers = adapter->detect(candidates, runner);
            result->success = true;
            return result;
        });
}

bool VmLabManager::refreshInventory()
{
    if (d->providers.isEmpty()) {
        setError(QStringLiteral("Detect VM providers before refreshing inventory."));
        return false;
    }
    const QList<ProviderInfo> providers = d->providers;
    const QList<Machine> catalog = d->catalog.machines();
    const std::shared_ptr<VmLabProviderAdapter> adapter = d->providerAdapter;
    return startAsync(
        QStringLiteral("refresh-inventory"), ManagerState::RefreshingInventory,
        [adapter, providers, catalog](CommandRunner &runner) {
            auto result = std::make_shared<AsyncResult>();
            result->kind = AsyncResult::Kind::Inventory;
            result->inventory = adapter->refreshInventory(providers, catalog, runner);
            result->success = result->inventory.success;
            result->error = result->inventory.error;
            return result;
        });
}

bool VmLabManager::refreshSnapshots()
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before refreshing snapshots."));
        return false;
    }
    const std::optional<ProviderInfo> selectedProvider = provider(selected->ref.providerId);
    if (!selectedProvider) {
        setError(QStringLiteral("The selected VM provider is unavailable."));
        return false;
    }
    OperationRequest request;
    request.action = ManagerAction::ListSnapshots;
    request.machine = selected;
    request.managedRoot = d->managedRoot;
    request.allMachines = d->machines;
    request.revision = d->catalog.revision();
    request.now = QDateTime::currentDateTimeUtc();
    const Plan plan = d->providerAdapter->plan(*selectedProvider, request);
    QString validationError;
    if (!Executor::validate(plan, request.revision, {}, request.now, &validationError)) {
        setError(validationError);
        return false;
    }
    const std::shared_ptr<VmLabProviderAdapter> adapter = d->providerAdapter;
    const QString providerId = selected->ref.providerId;
    return startAsync(
        QStringLiteral("list-snapshots"), ManagerState::RefreshingSnapshots,
        [adapter, providerId, plan, request](CommandRunner &runner) {
            auto result = std::make_shared<AsyncResult>();
            result->kind = AsyncResult::Kind::Snapshots;
            result->snapshotTarget = request.machine->ref;
            result->execution = Executor::execute(
                plan, request.revision, {}, request.now, runner);
            if (!result->execution.success) {
                result->error = result->execution.error;
                return result;
            }
            if (result->execution.processes.isEmpty()) {
                result->error = QStringLiteral("Provider returned no snapshot inventory evidence.");
                return result;
            }
            QString parseError;
            result->snapshots = adapter->parseSnapshots(
                providerId, result->execution.processes.constLast().standardOutput, &parseError);
            const QString snapshotRevision = commandEvidence(
                EvidenceFormat::RawSha256,
                result->execution.processes.constLast().standardOutput, &parseError);
            for (Snapshot &snapshot : result->snapshots)
                snapshot.inventoryRevision = snapshotRevision;
            result->success = parseError.isEmpty();
            result->error = parseError;
            return result;
        });
}

bool VmLabManager::cancel()
{
    if (!busy() || d->state == ManagerState::Cancelling)
        return false;
    std::shared_ptr<CancelableCommandRunner> runner;
    {
        const std::lock_guard lock(d->runnerMutex);
        runner = d->activeRunner;
    }
    if (!runner)
        return false;
    runner->requestCancel();
    setState(ManagerState::Cancelling);
    StructuredLogger::instance().log(
        LogSeverity::Warning, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.action.cancel-requested"),
        QStringLiteral("Cancellation was requested for the VM Lab action. / 已經要求取消 VM Lab 動作。"));
    return true;
}

void VmLabManager::finishAsync(const std::shared_ptr<AsyncResult> &result)
{
    {
        const std::lock_guard lock(d->runnerMutex);
        d->activeRunner.reset();
    }
    bool queueRefresh = false;
    bool selectionChangedValue = false;
    bool snapshotsChangedValue = false;

    if (result->kind == AsyncResult::Kind::Detect && result->success) {
        d->providers = result->providers;
        emit providersChanged();
        queueRefresh = d->autoRefresh
            && std::any_of(d->providers.cbegin(), d->providers.cend(),
                           [](const ProviderInfo &provider) { return provider.available; });
    } else if (result->kind == AsyncResult::Kind::Inventory && result->success) {
        d->machines = result->inventory.machines;
        if (d->selected) {
            const bool selectedStillExists = std::any_of(
                d->machines.cbegin(), d->machines.cend(), [this](const Machine &machine) {
                    return machine.ref.providerId == d->selected->providerId
                        && machine.ref.id == d->selected->id;
                });
            if (!selectedStillExists) {
                d->selected.reset();
                d->snapshots.clear();
                selectionChangedValue = true;
                snapshotsChangedValue = true;
            }
        }
        clearReviewedPlan();
        emit machinesChanged();
    } else if (result->kind == AsyncResult::Kind::Snapshots && result->success) {
        if (d->selected && result->snapshotTarget
            && d->selected->providerId == result->snapshotTarget->providerId
            && d->selected->id == result->snapshotTarget->id) {
            d->snapshots = result->snapshots;
            snapshotsChangedValue = true;
        }
    } else if (result->kind == AsyncResult::Kind::Execute) {
        clearReviewedPlan();
        if (result->success) {
            QString catalogError;
            if (result->request.action == ManagerAction::Create
                || result->request.action == ManagerAction::Register) {
                Machine machine;
                machine.ref = result->plan.preview.target;
                machine.ownership = result->request.ownership;
                machine.ownershipToken = result->plan.managedOwnershipToken;
                if (result->request.action == ManagerAction::Register) {
                    machine.configPath = QFileInfo(result->request.path).absoluteFilePath();
                } else if (result->request.createSpec.providerId == virtualBoxProviderId()) {
                    machine.configPath = QDir(result->request.createSpec.directory)
                                             .filePath(result->request.createSpec.name
                                                       + QLatin1Char('/')
                                                       + result->request.createSpec.name
                                                       + QStringLiteral(".vbox"));
                } else {
                    machine.configPath = QDir(result->request.createSpec.directory)
                                             .filePath(result->request.createSpec.name
                                                       + QStringLiteral(".vmx"));
                }
                if (!d->catalog.upsert(machine, &catalogError)
                    || !d->catalog.save(&catalogError)) {
                    result->success = false;
                    result->error = QStringLiteral(
                        "Provider operation succeeded, but the VM catalog update failed: %1")
                                        .arg(catalogError);
                    d->catalog.load(nullptr);
                }
            } else if (result->request.action == ManagerAction::Delete
                       || result->request.action == ManagerAction::Unregister
                       || result->request.action == ManagerAction::ForgetCatalog) {
                if (result->request.machine
                    && d->catalog.remove(result->request.machine->ref)
                    && !d->catalog.save(&catalogError)) {
                    result->success = false;
                    result->error = QStringLiteral(
                        "Provider operation succeeded, but the VM catalog update failed: %1")
                                        .arg(catalogError);
                    d->catalog.load(nullptr);
                }
                if (result->request.machine && d->selected
                    && result->request.machine->ref.providerId == d->selected->providerId
                    && result->request.machine->ref.id == d->selected->id) {
                    d->selected.reset();
                    d->snapshots.clear();
                    selectionChangedValue = true;
                    snapshotsChangedValue = true;
                }
            }
            queueRefresh = result->success && d->autoRefresh && !d->providers.isEmpty();
        }
    }

    if (result->kind == AsyncResult::Kind::Execute && !result->success
        && d->autoRefresh && !d->providers.isEmpty()
        && std::any_of(result->evidence.commands.cbegin(), result->evidence.commands.cend(),
                       [](const CommandTranscript &transcript) {
            return transcript.result.started;
        })) {
        queueRefresh = true;
    }

    if (result->kind == AsyncResult::Kind::Execute && d->catalog.transactionActive())
        d->catalog.endTransaction();

    if (selectionChangedValue)
        emit selectionChanged();
    if (snapshotsChangedValue)
        emit snapshotsChanged();
    if (result->success) {
        d->lastError.clear();
        setState(ManagerState::Idle);
    } else {
        d->lastError = result->error.isEmpty()
            ? QStringLiteral("VM Lab task failed.") : result->error;
        result->evidence.error = d->lastError;
        setState(ManagerState::Error);
        emit errorOccurred(d->lastError);
    }
    result->evidence.success = result->success;
    result->evidence.cancelled = result->cancelled;
    result->evidence.error = d->lastError;
    d->history.append(result->evidence);
    while (d->history.size() > 100)
        d->history.removeFirst();
    emit evidenceAdded(result->evidence);
    emit taskFinished(result->evidence);

    StructuredLogger::instance().log(
        result->success ? LogSeverity::Info
                        : (result->cancelled ? LogSeverity::Warning
                                             : LogSeverity::Error),
        QStringLiteral("vmlab"), QStringLiteral("vmlab.action.completed"),
        result->success
            ? QStringLiteral("The VM Lab action completed. / VM Lab 動作已經完成。")
            : (result->cancelled
                   ? QStringLiteral("The VM Lab action was cancelled. / VM Lab 動作已經取消。")
                   : QStringLiteral("The VM Lab action failed. / VM Lab 動作失敗。")),
        QJsonObject{
            {QStringLiteral("action"), result->evidence.action},
            {QStringLiteral("evidenceId"),
             result->evidence.id.toString(QUuid::WithoutBraces)},
            {QStringLiteral("success"), result->success},
            {QStringLiteral("cancelled"), result->cancelled},
            {QStringLiteral("errorPresent"), !d->lastError.isEmpty()},
            {QStringLiteral("durationMs"),
             result->evidence.startedAt.msecsTo(result->evidence.finishedAt)},
            {QStringLiteral("providerCommandCount"),
             result->evidence.commands.size()},
            {QStringLiteral("verifiedFileCount"),
             result->evidence.files.size()},
        });

    if (queueRefresh) {
        QMetaObject::invokeMethod(
            this, [this] { refreshInventory(); }, Qt::QueuedConnection);
    }
}

void VmLabManager::setState(ManagerState stateValue)
{
    if (d->state == stateValue)
        return;
    d->state = stateValue;
    emit stateChanged(stateValue);
}

void VmLabManager::setError(const QString &error)
{
    d->lastError = error;
    if (!error.isEmpty()) {
        StructuredLogger::instance().log(
            LogSeverity::Warning, QStringLiteral("vmlab"),
            QStringLiteral("vmlab.request.rejected"),
            QStringLiteral("A VM Lab request was rejected; diagnostic contents were omitted from logging. / VM Lab 要求被拒絕；記錄已經省略診斷內容。"),
            QJsonObject{{QStringLiteral("state"), managerStateName(d->state)},
                        {QStringLiteral("diagnosticPresent"), true}});
        emit errorOccurred(error);
    }
}

std::optional<ProviderInfo> VmLabManager::provider(const QString &id) const
{
    const auto found = std::find_if(d->providers.cbegin(), d->providers.cend(),
                                    [&id](const ProviderInfo &provider) {
        return provider.id == id;
    });
    return found == d->providers.cend()
        ? std::nullopt : std::optional<ProviderInfo>(*found);
}

std::optional<OperationPreview> VmLabManager::review(OperationRequest request)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.review.requested"),
        QStringLiteral("A VM Lab operation review was requested. / 已經要求檢視 VM Lab 操作。"),
        QJsonObject{{QStringLiteral("action"), managerActionName(request.action)},
                    {QStringLiteral("loaded"), d->loaded},
                    {QStringLiteral("busy"), busy()}});
    if (!d->loaded) {
        setError(QStringLiteral("Load the VM catalog before reviewing operations."));
        return std::nullopt;
    }
    if (busy()) {
        setError(QStringLiteral("Wait for the active VM task before reviewing another operation."));
        return std::nullopt;
    }
    const QString providerId = request.action == ManagerAction::Create
        ? request.createSpec.providerId
        : request.action == ManagerAction::Register
            ? (request.machine ? request.machine->ref.providerId : QString{})
            : request.machine ? request.machine->ref.providerId : QString{};
    request.managedRoot = d->managedRoot;
    request.allMachines = d->machines;
    request.revision = d->catalog.revision();
    request.now = QDateTime::currentDateTimeUtc();
    Plan plan;
    if (request.action == ManagerAction::ForgetCatalog && request.machine) {
        plan = makeForgetCatalogPlan(*request.machine, request.revision, request.now);
    } else {
        const std::optional<ProviderInfo> info = provider(providerId);
        if (!info) {
            setError(QStringLiteral("The requested VM provider has not been detected."));
            return std::nullopt;
        }
        plan = d->providerAdapter->plan(*info, request);
    }
    QString validationError;
    if (!Executor::validate(plan, request.revision, plan.preview.confirmation,
                            request.now, &validationError)) {
        setError(validationError);
        return std::nullopt;
    }
    d->reviewed = plan;
    d->reviewedRequest = request;
    d->lastError.clear();
    emit reviewedPlanChanged();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.review.completed"),
        QStringLiteral("The VM Lab operation preview is ready. / VM Lab 操作預覽已經準備好。"),
        QJsonObject{{QStringLiteral("action"), managerActionName(request.action)},
                    {QStringLiteral("previewId"),
                     plan.preview.id.toString(QUuid::WithoutBraces)},
                    {QStringLiteral("risk"), riskName(plan.preview.risk)},
                    {QStringLiteral("providerCommandCount"),
                     plan.preview.commands.size()},
                    {QStringLiteral("warningCount"),
                     plan.preview.warnings.size()},
                    {QStringLiteral("confirmationRequired"),
                     !plan.preview.confirmation.isEmpty()}});
    return plan.preview;
}

std::optional<OperationPreview> VmLabManager::reviewSelected(ManagerAction action)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before reviewing this operation."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = action;
    request.machine = selected;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewCreate(
    CreateSpec spec, Ownership ownership)
{
    if (spec.providerId == virtualBoxProviderId() && QUuid(spec.id).isNull())
        spec.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (spec.directory.trimmed().isEmpty()) {
        spec.directory = spec.providerId == virtualBoxProviderId()
            ? d->managedRoot : QDir(d->managedRoot).filePath(spec.name);
    }
    OperationRequest request;
    request.action = ManagerAction::Create;
    request.createSpec = std::move(spec);
    request.ownership = ownership;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewRegister(
    const QString &providerId,
    const QString &configurationPath,
    const QString &displayName,
    Ownership ownership)
{
    OperationRequest request;
    request.action = ManagerAction::Register;
    Machine machine;
    machine.ref = VmRef{providerId, configurationPath,
                        displayName.trimmed().isEmpty()
                            ? QFileInfo(configurationPath).completeBaseName()
                            : displayName};
    request.machine = std::move(machine);
    request.path = configurationPath;
    request.name = displayName;
    request.ownership = ownership;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewOpenConsole()
{
    return reviewSelected(ManagerAction::OpenConsole);
}

std::optional<OperationPreview> VmLabManager::reviewStart(bool headless)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before reviewing start."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::Start;
    request.machine = selected;
    request.headless = headless;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewGracefulShutdown()
{
    return reviewSelected(ManagerAction::GracefulShutdown);
}

std::optional<OperationPreview> VmLabManager::reviewPowerOff()
{
    return reviewSelected(ManagerAction::PowerOff);
}

std::optional<OperationPreview> VmLabManager::reviewPause()
{
    return reviewSelected(ManagerAction::Pause);
}

std::optional<OperationPreview> VmLabManager::reviewResume()
{
    return reviewSelected(ManagerAction::Resume);
}

std::optional<OperationPreview> VmLabManager::reviewReset()
{
    return reviewSelected(ManagerAction::Reset);
}

std::optional<OperationPreview> VmLabManager::reviewSaveState()
{
    return reviewSelected(ManagerAction::SaveState);
}

std::optional<OperationPreview> VmLabManager::reviewConfigure(const ConfigPatch &patch)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before reviewing configuration."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::Configure;
    request.machine = selected;
    request.configPatch = patch;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewAttachIso(const QString &isoPath)
{
    const QFileInfo iso(isoPath);
    if (!iso.isAbsolute() || !iso.exists() || !iso.isFile()
        || iso.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) != 0) {
        setError(QStringLiteral("ISO attachment requires an existing absolute ISO file."));
        return std::nullopt;
    }
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before attaching an ISO."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::AttachIso;
    request.machine = selected;
    request.path = iso.absoluteFilePath();
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewDetachIso()
{
    return reviewSelected(ManagerAction::DetachIso);
}

std::optional<OperationPreview> VmLabManager::reviewAttachStorage(
    const StorageDeviceSpec &storage)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before attaching storage."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::AttachStorage;
    request.machine = selected;
    request.storage = storage;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewDetachStorage(
    const StorageDeviceSpec &storage)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before detaching storage."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::DetachStorage;
    request.machine = selected;
    request.storage = storage;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewAttachNetwork(
    const NetworkAdapterSpec &network)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before attaching networking."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::AttachNetwork;
    request.machine = selected;
    request.network = network;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewDetachNetwork(int slot)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before detaching networking."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::DetachNetwork;
    request.machine = selected;
    request.network.slot = slot;
    request.network.mode = NetworkMode::Disconnected;
    request.network.connected = false;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewTakeSnapshot(
    const QString &name, const QString &description)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before taking a snapshot."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::TakeSnapshot;
    request.machine = selected;
    request.name = name;
    request.description = description;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewRestoreSnapshot(
    const Snapshot &snapshot)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before restoring a snapshot."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::RestoreSnapshot;
    request.machine = selected;
    request.snapshot = snapshot;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewDeleteSnapshot(
    const Snapshot &snapshot)
{
    const std::optional<Machine> selected = selectedMachine();
    if (!selected) {
        setError(QStringLiteral("Select a VM before deleting a snapshot."));
        return std::nullopt;
    }
    OperationRequest request;
    request.action = ManagerAction::DeleteSnapshot;
    request.machine = selected;
    request.snapshot = snapshot;
    return review(request);
}

std::optional<OperationPreview> VmLabManager::reviewUnregister()
{
    return reviewSelected(ManagerAction::Unregister);
}

std::optional<OperationPreview> VmLabManager::reviewForgetCatalog()
{
    return reviewSelected(ManagerAction::ForgetCatalog);
}

std::optional<OperationPreview> VmLabManager::reviewDelete()
{
    return reviewSelected(ManagerAction::Delete);
}

bool VmLabManager::executeReviewed(const QUuid &previewId,
                                   const QString &typedConfirmation)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("vmlab"),
        QStringLiteral("vmlab.execution.requested"),
        QStringLiteral("Execution of a reviewed VM Lab operation was requested. / 已經要求執行檢視過嘅 VM Lab 操作。"),
        QJsonObject{{QStringLiteral("previewIdPresent"), !previewId.isNull()},
                    {QStringLiteral("confirmationProvided"),
                     !typedConfirmation.isEmpty()},
                    {QStringLiteral("reviewedPlanPresent"), d->reviewed.has_value()}});
    if (!d->reviewed || !d->reviewedRequest
        || previewId.isNull() || d->reviewed->preview.id != previewId) {
        setError(QStringLiteral("Execute only the exact operation preview currently under review."));
        return false;
    }
    QString transactionError;
    if (!d->catalog.beginTransaction(d->reviewed->preview.revision,
                                     &transactionError)) {
        setError(transactionError);
        return false;
    }
    const QString currentRevision = d->catalog.revision();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QString validationError;
    if (!Executor::validate(*d->reviewed, currentRevision,
                            typedConfirmation, now, &validationError)) {
        d->catalog.endTransaction();
        setError(validationError);
        return false;
    }
    Plan plan = *d->reviewed;
    OperationRequest request = *d->reviewedRequest;
    const std::shared_ptr<VmLabProviderAdapter> adapter = d->providerAdapter;
    const QList<ProviderInfo> providers = d->providers;
    const QList<Machine> catalogMachines = d->catalog.machines();
    const bool started = startAsync(
        managerActionName(request.action), ManagerState::Executing,
        [plan, request, currentRevision, typedConfirmation, now,
         adapter, providers, catalogMachines](CommandRunner &runner) mutable {
            auto result = std::make_shared<AsyncResult>();
            result->kind = AsyncResult::Kind::Execute;
            if (request.action == ManagerAction::Delete
                && plan.managedDeletionAfterCommands) {
                const InventoryRefreshResult refreshed = adapter->refreshInventory(
                    providers, catalogMachines, runner);
                if (!refreshed.success || !refreshed.complete) {
                    result->error = refreshed.error.isEmpty()
                        ? QStringLiteral(
                              "Managed deletion requires a complete live inventory refresh immediately before execution.")
                        : refreshed.error;
                    return result;
                }
                const auto current = std::find_if(
                    refreshed.machines.cbegin(), refreshed.machines.cend(),
                    [&request](const Machine &machine) {
                        return request.machine
                            && machine.ref.providerId == request.machine->ref.providerId
                            && machine.ref.id == request.machine->ref.id;
                    });
                if (current == refreshed.machines.cend()) {
                    result->error = QStringLiteral(
                        "Managed deletion target disappeared during the required live refresh.");
                    return result;
                }
                request.machine = *current;
                request.allMachines = refreshed.machines;
                plan.managedDeletionAfterCommands->machine = *current;
                plan.managedDeletionAfterCommands->catalogMachines = refreshed.machines;
            }
            result->plan = plan;
            result->request = request;
            Executor::ManagedInventoryRefresh postCommandInventory;
            if (plan.managedDeletionAfterCommands) {
                QList<Machine> inventoryCatalog = catalogMachines;
                const ManagedDeletion deletion = *plan.managedDeletionAfterCommands;
                if (deletion.expectTargetAbsentAfterCommands) {
                    inventoryCatalog.erase(
                        std::remove_if(
                            inventoryCatalog.begin(), inventoryCatalog.end(),
                            [&deletion](const Machine &machine) {
                                return machine.ref.providerId == deletion.machine.ref.providerId
                                    && machine.ref.id == deletion.machine.ref.id;
                            }),
                        inventoryCatalog.end());
                }
                postCommandInventory = [adapter, providers, inventoryCatalog](
                                           CommandRunner &refreshRunner,
                                           QList<Machine> *machines,
                                           QString *error) {
                    try {
                        const InventoryRefreshResult refreshed = adapter->refreshInventory(
                            providers, inventoryCatalog, refreshRunner);
                        if (!refreshed.success || !refreshed.complete) {
                            if (error) {
                                *error = refreshed.error.isEmpty()
                                    ? QStringLiteral(
                                          "Post-command provider inventory was incomplete; managed files were preserved.")
                                    : refreshed.error;
                            }
                            return false;
                        }
                        if (machines)
                            *machines = refreshed.machines;
                        if (error)
                            error->clear();
                        return true;
                    } catch (const std::exception &exception) {
                        if (error) {
                            *error = QStringLiteral("Post-command provider inventory failed: %1")
                                         .arg(QString::fromUtf8(exception.what()).left(1024));
                        }
                    } catch (...) {
                        if (error)
                            *error = QStringLiteral(
                                "Post-command provider inventory failed unexpectedly.");
                    }
                    return false;
                };
            }
            result->execution = Executor::execute(
                plan, currentRevision, typedConfirmation, now, runner,
                postCommandInventory);
            result->success = result->execution.success;
            result->error = result->execution.error;
            return result;
        });
    if (!started)
        d->catalog.endTransaction();
    return started;
}

void VmLabManager::clearReviewedPlan()
{
    if (!d->reviewed && !d->reviewedRequest)
        return;
    d->reviewed.reset();
    d->reviewedRequest.reset();
    emit reviewedPlanChanged();
}

QString managerStateName(ManagerState state)
{
    switch (state) {
    case ManagerState::Idle: return QStringLiteral("idle");
    case ManagerState::DetectingProviders: return QStringLiteral("detecting-providers");
    case ManagerState::RefreshingInventory: return QStringLiteral("refreshing-inventory");
    case ManagerState::RefreshingSnapshots: return QStringLiteral("refreshing-snapshots");
    case ManagerState::Executing: return QStringLiteral("executing");
    case ManagerState::Cancelling: return QStringLiteral("cancelling");
    case ManagerState::Error: return QStringLiteral("error");
    }
    return QStringLiteral("unknown");
}

QString managerActionName(ManagerAction action)
{
    switch (action) {
    case ManagerAction::Create: return QStringLiteral("create");
    case ManagerAction::Register: return QStringLiteral("register");
    case ManagerAction::OpenConsole: return QStringLiteral("open-console");
    case ManagerAction::Start: return QStringLiteral("start");
    case ManagerAction::GracefulShutdown: return QStringLiteral("graceful-shutdown");
    case ManagerAction::PowerOff: return QStringLiteral("power-off");
    case ManagerAction::Pause: return QStringLiteral("pause");
    case ManagerAction::Resume: return QStringLiteral("resume");
    case ManagerAction::Reset: return QStringLiteral("reset");
    case ManagerAction::SaveState: return QStringLiteral("save-state");
    case ManagerAction::Configure: return QStringLiteral("configure");
    case ManagerAction::AttachIso: return QStringLiteral("attach-iso");
    case ManagerAction::DetachIso: return QStringLiteral("detach-iso");
    case ManagerAction::AttachStorage: return QStringLiteral("attach-storage");
    case ManagerAction::DetachStorage: return QStringLiteral("detach-storage");
    case ManagerAction::AttachNetwork: return QStringLiteral("attach-network");
    case ManagerAction::DetachNetwork: return QStringLiteral("detach-network");
    case ManagerAction::ListSnapshots: return QStringLiteral("list-snapshots");
    case ManagerAction::TakeSnapshot: return QStringLiteral("take-snapshot");
    case ManagerAction::RestoreSnapshot: return QStringLiteral("restore-snapshot");
    case ManagerAction::DeleteSnapshot: return QStringLiteral("delete-snapshot");
    case ManagerAction::ForgetCatalog: return QStringLiteral("forget");
    case ManagerAction::Unregister: return QStringLiteral("unregister");
    case ManagerAction::Delete: return QStringLiteral("delete");
    }
    return QStringLiteral("unknown");
}

QString storageBusName(StorageBus bus)
{
    switch (bus) {
    case StorageBus::Ide: return QStringLiteral("ide");
    case StorageBus::Sata: return QStringLiteral("sata");
    case StorageBus::Scsi: return QStringLiteral("scsi");
    case StorageBus::Nvme: return QStringLiteral("nvme");
    }
    return QStringLiteral("unknown");
}

} // namespace wimforge::vmlab
