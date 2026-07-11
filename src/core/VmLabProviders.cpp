#include "VmLabProviders.h"

#include "VmLabVmx.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QVersionNumber>

#include <algorithm>
#include <utility>

namespace wimforge::vmlab {
namespace {

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

bool trustedInstalledFile(const QString &path, const QString &trustedRoot,
                          QString *canonicalPath, QString *error)
{
    const QFileInfo rootInfo(trustedRoot);
    const QFileInfo fileInfo(path);
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    const QString canonicalFile = fileInfo.canonicalFilePath();
    if (!rootInfo.isAbsolute() || !rootInfo.exists() || !rootInfo.isDir()
        || canonicalRoot.isEmpty() || !fileInfo.isAbsolute() || !fileInfo.exists()
        || !fileInfo.isFile() || canonicalFile.isEmpty()) {
        setError(error, QStringLiteral("Provider executable is not a resolvable installed file."));
        return false;
    }
    const QString relative = QDir(canonicalRoot).relativeFilePath(canonicalFile);
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../"))
        || relative.startsWith(QStringLiteral("..\\")) || QDir::isAbsolutePath(relative)) {
        setError(error, QStringLiteral("Provider executable resolves outside protected Program Files."));
        return false;
    }
    QString cursor = canonicalRoot;
    for (const QString &segment : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        cursor = QDir(cursor).filePath(segment);
        const QFileInfo component(cursor);
        if (component.isSymLink()
#ifdef Q_OS_WIN
            || component.isJunction()
#endif
        ) {
            setError(error, QStringLiteral("Provider executable path contains a reparse point."));
            return false;
        }
    }
    if (canonicalPath)
        *canonicalPath = canonicalFile;
    setError(error, {});
    return true;
}

QStringList protectedProgramFilesRoots()
{
#ifdef Q_OS_WIN
    QSettings registry(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"),
        QSettings::NativeFormat);
    QStringList roots{
        registry.value(QStringLiteral("ProgramFilesDir")).toString(),
        registry.value(QStringLiteral("ProgramFilesDir (x86)")).toString(),
    };
    roots.removeAll(QString{});
    for (QString &root : roots)
        root = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    roots.removeDuplicates();
    return roots;
#else
    return {};
#endif
}

QString outputText(const ProcessResult &result)
{
    QByteArray bytes = result.standardOutput;
    if (!result.standardError.isEmpty()) {
        if (!bytes.isEmpty())
            bytes.append('\n');
        bytes.append(result.standardError);
    }
    return QString::fromLocal8Bit(bytes).trimmed();
}

Plan invalidPlan(const QString &error)
{
    Plan plan;
    plan.errors.append(error);
    return plan;
}

bool machineMatches(const Machine &machine, const ProviderInfo &info, QString *error)
{
    if (!info.available || !QFileInfo(info.executable).isAbsolute()
        || !machine.ref.valid() || machine.ref.providerId != info.id) {
        setError(error, QStringLiteral("VM does not belong to the available provider."));
        return false;
    }
    setError(error, {});
    return true;
}

bool poweredOff(const Machine &machine, QString *error)
{
    if (machine.powerState != PowerState::PoweredOff) {
        setError(error, QStringLiteral("Configuration and media edits require a powered-off VM."));
        return false;
    }
    setError(error, {});
    return true;
}

bool stateIs(const Machine &machine, std::initializer_list<PowerState> states)
{
    return std::find(states.begin(), states.end(), machine.powerState) != states.end();
}

QString unquoteMachineValue(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.front() == QLatin1Char('"')
        && value.back() == QLatin1Char('"')) {
        value = value.mid(1, value.size() - 2);
    }
    QString unescaped;
    unescaped.reserve(value.size());
    for (qsizetype index = 0; index < value.size(); ++index) {
        if (value.at(index) == QLatin1Char('\\') && index + 1 < value.size())
            unescaped.append(value.at(++index));
        else
            unescaped.append(value.at(index));
    }
    return unescaped;
}

QHash<QString, QString> machineValues(const QByteArray &output)
{
    QHash<QString, QString> values;
    const QString text = QString::fromUtf8(output);
    for (const QString &line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                          Qt::SkipEmptyParts)) {
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        values.insert(line.left(equals).trimmed(), unquoteMachineValue(line.mid(equals + 1)));
    }
    return values;
}

PowerState virtualBoxState(const QString &value)
{
    const QString state = value.trimmed().toLower();
    if (state == QStringLiteral("poweroff")) return PowerState::PoweredOff;
    if (state == QStringLiteral("running")) return PowerState::Running;
    if (state == QStringLiteral("paused")) return PowerState::Paused;
    if (state == QStringLiteral("saved")) return PowerState::Saved;
    if (state == QStringLiteral("aborted")) return PowerState::Aborted;
    if (state == QStringLiteral("inaccessible")) return PowerState::Inaccessible;
    return PowerState::Unknown;
}

bool providerBoolean(const QString &value)
{
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("enabled"), Qt::CaseInsensitive) == 0
        || value == QStringLiteral("1");
}

