#include "AppController.h"
#include "core/GpoPolicyCompiler.h"
#include "core/ImageSourceInspector.h"
#include "core/PayloadCatalog.h"
#include "core/ProcessLaunch.h"
#include "core/StructuredLogger.h"
#include "core/VmLabScope.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <utility>

using namespace wimforge;

namespace {

QString cleanPath(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QString() : QDir::cleanPath(trimmed);
}

QString bilingualCommitMessage(const QString &english, const QString &cantonese)
{
    return english + QStringLiteral(" / ") + cantonese;
}

void removeCaseInsensitive(QStringList &values, const QString &needle)
{
    values.removeIf([&needle](const QString &value) {
        return value.compare(needle, Qt::CaseInsensitive) == 0;
    });
}

bool isSafeConfigurationIdentity(const QString &value)
{
    const QString trimmed = value.trimmed();
    return !trimmed.isEmpty() && trimmed.size() <= 1024
        && !trimmed.contains(QLatin1Char('\r'))
        && !trimmed.contains(QLatin1Char('\n'))
        && !trimmed.contains(QChar::Null);
}

QString normalizedTaskPath(const QString &value)
{
    QString path = QDir::fromNativeSeparators(value.trimmed());
    // Task Scheduler commonly displays a single root backslash. Store the
    // path relative to Windows/System32/Tasks, as required by ProjectConfig.
    if (path.startsWith(QLatin1Char('/')) && !path.startsWith(QStringLiteral("//")))
        path.remove(0, 1);
    return path;
}

bool isSafeScheduledTaskPath(const QString &value)
{
    const QString path = normalizedTaskPath(value);
    if (path.isEmpty() || path.size() > 4096 || QDir::isAbsolutePath(path)
        || path.contains(QLatin1Char(':')) || path.contains(QLatin1Char('\r'))
        || path.contains(QLatin1Char('\n')) || path.contains(QChar::Null)) {
        return false;
    }
    const QStringList segments = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    return std::none_of(segments.cbegin(), segments.cend(), [](const QString &segment) {
        return segment.isEmpty() || segment == QStringLiteral(".")
            || segment == QStringLiteral("..");
    });
}

bool isAppxProvisioningPayload(const QFileInfo &file)
{
    static const QSet<QString> extensions{
        QStringLiteral("appx"), QStringLiteral("appxbundle"),
        QStringLiteral("msix"), QStringLiteral("msixbundle"),
    };
    return file.isFile() && file.isAbsolute()
        && extensions.contains(file.suffix().toLower());
}

constexpr qsizetype MaximumRecentProjects = 12;
constexpr qsizetype MaximumRecentProjectNameLength = 160;
constexpr qsizetype MaximumRecentProjectPathLength = 32'767;

bool isUnsafeDisplayCharacter(QChar character)
{
    const ushort codePoint = character.unicode();
    return codePoint < 0x20 || codePoint == 0x7f
        || (codePoint >= 0x202a && codePoint <= 0x202e)
        || (codePoint >= 0x2066 && codePoint <= 0x2069);
}

bool isSafeLocalRecentPath(const QString &path)
{
    if (path.isEmpty() || path.size() > MaximumRecentProjectPathLength
        || !QDir::isAbsolutePath(path)) {
        return false;
    }
    if (std::any_of(path.cbegin(), path.cend(), isUnsafeDisplayCharacter))
        return false;

    const QString portable = QDir::fromNativeSeparators(path);
#ifdef Q_OS_WIN
    // QSettings is user-writable while the GUI runs elevated. Never probe or
    // retain network shares, device namespaces, or other non-drive paths from
    // that store; doing so at startup could trigger an unintended privileged
    // filesystem/network access.
    if (portable.startsWith(QStringLiteral("//"))
        || !QRegularExpression(QStringLiteral("^[A-Za-z]:/")).match(portable).hasMatch()) {
        return false;
    }
#endif

    return !QDir(portable).isRoot();
}

QString recentPathKey(const QString &path)
{
    QString key = QDir::fromNativeSeparators(cleanPath(path));
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

std::optional<QVariantMap> sanitizedRecentProject(const QVariant &candidate)
{
    const QVariantMap source = candidate.toMap();
    const QString rawPath = source.isEmpty()
        ? candidate.toString()
        : source.value(QStringLiteral("path")).toString();
    const QString path = QDir::fromNativeSeparators(cleanPath(rawPath));
    if (!isSafeLocalRecentPath(path))
        return std::nullopt;

    QString name = source.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty())
        name = QFileInfo(path).fileName();
    std::replace_if(name.begin(), name.end(), isUnsafeDisplayCharacter, QLatin1Char(' '));
    name = name.simplified().left(MaximumRecentProjectNameLength);
    if (name.isEmpty())
        name = QFileInfo(path).fileName();

    QString lastOpened = source.value(QStringLiteral("lastOpened")).toString().trimmed();
    const QDateTime parsed = QDateTime::fromString(lastOpened, Qt::ISODateWithMs);
    if (!parsed.isValid())
        lastOpened.clear();

    return QVariantMap{
        {QStringLiteral("name"), name},
        {QStringLiteral("path"), path},
        {QStringLiteral("lastOpened"), lastOpened},
    };
}

QVariantMap payloadCatalogVariant(const ServicingPayloadEntry &entry)
{
    return QVariantMap{
        {QStringLiteral("path"), entry.path},
        {QStringLiteral("title"), entry.title},
        {QStringLiteral("detail"), entry.detail},
        {QStringLiteral("extension"), entry.extension},
        {QStringLiteral("kb"), entry.knowledgeBaseId},
        {QStringLiteral("provider"), entry.provider},
        {QStringLiteral("driverClass"), entry.driverClass},
        {QStringLiteral("driverVersion"), entry.driverVersion},
        {QStringLiteral("catalogFile"), entry.catalogFile},
        {QStringLiteral("sizeBytes"), entry.sizeBytes},
        {QStringLiteral("containedFileCount"), entry.containedFileCount},
        {QStringLiteral("exists"), entry.exists},
        {QStringLiteral("directory"), entry.directory},
        {QStringLiteral("supported"), entry.supported},
    };
}

QString notificationTimestamp(const QDateTime &value)
{
    if (!value.isValid())
        return QString();
    const QDateTime local = value.toLocalTime();
    return local.date() == QDate::currentDate()
        ? local.time().toString(QStringLiteral("HH:mm"))
        : local.toString(QStringLiteral("yyyy-MM-dd"));
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("Could not create %1").arg(QFileInfo(path).absolutePath()));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    setError(error, {});
    return true;
}

QVariantMap gpoElementVariant(const GpoElement &element)
{
    QVariantList options;
    for (const GpoEnumOption &option : element.options) {
        options.append(QVariantMap{
            {QStringLiteral("label"), option.displayName},
            {QStringLiteral("value"), option.value.toDisplayString()},
        });
    }
    const qint64 minimum = element.minimumValue.value_or(0);
    const qint64 maximum = element.maximumValue.value_or(std::numeric_limits<int>::max());
    return QVariantMap{
        {QStringLiteral("id"), element.id},
        {QStringLiteral("label"), element.presentationLabel.isEmpty()
             ? element.id : element.presentationLabel},
        {QStringLiteral("control"), element.materialControl()},
        {QStringLiteral("required"), element.required},
        {QStringLiteral("defaultValue"), gpoPresentationDefaultValue(element)},
        {QStringLiteral("defaultChecked"), element.presentationDefaultChecked},
        {QStringLiteral("minimum"), QString::number(minimum)},
        {QStringLiteral("maximum"), QString::number(maximum)},
        {QStringLiteral("spinStep"), QString::number(element.presentationSpinStep.value_or(1))},
        {QStringLiteral("numericTextEditor"), gpoUsesNumericTextEditor(element)},
        {QStringLiteral("minimumLength"), element.minimumLength.value_or(-1)},
        {QStringLiteral("maximumLength"), element.maximumLength.value_or(-1)},
        {QStringLiteral("maximumStrings"), element.maximumStrings.value_or(-1)},
        {QStringLiteral("options"), options},
    };
}

QVariantMap gpoPolicyVariant(const GpoPolicy &policy)
{
    QVariantList elements;
    for (const GpoElement &element : policy.elements)
        elements.append(gpoElementVariant(element));
    return QVariantMap{
        {QStringLiteral("id"), policy.qualifiedId()},
        {QStringLiteral("name"), policy.displayName.isEmpty() ? policy.id : policy.displayName},
        {QStringLiteral("category"), policy.categoryHierarchy.join(QStringLiteral("  ›  "))},
        {QStringLiteral("policyClass"), gpoPolicyClassName(policy.policyClass)},
        {QStringLiteral("supportedOn"), policy.supportedOn},
        {QStringLiteral("documentation"), policy.explainText},
        {QStringLiteral("registryKey"), policy.registryKey},
        {QStringLiteral("registryValue"), policy.registryValueName},
        {QStringLiteral("elements"), elements},
    };
}

QString openCodeText(const QByteArray &raw)
{
    QStringList candidates;
    const QList<QByteArray> lines = raw.split('\n');
    for (const QByteArray &line : lines) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            continue;
        const QJsonObject object = document.object();
        for (const QString &key : {QStringLiteral("text"), QStringLiteral("content"),
                                   QStringLiteral("message"), QStringLiteral("output")}) {
            const QJsonValue value = object.value(key);
            if (value.isString() && !value.toString().trimmed().isEmpty())
                candidates.append(value.toString().trimmed());
            else if (value.isObject()) {
                const QString nested = value.toObject().value(QStringLiteral("text")).toString().trimmed();
                if (!nested.isEmpty())
                    candidates.append(nested);
            }
        }
    }
    if (!candidates.isEmpty())
        return candidates.constLast();
    return QString::fromUtf8(raw).trimmed();
}

