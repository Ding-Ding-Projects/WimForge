#include "VmLabCliService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace wimforge::vmlab {

namespace {

constexpr int MaxEvidenceCommands = 64;
constexpr int MaxEvidenceBytes = 4096;
constexpr int MaxListItems = 1000;
constexpr int MaxShortText = 1024;

QJsonArray stringArray(const QStringList &values,
                       int maximumItems = MaxListItems,
                       int maximumCharacters = MaxShortText)
{
    QJsonArray result;
    const qsizetype count = std::min(values.size(), qsizetype(maximumItems));
    for (qsizetype index = 0; index < count; ++index)
        result.append(values.at(index).left(maximumCharacters));
    return result;
}

QJsonObject boundedBytes(const QByteArray &bytes)
{
    QJsonObject result;
    result.insert(QStringLiteral("byteCount"), static_cast<qint64>(bytes.size()));
    result.insert(QStringLiteral("sha256"), QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
    result.insert(QStringLiteral("truncated"), bytes.size() > MaxEvidenceBytes);
    result.insert(QStringLiteral("text"),
                  QString::fromLocal8Bit(bytes.left(MaxEvidenceBytes)));
    return result;
}

QJsonObject commandJson(const Command &command)
{
    QJsonArray arguments;
    for (const QString &argument : command.arguments)
        arguments.append(argument);
    return QJsonObject{
        {QStringLiteral("executable"), command.executable},
        {QStringLiteral("arguments"), arguments},
        {QStringLiteral("workingDirectory"), command.workingDirectory},
        {QStringLiteral("timeoutMs"), command.timeoutMs},
        {QStringLiteral("detached"), command.detached},
        {QStringLiteral("interruptible"), command.interruptible},
    };
}

QJsonObject providerJson(const ProviderInfo &provider)
{
    QStringList capabilities(provider.capabilities.cbegin(), provider.capabilities.cend());
    std::sort(capabilities.begin(), capabilities.end());
    return QJsonObject{
        {QStringLiteral("id"), provider.id},
        {QStringLiteral("displayName"), provider.displayName},
        {QStringLiteral("available"), provider.available},
        {QStringLiteral("version"), provider.version},
        {QStringLiteral("executable"), provider.executable},
        {QStringLiteral("consoleExecutable"), provider.consoleExecutable},
        {QStringLiteral("diskManagerExecutable"), provider.diskManagerExecutable},
        {QStringLiteral("capabilities"), stringArray(capabilities)},
        {QStringLiteral("evidence"), stringArray(provider.evidence, 32, 512)},
        {QStringLiteral("warnings"), stringArray(provider.warnings, 32, 512)},
    };
}

QJsonObject machineJson(const Machine &machine)
{
    QJsonArray storageDevices;
    for (const StorageAttachment &storage : machine.storageDevices) {
        storageDevices.append(QJsonObject{
            {QStringLiteral("id"), storage.id},
            {QStringLiteral("bus"), storage.bus},
            {QStringLiteral("controller"), storage.controller},
            {QStringLiteral("controllerName"), storage.controllerName},
            {QStringLiteral("port"), storage.port},
            {QStringLiteral("device"), storage.device},
            {QStringLiteral("path"), storage.path},
            {QStringLiteral("optical"), storage.optical},
        });
    }
    QJsonArray networkDevices;
    for (const NetworkAttachment &network : machine.networkDevices) {
        networkDevices.append(QJsonObject{
            {QStringLiteral("id"), network.id},
            {QStringLiteral("slot"), network.slot},
            {QStringLiteral("mode"), networkModeName(network.mode)},
            {QStringLiteral("interfaceName"), network.interfaceName},
            {QStringLiteral("model"), network.model},
            {QStringLiteral("macAddress"), network.macAddress},
            {QStringLiteral("connected"), network.connected},
        });
    }
    QJsonObject result{
        {QStringLiteral("providerId"), machine.ref.providerId},
        {QStringLiteral("id"), machine.ref.id},
        {QStringLiteral("name"), machine.ref.name},
        {QStringLiteral("configPath"), machine.configPath},
        {QStringLiteral("storagePaths"), stringArray(machine.storagePaths)},
        {QStringLiteral("storageDevices"), storageDevices},
        {QStringLiteral("networkDevices"), networkDevices},
        {QStringLiteral("powerState"), powerStateName(machine.powerState)},
        {QStringLiteral("ownership"), ownershipName(machine.ownership)},
        {QStringLiteral("inventoryComplete"), machine.inventoryComplete},
        {QStringLiteral("hardwareInventoryComplete"), machine.hardwareInventoryComplete},
        {QStringLiteral("stateRevision"), machine.stateRevision},
        {QStringLiteral("inaccessibleReason"), machine.inaccessibleReason.left(MaxShortText)},
        {QStringLiteral("warnings"), stringArray(machine.warnings, 32, 512)},
    };
    result.insert(QStringLiteral("cpuCount"), machine.cpuCount
                      ? QJsonValue(*machine.cpuCount) : QJsonValue());
    result.insert(QStringLiteral("memoryMiB"), machine.memoryMiB
                      ? QJsonValue(*machine.memoryMiB) : QJsonValue());
    result.insert(QStringLiteral("firmware"), machine.firmware
                      ? QJsonValue(firmwareName(*machine.firmware)) : QJsonValue());
    result.insert(QStringLiteral("secureBoot"), machine.secureBoot
                      ? QJsonValue(*machine.secureBoot) : QJsonValue());
    result.insert(QStringLiteral("tpm"), machine.tpm
                      ? QJsonValue(*machine.tpm) : QJsonValue());
    return result;
}

QJsonObject fileEvidenceJson(const FileEvidence &file)
{
    return QJsonObject{
        {QStringLiteral("path"), file.path},
        {QStringLiteral("sha256"), file.expectedSha256},
        {QStringLiteral("byteCount"), file.expectedSize},
        {QStringLiteral("lastModifiedMs"), file.expectedLastModifiedMs},
        {QStringLiteral("identity"), file.expectedIdentity},
        {QStringLiteral("description"), file.description.left(MaxShortText)},
    };
}

QJsonArray fileEvidenceJson(const QList<FileEvidence> &files)
{
    QJsonArray result;
    const qsizetype count = std::min(files.size(), qsizetype(MaxListItems));
    for (qsizetype index = 0; index < count; ++index)
        result.append(fileEvidenceJson(files.at(index)));
    return result;
}

QJsonObject snapshotJson(const Snapshot &snapshot)
{
    return QJsonObject{
        {QStringLiteral("id"), snapshot.id},
        {QStringLiteral("name"), snapshot.name},
        {QStringLiteral("description"), snapshot.description.left(MaxShortText)},
        {QStringLiteral("createdAt"), snapshot.createdAt.isValid()
             ? snapshot.createdAt.toUTC().toString(Qt::ISODateWithMs) : QString{}},
        {QStringLiteral("current"), snapshot.current},
        {QStringLiteral("inventoryRevision"), snapshot.inventoryRevision},
    };
}

QJsonArray machinesJson(const QList<Machine> &machines)
{
    QJsonArray result;
    const qsizetype count = std::min(machines.size(), qsizetype(MaxListItems));
    for (qsizetype index = 0; index < count; ++index)
        result.append(machineJson(machines.at(index)));
    return result;
}

QJsonArray providersJson(const QList<ProviderInfo> &providers)
{
    QJsonArray result;
    const qsizetype count = std::min(providers.size(), qsizetype(MaxListItems));
    for (qsizetype index = 0; index < count; ++index)
        result.append(providerJson(providers.at(index)));
    return result;
}

QJsonArray snapshotsJson(const QList<Snapshot> &snapshots)
{
    QJsonArray result;
    const qsizetype count = std::min(snapshots.size(), qsizetype(MaxListItems));
    for (qsizetype index = 0; index < count; ++index)
        result.append(snapshotJson(snapshots.at(index)));
    return result;
}

QJsonObject previewJson(const Plan &plan)
{
    const OperationPreview &preview = plan.preview;
    QJsonArray commands;
    for (const Command &command : preview.commands)
        commands.append(commandJson(command));
    QJsonArray preflight;
    for (const CommandEvidence &evidence : plan.preflight) {
        preflight.append(QJsonObject{
            {QStringLiteral("command"), commandJson(evidence.command)},
            {QStringLiteral("description"), evidence.description.left(MaxShortText)},
            {QStringLiteral("expectedEvidenceSha256"), evidence.expected},
        });
    }
    QJsonArray localWrites;
    for (const AtomicWrite &write : plan.atomicWritesAfterCommands) {
        localWrites.append(QJsonObject{
            {QStringLiteral("path"), write.path},
            {QStringLiteral("byteCount"), static_cast<qint64>(write.contents.size())},
            {QStringLiteral("contentSha256"), QString::fromLatin1(
                 QCryptographicHash::hash(write.contents, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("expectsExistingRevision"), !write.expectedSha256.isEmpty()},
        });
    }
    QJsonObject result{
        {QStringLiteral("id"), preview.id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("action"), preview.action},
        {QStringLiteral("target"), QJsonObject{
             {QStringLiteral("providerId"), preview.target.providerId},
             {QStringLiteral("id"), preview.target.id},
             {QStringLiteral("name"), preview.target.name},
         }},
        {QStringLiteral("risk"), riskName(preview.risk)},
        {QStringLiteral("effects"), stringArray(preview.effects, 64, MaxShortText)},
        {QStringLiteral("warnings"), stringArray(preview.warnings, 64, MaxShortText)},
        {QStringLiteral("commands"), commands},
        {QStringLiteral("preflight"), preflight},
        {QStringLiteral("reviewedFiles"), fileEvidenceJson(plan.filePreflight)},
        {QStringLiteral("localWrites"), localWrites},
        {QStringLiteral("managedDeletion"), plan.managedDeletionAfterCommands.has_value()},
        {QStringLiteral("catalogRevision"), preview.revision},
        {QStringLiteral("expiresAt"), preview.expiry.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("confirmation"), QJsonObject{
             {QStringLiteral("yesRequired"), true},
             {QStringLiteral("typedTokenRequired"), preview.risk == Risk::Destructive},
             {QStringLiteral("typedToken"), preview.confirmation},
         }},
    };
    if (!plan.errors.isEmpty())
        result.insert(QStringLiteral("errors"), stringArray(plan.errors, 64, MaxShortText));
    return result;
}

struct EvidenceEntry
{
    QJsonObject value;
};

class RecordingRunner final : public CommandRunner
{
public:
    explicit RecordingRunner(CommandRunner &inner) : m_inner(inner) {}

    ProcessResult run(const Command &command) override
    {
        ProcessResult result = m_inner.run(command);
        ++m_total;
        if (m_entries.size() < MaxEvidenceCommands) {
            m_entries.append(EvidenceEntry{QJsonObject{
                {QStringLiteral("command"), commandJson(command)},
                {QStringLiteral("started"), result.started},
                {QStringLiteral("timedOut"), result.timedOut},
                {QStringLiteral("deadlineExceeded"), result.deadlineExceeded},
                {QStringLiteral("exitCode"), result.exitCode},
                {QStringLiteral("error"), result.error.left(MaxShortText)},
                {QStringLiteral("standardOutput"), boundedBytes(result.standardOutput)},
                {QStringLiteral("standardError"), boundedBytes(result.standardError)},
                {QStringLiteral("standardOutputTruncated"), result.standardOutputTruncated},
                {QStringLiteral("standardErrorTruncated"), result.standardErrorTruncated},
            }});
        }
        return result;
    }

    [[nodiscard]] QJsonObject json() const
    {
        QJsonArray entries;
        for (const EvidenceEntry &entry : m_entries)
            entries.append(entry.value);
        return QJsonObject{
            {QStringLiteral("commands"), entries},
            {QStringLiteral("totalCommandCount"), m_total},
            {QStringLiteral("truncated"), m_total > m_entries.size()},
            {QStringLiteral("maximumRecordedCommands"), MaxEvidenceCommands},
            {QStringLiteral("maximumOutputBytesPerStream"), MaxEvidenceBytes},
        };
    }

private:
    CommandRunner &m_inner;
    QList<EvidenceEntry> m_entries;
    int m_total = 0;
};

class CatalogTransactionGuard final
{
public:
    explicit CatalogTransactionGuard(Catalog &catalog) : m_catalog(catalog) {}
    ~CatalogTransactionGuard() { m_catalog.endTransaction(); }

    CatalogTransactionGuard(const CatalogTransactionGuard &) = delete;
    CatalogTransactionGuard &operator=(const CatalogTransactionGuard &) = delete;

private:
    Catalog &m_catalog;
};

QJsonObject baseOutput(const QString &action,
                       const QString &catalogPath,
                       const QString &managedRoot)
{
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("wimforge.vm-lab-cli")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), action},
        {QStringLiteral("paths"), QJsonObject{
             {QStringLiteral("catalog"), catalogPath},
             {QStringLiteral("managedRoot"), managedRoot},
         }},
    };
}

VmLabCliResult failure(int exitCode, const QString &message, QJsonObject output)
{
    VmLabCliResult result;
    result.exitCode = exitCode;
    result.error = message;
    result.output = std::move(output);
    return result;
}

bool readString(const QJsonObject &object, const QString &key, QString *value,
                QString *error, bool required = false)
{
    if (!object.contains(key)) {
        if (required) {
            *error = QStringLiteral("Parameter '%1' is required.").arg(key);
            return false;
        }
        return true;
    }
    const QJsonValue candidate = object.value(key);
    if (!candidate.isString()) {
        *error = QStringLiteral("Parameter '%1' must be a string.").arg(key);
        return false;
    }
    *value = candidate.toString();
    if (required && value->trimmed().isEmpty()) {
        *error = QStringLiteral("Parameter '%1' cannot be empty.").arg(key);
        return false;
    }
    return true;
}

bool readBool(const QJsonObject &object, const QString &key, bool *value,
              QString *error)
{
    if (!object.contains(key))
        return true;
    if (!object.value(key).isBool()) {
        *error = QStringLiteral("Parameter '%1' must be true or false.").arg(key);
        return false;
    }
    *value = object.value(key).toBool();
    return true;
}

bool readInt(const QJsonObject &object, const QString &key, int *value,
             int minimum, int maximum, QString *error)
{
    if (!object.contains(key))
        return true;
    const QJsonValue candidate = object.value(key);
    const double number = candidate.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!candidate.isDouble() || !std::isfinite(number) || std::floor(number) != number
        || number < minimum || number > maximum) {
        *error = QStringLiteral("Parameter '%1' must be an integer between %2 and %3.")
                     .arg(key).arg(minimum).arg(maximum);
        return false;
    }
    *value = static_cast<int>(number);
    return true;
}