NetworkMode parsedNetworkMode(const QString &value)
{
    if (value.compare(QStringLiteral("nat"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("natnetwork"), Qt::CaseInsensitive) == 0)
        return NetworkMode::Nat;
    if (value.compare(QStringLiteral("bridged"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("bridgednetworking"), Qt::CaseInsensitive) == 0)
        return NetworkMode::Bridged;
    if (value.contains(QStringLiteral("hostonly"), Qt::CaseInsensitive))
        return NetworkMode::HostOnly;
    if (value.compare(QStringLiteral("intnet"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("custom"), Qt::CaseInsensitive) == 0)
        return NetworkMode::Internal;
    return NetworkMode::Disconnected;
}

QString virtualBoxNic(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::Nat: return QStringLiteral("nat");
    case NetworkMode::Bridged: return QStringLiteral("bridged");
    case NetworkMode::HostOnly: return QStringLiteral("hostonly");
    case NetworkMode::Internal: return QStringLiteral("intnet");
    case NetworkMode::Disconnected: return QStringLiteral("none");
    }
    return QStringLiteral("nat");
}

QString normalizedUuid(const QString &value)
{
    const QUuid uuid(value);
    return uuid.isNull() ? QString{} : uuid.toString(QUuid::WithoutBraces);
}

QVersionNumber parsedProviderVersion(const QString &value)
{
    const QRegularExpression expression(QStringLiteral("([0-9]+(?:\\.[0-9]+){1,3})"));
    const QRegularExpressionMatch match = expression.match(value);
    return match.hasMatch() ? QVersionNumber::fromString(match.captured(1)) : QVersionNumber{};
}

int vmwareHardwareVersion(const QVersionNumber &version)
{
    const int major = version.majorVersion();
    if (major >= 17) return 20;
    if (major >= 16) return 18;
    if (major >= 15) return 16;
    if (major >= 14) return 14;
    if (major >= 12) return 12;
    return 10;
}

VmRef virtualBoxTarget(const CreateSpec &spec)
{
    return VmRef{virtualBoxProviderId(), normalizedUuid(spec.id), spec.name};
}

QStringList providerCapabilities(bool includeCreate, bool includeOpen, bool includeRegistration)
{
    QStringList result{
        capability::inventory(), capability::lifecycle(), capability::configure(),
        capability::media(), capability::snapshots(), capability::deleteMachine()};
    if (includeCreate)
        result.append(capability::create());
    if (includeOpen)
        result.append(capability::openConsole());
    if (includeRegistration) {
        result.append(capability::registerMachine());
        result.append(capability::unregisterMachine());
    }
    return result;
}

bool validateCreateSpec(const CreateSpec &spec, const QString &providerId, QString *error)
{
    if (spec.providerId != providerId || !isSafeMachineFileStem(spec.name)
        || spec.guestType.trimmed().isEmpty() || !QFileInfo(spec.directory).isAbsolute()
        || spec.cpuCount < 1 || spec.cpuCount > 64 || spec.memoryMiB < 256
        || spec.memoryMiB > 1024 * 1024 || spec.diskMiB < 1024) {
        setError(error, QStringLiteral("VM create specification is incomplete or outside safe limits."));
        return false;
    }
    if (spec.secureBoot && spec.firmware != Firmware::Efi) {
        setError(error, QStringLiteral("Secure Boot requires EFI firmware."));
        return false;
    }
    if (!spec.isoPath.isEmpty() && !QFileInfo(spec.isoPath).isAbsolute()) {
        setError(error, QStringLiteral("ISO path must be absolute."));
        return false;
    }
    setError(error, {});
    return true;
}

QString vmxTarget(const Machine &machine)
{
    return QFileInfo(machine.configPath).absoluteFilePath();
}

bool providerPathEqual(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return QDir::fromNativeSeparators(QFileInfo(left).absoluteFilePath()).compare(
               QDir::fromNativeSeparators(QFileInfo(right).absoluteFilePath()), sensitivity) == 0;
}

} // namespace

namespace capability {
QString inventory() { return QStringLiteral("inventory"); }
QString create() { return QStringLiteral("create"); }
QString registerMachine() { return QStringLiteral("register"); }
QString openConsole() { return QStringLiteral("open-console"); }
QString lifecycle() { return QStringLiteral("lifecycle"); }
QString configure() { return QStringLiteral("configure"); }
QString media() { return QStringLiteral("media"); }
QString snapshots() { return QStringLiteral("snapshots"); }
QString unregisterMachine() { return QStringLiteral("unregister"); }
QString deleteMachine() { return QStringLiteral("delete"); }
QString secureBoot() { return QStringLiteral("secure-boot"); }
QString tpm() { return QStringLiteral("tpm"); }
} // namespace capability

QList<ProviderProbePaths> ProviderDetector::defaultWindowsCandidates()
{
    const QStringList programRoots = protectedProgramFilesRoots();
    QList<ProviderProbePaths> candidates;
    QSet<QString> seen;
    auto append = [&candidates, &seen](const ProviderProbePaths &candidate) {
        const QString key = candidate.providerId + QChar::Null
            + QDir::fromNativeSeparators(candidate.executable).toCaseFolded();
        if (!seen.contains(key)) {
            seen.insert(key);
            candidates.append(candidate);
        }
    };
    for (const QString &root : std::as_const(programRoots)) {
        const QString virtualBox = QDir(root).filePath(QStringLiteral("Oracle/VirtualBox"));
        append(ProviderProbePaths{
            virtualBoxProviderId(), QDir(virtualBox).filePath(QStringLiteral("VBoxManage.exe")),
            QDir(virtualBox).filePath(QStringLiteral("VirtualBox.exe")), {}, root});

        const QString workstation = QDir(root).filePath(QStringLiteral("VMware/VMware Workstation"));
        append(ProviderProbePaths{
            vmwareWorkstationProviderId(), QDir(workstation).filePath(QStringLiteral("vmrun.exe")),
            QDir(workstation).filePath(QStringLiteral("vmware.exe")),
            QDir(workstation).filePath(QStringLiteral("vmware-vdiskmanager.exe")), root});
        append(ProviderProbePaths{
            vmwarePlayerProviderId(), QDir(workstation).filePath(QStringLiteral("vmrun.exe")),
            QDir(workstation).filePath(QStringLiteral("vmplayer.exe")),
            QDir(workstation).filePath(QStringLiteral("vmware-vdiskmanager.exe")), root});

        const QString player = QDir(root).filePath(QStringLiteral("VMware/VMware Player"));
        append(ProviderProbePaths{
            vmwarePlayerProviderId(), QDir(player).filePath(QStringLiteral("vmrun.exe")),
            QDir(player).filePath(QStringLiteral("vmplayer.exe")),
            QDir(player).filePath(QStringLiteral("vmware-vdiskmanager.exe")), root});
    }
    return candidates;
}

QList<ProviderInfo> ProviderDetector::detect(const QList<ProviderProbePaths> &candidates,
                                             CommandRunner &runner)
{
    QList<ProviderInfo> providers;
    for (const ProviderProbePaths &candidate : candidates) {
        ProviderProbePaths safe = candidate;
        if (!candidate.trustedRoot.isEmpty() && QFileInfo::exists(candidate.executable)) {
            QString pathError;
            if (!trustedInstalledFile(candidate.executable, candidate.trustedRoot,
                                      &safe.executable, &pathError)) {
                ProviderInfo rejected;
                rejected.id = candidate.providerId;
                rejected.warnings.append(QStringLiteral(
                    "Automatic provider probe was rejected: %1").arg(pathError));
                providers.append(rejected);
                continue;
            }
            const auto normalizeOptional = [&candidate, &pathError](
                                               const QString &path,
                                               QString *destination) {
                if (path.isEmpty() || !QFileInfo::exists(path)) {
                    destination->clear();
                    return true;
                }
                return trustedInstalledFile(path, candidate.trustedRoot,
                                            destination, &pathError);
            };
            if (!normalizeOptional(candidate.consoleExecutable, &safe.consoleExecutable)
                || !normalizeOptional(candidate.diskManagerExecutable,
                                      &safe.diskManagerExecutable)) {
                ProviderInfo rejected;
                rejected.id = candidate.providerId;
                rejected.warnings.append(QStringLiteral(
                    "Automatic provider companion probe was rejected: %1").arg(pathError));
                providers.append(rejected);
                continue;
            }
        }
        if (safe.providerId == virtualBoxProviderId()) {
            providers.append(VirtualBoxProvider::detect(
                safe.executable, safe.consoleExecutable, runner));
        } else if (safe.providerId == vmwareWorkstationProviderId()
                   || safe.providerId == vmwarePlayerProviderId()) {
            providers.append(VmwareProvider::detect(
                safe.providerId, safe.executable, safe.consoleExecutable,
                safe.diskManagerExecutable, runner));
        } else {
            ProviderInfo unsupported;
            unsupported.id = safe.providerId;
            unsupported.warnings.append(QStringLiteral("Unknown VM provider probe was ignored."));
            providers.append(unsupported);
        }
    }
    return providers;
}

VirtualBoxProvider::VirtualBoxProvider(ProviderInfo info) : m_info(std::move(info)) {}

ProviderInfo VirtualBoxProvider::detect(const QString &vboxManagePath,
                                        const QString &consolePath,
                                        CommandRunner &runner)
{
    ProviderInfo info;
    info.id = virtualBoxProviderId();
    info.displayName = QStringLiteral("Oracle VM VirtualBox");
    info.executable = QFileInfo(vboxManagePath).absoluteFilePath();
    info.consoleExecutable = consolePath.isEmpty() ? QString{} : QFileInfo(consolePath).absoluteFilePath();
    if (!QFileInfo(vboxManagePath).isAbsolute() || !QFileInfo::exists(vboxManagePath)) {
        info.warnings.append(QStringLiteral("VBoxManage was not found at the probed absolute path."));
        return info;
    }
    const Command probe{info.executable, {QStringLiteral("--version")},
                        QFileInfo(info.executable).absolutePath(), 10000,
                        false, true};
    const ProcessResult result = runner.run(probe);
    info.evidence.append(QStringLiteral("%1 --version").arg(info.executable));
    if (!result.ok()) {
        info.warnings.append(result.error.isEmpty()
                                 ? QStringLiteral("VBoxManage version probe failed with exit code %1.")
                                       .arg(result.exitCode)
                                 : result.error);
        return info;
    }
    info.available = true;
    info.version = outputText(result).section(QRegularExpression(QStringLiteral("[\\r\\n]")), 0, 0).trimmed();
    for (const QString &item : providerCapabilities(true, QFileInfo::exists(consolePath), true))
        info.capabilities.insert(item);
    const QVersionNumber version = parsedProviderVersion(info.version);
    if (!version.isNull() && QVersionNumber::compare(version, QVersionNumber(7, 0)) >= 0)
        info.capabilities.insert(capability::tpm());
    // Secure Boot enrollment requires a running UEFI variable store. Do not
    // advertise a one-step edit until that guided lifecycle is implemented.
    return info;
}

QList<VmRef> VirtualBoxProvider::parseMachineList(const QByteArray &output, QString *error)
{
    QList<VmRef> machines;
    static const QRegularExpression lineExpression(QStringLiteral("^\"((?:\\\\.|[^\"])*)\"\\s+\\{([^{}]+)\\}$"));
    const QString text = QString::fromUtf8(output);
    for (const QString &raw : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts)) {
        const QRegularExpressionMatch match = lineExpression.match(raw.trimmed());
        if (!match.hasMatch()) {
            setError(error, QStringLiteral("Malformed VBoxManage list vms line: %1").arg(raw));
            return {};
        }
        const QString uuid = normalizedUuid(match.captured(2));
        if (uuid.isEmpty()) {
            setError(error, QStringLiteral("VBoxManage returned an invalid VM UUID."));
            return {};
        }
        machines.append(VmRef{virtualBoxProviderId(), uuid, unquoteMachineValue(
                                  QLatin1Char('"') + match.captured(1) + QLatin1Char('"'))});
    }
    setError(error, {});
    return machines;
}