QByteArray unfenceJson(QString text)
{
    text = text.trimmed();
    const QRegularExpression fence(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)\\s*```"),
                                   QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = fence.match(text);
    if (match.hasMatch())
        text = match.captured(1).trimmed();
    const qsizetype begin = text.indexOf(QLatin1Char('{'));
    const qsizetype end = text.lastIndexOf(QLatin1Char('}'));
    if (begin >= 0 && end >= begin)
        text = text.mid(begin, end - begin + 1);
    return text.toUtf8();
}

WinForgeRecipe emptyWinForgeRecipe(const QString &name = QStringLiteral("WinForge install recipe"))
{
    WinForgeRecipe recipe;
    recipe.id = QStringLiteral("winforge-install-recipe");
    recipe.name = name;
    recipe.description = QStringLiteral(
        "Approved actions replayed by the versioned WimForge WinForge Bridge after Windows setup.");
    recipe.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return recipe;
}

QString discoverWinForgeRuntimePath()
{
    const QString environmentPath = cleanPath(qEnvironmentVariable("WINFORGE_RUNTIME"));
    if (QFileInfo(QDir(environmentPath).filePath(QStringLiteral("WinForge.exe"))).isFile())
        return environmentPath;

    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QStringList directCandidates{
        QDir(applicationDirectory).filePath(QStringLiteral("WinForge")),
        QDir(applicationDirectory).filePath(QStringLiteral("../WinForge")),
        QDir::current().filePath(QStringLiteral("WinForge")),
    };
    for (const QString &candidate : directCandidates) {
        if (QFileInfo(QDir(candidate).filePath(QStringLiteral("WinForge.exe"))).isFile())
            return QDir::cleanPath(candidate);
    }

    const QString developmentBin = QDir::cleanPath(
        QDir::current().filePath(QStringLiteral("../WinForge/bin")));
    if (QFileInfo(developmentBin).isDir()) {
        QDirIterator iterator(developmentBin, {QStringLiteral("WinForge.exe")}, QDir::Files,
                              QDirIterator::Subdirectories);
        if (iterator.hasNext())
            return QFileInfo(iterator.next()).absolutePath();
    }
    return {};
}

QString winForgeCapability(WinForgeActionKind kind)
{
    switch (kind) {
    case WinForgeActionKind::Module: return QStringLiteral("apply.module.v1");
    case WinForgeActionKind::Page: return QStringLiteral("launch.page.v1");
    case WinForgeActionKind::Tweak: return QStringLiteral("apply.tweak.v1");
    case WinForgeActionKind::Command:
    case WinForgeActionKind::Registry:
    case WinForgeActionKind::Copy:
        return {};
    }
    return {};
}

std::optional<WinForgeActionKind> winForgeActionKind(const QString &value)
{
    const QString kind = value.trimmed().toLower();
    if (kind == QStringLiteral("page")) return WinForgeActionKind::Page;
    if (kind == QStringLiteral("module")) return WinForgeActionKind::Module;
    if (kind == QStringLiteral("tweak")) return WinForgeActionKind::Tweak;
    if (kind == QStringLiteral("command")) return WinForgeActionKind::Command;
    if (kind == QStringLiteral("registry")) return WinForgeActionKind::Registry;
    if (kind == QStringLiteral("copy")) return WinForgeActionKind::Copy;
    return std::nullopt;
}

QString bridgeActionSummary(const WinForgeAction &action)
{
    switch (action.kind) {
    case WinForgeActionKind::Module:
    case WinForgeActionKind::Page:
    case WinForgeActionKind::Tweak:
        return action.target;
    case WinForgeActionKind::Command:
        return QStringLiteral("%1 %2").arg(action.executable, action.arguments.join(QLatin1Char(' '))).trimmed();
    case WinForgeActionKind::Registry:
        return QStringLiteral("%1\\%2  →  %3").arg(action.registryHive, action.registryPath,
                                                    action.registryValueName);
    case WinForgeActionKind::Copy:
        return QStringLiteral("%1  →  %2").arg(action.sourceRelative, action.destination);
    }
    return {};
}

std::optional<vmlab::Firmware> vmFirmware(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("efi") || normalized == QStringLiteral("uefi"))
        return vmlab::Firmware::Efi;
    if (normalized == QStringLiteral("bios"))
        return vmlab::Firmware::Bios;
    return std::nullopt;
}

std::optional<vmlab::NetworkMode> vmNetworkMode(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("nat")) return vmlab::NetworkMode::Nat;
    if (normalized == QStringLiteral("bridged")) return vmlab::NetworkMode::Bridged;
    if (normalized == QStringLiteral("host-only")) return vmlab::NetworkMode::HostOnly;
    if (normalized == QStringLiteral("internal")) return vmlab::NetworkMode::Internal;
    if (normalized == QStringLiteral("disconnected")) return vmlab::NetworkMode::Disconnected;
    return std::nullopt;
}

std::optional<vmlab::StorageBus> vmStorageBus(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("ide")) return vmlab::StorageBus::Ide;
    if (normalized == QStringLiteral("sata")) return vmlab::StorageBus::Sata;
    if (normalized == QStringLiteral("scsi")) return vmlab::StorageBus::Scsi;
    if (normalized == QStringLiteral("nvme")) return vmlab::StorageBus::Nvme;
    return std::nullopt;
}

QString providerGuestType(const QString &providerId, const QString &label)
{
    const QString normalized = label.trimmed().toLower();
    const bool virtualBox = providerId == vmlab::virtualBoxProviderId();
    if (normalized.contains(QStringLiteral("windows 11")))
        return virtualBox ? QStringLiteral("Windows11_64") : QStringLiteral("windows11-64");
    if (normalized.contains(QStringLiteral("windows 10")))
        return virtualBox ? QStringLiteral("Windows10_64") : QStringLiteral("windows10-64");
    if (normalized.contains(QStringLiteral("server 2025"))) {
        // Current provider releases do not expose a stable cross-version 2025
        // identifier. Use the latest compatible Windows Server 64-bit profile.
        return virtualBox ? QStringLiteral("Windows2022_64")
                          : QStringLiteral("windows2019srvNext-64");
    }
    if (normalized.contains(QStringLiteral("server 2022")))
        return virtualBox ? QStringLiteral("Windows2022_64")
                          : QStringLiteral("windows2019srvNext-64");
    if (normalized.contains(QStringLiteral("other")))
        return virtualBox ? QStringLiteral("Windows10_64") : QStringLiteral("windows10-64");
    return label.trimmed();
}

QString isoTimestamp(const QDateTime &value)
{
    return value.isValid() ? value.toLocalTime().toString(Qt::ISODateWithMs) : QString();
}

QVariantMap vmProviderVariant(const vmlab::ProviderInfo &provider)
{
    QStringList capabilities(provider.capabilities.cbegin(), provider.capabilities.cend());
    capabilities.sort(Qt::CaseInsensitive);
    return QVariantMap{
        {QStringLiteral("id"), provider.id},
        {QStringLiteral("displayName"), provider.displayName},
        {QStringLiteral("available"), provider.available},
        {QStringLiteral("executable"), provider.executable},
        {QStringLiteral("consoleExecutable"), provider.consoleExecutable},
        {QStringLiteral("diskManagerExecutable"), provider.diskManagerExecutable},
        {QStringLiteral("version"), provider.version},
        {QStringLiteral("capabilities"), capabilities},
        {QStringLiteral("evidence"), provider.evidence},
        {QStringLiteral("warnings"), provider.warnings},
    };
}

QStringList vmAllowedActions(const vmlab::Machine &machine)
{
    QStringList result;
    const bool virtualBox = machine.ref.providerId == vmlab::virtualBoxProviderId();
    const bool vmware = machine.ref.providerId == vmlab::vmwareWorkstationProviderId()
        || machine.ref.providerId == vmlab::vmwarePlayerProviderId();
    if (!virtualBox && !vmware)
        return result;
    const bool active = machine.powerState == vmlab::PowerState::Running
        || machine.powerState == vmlab::PowerState::Paused;
    if ((virtualBox && (machine.powerState == vmlab::PowerState::PoweredOff
                        || machine.powerState == vmlab::PowerState::Saved))
        || (vmware && (machine.powerState == vmlab::PowerState::PoweredOff
                       || machine.powerState == vmlab::PowerState::Suspended
                       || machine.powerState == vmlab::PowerState::Saved))) {
        result.append(QStringLiteral("start"));
    }
    if (active) {
        result.append({QStringLiteral("shutdown"), QStringLiteral("powerOff"),
                       QStringLiteral("reset"), QStringLiteral("saveState")});
    }
    if (machine.powerState == vmlab::PowerState::Running)
        result.append(QStringLiteral("pause"));
    if ((virtualBox && machine.powerState == vmlab::PowerState::Paused)
        || (vmware && (machine.powerState == vmlab::PowerState::Running
                       || machine.powerState == vmlab::PowerState::Paused))) {
        result.append(QStringLiteral("resume"));
    }
    if (machine.powerState == vmlab::PowerState::PoweredOff)
        result.append({QStringLiteral("configure"), QStringLiteral("delete")});
    if (machine.ownership == vmlab::Ownership::External)
        result.append(QStringLiteral("forget"));
    return result;
}

QVariantMap vmMachineVariant(const vmlab::Machine &machine, const QString &consoleLog)
{
    QVariantList storageDevices;
    QVariantList opticalDevices;
    for (const vmlab::StorageAttachment &attachment : machine.storageDevices) {
        QVariantMap item{
            {QStringLiteral("id"), attachment.id},
            {QStringLiteral("bus"), attachment.bus},
            {QStringLiteral("controller"), attachment.controller},
            {QStringLiteral("port"), attachment.port},
            {QStringLiteral("device"), attachment.device},
            {QStringLiteral("controllerName"), attachment.controllerName},
            {QStringLiteral("path"), attachment.path},
            {QStringLiteral("optical"), attachment.optical},
        };
        if (attachment.optical) {
            item.insert(QStringLiteral("name"), QStringLiteral("%1 %2:%3")
                            .arg(attachment.bus)
                            .arg(attachment.controller)
                            .arg(attachment.port));
            item.insert(QStringLiteral("isoPath"), attachment.path);
            opticalDevices.append(item);
        } else {
            storageDevices.append(item);
        }
    }
    QVariantList networkDevices;
    for (const vmlab::NetworkAttachment &attachment : machine.networkDevices) {
        networkDevices.append(QVariantMap{
            {QStringLiteral("id"), attachment.id},
            {QStringLiteral("slot"), attachment.slot},
            {QStringLiteral("name"), QStringLiteral("Adapter %1").arg(attachment.slot)},
            {QStringLiteral("mode"), vmlab::networkModeName(attachment.mode)},
            {QStringLiteral("interfaceName"), attachment.interfaceName},
            {QStringLiteral("model"), attachment.model},
            {QStringLiteral("macAddress"), attachment.macAddress},
            {QStringLiteral("connected"), attachment.connected},
        });
    }
    QVariantMap result{
        {QStringLiteral("id"), machine.ref.id},
        {QStringLiteral("providerId"), machine.ref.providerId},
        {QStringLiteral("name"), machine.ref.name},
        {QStringLiteral("configPath"), machine.configPath},
        {QStringLiteral("storagePaths"), machine.storagePaths},
        {QStringLiteral("storageDevices"), storageDevices},
        {QStringLiteral("opticalDevices"), opticalDevices},
        {QStringLiteral("networkDevices"), networkDevices},
        {QStringLiteral("allowedActions"), vmAllowedActions(machine)},
        {QStringLiteral("powerState"), vmlab::powerStateName(machine.powerState)},
        {QStringLiteral("ownership"), vmlab::ownershipName(machine.ownership)},
        {QStringLiteral("inaccessibleReason"), machine.inaccessibleReason},
        {QStringLiteral("warnings"), machine.warnings},
        {QStringLiteral("inventoryComplete"), machine.inventoryComplete},
        {QStringLiteral("stateRevision"), machine.stateRevision},
        {QStringLiteral("consoleLog"), consoleLog},
    };
    if (machine.cpuCount)
        result.insert(QStringLiteral("cpuCount"), *machine.cpuCount);
    if (machine.memoryMiB)
        result.insert(QStringLiteral("memoryMiB"), *machine.memoryMiB);
    if (machine.firmware)
        result.insert(QStringLiteral("firmware"), vmlab::firmwareName(*machine.firmware));
    if (machine.secureBoot)
        result.insert(QStringLiteral("secureBoot"), *machine.secureBoot);
    if (machine.tpm)
        result.insert(QStringLiteral("tpm"), *machine.tpm);
    return result;
}

QVariantMap vmSnapshotVariant(const vmlab::Snapshot &snapshot)
{
    return QVariantMap{
        {QStringLiteral("id"), snapshot.id},
        {QStringLiteral("name"), snapshot.name},
        {QStringLiteral("description"), snapshot.description},
        {QStringLiteral("createdAt"), isoTimestamp(snapshot.createdAt)},
        {QStringLiteral("current"), snapshot.current},
        {QStringLiteral("inventoryRevision"), snapshot.inventoryRevision},
    };
}

QString validationResultName(vmvalidation::RunStatus status)
{
    switch (status) {
    case vmvalidation::RunStatus::Running: return QStringLiteral("running");
    case vmvalidation::RunStatus::Passed: return QStringLiteral("pass");
    case vmvalidation::RunStatus::Failed: return QStringLiteral("fail");
    case vmvalidation::RunStatus::Cancelled: return QStringLiteral("aborted");
    }
    return QStringLiteral("running");
}

QString validationMilestoneResult(vmvalidation::MilestoneStatus status)
{
    switch (status) {
    case vmvalidation::MilestoneStatus::Reached: return QStringLiteral("pass");
    case vmvalidation::MilestoneStatus::Failed: return QStringLiteral("fail");
    case vmvalidation::MilestoneStatus::Skipped: return QStringLiteral("skip");
    }
    return QStringLiteral("pending");
}

int validationMilestoneTarget(const QString &profile)
{
    if (profile == QStringLiteral("installation")) return 3;
    if (profile == QStringLiteral("first-boot")) return 4;
    if (profile == QStringLiteral("upgrade")) return 5;
    if (profile == QStringLiteral("customization")) return 3;
    return 8;
}

QStringList validationRequiredMilestones(const QString &profile)
{
    if (profile == QStringLiteral("installation")) {
        return {QStringLiteral("installation-boot"), QStringLiteral("disk-layout"),
                QStringLiteral("installation-complete")};
    }
    if (profile == QStringLiteral("first-boot")) {
        return {QStringLiteral("first-boot"), QStringLiteral("drivers"),
                QStringLiteral("networking"), QStringLiteral("smoke-test")};
    }
    if (profile == QStringLiteral("upgrade")) {
        return {QStringLiteral("installation-boot"), QStringLiteral("installation-complete"),
                QStringLiteral("first-boot"), QStringLiteral("drivers"),
                QStringLiteral("smoke-test")};
    }
    if (profile == QStringLiteral("customization")) {
        return {QStringLiteral("customizations"), QStringLiteral("first-boot"),
                QStringLiteral("smoke-test")};
    }
    if (profile == QStringLiteral("full-smoke")) {
        return {QStringLiteral("installation-boot"), QStringLiteral("disk-layout"),
                QStringLiteral("installation-complete"), QStringLiteral("first-boot"),
                QStringLiteral("drivers"), QStringLiteral("networking"),
                QStringLiteral("customizations"), QStringLiteral("smoke-test")};
    }
    return {};
}

QStringList validationPassBlockers(const vmvalidation::ValidationRun &run)
{
    QStringList blockers;
    const QString profile = run.configSnapshot.value(QStringLiteral("profile")).toString();
    const QStringList required = validationRequiredMilestones(profile);
    if (required.isEmpty())
        blockers.append(QStringLiteral("The validation profile is unsupported."));
    for (const vmvalidation::ValidationMilestone &milestone : run.milestones) {
        if (milestone.status == vmvalidation::MilestoneStatus::Failed) {
            blockers.append(QStringLiteral("Milestone '%1' failed.").arg(milestone.name));
        }
    }
    for (const QString &name : required) {
        const bool reached = std::any_of(
            run.milestones.cbegin(), run.milestones.cend(),
            [&name](const vmvalidation::ValidationMilestone &milestone) {
                return milestone.name == name
                    && milestone.status == vmvalidation::MilestoneStatus::Reached;
            });
        if (!reached)
            blockers.append(QStringLiteral("Required milestone '%1' has not passed.").arg(name));
    }
    const bool hashedEvidence = std::any_of(
        run.evidence.cbegin(), run.evidence.cend(),
        [](const vmvalidation::EvidenceReference &evidence) {
            return !evidence.file.sha256.isEmpty();
        });
    if (!hashedEvidence)
        blockers.append(QStringLiteral("At least one hashed evidence file is required."));
    blockers.removeDuplicates();
    return blockers;
}

QVariantMap vmValidationVariant(const vmvalidation::ValidationRun &run,
                                const QString &projectDirectory)
{
    QVariantList milestones;
    for (const vmvalidation::ValidationMilestone &milestone : run.milestones) {
        milestones.append(QVariantMap{
            {QStringLiteral("id"), milestone.id},
            {QStringLiteral("kind"), milestone.name},
            {QStringLiteral("name"), milestone.name},
            {QStringLiteral("phase"), vmvalidation::milestonePhaseName(milestone.phase)},
            {QStringLiteral("result"), validationMilestoneResult(milestone.status)},
            {QStringLiteral("occurredAt"), isoTimestamp(milestone.occurredAt)},
            {QStringLiteral("evidence"), milestone.data.value(QStringLiteral("evidence")).toString()},
            {QStringLiteral("notes"), milestone.note},
            {QStringLiteral("data"), milestone.data.toVariantMap()},
        });
    }
    QVariantList evidence;
    for (const vmvalidation::EvidenceReference &item : run.evidence) {
        evidence.append(QVariantMap{
            {QStringLiteral("id"), item.id},
            {QStringLiteral("kind"), vmvalidation::evidenceKindName(item.kind)},
            {QStringLiteral("label"), item.label},
            {QStringLiteral("path"), item.file.resolvedPath(projectDirectory)},
            {QStringLiteral("sha256"), item.file.sha256},
            {QStringLiteral("capturedAt"), isoTimestamp(item.capturedAt)},
        });
    }
    QStringList logLines;
    for (const vmvalidation::ValidationLogEntry &entry : run.logs) {
        logLines.append(QStringLiteral("[%1] %2: %3")
                            .arg(isoTimestamp(entry.occurredAt), entry.channel, entry.message));
    }
    if (!run.completionNote.isEmpty())
        logLines.append(QStringLiteral("Result: %1").arg(run.completionNote));
    const QString profile = run.configSnapshot.value(QStringLiteral("profile")).toString();
    const QStringList passBlockers = validationPassBlockers(run);
    QString name = run.configSnapshot.value(QStringLiteral("runName")).toString().trimmed();
    if (name.isEmpty())
        name = run.vm.vmName;
    return QVariantMap{
        {QStringLiteral("id"), run.id},
        {QStringLiteral("name"), name},
        {QStringLiteral("profile"), profile},
        {QStringLiteral("status"), vmvalidation::runStatusName(run.status)},
        {QStringLiteral("result"), validationResultName(run.status)},
        {QStringLiteral("startedAt"), isoTimestamp(run.startedAt)},
        {QStringLiteral("finishedAt"), isoTimestamp(run.endedAt)},
        {QStringLiteral("updatedAt"), isoTimestamp(run.updatedAt)},
        {QStringLiteral("providerId"), run.vm.providerId},
        {QStringLiteral("providerVersion"), run.vm.providerVersion},
        {QStringLiteral("vmId"), run.vm.vmId},
        {QStringLiteral("vmName"), run.vm.vmName},
        {QStringLiteral("vmConfigPath"), run.vm.config.resolvedPath(projectDirectory)},
        {QStringLiteral("isoPath"), run.iso.resolvedPath(projectDirectory)},
        {QStringLiteral("isoSha256"), run.iso.sha256},
        {QStringLiteral("imagePath"), run.image.resolvedPath(projectDirectory)},
        {QStringLiteral("imageSha256"), run.image.sha256},
        {QStringLiteral("configSnapshot"), run.configSnapshot.toVariantMap()},
        {QStringLiteral("milestones"), milestones},
        {QStringLiteral("milestoneCount"), validationMilestoneTarget(profile)},
        {QStringLiteral("completedMilestones"), milestones.size()},
        {QStringLiteral("evidenceFiles"), evidence},
        {QStringLiteral("canPass"), run.status == vmvalidation::RunStatus::Running
             && passBlockers.isEmpty()},
        {QStringLiteral("passBlockers"), passBlockers},
        {QStringLiteral("log"), logLines.join(QLatin1Char('\n'))},
        {QStringLiteral("revision"), run.revision},
    };
}

QString vmEvidenceText(const vmlab::OperationEvidence &evidence)
{
    QStringList lines;
    lines.append(QStringLiteral("%1  %2  %3")
                     .arg(isoTimestamp(evidence.finishedAt), evidence.action,
                          evidence.success ? QStringLiteral("OK")
                                           : evidence.cancelled ? QStringLiteral("CANCELLED")
                                                                : QStringLiteral("FAILED")));
    for (const vmlab::CommandTranscript &transcript : evidence.commands) {
        lines.append(QStringLiteral("> %1\n  args: %2")
                         .arg(transcript.command.executable,
                              transcript.command.arguments.join(QStringLiteral(" | "))));
        if (!transcript.result.standardOutput.trimmed().isEmpty())
            lines.append(QString::fromUtf8(transcript.result.standardOutput).trimmed());
        if (transcript.result.standardOutputTruncated)
            lines.append(QStringLiteral("[provider stdout truncated at the bounded capture limit]"));
        if (!transcript.result.standardError.trimmed().isEmpty())
            lines.append(QStringLiteral("stderr: %1")
                             .arg(QString::fromUtf8(transcript.result.standardError).trimmed()));
        if (transcript.result.standardErrorTruncated)
            lines.append(QStringLiteral("[provider stderr truncated at the bounded capture limit]"));
        if (!transcript.result.error.trimmed().isEmpty())
            lines.append(QStringLiteral("error: %1").arg(transcript.result.error));
    }
    for (const vmlab::FileEvidence &file : evidence.files) {
        lines.append(QStringLiteral("file: %1\n  sha256: %2\n  purpose: %3")
                         .arg(file.path, file.expectedSha256, file.description));
    }
    if (!evidence.error.isEmpty())
        lines.append(evidence.error);
    return lines.join(QLatin1Char('\n'));
}

QVariantMap vmPreviewVariant(const vmlab::OperationPreview &preview)
{
    QVariantList commands;
    for (const vmlab::Command &command : preview.commands) {
        commands.append(QVariantMap{
            {QStringLiteral("executable"), command.executable},
            {QStringLiteral("arguments"), command.arguments},
            {QStringLiteral("workingDirectory"), command.workingDirectory},
            {QStringLiteral("timeoutMs"), command.timeoutMs},
            {QStringLiteral("detached"), command.detached},
            {QStringLiteral("interruptible"), command.interruptible},
        });
    }
    return QVariantMap{
        {QStringLiteral("id"), preview.id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("action"), preview.action},
        {QStringLiteral("risk"), vmlab::riskName(preview.risk)},
        {QStringLiteral("target"), QVariantMap{
             {QStringLiteral("providerId"), preview.target.providerId},
             {QStringLiteral("id"), preview.target.id},
             {QStringLiteral("name"), preview.target.name},
         }},
        {QStringLiteral("effects"), preview.effects},
        {QStringLiteral("warnings"), preview.warnings},
        {QStringLiteral("commands"), commands},
        {QStringLiteral("confirmation"), preview.confirmation},
        {QStringLiteral("expiresAt"), isoTimestamp(preview.expiry)},
        {QStringLiteral("revision"), preview.revision},
    };
}

} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent),
      m_notificationStore(NotificationStore::defaultStoreDirectory()),
      m_jobEngine(this),
      m_settings(QStringLiteral("WimForge"), QStringLiteral("WimForge"))
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("controller"),
        QStringLiteral("controller.created"),
        QStringLiteral("Application controller initialization started."));
    m_openCodeSetup = std::make_unique<OpenCodeSetup>();
    connect(m_openCodeSetup.get(), &OpenCodeSetup::changed, this, [this] {
        if (m_openCodeSetup->busy() || m_openCodeSetup->state() == OpenCodeSetupState::Failed)
            m_openCodeRequestStatus.clear();
        emit studioChanged();
    });
    connect(m_openCodeSetup.get(), &OpenCodeSetup::becameReady, this,
            [this](bool installedDuringAttempt) {
        if (installedDuringAttempt) {
            notify(localized(QStringLiteral("OpenCode installed and verified"),
                             QStringLiteral("OpenCode 已安裝兼驗證")),
                   m_openCodeSetup->status(), QStringLiteral("success"));
        }
    });
    connect(m_openCodeSetup.get(), &OpenCodeSetup::failed, this,
            [this](const QString &error) { showError(error); });

    m_languageMode = qBound(0, m_settings.value(QStringLiteral("ui/language"), 2).toInt(), 2);
    m_themeMode = qBound(0, m_settings.value(QStringLiteral("ui/theme"), 0).toInt(), 2);
    m_interfaceScale = qBound(0.8, m_settings.value(QStringLiteral("ui/scale"), 1.0).toDouble(), 1.25);
    m_motionEnabled = m_settings.value(QStringLiteral("ui/motion"), true).toBool();
    m_maxParallelJobs = qBound(1, m_settings.value(QStringLiteral("jobs/parallel"), 4).toInt(), 16);
    m_threadLimit = qBound(1, m_settings.value(QStringLiteral("jobs/threads"), logicalCpuCount()).toInt(), logicalCpuCount());
    m_scratchReserveGb = qBound(5, m_settings.value(QStringLiteral("jobs/reserveGb"), 20).toInt(), 500);
    m_crashJournalEnabled = m_settings.value(QStringLiteral("safety/journal"), true).toBool();
    m_verifySourceHash = m_settings.value(QStringLiteral("safety/hash"), true).toBool();
    m_checkpointBeforeDestructive = m_settings.value(QStringLiteral("safety/checkpoint"), true).toBool();
    loadRecentProjects();
    m_winForgeIncludeRuntime = m_settings.value(QStringLiteral("bridge/includeRuntime"), true).toBool();
    m_winForgeRuntimePath = cleanPath(m_settings.value(QStringLiteral("bridge/runtimePath")).toString());
    if (m_winForgeRuntimePath.isEmpty())
        m_winForgeRuntimePath = discoverWinForgeRuntimePath();
    m_winForgeRecipe = emptyWinForgeRecipe();

    QString notificationError;
    if (m_notificationStore.initialize(&notificationError)) {
        if (m_notificationStore.list(true, true, nullptr).isEmpty()) {
            m_notificationStore.addNotification(
                QStringLiteral("Welcome to WimForge"),
                QStringLiteral("Projects and notifications both have Git history. Even this message can be read, dismissed, deleted and restored without losing its story."),
                QStringLiteral("success"), QStringLiteral("WimForge"), {}, nullptr);
        }
    }
    refreshNotifications();

    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &AppController::onWatchedProjectChanged);
    connect(&m_jobEngine, &JobEngine::stateChanged, this, [this] {
        m_plan = m_jobEngine.operations();
        m_statusText = m_jobEngine.statusText();
        emit stateChanged();
    });
    connect(&m_jobEngine, &JobEngine::operationChanged, this,
            [this](int, const QString &, const QString &) {
        m_plan = m_jobEngine.operations();
        emit stateChanged();
    });
    connect(&m_jobEngine, &JobEngine::finished, this,
            [this](bool success, const QString &message) {
        m_statusText = message;
        notify(success ? QStringLiteral("Servicing completed") : QStringLiteral("Servicing stopped"),
               message, success ? QStringLiteral("success") : QStringLiteral("error"));
        refreshRecoveryState();
        emit stateChanged();
    });

    restoreStudioState();
    if (!m_winForgeRuntimePath.isEmpty()) {
        QString bridgeError;
        m_winForgeRuntimeContract = WinForgeBridge::detectRuntimeContract(
            m_winForgeRuntimePath, &bridgeError);
        m_winForgeRuntimeStatus = m_winForgeRuntimeContract.runtimeFound
            ? QStringLiteral("Detected %1 runtime; capabilities: %2")
                  .arg(m_winForgeRuntimeContract.declaredContract ? QStringLiteral("declared-contract")
                                                                  : QStringLiteral("legacy"),
                       m_winForgeRuntimeContract.capabilities.join(QStringLiteral(", ")))
            : bridgeError;
    }
    if (!m_vmManager)
        recreateVmLab();

    // Host developer tools are intentionally idle at startup. The desktop is
    // elevated, so discovery/verification/setup begins only after the explicit
    // Package Studio action authorizes it for this session.
}

AppController::~AppController()
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("controller"),
        QStringLiteral("controller.destroying"),
        QStringLiteral("Application controller is shutting down."),
        QJsonObject{{QStringLiteral("projectLoaded"), projectLoaded()},
                    {QStringLiteral("jobRunning"), m_jobEngine.isRunning()},
                    {QStringLiteral("sourceInspectionRunning"), m_inspecting}});
    if (m_vmManager && m_vmManager->busy())
        m_vmManager->cancel();
    m_vmManager.reset();
    if (m_vmValidationWorker.joinable())
        m_vmValidationWorker.join();
    if (m_openCodeSetup)
        m_openCodeSetup->shutdown();
    if (!m_openCodeProcess)
        return;
    disconnect(m_openCodeProcess, nullptr, this, nullptr);
    if (m_openCodeProcess->state() != QProcess::NotRunning) {
        m_openCodeProcess->terminate();
        if (!m_openCodeProcess->waitForFinished(1'500)) {
            m_openCodeProcess->kill();
            m_openCodeProcess->waitForFinished(1'500);
        }
    }
    delete m_openCodeProcess;
    m_openCodeProcess = nullptr;
}

QString AppController::version() const { return QString::fromLatin1(WIMFORGE_VERSION); }
bool AppController::projectLoaded() const { return m_project.has_value(); }
QVariantList AppController::recentProjects() const { return m_recentProjects; }
QString AppController::projectName() const { return m_project ? m_project->projectName : QString(); }
QString AppController::projectRoot() const { return m_project ? m_project->projectDirectory : QString(); }
QString AppController::sourcePath() const { return m_project ? m_project->sourcePath : QString(); }
QString AppController::imagePath() const { return m_project ? m_project->imagePath : QString(); }
QString AppController::imageRelativePath() const
{
    return m_project
        ? m_project->options.extra.value(QStringLiteral("imageRelativePath")).toString()
        : QString();
}
QString AppController::mountPath() const { return m_project ? m_project->mountPath : QString(); }
QString AppController::outputPath() const { return m_project ? m_project->outputPath : QString(); }
QString AppController::outputFormat() const { return m_project ? m_project->outputFormat : QStringLiteral("wim"); }
QString AppController::isoLabel() const { return m_project ? m_project->isoLabel : QStringLiteral("WIMFORGE"); }
int AppController::imageIndex() const { return m_project ? m_project->selectedImageIndex : 1; }
bool AppController::cloneSource() const { return !m_project || m_project->cloneSource; }
QStringList AppController::editionNames() const
{
    return m_editionNames.isEmpty()
        ? QStringList{localized(QStringLiteral("Inspect source to load editions"),
                                QStringLiteral("檢查來源以載入版本"))}
        : m_editionNames;
}
QString AppController::imageSummary() const
{
    return localized(m_imageSummaryEn, m_imageSummaryZh);
}
QStringList AppController::drivers() const { return m_project ? m_project->drivers : QStringList(); }
QStringList AppController::updates() const { return m_project ? m_project->updates : QStringList(); }
QStringList AppController::packages() const { return m_project ? m_project->packages : QStringList(); }
QVariantList AppController::driverCatalog() const { return m_driverCatalogItems; }
QVariantList AppController::updateCatalog() const { return m_updateCatalogItems; }
QStringList AppController::features() const { return m_project ? m_project->featuresToEnable : QStringList(); }
QStringList AppController::featureDisables() const
{
    return m_project ? m_project->featuresToDisable : QStringList();
}
QVariantList AppController::capabilityChanges() const
{
    QVariantList changes;
    if (!m_project)
        return changes;
    for (const QString &identity : m_project->capabilitiesToAdd) {
        changes.append(QVariantMap{{QStringLiteral("identity"), identity},
                                   {QStringLiteral("state"), 1},
                                   {QStringLiteral("disposition"), QStringLiteral("add")}});
    }
    for (const QString &identity : m_project->capabilitiesToRemove) {
        changes.append(QVariantMap{{QStringLiteral("identity"), identity},
                                   {QStringLiteral("state"), -1},
                                   {QStringLiteral("disposition"), QStringLiteral("remove")}});
    }
    return changes;
}
QStringList AppController::appRemovals() const { return m_project ? m_project->appxPackagesToRemove : QStringList(); }
QStringList AppController::appProvisions() const
{
    return m_project ? m_project->appxPackagesToProvision : QStringList();
}
QStringList AppController::componentRemovals() const { return m_project ? m_project->componentsToRemove : QStringList(); }
QVariantList AppController::scheduledTaskChanges() const
{
    QVariantList changes;
    if (!m_project)
        return changes;
    for (const ScheduledTaskChange &change : m_project->scheduledTaskChanges) {
        const QString disposition = change.disposition == ScheduledTaskDisposition::Enable
            ? QStringLiteral("enable")
            : change.disposition == ScheduledTaskDisposition::Remove
                ? QStringLiteral("remove") : QStringLiteral("disable");
        changes.append(QVariantMap{
            {QStringLiteral("path"), change.taskPath},
            {QStringLiteral("disposition"), disposition},
            {QStringLiteral("compatibilityOverride"), change.compatibilityOverride},
        });
    }
    return changes;
}
QStringList AppController::unattendedFiles() const { return m_project ? m_project->unattendedFiles : QStringList(); }
QStringList AppController::postSetupItems() const { return m_project ? m_project->postSetupItems : QStringList(); }

QVariantList AppController::operationPlan() const
{
    QVariantList result;
    for (const ServicingOperation &operation : m_plan) {
        QVariantMap item = operation.toJson().toVariantMap();
        item.insert(QStringLiteral("title"), localized(operation.titleEn, operation.titleZh));
        item.insert(QStringLiteral("description"),
                    localized(operation.descriptionEn, operation.descriptionZh));
        item.insert(QStringLiteral("command"), operation.previewCommand());
        item.insert(QStringLiteral("admin"), operation.requiresAdministrator);
        item.insert(QStringLiteral("reboot"), operation.rebootRequired);
        item.insert(QStringLiteral("status"), ServicingPlan::operationStateName(operation.state));
        result.append(item);
    }
    return result;
}

int AppController::operationCount() const { return m_plan.size(); }

QVariantList AppController::projectHistory() const
{
    QVariantList result;
    for (const GitCommit &commit : m_history) {
        result.append(QVariantMap{
            {QStringLiteral("hash"), commit.hash},
            {QStringLiteral("shortHash"), commit.shortHash},
            {QStringLiteral("timestamp"), commit.authoredAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))},
            {QStringLiteral("subject"), commit.subject},
            {QStringLiteral("isRevert"), commit.isRevert()},
        });
    }
    return result;
}

int AppController::projectHistoryCount() const { return m_history.size(); }
QString AppController::gitStatusText() const
{
    return !m_project ? QString() : localized(QStringLiteral("✓ Auto-committed"), QStringLiteral("✓ 已自動 commit"));
}

QVariantList AppController::actionHistory() const
{
    return contextualHistory({}, {});
}

QString AppController::historyBranch() const
{
    if (!m_project)
        return QStringLiteral("main");
    return ActionHistory(m_project->projectDirectory).currentBranch(nullptr);
}

QStringList AppController::historyBranches() const
{
    if (!m_project)
        return {QStringLiteral("main")};
    return ActionHistory(m_project->projectDirectory).branchNames(nullptr);
}

QVariantList AppController::notifications() const
{
    QVariantList result;
    for (const Notification &item : m_notificationItems) {
        result.append(QVariantMap{
            {QStringLiteral("id"), item.id},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("message"), item.message},
            {QStringLiteral("kind"), item.severity},
            {QStringLiteral("timestamp"), notificationTimestamp(item.createdAt)},
            {QStringLiteral("read"), item.isRead},
            {QStringLiteral("dismissed"), item.isDismissed},
            {QStringLiteral("deleted"), item.isDeleted},
        });
    }
    return result;
}

int AppController::notificationUnreadCount() const
{
    return static_cast<int>(std::count_if(m_notificationItems.cbegin(), m_notificationItems.cend(),
        [](const Notification &item) { return !item.isRead && !item.isDismissed && !item.isDeleted; }));
}

QString AppController::notificationRepoPath() const { return m_notificationStore.storeDirectory(); }
bool AppController::busy() const { return m_jobEngine.isRunning() || m_inspecting; }
bool AppController::sourceInspectionBusy() const { return m_inspecting; }
double AppController::progress() const { return m_jobEngine.progress(); }
QString AppController::statusText() const { return m_statusText; }
int AppController::runningJobCount() const { return m_jobEngine.runningCount(); }
bool AppController::pendingRecovery() const { return m_pendingRecovery; }

QString AppController::recoverySummary() const
{
    if (!m_pendingRecovery)
        return localized(QStringLiteral("No interrupted work is waiting."), QStringLiteral("冇中斷工序等緊。"));
    return localized(
        QStringLiteral("Run %1 was interrupted while using mount %2. The source remains untouched. Review the journal, rebuild the plan, or safely unmount.")
            .arg(m_recoveryJournal.value(QStringLiteral("runId")).toString(),
                 m_recoveryJournal.value(QStringLiteral("mountPath")).toString()),
        QStringLiteral("工序 %1 喺掛載 %2 時中斷。來源冇畀人掂過；繼續或者卸載之前先睇日誌。")
            .arg(m_recoveryJournal.value(QStringLiteral("runId")).toString(),
                 m_recoveryJournal.value(QStringLiteral("mountPath")).toString()));
}

QString AppController::recoveryPath() const { return JobEngine::defaultRecoveryRoot(); }
int AppController::languageMode() const { return m_languageMode; }
int AppController::themeMode() const { return m_themeMode; }
double AppController::interfaceScale() const { return m_interfaceScale; }
bool AppController::motionEnabled() const { return m_motionEnabled; }
int AppController::maxParallelJobs() const { return m_maxParallelJobs; }
int AppController::logicalCpuCount() const { return qMax(1, QThread::idealThreadCount()); }
int AppController::threadLimit() const { return m_threadLimit; }
int AppController::scratchReserveGb() const { return m_scratchReserveGb; }
bool AppController::crashJournalEnabled() const { return m_crashJournalEnabled; }
bool AppController::verifySourceHash() const { return m_verifySourceHash; }
bool AppController::checkpointBeforeDestructive() const { return m_checkpointBeforeDestructive; }
bool AppController::autoImport() const { return m_project && m_project->autoImport; }
bool AppController::autoExport() const { return m_project && m_project->autoExport; }
QString AppController::autoExportPath() const { return m_project ? m_project->autoExportPath : QString(); }

QString AppController::defaultProjectPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("WimForge Projects/Windows 11 Custom"));
}

bool AppController::gpoLoaded() const { return m_gpoLoaded; }
int AppController::gpoPolicyCount() const { return m_gpoCatalog.policies().size(); }
QString AppController::gpoStatus() const { return m_gpoStatus; }

QVariantList AppController::gpoResults() const
{
    QVariantList result;
    for (const GpoPolicy &policy : m_gpoSearchResults)
        result.append(gpoPolicyVariant(policy));
    return result;
}

QVariantList AppController::packageCatalog() const
{
    QVariantList result;
    for (const PackageEntry &package : m_packageProfile.packages) {
        result.append(QVariantMap{
            {QStringLiteral("id"), package.id},
            {QStringLiteral("name"), package.displayName},
            {QStringLiteral("description"), package.description},
            {QStringLiteral("provider"), PackageStudio::providerName(package.provider)},
            {QStringLiteral("identifier"), package.packageIdentifier},
            {QStringLiteral("version"), package.version},
            {QStringLiteral("license"), package.license},
            {QStringLiteral("homepage"), package.homepage},
            {QStringLiteral("enabled"), package.enabled},
            {QStringLiteral("optional"), package.optional},
            {QStringLiteral("offline"), !package.requiresNetwork},
        });
    }
    return result;
}

QString AppController::packageProfileName() const { return m_packageProfile.name; }

int AppController::selectedPackageCount() const
{
    return static_cast<int>(std::count_if(m_packageProfile.packages.cbegin(),
                                          m_packageProfile.packages.cend(),
        [](const PackageEntry &package) { return package.enabled; }));
}

QVariantList AppController::unattendedSettings() const
{
    QVariantList result;
    for (const UnattendSetting &setting : m_unattendProfile.settings) {
        QStringList path;
        for (const UnattendPathSegment &segment : setting.path)
            path.append(segment.name);
        result.append(QVariantMap{
            {QStringLiteral("pass"), UnattendBuilder::passName(setting.pass)},
            {QStringLiteral("component"), setting.component},
            {QStringLiteral("path"), path.join(QLatin1Char('/'))},
            {QStringLiteral("value"), setting.value},
            {QStringLiteral("architecture"), setting.architecture},
        });
    }
    return result;
}

QVariantList AppController::microsoftProductKeys() const
{
    QVariantList result;
    for (const ProductKeyEntry &entry : UnattendBuilder::microsoftPublishedGvlks()) {
        result.append(QVariantMap{
            {QStringLiteral("edition"), entry.edition},
            {QStringLiteral("key"), entry.key},
            {QStringLiteral("channel"), entry.channel},
            {QStringLiteral("documentationUrl"), entry.documentationUrl},
            {QStringLiteral("licensingNotice"), entry.licensingNotice},
        });
    }
    return result;
}

int AppController::computerNameMode() const
{
    return static_cast<int>(m_unattendProfile.computerNameMode);
}

QString AppController::computerNameValue() const
{
    return m_unattendProfile.computerNameMode == ComputerNameMode::SerialNumber
        ? m_unattendProfile.serialPrefix : m_unattendProfile.computerName;
}

bool AppController::openCodeInstalled() const
{
    return m_openCodeSetup && m_openCodeSetup->installed();
}

bool AppController::openCodeBusy() const
{
    return (m_openCodeSetup && m_openCodeSetup->busy()) || m_openCodeRequestBusy;
}

bool AppController::openCodeReady() const
{
    return m_openCodeSetup && m_openCodeSetup->ready();
}

bool AppController::openCodeCanRetry() const
{
    return m_openCodeSetup && m_openCodeSetup->canRetry();
}

QString AppController::openCodeState() const
{
    return m_openCodeSetup ? m_openCodeSetup->stateName() : QStringLiteral("absent");
}

QString AppController::openCodeStatus() const
{
    if (!m_openCodeRequestStatus.isEmpty())
        return m_openCodeRequestStatus;
    return m_openCodeSetup ? m_openCodeSetup->status()
                           : localized(QStringLiteral("OpenCode status is unavailable."),
                                       QStringLiteral("而家攞唔到 OpenCode 狀態。"));
}

QString AppController::openCodeError() const
{
    return m_openCodeSetup ? m_openCodeSetup->error() : QString();
}

QVariantList AppController::winForgeBridgeActions() const
{
    QVariantList result;
    for (const WinForgeAction &action : m_winForgeRecipe.actions) {
        const QString capability = winForgeCapability(action.kind);
        const bool supported = capability.isEmpty()
            || (m_winForgeRuntimeContract.runtimeFound
                && m_winForgeRuntimeContract.capabilities.contains(capability)
                && m_winForgeRuntimeContract.invocations.contains(capability));
        QString supportReason;
        if (!supported) {
            supportReason = !m_winForgeRuntimeContract.runtimeFound
                ? QStringLiteral("Select and detect a WinForge runtime before approving this action.")
                : QStringLiteral("Runtime does not declare %1.").arg(capability);
        }
        result.append(QVariantMap{
            {QStringLiteral("id"), action.id},
            {QStringLiteral("title"), action.id},
            {QStringLiteral("kind"), WinForgeBridge::actionKindName(action.kind)},
            {QStringLiteral("phase"), WinForgeBridge::actionPhaseName(action.phase)},
            {QStringLiteral("enabled"), action.enabled},
            {QStringLiteral("target"), action.target},
            {QStringLiteral("summary"), bridgeActionSummary(action)},
            {QStringLiteral("supported"), supported},
            {QStringLiteral("supportReason"), supportReason},
        });
    }
    return result;
}

bool AppController::winForgeBridgeIncludeRuntime() const { return m_winForgeIncludeRuntime; }
QString AppController::winForgeBridgeRuntimePath() const { return m_winForgeRuntimePath; }
QString AppController::winForgeBridgeRuntimeStatus() const { return m_winForgeRuntimeStatus; }
QString AppController::winForgeBridgeStatus() const { return m_winForgeBridgeStatus; }
QVariantList AppController::vmProviders() const
{
    QVariantList result;
    if (!m_vmManager)
        return result;
    for (const vmlab::ProviderInfo &provider : m_vmManager->providers())
        result.append(vmProviderVariant(provider));
    return result;
}

QVariantList AppController::vmInventory() const
{
    QVariantList result;
    if (!m_vmManager)
        return result;
    for (const vmlab::Machine &machine : m_vmManager->machines())
        result.append(vmMachineVariant(machine, {}));
    return result;
}

QString AppController::vmSelectedId() const
{
    if (!m_vmManager)
        return {};
    const std::optional<vmlab::Machine> selected = m_vmManager->selectedMachine();
    return selected ? selected->ref.id : QString();
}

QVariant AppController::vmSelected() const
{
    if (!m_vmManager)
        return {};
    const std::optional<vmlab::Machine> selected = m_vmManager->selectedMachine();
    return selected ? QVariant(vmMachineVariant(*selected, m_vmLog)) : QVariant();
}

QVariantList AppController::vmSnapshots() const
{
    QVariantList result;
    if (!m_vmManager)
        return result;
    for (const vmlab::Snapshot &snapshot : m_vmManager->snapshots())
        result.append(vmSnapshotVariant(snapshot));
    return result;
}

QVariantList AppController::vmValidationRuns() const { return m_vmValidationItems; }
bool AppController::vmBusy() const
{
    return m_vmValidationBusy || (m_vmManager && m_vmManager->busy());
}

QVariantMap AppController::vmStatus() const
{
    const QString state = m_vmManager
        ? vmlab::managerStateName(m_vmManager->state()) : QStringLiteral("unavailable");
    return QVariantMap{
        {QStringLiteral("state"), state},
        {QStringLiteral("tone"), m_vmStatusTone},
        {QStringLiteral("message"), m_vmStatusMessage.isEmpty()
             ? localized(QStringLiteral("VM Lab is ready."),
                         QStringLiteral("VM 實驗室準備好。"))
             : m_vmStatusMessage},
        {QStringLiteral("detail"), m_vmStatusDetail},
        {QStringLiteral("managedRoot"), m_vmManager ? m_vmManager->managedRoot() : QString()},
        {QStringLiteral("projectScoped"), m_project.has_value()},
        {QStringLiteral("validationAvailable"), bool(m_vmValidationStore)},
    };
}

QVariantMap AppController::vmPendingPreview() const { return m_vmPendingPreview; }

QString AppController::currentOutput() const
{
    if (!m_project)
        return {};
    const QString output = cleanPath(m_project->outputPath);
    const QFileInfo outputInfo(output);
    if (!output.isEmpty()
        && outputInfo.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) == 0
        && outputInfo.isFile()) {
        return outputInfo.absoluteFilePath();
    }
    return {};
}

QString AppController::searchQuery() const { return m_searchQuery; }
QVariantList AppController::searchResults() const { return m_searchResults; }
QVariantList AppController::workspaceTabs() const { return m_workspaceTabs.tabs(); }
int AppController::activeWorkspaceTab() const { return m_workspaceTabs.activeIndex(); }
QString AppController::workspaceTabRepository() const { return m_workspaceTabs.repositoryPath(); }
QString AppController::applicationLogPath() const
{
    return StructuredLogger::instance().logPath();
}

void AppController::setLanguageMode(int value)
{
    value = qBound(0, value, 2);
    if (m_languageMode == value) return;
    m_languageMode = value; m_settings.setValue(QStringLiteral("ui/language"), value);
    emit preferencesChanged(); emit stateChanged(); emit notificationsChanged();
}
void AppController::setThemeMode(int value) { value = qBound(0, value, 2); if (m_themeMode == value) return; m_themeMode = value; m_settings.setValue(QStringLiteral("ui/theme"), value); emit preferencesChanged(); }
void AppController::setInterfaceScale(double value) { value = qBound(0.8, value, 1.25); if (qFuzzyCompare(m_interfaceScale, value)) return; m_interfaceScale = value; m_settings.setValue(QStringLiteral("ui/scale"), value); emit preferencesChanged(); }
void AppController::setMotionEnabled(bool value) { if (m_motionEnabled == value) return; m_motionEnabled = value; m_settings.setValue(QStringLiteral("ui/motion"), value); emit preferencesChanged(); }
void AppController::setMaxParallelJobs(int value) { value = qBound(1, value, 16); if (m_maxParallelJobs == value) return; m_maxParallelJobs = value; m_settings.setValue(QStringLiteral("jobs/parallel"), value); emit preferencesChanged(); }
void AppController::setThreadLimit(int value) { value = qBound(1, value, logicalCpuCount()); if (m_threadLimit == value) return; m_threadLimit = value; m_settings.setValue(QStringLiteral("jobs/threads"), value); emit preferencesChanged(); }
void AppController::setScratchReserveGb(int value) { value = qBound(5, value, 500); if (m_scratchReserveGb == value) return; m_scratchReserveGb = value; m_settings.setValue(QStringLiteral("jobs/reserveGb"), value); emit preferencesChanged(); }
void AppController::setCrashJournalEnabled(bool value) { if (m_crashJournalEnabled == value) return; m_crashJournalEnabled = value; m_settings.setValue(QStringLiteral("safety/journal"), value); emit preferencesChanged(); }
void AppController::setVerifySourceHash(bool value) { if (m_verifySourceHash == value) return; m_verifySourceHash = value; m_settings.setValue(QStringLiteral("safety/hash"), value); if (m_project) mutateProject(bilingualCommitMessage(QStringLiteral("safety: change payload verification"), QStringLiteral("安全：更改 payload 驗證")), [value](ProjectConfig &p) { p.options.verifyPayloads = value; }); emit preferencesChanged(); }
void AppController::setCheckpointBeforeDestructive(bool value) { if (m_checkpointBeforeDestructive == value) return; m_checkpointBeforeDestructive = value; m_settings.setValue(QStringLiteral("safety/checkpoint"), value); emit preferencesChanged(); }
void AppController::setAutoImport(bool value) { if (m_project) mutateProject(bilingualCommitMessage(QStringLiteral("automation: change auto import"), QStringLiteral("自動化：更改自動匯入")), [value](ProjectConfig &p) { p.autoImport = value; }); }
void AppController::setAutoExport(bool value) { if (m_project) mutateProject(bilingualCommitMessage(QStringLiteral("automation: change auto export"), QStringLiteral("自動化：更改自動匯出")), [value](ProjectConfig &p) { p.autoExport = value; }); }
void AppController::setAutoExportPath(const QString &value) { if (m_project) mutateProject(bilingualCommitMessage(QStringLiteral("automation: change export destination"), QStringLiteral("自動化：更改匯出目的地")), [value](ProjectConfig &p) { p.autoExportPath = cleanPath(value); }); }

void AppController::loadRecentProjects()
{
    QVariantList candidates = m_settings.value(QStringLiteral("project/recent")).toList();

    // Migrate the previous single-project preference into the list without
    // reopening it. Normal startup must always land on Project Start.
    const QString legacyLast = m_settings.value(QStringLiteral("project/last")).toString();
    if (!legacyLast.trimmed().isEmpty()) {
        candidates.prepend(QVariantMap{
            {QStringLiteral("path"), legacyLast},
            {QStringLiteral("lastOpened"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        });
    }
    m_settings.remove(QStringLiteral("project/last"));

    QVariantList sanitized;
    QSet<QString> seen;
    for (const QVariant &candidate : std::as_const(candidates)) {
        const auto entry = sanitizedRecentProject(candidate);
        if (!entry)
            continue;
        const QString key = recentPathKey(entry->value(QStringLiteral("path")).toString());
        if (key.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        sanitized.append(*entry);
        if (sanitized.size() >= MaximumRecentProjects)
            break;
    }
    m_recentProjects = std::move(sanitized);
    persistRecentProjects();
}

void AppController::rememberRecentProject(const QString &directory, const QString &name)
{
    const auto entry = sanitizedRecentProject(QVariantMap{
        {QStringLiteral("name"), name},
        {QStringLiteral("path"), directory},
        {QStringLiteral("lastOpened"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    });
    if (!entry)
        return;

    const QString wanted = recentPathKey(entry->value(QStringLiteral("path")).toString());
    QVariantList updated{*entry};
    for (const QVariant &candidate : std::as_const(m_recentProjects)) {
        const QString candidatePath = candidate.toMap().value(QStringLiteral("path")).toString();
        if (recentPathKey(candidatePath) != wanted)
            updated.append(candidate);
        if (updated.size() >= MaximumRecentProjects)
            break;
    }
    m_recentProjects = std::move(updated);
    persistRecentProjects();
    emit recentProjectsChanged();
}

void AppController::persistRecentProjects()
{
    if (m_recentProjects.isEmpty())
        m_settings.remove(QStringLiteral("project/recent"));
    else
        m_settings.setValue(QStringLiteral("project/recent"), m_recentProjects);
    m_settings.sync();
}

void AppController::removeRecentProject(const QString &directory)
{
    const QString wanted = recentPathKey(directory);
    if (wanted.isEmpty())
        return;

    bool changed = false;
    for (qsizetype index = m_recentProjects.size(); index-- > 0;) {
        const QString path = m_recentProjects.at(index).toMap()
                                 .value(QStringLiteral("path")).toString();
        if (recentPathKey(path) == wanted) {
            m_recentProjects.removeAt(index);
            changed = true;
        }
    }
    if (!changed)
        return;
    persistRecentProjects();
    emit recentProjectsChanged();
}

void AppController::clearRecentProjects()
{
    if (m_recentProjects.isEmpty())
        return;
    m_recentProjects.clear();
    persistRecentProjects();
    emit recentProjectsChanged();
}

void AppController::requestNewProject() { emit newProjectRequested(); }
void AppController::requestOpenProject() { emit openProjectRequested(); }

bool AppController::createProject(const QString &directory, const QString &name)
{
    ProjectConfig project;
    project.projectDirectory = cleanPath(directory);
    project.projectName = name.trimmed();
    project.mountPath = QDir(project.projectDirectory).filePath(QStringLiteral("mount"));
    project.outputPath = QDir(project.projectDirectory).filePath(QStringLiteral("output/custom-windows.wim"));
    project.options.scratchDirectory = QDir(project.projectDirectory).filePath(QStringLiteral("scratch"));
    project.options.verifyPayloads = m_verifySourceHash;
    project.options.maximumParallelOperations = m_maxParallelJobs;
    QString error;
    if (!project.save(&error, bilingualCommitMessage(
            QStringLiteral("project: create %1").arg(project.projectName),
            QStringLiteral("工程：建立 %1").arg(project.projectName)))) {
        showError(error); return false;
    }
    m_project = std::move(project);
    rememberRecentProject(m_project->projectDirectory, m_project->projectName);
    loadProjectState();
    notify(localized(QStringLiteral("Project created"), QStringLiteral("工程開好")),
           localized(QStringLiteral("Every configuration action is now committed in this project's local Git repository."),
                     QStringLiteral("之後每次改設定，都會 commit 落呢個工程嘅本機 Git repository。")),
           QStringLiteral("success"));
    showSuccess(localized(QStringLiteral("Project created — Git history is active."), QStringLiteral("工程開好 — Git 歷史已經開工。")));
    return true;
}

bool AppController::openProject(const QString &directory)
{
    QString error;
    const auto project = ProjectConfig::load(cleanPath(directory), &error);
    if (!project) { showError(error); return false; }
    m_project = *project;
    rememberRecentProject(m_project->projectDirectory, m_project->projectName);
    loadProjectState();
    showSuccess(localized(QStringLiteral("Project opened."), QStringLiteral("工程開咗。")));
    return true;
}

bool AppController::importProject(const QString &sourceFile, const QString &destinationDirectory)
{
    if (QFileInfo(sourceFile).suffix().compare(QStringLiteral("wimforge"), Qt::CaseInsensitive) == 0) {
        QString error;
        const auto imported = ProjectBundle::importFromFile(cleanPath(sourceFile),
                                                             cleanPath(destinationDirectory), {}, &error);
        if (!imported) {
            showError(error);
            return false;
        }
        const QString projectPath = imported->repositoryPaths.value(ProjectBundle::ProjectRepositoryRole);
        const QString notificationPath = imported->repositoryPaths.value(ProjectBundle::NotificationRepositoryRole);
        const auto project = ProjectConfig::load(projectPath, &error);
        if (!project) {
            showError(error);
            return false;
        }
        m_project = *project;
        if (!notificationPath.isEmpty()) {
            m_notificationStore = NotificationStore(notificationPath);
            if (!m_notificationStore.initialize(&error)) {
                showError(error);
                return false;
            }
            ProjectConfig candidate = *m_project;
            candidate.settings.insert(QStringLiteral("_notificationRepoPath"), notificationPath);
            if (!candidate.save(&error, bilingualCommitMessage(
                    QStringLiteral("bundle: reconnect notification history"),
                    QStringLiteral("Bundle：重新連接通知歷史")))) {
                showError(error);
                return false;
            }
            m_project = candidate;
        }
        rememberRecentProject(m_project->projectDirectory, m_project->projectName);
        loadProjectState();
        refreshNotifications();
        notify(localized(QStringLiteral("Complete project bundle imported"),
                         QStringLiteral("完整工程 bundle 已匯入")),
               localized(QStringLiteral("Project commits, action branches, notification events, tombstones and all local Git metadata were restored."),
                         QStringLiteral("工程 commit、操作 branch、通知事件、刪除記錄同全部本機 Git metadata 都已還原。")),
               QStringLiteral("success"));
        return true;
    }
    QString error;
    const auto project = ProjectConfig::importJson(cleanPath(sourceFile), cleanPath(destinationDirectory), &error);
    if (!project) { showError(error); return false; }
    m_project = *project;
    rememberRecentProject(m_project->projectDirectory, m_project->projectName);
    loadProjectState();
    notify(localized(QStringLiteral("Project imported"), QStringLiteral("工程已匯入")),
           localized(QStringLiteral("The imported configuration now has its own local Git history."),
                     QStringLiteral("匯入咗嘅設定而家有自己一份本機 Git 歷史。")),
           QStringLiteral("success"));
    return true;
}

bool AppController::exportProject(const QString &destinationFile)
{
    if (!m_project) { showError(QStringLiteral("Open a project first.")); return false; }
    QString error;
    const QString destination = cleanPath(destinationFile);
    if (QFileInfo(destination).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0) {
        if (!m_project->exportJson(destination, &error)) { showError(error); return false; }
    } else {
        const QList<ProjectBundleRepository> repositories{
            {ProjectBundle::ProjectRepositoryRole, m_project->projectDirectory,
             QStringLiteral("project")},
            {ProjectBundle::NotificationRepositoryRole, m_notificationStore.storeDirectory(),
             QStringLiteral("notifications")},
        };
        if (!ProjectBundle::exportToFile(destination, repositories, {}, &error)) {
            showError(error);
            return false;
        }
    }
    showSuccess(localized(QStringLiteral("Complete portable project bundle exported."),
                          QStringLiteral("完整可攜工程 bundle 已匯出。")));
    notify(QStringLiteral("Project exported"), destinationFile, QStringLiteral("success"));
    return true;
}

bool AppController::exportScript(const QString &destinationFile)
{
    if (!m_project) { showError(QStringLiteral("Open a project first.")); return false; }
    QString error;
    if (!ServicingPlan::exportPowerShell(*m_project, m_plan, cleanPath(destinationFile), &error)) { showError(error); return false; }
    showSuccess(localized(QStringLiteral("Reproducible PowerShell plan exported."), QStringLiteral("可重現 PowerShell 計劃已匯出。")));
    return true;
}

void AppController::requestExportProject() { emit exportProjectRequested(); }
void AppController::requestExportScript() { emit exportScriptRequested(); }

bool AppController::mutateProject(const QString &message, const ProjectMutation &mutation)
{
    if (!m_project) { showError(QStringLiteral("Open a project first.")); return false; }
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("controller.action"),
        QStringLiteral("project.mutation_started"),
        QStringLiteral("Starting a project configuration mutation."),
        QJsonObject{{QStringLiteral("messageLength"), message.size()},
                    {QStringLiteral("bilingualMessage"), message.contains(QStringLiteral(" / "))}});
    const QJsonObject before = m_project->toJson();
    ProjectConfig candidate = *m_project;
    mutation(candidate);
    QString error;
    if (!candidate.save(&error, message)) { showError(error); return false; }
    const QJsonObject after = candidate.toJson();
    m_project = std::move(candidate);

    ActionDraft action;
    const qsizetype separator = message.indexOf(QLatin1Char(':'));
    action.contextKey = separator > 0 ? message.left(separator).trimmed() : QStringLiteral("project");
    action.elementId = separator >= 0 ? message.mid(separator + 1).simplified() : message.simplified();
    action.title = message;
    action.description = QStringLiteral("Committed project configuration change.");
    action.icon = action.contextKey == QStringLiteral("gpo") ? QStringLiteral("policy")
        : action.contextKey == QStringLiteral("packages") ? QStringLiteral("inventory_2")
        : action.contextKey == QStringLiteral("unattended") ? QStringLiteral("auto_fix_high")
        : QStringLiteral("edit");
    action.forwardDiff = ActionHistory::createMergePatch(before, after);
    action.inverseDiff = ActionHistory::createMergePatch(after, before);
    action.metadata = QJsonObject{{QStringLiteral("diffSummary"), message},
                                  {QStringLiteral("stateFormat"), QStringLiteral("merge-patch")},
                                  {QStringLiteral("beforeState"), before},
                                  {QStringLiteral("afterState"), after}};
    QString historyError;
    if (!ActionHistory(m_project->projectDirectory).record(action, nullptr, &historyError)) {
        notify(QStringLiteral("Context history warning"),
               QStringLiteral("The project Git commit is safe, but the contextual event could not be recorded: %1")
                   .arg(historyError), QStringLiteral("warning"));
    }
    if (m_project->autoExport
        && QFileInfo(m_project->autoExportPath).suffix().compare(
               QStringLiteral("wimforge"), Qt::CaseInsensitive) == 0) {
        const QList<ProjectBundleRepository> repositories{
            {ProjectBundle::ProjectRepositoryRole, m_project->projectDirectory,
             QStringLiteral("project")},
            {ProjectBundle::NotificationRepositoryRole, m_notificationStore.storeDirectory(),
             QStringLiteral("notifications")},
        };
        QString bundleError;
        if (!ProjectBundle::exportToFile(m_project->autoExportPath, repositories, {}, &bundleError))
            showError(QStringLiteral("The action was committed, but complete auto-export failed: %1")
                          .arg(bundleError));
    }
    loadProjectState();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("controller.action"),
        QStringLiteral("project.mutation_committed"),
        QStringLiteral("Project configuration mutation committed."),
        QJsonObject{{QStringLiteral("messageLength"), message.size()},
                    {QStringLiteral("bilingualMessage"), message.contains(QStringLiteral(" / "))}});
    return true;
}

bool AppController::saveProject(const QString &message)
{
    return mutateProject(message, [](ProjectConfig &) {});
}

void AppController::setProjectField(const QString &field, const QString &value)
{
    mutateProject(bilingualCommitMessage(
                      QStringLiteral("config: set %1").arg(field),
                      QStringLiteral("設定：更新 %1").arg(field)),
                  [field, value](ProjectConfig &p) {
        const QString path = cleanPath(value);
        if (field == QStringLiteral("sourcePath")) {
            if (p.sourcePath != path) {
                p.sourcePath = path;
                p.imagePath.clear();
                p.selectedImageIndex = 1;
                p.options.extra.remove(QStringLiteral("imageRelativePath"));
                p.options.extra.remove(QStringLiteral("imageInventory"));
            }
        }
        else if (field == QStringLiteral("imagePath")) {
            if (p.imagePath != path
                || p.options.extra.contains(QStringLiteral("imageRelativePath"))) {
                p.imagePath = path;
                p.selectedImageIndex = 1;
                p.options.extra.remove(QStringLiteral("imageInventory"));
                if (!path.isEmpty())
                    p.options.extra.remove(QStringLiteral("imageRelativePath"));
            }
        }
        else if (field == QStringLiteral("mountPath")) p.mountPath = path;
        else if (field == QStringLiteral("outputPath")) p.outputPath = path;
        else if (field == QStringLiteral("outputFormat")) p.outputFormat = value.trimmed().toLower();
        else if (field == QStringLiteral("isoLabel")) p.isoLabel = value.trimmed();
        else if (field == QStringLiteral("unattendedXmlPath")) p.unattendedXmlPath = path;
    });
}

void AppController::setProjectBool(const QString &field, bool value)
{
    mutateProject(bilingualCommitMessage(
                      QStringLiteral("config: set %1").arg(field),
                      QStringLiteral("設定：更新 %1").arg(field)),
                  [field, value](ProjectConfig &p) {
        if (field == QStringLiteral("cloneSource")) p.cloneSource = value;
        else if (field == QStringLiteral("createIso")) p.options.createIso = value;
        else if (field == QStringLiteral("resetBase")) p.options.resetBase = value;
        else if (field == QStringLiteral("dryRun")) p.options.dryRun = value;
    });
}

void AppController::setProjectNumber(const QString &field, int value)
{
    const int knownImageMaximum = m_editionNames.isEmpty()
        ? std::numeric_limits<int>::max() : m_editionNames.size();
    mutateProject(bilingualCommitMessage(
                      QStringLiteral("config: set %1").arg(field),
                      QStringLiteral("設定：更新 %1").arg(field)),
                  [field, value, knownImageMaximum](ProjectConfig &p) {
        if (field == QStringLiteral("imageIndex"))
            p.selectedImageIndex = qBound(1, value, knownImageMaximum);
        else if (field == QStringLiteral("parallel")) p.options.maximumParallelOperations = qBound(1, value, 16);
        else if (field == QStringLiteral("splitSizeMb")) p.options.extra.insert(QStringLiteral("splitSizeMb"), qBound(100, value, 4095));
    });
}

QStringList *AppController::listForCategory(ProjectConfig &project, const QString &category)
{
    if (category == QStringLiteral("drivers")) return &project.drivers;
    if (category == QStringLiteral("packages")) return &project.packages;
    if (category == QStringLiteral("updates")) return &project.updates;
    if (category == QStringLiteral("features")) return &project.featuresToEnable;
    if (category == QStringLiteral("featureDisables")) return &project.featuresToDisable;
    if (category == QStringLiteral("capabilityAdds")) return &project.capabilitiesToAdd;
    if (category == QStringLiteral("capabilityRemovals")) return &project.capabilitiesToRemove;
    if (category == QStringLiteral("appRemovals")) return &project.appxPackagesToRemove;
    if (category == QStringLiteral("appProvision")) return &project.appxPackagesToProvision;
    if (category == QStringLiteral("componentRemovals")) return &project.componentsToRemove;
    if (category == QStringLiteral("unattendFiles")) return &project.unattendedFiles;
    if (category == QStringLiteral("postSetupItems")) return &project.postSetupItems;
    return nullptr;
}

void AppController::addListItem(const QString &category, const QString &value)
{
    tryAddListItem(category, value);
}

bool AppController::tryAddListItem(const QString &category, const QString &value)
{
    const QString item = value.trimmed();
    if (item.isEmpty())
        return false;
    if (!m_project) {
        showError(localized(QStringLiteral("Open a project before adding an item."),
                            QStringLiteral("加入項目之前請先開工程。")));
        return false;
    }
    QStringList *current = listForCategory(*m_project, category);
    if (!current) {
        showError(localized(
            QStringLiteral("Unsupported project-list category: %1").arg(category),
            QStringLiteral("唔支援呢個工程清單類別：%1").arg(category)));
        return false;
    }
    if (current->contains(item, Qt::CaseInsensitive))
        return true;
    return mutateProject(bilingualCommitMessage(
                      QStringLiteral("config: add %1 item").arg(category),
                      QStringLiteral("設定：加入 %1 項目").arg(category)),
                  [this, category, item](ProjectConfig &p) {
        if (QStringList *list = listForCategory(p, category); list && !list->contains(item, Qt::CaseInsensitive))
            list->append(item);
    });
}

void AppController::removeListItem(const QString &category, int index)
{
    mutateProject(bilingualCommitMessage(
                      QStringLiteral("config: remove %1 item").arg(category),
                      QStringLiteral("設定：移除 %1 項目").arg(category)),
                  [this, category, index](ProjectConfig &p) {
        if (QStringList *list = listForCategory(p, category); list && index >= 0 && index < list->size())
            list->removeAt(index);
    });
}

bool AppController::addPayloadFiles(const QString &category, const QVariantList &files)
{
    if (!m_project) {
        showError(localized(QStringLiteral("Open a project before adding payloads."),
                            QStringLiteral("加 payload 之前請先開工程。")));
        return false;
    }
    const bool driversCategory = category == QStringLiteral("drivers");
    const bool updatesCategory = category == QStringLiteral("updates");
    if (!driversCategory && !updatesCategory) {
        showError(localized(
            QStringLiteral("Unsupported payload category: %1").arg(category),
            QStringLiteral("唔支援呢個 payload 類別：%1").arg(category)));
        return false;
    }
    const ServicingPayloadKind kind = driversCategory
        ? ServicingPayloadKind::Driver : ServicingPayloadKind::Update;
    const QStringList existing = driversCategory ? m_project->drivers : m_project->updates;
    QStringList accepted;
    QStringList rejected;
    for (const QVariant &value : files) {
        const QUrl url = value.toUrl();
        QString path = url.isLocalFile() ? url.toLocalFile() : value.toString();
        if (path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
            path = QUrl(path).toLocalFile();
        path = cleanPath(path);
        const QFileInfo info(path);
        if (path.isEmpty() || !info.isFile()
            || !PayloadCatalog::isSupportedFile(path, kind)) {
            if (!path.isEmpty())
                rejected.append(path);
            continue;
        }
        path = info.absoluteFilePath();
        if (!existing.contains(path, Qt::CaseInsensitive)
            && !accepted.contains(path, Qt::CaseInsensitive)) {
            accepted.append(path);
        }
    }
    if (accepted.isEmpty()) {
        const QString expected = driversCategory ? QStringLiteral("INF")
                                                 : QStringLiteral("CAB/MSU");
        showError(localized(
            QStringLiteral("No new %1 files were selected. Choose existing %1 payloads.")
                .arg(expected),
            QStringLiteral("未揀到新嘅 %1 檔案。請揀現有嘅 %1 payload。").arg(expected)));
        return false;
    }
    const bool saved = mutateProject(
        bilingualCommitMessage(
            QStringLiteral("%1: add %2 local payload(s)").arg(category).arg(accepted.size()),
            QStringLiteral("%1：加入 %2 個本機 payload").arg(category).arg(accepted.size())),
        [category, accepted, this](ProjectConfig &project) {
            if (QStringList *list = listForCategory(project, category)) {
                for (const QString &path : accepted) {
                    if (!list->contains(path, Qt::CaseInsensitive))
                        list->append(path);
                }
            }
        });
    if (!saved)
        return false;
    emit snackbarRequested(localized(
        QStringLiteral("Queued %1 locally inspected payload(s)%2.")
            .arg(accepted.size())
            .arg(rejected.isEmpty() ? QString()
                                    : QStringLiteral("; skipped %1 unsupported path(s)")
                                          .arg(rejected.size())),
        QStringLiteral("已排隊 %1 個本機 payload%2。").arg(accepted.size()).arg(
            rejected.isEmpty() ? QString()
                               : QStringLiteral("；略過 %1 個唔支援路徑").arg(rejected.size()))),
        rejected.isEmpty() ? QStringLiteral("success") : QStringLiteral("warning"));
    return true;
}

bool AppController::addPayloadDirectory(const QString &category, const QUrl &directory)
{
    if (!m_project) {
        showError(localized(QStringLiteral("Open a project before adding a payload folder."),
                            QStringLiteral("加 payload 資料夾之前請先開工程。")));
        return false;
    }
    const bool driversCategory = category == QStringLiteral("drivers");
    const bool updatesCategory = category == QStringLiteral("updates");
    if (!driversCategory && !updatesCategory) {
        showError(localized(
            QStringLiteral("Unsupported payload category: %1").arg(category),
            QStringLiteral("唔支援呢個 payload 類別：%1").arg(category)));
        return false;
    }
    const QString path = cleanPath(directory.isLocalFile() ? directory.toLocalFile()
                                                           : directory.toString());
    const QFileInfo info(path);
    if (!info.isDir()) {
        showError(localized(QStringLiteral("Choose an existing payload folder."),
                            QStringLiteral("請揀一個存在嘅 payload 資料夾。")));
        return false;
    }

    const ServicingPayloadKind kind = driversCategory
        ? ServicingPayloadKind::Driver : ServicingPayloadKind::Update;
    const QStringList discovered = PayloadCatalog::discoverFiles(info.absoluteFilePath(), kind);
    if (discovered.isEmpty()) {
        showError(localized(
            driversCategory
                ? QStringLiteral("That folder contains no INF driver packages.")
                : QStringLiteral("That folder contains no CAB or MSU update packages."),
            driversCategory
                ? QStringLiteral("嗰個資料夾搵唔到 INF 驅動套件。")
                : QStringLiteral("嗰個資料夾搵唔到 CAB 或 MSU 更新套件。")));
        return false;
    }

    QStringList accepted;
    if (driversCategory) {
        const QString folder = info.absoluteFilePath();
        if (!m_project->drivers.contains(folder, Qt::CaseInsensitive))
            accepted.append(folder);
    } else {
        for (const QString &file : discovered) {
            if (!m_project->updates.contains(file, Qt::CaseInsensitive))
                accepted.append(file);
        }
    }
    if (accepted.isEmpty()) {
        emit snackbarRequested(localized(QStringLiteral("Every discovered payload is already queued."),
                                         QStringLiteral("搵到嘅 payload 已經全部排咗隊。")),
                                 QStringLiteral("info"));
        return true;
    }

    const bool saved = mutateProject(
        bilingualCommitMessage(
            QStringLiteral("%1: import folder (%2 discovered file(s))")
                .arg(category).arg(discovered.size()),
            QStringLiteral("%1：匯入資料夾（搵到 %2 個檔案）")
                .arg(category).arg(discovered.size())),
        [category, accepted, this](ProjectConfig &project) {
            if (QStringList *list = listForCategory(project, category)) {
                for (const QString &item : accepted) {
                    if (!list->contains(item, Qt::CaseInsensitive))
                        list->append(item);
                }
            }
        });
    if (saved) {
        emit snackbarRequested(localized(
            driversCategory
                ? QStringLiteral("Queued one recursive driver source with %1 INF package(s).")
                      .arg(discovered.size())
                : QStringLiteral("Queued %1 CAB/MSU update package(s) from that folder.")
                      .arg(accepted.size()),
            driversCategory
                ? QStringLiteral("已排隊一個驅動來源，入面有 %1 個 INF 套件。")
                      .arg(discovered.size())
                : QStringLiteral("已由資料夾排隊 %1 個 CAB/MSU 更新套件。")
                      .arg(accepted.size())),
            QStringLiteral("success"));
    }
    return saved;
}

void AppController::refreshPayloadCatalog()
{
    reloadPayloadCatalog(true);
}

void AppController::openMicrosoftUpdateCatalog(const QString &query)
{
    QUrl url(query.trimmed().isEmpty()
                 ? QStringLiteral("https://www.catalog.update.microsoft.com/Home.aspx")
                 : QStringLiteral("https://www.catalog.update.microsoft.com/Search.aspx"));
    if (!query.trimmed().isEmpty()) {
        QUrlQuery parameters;
        parameters.addQueryItem(QStringLiteral("q"), query.trimmed());
        url.setQuery(parameters);
    }
    if (!QDesktopServices::openUrl(url)) {
        showError(localized(QStringLiteral("Windows could not open the Microsoft Update Catalog."),
                            QStringLiteral("Windows 開唔到 Microsoft Update Catalog。")));
    }
}

void AppController::setFeature(const QString &name, bool enabled)
{
    setFeatureState(name, enabled ? 1 : -1);
}

int AppController::featureState(const QString &name) const
{
    if (!m_project)
        return 0;
    if (m_project->featuresToEnable.contains(name.trimmed(), Qt::CaseInsensitive))
        return 1;
    if (m_project->featuresToDisable.contains(name.trimmed(), Qt::CaseInsensitive))
        return -1;
    return 0;
}

bool AppController::setFeatureState(const QString &name, int state)
{
    const QString identity = name.trimmed();
    if (!isSafeConfigurationIdentity(identity) || state < -1 || state > 1) {
        showError(localized(
            QStringLiteral("Choose a valid Windows feature identity and state."),
            QStringLiteral("請揀有效嘅 Windows 功能 identity 同狀態。")));
        return false;
    }
    if (!m_project) {
        showError(localized(QStringLiteral("Open a project before changing features."),
                            QStringLiteral("改功能之前請先開工程。")));
        return false;
    }
    if (featureState(identity) == state)
        return true;

    const QString actionEn = state > 0 ? QStringLiteral("enable")
        : state < 0 ? QStringLiteral("disable") : QStringLiteral("leave unchanged");
    const QString actionZh = state > 0 ? QStringLiteral("啟用")
        : state < 0 ? QStringLiteral("停用") : QStringLiteral("回復不變");
    return mutateProject(
        bilingualCommitMessage(
            QStringLiteral("feature: %1 %2").arg(actionEn, identity),
            QStringLiteral("功能：%1 %2").arg(actionZh, identity)),
        [identity, state](ProjectConfig &project) {
            removeCaseInsensitive(project.featuresToEnable, identity);
            removeCaseInsensitive(project.featuresToDisable, identity);
            if (state > 0)
                project.featuresToEnable.append(identity);
            else if (state < 0)
                project.featuresToDisable.append(identity);
        });
}

int AppController::capabilityState(const QString &name) const
{
    if (!m_project)
        return 0;
    if (m_project->capabilitiesToAdd.contains(name.trimmed(), Qt::CaseInsensitive))
        return 1;
    if (m_project->capabilitiesToRemove.contains(name.trimmed(), Qt::CaseInsensitive))
        return -1;
    return 0;
}

bool AppController::setCapabilityState(const QString &name, int state)
{
    const QString identity = name.trimmed();
    if (!isSafeConfigurationIdentity(identity) || state < -1 || state > 1) {
        showError(localized(
            QStringLiteral("Choose a valid Windows capability identity and state."),
            QStringLiteral("請揀有效嘅 Windows capability identity 同狀態。")));
        return false;
    }
    if (!m_project) {
        showError(localized(QStringLiteral("Open a project before changing capabilities."),
                            QStringLiteral("改 capability 之前請先開工程。")));
        return false;
    }
    if (capabilityState(identity) == state)
        return true;

    const QString actionEn = state > 0 ? QStringLiteral("add")
        : state < 0 ? QStringLiteral("remove") : QStringLiteral("leave unchanged");
    const QString actionZh = state > 0 ? QStringLiteral("加入")
        : state < 0 ? QStringLiteral("移除") : QStringLiteral("回復不變");
    return mutateProject(
        bilingualCommitMessage(
            QStringLiteral("capability: %1 %2").arg(actionEn, identity),
            QStringLiteral("能力：%1 %2").arg(actionZh, identity)),
        [identity, state](ProjectConfig &project) {
            removeCaseInsensitive(project.capabilitiesToAdd, identity);
            removeCaseInsensitive(project.capabilitiesToRemove, identity);
            if (state > 0)
                project.capabilitiesToAdd.append(identity);
            else if (state < 0)
                project.capabilitiesToRemove.append(identity);
        });
}

bool AppController::addAppxProvisionFiles(const QVariantList &files)
{
    if (!m_project) {
        showError(localized(
            QStringLiteral("Open a project before provisioning apps."),
            QStringLiteral("預載 App 之前請先開工程。")));
        return false;
    }

    QStringList accepted;
    QStringList rejected;
    for (const QVariant &value : files) {
        const QUrl url = value.toUrl();
        QString path = url.isLocalFile() ? url.toLocalFile() : value.toString();
        if (path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
            path = QUrl(path).toLocalFile();
        const QFileInfo file(cleanPath(path));
        if (!isAppxProvisioningPayload(file)) {
            if (!path.trimmed().isEmpty())
                rejected.append(path);
            continue;
        }
        const QString absolute = file.absoluteFilePath();
        if (!m_project->appxPackagesToProvision.contains(absolute, Qt::CaseInsensitive)
            && !accepted.contains(absolute, Qt::CaseInsensitive)) {
            accepted.append(absolute);
        }
    }

    if (accepted.isEmpty()) {
        showError(localized(
            QStringLiteral("No new signed Appx/MSIX package was selected. Choose an existing .appx, .appxbundle, .msix, or .msixbundle file."),
            QStringLiteral("未揀到新嘅已簽署 Appx/MSIX 套件。請揀現有嘅 .appx、.appxbundle、.msix 或 .msixbundle 檔案。")));
        return false;
    }

    const bool saved = mutateProject(
        bilingualCommitMessage(
            QStringLiteral("appx: provision %1 signed package(s)").arg(accepted.size()),
            QStringLiteral("Appx：預載 %1 個已簽署套件").arg(accepted.size())),
        [accepted](ProjectConfig &project) {
            for (const QString &path : accepted) {
                if (!project.appxPackagesToProvision.contains(path, Qt::CaseInsensitive))
                    project.appxPackagesToProvision.append(path);
            }
        });
    if (!saved)
        return false;

    emit snackbarRequested(localized(
        QStringLiteral("Queued %1 signed Appx/MSIX package(s)%2. DISM verifies package signatures during servicing.")
            .arg(accepted.size())
            .arg(rejected.isEmpty() ? QString()
                                    : QStringLiteral("; skipped %1 unsupported file(s)")
                                          .arg(rejected.size())),
        QStringLiteral("已排隊 %1 個已簽署 Appx/MSIX 套件%2。DISM 會喺維護期間驗證套件簽署。")
            .arg(accepted.size())
            .arg(rejected.isEmpty() ? QString()
                                    : QStringLiteral("；略過 %1 個唔支援嘅檔案")
                                          .arg(rejected.size()))),
        rejected.isEmpty() ? QStringLiteral("success") : QStringLiteral("warning"));
    return true;
}

bool AppController::setScheduledTaskChange(const QString &taskPath,
                                           const QString &disposition,
                                           bool compatibilityOverride)
{
    if (!m_project) {
        showError(localized(
            QStringLiteral("Open a project before changing scheduled tasks."),
            QStringLiteral("改排程工作之前請先開工程。")));
        return false;
    }
    const QString path = normalizedTaskPath(taskPath);
    if (!isSafeScheduledTaskPath(path)) {
        showError(localized(
            QStringLiteral("Use a safe task path relative to Windows\\System32\\Tasks; absolute paths and parent traversal are blocked."),
            QStringLiteral("請用相對於 Windows\\System32\\Tasks 嘅安全工作路徑；唔准絕對路徑同返去上層。")));
        return false;
    }

    const QString normalizedDisposition = disposition.trimmed().toLower();
    ScheduledTaskDisposition parsedDisposition;
    if (normalizedDisposition == QStringLiteral("enable")) {
        parsedDisposition = ScheduledTaskDisposition::Enable;
    } else if (normalizedDisposition == QStringLiteral("disable")) {
        parsedDisposition = ScheduledTaskDisposition::Disable;
    } else if (normalizedDisposition == QStringLiteral("remove")
               || normalizedDisposition == QStringLiteral("delete")) {
        parsedDisposition = ScheduledTaskDisposition::Remove;
    } else {
        showError(localized(
            QStringLiteral("Scheduled-task action must be Enable, Disable, or Delete."),
            QStringLiteral("排程工作動作一定要係啟用、停用或者刪除。")));
        return false;
    }
    if (parsedDisposition == ScheduledTaskDisposition::Remove
        && !compatibilityOverride) {
        showError(localized(
            QStringLiteral("Deleting an offline scheduled task requires the explicit compatibility override."),
            QStringLiteral("刪除離線排程工作之前，一定要明確確認相容性解鎖。")));
        return false;
    }

    const QString actionEn = parsedDisposition == ScheduledTaskDisposition::Enable
        ? QStringLiteral("enable")
        : parsedDisposition == ScheduledTaskDisposition::Disable
            ? QStringLiteral("disable") : QStringLiteral("delete");
    const QString actionZh = parsedDisposition == ScheduledTaskDisposition::Enable
        ? QStringLiteral("啟用")
        : parsedDisposition == ScheduledTaskDisposition::Disable
            ? QStringLiteral("停用") : QStringLiteral("刪除");
    return mutateProject(
        bilingualCommitMessage(
            QStringLiteral("scheduled task: %1 %2").arg(actionEn, path),
            QStringLiteral("排程工作：%1 %2").arg(actionZh, path)),
        [path, parsedDisposition, compatibilityOverride](ProjectConfig &project) {
            auto found = std::find_if(project.scheduledTaskChanges.begin(),
                                      project.scheduledTaskChanges.end(),
                                      [&path](const ScheduledTaskChange &change) {
                return change.taskPath.compare(path, Qt::CaseInsensitive) == 0;
            });
            const ScheduledTaskChange replacement{
                path, parsedDisposition,
                parsedDisposition == ScheduledTaskDisposition::Remove
                    && compatibilityOverride,
            };
            if (found == project.scheduledTaskChanges.end())
                project.scheduledTaskChanges.append(replacement);
            else
                *found = replacement;
        });
}

bool AppController::removeScheduledTaskChange(int index)
{
    if (!m_project || index < 0 || index >= m_project->scheduledTaskChanges.size()) {
        showError(localized(
            QStringLiteral("Choose a queued scheduled-task change to remove."),
            QStringLiteral("請揀一項已排隊嘅排程工作變更嚟移除。")));
        return false;
    }
    const QString path = m_project->scheduledTaskChanges.at(index).taskPath;
    return mutateProject(
        bilingualCommitMessage(
            QStringLiteral("scheduled task: clear queued change for %1").arg(path),
            QStringLiteral("排程工作：清除 %1 嘅排隊變更").arg(path)),
        [index](ProjectConfig &project) {
            if (index >= 0 && index < project.scheduledTaskChanges.size())
                project.scheduledTaskChanges.removeAt(index);
        });
}

void AppController::setSetting(const QString &name, bool enabled)
{
    mutateProject(bilingualCommitMessage(
                      QStringLiteral("setting: %1 %2").arg(enabled ? QStringLiteral("enable") : QStringLiteral("disable"), name),
                      QStringLiteral("設定：%1 %2").arg(enabled ? QStringLiteral("啟用") : QStringLiteral("停用"), name)),
                  [name, enabled](ProjectConfig &p) { p.settings.insert(name, enabled); });
}

bool AppController::settingEnabled(const QString &name) const
{
    return m_project && m_project->settings.value(name).toBool(false);
}

void AppController::inspectSource()
{
    if (!m_project) {
        showError(localized(QStringLiteral("Create or open a project before choosing a source."),
                            QStringLiteral("請先建立或者開啟工程，先至揀來源。")));
        return;
    }
    if (m_inspecting) {
        showError(localized(QStringLiteral("Source inspection is already running."),
                            QStringLiteral("來源點貨已經進行緊。")));
        return;
    }

    const QString sourceAtLaunch = m_project->sourcePath;
    const ImageInspectionCommand command = ImageSourceInspector::commandFor(
        sourceAtLaunch, m_project->imagePath);
    if (!command.error.isEmpty()) {
        showError(command.error);
        return;
    }

    m_inspecting = true;
    m_statusText = command.isoSource
        ? localized(QStringLiteral("Mounting the ISO read-only and inspecting editions"),
                    QStringLiteral("以唯讀方式掛載 ISO，再檢查版本"))
        : localized(QStringLiteral("Inspecting image editions"),
                    QStringLiteral("檢查映像版本"));
    emit stateChanged();
    auto *process = new QProcess(this);
    process->setProgram(resolveExecutableForLaunch(command.program));
    process->setArguments(command.arguments);
    process->setProcessEnvironment(command.environment);
    process->setProcessChannelMode(QProcess::MergedChannels);
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("process.source_inspection"),
        QStringLiteral("process.launch_requested"),
        QStringLiteral("Starting Windows image-source inspection."),
        QJsonObject{{QStringLiteral("argumentCount"), command.arguments.size()},
                    {QStringLiteral("isoSource"), command.isoSource}});
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, command, sourceAtLaunch](int exitCode, QProcess::ExitStatus status) {
        const QByteArray rawOutput = process->readAll();
        const ImageInspectionResult result = ImageSourceInspector::parseOutput(
            rawOutput, command.isoSource, command.utf8Output);
        StructuredLogger::instance().log(
            status == QProcess::NormalExit && exitCode == 0
                ? LogSeverity::Info : LogSeverity::Error,
            QStringLiteral("process.source_inspection"),
            QStringLiteral("process.finished"),
            QStringLiteral("Windows image-source inspection finished."),
            QJsonObject{{QStringLiteral("argumentCount"), command.arguments.size()},
                        {QStringLiteral("exitCode"), exitCode},
                        {QStringLiteral("normalExit"), status == QProcess::NormalExit},
                        {QStringLiteral("isoSource"), command.isoSource},
                        {QStringLiteral("outputBytes"), rawOutput.size()}});
        process->deleteLater(); m_inspecting = false;
        if (status != QProcess::NormalExit || exitCode != 0) {
            m_statusText = localized(QStringLiteral("Image inspection failed"),
                                     QStringLiteral("映像檢查失敗"));
            showError(result.output.trimmed().isEmpty()
                          ? localized(QStringLiteral("Windows could not inspect the selected source."),
                                      QStringLiteral("Windows 檢查唔到揀咗嘅來源。"))
                          : result.output.trimmed());
            emit stateChanged();
            return;
        }
        if (!m_project || cleanPath(m_project->sourcePath) != cleanPath(sourceAtLaunch)) {
            m_statusText = localized(QStringLiteral("Source changed; inspection result was ignored"),
                                     QStringLiteral("來源已經改咗；今次結果唔採用"));
            emit stateChanged();
            return;
        }
        if (result.editions.isEmpty()
            || (command.isoSource && result.relativeImagePath.isEmpty())) {
            m_statusText = localized(QStringLiteral("Image inspection returned incomplete metadata"),
                                     QStringLiteral("映像檢查資料唔完整"));
            showError(localized(
                QStringLiteral("DISM completed, but WimForge could not read an image path and edition list from its output."),
                QStringLiteral("DISM 完成咗，但 WimForge 讀唔到映像路徑同版本清單。")));
            emit stateChanged();
            return;
        }

        const QString relativeImage = result.relativeImagePath;
        const QString imageDescription = command.isoSource
            ? QStringLiteral("%1 inside %2")
                  .arg(relativeImage, QFileInfo(sourceAtLaunch).fileName())
            : command.imagePath;
        const QString summaryEn = QStringLiteral("%1 edition(s) · %2")
                                      .arg(result.editions.size())
                                      .arg(imageDescription);
        const QString summaryZh = QStringLiteral("%1 個版本 · %2")
                                      .arg(result.editions.size())
                                      .arg(imageDescription);
        const QJsonObject inventory{
            {QStringLiteral("editions"), QJsonArray::fromStringList(result.editions)},
            {QStringLiteral("summaryEn"), summaryEn},
            {QStringLiteral("summaryZh"), summaryZh},
        };
        const int editionCount = static_cast<int>(result.editions.size());
        const bool saved = mutateProject(
            bilingualCommitMessage(
                command.isoSource
                    ? QStringLiteral("source: inspect image inventory inside ISO")
                    : QStringLiteral("source: inspect image-container inventory"),
                command.isoSource
                    ? QStringLiteral("來源：檢查 ISO 入面嘅映像版本清單")
                    : QStringLiteral("來源：檢查映像容器版本清單")),
            [command, relativeImage, inventory, editionCount](ProjectConfig &project) {
                if (command.isoSource) {
                    project.imagePath.clear();
                    project.options.extra.insert(QStringLiteral("imageRelativePath"),
                                                 relativeImage);
                } else {
                    project.imagePath = command.imagePath;
                    project.options.extra.remove(QStringLiteral("imageRelativePath"));
                }
                project.options.extra.insert(QStringLiteral("imageInventory"), inventory);
                project.selectedImageIndex = qBound(1, project.selectedImageIndex,
                                                    editionCount);
            });
        if (!saved)
            return;

        m_statusText = localized(QStringLiteral("Image inventory ready"),
                                 QStringLiteral("映像點貨完成"));
        notify(localized(QStringLiteral("Image inventory ready"),
                         QStringLiteral("映像點貨完成")),
               imageSummary(), QStringLiteral("success"));
        emit stateChanged();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, command](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_inspecting = false;
            StructuredLogger::instance().log(
                LogSeverity::Error, QStringLiteral("process.source_inspection"),
                QStringLiteral("process.failed_to_start"),
                QStringLiteral("Windows image-source inspection could not start."),
                QJsonObject{{QStringLiteral("argumentCount"), command.arguments.size()},
                            {QStringLiteral("processError"), static_cast<int>(error)},
                            {QStringLiteral("isoSource"), command.isoSource}});
            showError(localized(
                QStringLiteral("Could not start %1: %2").arg(command.program, process->errorString()),
                QStringLiteral("開唔到 %1：%2").arg(command.program, process->errorString())));
            process->deleteLater();
            emit stateChanged();
        }
    });
    configureProcessWithoutConsole(*process);
    process->start();
}

void AppController::importHostDrivers()
{
    if (!m_project) {
        showError(localized(QStringLiteral("Open a project first."),
                            QStringLiteral("請先開工程。")));
        return;
    }
    const QString destination = QDir(m_project->projectDirectory).filePath(QStringLiteral("payloads/host-drivers"));
    QDir().mkpath(destination);
    auto *process = new QProcess(this);
    process->setProgram(resolveExecutableForLaunch(QStringLiteral("dism.exe")));
    process->setArguments({QStringLiteral("/Online"), QStringLiteral("/Export-Driver"),
                           QStringLiteral("/Destination:%1").arg(destination)});
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_inspecting = true;
    m_statusText = localized(QStringLiteral("Exporting host drivers"),
                             QStringLiteral("匯出主機驅動程式中"));
    emit stateChanged();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("process.host_driver_export"),
        QStringLiteral("process.launch_requested"),
        QStringLiteral("Starting host-driver export."),
        QJsonObject{{QStringLiteral("argumentCount"), process->arguments().size()}});
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, destination](int code, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(process->readAll()); process->deleteLater(); m_inspecting = false;
        StructuredLogger::instance().log(
            status == QProcess::NormalExit && code == 0
                ? LogSeverity::Info : LogSeverity::Error,
            QStringLiteral("process.host_driver_export"),
            QStringLiteral("process.finished"),
            QStringLiteral("Host-driver export finished."),
            QJsonObject{{QStringLiteral("argumentCount"), process->arguments().size()},
                        {QStringLiteral("exitCode"), code},
                        {QStringLiteral("normalExit"), status == QProcess::NormalExit},
                        {QStringLiteral("outputBytes"), output.toUtf8().size()}});
        if (status == QProcess::NormalExit && code == 0) {
            addListItem(QStringLiteral("drivers"), destination);
            notify(localized(QStringLiteral("Host drivers exported"),
                             QStringLiteral("主機驅動程式已匯出")),
                   localized(QStringLiteral("Exported to %1").arg(destination),
                             QStringLiteral("已匯出到 %1").arg(destination)),
                   QStringLiteral("success"));
        } else showError(output.trimmed());
        emit stateChanged();
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, destination](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_inspecting = false;
        StructuredLogger::instance().log(
            LogSeverity::Error, QStringLiteral("process.host_driver_export"),
            QStringLiteral("process.failed_to_start"),
            QStringLiteral("Host-driver export could not start."),
            QJsonObject{{QStringLiteral("argumentCount"), process->arguments().size()},
                        {QStringLiteral("processError"), static_cast<int>(error)}});
        showError(localized(
            QStringLiteral("Could not start host-driver export: %1").arg(process->errorString()),
            QStringLiteral("開唔到主機驅動匯出：%1").arg(process->errorString())));
        process->deleteLater();
        emit stateChanged();
    });
    configureProcessWithoutConsole(*process);
    process->start();
}

void AppController::refreshPlan()
{
    m_plan.clear();
    if (!m_project) { emit stateChanged(); return; }
    ServicingPlanResult result = ServicingPlan::build(*m_project);
    m_plan = result.operations;
    const QJsonArray order = m_project->options.extra.value(QStringLiteral("planOrder")).toArray();
    if (!order.isEmpty()) {
        QList<ServicingOperation> reordered;
        QSet<QString> inserted;
        for (const QJsonValue &value : order) {
            const QString id = value.toString();
            const auto found = std::find_if(m_plan.cbegin(), m_plan.cend(),
                [&id](const ServicingOperation &operation) { return operation.id == id; });
            if (found != m_plan.cend()) {
                reordered.append(*found);
                inserted.insert(id);
            }
        }
        for (const ServicingOperation &operation : std::as_const(m_plan))
            if (!inserted.contains(operation.id)) reordered.append(operation);
        m_plan = reordered;
    }
    const QJsonArray skipped = m_project->options.extra.value(QStringLiteral("planSkipped")).toArray();
    QSet<QString> skippedIds;
    for (const QJsonValue &value : skipped)
        skippedIds.insert(value.toString());
    for (ServicingOperation &operation : m_plan)
        if (skippedIds.contains(operation.id)) operation.state = OperationState::Skipped;
    if (!result.errors.isEmpty()) m_statusText = result.errors.first();
    else m_statusText = QStringLiteral("Plan ready — %1 operations").arg(m_plan.size());
    emit stateChanged();
}

void AppController::requestRunPlan()
{
    if (!m_project || m_plan.isEmpty()) { showError(QStringLiteral("Build a valid plan first.")); return; }
    const int destructive = static_cast<int>(std::count_if(m_plan.cbegin(), m_plan.cend(), [](const ServicingOperation &item) { return item.destructive; }));
    emit runConfirmationRequested(localized(
        QStringLiteral("%1 operations will run with up to %2 concurrent jobs. Writes to the mounted image remain serialized.").arg(m_plan.size()).arg(m_maxParallelJobs),
        QStringLiteral("%1 項工序，最多 %2 個平行工作；寫入掛載映像仍然會逐個排隊。").arg(m_plan.size()).arg(m_maxParallelJobs)), destructive);
}

void AppController::runPlan()
{
    if (!m_project) return;
    QString error;
    if (!m_jobEngine.start(*m_project, m_plan, m_maxParallelJobs, &error)) { showError(error); return; }
    notify(QStringLiteral("Servicing started"), QStringLiteral("The crash journal is active. You can keep using WimForge while independent jobs run."), QStringLiteral("progress"));
}

void AppController::cancelJobs() { m_jobEngine.cancel(); }

void AppController::moveOperation(int index, int offset)
{
    const int destination = index + offset;
    if (index < 0 || index >= m_plan.size() || destination < 0 || destination >= m_plan.size() || busy()) return;
    m_plan.move(index, destination);
    QJsonArray order;
    for (const ServicingOperation &operation : std::as_const(m_plan))
        order.append(operation.id);
    mutateProject(bilingualCommitMessage(
        QStringLiteral("plan: reorder operation"),
        QStringLiteral("計劃：重新排列工序")), [order](ProjectConfig &project) {
        project.options.extra.insert(QStringLiteral("planOrder"), order);
    });
}

void AppController::skipOperation(int index)
{
    if (index < 0 || index >= m_plan.size() || busy()) return;
    const ServicingOperation &operation = m_plan.at(index);
    if (operation.state != OperationState::Skipped
        && operation.skipConsequence != SkipConsequence::OmitsOptionalChange) {
        showError(localized(QStringLiteral("That operation is a safety or image-structure dependency and cannot be skipped."),
                            QStringLiteral("嗰項係安全／映像結構依賴，唔可以略過。")));
        return;
    }
    const QString id = m_plan.at(index).id;
    const bool willSkip = m_plan.at(index).state != OperationState::Skipped;
    mutateProject(bilingualCommitMessage(
        QStringLiteral("plan: %1 %2")
            .arg(willSkip ? QStringLiteral("skip") : QStringLiteral("restore"), id),
        QStringLiteral("計劃：%1 %2")
            .arg(willSkip ? QStringLiteral("略過") : QStringLiteral("恢復"), id)),
        [id, willSkip](ProjectConfig &project) {
            QJsonArray existing = project.options.extra.value(QStringLiteral("planSkipped")).toArray();
            QJsonArray updated;
            for (const QJsonValue &value : existing)
                if (value.toString() != id) updated.append(value);
            if (willSkip) updated.append(id);
            project.options.extra.insert(QStringLiteral("planSkipped"), updated);
        });
}

void AppController::refreshHistory()
{
    m_history.clear();
    if (m_project) {
        QString error; m_history = m_project->history(500, &error);
        if (!error.isEmpty()) showError(error);
    }
    emit stateChanged();
}

void AppController::undoLatestProjectChange()
{
    if (!m_project) return;
    ActionHistory actionHistory(m_project->projectDirectory);
    QString actionError;
    const QList<ActionEvent> actions = actionHistory.events(500, &actionError);
    if (actionError.isEmpty()) {
        for (const ActionEvent &event : actions) {
            if (!event.isAction() && !event.isCompensation())
                continue;
            QString effectiveError;
            const bool effective = actionHistory.isEffective(event.id, &effectiveError);
            if (!effectiveError.isEmpty())
                continue;
            if (effective)
                undoHistoryEvent(event.id);
            else
                redoHistoryEvent(event.id);
            return;
        }
    }

    QString error;
    if (!m_project->revertLatest(&error)) { showError(error); return; }
    const QString directory = m_project->projectDirectory;
    const auto reloaded = ProjectConfig::load(directory, &error);
    if (!reloaded) { showError(error); return; }
    m_project = *reloaded;
    loadProjectState();
    showSuccess(localized(QStringLiteral("Latest project action reversed. Undo again to reverse this undo."),
                          QStringLiteral("最新工程動作已逆轉。再 Undo 一次就會逆轉今次 Undo。")));
}

QVariantList AppController::contextualHistory(const QString &contextKey,
                                              const QString &elementId) const
{
    QVariantList result;
    if (!m_project)
        return result;
    ActionHistory history(m_project->projectDirectory);
    QString error;
    QList<ActionEvent> events;
    if (contextKey.trimmed().isEmpty()) {
        events = history.events(80, &error);
    } else if (elementId.trimmed().isEmpty()) {
        const QList<ActionEvent> all = history.events(200, &error);
        for (const ActionEvent &event : all) {
            if (event.contextKey == contextKey.trimmed() && events.size() < 40)
                events.append(event);
        }
    } else {
        events = history.recentForElement(contextKey.trimmed(), elementId.trimmed(), 40, &error);
    }
    if (!error.isEmpty())
        return result;
    for (const ActionEvent &event : events) {
        bool effective = true;
        if (event.isAction() || event.isCompensation()) {
            QString effectiveError;
            effective = history.isEffective(event.id, &effectiveError);
            if (!effectiveError.isEmpty())
                effective = true;
        }
        result.append(event.toVariantMap(effective));
    }
    return result;
}

void AppController::undoContext(const QString &contextKey, const QString &elementId)
{
    if (!m_project)
        return;
    ActionHistory history(m_project->projectDirectory);
    QString error;
    QList<ActionEvent> events;
    if (contextKey.trimmed().isEmpty()) {
        events = history.events(500, &error);
    } else if (elementId.trimmed().isEmpty()) {
        const QList<ActionEvent> all = history.events(500, &error);
        for (const ActionEvent &event : all)
            if (event.contextKey == contextKey.trimmed()) events.append(event);
    } else {
        events = history.recentForElement(contextKey.trimmed(), elementId.trimmed(), 200, &error);
    }
    if (!error.isEmpty()) {
        showError(error);
        return;
    }
    for (const ActionEvent &event : events) {
        if (!event.isAction() && !event.isCompensation())
            continue;
        QString effectiveError;
        const bool effective = history.isEffective(event.id, &effectiveError);
        if (!effectiveError.isEmpty())
            continue;
        if (effective)
            undoHistoryEvent(event.id);
        else
            redoHistoryEvent(event.id);
        return;
    }
    if (!contextKey.trimmed().isEmpty()) {
        const QList<ActionEvent> all = history.events(500, &error);
        if (!error.isEmpty()) {
            showError(error);
            return;
        }
        for (const ActionEvent &event : all) {
            if (!event.isAction() && !event.isCompensation())
                continue;
            QString effectiveError;
            const bool effective = history.isEffective(event.id, &effectiveError);
            if (!effectiveError.isEmpty())
                continue;
            if (effective)
                undoHistoryEvent(event.id);
            else
                redoHistoryEvent(event.id);
            return;
        }
    }
    emit snackbarRequested(localized(QStringLiteral("Nothing undoable in this context yet."),
                                     QStringLiteral("呢個位置暫時冇嘢可以 Undo。")), QStringLiteral("info"));
}

bool AppController::applyHistoryState(const ActionEvent &event, const QString &message)
{
    if (!m_project)
        return false;
    QString error;
    const QString directory = m_project->projectDirectory;
    const QJsonObject current = m_project->toJson();
    QJsonObject patch = event.forwardDiff;
    QJsonObject expectedPatch = event.inverseDiff;
    const bool fullState = event.metadata.value(QStringLiteral("fullProjectState")).toBool()
        || (event.metadata.value(QStringLiteral("stateFormat")).toString().isEmpty()
            && event.forwardDiff.contains(QStringLiteral("schemaVersion"))
            && event.forwardDiff.contains(QStringLiteral("projectName")));
    if (fullState) {
        patch = ActionHistory::createMergePatch(event.inverseDiff, event.forwardDiff);
        expectedPatch = ActionHistory::createMergePatch(event.forwardDiff, event.inverseDiff);
    }
    const MergePatchApplyResult merged = ActionHistory::applyMergePatchGuarded(
        current, patch, expectedPatch);
    if (!merged.applied) {
        showError(QStringLiteral(
            "This action cannot be changed selectively because later work modified: %1. "
            "Undo the conflicting newer action first, or use an explicit restore point.")
                      .arg(merged.conflicts.join(QStringLiteral(", "))));
        return false;
    }
    const auto restored = ProjectConfig::fromJson(merged.state, directory, &error);
    if (!restored) {
        showError(QStringLiteral("The selected history state could not be restored: %1").arg(error));
        return false;
    }
    if (!restored->save(&error, message)) {
        showError(error);
        return false;
    }
    m_project = *restored;
    loadProjectState();
    return true;
}

bool AppController::undoHistoryEvent(const QString &eventId)
{
    if (!m_project)
        return false;
    ActionHistory history(m_project->projectDirectory);
    ActionEvent compensation;
    QString error;
    if (!history.undoAction(eventId, &compensation, &error)) {
        showError(error);
        return false;
    }
    if (!applyHistoryState(compensation, compensation.title)) {
        ActionEvent repair;
        QString repairError;
        history.redoAction(eventId, &repair, &repairError);
        return false;
    }
    showSuccess(localized(QStringLiteral("Action undone. The undo is itself recorded and undoable."),
                          QStringLiteral("動作已 Undo；今次 Undo 自己都有記錄，亦可以 Undo。")));
    return true;
}

bool AppController::redoHistoryEvent(const QString &eventId)
{
    if (!m_project)
        return false;
    ActionHistory history(m_project->projectDirectory);
    ActionEvent compensation;
    QString error;
    if (!history.redoAction(eventId, &compensation, &error)) {
        showError(error);
        return false;
    }
    if (!applyHistoryState(compensation, compensation.title)) {
        ActionEvent repair;
        QString repairError;
        history.undoAction(eventId, &repair, &repairError);
        return false;
    }
    showSuccess(localized(QStringLiteral("Action redone as a new history event."),
                          QStringLiteral("動作已用新歷史事件 Redo。")));
    return true;
}

bool AppController::restoreHistoryEvent(const QString &eventId)
{
    if (!m_project)
        return false;
    ActionHistory history(m_project->projectDirectory);
    QString error;
    const QList<ActionEvent> events = history.events(2'000, &error);
    const auto selected = std::find_if(events.cbegin(), events.cend(),
        [&eventId](const ActionEvent &event) { return event.id == eventId; });
    if (selected == events.cend() || !selected->isAction()) {
        showError(error.isEmpty() ? QStringLiteral("The selected restore point no longer exists.") : error);
        return false;
    }
    const QJsonObject before = m_project->toJson();
    const QString directory = m_project->projectDirectory;
    QJsonObject restoredState = selected->metadata.value(QStringLiteral("afterState")).toObject();
    if (restoredState.isEmpty())
        restoredState = selected->forwardDiff;
    const auto restored = ProjectConfig::fromJson(restoredState, directory, &error);
    if (!restored || !restored->save(&error, bilingualCommitMessage(
            QStringLiteral("history: restore event #%1").arg(selected->sequence),
            QStringLiteral("歷史：還原事件 #%1").arg(selected->sequence)))) {
        showError(error);
        return false;
    }
    m_project = *restored;
    ActionDraft draft;
    draft.title = bilingualCommitMessage(
        QStringLiteral("Restore history event #%1").arg(selected->sequence),
        QStringLiteral("還原歷程事件 #%1").arg(selected->sequence));
    draft.description = selected->title;
    draft.icon = QStringLiteral("restore");
    draft.contextKey = selected->contextKey;
    draft.elementId = selected->elementId;
    const QJsonObject after = restored->toJson();
    draft.forwardDiff = ActionHistory::createMergePatch(before, after);
    draft.inverseDiff = ActionHistory::createMergePatch(after, before);
    draft.metadata = QJsonObject{{QStringLiteral("diffSummary"), draft.title},
                                 {QStringLiteral("restoredEventId"), selected->id},
                                 {QStringLiteral("stateFormat"), QStringLiteral("merge-patch")},
                                 {QStringLiteral("beforeState"), before},
                                 {QStringLiteral("afterState"), after}};
    if (!history.record(draft, nullptr, &error)) {
        showError(error);
        return false;
    }
    loadProjectState();
    showSuccess(localized(QStringLiteral("Restored that point as a new reversible action."),
                          QStringLiteral("已將嗰個時間點還原成新嘅可逆動作。")));
    return true;
}

bool AppController::bookmarkHistoryEvent(const QString &eventId, const QString &name)
{
    if (!m_project)
        return false;
    QString error;
    if (!ActionHistory(m_project->projectDirectory).createBookmark(name, eventId, nullptr, &error)) {
        showError(error);
        return false;
    }
    refreshHistory();
    return true;
}

bool AppController::branchHistoryEvent(const QString &eventId, const QString &name)
{
    if (!m_project)
        return false;
    QString error;
    if (!ActionHistory(m_project->projectDirectory).createBranch(name, eventId, nullptr, &error)) {
        showError(error);
        return false;
    }
    refreshHistory();
    return true;
}

bool AppController::switchHistoryBranch(const QString &name)
{
    if (!m_project)
        return false;
    QString error;
    if (!ActionHistory(m_project->projectDirectory).switchBranch(name, nullptr, &error)) {
        showError(error);
        return false;
    }
    refreshHistory();
    showSuccess(localized(QStringLiteral("History lane switched to %1.").arg(name),
                          QStringLiteral("歷史分支已轉去 %1。 ").arg(name)));
    return true;
}

void AppController::undoLatestNotificationChange()
{
    QString error;
    if (!m_notificationStore.revertLatest(&error)) { showError(error); return; }
    refreshNotifications();
    emit snackbarRequested(localized(QStringLiteral("Notification action reversed; repeat to redo it."),
                                     QStringLiteral("通知動作已逆轉；再做一次就 redo。")), QStringLiteral("success"));
}

void AppController::markNotificationRead(const QString &id) { QString e; if (!m_notificationStore.markRead(id, &e)) showError(e); refreshNotifications(); }
void AppController::markNotificationUnread(const QString &id) { QString e; if (!m_notificationStore.markUnread(id, &e)) showError(e); refreshNotifications(); }
void AppController::dismissNotification(const QString &id) { QString e; if (!m_notificationStore.dismiss(id, &e)) showError(e); refreshNotifications(); }
void AppController::deleteNotification(const QString &id) { QString e; if (!m_notificationStore.softDelete(id, &e)) showError(e); refreshNotifications(); }
void AppController::restoreNotification(const QString &id) { QString e; if (!m_notificationStore.restore(id, &e)) showError(e); refreshNotifications(); }

void AppController::sendTestNotification()
{
    notify(QStringLiteral("Test notification"), localized(
        QStringLiteral("This notification and every action you take on it are now committed to the notification repository."),
        QStringLiteral("呢個通知連你之後點樣搞佢，都已經 commit 去通知倉。走唔甩啦。")), QStringLiteral("info"));
}

void AppController::resumeRecovery()
{
    refreshPlan();
    emit recoveryReviewRequested();
    showSuccess(localized(
        QStringLiteral("The plan was rebuilt for review. Completed external steps are not guessed or skipped after a crash."),
        QStringLiteral("計劃已重新建立畀你檢查。死機之後唔會估外部工序做完未，更加唔會亂跳步。")));
}

void AppController::rollbackRecovery()
{
    undoLatestProjectChange();
    notify(QStringLiteral("Latest configuration change undone"), QStringLiteral("Only project configuration was reversed. External servicing side effects were not claimed as rolled back; the pristine source remains the clean baseline."), QStringLiteral("warning"));
}

void AppController::safeUnmountRecovery()
{
    if (!m_project || !m_pendingRecovery) {
        showError(QStringLiteral("There is no interrupted servicing run to unmount."));
        return;
    }
    if (m_recoveryUnmountBusy) {
        emit snackbarRequested(QStringLiteral("Recovery unmount is already running."),
                               QStringLiteral("info"));
        return;
    }
    if (m_jobEngine.isRunning()) {
        showError(QStringLiteral("Wait for the active servicing run before recovering an older mount."));
        return;
    }

    // Recovery must use the immutable path captured by the interrupted run. The
    // user may have edited the current project mount path since the crash.
    const QString mountPath = cleanPath(m_recoveryJournal.value(QStringLiteral("mountPath")).toString());
    const QString runId = m_recoveryJournal.value(QStringLiteral("runId")).toString().trimmed();
    if (mountPath.isEmpty() || !QDir::isAbsolutePath(mountPath) || runId.isEmpty()) {
        showError(QStringLiteral("The recovery journal has no valid absolute mount path or run ID."));
        return;
    }
    if (!JobEngine::isAdministrator()) {
        showError(QStringLiteral("Safe unmount needs an elevated WimForge session because DISM requires administrator rights."));
        return;
    }

    const QString journalPath = JobEngine::journalPathForProject(m_project->projectDirectory);
    auto *process = new QProcess(this);
    process->setProgram(resolveExecutableForLaunch(QStringLiteral("dism.exe")));
    process->setArguments({QStringLiteral("/English"), QStringLiteral("/Unmount-Image"),
                           QStringLiteral("/MountDir:%1").arg(mountPath), QStringLiteral("/Discard")});
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_recoveryUnmountBusy = true;
    notify(QStringLiteral("Recovery unmount started"),
           QStringLiteral("DISM is discarding the interrupted mount recorded by run %1. The original journal remains intact until DISM succeeds.").arg(runId),
           QStringLiteral("progress"));

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart || process->property("recoveryHandled").toBool())
            return;
        process->setProperty("recoveryHandled", true);
        m_recoveryUnmountBusy = false;
        showError(QStringLiteral("DISM could not start: %1. The recovery journal was not changed.")
                      .arg(process->errorString()));
        process->deleteLater();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, journalPath, runId, mountPath](int exitCode, QProcess::ExitStatus exitStatus) {
        if (process->property("recoveryHandled").toBool())
            return;
        process->setProperty("recoveryHandled", true);
        const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
        process->deleteLater();
        m_recoveryUnmountBusy = false;

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            showError(QStringLiteral("DISM did not discard the interrupted mount (exit %1). The recovery journal was preserved.%2")
                          .arg(exitCode)
                          .arg(output.isEmpty() ? QString() : QStringLiteral("\n\n%1").arg(output.right(3000))));
            return;
        }

        QFile journalFile(journalPath);
        if (!journalFile.open(QIODevice::ReadOnly)) {
            showError(QStringLiteral("The mount was discarded, but the original recovery journal could not be reopened: %1")
                          .arg(journalFile.errorString()));
            refreshRecoveryState();
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(journalFile.readAll(), &parseError);
        journalFile.close();
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            showError(QStringLiteral("The mount was discarded, but the recovery journal is no longer valid. It was left untouched."));
            refreshRecoveryState();
            return;
        }
        QJsonObject journal = document.object();
        if (journal.value(QStringLiteral("runId")).toString() != runId) {
            showError(QStringLiteral("The mount was discarded, but a newer recovery run replaced the journal. The newer journal was left untouched."));
            refreshRecoveryState();
            return;
        }
        const QString recoveredAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        journal.insert(QStringLiteral("status"), QStringLiteral("recovered-discarded"));
        journal.insert(QStringLiteral("recoveredAt"), recoveredAt);
        journal.insert(QStringLiteral("updatedAt"), recoveredAt);
        journal.insert(QStringLiteral("recoveryAction"), QStringLiteral("dism-unmount-discard"));
        journal.insert(QStringLiteral("recoveredMountPath"), mountPath);
        QString writeError;
        if (!writeBytes(journalPath, QJsonDocument(journal).toJson(QJsonDocument::Indented), &writeError)) {
            showError(QStringLiteral("The mount was discarded, but its journal status could not be saved: %1")
                          .arg(writeError));
            refreshRecoveryState();
            return;
        }
        refreshRecoveryState();
        emit stateChanged();
        notify(QStringLiteral("Interrupted mount discarded"),
               QStringLiteral("DISM succeeded and recovery run %1 is now closed without claiming its external steps were rolled back.").arg(runId),
               QStringLiteral("success"));
    });
    configureProcessWithoutConsole(*process);
    process->start();
}

void AppController::openUnattendGenerator()
{
    emit unattendedStudioRequested();
    emit snackbarRequested(localized(QStringLiteral("Unattended Studio is opening inside WimForge."),
                                     QStringLiteral("無人值守工房喺 WimForge 入面開緊。")), QStringLiteral("info"));
}

void AppController::search(const QString &query)
{
    m_searchQuery = query.trimmed();
    m_searchResults.clear();
    if (m_searchQuery.isEmpty()) {
        emit searchChanged();
        return;
    }

    SearchIndex index;
    auto add = [&index](const QString &id, const QString &kind,
                        const QString &titleEn, const QString &titleZh,
                        const QString &subtitleEn, const QString &subtitleZh,
                        const QStringList &keywords, int page,
                        const QString &action = {}, const QJsonObject &payload = {}) {
        index.add(SearchEntry{id, kind, titleEn, titleZh, subtitleEn, subtitleZh,
                              keywords, page, action, payload});
    };

    struct StaticEntry {
        const char *id;
        const char *titleEn;
        const char *titleZh;
        const char *keywords;
        int page;
    };
    static constexpr StaticEntry pages[]{
        {"overview", "Overview", "總覽", "dashboard status safety project", 0},
        {"source", "Source & editions", "來源同版本", "iso wim esd editions inspect drivers", 1},
        {"customize", "Customize", "調校", "updates drivers features apps components settings payload", 2},
        {"gpo", "Group Policy Studio", "群組原則工房", "gpo admx adml registry policy", 3},
        {"unattended", "Unattended Studio", "無人值守工房", "answer file autounattend oobe setup", 4},
        {"packages", "Package Studio", "套件工房", "software winget choco scoop npm pip profile", 5},
        {"winforge", "WinForge Bridge", "WinForge 橋接", "recipe runtime post setup automation", 6},
        {"vm-lab", "Virtual Machine Lab", "虛擬機實驗室", "vmware virtualbox vm iso test snapshots validation", 7},
        {"plan", "Review & run", "檢查同開工", "servicing operations command dependencies execute", 8},
        {"history", "History & recovery", "歷史同復原", "git undo redo restore branch notifications", 9},
        {"settings", "Settings", "設定", "theme language density motion workers safety", 10},
        {"terminal", "Embedded terminal", "內嵌終端機", "conpty windows terminal powershell cmd console shell", 11},
    };
    for (const StaticEntry &page : pages) {
        add(QStringLiteral("page:%1").arg(QString::fromLatin1(page.id)),
            QStringLiteral("page"), QString::fromUtf8(page.titleEn),
            QString::fromUtf8(page.titleZh),
            QStringLiteral("Open this workspace"), QStringLiteral("開啟呢個工作區"),
            QString::fromLatin1(page.keywords).split(QLatin1Char(' ')), page.page);
    }

    static constexpr StaticEntry settings[]{
        {"language", "Language mode", "語言模式", "english cantonese bilingual zh hk", 10},
        {"theme", "Color theme", "顏色主題", "light dark system appearance", 10},
        {"density", "Interface density", "介面密度", "scale spacing compact comfortable", 10},
        {"motion", "Motion and transitions", "動畫同轉場", "animation accessibility reduced motion", 10},
        {"parallel", "Parallel servicing jobs", "平行維護工序", "workers concurrency job engine", 10},
        {"threads", "CPU thread ceiling", "CPU 執行緒上限", "processor cpu worker limit", 10},
        {"scratch", "Scratch-space reserve", "暫存空間預留", "disk free space reserve", 10},
        {"journal", "Crash recovery journal", "崩潰復原日誌", "recovery safety journal", 10},
        {"hash", "Verify source hashes", "驗證來源雜湊", "sha256 integrity source", 10},
        {"checkpoint", "Destructive checkpoints", "破壞性工序檢查點", "backup recovery safety", 10},
    };
    for (const StaticEntry &setting : settings) {
        add(QStringLiteral("setting:%1").arg(QString::fromLatin1(setting.id)),
            QStringLiteral("setting"), QString::fromUtf8(setting.titleEn),
            QString::fromUtf8(setting.titleZh),
            QStringLiteral("Application preference"), QStringLiteral("應用程式偏好設定"),
            QString::fromLatin1(setting.keywords).split(QLatin1Char(' ')), setting.page);
    }

    static constexpr StaticEntry features[]{
        {"NetFx3", ".NET Framework 3.5", ".NET Framework 3.5", "dotnet legacy optional feature", 2},
        {"Microsoft-Windows-Subsystem-Linux", "Windows Subsystem for Linux", "Windows Linux 子系統", "wsl linux optional feature", 2},
        {"VirtualMachinePlatform", "Virtual Machine Platform", "虛擬機平台", "virtualization wsl vm optional feature", 2},
        {"Microsoft-Hyper-V-All", "Hyper-V", "Hyper-V", "virtualization hypervisor vm feature", 2},
        {"Containers", "Windows Containers", "Windows 容器", "docker container optional feature", 2},
        {"TelnetClient", "Telnet client", "Telnet 用戶端", "network legacy optional feature", 2},
        {"SMB1Protocol", "SMB 1.0 (legacy and risky)", "SMB 1.0（舊式兼高危）", "file sharing legacy risky feature", 2},
        {"Printing-PrintToPDFServices-Features", "Microsoft Print to PDF", "Microsoft 列印到 PDF", "printer pdf optional feature", 2},
    };
    for (const StaticEntry &feature : features) {
        const QString id = QString::fromLatin1(feature.id);
        const bool enabled = m_project && m_project->featuresToEnable.contains(id);
        add(QStringLiteral("feature:%1").arg(id), QStringLiteral("feature"),
            QString::fromUtf8(feature.titleEn), QString::fromUtf8(feature.titleZh),
            enabled ? QStringLiteral("Enabled in the current project")
                    : QStringLiteral("Available Windows feature"),
            enabled ? QStringLiteral("而家工程已啟用") : QStringLiteral("可用 Windows 功能"),
            QString::fromLatin1(feature.keywords).split(QLatin1Char(' ')), feature.page,
            {}, QJsonObject{{QStringLiteral("featureId"), id}});
    }

    struct CommandEntry {
        const char *id;
        const char *title;
        const char *titleZh;
        const char *keywords;
        int page;
    };
    static constexpr CommandEntry commands[]{
        {"new-project", "Create a new project", "建立新工程", "new create project", 0},
        {"open-project", "Open an existing project", "開啟現有工程", "open load project", 0},
        {"inspect-source", "Inspect source editions", "檢查來源版本", "scan iso wim esd source", 1},
        {"import-host-drivers", "Import host drivers", "匯入本機驅動程式", "export pnputil drivers", 2},
        {"refresh-plan", "Rebuild the servicing plan", "重建維護計劃", "refresh review regenerate operations", 8},
        {"export-script", "Export reviewed PowerShell", "匯出已檢查 PowerShell", "script powershell plan", 8},
        {"run-plan", "Run the reviewed plan", "執行已檢查計劃", "apply execute servicing", 8},
        {"package-ai", "Load AI development package template", "載入 AI 開發套件範本", "software profile opencode node python", 5},
        {"unattend-ai", "Load AI development unattended template", "載入 AI 開發無人值守範本", "answer file oobe setup", 4},
        {"test-notification", "Send a test notification", "傳送測試通知", "notification center alert", 9},
    };
    for (const CommandEntry &command : commands) {
        add(QStringLiteral("command:%1").arg(QString::fromLatin1(command.id)),
            QStringLiteral("command"), QString::fromUtf8(command.title),
            QString::fromUtf8(command.titleZh),
            QStringLiteral("Run command"), QStringLiteral("執行指令"),
            QString::fromLatin1(command.keywords).split(QLatin1Char(' ')), command.page,
            QString::fromLatin1(command.id));
    }

    for (const PackageEntry &package : m_packageProfile.packages) {
        add(QStringLiteral("package:%1").arg(package.id), QStringLiteral("package"),
            package.displayName, package.displayName, package.description, package.description,
            {package.id, package.packageIdentifier, PackageStudio::providerName(package.provider),
             package.version, package.license}, 5, {},
            QJsonObject{{QStringLiteral("packageId"), package.id}});
    }

    auto addProjectValue = [&add](const QString &id, const QString &title,
                                  const QString &value, int page) {
        if (!value.trimmed().isEmpty()) {
            add(QStringLiteral("project:%1").arg(id), QStringLiteral("project"), title, title,
                value, value, {value}, page);
        }
    };
    if (m_project) {
        addProjectValue(QStringLiteral("name"), QStringLiteral("Current project"),
                        m_project->projectName, 0);
        addProjectValue(QStringLiteral("source"), QStringLiteral("Source path"),
                        m_project->sourcePath, 1);
        addProjectValue(QStringLiteral("image"), QStringLiteral("Working image"),
                        m_project->imagePath, 1);
        addProjectValue(QStringLiteral("output"), QStringLiteral("Output path"),
                        m_project->outputPath, 1);
        auto addList = [&add](const QString &prefix, const QString &kind, const QStringList &values,
                              int page) {
            for (qsizetype item = 0; item < values.size(); ++item) {
                add(QStringLiteral("project:%1:%2").arg(prefix).arg(item),
                    QStringLiteral("project"), values.at(item), values.at(item), kind, kind,
                    {prefix, kind, values.at(item)}, page);
            }
        };
        addList(QStringLiteral("driver"), QStringLiteral("Project driver"), m_project->drivers, 2);
        addList(QStringLiteral("update"), QStringLiteral("Project update"), m_project->updates, 2);
        addList(QStringLiteral("package"), QStringLiteral("Project package"), m_project->packages, 2);
        addList(QStringLiteral("feature"), QStringLiteral("Enabled project feature"),
                m_project->featuresToEnable, 2);
        addList(QStringLiteral("capability"), QStringLiteral("Added project capability"),
                m_project->capabilitiesToAdd, 2);
        addList(QStringLiteral("appx"), QStringLiteral("Project Appx change"),
                m_project->appxPackagesToRemove + m_project->appxPackagesToProvision, 2);
    }

    if (m_searchQuery.size() >= 2) {
        if (!m_gpoLoaded)
            loadGpoCatalog();
        if (m_gpoLoaded) {
            QString gpoError;
            const QList<GpoPolicy> policies = m_gpoCatalog.search(
                m_searchQuery, GpoSearchMode::PlainText, &gpoError);
            const int count = qMin(80, policies.size());
            for (int item = 0; item < count; ++item) {
                const GpoPolicy &policy = policies.at(item);
                const QString name = policy.displayName.isEmpty() ? policy.id : policy.displayName;
                add(QStringLiteral("gpo:%1").arg(policy.qualifiedId()),
                    QStringLiteral("gpo"), name, name,
                    policy.categoryHierarchy.join(QStringLiteral(" › ")),
                    policy.categoryHierarchy.join(QStringLiteral(" › ")),
                    {policy.id, policy.explainText, policy.registryKey,
                     policy.registryValueName, gpoPolicyClassName(policy.policyClass)}, 3, {},
                    QJsonObject{{QStringLiteral("policyId"), policy.qualifiedId()},
                                {QStringLiteral("query"), name}});
            }
        }
    }

    auto kindLabel = [this](const QString &kind) {
        if (kind == QStringLiteral("page")) return localized(QStringLiteral("Page"), QStringLiteral("頁面"));
        if (kind == QStringLiteral("command")) return localized(QStringLiteral("Command"), QStringLiteral("指令"));
        if (kind == QStringLiteral("setting")) return localized(QStringLiteral("Setting"), QStringLiteral("設定"));
        if (kind == QStringLiteral("feature")) return localized(QStringLiteral("Feature"), QStringLiteral("功能"));
        if (kind == QStringLiteral("package")) return localized(QStringLiteral("Package"), QStringLiteral("套件"));
        if (kind == QStringLiteral("gpo")) return localized(QStringLiteral("Policy"), QStringLiteral("政策"));
        return localized(QStringLiteral("Project"), QStringLiteral("工程"));
    };
    auto kindIcon = [](const QString &kind) {
        if (kind == QStringLiteral("page")) return QStringLiteral("▣");
        if (kind == QStringLiteral("command")) return QStringLiteral("▶");
        if (kind == QStringLiteral("setting")) return QStringLiteral("⚙");
        if (kind == QStringLiteral("feature")) return QStringLiteral("◆");
        if (kind == QStringLiteral("package")) return QStringLiteral("▦");
        if (kind == QStringLiteral("gpo")) return QStringLiteral("▤");
        return QStringLiteral("◫");
    };
    for (const SearchMatch &match : index.search(m_searchQuery, 80)) {
        m_searchResults.append(QVariantMap{
            {QStringLiteral("id"), match.entry.id},
            {QStringLiteral("kind"), match.entry.kind},
            {QStringLiteral("kindLabel"), kindLabel(match.entry.kind)},
            {QStringLiteral("icon"), kindIcon(match.entry.kind)},
            {QStringLiteral("title"), localized(match.entry.titleEn, match.entry.titleZh)},
            {QStringLiteral("subtitle"), localized(match.entry.subtitleEn, match.entry.subtitleZh)},
            {QStringLiteral("page"), match.entry.page},
            {QStringLiteral("action"), match.entry.action},
            {QStringLiteral("payload"), match.entry.payload.toVariantMap()},
            {QStringLiteral("score"), match.score},
        });
    }
    emit searchChanged();
    emit searchRequested(m_searchQuery);
}

void AppController::clearSearch()
{
    if (m_searchQuery.isEmpty() && m_searchResults.isEmpty())
        return;
    m_searchQuery.clear();
    m_searchResults.clear();
    emit searchChanged();
}

void AppController::activateSearchResult(const QVariantMap &result)
{
    const QString action = result.value(QStringLiteral("action")).toString();
    const int page = result.value(QStringLiteral("page"), -1).toInt();
    const QString kind = result.value(QStringLiteral("kind")).toString();
    const QVariantMap payload = result.value(QStringLiteral("payload")).toMap();
    const QString title = result.value(QStringLiteral("title")).toString();

    if (action == QStringLiteral("new-project")) requestNewProject();
    else if (action == QStringLiteral("open-project")) requestOpenProject();
    else if (action == QStringLiteral("inspect-source")) inspectSource();
    else if (action == QStringLiteral("import-host-drivers")) importHostDrivers();
    else if (action == QStringLiteral("refresh-plan")) refreshPlan();
    else if (action == QStringLiteral("export-script")) requestExportScript();
    else if (action == QStringLiteral("run-plan")) requestRunPlan();
    else if (action == QStringLiteral("package-ai")) loadAiDevelopmentPackageTemplate();
    else if (action == QStringLiteral("unattend-ai")) loadUnattendedTemplate(QStringLiteral("ai-development"));
    else if (action == QStringLiteral("test-notification")) sendTestNotification();

    QString focusId;
    QString navigationQuery;
    if (kind == QStringLiteral("gpo")) {
        focusId = payload.value(QStringLiteral("policyId")).toString();
        navigationQuery = payload.value(QStringLiteral("query")).toString();
        searchGpo(navigationQuery, false);
    } else if (kind == QStringLiteral("feature")) {
        focusId = payload.value(QStringLiteral("featureId")).toString();
    } else if (kind == QStringLiteral("package")) {
        focusId = payload.value(QStringLiteral("packageId")).toString();
    } else {
        focusId = result.value(QStringLiteral("id")).toString();
    }
    if (page >= 0)
        emit searchNavigationRequested(page, focusId, navigationQuery);
    if (!title.isEmpty()) {
        emit snackbarRequested(localized(QStringLiteral("Opened %1").arg(title),
                                         QStringLiteral("已開啟 %1").arg(title)),
                                 QStringLiteral("info"));
    }
    clearSearch();
}

void AppController::copyText(const QString &text)
{
    if (QGuiApplication::clipboard()) QGuiApplication::clipboard()->setText(text);
    emit snackbarRequested(localized(QStringLiteral("Copied to clipboard."), QStringLiteral("已複製去剪貼簿。")), QStringLiteral("success"));
}

void AppController::restoreStudioState()
{
    bool packageLoaded = false;
    bool unattendLoaded = false;
    bool bridgeLoaded = false;
    if (m_project) {
        const QJsonValue packageValue = m_project->settings.value(QStringLiteral("_packageProfile"));
        if (packageValue.isObject()) {
            QString error;
            const auto profile = PackageStudio::fromJson(packageValue.toObject(), &error);
            if (profile) {
                m_packageProfile = *profile;
                packageLoaded = true;
            }
        }
        const QJsonValue unattendedValue = m_project->settings.value(QStringLiteral("_unattendProfile"));
        if (unattendedValue.isObject()) {
            QString error;
            const auto profile = UnattendProfile::fromJson(unattendedValue.toObject(), &error);
            if (profile) {
                m_unattendProfile = *profile;
                unattendLoaded = true;
            }
        }
        const QJsonValue bridgeValue = m_project->settings.value(QStringLiteral("_winForgeRecipe"));
        if (bridgeValue.isObject()) {
            QString error;
            const auto recipe = WinForgeBridge::fromJson(bridgeValue.toObject(), &error);
            if (recipe) {
                m_winForgeRecipe = *recipe;
                bridgeLoaded = true;
            } else {
                m_winForgeBridgeStatus = QStringLiteral("Saved WinForge recipe is invalid: %1").arg(error);
            }
        }
        const QJsonValue includeRuntime = m_project->settings.value(
            QStringLiteral("_winForgeIncludeRuntime"));
        if (includeRuntime.isBool())
            m_winForgeIncludeRuntime = includeRuntime.toBool();
        const QJsonValue runtimePath = m_project->settings.value(
            QStringLiteral("_winForgeRuntimePath"));
        if (runtimePath.isString())
            m_winForgeRuntimePath = cleanPath(runtimePath.toString());
    }
    if (!packageLoaded) {
        m_packageProfile = PackageStudio::fullAiDevelopmentTemplate();
        m_packageProfile.name = QStringLiteral("Custom software profile");
        m_packageProfile.description = QStringLiteral("Choose exactly what the finished Windows image installs.");
        for (PackageEntry &package : m_packageProfile.packages)
            package.enabled = false;
    }
    if (!unattendLoaded) {
        m_unattendProfile = UnattendProfile{};
        m_unattendProfile.name = QStringLiteral("Custom unattended setup");
        m_unattendProfile.description = QStringLiteral("All Windows setup passes remain available through the generic setting editor.");
    }
    if (!bridgeLoaded)
        m_winForgeRecipe = emptyWinForgeRecipe();
}

void AppController::loadGpoCatalog()
{
    if (m_gpoLoaded) {
        emit studioChanged();
        return;
    }
    m_gpoStatus = localized(QStringLiteral("Loading installed ADMX and ADML policy definitions…"),
                            QStringLiteral("載入已安裝 ADMX 同 ADML 政策定義…"));
    emit studioChanged();
    QString error;
    if (!m_gpoCatalog.loadInstalled({QStringLiteral("en-US"), QStringLiteral("zh-HK")}, &error)) {
        m_gpoStatus = error;
        showError(error);
        emit studioChanged();
        return;
    }
    m_gpoLoaded = true;
    const int initialCount = qMin(120, m_gpoCatalog.policies().size());
    m_gpoSearchResults = m_gpoCatalog.policies().mid(0, initialCount);
    m_gpoStatus = localized(
        QStringLiteral("%1 policies loaded from %2. Showing %3; search to narrow them.")
            .arg(m_gpoCatalog.policies().size()).arg(m_gpoCatalog.policyDefinitionsPath()).arg(initialCount),
        QStringLiteral("由 %2 載入 %1 項政策。暫時顯示 %3 項；搜尋就可以收窄。")
            .arg(m_gpoCatalog.policies().size()).arg(m_gpoCatalog.policyDefinitionsPath()).arg(initialCount));
    if (!m_gpoCatalog.warnings().isEmpty())
        notify(QStringLiteral("GPO locale notice"), m_gpoCatalog.warnings().join(QLatin1Char('\n')), QStringLiteral("warning"));
    emit studioChanged();
}

void AppController::searchGpo(const QString &query, bool regularExpression)
{
    if (!m_gpoLoaded)
        loadGpoCatalog();
    if (!m_gpoLoaded)
        return;
    QString error;
    QList<GpoPolicy> results = m_gpoCatalog.search(
        query.trimmed(), regularExpression ? GpoSearchMode::RegularExpression
                                           : GpoSearchMode::PlainText, &error);
    if (!error.isEmpty()) {
        m_gpoStatus = error;
        emit studioChanged();
        return;
    }
    constexpr int resultLimit = 300;
    m_gpoSearchResults = results.mid(0, resultLimit);
    m_gpoStatus = localized(
        QStringLiteral("%1 match(es)%2.").arg(results.size())
            .arg(results.size() > resultLimit ? QStringLiteral("; showing the first %1").arg(resultLimit) : QString()),
        QStringLiteral("搵到 %1 項%2。 ").arg(results.size())
            .arg(results.size() > resultLimit ? QStringLiteral("；顯示頭 %1 項").arg(resultLimit) : QString()));
    emit studioChanged();
}

bool AppController::applyGpoPolicy(const QString &qualifiedId,
                                   const QString &state,
                                   const QVariantMap &elementValues)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before applying policy configuration."));
        return false;
    }
    const auto found = std::find_if(m_gpoCatalog.policies().cbegin(), m_gpoCatalog.policies().cend(),
        [&qualifiedId](const GpoPolicy &policy) { return policy.qualifiedId() == qualifiedId; });
    if (found == m_gpoCatalog.policies().cend()) {
        showError(QStringLiteral("The selected GPO is no longer present in the loaded catalog."));
        return false;
    }
    const GpoPolicy policy = *found;
    const GpoPolicyCompilation compilation = compileGpoPolicy(
        policy, state, elementValues, m_project->registryTweaks);
    if (!compilation.ok()) {
        showError(compilation.error);
        return false;
    }
    const bool saved = mutateProject(
        bilingualCommitMessage(
            QStringLiteral("gpo: %1 %2").arg(state, policy.displayName),
            QStringLiteral("群組原則：%1 %2").arg(state, policy.displayName)),
        [compilation](ProjectConfig &project) {
            mergeGpoPolicyCompilation(project, compilation);
        });
    if (saved)
        showSuccess(localized(QStringLiteral("Policy action committed to project history."),
                              QStringLiteral("政策動作已 commit 入工程歷史。")));
    return saved;
}

bool AppController::exportGpoDocumentation(const QString &destinationFile)
{
    if (!m_gpoLoaded)
        loadGpoCatalog();
    if (!m_gpoLoaded)
        return false;
    QString error;
    if (!m_gpoCatalog.exportMarkdown(cleanPath(destinationFile), QStringLiteral("en-US"),
                                     QStringLiteral("zh-HK"), &error)) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Complete GPO documentation exported."),
                          QStringLiteral("完整 GPO 文件已匯出。")));
    return true;
}

void AppController::askOpenCodeForGpo(const QString &intent)
{
    const QString request = intent.trimmed();
    if (request.isEmpty())
        return;
    runOpenCode(QStringLiteral(
        "You are helping search a local Windows ADMX policy catalog. Convert the user's intent "
        "into a short plain-text AND-token search query. Return only the search query, no prose. Intent: %1")
        .arg(request), [this](const QString &answer) {
            QString query = answer.section(QLatin1Char('\n'), -1).trimmed();
            query.remove(QLatin1Char('`'));
            searchGpo(query, false);
            emit snackbarRequested(localized(QStringLiteral("OpenCode suggested: %1").arg(query),
                                             QStringLiteral("OpenCode 建議搜尋：%1").arg(query)),
                                     QStringLiteral("success"));
        });
}