std::optional<Firmware> parseFirmware(const QString &value)
{
    if (value.compare(QStringLiteral("bios"), Qt::CaseInsensitive) == 0)
        return Firmware::Bios;
    if (value.compare(QStringLiteral("efi"), Qt::CaseInsensitive) == 0)
        return Firmware::Efi;
    return std::nullopt;
}

std::optional<NetworkMode> parseNetworkMode(const QString &value)
{
    if (value.compare(QStringLiteral("nat"), Qt::CaseInsensitive) == 0)
        return NetworkMode::Nat;
    if (value.compare(QStringLiteral("bridged"), Qt::CaseInsensitive) == 0)
        return NetworkMode::Bridged;
    if (value.compare(QStringLiteral("host-only"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("hostonly"), Qt::CaseInsensitive) == 0)
        return NetworkMode::HostOnly;
    if (value.compare(QStringLiteral("internal"), Qt::CaseInsensitive) == 0)
        return NetworkMode::Internal;
    if (value.compare(QStringLiteral("disconnected"), Qt::CaseInsensitive) == 0)
        return NetworkMode::Disconnected;
    return std::nullopt;
}

std::optional<StorageBus> parseStorageBus(const QString &value)
{
    if (value.compare(QStringLiteral("ide"), Qt::CaseInsensitive) == 0)
        return StorageBus::Ide;
    if (value.compare(QStringLiteral("sata"), Qt::CaseInsensitive) == 0)
        return StorageBus::Sata;
    if (value.compare(QStringLiteral("scsi"), Qt::CaseInsensitive) == 0)
        return StorageBus::Scsi;
    if (value.compare(QStringLiteral("nvme"), Qt::CaseInsensitive) == 0)
        return StorageBus::Nvme;
    return std::nullopt;
}

std::optional<Ownership> parseOwnership(const QString &value)
{
    if (value.compare(QStringLiteral("managed"), Qt::CaseInsensitive) == 0)
        return Ownership::Managed;
    if (value.compare(QStringLiteral("external"), Qt::CaseInsensitive) == 0)
        return Ownership::External;
    return std::nullopt;
}

std::optional<ManagerAction> parseAction(const QString &action)
{
    if (action == QStringLiteral("create")) return ManagerAction::Create;
    if (action == QStringLiteral("register")) return ManagerAction::Register;
    if (action == QStringLiteral("open-console")) return ManagerAction::OpenConsole;
    if (action == QStringLiteral("start")) return ManagerAction::Start;
    if (action == QStringLiteral("graceful-shutdown")) return ManagerAction::GracefulShutdown;
    if (action == QStringLiteral("power-off")) return ManagerAction::PowerOff;
    if (action == QStringLiteral("pause")) return ManagerAction::Pause;
    if (action == QStringLiteral("resume")) return ManagerAction::Resume;
    if (action == QStringLiteral("reset")) return ManagerAction::Reset;
    if (action == QStringLiteral("save-state")) return ManagerAction::SaveState;
    if (action == QStringLiteral("configure")) return ManagerAction::Configure;
    if (action == QStringLiteral("attach-iso")) return ManagerAction::AttachIso;
    if (action == QStringLiteral("detach-iso")) return ManagerAction::DetachIso;
    if (action == QStringLiteral("attach-storage")) return ManagerAction::AttachStorage;
    if (action == QStringLiteral("detach-storage")) return ManagerAction::DetachStorage;
    if (action == QStringLiteral("attach-network")) return ManagerAction::AttachNetwork;
    if (action == QStringLiteral("detach-network")) return ManagerAction::DetachNetwork;
    if (action == QStringLiteral("list-snapshots")) return ManagerAction::ListSnapshots;
    if (action == QStringLiteral("take-snapshot")) return ManagerAction::TakeSnapshot;
    if (action == QStringLiteral("restore-snapshot")) return ManagerAction::RestoreSnapshot;
    if (action == QStringLiteral("delete-snapshot")) return ManagerAction::DeleteSnapshot;
    if (action == QStringLiteral("forget")) return ManagerAction::ForgetCatalog;
    if (action == QStringLiteral("unregister")) return ManagerAction::Unregister;
    if (action == QStringLiteral("delete")) return ManagerAction::Delete;
    return std::nullopt;
}

QString requiredCapability(ManagerAction action, const QString &providerId)
{
    switch (action) {
    case ManagerAction::Create: return capability::create();
    case ManagerAction::Register:
        // VMware "register" is a catalog-only VMX import and intentionally does
        // not claim a provider registration capability.
        return providerId == virtualBoxProviderId() ? capability::registerMachine() : QString{};
    case ManagerAction::OpenConsole: return capability::openConsole();
    case ManagerAction::Start:
    case ManagerAction::GracefulShutdown:
    case ManagerAction::PowerOff:
    case ManagerAction::Pause:
    case ManagerAction::Resume:
    case ManagerAction::Reset:
    case ManagerAction::SaveState:
        return capability::lifecycle();
    case ManagerAction::Configure:
    case ManagerAction::AttachNetwork:
    case ManagerAction::DetachNetwork:
        return capability::configure();
    case ManagerAction::AttachIso:
    case ManagerAction::DetachIso:
    case ManagerAction::AttachStorage:
    case ManagerAction::DetachStorage:
        return capability::media();
    case ManagerAction::ListSnapshots:
    case ManagerAction::TakeSnapshot:
    case ManagerAction::RestoreSnapshot:
    case ManagerAction::DeleteSnapshot:
        return capability::snapshots();
    case ManagerAction::ForgetCatalog: return {};
    case ManagerAction::Unregister: return capability::unregisterMachine();
    case ManagerAction::Delete: return capability::deleteMachine();
    }
    return {};
}

bool requiresPoweredOff(ManagerAction action)
{
    return action == ManagerAction::Configure
        || action == ManagerAction::AttachIso
        || action == ManagerAction::DetachIso
        || action == ManagerAction::AttachStorage
        || action == ManagerAction::DetachStorage
        || action == ManagerAction::AttachNetwork
        || action == ManagerAction::DetachNetwork;
}

const ProviderInfo *findProvider(const QList<ProviderInfo> &providers,
                                 const QString &providerId)
{
    const auto found = std::find_if(providers.cbegin(), providers.cend(),
                                    [&providerId](const ProviderInfo &provider) {
        return provider.id == providerId && provider.available;
    });
    return found == providers.cend() ? nullptr : &*found;
}

const Machine *findMachine(const QList<Machine> &machines,
                           const QString &providerId,
                           const QString &id)
{
    const auto found = std::find_if(machines.cbegin(), machines.cend(),
                                    [&providerId, &id](const Machine &machine) {
        return machine.ref.providerId == providerId && machine.ref.id == id;
    });
    return found == machines.cend() ? nullptr : &*found;
}

bool parseCreate(const QJsonObject &parameters,
                 const QString &managedRoot,
                 OperationRequest *request,
                 QString *error)
{
    CreateSpec &spec = request->createSpec;
    if (!readString(parameters, QStringLiteral("providerId"), &spec.providerId, error, true)
        || !readString(parameters, QStringLiteral("id"), &spec.id, error)
        || !readString(parameters, QStringLiteral("name"), &spec.name, error, true)
        || !readString(parameters, QStringLiteral("directory"), &spec.directory, error)
        || !readString(parameters, QStringLiteral("guestType"), &spec.guestType, error)
        || !readString(parameters, QStringLiteral("bridgedInterface"), &spec.bridgedInterface, error)
        || !readString(parameters, QStringLiteral("isoPath"), &spec.isoPath, error)
        || !readBool(parameters, QStringLiteral("secureBoot"), &spec.secureBoot, error)
        || !readBool(parameters, QStringLiteral("tpm"), &spec.tpm, error)
        || !readBool(parameters, QStringLiteral("unattendedBoot"), &spec.unattendedBoot, error)
        || !readInt(parameters, QStringLiteral("virtualHardwareVersion"),
                    &spec.virtualHardwareVersion, 0, 1000, error)
        || !readInt(parameters, QStringLiteral("cpuCount"), &spec.cpuCount, 1, 1024, error)
        || !readInt(parameters, QStringLiteral("memoryMiB"), &spec.memoryMiB, 128,
                    std::numeric_limits<int>::max(), error)
        || !readInt(parameters, QStringLiteral("diskMiB"), &spec.diskMiB, 1,
                    std::numeric_limits<int>::max(), error)) {
        return false;
    }
    if (!isKnownProviderId(spec.providerId)) {
        *error = QStringLiteral("Parameter 'providerId' is not a supported VM provider.");
        return false;
    }
    if (spec.id.isEmpty() && spec.providerId == virtualBoxProviderId())
        spec.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (spec.directory.trimmed().isEmpty()) {
        spec.directory = spec.providerId == virtualBoxProviderId()
            ? managedRoot : QDir(managedRoot).filePath(spec.name);
    }
    if (spec.guestType.trimmed().isEmpty()) {
        spec.guestType = spec.providerId == virtualBoxProviderId()
            ? QStringLiteral("Windows11_64") : QStringLiteral("windows11-64");
    }
    QString firmware;
    if (!readString(parameters, QStringLiteral("firmware"), &firmware, error))
        return false;
    if (!firmware.isEmpty()) {
        const std::optional<Firmware> parsed = parseFirmware(firmware);
        if (!parsed) {
            *error = QStringLiteral("Parameter 'firmware' must be 'bios' or 'efi'.");
            return false;
        }
        spec.firmware = *parsed;
    }
    QString networkMode;
    if (!readString(parameters, QStringLiteral("networkMode"), &networkMode, error))
        return false;
    if (!networkMode.isEmpty()) {
        const std::optional<NetworkMode> parsed = parseNetworkMode(networkMode);
        if (!parsed) {
            *error = QStringLiteral("Parameter 'networkMode' is unsupported.");
            return false;
        }
        spec.networkMode = *parsed;
    }
    QString ownership = QStringLiteral("managed");
    if (!readString(parameters, QStringLiteral("ownership"), &ownership, error))
        return false;
    const std::optional<Ownership> parsedOwnership = parseOwnership(ownership);
    if (!parsedOwnership) {
        *error = QStringLiteral("Parameter 'ownership' must be 'managed' or 'external'.");
        return false;
    }
    request->ownership = *parsedOwnership;
    return true;
}

bool parseConfiguration(const QJsonObject &parameters, ConfigPatch *patch, QString *error)
{
    if (parameters.contains(QStringLiteral("cpuCount"))) {
        int value = 0;
        if (!readInt(parameters, QStringLiteral("cpuCount"), &value, 1, 1024, error))
            return false;
        patch->cpuCount = value;
    }
    if (parameters.contains(QStringLiteral("memoryMiB"))) {
        int value = 0;
        if (!readInt(parameters, QStringLiteral("memoryMiB"), &value, 128,
                     std::numeric_limits<int>::max(), error))
            return false;
        patch->memoryMiB = value;
    }
    if (parameters.contains(QStringLiteral("firmware"))) {
        QString value;
        if (!readString(parameters, QStringLiteral("firmware"), &value, error))
            return false;
        const std::optional<Firmware> parsed = parseFirmware(value);
        if (!parsed) {
            *error = QStringLiteral("Parameter 'firmware' must be 'bios' or 'efi'.");
            return false;
        }
        patch->firmware = *parsed;
    }
    if (parameters.contains(QStringLiteral("secureBoot"))) {
        bool value = false;
        if (!readBool(parameters, QStringLiteral("secureBoot"), &value, error))
            return false;
        patch->secureBoot = value;
    }
    if (parameters.contains(QStringLiteral("tpm"))) {
        bool value = false;
        if (!readBool(parameters, QStringLiteral("tpm"), &value, error))
            return false;
        patch->tpm = value;
    }
    if (parameters.contains(QStringLiteral("networkMode"))) {
        QString value;
        if (!readString(parameters, QStringLiteral("networkMode"), &value, error))
            return false;
        const std::optional<NetworkMode> parsed = parseNetworkMode(value);
        if (!parsed) {
            *error = QStringLiteral("Parameter 'networkMode' is unsupported.");
            return false;
        }
        patch->networkMode = *parsed;
    }
    if (parameters.contains(QStringLiteral("bridgedInterface"))) {
        QString value;
        if (!readString(parameters, QStringLiteral("bridgedInterface"), &value, error))
            return false;
        patch->bridgedInterface = value;
    }
    if (parameters.contains(QStringLiteral("isoPath"))) {
        QString value;
        if (!readString(parameters, QStringLiteral("isoPath"), &value, error))
            return false;
        patch->isoPath = value;
    }
    if (patch->empty()) {
        *error = QStringLiteral("At least one powered-off VM configuration field is required.");
        return false;
    }
    return true;
}

bool parseStorage(const QJsonObject &parameters, bool attaching,
                  StorageDeviceSpec *storage, QString *error)
{
    QString bus = QStringLiteral("sata");
    if (!readString(parameters, QStringLiteral("bus"), &bus, error)
        || !readString(parameters, QStringLiteral("controllerName"),
                       &storage->controllerName, error)
        || !readBool(parameters, QStringLiteral("optical"), &storage->optical, error)
        || !readInt(parameters, QStringLiteral("controller"), &storage->controller, 0, 3, error)
        || !readInt(parameters, QStringLiteral("port"), &storage->port, 0, 29, error)
        || !readInt(parameters, QStringLiteral("device"), &storage->device, 0, 1, error)) {
        return false;
    }
    const std::optional<StorageBus> parsed = parseStorageBus(bus);
    if (!parsed) {
        *error = QStringLiteral("Parameter 'bus' must be ide, sata, scsi, or nvme.");
        return false;
    }
    storage->bus = *parsed;
    if (attaching
        && !readString(parameters, QStringLiteral("path"), &storage->path, error, true)) {
        return false;
    }
    return true;
}

bool parseNetwork(const QJsonObject &parameters, bool attaching,
                  NetworkAdapterSpec *network, QString *error)
{
    if (!readInt(parameters, QStringLiteral("slot"), &network->slot, 1, 10, error))
        return false;
    if (!attaching) {
        network->mode = NetworkMode::Disconnected;
        network->connected = false;
        return true;
    }
    QString mode = QStringLiteral("nat");
    if (!readString(parameters, QStringLiteral("networkMode"), &mode, error)
        || !readString(parameters, QStringLiteral("interfaceName"),
                       &network->interfaceName, error)
        || !readBool(parameters, QStringLiteral("connected"), &network->connected, error)) {
        return false;
    }
    const std::optional<NetworkMode> parsed = parseNetworkMode(mode);
    if (!parsed) {
        *error = QStringLiteral("Parameter 'networkMode' is unsupported.");
        return false;
    }
    network->mode = *parsed;
    return true;
}

bool parseOperationParameters(const QJsonObject &parameters,
                              const QString &managedRoot,
                              ManagerAction action,
                              OperationRequest *request,
                              QString *providerId,
                              QString *machineId,
                              QString *error)
{
    request->action = action;
    if (action == ManagerAction::Create) {
        if (!parseCreate(parameters, managedRoot, request, error))
            return false;
        *providerId = request->createSpec.providerId;
        return true;
    }

    if (!readString(parameters, QStringLiteral("providerId"), providerId, error, true)
        || !isKnownProviderId(*providerId)) {
        if (error->isEmpty())
            *error = QStringLiteral("Parameter 'providerId' is not a supported VM provider.");
        return false;
    }
    if (action == ManagerAction::Register) {
        if (!readString(parameters, QStringLiteral("configPath"), &request->path, error, true)
            || !readString(parameters, QStringLiteral("name"), &request->name, error)) {
            return false;
        }
        QString ownership = QStringLiteral("external");
        if (!readString(parameters, QStringLiteral("ownership"), &ownership, error))
            return false;
        const std::optional<Ownership> parsed = parseOwnership(ownership);
        if (!parsed) {
            *error = QStringLiteral("Parameter 'ownership' must be 'managed' or 'external'.");
            return false;
        }
        request->ownership = *parsed;
        const QString displayName = request->name.trimmed().isEmpty()
            ? QFileInfo(request->path).completeBaseName() : request->name.trimmed();
        Machine machine;
        machine.ref = VmRef{*providerId, request->path, displayName};
        request->machine = std::move(machine);
        return true;
    }

    if (!readString(parameters, QStringLiteral("vmId"), machineId, error, true))
        return false;

    switch (action) {
    case ManagerAction::Start:
        return readBool(parameters, QStringLiteral("headless"), &request->headless, error);
    case ManagerAction::Configure:
        return parseConfiguration(parameters, &request->configPatch, error);
    case ManagerAction::AttachIso:
        return readString(parameters, QStringLiteral("isoPath"), &request->path, error, true);
    case ManagerAction::AttachStorage:
        return parseStorage(parameters, true, &request->storage, error);
    case ManagerAction::DetachStorage:
        return parseStorage(parameters, false, &request->storage, error);
    case ManagerAction::AttachNetwork:
        return parseNetwork(parameters, true, &request->network, error);
    case ManagerAction::DetachNetwork:
        return parseNetwork(parameters, false, &request->network, error);
    case ManagerAction::TakeSnapshot:
        return readString(parameters, QStringLiteral("name"), &request->name, error, true)
            && readString(parameters, QStringLiteral("description"),
                          &request->description, error);
    case ManagerAction::RestoreSnapshot:
    case ManagerAction::DeleteSnapshot:
        if (!readString(parameters, QStringLiteral("snapshotId"),
                        &request->snapshot.id, error)
            || !readString(parameters, QStringLiteral("snapshotName"),
                           &request->snapshot.name, error)) {
            return false;
        }
        if (request->snapshot.id.trimmed().isEmpty()
            && request->snapshot.name.trimmed().isEmpty()) {
            *error = QStringLiteral("A snapshotId or snapshotName is required.");
            return false;
        }
        return true;
    case ManagerAction::Create:
    case ManagerAction::Register:
    case ManagerAction::OpenConsole:
    case ManagerAction::GracefulShutdown:
    case ManagerAction::PowerOff:
    case ManagerAction::Pause:
    case ManagerAction::Resume:
    case ManagerAction::Reset:
    case ManagerAction::SaveState:
    case ManagerAction::DetachIso:
    case ManagerAction::ListSnapshots:
    case ManagerAction::ForgetCatalog:
    case ManagerAction::Unregister:
    case ManagerAction::Delete:
        return true;
    }
    return true;
}

QString createdConfigurationPath(const OperationRequest &request)
{
    if (request.action == ManagerAction::Register)
        return QFileInfo(request.path).absoluteFilePath();
    const CreateSpec &spec = request.createSpec;
    if (spec.providerId == virtualBoxProviderId()) {
        return QDir(spec.directory).filePath(
            spec.name + QLatin1Char('/') + spec.name + QStringLiteral(".vbox"));
    }
    return QDir(spec.directory).filePath(spec.name + QStringLiteral(".vmx"));
}

} // namespace