std::optional<Machine> VirtualBoxProvider::parseMachineInfo(const QByteArray &output,
                                                            QString *error)
{
    const QHash<QString, QString> values = machineValues(output);
    const QString uuid = normalizedUuid(values.value(QStringLiteral("UUID")));
    const QString name = values.value(QStringLiteral("name"));
    if (uuid.isEmpty() || name.trimmed().isEmpty()) {
        setError(error, QStringLiteral("VBoxManage machine-readable info lacks UUID or name."));
        return std::nullopt;
    }
    Machine machine;
    machine.ref = VmRef{virtualBoxProviderId(), uuid, name};
    machine.configPath = values.value(QStringLiteral("CfgFile"));
    machine.powerState = virtualBoxState(values.value(QStringLiteral("VMState")));
    bool integerOk = false;
    const int cpus = values.value(QStringLiteral("cpus")).toInt(&integerOk);
    if (integerOk && cpus > 0)
        machine.cpuCount = cpus;
    const int memory = values.value(QStringLiteral("memory")).toInt(&integerOk);
    if (integerOk && memory > 0)
        machine.memoryMiB = memory;
    const QString firmware = values.value(QStringLiteral("firmware"));
    if (!firmware.isEmpty())
        machine.firmware = firmware.contains(QStringLiteral("efi"), Qt::CaseInsensitive)
            ? Firmware::Efi : Firmware::Bios;
    const QString secureBoot = values.value(QStringLiteral("SecureBoot"),
        values.value(QStringLiteral("secureboot")));
    if (!secureBoot.isEmpty())
        machine.secureBoot = providerBoolean(secureBoot);
    const QString tpmType = values.value(QStringLiteral("TPMType"),
        values.value(QStringLiteral("TPM")));
    if (!tpmType.isEmpty())
        machine.tpm = tpmType.compare(QStringLiteral("none"), Qt::CaseInsensitive) != 0
            && tpmType.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) != 0;
    struct ControllerTopology {
        QString name;
        QString bus;
        int index = 0;
    };
    QList<ControllerTopology> controllers;
    for (int index = 0; index < 16; ++index) {
        const QString suffix = QString::number(index);
        const QString controllerName = values.value(
            QStringLiteral("storagecontrollername") + suffix);
        if (controllerName.isEmpty())
            continue;
        const QString type = values.value(QStringLiteral("storagecontrollertype") + suffix);
        QString bus = QStringLiteral("scsi");
        if (type.contains(QStringLiteral("ahci"), Qt::CaseInsensitive))
            bus = QStringLiteral("sata");
        else if (type.contains(QStringLiteral("piix"), Qt::CaseInsensitive)
                 || type.contains(QStringLiteral("ich6"), Qt::CaseInsensitive))
            bus = QStringLiteral("ide");
        else if (type.contains(QStringLiteral("nvme"), Qt::CaseInsensitive))
            bus = QStringLiteral("nvme");
        else if (type.contains(QStringLiteral("sas"), Qt::CaseInsensitive))
            bus = QStringLiteral("sas");
        controllers.append(ControllerTopology{controllerName, bus, index});
    }
    static const QRegularExpression storageKey(
        QStringLiteral("^(.+)-([0-9]+)-([0-9]+)$"),
        QRegularExpression::CaseInsensitiveOption);
    for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
        const QRegularExpressionMatch match = storageKey.match(iterator.key());
        if (!match.hasMatch() || !QFileInfo(iterator.value()).isAbsolute())
            continue;
        const QString controllerText = match.captured(1);
        const auto knownController = std::find_if(
            controllers.cbegin(), controllers.cend(),
            [&controllerText](const ControllerTopology &controller) {
                return controller.name.compare(controllerText, Qt::CaseInsensitive) == 0;
            });
        QString bus = controllerText.toLower();
        int controllerIndex = 0;
        QString controllerName = controllerText;
        if (knownController != controllers.cend()) {
            bus = knownController->bus;
            controllerIndex = knownController->index;
            controllerName = knownController->name;
        } else if (bus != QStringLiteral("ide") && bus != QStringLiteral("sata")
                   && bus != QStringLiteral("scsi") && bus != QStringLiteral("sas")
                   && bus != QStringLiteral("nvme")) {
            continue;
        }
        StorageAttachment attachment;
        attachment.id = iterator.key();
        attachment.bus = bus;
        attachment.controller = controllerIndex;
        attachment.controllerName = controllerName;
        attachment.port = match.captured(2).toInt();
        attachment.device = match.captured(3).toInt();
        attachment.path = QFileInfo(iterator.value()).absoluteFilePath();
        attachment.optical = QFileInfo(attachment.path).suffix().compare(
            QStringLiteral("iso"), Qt::CaseInsensitive) == 0;
        machine.storageDevices.append(attachment);
    }
    std::sort(machine.storageDevices.begin(), machine.storageDevices.end(),
              [](const StorageAttachment &left, const StorageAttachment &right) {
        const int controllerNameOrder = left.controllerName.compare(
            right.controllerName, Qt::CaseInsensitive);
        if (controllerNameOrder != 0)
            return controllerNameOrder < 0;
        if (left.controller != right.controller)
            return left.controller < right.controller;
        if (left.port != right.port)
            return left.port < right.port;
        if (left.device != right.device)
            return left.device < right.device;
        if (left.optical != right.optical)
            return !left.optical;
        return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
    });
    for (const StorageAttachment &attachment : std::as_const(machine.storageDevices)) {
        if (!attachment.optical)
            machine.storagePaths.append(attachment.path);
    }
    for (int slot = 1; slot <= 36; ++slot) {
        const QString suffix = QString::number(slot);
        const QString modeText = values.value(QStringLiteral("nic") + suffix);
        if (modeText.isEmpty() || modeText.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
            continue;
        NetworkAttachment attachment;
        attachment.id = QStringLiteral("nic%1").arg(slot);
        attachment.slot = slot;
        attachment.mode = parsedNetworkMode(modeText);
        attachment.model = values.value(QStringLiteral("nictype") + suffix);
        attachment.macAddress = values.value(QStringLiteral("macaddress") + suffix);
        attachment.connected = providerBoolean(
            values.value(QStringLiteral("cableconnected") + suffix, QStringLiteral("on")));
        if (attachment.mode == NetworkMode::Bridged)
            attachment.interfaceName = values.value(QStringLiteral("bridgeadapter") + suffix);
        else if (attachment.mode == NetworkMode::HostOnly)
            attachment.interfaceName = values.value(QStringLiteral("hostonlyadapter") + suffix);
        else if (attachment.mode == NetworkMode::Internal)
            attachment.interfaceName = values.value(QStringLiteral("intnet") + suffix);
        machine.networkDevices.append(attachment);
    }
    if (values.value(QStringLiteral("accessible")).compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
        machine.powerState = PowerState::Inaccessible;
        machine.inaccessibleReason = values.value(QStringLiteral("accessError"));
    }
    machine.stateRevision = commandEvidence(EvidenceFormat::RawSha256, output);
    machine.inventoryComplete = true;
    machine.hardwareInventoryComplete = true;
    setError(error, {});
    return machine;
}

QList<Snapshot> VirtualBoxProvider::parseSnapshotList(const QByteArray &output, QString *error)
{
    const QHash<QString, QString> values = machineValues(output);
    QSet<QString> suffixes;
    for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
        if (iterator.key() == QStringLiteral("SnapshotName"))
            suffixes.insert(QString{});
        else if (iterator.key().startsWith(QStringLiteral("SnapshotName-")))
            suffixes.insert(iterator.key().mid(QStringLiteral("SnapshotName").size()));
    }
    QList<QString> ordered = suffixes.values();
    std::sort(ordered.begin(), ordered.end());
    QList<Snapshot> snapshots;
    const QString current = normalizedUuid(values.value(QStringLiteral("CurrentSnapshotUUID")));
    for (const QString &suffix : std::as_const(ordered)) {
        Snapshot snapshot;
        snapshot.name = values.value(QStringLiteral("SnapshotName") + suffix);
        snapshot.id = normalizedUuid(values.value(QStringLiteral("SnapshotUUID") + suffix));
        snapshot.description = values.value(QStringLiteral("SnapshotDescription") + suffix);
        snapshot.createdAt = QDateTime::fromString(
            values.value(QStringLiteral("SnapshotTimeStamp") + suffix), Qt::ISODateWithMs);
        snapshot.current = !current.isEmpty() && snapshot.id == current;
        if (snapshot.name.isEmpty() || snapshot.id.isEmpty()) {
            setError(error, QStringLiteral("VBoxManage returned incomplete snapshot metadata."));
            return {};
        }
        snapshots.append(snapshot);
    }
    setError(error, {});
    return snapshots;
}

Command VirtualBoxProvider::command(const QStringList &arguments, int timeoutMs) const
{
    return Command{m_info.executable, arguments,
                   QFileInfo(m_info.executable).absolutePath(), timeoutMs};
}

void VirtualBoxProvider::addPreflight(Plan &plan, const Machine &machine) const
{
    if (machine.stateRevision.isEmpty()) {
        plan.errors.append(QStringLiteral("Refresh VirtualBox machine state before reviewing this operation."));
        return;
    }
    plan.preflight.append(CommandEvidence{
        machineInfoCommand(machine.ref), EvidenceFormat::RawSha256,
        machine.stateRevision, QStringLiteral("VirtualBox machine state/configuration")});
    QString fileError;
    if (!addFileEvidence(plan, machine.configPath,
                         QStringLiteral("VirtualBox machine configuration"), &fileError))
        plan.errors.append(fileError);
}

Command VirtualBoxProvider::inventoryCommand() const
{
    Command result = command({QStringLiteral("list"), QStringLiteral("vms")});
    result.interruptible = true;
    return result;
}

Command VirtualBoxProvider::machineInfoCommand(const VmRef &machine) const
{
    Command result = command(
        {QStringLiteral("showvminfo"), machine.id, QStringLiteral("--machinereadable")});
    result.interruptible = true;
    return result;
}