bool AppController::persistPackageProfile(const QString &message)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return false;
    }
    const QString profilePath = QDir(m_project->projectDirectory)
        .filePath(QStringLiteral(".wimforge/generated/packages/profile.json"));
    QString error;
    if (!writeBytes(profilePath,
                    QJsonDocument(PackageStudio::toJson(m_packageProfile)).toJson(QJsonDocument::Indented),
                    &error)) {
        showError(error);
        return false;
    }
    const QJsonObject profileJson = PackageStudio::toJson(m_packageProfile);
    return mutateProject(message, [profileJson](ProjectConfig &project) {
        project.settings.insert(QStringLiteral("_packageProfile"), profileJson);
    });
}

void AppController::loadAiDevelopmentPackageTemplate()
{
    m_packageProfile = PackageStudio::fullAiDevelopmentTemplate();
    if (persistPackageProfile(bilingualCommitMessage(
            QStringLiteral("packages: select Full AI Development ISO template"),
            QStringLiteral("套件：選擇完整 AI 開發 ISO 範本")))) {
        notify(QStringLiteral("AI development template selected"),
               QStringLiteral("Codex, Claude Code, Claude Desktop, OpenCode, Node/npm, Python, CMake, Java, Git, VS Build Tools, VS Code and Docker are in the reproducible profile. Desktop payloads without an official package ID stay visibly optional."),
               QStringLiteral("success"));
    }
    emit studioChanged();
}