VmLabCliService::VmLabCliService(
    QString catalogPath,
    QString managedRoot,
    std::shared_ptr<VmLabProviderAdapter> providerAdapter)
    : m_catalogPath(std::move(catalogPath)),
      m_managedRoot(std::move(managedRoot)),
      m_providerAdapter(providerAdapter ? std::move(providerAdapter)
                                        : std::make_shared<NativeVmLabProviderAdapter>())
{
}

QString VmLabCliService::catalogPath() const { return m_catalogPath; }
QString VmLabCliService::managedRoot() const { return m_managedRoot; }

QStringList VmLabCliService::supportedActions()
{
    return {
        QStringLiteral("paths"), QStringLiteral("catalog"), QStringLiteral("detect"),
        QStringLiteral("inventory"), QStringLiteral("create"), QStringLiteral("register"),
        QStringLiteral("open-console"), QStringLiteral("start"),
        QStringLiteral("graceful-shutdown"), QStringLiteral("power-off"),
        QStringLiteral("pause"), QStringLiteral("resume"), QStringLiteral("reset"),
        QStringLiteral("save-state"), QStringLiteral("configure"),
        QStringLiteral("attach-iso"), QStringLiteral("detach-iso"),
        QStringLiteral("attach-storage"), QStringLiteral("detach-storage"),
        QStringLiteral("attach-network"), QStringLiteral("detach-network"),
        QStringLiteral("list-snapshots"), QStringLiteral("take-snapshot"),
        QStringLiteral("restore-snapshot"), QStringLiteral("delete-snapshot"),
        QStringLiteral("forget"), QStringLiteral("unregister"), QStringLiteral("delete"),
    };
}