Plan VirtualBoxProvider::create(const CreateSpec &spec, const QString &revision,
                                const QDateTime &now) const
{
    QString validationError;
    if (!m_info.supports(capability::create()) || !QFileInfo(m_info.executable).isAbsolute())
        return invalidPlan(QStringLiteral("VirtualBox create capability is unavailable."));
    if (!validateCreateSpec(spec, virtualBoxProviderId(), &validationError))
        return invalidPlan(validationError);
    const VmRef target = virtualBoxTarget(spec);
    if (!target.valid())
        return invalidPlan(QStringLiteral("VirtualBox create requires a reviewed VM UUID."));
    if (spec.secureBoot)
        return invalidPlan(QStringLiteral("Detected VirtualBox capability evidence does not prove Secure Boot support."));
    if (spec.tpm && !m_info.supports(capability::tpm()))
        return invalidPlan(QStringLiteral("Detected VirtualBox capability evidence does not prove TPM support."));

    const QString machineDirectory = QDir(spec.directory).filePath(spec.name);
    const QString diskPath = QDir(machineDirectory).filePath(spec.name + QStringLiteral(".vdi"));
    QList<Command> commands;
    commands.append(command({QStringLiteral("createvm"), QStringLiteral("--name"), spec.name,
                             QStringLiteral("--uuid"), target.id, QStringLiteral("--basefolder"),
                             spec.directory, QStringLiteral("--ostype"), spec.guestType,
                             QStringLiteral("--register")}));
    QStringList modify{QStringLiteral("modifyvm"), target.id,
                       QStringLiteral("--cpus"), QString::number(spec.cpuCount),
                       QStringLiteral("--memory"), QString::number(spec.memoryMiB),
                       QStringLiteral("--firmware"), firmwareName(spec.firmware),
                       QStringLiteral("--nic1"), virtualBoxNic(spec.networkMode),
                       QStringLiteral("--boot1"), spec.unattendedBoot ? QStringLiteral("dvd")
                                                                     : QStringLiteral("disk")};
    if (m_info.supports(capability::tpm()))
        modify.append({QStringLiteral("--tpm-type"), spec.tpm ? QStringLiteral("2.0")
                                                              : QStringLiteral("none")});
    if (spec.networkMode == NetworkMode::Bridged && !spec.bridgedInterface.isEmpty())
        modify.append({QStringLiteral("--bridgeadapter1"), spec.bridgedInterface});
    else if (spec.networkMode == NetworkMode::HostOnly && !spec.bridgedInterface.isEmpty())
        modify.append({QStringLiteral("--hostonlyadapter1"), spec.bridgedInterface});
    else if (spec.networkMode == NetworkMode::Internal && !spec.bridgedInterface.isEmpty())
        modify.append({QStringLiteral("--intnet1"), spec.bridgedInterface});
    commands.append(command(modify));
    commands.append(command({QStringLiteral("createmedium"), QStringLiteral("disk"),
                             QStringLiteral("--filename"), diskPath, QStringLiteral("--size"),
                             QString::number(spec.diskMiB), QStringLiteral("--format"),
                             QStringLiteral("VDI")}, 10 * 60 * 1000));
    commands.append(command({QStringLiteral("storagectl"), target.id, QStringLiteral("--name"),
                             QStringLiteral("SATA"), QStringLiteral("--add"), QStringLiteral("sata"),
                             QStringLiteral("--controller"), QStringLiteral("IntelAhci")}));
    commands.append(command({QStringLiteral("storageattach"), target.id,
                             QStringLiteral("--storagectl"), QStringLiteral("SATA"),
                             QStringLiteral("--port"), QStringLiteral("0"),
                             QStringLiteral("--device"), QStringLiteral("0"),
                             QStringLiteral("--type"), QStringLiteral("hdd"),
                             QStringLiteral("--medium"), diskPath}));
    if (!spec.isoPath.isEmpty()) {
        commands.append(command({QStringLiteral("storageattach"), target.id,
                                 QStringLiteral("--storagectl"), QStringLiteral("SATA"),
                                 QStringLiteral("--port"), QStringLiteral("1"),
                                 QStringLiteral("--device"), QStringLiteral("0"),
                                 QStringLiteral("--type"), QStringLiteral("dvddrive"),
                                 QStringLiteral("--medium"), spec.isoPath}));
    }
    Plan plan;
    plan.preview = makePreview(QStringLiteral("create"), target, Risk::Reversible,
                               {QStringLiteral("Create and register a VirtualBox VM under %1.")
                                    .arg(spec.directory)},
                               {}, commands, revision, now);
    if (!spec.isoPath.isEmpty()
        && !addFileEvidence(plan, spec.isoPath,
                            QStringLiteral("VirtualBox installation ISO"), &validationError))
        plan.errors.append(validationError);
    return plan;
}

Plan VirtualBoxProvider::registerMachine(const QString &vboxPath, const QString &name,
                                         const QString &revision, const QDateTime &now) const
{
    if (!m_info.supports(capability::registerMachine())
        || !QFileInfo(m_info.executable).isAbsolute() || !QFileInfo(vboxPath).isAbsolute())
        return invalidPlan(QStringLiteral("VirtualBox registration requires an absolute .vbox path."));
    constexpr qint64 MaxVirtualBoxConfigBytes = 16 * 1024 * 1024;
    const QFileInfo configuration(vboxPath);
    if (!configuration.exists() || !configuration.isFile() || configuration.isSymLink()
#ifdef Q_OS_WIN
        || configuration.isJunction()
#endif
        || configuration.size() < 0 || configuration.size() > MaxVirtualBoxConfigBytes) {
        return invalidPlan(QStringLiteral(
            "VirtualBox registration requires a regular configuration no larger than 16 MiB."));
    }
    QFile file(vboxPath);
    if (!file.open(QIODevice::ReadOnly))
        return invalidPlan(file.errorString());
    const QString xml = QString::fromUtf8(file.readAll());
    const QRegularExpression uuidExpression(QStringLiteral("\\buuid=\"\\{([^{}]+)\\}\""));
    const QString uuid = normalizedUuid(uuidExpression.match(xml).captured(1));
    const VmRef target{virtualBoxProviderId(), uuid, name};
    if (!target.valid())
        return invalidPlan(QStringLiteral("The .vbox file does not expose a valid machine UUID."));
    Plan plan;
    plan.preview = makePreview(QStringLiteral("register"), target, Risk::Reversible,
                               {QStringLiteral("Register the existing .vbox file without deleting or moving it.")},
                               {}, {command({QStringLiteral("registervm"), vboxPath})}, revision, now);
    QString evidenceError;
    if (!addFileEvidence(plan, vboxPath,
                         QStringLiteral("VirtualBox registration file"), &evidenceError))
        plan.errors.append(evidenceError);
    return plan;
}