void AppController::setPackageEnabled(const QString &id, bool enabled)
{
    bool found = false;
    for (PackageEntry &package : m_packageProfile.packages) {
        if (package.id == id) {
            package.enabled = enabled;
            found = true;
            break;
        }
    }
    if (!found)
        return;
    if (enabled) {
        QSet<QString> wanted{id};
        bool changed = true;
        while (changed) {
            changed = false;
            for (PackageEntry &package : m_packageProfile.packages) {
                if (!wanted.contains(package.id))
                    continue;
                for (const QString &dependency : package.dependencies) {
                    if (!wanted.contains(dependency)) {
                        wanted.insert(dependency);
                        changed = true;
                    }
                }
            }
        }
        for (PackageEntry &package : m_packageProfile.packages)
            if (wanted.contains(package.id)) package.enabled = true;
    }
    persistPackageProfile(bilingualCommitMessage(
        QStringLiteral("packages: %1 %2")
            .arg(enabled ? QStringLiteral("select") : QStringLiteral("remove"), id),
        QStringLiteral("套件：%1 %2")
            .arg(enabled ? QStringLiteral("選擇") : QStringLiteral("移除"), id)));
    emit studioChanged();
}

bool AppController::importPackageProfile(const QString &sourceFile)
{
    QString error;
    const auto profile = PackageStudio::importJson(cleanPath(sourceFile), &error);
    if (!profile) {
        showError(error);
        return false;
    }
    m_packageProfile = *profile;
    const bool saved = persistPackageProfile(bilingualCommitMessage(
        QStringLiteral("packages: import profile"),
        QStringLiteral("套件：匯入設定檔")));
    emit studioChanged();
    return saved;
}