VmLabCliResult VmLabCliService::handle(const VmLabCliRequest &request,
                                       CommandRunner &runner) const
{
    const QString action = request.action.trimmed().toLower();
    QJsonObject output = baseOutput(action, m_catalogPath, m_managedRoot);
    RecordingRunner recording(runner);
    const auto finish = [&recording](VmLabCliResult result) {
        result.output.insert(QStringLiteral("ok"), result.success);
        result.output.insert(QStringLiteral("exitCode"), result.exitCode);
        if (!result.error.isEmpty())
            result.output.insert(QStringLiteral("error"), result.error);
        result.output.insert(QStringLiteral("evidence"), recording.json());
        return result;
    };
    const auto fail = [&finish, &output](int code, const QString &message) {
        return finish(failure(code, message, output));
    };

    if (!QFileInfo(m_catalogPath).isAbsolute())
        return fail(VmLabCliResult::InvalidRequest,
                    QStringLiteral("VM catalog path must be absolute."));
    const QFileInfo rootInfo(m_managedRoot);
    if (!rootInfo.isAbsolute())
        return fail(VmLabCliResult::InvalidRequest,
                    QStringLiteral("Managed VM root must be absolute."));
    if (!rootInfo.exists() && !QDir().mkpath(rootInfo.absoluteFilePath()))
        return fail(VmLabCliResult::CatalogFailure,
                    QStringLiteral("Could not create the managed VM root."));
    if (!QFileInfo(m_managedRoot).isDir())
        return fail(VmLabCliResult::InvalidRequest,
                    QStringLiteral("Managed VM root is not a directory."));

    Catalog catalog(m_catalogPath);
    QString catalogError;
    if (!catalog.load(&catalogError))
        return fail(VmLabCliResult::CatalogFailure, catalogError);
    output.insert(QStringLiteral("catalogRevision"), catalog.revision());

    if (action == QStringLiteral("paths")) {
        VmLabCliResult result;
        result.success = true;
        result.exitCode = VmLabCliResult::Ok;
        result.output = output;
        return finish(std::move(result));
    }
    if (action == QStringLiteral("catalog")) {
        output.insert(QStringLiteral("machines"), machinesJson(catalog.machines()));
        output.insert(QStringLiteral("machineCount"), catalog.machines().size());
        output.insert(QStringLiteral("truncated"), catalog.machines().size() > MaxListItems);
        VmLabCliResult result;
        result.success = true;
        result.exitCode = VmLabCliResult::Ok;
        result.output = output;
        return finish(std::move(result));
    }

    if (action == QStringLiteral("forget")) {
        OperationRequest operation;
        QString providerId;
        QString machineId;
        QString parseError;
        if (!parseOperationParameters(request.parameters, m_managedRoot,
                                      ManagerAction::ForgetCatalog, &operation,
                                      &providerId, &machineId, &parseError)) {
            return fail(VmLabCliResult::InvalidRequest, parseError);
        }
        const Machine *selected = findMachine(catalog.machines(), providerId, machineId);
        if (!selected) {
            return fail(VmLabCliResult::InvalidRequest,
                        QStringLiteral("VM '%1' is absent from the WimForge catalog.")
                            .arg(machineId));
        }
        const VmRef selectedReference = selected->ref;
        operation.machine = *selected;
        operation.managedRoot = m_managedRoot;
        operation.allMachines = catalog.machines();
        operation.revision = catalog.revision();
        operation.now = request.now.isValid()
            ? request.now.toUTC() : QDateTime::currentDateTimeUtc();
        const Plan plan = makeForgetCatalogPlan(*selected, operation.revision,
                                                operation.now);
        output.insert(QStringLiteral("mode"), request.execute
                          ? QStringLiteral("execute") : QStringLiteral("review"));
        output.insert(QStringLiteral("preview"), previewJson(plan));
        QString validationError;
        if (!Executor::validate(plan, operation.revision, plan.preview.confirmation,
                                operation.now, &validationError)) {
            return fail(VmLabCliResult::ProviderFailure, validationError);
        }
        if (!request.execute) {
            VmLabCliResult result;
            result.success = true;
            result.exitCode = VmLabCliResult::Ok;
            result.output = output;
            return finish(std::move(result));
        }
        if (!request.yes) {
            return fail(VmLabCliResult::ConfirmationRequired,
                        QStringLiteral(
                            "Execution requires the explicit --yes option after reviewing the preview."));
        }
        if (!catalog.beginTransaction(operation.revision, &catalogError))
            return fail(VmLabCliResult::CatalogFailure, catalogError);
        const CatalogTransactionGuard transaction(catalog);
        operation.revision = catalog.revision();
        const Result execution = Executor::execute(
            plan, operation.revision, {}, operation.now, recording);
        output.insert(QStringLiteral("execution"), QJsonObject{
            {QStringLiteral("success"), execution.success},
            {QStringLiteral("processCount"), execution.processes.size()},
            {QStringLiteral("verifiedFiles"), fileEvidenceJson(execution.verifiedFiles)},
            {QStringLiteral("error"), execution.error.left(MaxShortText)},
        });
        if (!execution.success)
            return fail(VmLabCliResult::ProviderFailure, execution.error);
        if (!catalog.remove(selectedReference) || !catalog.save(&catalogError))
            return fail(VmLabCliResult::CatalogFailure,
                        catalogError.isEmpty()
                            ? QStringLiteral("The catalog VM entry could not be removed.")
                            : catalogError);
        output.insert(QStringLiteral("catalogRevisionAfter"), catalog.revision());
        VmLabCliResult result;
        result.success = true;
        result.exitCode = VmLabCliResult::Ok;
        result.output = output;
        return finish(std::move(result));
    }

    if (!m_providerAdapter)
        return fail(VmLabCliResult::InvalidRequest,
                    QStringLiteral("VM Lab provider adapter is unavailable."));

    QList<ProviderProbePaths> candidates = request.probeCandidates;
    if (candidates.isEmpty())
        candidates = ProviderDetector::defaultWindowsCandidates();

    QList<ProviderInfo> providers;
    try {
        providers = m_providerAdapter->detect(candidates, recording);
    } catch (const std::exception &exception) {
        return fail(VmLabCliResult::ProviderFailure,
                    QStringLiteral("Provider detection failed: %1")
                        .arg(QString::fromUtf8(exception.what()).left(MaxShortText)));
    } catch (...) {
        return fail(VmLabCliResult::ProviderFailure,
                    QStringLiteral("Provider detection failed unexpectedly."));
    }
    output.insert(QStringLiteral("providers"), providersJson(providers));
    output.insert(QStringLiteral("providerCount"), providers.size());

    if (action == QStringLiteral("detect")) {
        VmLabCliResult result;
        result.success = true;
        result.exitCode = VmLabCliResult::Ok;
        result.output = output;
        return finish(std::move(result));
    }

    if (action == QStringLiteral("inventory")) {
        InventoryRefreshResult inventory;
        try {
            inventory = m_providerAdapter->refreshInventory(
                providers, catalog.machines(), recording);
        } catch (const std::exception &exception) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Provider inventory failed: %1")
                            .arg(QString::fromUtf8(exception.what()).left(MaxShortText)));
        } catch (...) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Provider inventory failed unexpectedly."));
        }
        output.insert(QStringLiteral("inventory"), QJsonObject{
            {QStringLiteral("complete"), inventory.complete},
            {QStringLiteral("machines"), machinesJson(inventory.machines)},
            {QStringLiteral("machineCount"), inventory.machines.size()},
            {QStringLiteral("truncated"), inventory.machines.size() > MaxListItems},
            {QStringLiteral("warnings"), stringArray(inventory.warnings, 64, MaxShortText)},
        });
        if (!inventory.success)
            return fail(VmLabCliResult::ProviderFailure,
                        inventory.error.isEmpty()
                            ? QStringLiteral("Provider inventory did not return usable state.")
                            : inventory.error);
        VmLabCliResult result;
        result.success = true;
        result.exitCode = VmLabCliResult::Ok;
        result.output = output;
        return finish(std::move(result));
    }

    const std::optional<ManagerAction> managerAction = parseAction(action);
    if (!managerAction) {
        output.insert(QStringLiteral("supportedActions"), stringArray(supportedActions()));
        return fail(VmLabCliResult::InvalidRequest,
                    QStringLiteral("Unsupported VM Lab action '%1'.").arg(action));
    }

    OperationRequest operation;
    QString providerId;
    QString machineId;
    QString parseError;
    if (!parseOperationParameters(request.parameters, m_managedRoot, *managerAction,
                                  &operation, &providerId, &machineId, &parseError)) {
        return fail(VmLabCliResult::InvalidRequest, parseError);
    }
    const ProviderInfo *provider = findProvider(providers, providerId);
    if (!provider) {
        return fail(VmLabCliResult::ProviderFailure,
                    QStringLiteral("Provider '%1' is not available from current detection evidence.")
                        .arg(providerId));
    }
    const QString capabilityName = requiredCapability(*managerAction, providerId);
    if (!capabilityName.isEmpty() && !provider->supports(capabilityName)) {
        return fail(VmLabCliResult::ProviderFailure,
                    QStringLiteral("Provider '%1' did not prove capability '%2'.")
                        .arg(providerId, capabilityName));
    }

    QList<Machine> liveMachines = catalog.machines();
    if (*managerAction != ManagerAction::Create
        && *managerAction != ManagerAction::Register) {
        InventoryRefreshResult inventory;
        try {
            inventory = m_providerAdapter->refreshInventory(
                providers, catalog.machines(), recording);
        } catch (const std::exception &exception) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Provider inventory failed: %1")
                            .arg(QString::fromUtf8(exception.what()).left(MaxShortText)));
        } catch (...) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Provider inventory failed unexpectedly."));
        }
        if (!inventory.success) {
            return fail(VmLabCliResult::ProviderFailure,
                        inventory.error.isEmpty()
                            ? QStringLiteral("Refresh live VM inventory before reviewing this action.")
                            : inventory.error);
        }
        liveMachines = inventory.machines;
        const Machine *selected = findMachine(liveMachines, providerId, machineId);
        if (!selected) {
            return fail(VmLabCliResult::InvalidRequest,
                        QStringLiteral("VM '%1' is absent from current provider inventory.")
                            .arg(machineId));
        }
        if (!selected->inventoryComplete || selected->stateRevision.isEmpty()) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Complete live state evidence is required before review."));
        }
        if (requiresPoweredOff(*managerAction)
            && selected->powerState != PowerState::PoweredOff) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("This configuration action requires a powered-off VM."));
        }
        operation.machine = *selected;
    }

    operation.managedRoot = m_managedRoot;
    operation.allMachines = liveMachines;
    operation.revision = catalog.revision();
    operation.now = request.now.isValid()
        ? request.now.toUTC() : QDateTime::currentDateTimeUtc();

    if (*managerAction == ManagerAction::RestoreSnapshot
        || *managerAction == ManagerAction::DeleteSnapshot) {
        OperationRequest listRequest = operation;
        listRequest.action = ManagerAction::ListSnapshots;
        const Plan listPlan = m_providerAdapter->plan(*provider, listRequest);
        const Result listed = Executor::execute(
            listPlan, operation.revision, {}, operation.now, recording);
        if (!listed.success || listed.processes.isEmpty()) {
            return fail(VmLabCliResult::ProviderFailure,
                        listed.error.isEmpty()
                            ? QStringLiteral("Could not refresh snapshot inventory before review.")
                            : listed.error);
        }
        QString snapshotError;
        QList<Snapshot> snapshots = m_providerAdapter->parseSnapshots(
            providerId, listed.processes.constLast().standardOutput, &snapshotError);
        const QString snapshotRevision = commandEvidence(
            EvidenceFormat::RawSha256,
            listed.processes.constLast().standardOutput, &snapshotError);
        if (!snapshotError.isEmpty())
            return fail(VmLabCliResult::ProviderFailure, snapshotError);
        for (Snapshot &snapshot : snapshots)
            snapshot.inventoryRevision = snapshotRevision;
        const auto selectedSnapshot = std::find_if(
            snapshots.cbegin(), snapshots.cend(), [&operation](const Snapshot &snapshot) {
                return (!operation.snapshot.id.trimmed().isEmpty()
                        && snapshot.id == operation.snapshot.id)
                    || (!operation.snapshot.name.trimmed().isEmpty()
                        && snapshot.name == operation.snapshot.name);
            });
        if (selectedSnapshot == snapshots.cend()) {
            return fail(VmLabCliResult::InvalidRequest,
                        QStringLiteral("Requested snapshot is absent from refreshed provider inventory."));
        }
        operation.snapshot = *selectedSnapshot;
    }

    Plan plan;
    try {
        plan = m_providerAdapter->plan(*provider, operation);
    } catch (const std::exception &exception) {
        return fail(VmLabCliResult::ProviderFailure,
                    QStringLiteral("Provider planning failed: %1")
                        .arg(QString::fromUtf8(exception.what()).left(MaxShortText)));
    } catch (...) {
        return fail(VmLabCliResult::ProviderFailure,
                    QStringLiteral("Provider planning failed unexpectedly."));
    }
    output.insert(QStringLiteral("mode"), request.execute
                      ? QStringLiteral("execute") : QStringLiteral("review"));
    output.insert(QStringLiteral("preview"), previewJson(plan));

    QString validationError;
    if (!Executor::validate(plan, operation.revision, plan.preview.confirmation,
                            operation.now, &validationError)) {
        return fail(VmLabCliResult::ProviderFailure, validationError);
    }

    if (!request.execute) {
        VmLabCliResult result;
        result.success = true;
        result.exitCode = VmLabCliResult::Ok;
        result.output = output;
        return finish(std::move(result));
    }
    if (!request.yes) {
        return fail(VmLabCliResult::ConfirmationRequired,
                    QStringLiteral("Execution requires the explicit --yes option after reviewing the preview."));
    }
    if (plan.preview.risk == Risk::Destructive
        && request.typedConfirmation != plan.preview.confirmation) {
        return fail(VmLabCliResult::ConfirmationRequired,
                    QStringLiteral("Type the exact destructive confirmation token from the preview."));
    }

    if (!catalog.beginTransaction(operation.revision, &catalogError))
        return fail(VmLabCliResult::CatalogFailure, catalogError);
    const CatalogTransactionGuard transaction(catalog);
    operation.revision = catalog.revision();

    if (*managerAction == ManagerAction::Delete
        && plan.managedDeletionAfterCommands) {
        InventoryRefreshResult refreshed;
        try {
            refreshed = m_providerAdapter->refreshInventory(
                providers, catalog.machines(), recording);
        } catch (const std::exception &exception) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Pre-delete inventory failed: %1")
                            .arg(QString::fromUtf8(exception.what()).left(MaxShortText)));
        } catch (...) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Pre-delete inventory failed unexpectedly."));
        }
        if (!refreshed.success || !refreshed.complete) {
            return fail(VmLabCliResult::ProviderFailure,
                        refreshed.error.isEmpty()
                            ? QStringLiteral(
                                  "Managed deletion requires complete live inventory immediately before execution.")
                            : refreshed.error);
        }
        const Machine *current = operation.machine
            ? findMachine(refreshed.machines, operation.machine->ref.providerId,
                          operation.machine->ref.id)
            : nullptr;
        if (!current) {
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Managed deletion target disappeared during live refresh."));
        }
        operation.machine = *current;
        operation.allMachines = refreshed.machines;
        plan.managedDeletionAfterCommands->machine = *current;
        plan.managedDeletionAfterCommands->catalogMachines = refreshed.machines;
    }

    Executor::ManagedInventoryRefresh postCommandInventory;
    if (plan.managedDeletionAfterCommands) {
        QList<Machine> inventoryCatalog = catalog.machines();
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
        postCommandInventory = [this, providers, inventoryCatalog](
                                   CommandRunner &refreshRunner,
                                   QList<Machine> *machines,
                                   QString *error) {
            try {
                const InventoryRefreshResult refreshed = m_providerAdapter->refreshInventory(
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
                                 .arg(QString::fromUtf8(exception.what()).left(MaxShortText));
                }
            } catch (...) {
                if (error)
                    *error = QStringLiteral(
                        "Post-command provider inventory failed unexpectedly.");
            }
            return false;
        };
    }

    const Result execution = Executor::execute(
        plan, operation.revision, request.typedConfirmation, operation.now, recording,
        postCommandInventory);
    output.insert(QStringLiteral("execution"), QJsonObject{
        {QStringLiteral("success"), execution.success},
        {QStringLiteral("processCount"), execution.processes.size()},
        {QStringLiteral("verifiedFiles"), fileEvidenceJson(execution.verifiedFiles)},
        {QStringLiteral("error"), execution.error.left(MaxShortText)},
    });
    if (!execution.success)
        return fail(VmLabCliResult::ProviderFailure, execution.error);

    if (*managerAction == ManagerAction::ListSnapshots) {
        if (execution.processes.isEmpty())
            return fail(VmLabCliResult::ProviderFailure,
                        QStringLiteral("Snapshot inventory returned no provider process output."));
        QString snapshotError;
        QList<Snapshot> snapshots = m_providerAdapter->parseSnapshots(
            providerId, execution.processes.constLast().standardOutput, &snapshotError);
        const QString snapshotRevision = commandEvidence(
            EvidenceFormat::RawSha256,
            execution.processes.constLast().standardOutput, &snapshotError);
        for (Snapshot &snapshot : snapshots)
            snapshot.inventoryRevision = snapshotRevision;
        if (!snapshotError.isEmpty())
            return fail(VmLabCliResult::ProviderFailure, snapshotError);
        output.insert(QStringLiteral("snapshots"), snapshotsJson(snapshots));
        output.insert(QStringLiteral("snapshotCount"), snapshots.size());
    }

    if (*managerAction == ManagerAction::Create
        || *managerAction == ManagerAction::Register) {
        Machine machine;
        machine.ref = plan.preview.target;
        machine.configPath = createdConfigurationPath(operation);
        machine.ownership = operation.ownership;
        machine.ownershipToken = plan.managedOwnershipToken;
        if (!catalog.upsert(machine, &catalogError) || !catalog.save(&catalogError)) {
            output.insert(QStringLiteral("providerOperationSucceeded"), true);
            return fail(VmLabCliResult::CatalogFailure,
                        QStringLiteral("Provider operation succeeded, but the VM catalog update failed: %1")
                            .arg(catalogError));
        }
    } else if ((*managerAction == ManagerAction::Delete
                || *managerAction == ManagerAction::Unregister
                || *managerAction == ManagerAction::ForgetCatalog)
               && operation.machine && catalog.remove(operation.machine->ref)
               && !catalog.save(&catalogError)) {
        output.insert(QStringLiteral("providerOperationSucceeded"), true);
        return fail(VmLabCliResult::CatalogFailure,
                    QStringLiteral("Provider operation succeeded, but the VM catalog update failed: %1")
                        .arg(catalogError));
    }

    output.insert(QStringLiteral("catalogRevisionAfter"), catalog.revision());
    VmLabCliResult result;
    result.success = true;
    result.exitCode = VmLabCliResult::Ok;
    result.output = output;
    return finish(std::move(result));
}

} // namespace wimforge::vmlab