Plan VirtualBoxProvider::openConsole(const Machine &machine, const QString &revision,
                                     const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !m_info.supports(capability::openConsole())
        || !QFileInfo(m_info.consoleExecutable).isAbsolute())
        return invalidPlan(error.isEmpty() ? QStringLiteral("VirtualBox console capability is unavailable.") : error);
    const Command open{m_info.consoleExecutable,
                       {QStringLiteral("--startvm"), machine.ref.id}, {}, 30000, true};
    Plan plan;
    plan.preview = makePreview(QStringLiteral("open-console"), machine.ref, Risk::Disruptive,
                               {QStringLiteral("Open the provider console and start the VM if needed.")},
                               {}, {open}, revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VirtualBoxProvider::start(const Machine &machine, bool headless, const QString &revision,
                               const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error)
        || !stateIs(machine, {PowerState::PoweredOff, PowerState::Saved}))
        return invalidPlan(error.isEmpty() ? QStringLiteral("VirtualBox start requires a powered-off VM.") : error);
    Plan plan;
    plan.preview = makePreview(QStringLiteral("start"), machine.ref, Risk::Disruptive,
                               {QStringLiteral("Start the virtual machine.")}, {},
                               {command({QStringLiteral("startvm"), machine.ref.id,
                                         QStringLiteral("--type"),
                                         headless ? QStringLiteral("headless") : QStringLiteral("gui")})},
                               revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VirtualBoxProvider::control(const Machine &machine, const QString &verb, Risk risk,
                                 const QString &effect, const QString &revision,
                                 const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !m_info.supports(capability::lifecycle()))
        return invalidPlan(error.isEmpty() ? QStringLiteral("VirtualBox lifecycle capability is unavailable.") : error);
    Plan plan;
    plan.preview = makePreview(verb, machine.ref, risk, {effect}, {},
                               {command({QStringLiteral("controlvm"), machine.ref.id, verb})},
                               revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VirtualBoxProvider::gracefulShutdown(const Machine &machine, const QString &revision,
                                          const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Graceful shutdown requires a running or paused VM."));
    return control(machine, QStringLiteral("acpipowerbutton"), Risk::Disruptive,
                   QStringLiteral("Request an ACPI guest shutdown; the guest may decline."), revision, now);
}

Plan VirtualBoxProvider::powerOff(const Machine &machine, const QString &revision,
                                  const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Power off requires a running or paused VM."));
    Plan plan = control(machine, QStringLiteral("poweroff"), Risk::Disruptive,
                        QStringLiteral("Immediately cut VM power; unsaved guest data may be lost."),
                        revision, now);
    plan.preview.warnings.append(QStringLiteral("This is equivalent to removing power from a physical machine."));
    return plan;
}

Plan VirtualBoxProvider::pause(const Machine &machine, const QString &revision,
                               const QDateTime &now) const
{
    if (machine.powerState != PowerState::Running)
        return invalidPlan(QStringLiteral("Pause requires a running VM."));
    return control(machine, QStringLiteral("pause"), Risk::Reversible,
                   QStringLiteral("Pause VM execution in memory."), revision, now);
}

Plan VirtualBoxProvider::resume(const Machine &machine, const QString &revision,
                                const QDateTime &now) const
{
    if (machine.powerState != PowerState::Paused)
        return invalidPlan(QStringLiteral("Resume requires a paused VM."));
    return control(machine, QStringLiteral("resume"), Risk::Reversible,
                   QStringLiteral("Resume a paused VM."), revision, now);
}

Plan VirtualBoxProvider::reset(const Machine &machine, const QString &revision,
                               const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Reset requires a running or paused VM."));
    return control(machine, QStringLiteral("reset"), Risk::Disruptive,
                   QStringLiteral("Reset the VM immediately; unsaved guest data may be lost."), revision, now);
}

Plan VirtualBoxProvider::saveState(const Machine &machine, const QString &revision,
                                   const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Save state requires a running or paused VM."));
    return control(machine, QStringLiteral("savestate"), Risk::Reversible,
                   QStringLiteral("Save VM memory and device state, then stop execution."), revision, now);
}

Plan VirtualBoxProvider::configure(const Machine &machine, const ConfigPatch &patch,
                                   const QString &revision, const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !poweredOff(machine, &error))
        return invalidPlan(error);
    if (!m_info.supports(capability::configure()))
        return invalidPlan(QStringLiteral("VirtualBox configure capability is unavailable."));
    if (patch.empty())
        return invalidPlan(QStringLiteral("Configuration patch is empty."));
    if ((patch.cpuCount && (*patch.cpuCount < 1 || *patch.cpuCount > 64))
        || (patch.memoryMiB && (*patch.memoryMiB < 256 || *patch.memoryMiB > 1024 * 1024)))
        return invalidPlan(QStringLiteral("CPU or memory patch is outside safe limits."));
    if (patch.secureBoot)
        return invalidPlan(QStringLiteral("VirtualBox Secure Boot changes require the guided UEFI enrollment workflow."));
    if (patch.tpm && !m_info.supports(capability::tpm()))
        return invalidPlan(QStringLiteral("Detected VirtualBox capabilities do not prove TPM editing support."));
    QStringList arguments{QStringLiteral("modifyvm"), machine.ref.id};
    if (patch.cpuCount) arguments.append({QStringLiteral("--cpus"), QString::number(*patch.cpuCount)});
    if (patch.memoryMiB) arguments.append({QStringLiteral("--memory"), QString::number(*patch.memoryMiB)});
    if (patch.firmware) arguments.append({QStringLiteral("--firmware"), firmwareName(*patch.firmware)});
    if (patch.tpm) arguments.append({QStringLiteral("--tpm-type"), *patch.tpm
                                     ? QStringLiteral("2.0") : QStringLiteral("none")});
    if (patch.networkMode) arguments.append({QStringLiteral("--nic1"), virtualBoxNic(*patch.networkMode)});
    if (patch.bridgedInterface && !patch.bridgedInterface->isEmpty()) {
        if (!patch.networkMode)
            return invalidPlan(QStringLiteral("A network interface patch must include its reviewed network mode."));
        if (*patch.networkMode == NetworkMode::Bridged)
            arguments.append({QStringLiteral("--bridgeadapter1"), *patch.bridgedInterface});
        else if (*patch.networkMode == NetworkMode::HostOnly)
            arguments.append({QStringLiteral("--hostonlyadapter1"), *patch.bridgedInterface});
        else if (*patch.networkMode == NetworkMode::Internal)
            arguments.append({QStringLiteral("--intnet1"), *patch.bridgedInterface});
        else
            return invalidPlan(QStringLiteral("The selected network mode does not accept an interface name."));
    }
    QList<Command> commands;
    if (arguments.size() > 2)
        commands.append(command(arguments));
    if (patch.isoPath) {
        const QString medium = patch.isoPath->isEmpty() ? QStringLiteral("none") : *patch.isoPath;
        if (!patch.isoPath->isEmpty() && !QFileInfo(*patch.isoPath).isAbsolute())
            return invalidPlan(QStringLiteral("ISO path must be absolute."));
        commands.append(command({QStringLiteral("storageattach"), machine.ref.id,
                                 QStringLiteral("--storagectl"), QStringLiteral("SATA"),
                                 QStringLiteral("--port"), QStringLiteral("1"),
                                 QStringLiteral("--device"), QStringLiteral("0"),
                                 QStringLiteral("--type"), QStringLiteral("dvddrive"),
                                 QStringLiteral("--medium"), medium}));
    }
    Plan plan;
    plan.preview = makePreview(QStringLiteral("configure"), machine.ref, Risk::Reversible,
                               {QStringLiteral("Update powered-off VirtualBox settings.")},
                               {}, commands, revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VirtualBoxProvider::attachIso(const Machine &machine, const QString &isoPath,
                                   const QString &revision, const QDateTime &now) const
{
    ConfigPatch patch;
    patch.isoPath = isoPath;
    Plan plan = configure(machine, patch, revision, now);
    plan.preview.action = QStringLiteral("attach-iso");
    QString evidenceError;
    if (plan.ok() && !addFileEvidence(plan, isoPath,
                                      QStringLiteral("VirtualBox attached ISO"),
                                      &evidenceError))
        plan.errors.append(evidenceError);
    return plan;
}

Plan VirtualBoxProvider::detachIso(const Machine &machine, const QString &revision,
                                   const QDateTime &now) const
{
    ConfigPatch patch;
    patch.isoPath = QString{};
    Plan plan = configure(machine, patch, revision, now);
    plan.preview.action = QStringLiteral("detach-iso");
    return plan;
}

Plan VirtualBoxProvider::listSnapshots(const Machine &machine, const QString &revision,
                                       const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !m_info.supports(capability::snapshots()))
        return invalidPlan(error.isEmpty() ? QStringLiteral("VirtualBox snapshot capability is unavailable.") : error);
    Plan plan;
    Command list = command({QStringLiteral("snapshot"), machine.ref.id,
                            QStringLiteral("list"), QStringLiteral("--machinereadable")});
    list.interruptible = true;
    plan.preview = makePreview(QStringLiteral("list-snapshots"), machine.ref, Risk::ReadOnly,
                               {QStringLiteral("Read provider snapshot inventory.")}, {},
                               {list},
                               revision, now);
    return plan;
}

Plan VirtualBoxProvider::takeSnapshot(const Machine &machine, const QString &name,
                                      const QString &description, const QString &revision,
                                      const QDateTime &now) const
{
    if (name.trimmed().isEmpty()) return invalidPlan(QStringLiteral("Snapshot name is required."));
    QString error;
    if (!machineMatches(machine, m_info, &error) || !m_info.supports(capability::snapshots()))
        return invalidPlan(error.isEmpty() ? QStringLiteral("VirtualBox snapshot capability is unavailable.") : error);
    Plan plan;
    plan.preview = makePreview(QStringLiteral("take-snapshot"), machine.ref, Risk::Reversible,
                               {QStringLiteral("Create a provider snapshot named '%1'.").arg(name)}, {},
                               {command({QStringLiteral("snapshot"), machine.ref.id, QStringLiteral("take"),
                                         name, QStringLiteral("--description"), description})},
                               revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VirtualBoxProvider::restoreSnapshot(const Machine &machine, const Snapshot &snapshot,
                                         const QString &revision, const QDateTime &now) const
{
    if (snapshot.id.isEmpty() || snapshot.inventoryRevision.isEmpty())
        return invalidPlan(QStringLiteral("Refresh snapshot inventory before restoring this snapshot."));
    QString error;
    if (!machineMatches(machine, m_info, &error)
        || !m_info.supports(capability::snapshots())
        || machine.powerState != PowerState::PoweredOff)
        return invalidPlan(error.isEmpty() ? QStringLiteral("Snapshot restore requires a powered-off VM.") : error);
    Plan plan;
    plan.preview = makePreview(QStringLiteral("restore-snapshot"), machine.ref, Risk::Destructive,
                               {QStringLiteral("Restore snapshot '%1' and discard newer current state.")
                                    .arg(snapshot.name)},
                               {QStringLiteral("Current VM state after the snapshot may be lost.")},
                               {command({QStringLiteral("snapshot"), machine.ref.id,
                                         QStringLiteral("restore"), snapshot.id})}, revision, now);
    addPreflight(plan, machine);
    plan.preflight.append(CommandEvidence{
        command({QStringLiteral("snapshot"), machine.ref.id,
                 QStringLiteral("list"), QStringLiteral("--machinereadable")}),
        EvidenceFormat::RawSha256, snapshot.inventoryRevision,
        QStringLiteral("VirtualBox snapshot inventory")});
    plan.preflight.last().command.interruptible = true;
    return plan;
}

Plan VirtualBoxProvider::deleteSnapshot(const Machine &machine, const Snapshot &snapshot,
                                        const QString &revision, const QDateTime &now) const
{
    if (snapshot.id.isEmpty() || snapshot.inventoryRevision.isEmpty())
        return invalidPlan(QStringLiteral("Refresh snapshot inventory before deleting this snapshot."));
    QString error;
    if (!machineMatches(machine, m_info, &error)
        || !m_info.supports(capability::snapshots())
        || machine.powerState != PowerState::PoweredOff)
        return invalidPlan(error.isEmpty() ? QStringLiteral("Snapshot deletion requires a powered-off VM.") : error);
    Plan plan;
    plan.preview = makePreview(QStringLiteral("delete-snapshot"), machine.ref, Risk::Destructive,
                               {QStringLiteral("Permanently delete snapshot '%1'.").arg(snapshot.name)},
                               {QStringLiteral("Snapshot rollback data cannot be recovered by WimForge.")},
                               {command({QStringLiteral("snapshot"), machine.ref.id,
                                         QStringLiteral("delete"), snapshot.id}, 10 * 60 * 1000)},
                               revision, now);
    addPreflight(plan, machine);
    plan.preflight.append(CommandEvidence{
        command({QStringLiteral("snapshot"), machine.ref.id,
                 QStringLiteral("list"), QStringLiteral("--machinereadable")}),
        EvidenceFormat::RawSha256, snapshot.inventoryRevision,
        QStringLiteral("VirtualBox snapshot inventory")});
    plan.preflight.last().command.interruptible = true;
    return plan;
}

Plan VirtualBoxProvider::unregisterMachine(const Machine &machine, const QString &revision,
                                           const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || machine.powerState != PowerState::PoweredOff)
        return invalidPlan(error.isEmpty() ? QStringLiteral("Unregister requires a powered-off VM.") : error);
    if (machine.ownership != Ownership::External) {
        return invalidPlan(QStringLiteral(
            "Managed VMs cannot be unregistered while their owned files remain. "
            "Use the guarded delete operation instead."));
    }
    Plan plan;
    plan.preview = makePreview(QStringLiteral("unregister"), machine.ref, Risk::Reversible,
                               {QStringLiteral("Unregister the VM while preserving every provider file.")}, {},
                               {command({QStringLiteral("unregistervm"), machine.ref.id})}, revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VirtualBoxProvider::deleteMachine(const Machine &machine, const QString &managedRoot,
                                       const QList<Machine> &catalogMachines,
                                       const QString &revision, const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || machine.powerState != PowerState::PoweredOff)
        return invalidPlan(error.isEmpty() ? QStringLiteral("Delete requires a powered-off VM.") : error);
    const DeletionGuard guard = PathPolicy::managedDeletionGuard(machine, managedRoot, catalogMachines);
    if (!guard.allowed) return invalidPlan(guard.error);
    Plan plan;
    plan.preview = makePreview(QStringLiteral("delete"), machine.ref, Risk::Destructive,
                               {QStringLiteral("Unregister the VM and delete its guarded managed directory %1.")
                                    .arg(guard.canonicalDirectory)},
                               {QStringLiteral("Virtual disks and snapshots will be permanently deleted.")},
                               {command({QStringLiteral("unregistervm"), machine.ref.id},
                                        10 * 60 * 1000)},
                               revision, now);
    // Never delegate filesystem deletion to VBoxManage --delete. Re-run the
    // managed-root, shared-storage, and reparse-point guards immediately after
    // unregistering, then remove only the canonical contained directory.
    plan.managedDeletionAfterCommands = ManagedDeletion{
        machine, managedRoot, catalogMachines, guard.identity,
        guard.rootIdentity, true};
    addPreflight(plan, machine);
    return plan;
}

VmwareProvider::VmwareProvider(ProviderInfo info) : m_info(std::move(info)) {}

ProviderInfo VmwareProvider::detect(const QString &providerId,
                                    const QString &vmrunPath,
                                    const QString &consolePath,
                                    const QString &diskManagerPath,
                                    CommandRunner &runner)
{
    ProviderInfo info;
    info.id = providerId;
    info.displayName = providerId == vmwarePlayerProviderId()
        ? QStringLiteral("VMware Workstation Player") : QStringLiteral("VMware Workstation Pro");
    info.executable = QFileInfo(vmrunPath).absoluteFilePath();
    info.consoleExecutable = consolePath.isEmpty() ? QString{} : QFileInfo(consolePath).absoluteFilePath();
    info.diskManagerExecutable = diskManagerPath.isEmpty()
        ? QString{} : QFileInfo(diskManagerPath).absoluteFilePath();
    if ((providerId != vmwareWorkstationProviderId() && providerId != vmwarePlayerProviderId())
        || !QFileInfo(vmrunPath).isAbsolute() || !QFileInfo::exists(vmrunPath)) {
        info.warnings.append(QStringLiteral("vmrun was not found for the requested VMware provider."));
        return info;
    }
    const QString target = providerId == vmwarePlayerProviderId()
        ? QStringLiteral("player") : QStringLiteral("ws");
    const ProcessResult inventoryProbe = runner.run(
        Command{info.executable, {QStringLiteral("-T"), target, QStringLiteral("list")},
                QFileInfo(info.executable).absolutePath(),
                10000, false, true});
    info.evidence.append(QStringLiteral("%1 -T %2 list").arg(info.executable, target));
    if (!inventoryProbe.ok()) {
        info.warnings.append(inventoryProbe.error.isEmpty()
                                 ? QStringLiteral("vmrun provider probe failed with exit code %1.")
                                       .arg(inventoryProbe.exitCode)
                                 : inventoryProbe.error);
        return info;
    }
    info.available = true;
    // vmrun prints its version in the usage banner. A non-zero usage exit is
    // acceptable evidence only for version text, never for availability.
    const ProcessResult versionProbe = runner.run(
        Command{info.executable, {}, QFileInfo(info.executable).absolutePath(),
                10000, false, true});
    info.evidence.append(QStringLiteral("%1 [usage/version banner]").arg(info.executable));
    const QString banner = outputText(versionProbe);
    const QRegularExpression versionExpression(
        QStringLiteral("(?i)(?:version|vmrun)\\s+([0-9]+(?:\\.[0-9]+)+)"));
    const QRegularExpressionMatch versionMatch = versionExpression.match(banner);
    info.version = versionMatch.hasMatch() ? versionMatch.captured(1) : QStringLiteral("unknown");
    const QVersionNumber version = parsedProviderVersion(info.version);
    const bool canCreate = QFileInfo(info.diskManagerExecutable).isAbsolute()
        && QFileInfo::exists(info.diskManagerExecutable);
    const bool canOpen = QFileInfo(info.consoleExecutable).isAbsolute()
        && QFileInfo::exists(info.consoleExecutable);
    info.capabilities.insert(capability::inventory());
    info.capabilities.insert(capability::configure());
    info.capabilities.insert(capability::media());
    info.capabilities.insert(capability::deleteMachine());
    if (canCreate) info.capabilities.insert(capability::create());
    if (canOpen) info.capabilities.insert(capability::openConsole());
    const bool lifecycleProven = banner.contains(
        QRegularExpression(QStringLiteral("\\bstart\\b.*\\bstop\\b|\\bstop\\b.*\\bstart\\b"),
                           QRegularExpression::CaseInsensitiveOption
                               | QRegularExpression::DotMatchesEverythingOption));
    const bool snapshotsProven = banner.contains(
        QRegularExpression(QStringLiteral("\\blistSnapshots\\b.*\\bsnapshot\\b|\\bsnapshot\\b.*\\blistSnapshots\\b"),
                           QRegularExpression::CaseInsensitiveOption
                               | QRegularExpression::DotMatchesEverythingOption));
    if (lifecycleProven) info.capabilities.insert(capability::lifecycle());
    if (snapshotsProven) info.capabilities.insert(capability::snapshots());
    if (!version.isNull() && version.majorVersion() >= 14)
        info.capabilities.insert(capability::secureBoot());
    if (!lifecycleProven)
        info.warnings.append(QStringLiteral("vmrun usage evidence did not prove lifecycle commands; they are disabled."));
    if (!snapshotsProven)
        info.warnings.append(QStringLiteral("vmrun usage evidence did not prove snapshot commands; they are disabled."));
    if (!canCreate)
        info.warnings.append(QStringLiteral("vmware-vdiskmanager is absent; create is disabled while management remains available."));
    if (!canOpen)
        info.warnings.append(QStringLiteral("Provider console executable is absent; open-console is disabled."));
    // Never advertise register/unregister: vmrun has no safe registration API.
    return info;
}

QStringList VmwareProvider::parseRunningVmList(const QByteArray &output, QString *error)
{
    QStringList paths;
    const QStringList lines = QString::fromLocal8Bit(output).split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        setError(error, QStringLiteral("vmrun list output is empty."));
        return {};
    }
    const QRegularExpression header(QStringLiteral("^Total running VMs:\\s*([0-9]+)$"),
                                    QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch headerMatch = header.match(lines.first().trimmed());
    if (!headerMatch.hasMatch()) {
        setError(error, QStringLiteral("vmrun list output lacks its count header."));
        return {};
    }
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const QString line = lines.at(index).trimmed();
        if (!QFileInfo(line).isAbsolute()) {
            setError(error, QStringLiteral("vmrun returned a non-absolute VMX path: %1").arg(line));
            return {};
        }
        paths.append(QFileInfo(line).absoluteFilePath());
    }
    if (headerMatch.captured(1).toLongLong() != paths.size()) {
        setError(error, QStringLiteral("vmrun list count does not match returned VMX paths."));
        return {};
    }
    setError(error, {});
    return paths;
}

QList<Snapshot> VmwareProvider::parseSnapshotList(const QByteArray &output, QString *error)
{
    QList<Snapshot> snapshots;
    const QStringList lines = QString::fromLocal8Bit(output).split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        setError(error, QStringLiteral("vmrun snapshot output is empty."));
        return {};
    }
    const QRegularExpression header(QStringLiteral("^Total snapshots:\\s*([0-9]+)$"),
                                    QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch headerMatch = header.match(lines.first().trimmed());
    if (!headerMatch.hasMatch()) {
        setError(error, QStringLiteral("vmrun snapshot output lacks its count header."));
        return {};
    }
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const QString line = lines.at(index).trimmed();
        if (line.isEmpty()) {
            setError(error, QStringLiteral("vmrun returned an empty snapshot name."));
            return {};
        }
        snapshots.append(Snapshot{line, line, {}, {}, false, {}});
    }
    if (headerMatch.captured(1).toLongLong() != snapshots.size()) {
        setError(error, QStringLiteral("vmrun snapshot count does not match returned names."));
        return {};
    }
    setError(error, {});
    return snapshots;
}

Machine VmwareProvider::inspectMachine(const QString &vmxPath,
                                       const QStringList &runningVmxPaths,
                                       Ownership ownership,
                                       QString *error) const
{
    Machine machine;
    machine.ref.providerId = m_info.id;
    machine.ref.id = QFileInfo(vmxPath).absoluteFilePath();
    machine.ref.name = QFileInfo(vmxPath).completeBaseName();
    machine.configPath = machine.ref.id;
    machine.ownership = ownership;
    QString vmxError;
    const std::optional<VmxDocument> document = VmxDocument::load(machine.configPath, &vmxError);
    if (!document) {
        machine.powerState = PowerState::Inaccessible;
        machine.inaccessibleReason = vmxError;
        setError(error, {});
        return machine;
    }
    const QString displayName = document->value(QStringLiteral("displayName"));
    if (!displayName.trimmed().isEmpty())
        machine.ref.name = displayName;
    machine.storagePaths = document->storagePaths(QFileInfo(vmxPath).absolutePath());
    bool integerOk = false;
    const int cpus = document->value(QStringLiteral("numvcpus")).toInt(&integerOk);
    if (integerOk && cpus > 0)
        machine.cpuCount = cpus;
    const int memory = document->value(QStringLiteral("memsize")).toInt(&integerOk);
    if (integerOk && memory > 0)
        machine.memoryMiB = memory;
    const QString firmware = document->value(QStringLiteral("firmware"));
    if (!firmware.isEmpty())
        machine.firmware = firmware.compare(QStringLiteral("efi"), Qt::CaseInsensitive) == 0
            ? Firmware::Efi : Firmware::Bios;
    if (document->contains(QStringLiteral("uefi.secureBoot.enabled")))
        machine.secureBoot = providerBoolean(
            document->value(QStringLiteral("uefi.secureBoot.enabled")));
    if (document->contains(QStringLiteral("tpm.present"))
        || document->contains(QStringLiteral("managedVM.autoAddVTPM"))) {
        machine.tpm = providerBoolean(document->value(QStringLiteral("tpm.present")))
            || providerBoolean(document->value(QStringLiteral("managedVM.autoAddVTPM")));
    }
    static const QRegularExpression vmwareStorage(
        QStringLiteral("^(ide|sata|scsi|nvme)([0-9]+):([0-9]+)\\.fileName$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression vmwareEthernet(
        QStringLiteral("^ethernet([0-9]+)\\.present$"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &key : document->keys()) {
        const QRegularExpressionMatch storage = vmwareStorage.match(key);
        if (storage.hasMatch()) {
            const QString prefix = key.left(key.size() - QStringLiteral(".fileName").size());
            if (!providerBoolean(document->value(prefix + QStringLiteral(".present"))))
                continue;
            const QString configured = document->value(key);
            const QFileInfo source(configured);
            StorageAttachment attachment;
            attachment.id = prefix;
            attachment.bus = storage.captured(1).toLower();
            attachment.controller = storage.captured(2).toInt();
            attachment.port = storage.captured(3).toInt();
            attachment.controllerName = attachment.bus + QString::number(attachment.controller);
            attachment.path = source.isAbsolute() ? source.absoluteFilePath()
                : QFileInfo(QDir(QFileInfo(vmxPath).absolutePath()).filePath(configured)).absoluteFilePath();
            attachment.optical = document->value(prefix + QStringLiteral(".deviceType"))
                                     .contains(QStringLiteral("cdrom"), Qt::CaseInsensitive)
                || QFileInfo(attachment.path).suffix().compare(
                       QStringLiteral("iso"), Qt::CaseInsensitive) == 0;
            machine.storageDevices.append(attachment);
            continue;
        }
        const QRegularExpressionMatch ethernet = vmwareEthernet.match(key);
        if (!ethernet.hasMatch() || !providerBoolean(document->value(key)))
            continue;
        const int zeroBased = ethernet.captured(1).toInt();
        const QString prefix = QStringLiteral("ethernet%1").arg(zeroBased);
        NetworkAttachment attachment;
        attachment.id = prefix;
        attachment.slot = zeroBased + 1;
        attachment.mode = parsedNetworkMode(
            document->value(prefix + QStringLiteral(".connectionType")));
        attachment.interfaceName = document->value(prefix + QStringLiteral(".vnet"));
        attachment.model = document->value(prefix + QStringLiteral(".virtualDev"));
        attachment.macAddress = document->value(prefix + QStringLiteral(".address"));
        if (attachment.macAddress.isEmpty())
            attachment.macAddress = document->value(prefix + QStringLiteral(".generatedAddress"));
        attachment.connected = providerBoolean(
            document->value(prefix + QStringLiteral(".startConnected")));
        machine.networkDevices.append(attachment);
    }
    machine.powerState = std::any_of(
        runningVmxPaths.cbegin(), runningVmxPaths.cend(), [&vmxPath](const QString &running) {
            return providerPathEqual(vmxPath, running);
        }) ? PowerState::Running : PowerState::PoweredOff;
    if (machine.powerState == PowerState::Running) {
        machine.warnings.append(QStringLiteral(
            "vmrun reports the VM as active but cannot distinguish running from paused; "
            "pause/resume commands remain provider-validated."));
    }
    const QString suspendedPath = QDir(QFileInfo(vmxPath).absolutePath())
                                      .filePath(QFileInfo(vmxPath).completeBaseName()
                                                + QStringLiteral(".vmss"));
    if (machine.powerState != PowerState::Running && QFileInfo::exists(suspendedPath))
        machine.powerState = PowerState::Suspended;
    QByteArray runningEvidence = QByteArray("Total running VMs: ")
        + QByteArray::number(runningVmxPaths.size()) + '\n';
    for (const QString &path : runningVmxPaths)
        runningEvidence += path.toLocal8Bit() + '\n';
    machine.stateRevision = commandEvidence(
        EvidenceFormat::VmwareRunningPathsSha256, runningEvidence);
    machine.inventoryComplete = true;
    machine.hardwareInventoryComplete = true;
    setError(error, {});
    return machine;
}

QString VmwareProvider::targetToken() const
{
    return m_info.id == vmwarePlayerProviderId() ? QStringLiteral("player") : QStringLiteral("ws");
}

Command VmwareProvider::vmrun(const QStringList &arguments, int timeoutMs) const
{
    QStringList complete{QStringLiteral("-T"), targetToken()};
    complete.append(arguments);
    return Command{m_info.executable, complete,
                   QFileInfo(m_info.executable).absolutePath(), timeoutMs};
}

void VmwareProvider::addPreflight(Plan &plan, const Machine &machine) const
{
    if (machine.stateRevision.isEmpty()) {
        plan.errors.append(QStringLiteral("Refresh VMware live inventory before reviewing this operation."));
        return;
    }
    plan.preflight.append(CommandEvidence{
        inventoryCommand(), EvidenceFormat::VmwareRunningPathsSha256,
        machine.stateRevision, QStringLiteral("VMware running-machine inventory")});
    QString fileError;
    if (!addFileEvidence(plan, machine.configPath,
                         QStringLiteral("VMware VMX configuration"), &fileError))
        plan.errors.append(fileError);
}

Command VmwareProvider::inventoryCommand() const
{
    Command result = vmrun({QStringLiteral("list")});
    result.interruptible = true;
    return result;
}

Plan VmwareProvider::create(const CreateSpec &spec, const QString &revision,
                            const QDateTime &now) const
{
    QString validationError;
    if (!m_info.supports(capability::create()) || !QFileInfo(m_info.executable).isAbsolute()
        || !QFileInfo(m_info.diskManagerExecutable).isAbsolute())
        return invalidPlan(QStringLiteral("VMware create is unavailable because vmware-vdiskmanager was not detected."));
    if (!validateCreateSpec(spec, m_info.id, &validationError))
        return invalidPlan(validationError);
    if (spec.secureBoot && !m_info.supports(capability::secureBoot()))
        return invalidPlan(QStringLiteral("Detected VMware capabilities do not prove Secure Boot support."));
    if (spec.tpm)
        return invalidPlan(QStringLiteral("VMware TPM creation is disabled without a declared encrypted-VM capability."));
    const QString vmxPath = QDir(spec.directory).filePath(spec.name + QStringLiteral(".vmx"));
    const QString diskPath = QDir(spec.directory).filePath(spec.name + QStringLiteral(".vmdk"));
    QString vmxError;
    CreateSpec effectiveSpec = spec;
    effectiveSpec.virtualHardwareVersion = vmwareHardwareVersion(parsedProviderVersion(m_info.version));
    const std::optional<VmxDocument> document = VmxDocument::fromCreateSpec(
        effectiveSpec, diskPath, &vmxError);
    if (!document)
        return invalidPlan(vmxError);
    const VmRef target{m_info.id, vmxPath, spec.name};
    const Command disk{m_info.diskManagerExecutable,
                       {QStringLiteral("-c"), QStringLiteral("-s"),
                        QStringLiteral("%1MB").arg(spec.diskMiB), QStringLiteral("-a"),
                        QStringLiteral("lsilogic"), QStringLiteral("-t"), QStringLiteral("1"),
                        diskPath}, QFileInfo(m_info.diskManagerExecutable).absolutePath(),
                       10 * 60 * 1000};
    Plan plan;
    plan.preview = makePreview(QStringLiteral("create"), target, Risk::Reversible,
                               {QStringLiteral("Create a VMDK and atomically write %1.").arg(vmxPath)},
                               {QStringLiteral("VMware registration is not claimed; open or start the VMX explicitly.")},
                               {disk}, revision, now);
    plan.atomicWritesAfterCommands.append(AtomicWrite{vmxPath, document->serialize(), {}});
    if (!spec.isoPath.isEmpty()
        && !addFileEvidence(plan, spec.isoPath,
                            QStringLiteral("VMware installation ISO"), &validationError))
        plan.errors.append(validationError);
    return plan;
}

Plan VmwareProvider::registerMachine(const QString &, const QString &, const QString &,
                                     const QDateTime &) const
{
    return invalidPlan(QStringLiteral("VMware vmrun does not provide a safe register operation; VM Lab will not claim one."));
}

Plan VmwareProvider::openConsole(const Machine &machine, const QString &revision,
                                 const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !m_info.supports(capability::openConsole()))
        return invalidPlan(error.isEmpty() ? QStringLiteral("VMware console capability is unavailable.") : error);
    const Command open{m_info.consoleExecutable, {vmxTarget(machine)}, {}, 30000, true};
    Plan plan;
    plan.preview = makePreview(QStringLiteral("open-console"), machine.ref, Risk::ReadOnly,
                               {QStringLiteral("Open the VMX in the provider console.")}, {},
                               {open}, revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VmwareProvider::lifecycle(const Machine &machine, const QStringList &arguments,
                               const QString &action, Risk risk, const QString &effect,
                               const QString &revision, const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !QFileInfo(machine.configPath).isAbsolute()
        || !m_info.supports(capability::lifecycle()))
        return invalidPlan(error.isEmpty() ? QStringLiteral("VMware lifecycle capability is unavailable.") : error);
    Plan plan;
    plan.preview = makePreview(action, machine.ref, risk, {effect}, {},
                               {vmrun(arguments)}, revision, now);
    addPreflight(plan, machine);
    return plan;
}

Plan VmwareProvider::start(const Machine &machine, bool headless, const QString &revision,
                           const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::PoweredOff, PowerState::Suspended, PowerState::Saved}))
        return invalidPlan(QStringLiteral("VMware start requires a powered-off or suspended VM."));
    return lifecycle(machine, {QStringLiteral("start"), vmxTarget(machine),
                               headless ? QStringLiteral("nogui") : QStringLiteral("gui")},
                     QStringLiteral("start"), Risk::Disruptive,
                     QStringLiteral("Start the VMware virtual machine."), revision, now);
}

Plan VmwareProvider::gracefulShutdown(const Machine &machine, const QString &revision,
                                      const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Graceful shutdown requires a running or paused VM."));
    return lifecycle(machine, {QStringLiteral("stop"), vmxTarget(machine), QStringLiteral("soft")},
                     QStringLiteral("graceful-shutdown"), Risk::Disruptive,
                     QStringLiteral("Request a guest-aware soft shutdown."), revision, now);
}

Plan VmwareProvider::powerOff(const Machine &machine, const QString &revision,
                              const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Power off requires a running or paused VM."));
    Plan plan = lifecycle(machine, {QStringLiteral("stop"), vmxTarget(machine), QStringLiteral("hard")},
                          QStringLiteral("power-off"), Risk::Disruptive,
                          QStringLiteral("Immediately stop the VM."), revision, now);
    plan.preview.warnings.append(QStringLiteral("Hard stop may lose unsaved guest data."));
    return plan;
}

Plan VmwareProvider::pause(const Machine &machine, const QString &revision,
                           const QDateTime &now) const
{
    if (machine.powerState != PowerState::Running)
        return invalidPlan(QStringLiteral("Pause requires a running VM."));
    Plan plan = lifecycle(machine, {QStringLiteral("pause"), vmxTarget(machine)},
                          QStringLiteral("pause"), Risk::Reversible,
                          QStringLiteral("Pause VM execution."), revision, now);
    plan.preview.warnings.append(QStringLiteral(
        "vmrun inventory cannot distinguish an already-paused VM; the provider may reject this idempotently."));
    return plan;
}

Plan VmwareProvider::resume(const Machine &machine, const QString &revision,
                            const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Paused, PowerState::Running}))
        return invalidPlan(QStringLiteral("Resume requires an active VMware VM."));
    Plan plan = lifecycle(machine, {QStringLiteral("unpause"), vmxTarget(machine)},
                          QStringLiteral("resume"), Risk::Reversible,
                          QStringLiteral("Resume a paused VM."), revision, now);
    if (machine.powerState == PowerState::Running)
        plan.preview.warnings.append(QStringLiteral(
            "vmrun inventory cannot distinguish running from paused; unpause may report that the VM is already running."));
    return plan;
}