bool AppController::exportPackageProfile(const QString &destinationFile)
{
    QString error;
    if (!PackageStudio::exportJson(m_packageProfile, cleanPath(destinationFile), &error)) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Package profile exported."),
                          QStringLiteral("套件設定已匯出。")));
    return true;
}

bool AppController::stagePackageProfile()
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return false;
    }
    QString error;
    const QString bundleRoot = QDir(m_project->projectDirectory)
        .filePath(QStringLiteral(".wimforge/generated/packages/bundle"));
    const auto bundle = PackageStudio::materializeFirstLogonBundle(
        m_packageProfile, m_project->projectDirectory, bundleRoot, &error);
    if (!bundle) {
        showError(error);
        return false;
    }
    const QJsonObject profileJson = PackageStudio::toJson(m_packageProfile);
    const QJsonObject stagingManifest = PackageStudio::generateIsoStagingManifest(
        m_packageProfile, &error);
    const QList<PackageStagedFile> stagedFiles = bundle->files;
    const bool saved = mutateProject(bilingualCommitMessage(
        QStringLiteral("packages: stage profile into image"),
        QStringLiteral("套件：將設定檔加入映像")),
        [profileJson, stagingManifest, stagedFiles](ProjectConfig &project) {
            project.settings.insert(QStringLiteral("_packageProfile"), profileJson);
            project.settings.insert(QStringLiteral("_packageStagingManifest"), stagingManifest);
            QJsonArray staged = project.options.extra.value(QStringLiteral("stagedFiles")).toArray();
            QJsonArray filtered;
            for (const QJsonValue &item : staged) {
                if (!item.toObject().value(QStringLiteral("role")).toString()
                         .startsWith(QStringLiteral("package-"))) {
                    filtered.append(item);
                }
            }
            for (const PackageStagedFile &file : stagedFiles) {
                filtered.append(QJsonObject{
                    {QStringLiteral("role"), file.role},
                    {QStringLiteral("source"), file.sourcePath},
                    {QStringLiteral("destination"),
                     QStringLiteral("ProgramData/WimForge/PackageStudio/%1")
                         .arg(file.relativePath)},
                    {QStringLiteral("scope"), QStringLiteral("image")},
                    {QStringLiteral("sha256"), file.sha256},
                });
            }
            project.options.extra.insert(QStringLiteral("stagedFiles"), filtered);
            project.postSetupItems.erase(
                std::remove_if(project.postSetupItems.begin(), project.postSetupItems.end(),
                    [](const QString &item) {
                        return item.contains(QStringLiteral("WimForge-Packages.ps1"),
                                             Qt::CaseInsensitive)
                            || item.contains(QStringLiteral("register-first-logon.ps1"),
                                             Qt::CaseInsensitive);
                    }), project.postSetupItems.end());
            const QString command = QStringLiteral(
                "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                "-File \"%ProgramData%\\WimForge\\PackageStudio\\register-first-logon.ps1\"");
            if (!project.postSetupItems.contains(command))
                project.postSetupItems.append(command);
        });
    if (saved)
        notify(QStringLiteral("Package profile staged"),
                QStringLiteral("%1 verified bundle files were staged. Setup registers an elevated interactive first-logon task; failed packages retry at a later administrator logon.")
                    .arg(stagedFiles.size()),
                QStringLiteral("success"));
    return saved;
}