Plan VmwareProvider::reset(const Machine &machine, const QString &revision,
                           const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Reset requires a running or paused VM."));
    return lifecycle(machine, {QStringLiteral("reset"), vmxTarget(machine), QStringLiteral("hard")},
                     QStringLiteral("reset"), Risk::Disruptive,
                     QStringLiteral("Hard-reset the VM; unsaved guest data may be lost."), revision, now);
}

Plan VmwareProvider::saveState(const Machine &machine, const QString &revision,
                               const QDateTime &now) const
{
    if (!stateIs(machine, {PowerState::Running, PowerState::Paused}))
        return invalidPlan(QStringLiteral("Save state requires a running or paused VM."));
    return lifecycle(machine, {QStringLiteral("suspend"), vmxTarget(machine), QStringLiteral("soft")},
                     QStringLiteral("save-state"), Risk::Reversible,
                     QStringLiteral("Suspend the VM and persist its runtime state."), revision, now);
}

Plan VmwareProvider::configure(const Machine &machine, const ConfigPatch &patch,
                               const QString &revision, const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !poweredOff(machine, &error))
        return invalidPlan(error);
    if (!m_info.supports(capability::configure()))
        return invalidPlan(QStringLiteral("VMware configure capability is unavailable."));
    if (patch.empty())
        return invalidPlan(QStringLiteral("Configuration patch is empty."));
    std::optional<VmxDocument> document = VmxDocument::load(machine.configPath, &error);
    if (!document || !applyConfigPatch(*document, patch, &error))
        return invalidPlan(error);
    const QString expected = fileSha256(machine.configPath, &error);
    if (!error.isEmpty())
        return invalidPlan(error);
    Plan plan;
    plan.preview = makePreview(QStringLiteral("configure"), machine.ref, Risk::Reversible,
                               {QStringLiteral("Atomically update the powered-off VMX.")}, {},
                               {}, revision, now);
    plan.atomicWritesAfterCommands.append(AtomicWrite{machine.configPath, document->serialize(), expected});
    addPreflight(plan, machine);
    return plan;
}

Plan VmwareProvider::attachIso(const Machine &machine, const QString &isoPath,
                               const QString &revision, const QDateTime &now) const
{
    ConfigPatch patch;
    patch.isoPath = isoPath;
    Plan plan = configure(machine, patch, revision, now);
    plan.preview.action = QStringLiteral("attach-iso");
    QString evidenceError;
    if (plan.ok() && !addFileEvidence(plan, isoPath,
                                      QStringLiteral("VMware attached ISO"),
                                      &evidenceError))
        plan.errors.append(evidenceError);
    return plan;
}

Plan VmwareProvider::detachIso(const Machine &machine, const QString &revision,
                               const QDateTime &now) const
{
    ConfigPatch patch;
    patch.isoPath = QString{};
    Plan plan = configure(machine, patch, revision, now);
    plan.preview.action = QStringLiteral("detach-iso");
    return plan;
}

Plan VmwareProvider::listSnapshots(const Machine &machine, const QString &revision,
                                   const QDateTime &now) const
{
    if (!m_info.supports(capability::snapshots()))
        return invalidPlan(QStringLiteral("VMware snapshot capability is unavailable."));
    Plan plan = lifecycle(machine, {QStringLiteral("listSnapshots"), vmxTarget(machine)},
                     QStringLiteral("list-snapshots"), Risk::ReadOnly,
                     QStringLiteral("Read VMware snapshot inventory."), revision, now);
    if (!plan.preview.commands.isEmpty())
        plan.preview.commands.first().interruptible = true;
    return plan;
}