bool AppController::persistUnattendedProfile(const QString &message, bool writeXml)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return false;
    }
    QString xmlPath = m_project->unattendedXmlPath;
    QString error;
    if (writeXml) {
        xmlPath = QDir(m_project->projectDirectory)
            .filePath(QStringLiteral(".wimforge/generated/unattended/autounattend.xml"));
        if (!m_unattendProfile.exportXml(xmlPath, &error)) {
            showError(error);
            return false;
        }
    }
    const QJsonObject profileJson = m_unattendProfile.toJson();
    const UnattendProfile profile = m_unattendProfile;
    return mutateProject(message, [profileJson, profile, xmlPath](ProjectConfig &project) {
        project.settings.insert(QStringLiteral("_unattendProfile"), profileJson);
        if (!xmlPath.isEmpty())
            project.unattendedXmlPath = xmlPath;
        QJsonArray staged = project.options.extra.value(QStringLiteral("stagedFiles")).toArray();
        QJsonArray filtered;
        for (const QJsonValue &item : staged) {
            if (item.toObject().value(QStringLiteral("role")).toString() != QStringLiteral("unattended-answer"))
                filtered.append(item);
        }
        if (!xmlPath.isEmpty() && profile.copyToMediaRoot) {
            filtered.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("unattended-answer")},
                {QStringLiteral("source"), xmlPath},
                {QStringLiteral("destination"), QStringLiteral("autounattend.xml")},
                {QStringLiteral("scope"), QStringLiteral("media")},
            });
        }
        if (!xmlPath.isEmpty() && profile.copyToInstallImage) {
            filtered.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("unattended-answer")},
                {QStringLiteral("source"), xmlPath},
                {QStringLiteral("destination"), QStringLiteral("Windows/Panther/unattend.xml")},
                {QStringLiteral("scope"), QStringLiteral("image")},
            });
        }
        project.options.extra.insert(QStringLiteral("stagedFiles"), filtered);
    });
}

void AppController::loadUnattendedTemplate(const QString &templateId)
{
    m_unattendProfile = templateId.compare(QStringLiteral("ai-development"), Qt::CaseInsensitive) == 0
        ? UnattendBuilder::aiDevelopmentTemplate() : UnattendBuilder::fullAutomationTemplate();
    persistUnattendedProfile(bilingualCommitMessage(
        QStringLiteral("unattended: select %1 template").arg(templateId),
        QStringLiteral("無人值守：選擇 %1 範本").arg(templateId)));
    emit studioChanged();
}

void AppController::setComputerNameBehavior(int mode, const QString &value)
{
    const int bounded = qBound(0, mode, 3);
    m_unattendProfile.computerNameMode = static_cast<ComputerNameMode>(bounded);
    if (m_unattendProfile.computerNameMode == ComputerNameMode::SerialNumber)
        m_unattendProfile.serialPrefix = value.trimmed();
    else
        m_unattendProfile.computerName = value.trimmed();
    m_unattendProfile.applyComputerNameBehavior();
    persistUnattendedProfile(bilingualCommitMessage(
        QStringLiteral("unattended: change computer-name behavior"),
        QStringLiteral("無人值守：更改電腦名稱行為")));
    emit studioChanged();
}

void AppController::setUnattendedValue(const QString &pass,
                                       const QString &component,
                                       const QString &path,
                                       const QString &value)
{
    const auto parsedPass = UnattendBuilder::parsePass(pass);
    if (!parsedPass || component.trimmed().isEmpty() || path.trimmed().isEmpty()) {
        showError(QStringLiteral("Pass, component, and setting path are required."));
        return;
    }
    m_unattendProfile.setValue(*parsedPass, component.trimmed(),
                               path.split(QLatin1Char('/'), Qt::SkipEmptyParts), value);
    persistUnattendedProfile(bilingualCommitMessage(
        QStringLiteral("unattended: set %1/%2/%3")
            .arg(pass, component.trimmed(), path.trimmed()),
        QStringLiteral("無人值守：設定 %1/%2/%3")
            .arg(pass, component.trimmed(), path.trimmed())));
    emit studioChanged();
}

bool AppController::importUnattended(const QString &sourceFile)
{
    QString error;
    const QString path = cleanPath(sourceFile);
    const auto profile = QFileInfo(path).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0
        ? UnattendProfile::importJson(path, &error) : UnattendProfile::importXml(path, &error);
    if (!profile) {
        showError(error);
        return false;
    }
    m_unattendProfile = *profile;
    const bool saved = persistUnattendedProfile(bilingualCommitMessage(
        QStringLiteral("unattended: import answer file"),
        QStringLiteral("無人值守：匯入答案檔")));
    emit studioChanged();
    return saved;
}

bool AppController::exportUnattended(const QString &destinationFile)
{
    QString error;
    const QString path = cleanPath(destinationFile);
    const bool ok = QFileInfo(path).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0
        ? m_unattendProfile.exportJson(path, &error) : m_unattendProfile.exportXml(path, &error);
    if (!ok) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Unattended profile exported."),
                          QStringLiteral("無人值守設定已匯出。")));
    return true;
}

void AppController::runOpenCode(const QString &prompt,
                                const std::function<void(const QString &)> &completed)
{
    if (prompt.trimmed().isEmpty()) {
        showError(localized(QStringLiteral("OpenCode needs a non-empty request."),
                            QStringLiteral("OpenCode 需要一段唔可以留空嘅要求。")));
        return;
    }
    if (!m_openCodeSetup || !m_openCodeSetup->ready()) {
        showError(localized(
            QStringLiteral("OpenCode host integration is not ready. Open Package Studio and select Verify / install now before using an assisted action."),
            QStringLiteral("OpenCode host 整合未準備好。請先去 Package Studio 撳 Verify / install now，之後先用輔助動作。")));
        return;
    }
    m_openCodeRequests.enqueue({prompt, completed});
    processNextOpenCodeRequest();
}

void AppController::processNextOpenCodeRequest()
{
    if (m_openCodeRequestBusy || m_openCodeRequests.isEmpty())
        return;
    if (!m_openCodeSetup || !m_openCodeSetup->ready()) {
        m_openCodeRequests.clear();
        m_openCodeRequestStatus = localized(
            QStringLiteral("OpenCode host integration lost readiness. Select Verify / install now again before retrying."),
            QStringLiteral("OpenCode host 整合已經唔再 ready。請再撳 Verify / install now，之後先重試。"));
        showError(m_openCodeRequestStatus);
        emit studioChanged();
        return;
    }

    OpenCodeRequest request = m_openCodeRequests.dequeue();
    const QString executable = m_openCodeSetup->executablePath();
    if (executable.isEmpty()) {
        m_openCodeRequestStatus = localized(
            QStringLiteral("OpenCode was verified but its executable is no longer available."),
            QStringLiteral("OpenCode 本來已驗證，但而家搵唔到佢個執行檔。"));
        showError(m_openCodeRequestStatus);
        m_openCodeRequests.clear();
        emit studioChanged();
        return;
    }

    auto *process = new QProcess(this);
    m_openCodeProcess = process;
    m_openCodeRequestBusy = true;
    m_openCodeRequestTimedOut = false;
    m_openCodeRequestStatus = localized(QStringLiteral("OpenCode is reasoning locally…"),
                                        QStringLiteral("OpenCode 喺本機諗緊…"));
    process->setProgram(resolveExecutableForLaunch(executable));
    process->setArguments({QStringLiteral("run"), request.prompt,
                           QStringLiteral("--format"), QStringLiteral("json")});
    process->setProcessChannelMode(QProcess::MergedChannels);
    emit studioChanged();

    QTimer::singleShot(300'000, process, [this, process] {
            if (process != m_openCodeProcess || process->state() == QProcess::NotRunning)
                return;
            m_openCodeRequestTimedOut = true;
            process->terminate();
            QTimer::singleShot(1'500, process, [this, process] {
                if (process == m_openCodeProcess
                    && process->state() != QProcess::NotRunning) {
                    process->kill();
                }
            });
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, request = std::move(request)](
                int code, QProcess::ExitStatus status) mutable {
                if (process->property("wimforgeHandled").toBool())
                    return;
                process->setProperty("wimforgeHandled", true);
                const QByteArray output = process->readAll();
                const bool timedOut = m_openCodeRequestTimedOut;
                m_openCodeProcess = nullptr;
                m_openCodeRequestBusy = false;
                m_openCodeRequestTimedOut = false;
                process->deleteLater();
                if (timedOut || status != QProcess::NormalExit || code != 0) {
                    m_openCodeRequestStatus = timedOut
                        ? localized(
                              QStringLiteral("OpenCode request timed out after five minutes."),
                              QStringLiteral("OpenCode 要求等咗五分鐘都未完成，已經逾時。"))
                        : localized(
                              QStringLiteral("OpenCode request failed: %1")
                                  .arg(QString::fromUtf8(output).trimmed()),
                              QStringLiteral("OpenCode 要求失敗：%1")
                                  .arg(QString::fromUtf8(output).trimmed()));
                    showError(m_openCodeRequestStatus);
                } else {
                    m_openCodeRequestStatus = localized(
                        QStringLiteral("OpenCode completed the request."),
                        QStringLiteral("OpenCode 已經完成要求。"));
                    if (request.completed)
                        request.completed(openCodeText(output));
                }
                emit studioChanged();
                processNextOpenCodeRequest();
        });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart
                    || process->property("wimforgeHandled").toBool()) {
                    return;
                }
                process->setProperty("wimforgeHandled", true);
                m_openCodeProcess = nullptr;
                m_openCodeRequestBusy = false;
                m_openCodeRequestTimedOut = false;
                m_openCodeRequestStatus = localized(
                    QStringLiteral("OpenCode request could not start: %1")
                        .arg(process->errorString()),
                    QStringLiteral("OpenCode 要求開始唔到：%1")
                        .arg(process->errorString()));
                process->deleteLater();
                showError(m_openCodeRequestStatus);
                emit studioChanged();
                processNextOpenCodeRequest();
        });
    configureProcessWithoutConsole(*process);
    process->start();
}

void AppController::askOpenCodeToFillUnattended(const QString &intent)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return;
    }
    const QString current = QString::fromUtf8(
        QJsonDocument(m_unattendProfile.toJson()).toJson(QJsonDocument::Compact));
    runOpenCode(QStringLiteral(
        "Fill a WimForge unattended profile from the user's intent. Preserve its schema and all existing "
        "fields, never invent a product key, never put secrets in plaintext, and return only the complete "
        "JSON object. For a computer-name prompt use computerNameMode=prompt; do not write literal [Prompt] "
        "into Microsoft ComputerName. Current profile: %1 User intent: %2")
        .arg(current, intent.trimmed()), [this](const QString &answer) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(unfenceJson(answer), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                showError(localized(
                    QStringLiteral("OpenCode returned invalid unattended JSON: %1")
                        .arg(parseError.errorString()),
                    QStringLiteral("OpenCode 傳返嚟嘅無人值守 JSON 無效：%1")
                        .arg(parseError.errorString())));
                return;
            }
            QString error;
            const auto profile = UnattendProfile::fromJson(document.object(), &error);
            if (!profile) {
                showError(localized(
                    QStringLiteral("OpenCode's unattended result did not validate: %1").arg(error),
                    QStringLiteral("OpenCode 嘅無人值守結果過唔到驗證：%1").arg(error)));
                return;
            }
            m_unattendProfile = *profile;
            persistUnattendedProfile(bilingualCommitMessage(
                QStringLiteral("unattended: apply OpenCode-assisted fill"),
                QStringLiteral("無人值守：套用 OpenCode 協助填寫")));
            notify(localized(QStringLiteral("Unattended profile filled"),
                             QStringLiteral("無人值守設定已填好")),
                   localized(QStringLiteral("OpenCode's proposal was validated, written to XML and committed. Use Ctrl+Z to reverse it."),
                             QStringLiteral("OpenCode 提議已通過驗證、寫入 XML 同 commit。可以用 Ctrl+Z 復原。")),
                   QStringLiteral("success"));
            emit studioChanged();
        });
}

void AppController::ensureOpenCode()
{
    m_openCodeSetup->retry([this](bool ready, const QString &) {
        if (ready) {
            showSuccess(localized(QStringLiteral("OpenCode is ready."),
                                  QStringLiteral("OpenCode 準備好。")));
        }
    });
}

bool AppController::persistWinForgeBridgeRecipe(const QString &message,
                                                const WinForgeRecipe &recipe)
{
    const WinForgeBridgeValidation validation = WinForgeBridge::validateRecipe(recipe);
    if (!validation.ok()) {
        showError(validation.message());
        return false;
    }
    const QJsonObject recipeJson = WinForgeBridge::toJson(recipe);
    const QString runtimePath = m_winForgeRuntimePath;
    const bool includeRuntime = m_winForgeIncludeRuntime;
    return mutateProject(message, [recipeJson, runtimePath, includeRuntime](ProjectConfig &project) {
        project.settings.insert(QStringLiteral("_winForgeRecipe"), recipeJson);
        project.settings.insert(QStringLiteral("_winForgeRuntimePath"), runtimePath);
        project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), includeRuntime);
    });
}

void AppController::proposeWinForgeBridgeActions(const QString &intent)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before creating a WinForge recipe."));
        return;
    }
    const QString request = intent.trimmed();
    if (request.isEmpty())
        return;
    runOpenCode(QStringLiteral(
        "Translate the user's desired post-install outcome into one safe WinForge page deep-link. "
        "The verified legacy interface is WinForge.exe --page <alias>; it does not expose a hidden "
        "headless tweak CLI. Use a known alias such as dashboard, ai, aichat, packages, git, vscode, "
        "terminal, settings, or use search:<short query> when uncertain. Return only a JSON object "
        "with string fields page and title. Do not return commands, scripts, registry edits, or prose. "
        "User intent: %1").arg(request), [this](const QString &answer) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(unfenceJson(answer), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                showError(QStringLiteral("OpenCode returned an invalid WinForge proposal: %1")
                              .arg(parseError.errorString()));
                return;
            }
            const QJsonObject proposal = document.object();
            const QString page = proposal.value(QStringLiteral("page")).toString().trimmed();
            if (page.isEmpty() || page.size() > 2048 || page.contains(QChar::Null)) {
                showError(QStringLiteral("OpenCode's proposed page target did not pass validation."));
                return;
            }
            WinForgeRecipe candidate = m_winForgeRecipe;
            WinForgeAction action;
            action.id = QStringLiteral("proposal-%1")
                            .arg(QDateTime::currentMSecsSinceEpoch());
            action.idempotencyKey = action.id;
            action.kind = WinForgeActionKind::Page;
            action.phase = WinForgeActionPhase::User;
            action.enabled = false;
            action.target = page;
            candidate.actions.append(action);
            if (persistWinForgeBridgeRecipe(bilingualCommitMessage(
                    QStringLiteral("winforge: add OpenCode proposal %1").arg(page),
                    QStringLiteral("WinForge：加入 OpenCode 提議 %1").arg(page)), candidate)) {
                m_winForgeBridgeStatus = localized(
                    QStringLiteral("OpenCode proposed '%1'. Review it, then switch it on to approve.")
                        .arg(proposal.value(QStringLiteral("title")).toString(page)),
                    QStringLiteral("OpenCode 提議咗「%1」；睇清楚先撳掣批准。")
                        .arg(proposal.value(QStringLiteral("title")).toString(page)));
                emit studioChanged();
            }
        });
}

bool AppController::addWinForgeBridgeAction(const QString &kind,
                                            const QString &target,
                                            const QString &executable,
                                            const QString &argumentsJson,
                                            const QString &phase)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before editing its WinForge recipe."));
        return false;
    }
    const auto parsedKind = winForgeActionKind(kind);
    if (!parsedKind) {
        showError(QStringLiteral("Unknown WinForge bridge action kind."));
        return false;
    }
    if (*parsedKind == WinForgeActionKind::Registry || *parsedKind == WinForgeActionKind::Copy) {
        showError(QStringLiteral(
            "Registry and verified-copy actions require their full typed schema. Import a validated "
            "WinForge recipe JSON; WimForge will not guess security-sensitive fields."));
        return false;
    }

    WinForgeAction action;
    action.id = QStringLiteral("%1-%2")
                    .arg(WinForgeBridge::actionKindName(*parsedKind))
                    .arg(QDateTime::currentMSecsSinceEpoch());
    action.idempotencyKey = action.id;
    action.kind = *parsedKind;
    action.phase = phase.trimmed().compare(QStringLiteral("machine"), Qt::CaseInsensitive) == 0
        ? WinForgeActionPhase::Machine : WinForgeActionPhase::User;
    action.enabled = false;
    if (*parsedKind == WinForgeActionKind::Command) {
        action.executable = executable.trimmed();
        if (!argumentsJson.trimmed().isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument arguments = QJsonDocument::fromJson(argumentsJson.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !arguments.isArray()) {
                showError(QStringLiteral("Command arguments must be one JSON string array: %1")
                              .arg(parseError.errorString()));
                return false;
            }
            for (const QJsonValue &value : arguments.array()) {
                if (!value.isString()) {
                    showError(QStringLiteral("Every command argument must be a JSON string."));
                    return false;
                }
                action.arguments.append(value.toString());
            }
        }
    } else {
        action.target = target.trimmed();
        if (*parsedKind == WinForgeActionKind::Tweak)
            action.value = true;
    }

    WinForgeRecipe candidate = m_winForgeRecipe;
    candidate.actions.append(action);
    const QString actionKind = WinForgeBridge::actionKindName(*parsedKind);
    const bool saved = persistWinForgeBridgeRecipe(bilingualCommitMessage(
        QStringLiteral("winforge: add %1 draft").arg(actionKind),
        QStringLiteral("WinForge：加入 %1 草稿").arg(actionKind)),
        candidate);
    if (saved) {
        m_winForgeBridgeStatus = localized(
            QStringLiteral("Draft added. Switch it on only after reviewing its exact typed fields."),
            QStringLiteral("草稿已加入；睇清楚 typed 欄位先好開啟。"));
        emit studioChanged();
    }
    return saved;
}

bool AppController::removeWinForgeBridgeAction(const QString &id)
{
    WinForgeRecipe candidate = m_winForgeRecipe;
    const qsizetype originalCount = candidate.actions.size();
    candidate.actions.erase(std::remove_if(candidate.actions.begin(), candidate.actions.end(),
        [&id](const WinForgeAction &action) { return action.id == id; }), candidate.actions.end());
    if (candidate.actions.size() == originalCount) {
        showError(QStringLiteral("The selected WinForge action no longer exists."));
        return false;
    }
    return persistWinForgeBridgeRecipe(bilingualCommitMessage(
        QStringLiteral("winforge: remove action %1").arg(id),
        QStringLiteral("WinForge：移除動作 %1").arg(id)), candidate);
}

bool AppController::setWinForgeBridgeActionEnabled(const QString &id, bool enabled)
{
    WinForgeRecipe candidate = m_winForgeRecipe;
    auto found = std::find_if(candidate.actions.begin(), candidate.actions.end(),
        [&id](const WinForgeAction &action) { return action.id == id; });
    if (found == candidate.actions.end()) {
        showError(QStringLiteral("The selected WinForge action no longer exists."));
        return false;
    }
    if (enabled) {
        const QString capability = winForgeCapability(found->kind);
        if (!capability.isEmpty()
            && (!m_winForgeRuntimeContract.runtimeFound
                || !m_winForgeRuntimeContract.capabilities.contains(capability)
                || !m_winForgeRuntimeContract.invocations.contains(capability))) {
            showError(QStringLiteral(
                "This action cannot be approved until the selected runtime declares '%1'.")
                          .arg(capability));
            emit studioChanged();
            return false;
        }
    }
    found->enabled = enabled;
    return persistWinForgeBridgeRecipe(bilingualCommitMessage(
        QStringLiteral("winforge: %1 action %2")
            .arg(enabled ? QStringLiteral("approve") : QStringLiteral("disable"), id),
        QStringLiteral("WinForge：%1動作 %2")
            .arg(enabled ? QStringLiteral("批准") : QStringLiteral("停用"), id)), candidate);
}

void AppController::setWinForgeBridgeIncludeRuntime(bool enabled)
{
    if (m_winForgeIncludeRuntime == enabled)
        return;
    m_winForgeIncludeRuntime = enabled;
    m_settings.setValue(QStringLiteral("bridge/includeRuntime"), enabled);
    if (m_project) {
        mutateProject(bilingualCommitMessage(
            QStringLiteral("winforge: change self-contained runtime bundling"),
            QStringLiteral("WinForge：更改自包含 runtime bundling")),
            [enabled](ProjectConfig &project) {
                project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), enabled);
            });
    }
    emit studioChanged();
}

void AppController::setWinForgeBridgeRuntimePath(const QString &path)
{
    const QString cleaned = cleanPath(path);
    if (m_winForgeRuntimePath == cleaned)
        return;
    m_winForgeRuntimePath = cleaned;
    m_winForgeRuntimeContract = {};
    m_winForgeRuntimeStatus = QStringLiteral("Runtime path changed; detect its contract before approval.");
    m_settings.setValue(QStringLiteral("bridge/runtimePath"), cleaned);
    if (m_project) {
        mutateProject(bilingualCommitMessage(
            QStringLiteral("winforge: select runtime folder"),
            QStringLiteral("WinForge：選擇 runtime 資料夾")),
            [cleaned](ProjectConfig &project) {
                project.settings.insert(QStringLiteral("_winForgeRuntimePath"), cleaned);
            });
    }
    emit studioChanged();
}

void AppController::detectWinForgeBridgeRuntime()
{
    QString error;
    m_winForgeRuntimeContract = WinForgeBridge::detectRuntimeContract(m_winForgeRuntimePath, &error);
    if (!m_winForgeRuntimeContract.runtimeFound) {
        m_winForgeRuntimeStatus = error.isEmpty()
            ? QStringLiteral("No compatible WinForge runtime was found.") : error;
        showError(m_winForgeRuntimeStatus);
        emit studioChanged();
        return;
    }
    const WinForgeBridgeValidation validation = WinForgeBridge::validateAgainstRuntime(
        m_winForgeRecipe, m_winForgeRuntimeContract);
    m_winForgeRuntimeStatus = QStringLiteral("%1 runtime · contract %2 · %3")
        .arg(m_winForgeRuntimeContract.declaredContract ? QStringLiteral("Declared")
                                                        : QStringLiteral("Legacy"))
        .arg(m_winForgeRuntimeContract.contractVersion)
        .arg(m_winForgeRuntimeContract.capabilities.join(QStringLiteral(", ")));
    m_winForgeBridgeStatus = validation.ok()
        ? QStringLiteral("Recipe and runtime contract are compatible.")
        : validation.message();
    showSuccess(localized(QStringLiteral("WinForge runtime contract detected."),
                          QStringLiteral("WinForge runtime contract 偵測完成。")));
    emit studioChanged();
}

bool AppController::importWinForgeBridgeRecipe(const QString &sourceFile)
{
    QString error;
    const auto recipe = WinForgeBridge::importJson(cleanPath(sourceFile), &error);
    if (!recipe) {
        showError(error);
        return false;
    }
    const bool saved = persistWinForgeBridgeRecipe(bilingualCommitMessage(
        QStringLiteral("winforge: import validated recipe %1").arg(recipe->id),
        QStringLiteral("WinForge：匯入已驗證 recipe %1").arg(recipe->id)), *recipe);
    if (saved)
        showSuccess(localized(QStringLiteral("WinForge recipe imported and committed."),
                              QStringLiteral("WinForge recipe 已匯入同 commit。")));
    return saved;
}

bool AppController::exportWinForgeBridgeRecipe(const QString &destinationFile)
{
    QString error;
    if (!WinForgeBridge::exportJson(m_winForgeRecipe, cleanPath(destinationFile), &error)) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Portable WinForge recipe exported."),
                          QStringLiteral("可攜 WinForge recipe 已匯出。")));
    return true;
}

bool AppController::stageWinForgeBridgeIntoIso(const QString &isoStagingPath)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before staging its WinForge bridge."));
        return false;
    }
    const QString staging = cleanPath(isoStagingPath);
    if (staging.isEmpty() || !QFileInfo(staging).isAbsolute()) {
        showError(QStringLiteral("The ISO staging folder must be an absolute path."));
        return false;
    }
    WinForgeStageOptions options;
    options.includeRuntime = m_winForgeIncludeRuntime;
    options.overwriteExisting = true;
    options.payloadDirectory = QDir(m_project->projectDirectory)
        .filePath(QStringLiteral(".wimforge/winforge-payloads"));
    QString error;
    const auto staged = WinForgeBridge::stageForIso(
        m_winForgeRecipe, m_winForgeRuntimePath, staging, options, &error);
    if (!staged) {
        showError(error);
        return false;
    }

    const QString oemSource = QDir(staging).filePath(QStringLiteral("sources/$OEM$"));
    const QJsonObject recipeJson = WinForgeBridge::toJson(m_winForgeRecipe);
    const QString runtimePath = m_winForgeRuntimePath;
    const bool includeRuntime = m_winForgeIncludeRuntime;
    const QJsonObject stageInfo{
        {QStringLiteral("stagingRoot"), staging},
        {QStringLiteral("bundleDirectory"), staged->bundleDirectory},
        {QStringLiteral("manifest"), staged->manifestPath},
        {QStringLiteral("manifestSha256"), staged->manifestSha256},
    };
    const bool saved = mutateProject(bilingualCommitMessage(
        QStringLiteral("winforge: stage approved bridge into ISO plan"),
        QStringLiteral("WinForge：將已批准 bridge 加入 ISO 計劃")),
        [recipeJson, runtimePath, includeRuntime, stageInfo, oemSource](ProjectConfig &project) {
            project.settings.insert(QStringLiteral("_winForgeRecipe"), recipeJson);
            project.settings.insert(QStringLiteral("_winForgeRuntimePath"), runtimePath);
            project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), includeRuntime);
            project.settings.insert(QStringLiteral("_winForgeLastStage"), stageInfo);
            QJsonArray existing = project.options.extra.value(QStringLiteral("stagedFiles")).toArray();
            QJsonArray updated;
            for (const QJsonValue &value : existing) {
                if (!value.isObject()
                    || value.toObject().value(QStringLiteral("role")).toString()
                        != QStringLiteral("winforge-bridge")) {
                    updated.append(value);
                }
            }
            updated.append(QJsonObject{
                {QStringLiteral("source"), oemSource},
                {QStringLiteral("destination"), QStringLiteral("sources/$OEM$")},
                {QStringLiteral("scope"), QStringLiteral("media")},
                {QStringLiteral("role"), QStringLiteral("winforge-bridge")},
            });
            project.options.extra.insert(QStringLiteral("stagedFiles"), updated);
        });
    if (!saved)
        return false;
    m_winForgeBridgeStatus = localized(
        QStringLiteral("Staged %1 file(s), %2 bytes. The verified OEM tree is now part of the reviewed ISO plan.")
            .arg(staged->fileCount).arg(staged->totalBytes),
        QStringLiteral("已放入 %1 個檔、%2 bytes；驗證過嘅 OEM tree 已加入 ISO 計劃。")
            .arg(staged->fileCount).arg(staged->totalBytes));
    notify(QStringLiteral("WinForge bridge staged"), m_winForgeBridgeStatus,
           QStringLiteral("success"));
    emit studioChanged();
    return true;
}

void AppController::updateVmStatus(const QString &message,
                                   const QString &tone,
                                   const QString &detail)
{
    m_vmStatusMessage = message;
    m_vmStatusTone = tone;
    m_vmStatusDetail = detail;
    emit vmLabChanged();
}

void AppController::appendVmLog(const QString &message)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty())
        return;
    if (!m_vmLog.isEmpty())
        m_vmLog.append(QStringLiteral("\n\n"));
    m_vmLog.append(trimmed);
    constexpr qsizetype maximumCharacters = 128 * 1024;
    if (m_vmLog.size() > maximumCharacters)
        m_vmLog = m_vmLog.right(maximumCharacters);
}

void AppController::refreshVmValidationRuns()
{
    m_vmValidationItems.clear();
    if (!m_vmValidationStore) {
        emit vmLabChanged();
        return;
    }
    QString error;
    vmvalidation::RunFilter filter;
    filter.maximumCount = 1'000;
    const QList<vmvalidation::ValidationRun> runs = m_vmValidationStore->history(filter, &error);
    if (!error.isEmpty()) {
        updateVmStatus(localized(QStringLiteral("Validation history needs attention."),
                                 QStringLiteral("驗證歷史需要處理。")),
                       QStringLiteral("error"), error);
        return;
    }
    for (const vmvalidation::ValidationRun &run : runs)
        m_vmValidationItems.append(vmValidationVariant(run, m_vmValidationStore->projectDirectory()));
    emit vmLabChanged();
}

void AppController::recreateVmLab()
{
    if (m_vmManager && m_vmManager->busy())
        m_vmManager->cancel();
    m_vmManager.reset();
    if (m_vmValidationWorker.joinable())
        m_vmValidationWorker.join();
    m_vmValidationBusy = false;
    m_pendingVmBootProvider.clear();
    m_pendingVmBootId.clear();

    QString scopeError;
    const vmlab::VmLabScope scope = vmlab::resolveVmLabScope(
        m_project ? m_project->projectDirectory : QString{}, &scopeError);
    QString scopeRoot = scope.root;
    if (scopeRoot.isEmpty()) {
        m_vmValidationStore.reset();
        updateVmStatus(localized(QStringLiteral("Project VM Lab identity could not be prepared safely."),
                                 QStringLiteral("安全準備唔到工程 VM 實驗室身份。")),
                       QStringLiteral("error"), scopeError);
        return;
    }
    if (m_project) {
        // Multi-gigabyte VM files and absolute provider paths never live in
        // the Git-backed project. A persisted project UUID keeps each
        // catalog isolated while validation evidence remains project-scoped.
        m_vmValidationStore = std::make_unique<vmvalidation::VmValidationStore>(
            m_project->projectDirectory);
    } else {
        m_vmValidationStore.reset();
        m_vmValidationItems.clear();
    }
    scopeRoot = QFileInfo(scopeRoot).absoluteFilePath();
    if (!QDir().mkpath(scopeRoot)) {
        updateVmStatus(localized(QStringLiteral("VM Lab storage could not be prepared."),
                                 QStringLiteral("準備唔到 VM 實驗室儲存空間。")),
                       QStringLiteral("error"), scopeRoot);
        return;
    }

    m_vmManager = std::make_unique<vmlab::VmLabManager>(
        QDir(scopeRoot).filePath(QStringLiteral("catalog.json")),
        QDir(scopeRoot).filePath(QStringLiteral("machines")));

    connect(m_vmManager.get(), &vmlab::VmLabManager::stateChanged, this,
            [this](vmlab::ManagerState state) {
        QString message;
        switch (state) {
        case vmlab::ManagerState::Idle:
            message = localized(QStringLiteral("VM Lab is ready."),
                                QStringLiteral("VM 實驗室準備好。"));
            break;
        case vmlab::ManagerState::DetectingProviders:
            message = localized(QStringLiteral("Detecting VMware and VirtualBox…"),
                                QStringLiteral("正在偵測 VMware 同 VirtualBox……"));
            break;
        case vmlab::ManagerState::RefreshingInventory:
            message = localized(QStringLiteral("Refreshing provider inventory…"),
                                QStringLiteral("正在重新整理供應器清單……"));
            break;
        case vmlab::ManagerState::RefreshingSnapshots:
            message = localized(QStringLiteral("Refreshing snapshots…"),
                                QStringLiteral("正在重新整理快照……"));
            break;
        case vmlab::ManagerState::Executing:
            message = localized(QStringLiteral("Executing the exact reviewed VM operation…"),
                                QStringLiteral("正在執行完全相同嘅已審閱 VM 操作……"));
            break;
        case vmlab::ManagerState::Cancelling:
            message = localized(QStringLiteral("Cancelling the provider operation…"),
                                QStringLiteral("正在取消供應器操作……"));
            break;
        case vmlab::ManagerState::Error:
            message = localized(QStringLiteral("VM Lab needs attention."),
                                QStringLiteral("VM 實驗室需要處理。"));
            break;
        }
        const QString tone = state == vmlab::ManagerState::Error
            ? QStringLiteral("error") : state == vmlab::ManagerState::Idle
                ? QStringLiteral("success") : QStringLiteral("info");
        updateVmStatus(message, tone, m_vmManager ? m_vmManager->lastError() : QString());
    });
    connect(m_vmManager.get(), &vmlab::VmLabManager::providersChanged,
            this, &AppController::vmLabChanged);
    connect(m_vmManager.get(), &vmlab::VmLabManager::machinesChanged, this, [this] {
        emit vmLabChanged();
        QTimer::singleShot(0, this, [this] {
            tryPendingVmBoot();
            if (!m_vmManager || m_vmManager->busy() || !m_vmManager->selectedMachine())
                return;
            const QString providerId = m_vmManager->selectedMachine()->ref.providerId;
            const auto providers = m_vmManager->providers();
            const auto found = std::find_if(providers.cbegin(), providers.cend(),
                                            [&providerId](const vmlab::ProviderInfo &provider) {
                return provider.id == providerId;
            });
            if (found != providers.cend()
                && found->supports(vmlab::capability::snapshots())) {
                m_vmManager->refreshSnapshots();
            }
        });
    });
    connect(m_vmManager.get(), &vmlab::VmLabManager::selectionChanged,
            this, &AppController::vmLabChanged);
    connect(m_vmManager.get(), &vmlab::VmLabManager::snapshotsChanged,
            this, &AppController::vmLabChanged);
    connect(m_vmManager.get(), &vmlab::VmLabManager::reviewedPlanChanged,
            this, &AppController::vmLabChanged);
    connect(m_vmManager.get(), &vmlab::VmLabManager::evidenceAdded, this,
            [this](const vmlab::OperationEvidence &evidence) {
        appendVmLog(vmEvidenceText(evidence));
        emit vmLabChanged();
    });
    connect(m_vmManager.get(), &vmlab::VmLabManager::errorOccurred, this,
            [this](const QString &error) {
        updateVmStatus(localized(QStringLiteral("VM operation was rejected or failed."),
                                 QStringLiteral("VM 操作被拒絕或失敗。")),
                       QStringLiteral("error"), error);
        emit snackbarRequested(error, QStringLiteral("error"));
    });
    connect(m_vmManager.get(), &vmlab::VmLabManager::taskFinished, this,
            [this](const vmlab::OperationEvidence &evidence) {
        if (evidence.action == QStringLiteral("create") && !evidence.success) {
            m_pendingVmBootProvider.clear();
            m_pendingVmBootId.clear();
        }
        updateVmStatus(
            evidence.success
                ? localized(QStringLiteral("VM operation completed."),
                            QStringLiteral("VM 操作已完成。"))
                : evidence.cancelled
                    ? localized(QStringLiteral("VM operation was cancelled."),
                                QStringLiteral("VM 操作已取消。"))
                    : localized(QStringLiteral("VM operation failed."),
                                QStringLiteral("VM 操作失敗。")),
            evidence.success ? QStringLiteral("success")
                             : evidence.cancelled ? QStringLiteral("warning")
                                                  : QStringLiteral("error"),
            evidence.error);
    });

    QString loadError;
    if (!m_vmManager->load(&loadError)) {
        updateVmStatus(localized(QStringLiteral("VM Lab catalog could not be loaded."),
                                 QStringLiteral("載入唔到 VM 實驗室目錄。")),
                       QStringLiteral("error"), loadError);
        return;
    }
    refreshVmValidationRuns();
    updateVmStatus(
        m_project
            ? localized(QStringLiteral("Project VM Lab is ready."),
                        QStringLiteral("工程 VM 實驗室準備好。"))
            : localized(QStringLiteral("Global VM management is ready; open a project to record validation evidence."),
                        QStringLiteral("全域 VM 管理準備好；開啟工程先可以記錄驗證證據。")),
        QStringLiteral("success"));
    QTimer::singleShot(0, this, [this] {
        if (m_vmManager && !m_vmManager->busy())
            m_vmManager->detectProviders();
    });
}

void AppController::tryPendingVmBoot()
{
    if (!m_vmManager || m_vmManager->busy()
        || m_pendingVmBootProvider.isEmpty() || m_pendingVmBootId.isEmpty()) {
        return;
    }
    const QList<vmlab::Machine> machines = m_vmManager->machines();
    const auto found = std::find_if(machines.cbegin(), machines.cend(), [this](const vmlab::Machine &machine) {
        return machine.ref.providerId == m_pendingVmBootProvider
            && machine.ref.id == m_pendingVmBootId;
    });
    if (found == machines.cend())
        return;
    const QString providerId = m_pendingVmBootProvider;
    const QString id = m_pendingVmBootId;
    m_pendingVmBootProvider.clear();
    m_pendingVmBootId.clear();
    if (!m_vmManager->selectMachine(providerId, id))
        return;
    const std::optional<vmlab::OperationPreview> preview = m_vmManager->reviewStart(false);
    if (!stageVmPreview(preview)) {
        updateVmStatus(localized(QStringLiteral("The VM was created, but automatic boot was rejected."),
                                 QStringLiteral("VM 已建立，但自動啟動被拒絕。")),
                       QStringLiteral("warning"), m_vmManager->lastError());
    }
}

void AppController::refreshVmLab()
{
    if (!m_vmManager) {
        recreateVmLab();
        return;
    }
    if (m_vmManager->busy()) {
        updateVmStatus(localized(QStringLiteral("Wait for the active VM task before refreshing."),
                                 QStringLiteral("等而家個 VM 工作完成先重新整理。")),
                       QStringLiteral("warning"));
        return;
    }
    m_vmManager->detectProviders();
}

bool AppController::selectVm(const QString &providerId, const QString &id)
{
    if (!m_vmManager || m_vmManager->busy())
        return false;
    const QList<vmlab::Machine> machines = m_vmManager->machines();
    const auto match = std::find_if(machines.cbegin(), machines.cend(),
                                    [&providerId, &id](const vmlab::Machine &machine) {
        return machine.ref.providerId == providerId && machine.ref.id == id;
    });
    if (match == machines.cend()) {
        const QString error = QStringLiteral("The selected VM is no longer in provider inventory.");
        updateVmStatus(localized(QStringLiteral("The VM selection could not be applied."),
                                 QStringLiteral("套用唔到 VM 選擇。")),
                       QStringLiteral("error"), error);
        return false;
    }
    if (!m_vmManager->selectMachine(match->ref.providerId, match->ref.id))
        return false;
    updateVmStatus(localized(QStringLiteral("Virtual machine selected."),
                             QStringLiteral("已選取虛擬機。")),
                   QStringLiteral("success"), match->ref.name);
    const auto providers = m_vmManager->providers();
    const auto provider = std::find_if(providers.cbegin(), providers.cend(),
                                       [&providerId](const vmlab::ProviderInfo &item) {
        return item.id == providerId;
    });
    if (provider != providers.cend() && provider->supports(vmlab::capability::snapshots())) {
        QTimer::singleShot(0, this, [this] {
            if (m_vmManager && !m_vmManager->busy())
                m_vmManager->refreshSnapshots();
        });
    }
    emit vmLabChanged();
    return true;
}

bool AppController::stageVmPreview(
    const std::optional<vmlab::OperationPreview> &preview)
{
    if (!m_vmManager)
        return false;
    m_vmPendingPreview.clear();
    m_pendingVmBootProvider.clear();
    m_pendingVmBootId.clear();
    if (!preview) {
        updateVmStatus(localized(QStringLiteral("The provider could not produce a safe operation preview."),
                                 QStringLiteral("供應器產生唔到安全操作預覽。")),
                       QStringLiteral("error"), m_vmManager->lastError());
        return false;
    }
    m_vmPendingPreview = vmPreviewVariant(*preview);
    appendVmLog(QStringLiteral("PREVIEW %1 [%2]\nEffects: %3\nWarnings: %4")
                    .arg(preview->action,
                         preview->id.toString(QUuid::WithoutBraces),
                         preview->effects.join(QStringLiteral(" | ")),
                         preview->warnings.join(QStringLiteral(" | "))));
    updateVmStatus(localized(QStringLiteral("Review the exact provider operation before execution."),
                             QStringLiteral("執行前請審閱完全相同嘅供應器操作。")),
                   QStringLiteral("warning"), preview->action);
    emit vmLabChanged();
    emit vmPreviewReady();
    return true;
}

bool AppController::executePendingVmPreview(const QString &previewId,
                                            const QString &typedConfirmation)
{
    if (!m_vmManager || m_vmPendingPreview.isEmpty())
        return false;
    const QString expectedId = m_vmPendingPreview.value(QStringLiteral("id")).toString();
    if (previewId.trimmed() != expectedId) {
        updateVmStatus(localized(QStringLiteral("The reviewed preview changed; review the operation again."),
                                 QStringLiteral("已審閱預覽已改變；請重新審閱操作。")),
                       QStringLiteral("error"));
        return false;
    }
    const QString expectedConfirmation = m_vmPendingPreview
        .value(QStringLiteral("confirmation")).toString();
    if (!expectedConfirmation.isEmpty() && typedConfirmation != expectedConfirmation) {
        updateVmStatus(localized(QStringLiteral("The exact destructive confirmation token did not match."),
                                 QStringLiteral("破壞性操作嘅完整確認字句唔吻合。")),
                       QStringLiteral("error"), expectedConfirmation);
        return false;
    }
    appendVmLog(QStringLiteral("EXECUTE REVIEWED PREVIEW %1 [%2]")
                    .arg(m_vmPendingPreview.value(QStringLiteral("action")).toString(),
                         expectedId));
    const QUuid id(expectedId);
    if (id.isNull() || !m_vmManager->executeReviewed(id, typedConfirmation)) {
        updateVmStatus(localized(QStringLiteral("The reviewed VM operation was not executed."),
                                 QStringLiteral("已審閱 VM 操作未有執行。")),
                       QStringLiteral("error"), m_vmManager->lastError());
        return false;
    }
    const QString action = m_vmPendingPreview.value(QStringLiteral("action")).toString();
    m_vmPendingPreview.clear();
    updateVmStatus(localized(QStringLiteral("Executing the exact reviewed provider operation…"),
                             QStringLiteral("正在執行完全相同嘅已審閱供應器操作……")),
                   QStringLiteral("info"), action);
    emit vmLabChanged();
    return true;
}

void AppController::discardPendingVmPreview()
{
    if (m_vmManager)
        m_vmManager->clearReviewedPlan();
    m_vmPendingPreview.clear();
    m_pendingVmBootProvider.clear();
    m_pendingVmBootId.clear();
    updateVmStatus(localized(QStringLiteral("Reviewed VM operation was discarded."),
                             QStringLiteral("已放棄審閱嘅 VM 操作。")),
                   QStringLiteral("info"));
    emit vmLabChanged();
}

bool AppController::createVm(const QVariantMap &spec)
{
    if (!m_vmManager || m_vmManager->busy())
        return false;
    vmlab::CreateSpec request;
    request.providerId = spec.value(QStringLiteral("providerId")).toString().trimmed();
    request.name = spec.value(QStringLiteral("name")).toString().trimmed();
    request.directory = request.providerId == vmlab::virtualBoxProviderId()
        ? m_vmManager->managedRoot()
        : QDir(m_vmManager->managedRoot()).filePath(request.name);
    request.guestType = providerGuestType(
        request.providerId, spec.value(QStringLiteral("guestType")).toString());
    const std::optional<vmlab::Firmware> firmware = vmFirmware(
        spec.value(QStringLiteral("firmware"), QStringLiteral("efi")).toString());
    const std::optional<vmlab::NetworkMode> network = vmNetworkMode(
        spec.value(QStringLiteral("networkMode"), QStringLiteral("nat")).toString());
    if (!firmware || !network) {
        updateVmStatus(localized(QStringLiteral("VM firmware or network mode is invalid."),
                                 QStringLiteral("VM 韌體或網絡模式無效。")),
                       QStringLiteral("error"));
        return false;
    }
    request.firmware = *firmware;
    request.networkMode = *network;
    request.secureBoot = spec.value(QStringLiteral("secureBoot"), false).toBool();
    request.tpm = spec.value(QStringLiteral("tpm"), false).toBool();
    request.cpuCount = spec.value(QStringLiteral("cpuCount"), 2).toInt();
    request.memoryMiB = spec.value(QStringLiteral("memoryMiB"), 4096).toInt();
    request.diskMiB = spec.value(QStringLiteral("diskMiB"), 65536).toInt();
    request.bridgedInterface = spec.value(QStringLiteral("bridgedInterface")).toString().trimmed();
    request.isoPath = cleanPath(spec.value(QStringLiteral("isoPath")).toString());
    request.unattendedBoot = spec.value(QStringLiteral("unattendedBoot"), false).toBool();
    const bool virtualBoxCreate = request.providerId == vmlab::virtualBoxProviderId();
    const bool vmwareCreate = request.providerId == vmlab::vmwareWorkstationProviderId()
        || request.providerId == vmlab::vmwarePlayerProviderId();
    if (vmwareCreate && request.networkMode == vmlab::NetworkMode::Internal) {
        updateVmStatus(localized(QStringLiteral("VMware creation does not expose a provider-neutral internal network."),
                                 QStringLiteral("VMware 建立流程未有提供供應器中立嘅 internal network。")),
                       QStringLiteral("error"));
        return false;
    }
    if (virtualBoxCreate
        && (request.networkMode == vmlab::NetworkMode::Bridged
            || request.networkMode == vmlab::NetworkMode::HostOnly
            || request.networkMode == vmlab::NetworkMode::Internal)
        && request.bridgedInterface.isEmpty()) {
        updateVmStatus(localized(QStringLiteral("This VirtualBox network mode needs an explicit host interface or network name."),
                                 QStringLiteral("呢個 VirtualBox 網絡模式需要指定主機介面或網絡名稱。")),
                       QStringLiteral("error"));
        return false;
    }
    if (vmwareCreate)
        request.bridgedInterface.clear();
    const std::optional<vmlab::OperationPreview> preview = m_vmManager->reviewCreate(
        request, vmlab::Ownership::Managed);
    if (!preview)
        return stageVmPreview(preview);
    if (!stageVmPreview(preview))
        return false;
    if (spec.value(QStringLiteral("bootAfterCreate"), false).toBool()) {
        m_pendingVmBootProvider = preview->target.providerId;
        m_pendingVmBootId = preview->target.id;
    }
    return true;
}

bool AppController::runVmAction(const QString &action, const QVariantMap &options)
{
    if (!m_vmManager || m_vmManager->busy())
        return false;
    const QString normalized = action.trimmed();
    if (normalized == QStringLiteral("register")) {
        const QString providerId = options.value(QStringLiteral("providerId")).toString().trimmed();
        const QString path = cleanPath(options.value(QStringLiteral("path")).toString());
        const QString name = options.value(QStringLiteral("name")).toString().trimmed();
        return stageVmPreview(m_vmManager->reviewRegister(
            providerId, path, name, vmlab::Ownership::External));
    }
    const std::optional<vmlab::Machine> selected = m_vmManager->selectedMachine();
    if (!selected) {
        updateVmStatus(localized(QStringLiteral("Select a virtual machine first."),
                                 QStringLiteral("請先選取虛擬機。")),
                       QStringLiteral("warning"));
        return false;
    }
    const QString expectedId = options.value(QStringLiteral("vmId")).toString();
    if (!expectedId.isEmpty() && expectedId != selected->ref.id) {
        updateVmStatus(localized(QStringLiteral("The VM selection changed; review the action again."),
                                 QStringLiteral("VM 選擇已改變；請重新審閱操作。")),
                       QStringLiteral("error"));
        return false;
    }
    std::optional<vmlab::OperationPreview> preview;
    if (normalized == QStringLiteral("openConsole")) preview = m_vmManager->reviewOpenConsole();
    else if (normalized == QStringLiteral("start"))
        preview = m_vmManager->reviewStart(options.value(QStringLiteral("headless"), false).toBool());
    else if (normalized == QStringLiteral("shutdown")) preview = m_vmManager->reviewGracefulShutdown();
    else if (normalized == QStringLiteral("powerOff")) {
        preview = m_vmManager->reviewPowerOff();
    } else if (normalized == QStringLiteral("pause")) preview = m_vmManager->reviewPause();
    else if (normalized == QStringLiteral("resume")) preview = m_vmManager->reviewResume();
    else if (normalized == QStringLiteral("reset")) {
        preview = m_vmManager->reviewReset();
    } else if (normalized == QStringLiteral("saveState")) preview = m_vmManager->reviewSaveState();
    else if (normalized == QStringLiteral("forget")) {
        preview = m_vmManager->reviewForgetCatalog();
    } else if (normalized == QStringLiteral("delete")) {
        const bool deleteFiles = options.value(QStringLiteral("deleteFiles"), false).toBool();
        if (!deleteFiles && selected->ownership == vmlab::Ownership::Managed) {
            updateVmStatus(localized(
                QStringLiteral("Managed VM files cannot be orphaned by removing their ownership record. Keep the VM or use the reviewed delete-files action."),
                QStringLiteral("唔可以刪除受管理 VM 嘅擁有權記錄而留下孤立檔案。請保留 VM，或者使用已審閱嘅刪除檔案操作。")),
                QStringLiteral("error"));
            return false;
        }
        preview = deleteFiles ? m_vmManager->reviewDelete()
                              : m_vmManager->reviewUnregister();
    } else {
        updateVmStatus(localized(QStringLiteral("That VM action is unsupported."),
                                 QStringLiteral("唔支援呢個 VM 操作。")),
                       QStringLiteral("error"), normalized);
        return false;
    }
    return stageVmPreview(preview);
}

bool AppController::updateVmConfiguration(const QVariantMap &spec)
{
    if (!m_vmManager || m_vmManager->busy() || !m_vmManager->selectedMachine())
        return false;
    const QString expectedId = spec.value(QStringLiteral("vmId")).toString();
    if (!expectedId.isEmpty() && expectedId != m_vmManager->selectedMachine()->ref.id)
        return false;
    vmlab::ConfigPatch patch;
    if (spec.contains(QStringLiteral("cpuCount")))
        patch.cpuCount = spec.value(QStringLiteral("cpuCount")).toInt();
    if (spec.contains(QStringLiteral("memoryMiB")))
        patch.memoryMiB = spec.value(QStringLiteral("memoryMiB")).toInt();
    if (spec.contains(QStringLiteral("firmware"))) {
        const std::optional<vmlab::Firmware> value = vmFirmware(
            spec.value(QStringLiteral("firmware")).toString());
        if (!value)
            return false;
        patch.firmware = *value;
    }
    if (spec.contains(QStringLiteral("secureBoot")))
        patch.secureBoot = spec.value(QStringLiteral("secureBoot")).toBool();
    if (spec.contains(QStringLiteral("tpm")))
        patch.tpm = spec.value(QStringLiteral("tpm")).toBool();
    if (spec.contains(QStringLiteral("networkMode"))) {
        const std::optional<vmlab::NetworkMode> value = vmNetworkMode(
            spec.value(QStringLiteral("networkMode")).toString());
        if (!value)
            return false;
        patch.networkMode = *value;
        patch.bridgedInterface = spec.value(QStringLiteral("bridgedInterface")).toString();
    }
    if (spec.contains(QStringLiteral("isoPath")))
        patch.isoPath = cleanPath(spec.value(QStringLiteral("isoPath")).toString());
    return stageVmPreview(m_vmManager->reviewConfigure(patch));
}

bool AppController::vmDeviceAction(const QString &action, const QVariantMap &spec)
{
    if (!m_vmManager || m_vmManager->busy() || !m_vmManager->selectedMachine())
        return false;
    const QString expectedId = spec.value(QStringLiteral("vmId")).toString();
    if (!expectedId.isEmpty() && expectedId != m_vmManager->selectedMachine()->ref.id)
        return false;
    const QString normalized = action.trimmed();
    if (normalized == QStringLiteral("addNetwork")) {
        const std::optional<vmlab::NetworkMode> mode = vmNetworkMode(
            spec.value(QStringLiteral("mode")).toString());
        const int slot = spec.value(QStringLiteral("slot")).toInt();
        QString interfaceName = spec.value(QStringLiteral("interfaceName")).toString().trimmed();
        if (!mode || slot < 1 || slot > 32)
            return false;
        const QString providerId = m_vmManager->selectedMachine()->ref.providerId;
        const bool virtualBox = providerId == vmlab::virtualBoxProviderId();
        const bool vmware = providerId == vmlab::vmwareWorkstationProviderId()
            || providerId == vmlab::vmwarePlayerProviderId();
        if (vmware && *mode == vmlab::NetworkMode::Internal) {
            updateVmStatus(localized(QStringLiteral("VMware does not expose a provider-neutral internal network mode."),
                                     QStringLiteral("VMware 未有提供供應器中立嘅 internal network 模式。")),
                           QStringLiteral("error"));
            return false;
        }
        if (virtualBox
            && (*mode == vmlab::NetworkMode::Bridged
                || *mode == vmlab::NetworkMode::HostOnly
                || *mode == vmlab::NetworkMode::Internal)
            && interfaceName.isEmpty()) {
            updateVmStatus(localized(QStringLiteral("This network mode needs an explicit interface or network name."),
                                     QStringLiteral("呢個網絡模式需要指定介面或網絡名稱。")),
                           QStringLiteral("error"));
            return false;
        }
        if (vmware)
            interfaceName.clear();
        vmlab::NetworkAdapterSpec network;
        network.slot = slot;
        network.mode = *mode;
        network.interfaceName = interfaceName;
        network.connected = *mode != vmlab::NetworkMode::Disconnected;
        return stageVmPreview(m_vmManager->reviewAttachNetwork(network));
    }
    if (normalized == QStringLiteral("removeNetwork")) {
        const int slot = spec.value(QStringLiteral("slot")).toInt();
        if (slot < 1 || slot > 32)
            return false;
        return stageVmPreview(m_vmManager->reviewDetachNetwork(slot));
    }

    const std::optional<vmlab::StorageBus> bus = vmStorageBus(
        spec.value(QStringLiteral("bus")).toString());
    if (!bus || !spec.contains(QStringLiteral("controller"))
        || !spec.contains(QStringLiteral("port"))
        || !spec.contains(QStringLiteral("device"))) {
        updateVmStatus(localized(QStringLiteral("Storage actions require an explicit bus, controller, port, and device."),
                                 QStringLiteral("儲存操作需要明確 bus、controller、port 同 device。")),
                       QStringLiteral("error"));
        return false;
    }
    vmlab::StorageDeviceSpec storage;
    storage.bus = *bus;
    storage.controller = spec.value(QStringLiteral("controller")).toInt();
    storage.port = spec.value(QStringLiteral("port")).toInt();
    storage.device = spec.value(QStringLiteral("device")).toInt();
    storage.controllerName = spec.value(QStringLiteral("controllerName")).toString().trimmed();
    storage.path = cleanPath(normalized == QStringLiteral("attachIso")
        ? spec.value(QStringLiteral("isoPath")).toString()
        : spec.value(QStringLiteral("path")).toString());
    storage.optical = normalized == QStringLiteral("attachIso")
        || normalized == QStringLiteral("ejectIso");
    if (m_vmManager->selectedMachine()->ref.providerId == vmlab::virtualBoxProviderId()
        && storage.controllerName.isEmpty()) {
        updateVmStatus(localized(QStringLiteral("VirtualBox storage actions require the exact controller name from live inventory."),
                                 QStringLiteral("VirtualBox 儲存操作需要即時清單提供嘅確切 controller 名稱。")),
                       QStringLiteral("error"));
        return false;
    }
    if (normalized == QStringLiteral("addDisk") || normalized == QStringLiteral("attachIso"))
        return stageVmPreview(m_vmManager->reviewAttachStorage(storage));
    if (normalized == QStringLiteral("detachDisk"))
        return stageVmPreview(m_vmManager->reviewDetachStorage(storage));
    if (normalized == QStringLiteral("ejectIso"))
        return stageVmPreview(m_vmManager->reviewDetachStorage(storage));
    updateVmStatus(localized(QStringLiteral("That device action is unsupported."),
                             QStringLiteral("唔支援呢個裝置操作。")),
                   QStringLiteral("error"), normalized);
    return false;
}

bool AppController::vmSnapshotAction(const QString &action, const QVariantMap &spec)
{
    if (!m_vmManager || m_vmManager->busy() || !m_vmManager->selectedMachine())
        return false;
    const QString expectedId = spec.value(QStringLiteral("vmId")).toString();
    if (!expectedId.isEmpty() && expectedId != m_vmManager->selectedMachine()->ref.id)
        return false;
    const QString normalized = action.trimmed();
    if (normalized == QStringLiteral("take")) {
        return stageVmPreview(m_vmManager->reviewTakeSnapshot(
            spec.value(QStringLiteral("name")).toString().trimmed(),
            spec.value(QStringLiteral("description")).toString().trimmed()));
    }
    const QString snapshotId = spec.value(QStringLiteral("snapshotId")).toString();
    const QList<vmlab::Snapshot> snapshots = m_vmManager->snapshots();
    const auto found = std::find_if(snapshots.cbegin(), snapshots.cend(),
                                    [&snapshotId](const vmlab::Snapshot &snapshot) {
        return snapshot.id == snapshotId;
    });
    if (found == snapshots.cend()) {
        updateVmStatus(localized(QStringLiteral("The snapshot selection changed; refresh and review again."),
                                 QStringLiteral("快照選擇已改變；請重新整理再審閱。")),
                       QStringLiteral("error"));
        return false;
    }
    if (normalized == QStringLiteral("restore"))
        return stageVmPreview(m_vmManager->reviewRestoreSnapshot(*found));
    if (normalized == QStringLiteral("delete"))
        return stageVmPreview(m_vmManager->reviewDeleteSnapshot(*found));
    return false;
}

bool AppController::startVmValidation(const QVariantMap &spec)
{
    if (!m_project || !m_vmValidationStore) {
        updateVmStatus(localized(QStringLiteral("Open a project before recording validation evidence."),
                                 QStringLiteral("記錄驗證證據之前請先開工程。")),
                       QStringLiteral("warning"));
        return false;
    }
    if (vmBusy() || !m_vmManager || !m_vmManager->selectedMachine())
        return false;
    const vmlab::Machine machine = *m_vmManager->selectedMachine();
    if ((!spec.value(QStringLiteral("vmId")).toString().isEmpty()
         && spec.value(QStringLiteral("vmId")).toString() != machine.ref.id)
        || (!spec.value(QStringLiteral("providerId")).toString().isEmpty()
            && spec.value(QStringLiteral("providerId")).toString() != machine.ref.providerId)) {
        updateVmStatus(localized(QStringLiteral("The selected VM changed before validation started."),
                                 QStringLiteral("驗證開始前所選 VM 已改變。")),
                       QStringLiteral("error"));
        return false;
    }
    const QFileInfo iso(cleanPath(spec.value(QStringLiteral("isoPath")).toString()));
    const QFileInfo image(cleanPath(m_project->imagePath));
    const QFileInfo configuration(machine.configPath);
    if (!iso.isAbsolute() || !iso.isFile()
        || iso.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) != 0) {
        updateVmStatus(localized(QStringLiteral("Validation requires an existing ISO output."),
                                 QStringLiteral("驗證需要現有 ISO 輸出。")),
                       QStringLiteral("error"), iso.filePath());
        return false;
    }
    if (!image.isAbsolute() || !image.isFile()) {
        updateVmStatus(localized(QStringLiteral("Validation requires the selected source image file."),
                                 QStringLiteral("驗證需要已選來源映像檔。")),
                       QStringLiteral("error"), image.filePath());
        return false;
    }
    if (!configuration.isAbsolute() || !configuration.isFile()) {
        updateVmStatus(localized(QStringLiteral("Validation requires the selected VM configuration file."),
                                 QStringLiteral("驗證需要已選 VM 設定檔。")),
                       QStringLiteral("error"), configuration.filePath());
        return false;
    }
    QString providerVersion;
    for (const vmlab::ProviderInfo &provider : m_vmManager->providers()) {
        if (provider.id == machine.ref.providerId) {
            providerVersion = provider.version;
            break;
        }
    }
    const QString runName = spec.value(QStringLiteral("name")).toString().trimmed();
    const QString profile = spec.value(QStringLiteral("profile"),
                                       QStringLiteral("full-smoke")).toString().trimmed();
    const QString notes = spec.value(QStringLiteral("notes")).toString();
    QJsonObject configSnapshot = QJsonObject::fromVariantMap(vmMachineVariant(machine, {}));
    configSnapshot.insert(QStringLiteral("runName"), runName);
    configSnapshot.insert(QStringLiteral("profile"), profile);
    configSnapshot.insert(QStringLiteral("notes"), notes);
    configSnapshot.insert(QStringLiteral("imageIndex"), m_project->selectedImageIndex);
    configSnapshot.insert(QStringLiteral("outputFormat"), m_project->outputFormat);

    vmvalidation::RunStart start;
    start.isoPath = iso.absoluteFilePath();
    start.isoSha256 = spec.value(QStringLiteral("isoSha256")).toString().trimmed().toLower();
    start.imagePath = image.absoluteFilePath();
    start.providerId = machine.ref.providerId;
    start.providerVersion = providerVersion;
    start.vmId = machine.ref.id;
    start.vmName = machine.ref.name;
    start.vmConfigPath = configuration.absoluteFilePath();
    start.configSnapshot = configSnapshot;
    start.startedAt = QDateTime::currentDateTimeUtc();
    const QString scope = m_vmValidationStore->projectDirectory();
    if (m_vmValidationWorker.joinable())
        m_vmValidationWorker.join();
    m_vmValidationBusy = true;
    updateVmStatus(localized(QStringLiteral("Hashing the exact ISO and image for validation…"),
                             QStringLiteral("正在雜湊驗證用嘅確切 ISO 同映像……")),
                   QStringLiteral("info"), iso.absoluteFilePath());
    m_vmValidationWorker = std::jthread([this, scope, start] {
        vmvalidation::VmValidationStore store(scope);
        vmvalidation::MutationResult mutation;
        QString error;
        const bool success = store.appendRun(start, &mutation, &error);
        QMetaObject::invokeMethod(this, [this, scope, success, error] {
            if (!m_vmValidationStore || m_vmValidationStore->projectDirectory() != scope)
                return;
            m_vmValidationBusy = false;
            refreshVmValidationRuns();
            updateVmStatus(
                success
                    ? localized(QStringLiteral("Validation run started with exact SHA-256 evidence."),
                                QStringLiteral("驗證執行已開始，並記錄確切 SHA-256 證據。"))
                    : localized(QStringLiteral("Validation run could not be started."),
                                QStringLiteral("開始唔到驗證執行。")),
                success ? QStringLiteral("success") : QStringLiteral("error"), error);
            if (!success && !error.isEmpty())
                emit snackbarRequested(error, QStringLiteral("error"));
        }, Qt::QueuedConnection);
    });
    emit vmLabChanged();
    return true;
}