Plan VmwareProvider::takeSnapshot(const Machine &machine, const QString &name,
                                  const QString &description, const QString &revision,
                                  const QDateTime &now) const
{
    if (name.trimmed().isEmpty()) return invalidPlan(QStringLiteral("Snapshot name is required."));
    if (!m_info.supports(capability::snapshots()))
        return invalidPlan(QStringLiteral("VMware snapshot capability is unavailable."));
    Plan plan = lifecycle(machine, {QStringLiteral("snapshot"), vmxTarget(machine), name},
                          QStringLiteral("take-snapshot"), Risk::Reversible,
                          QStringLiteral("Create VMware snapshot '%1'.").arg(name), revision, now);
    if (!description.isEmpty())
        plan.preview.warnings.append(QStringLiteral("vmrun does not accept snapshot descriptions; the reviewed description is not sent."));
    return plan;
}

Plan VmwareProvider::restoreSnapshot(const Machine &machine, const Snapshot &snapshot,
                                     const QString &revision, const QDateTime &now) const
{
    if (snapshot.name.isEmpty() || snapshot.inventoryRevision.isEmpty())
        return invalidPlan(QStringLiteral("Refresh snapshot inventory before restoring this snapshot."));
    if (machine.powerState != PowerState::PoweredOff)
        return invalidPlan(QStringLiteral("Snapshot restore requires a powered-off VM."));
    Plan plan = lifecycle(machine, {QStringLiteral("revertToSnapshot"), vmxTarget(machine), snapshot.name},
                      QStringLiteral("restore-snapshot"), Risk::Destructive,
                      QStringLiteral("Restore snapshot '%1'; newer state may be lost.").arg(snapshot.name),
                      revision, now);
    plan.preflight.append(CommandEvidence{
        vmrun({QStringLiteral("listSnapshots"), vmxTarget(machine)}),
        EvidenceFormat::RawSha256, snapshot.inventoryRevision,
        QStringLiteral("VMware snapshot inventory")});
    plan.preflight.last().command.interruptible = true;
    return plan;
}

Plan VmwareProvider::deleteSnapshot(const Machine &machine, const Snapshot &snapshot,
                                    const QString &revision, const QDateTime &now) const
{
    if (snapshot.name.isEmpty() || snapshot.inventoryRevision.isEmpty())
        return invalidPlan(QStringLiteral("Refresh snapshot inventory before deleting this snapshot."));
    if (machine.powerState != PowerState::PoweredOff)
        return invalidPlan(QStringLiteral("Snapshot deletion requires a powered-off VM."));
    Plan plan = lifecycle(machine, {QStringLiteral("deleteSnapshot"), vmxTarget(machine), snapshot.name},
                      QStringLiteral("delete-snapshot"), Risk::Destructive,
                      QStringLiteral("Permanently delete snapshot '%1'.").arg(snapshot.name),
                      revision, now);
    plan.preflight.append(CommandEvidence{
        vmrun({QStringLiteral("listSnapshots"), vmxTarget(machine)}),
        EvidenceFormat::RawSha256, snapshot.inventoryRevision,
        QStringLiteral("VMware snapshot inventory")});
    plan.preflight.last().command.interruptible = true;
    return plan;
}

Plan VmwareProvider::unregisterMachine(const Machine &, const QString &,
                                       const QDateTime &) const
{
    return invalidPlan(QStringLiteral("VMware vmrun does not provide a safe unregister operation; remove only the WimForge catalog entry."));
}

Plan VmwareProvider::deleteMachine(const Machine &machine, const QString &managedRoot,
                                   const QList<Machine> &catalogMachines,
                                   const QString &revision, const QDateTime &now) const
{
    QString error;
    if (!machineMatches(machine, m_info, &error) || !poweredOff(machine, &error))
        return invalidPlan(error);
    const DeletionGuard guard = PathPolicy::managedDeletionGuard(machine, managedRoot, catalogMachines);
    if (!guard.allowed)
        return invalidPlan(guard.error);
    Plan plan;
    plan.preview = makePreview(QStringLiteral("delete"), machine.ref, Risk::Destructive,
                               {QStringLiteral("Delete the canonical managed VM directory %1.")
                                    .arg(guard.canonicalDirectory)},
                               {QStringLiteral("VMX, virtual disks, snapshots, and logs will be permanently deleted.")},
                               {}, revision, now);
    plan.managedDeletionAfterCommands = ManagedDeletion{
        machine, managedRoot, catalogMachines, guard.identity,
        guard.rootIdentity, false};
    addPreflight(plan, machine);
    return plan;
}

} // namespace wimforge::vmlab