bool AppController::recordVmValidationMilestone(const QString &runId,
                                                const QVariantMap &spec)
{
    if (!m_vmValidationStore || m_vmValidationBusy)
        return false;
    QString error;
    const std::optional<vmvalidation::ValidationRun> run = m_vmValidationStore->find(runId, &error);
    if (!run) {
        updateVmStatus(localized(QStringLiteral("The validation run was not found."),
                                 QStringLiteral("搵唔到驗證執行。")),
                       QStringLiteral("error"), error);
        return false;
    }
    const QString kind = spec.value(QStringLiteral("kind")).toString().trimmed();
    const QString result = spec.value(QStringLiteral("result")).toString().trimmed().toLower();
    if (kind.isEmpty() || (result != QStringLiteral("pass")
                           && result != QStringLiteral("fail")
                           && result != QStringLiteral("skip"))) {
        return false;
    }
    vmvalidation::MilestoneDraft milestone;
    milestone.phase = kind.startsWith(QStringLiteral("installation"))
            || kind == QStringLiteral("disk-layout")
        ? vmvalidation::MilestonePhase::Install : vmvalidation::MilestonePhase::Boot;
    milestone.name = kind;
    milestone.status = result == QStringLiteral("pass")
        ? vmvalidation::MilestoneStatus::Reached
        : result == QStringLiteral("fail")
            ? vmvalidation::MilestoneStatus::Failed
            : vmvalidation::MilestoneStatus::Skipped;
    milestone.occurredAt = QDateTime::currentDateTimeUtc();
    milestone.note = spec.value(QStringLiteral("notes")).toString();
    milestone.data.insert(QStringLiteral("evidence"),
                          spec.value(QStringLiteral("evidence")).toString());
    vmvalidation::RunUpdate update;
    update.milestones.append(milestone);
    vmvalidation::LogDraft log;
    log.occurredAt = milestone.occurredAt;
    log.channel = QStringLiteral("milestone");
    log.message = QStringLiteral("%1: %2").arg(kind, result);
    update.logs.append(log);

    const QString evidencePath = cleanPath(spec.value(QStringLiteral("evidence")).toString());
    const QFileInfo evidenceFile(evidencePath);
    const QString projectRoot = QFileInfo(m_vmValidationStore->projectDirectory()).canonicalFilePath();
    if (evidenceFile.isFile() && !projectRoot.isEmpty()) {
        const QString canonical = evidenceFile.canonicalFilePath();
        const QString relative = QDir(projectRoot).relativeFilePath(canonical);
        vmvalidation::EvidenceDraft evidence;
        const QString suffix = evidenceFile.suffix().toLower();
        evidence.kind = suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg")
                || suffix == QStringLiteral("jpeg")
            ? vmvalidation::EvidenceKind::Screenshot
            : suffix == QStringLiteral("log") || suffix == QStringLiteral("txt")
                ? vmvalidation::EvidenceKind::Log
                : suffix == QStringLiteral("json") || suffix == QStringLiteral("html")
                    ? vmvalidation::EvidenceKind::Report
                    : vmvalidation::EvidenceKind::Other;
        evidence.label = kind;
        evidence.path = canonical;
        evidence.capturedAt = milestone.occurredAt;
        const bool contained = !relative.startsWith(QStringLiteral("../"))
            && relative != QStringLiteral("..");
        if (!contained) {
            evidence.external = true;
            evidence.externalMetadata = QJsonObject{
                {QStringLiteral("selectedBy"), QStringLiteral("desktop-user")},
                {QStringLiteral("capturedFor"), kind},
            };
        }
        update.evidence.append(evidence);
    }
    vmvalidation::MutationResult mutation;
    if (!m_vmValidationStore->updateRun(runId, run->revision, update, &mutation, &error)) {
        updateVmStatus(localized(QStringLiteral("The validation milestone was not saved."),
                                 QStringLiteral("驗證里程碑未能儲存。")),
                       QStringLiteral("error"), error);
        return false;
    }
    refreshVmValidationRuns();
    updateVmStatus(localized(QStringLiteral("Validation milestone saved."),
                             QStringLiteral("驗證里程碑已儲存。")),
                   result == QStringLiteral("fail") ? QStringLiteral("warning")
                                                      : QStringLiteral("success"));
    return true;
}

bool AppController::finishVmValidation(const QString &runId,
                                       const QVariantMap &result)
{
    if (!m_vmValidationStore || m_vmValidationBusy)
        return false;
    QString error;
    const std::optional<vmvalidation::ValidationRun> run = m_vmValidationStore->find(runId, &error);
    if (!run)
        return false;
    const QString resultName = result.value(QStringLiteral("result")).toString().trimmed().toLower();
    vmvalidation::RunStatus status;
    if (resultName == QStringLiteral("pass")) status = vmvalidation::RunStatus::Passed;
    else if (resultName == QStringLiteral("fail")) status = vmvalidation::RunStatus::Failed;
    else if (resultName == QStringLiteral("aborted")
             || resultName == QStringLiteral("cancelled")) {
        status = vmvalidation::RunStatus::Cancelled;
    } else {
        return false;
    }
    const QString notes = result.value(QStringLiteral("notes")).toString().trimmed();
    if ((status == vmvalidation::RunStatus::Failed
         || status == vmvalidation::RunStatus::Cancelled)
        && notes.isEmpty()) {
        updateVmStatus(localized(QStringLiteral("Failed and aborted validation runs require reviewer notes."),
                                 QStringLiteral("失敗同中止嘅驗證執行需要審查備註。")),
                       QStringLiteral("warning"));
        return false;
    }
    if (status == vmvalidation::RunStatus::Passed) {
        const QStringList blockers = validationPassBlockers(*run);
        if (!blockers.isEmpty()) {
            updateVmStatus(localized(QStringLiteral("Validation cannot pass until every required gate has evidence."),
                                     QStringLiteral("所有必要關卡有證據之前，驗證唔可以通過。")),
                           QStringLiteral("warning"), blockers.join(QLatin1Char('\n')));
            return false;
        }
    }
    vmvalidation::MutationResult mutation;
    if (!m_vmValidationStore->completeRun(
            runId, run->revision, status, notes, {}, &mutation, &error)) {
        updateVmStatus(localized(QStringLiteral("The validation result was not saved."),
                                 QStringLiteral("驗證結果未能儲存。")),
                       QStringLiteral("error"), error);
        return false;
    }
    refreshVmValidationRuns();
    updateVmStatus(localized(QStringLiteral("Validation result finalized and locked."),
                             QStringLiteral("驗證結果已完成並鎖定。")),
                   status == vmvalidation::RunStatus::Passed ? QStringLiteral("success")
                                                             : QStringLiteral("warning"));
    return true;
}

bool AppController::cancelVmAction()
{
    if (m_vmValidationBusy) {
        updateVmStatus(localized(QStringLiteral("The initial SHA-256 capture finishes atomically and cannot be cancelled safely."),
                                 QStringLiteral("初始 SHA-256 擷取會原子完成，冇辦法安全取消。")),
                       QStringLiteral("warning"));
        return false;
    }
    return m_vmManager && m_vmManager->cancel();
}

QString AppController::pathFromUrl(const QUrl &url) const
{
    if (url.isLocalFile())
        return QDir::cleanPath(url.toLocalFile());
    return QDir::cleanPath(url.toString(QUrl::PreferLocalFile));
}

bool AppController::openWorkspacePage(int page, const QString &defaultTitle)
{
    QString error;
    if (!m_workspaceTabs.openPage(page, defaultTitle, &error)) {
        showError(error);
        return false;
    }
    emit workspaceTabsChanged();
    return true;
}

bool AppController::activateWorkspaceTab(int index)
{
    QString error;
    if (!m_workspaceTabs.activate(index, &error)) {
        showError(error);
        return false;
    }
    emit workspaceTabsChanged();
    return true;
}

bool AppController::closeWorkspaceTab(int index)
{
    QString error;
    if (!m_workspaceTabs.close(index, &error)) {
        showError(error);
        return false;
    }
    emit workspaceTabsChanged();
    return true;
}

bool AppController::moveWorkspaceTab(int from, int to)
{
    QString error;
    if (!m_workspaceTabs.move(from, to, &error)) {
        showError(error);
        return false;
    }
    emit workspaceTabsChanged();
    return true;
}

bool AppController::updateWorkspaceTab(int index, const QVariantMap &changes)
{
    QString error;
    if (!m_workspaceTabs.update(index, changes, &error)) {
        showError(error);
        return false;
    }
    emit workspaceTabsChanged();
    return true;
}

bool AppController::exportWorkspaceTabs(const QString &destinationFile)
{
    QString error;
    const QString destination = cleanPath(destinationFile);
    if (!m_workspaceTabs.exportTabs(destination, &error)) {
        showError(error);
        return false;
    }
    notify(localized(QStringLiteral("Workspace tabs exported"),
                     QStringLiteral("工作區分頁已匯出")),
           localized(QStringLiteral("Exported to %1").arg(destination),
                     QStringLiteral("已匯出到 %1").arg(destination)),
           QStringLiteral("success"));
    showSuccess(localized(QStringLiteral("Portable workspace tabs exported."),
                          QStringLiteral("可攜工作區分頁已匯出。")));
    return true;
}

bool AppController::importWorkspaceTabs(const QString &sourceFile)
{
    QString error;
    const QString source = cleanPath(sourceFile);
    if (!m_workspaceTabs.importTabs(source, &error)) {
        showError(error);
        return false;
    }
    emit workspaceTabsChanged();
    notify(localized(QStringLiteral("Workspace tabs imported"),
                     QStringLiteral("工作區分頁已匯入")),
           localized(QStringLiteral("Imported from %1").arg(source),
                     QStringLiteral("已由 %1 匯入").arg(source)),
           QStringLiteral("success"));
    showSuccess(localized(
        QStringLiteral("Workspace tabs imported and committed to local history."),
        QStringLiteral("工作區分頁已匯入，亦已 commit 入本機歷史。")));
    return true;
}

bool AppController::exportWorkspaceTabRepository(const QString &destinationFile)
{
    QString error;
    const QString destination = cleanPath(destinationFile);
    if (!m_workspaceTabs.exportRepository(destination, &error)) {
        showError(error);
        return false;
    }
    notify(localized(QStringLiteral("Complete tab repository exported"),
                     QStringLiteral("完整分頁 repository 已匯出")),
           localized(QStringLiteral("Exported to %1").arg(destination),
                     QStringLiteral("已匯出到 %1").arg(destination)),
           QStringLiteral("success"));
    showSuccess(localized(
        QStringLiteral("Complete Git-backed tab repository exported as one file."),
        QStringLiteral("完整 Git-backed 分頁 repository 已匯出成單一檔案。")));
    return true;
}

bool AppController::importWorkspaceTabRepository(const QString &sourceFile)
{
    QString error;
    const QString source = cleanPath(sourceFile);
    if (!m_workspaceTabs.importRepository(source, &error)) {
        showError(error);
        return false;
    }
    emit workspaceTabsChanged();
    notify(localized(QStringLiteral("Complete tab repository imported"),
                     QStringLiteral("完整分頁 repository 已匯入")),
           localized(QStringLiteral("Imported from %1").arg(source),
                     QStringLiteral("已由 %1 匯入").arg(source)),
           QStringLiteral("success"));
    showSuccess(localized(
        QStringLiteral("Tab state and complete local Git history restored."),
        QStringLiteral("分頁狀態同完整本機 Git 歷史已還原。")));
    return true;
}

bool AppController::openApplicationLog()
{
    const QString path = StructuredLogger::instance().logPath();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("controller.action"),
        QStringLiteral("log.open_requested"),
        QStringLiteral("Opening the current application log."));
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        showError(localized(
            QStringLiteral("Windows could not open the application log: %1").arg(path),
            QStringLiteral("Windows 開唔到應用程式 log：%1").arg(path)));
        return false;
    }
    return true;
}

bool AppController::openApplicationLogFolder()
{
    const QString directory = StructuredLogger::instance().logDirectory();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("controller.action"),
        QStringLiteral("log_folder.open_requested"),
        QStringLiteral("Opening the application log folder."));
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
        showError(localized(
            QStringLiteral("Windows could not open the application log folder: %1")
                .arg(directory),
            QStringLiteral("Windows 開唔到應用程式 log 資料夾：%1")
                .arg(directory)));
        return false;
    }
    return true;
}

bool AppController::loadDemoProject(QString *error)
{
    const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("WimForge-Demo"));
    QDir().mkpath(QDir(directory).filePath(QStringLiteral("payloads/drivers")));
    QDir().mkpath(QDir(directory).filePath(QStringLiteral("payloads/updates")));
    auto touch = [](const QString &path) {
        QFile file(path); if (!file.exists() && file.open(QIODevice::WriteOnly)) file.write("WimForge demo payload\n");
    };
    const QString iso = QDir(directory).filePath(QStringLiteral("Windows11-25H2.iso"));
    const QString image = QDir(directory).filePath(QStringLiteral("install.wim"));
    const QString driver = QDir(directory).filePath(QStringLiteral("payloads/drivers/netadapter.inf"));
    const QString update = QDir(directory).filePath(QStringLiteral("payloads/updates/KB-demo.cab"));
    const QString demoRuntime = QDir(directory).filePath(
        QStringLiteral("fixtures/WinForge-runtime/WinForge.exe"));
    QDir().mkpath(QFileInfo(demoRuntime).absolutePath());
    touch(iso); touch(image); touch(driver); touch(update); touch(demoRuntime);

    ProjectConfig project;
    project.projectDirectory = directory;
    project.projectName = QStringLiteral("AI Dev Workstation — 香港版");
    project.description = QStringLiteral("Material Design demo with a reversible Windows 11 image recipe.");
    project.sourcePath = iso; project.imagePath = image;
    project.mountPath = QDir(directory).filePath(QStringLiteral("mount"));
    project.outputPath = QDir(directory).filePath(QStringLiteral("output/AI-Dev-Windows11.iso"));
    project.outputFormat = QStringLiteral("iso"); project.isoLabel = QStringLiteral("WIMFORGE_AI");
    project.drivers = {driver}; project.updates = {update};
    project.featuresToEnable = {QStringLiteral("NetFx3"), QStringLiteral("Microsoft-Windows-Subsystem-Linux"), QStringLiteral("VirtualMachinePlatform")};
    project.appxPackagesToRemove = {QStringLiteral("Microsoft.BingNews_8wekyb3d8bbwe"), QStringLiteral("Microsoft.XboxGamingOverlay_8wekyb3d8bbwe")};
    project.postSetupItems = {QStringLiteral("winget install Git.Git -e --silent"), QStringLiteral("npm install -g opencode-ai@latest @openai/codex")};
    project.settings.insert(QStringLiteral("disableTelemetry"), true);
    project.settings.insert(QStringLiteral("enableLongPaths"), true);
    project.settings.insert(QStringLiteral("_packageProfile"),
                            PackageStudio::toJson(PackageStudio::fullAiDevelopmentTemplate()));
    project.settings.insert(QStringLiteral("_unattendProfile"),
                            UnattendBuilder::aiDevelopmentTemplate().toJson());
    WinForgeRecipe bridgeRecipe = emptyWinForgeRecipe(QStringLiteral("AI development WinForge recipe"));
    for (const QString &page : {QStringLiteral("ai"), QStringLiteral("packages"),
                                QStringLiteral("git")}) {
        WinForgeAction action;
        action.id = QStringLiteral("open-%1").arg(page);
        action.idempotencyKey = action.id;
        action.kind = WinForgeActionKind::Page;
        action.phase = WinForgeActionPhase::User;
        action.enabled = false;
        action.target = page;
        bridgeRecipe.actions.append(action);
    }
    project.settings.insert(QStringLiteral("_winForgeRecipe"),
                            WinForgeBridge::toJson(bridgeRecipe));
    project.settings.insert(QStringLiteral("_winForgeRuntimePath"),
                            QFileInfo(demoRuntime).absolutePath());
    project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), true);
    project.options.cleanupComponentStore = true;
    project.options.extra.insert(QStringLiteral("mediaWorkspace"), QDir(directory).filePath(QStringLiteral("media")));
    project.options.extra.insert(QStringLiteral("imageInventory"), QJsonObject{
        {QStringLiteral("editions"), QJsonArray{
             QStringLiteral("Index 1 — Windows 11 Pro"),
             QStringLiteral("Index 2 — Windows 11 Enterprise")}},
        {QStringLiteral("summaryEn"), QStringLiteral("2 editions · Windows 11 25H2 · amd64")},
        {QStringLiteral("summaryZh"), QStringLiteral("2 個版本 · Windows 11 25H2 · amd64")},
    });
    QString saveError;
    if (!project.save(&saveError, bilingualCommitMessage(
            QStringLiteral("demo: create AI development image recipe"),
            QStringLiteral("示範：建立 AI 開發映像配方")))) {
        setError(error, saveError); return false;
    }
    ActionHistory demoHistory(directory);
    QString actionError;
    if (demoHistory.events(1, &actionError).isEmpty() && actionError.isEmpty()) {
        const QJsonObject finalState = project.toJson();
        struct DemoAction {
            QString context;
            QString element;
            QString title;
            QString icon;
            QString summary;
            std::function<void(QJsonObject &)> makeInverse;
        };
        const QList<DemoAction> actions{
            {QStringLiteral("source"), QStringLiteral("windows-11-25h2"),
             QStringLiteral("source: select Windows 11 25H2 image / 來源：揀 Windows 11 25H2 映像"),
             QStringLiteral("image"),
             QStringLiteral("Source and edition selection / 來源同版本選擇"), [](QJsonObject &state) {
                 QJsonObject paths = state.value(QStringLiteral("paths")).toObject();
                 paths.insert(QStringLiteral("source"), QString());
                 paths.insert(QStringLiteral("image"), QString());
                 state.insert(QStringLiteral("paths"), paths);
            }},
            {QStringLiteral("gpo"), QStringLiteral("enable-long-paths"),
             QStringLiteral("gpo: enable Win32 long paths / GPO：啟用 Win32 長路徑"),
             QStringLiteral("policy"),
             QStringLiteral("HKLM policy changed from Not Configured to Enabled / HKLM 原則由未設定改做啟用"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.insert(QStringLiteral("enableLongPaths"), false);
                 state.insert(QStringLiteral("settings"), settings);
            }},
            {QStringLiteral("packages"), QStringLiteral("full-ai-development"),
             QStringLiteral("packages: select Full AI Development ISO / 套件：揀完整 AI 開發 ISO"),
             QStringLiteral("inventory_2"),
             QStringLiteral("20 enabled tools plus optional desktop payloads / 20 個已啟用工具，加埋可選桌面 payload"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.remove(QStringLiteral("_packageProfile"));
                 state.insert(QStringLiteral("settings"), settings);
            }},
            {QStringLiteral("unattended"), QStringLiteral("ai-development"),
             QStringLiteral("unattended: apply AI development template / 無人值守：套用 AI 開發範本"),
             QStringLiteral("auto_fix_high"),
             QStringLiteral("Answer file and prompt-safe computer naming / 回應檔同安全提示電腦命名"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.remove(QStringLiteral("_unattendProfile"));
                 state.insert(QStringLiteral("settings"), settings);
            }},
            {QStringLiteral("winforge"), QStringLiteral("ai-tools"),
             QStringLiteral("winforge: draft post-install AI tool pages / WinForge：草擬安裝後 AI 工具頁"),
             QStringLiteral("account_tree"),
             QStringLiteral("Three contract-checked page actions / 三個已通過合約檢查嘅頁面動作"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.remove(QStringLiteral("_winForgeRecipe"));
                 state.insert(QStringLiteral("settings"), settings);
             }},
        };
        for (const DemoAction &demo : actions) {
            QJsonObject inverse = finalState;
            demo.makeInverse(inverse);
            ActionDraft draft;
            draft.contextKey = demo.context;
            draft.elementId = demo.element;
            draft.title = demo.title;
            draft.description = demo.summary;
            draft.icon = demo.icon;
            draft.forwardDiff = finalState;
            draft.inverseDiff = inverse;
            draft.metadata = QJsonObject{
                {QStringLiteral("diffSummary"), demo.summary},
                {QStringLiteral("fullProjectState"), true},
                {QStringLiteral("demo"), true},
            };
            if (!demoHistory.record(draft, nullptr, &actionError)) {
                setError(error, actionError);
                return false;
            }
        }
    }
    m_project = std::move(project);
    loadProjectState();
    notify(QStringLiteral("Demo recipe ready"), QStringLiteral("No real image will be modified. Explore the Material UI, plan, Git history and notification actions."), QStringLiteral("success"));
    setError(error, {});
    return true;
}

void AppController::reloadPayloadCatalog(bool force)
{
    const QStringList driverPaths = m_project ? m_project->drivers : QStringList();
    const QStringList updatePaths = m_project ? m_project->updates : QStringList();
    if (!force && driverPaths == m_catalogDriverPaths
        && updatePaths == m_catalogUpdatePaths) {
        return;
    }

    m_catalogDriverPaths = driverPaths;
    m_catalogUpdatePaths = updatePaths;
    m_driverCatalogItems.clear();
    m_updateCatalogItems.clear();
    for (const ServicingPayloadEntry &entry : PayloadCatalog::inspectAll(
             driverPaths, ServicingPayloadKind::Driver)) {
        m_driverCatalogItems.append(payloadCatalogVariant(entry));
    }
    for (const ServicingPayloadEntry &entry : PayloadCatalog::inspectAll(
             updatePaths, ServicingPayloadKind::Update)) {
        m_updateCatalogItems.append(payloadCatalogVariant(entry));
    }
    emit payloadCatalogChanged();
}

void AppController::loadProjectState()
{
    m_editionNames.clear();
    m_imageSummaryEn = QStringLiteral("Inspect a source to load edition metadata.");
    m_imageSummaryZh = QStringLiteral("檢查來源之後，就會載入映像版本資料。");
    if (m_project) {
        const QJsonObject inventory = m_project->options.extra
            .value(QStringLiteral("imageInventory")).toObject();
        const QJsonArray editions = inventory.value(QStringLiteral("editions")).toArray();
        for (const QJsonValue &edition : editions) {
            if (edition.isString() && !edition.toString().trimmed().isEmpty())
                m_editionNames.append(edition.toString());
        }
        if (!m_editionNames.isEmpty()) {
            m_project->selectedImageIndex = qBound(
                1, m_project->selectedImageIndex,
                static_cast<int>(m_editionNames.size()));
        }
        const QString summaryEn = inventory.value(QStringLiteral("summaryEn"))
                                      .toString().trimmed();
        const QString summaryZh = inventory.value(QStringLiteral("summaryZh"))
                                      .toString().trimmed();
        if (!m_editionNames.isEmpty() && !summaryEn.isEmpty())
            m_imageSummaryEn = summaryEn;
        if (!m_editionNames.isEmpty() && !summaryZh.isEmpty())
            m_imageSummaryZh = summaryZh;
        else if (!m_editionNames.isEmpty())
            m_imageSummaryZh = m_imageSummaryEn;
    }
    if (m_project) {
        const QString notificationPath = m_project->settings
            .value(QStringLiteral("_notificationRepoPath")).toString();
        if (!notificationPath.isEmpty()
            && QDir::cleanPath(notificationPath) != QDir::cleanPath(m_notificationStore.storeDirectory())) {
            QString error;
            NotificationStore projectStore(notificationPath);
            if (projectStore.initialize(&error)) {
                m_notificationStore = projectStore;
                refreshNotifications();
            }
        }
    }
    if (m_project) {
        QString tabError;
        if (!m_workspaceTabs.openProject(m_project->projectDirectory, &tabError))
            showError(localized(
                QStringLiteral("Workspace tabs could not be opened: %1").arg(tabError),
                QStringLiteral("開唔到工作區分頁：%1").arg(tabError)));
        emit workspaceTabsChanged();
    }
    restoreStudioState();
    if (m_winForgeRuntimePath.isEmpty()) {
        m_winForgeRuntimeContract = {};
        m_winForgeRuntimeStatus = QStringLiteral("Choose a WinForge runtime folder to detect its contract.");
    } else {
        QString bridgeError;
        m_winForgeRuntimeContract = WinForgeBridge::detectRuntimeContract(
            m_winForgeRuntimePath, &bridgeError);
        m_winForgeRuntimeStatus = m_winForgeRuntimeContract.runtimeFound
            ? QStringLiteral("%1 runtime · contract %2 · %3")
                  .arg(m_winForgeRuntimeContract.declaredContract ? QStringLiteral("Declared")
                                                                  : QStringLiteral("Legacy"))
                  .arg(m_winForgeRuntimeContract.contractVersion)
                  .arg(m_winForgeRuntimeContract.capabilities.join(QStringLiteral(", ")))
            : bridgeError;
    }
    reloadPayloadCatalog();
    refreshPlan(); refreshHistory(); refreshRecoveryState(); updateWatcher();
    recreateVmLab();
    emit stateChanged();
    emit studioChanged();
}

void AppController::refreshNotifications()
{
    QString error;
    m_notificationItems = m_notificationStore.list(true, true, &error);
    if (!error.isEmpty()) emit snackbarRequested(error, QStringLiteral("error"));
    emit notificationsChanged();
}

void AppController::refreshRecoveryState()
{
    m_pendingRecovery = false; m_recoveryJournal = {};
    if (m_project) {
        QString error;
        m_pendingRecovery = JobEngine::hasInterruptedRun(m_project->projectDirectory, &m_recoveryJournal, &error);
        if (!error.isEmpty()) emit snackbarRequested(error, QStringLiteral("warning"));
    }
}

void AppController::updateWatcher()
{
    if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
    if (m_project && m_project->autoImport && QFileInfo::exists(m_project->projectFilePath()))
        m_watcher.addPath(m_project->projectFilePath());
}

void AppController::onWatchedProjectChanged(const QString &path)
{
    if (!m_project || !m_project->autoImport) return;
    const QString directory = m_project->projectDirectory;
    QTimer::singleShot(250, this, [this, directory, path] {
        QString error;
        const auto updated = ProjectConfig::load(directory, &error);
        if (!updated) { showError(error); updateWatcher(); return; }
        m_project = *updated; loadProjectState();
        notify(localized(QStringLiteral("External config imported"),
                         QStringLiteral("外部設定已匯入")),
               localized(QStringLiteral("Imported from %1").arg(path),
                         QStringLiteral("已由 %1 匯入").arg(path)),
               QStringLiteral("info"));
    });
}

void AppController::notify(const QString &title, const QString &message, const QString &severity)
{
    StructuredLogger::instance().log(
        severity == QStringLiteral("error") ? LogSeverity::Error
            : severity == QStringLiteral("warning") ? LogSeverity::Warning
                                                    : LogSeverity::Info,
        QStringLiteral("notification"), QStringLiteral("notification.created"),
        QStringLiteral("A user-visible notification was created."),
        QJsonObject{{QStringLiteral("titleLength"), title.size()},
                    {QStringLiteral("messageLength"), message.size()},
                    {QStringLiteral("notificationSeverity"), severity}});
    QString error;
    m_notificationStore.addNotification(title, message, severity, QStringLiteral("WimForge"), {}, &error);
    if (!error.isEmpty()) emit snackbarRequested(error, QStringLiteral("error"));
    refreshNotifications();
}

void AppController::showError(const QString &message)
{
    if (message.trimmed().isEmpty()) return;
    StructuredLogger::instance().log(
        LogSeverity::Error, QStringLiteral("controller.action"),
        QStringLiteral("action.failed"),
        QStringLiteral("A user-visible action failed."),
        QJsonObject{{QStringLiteral("projectLoaded"), projectLoaded()},
                    {QStringLiteral("messageLength"), message.trimmed().size()}});
    m_statusText = message.trimmed();
    emit snackbarRequested(message.trimmed(), QStringLiteral("error"));
    QString notificationError;
    m_notificationStore.addNotification(
        localized(QStringLiteral("Action needs attention"),
                  QStringLiteral("呢個動作要處理")),
        message.trimmed(), QStringLiteral("error"),
        QStringLiteral("WimForge"), {}, &notificationError);
    if (notificationError.isEmpty())
        refreshNotifications();
    emit stateChanged();
}

void AppController::showSuccess(const QString &message)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("controller.action"),
        QStringLiteral("action.succeeded"),
        QStringLiteral("A user-visible action succeeded."),
        QJsonObject{{QStringLiteral("projectLoaded"), projectLoaded()},
                    {QStringLiteral("messageLength"), message.size()}});
    m_statusText = message;
    emit snackbarRequested(message, QStringLiteral("success"));
    emit stateChanged();
}

QString AppController::localized(const QString &en, const QString &zh) const
{
    if (m_languageMode == 1) return zh;
    if (m_languageMode == 2) return en + QStringLiteral("  ·  ") + zh;
    return en;
}
