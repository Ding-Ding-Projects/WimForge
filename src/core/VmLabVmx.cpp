#include "VmLabVmx.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>

#include <algorithm>
#include <utility>

namespace wimforge::vmlab {
namespace {

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

bool validKey(const QString &key)
{
    static const QRegularExpression expression(
        QStringLiteral("^[A-Za-z0-9_.:-]+$"), QRegularExpression::UseUnicodePropertiesOption);
    return expression.match(key).hasMatch();
}

QString unescapeValue(const QString &escaped)
{
    QString value;
    value.reserve(escaped.size());
    for (qsizetype index = 0; index < escaped.size(); ++index) {
        const QChar character = escaped.at(index);
        if (character != QLatin1Char('\\') || index + 1 >= escaped.size()) {
            value.append(character);
            continue;
        }
        const QChar next = escaped.at(++index);
        if (next == QLatin1Char('n'))
            value.append(QLatin1Char('\n'));
        else if (next == QLatin1Char('r'))
            value.append(QLatin1Char('\r'));
        else if (next == QLatin1Char('\\') || next == QLatin1Char('"'))
            value.append(next);
        else {
            // Existing VMX files commonly contain literal Windows paths. Do
            // not silently turn an unknown sequence such as "\\U" into "U".
            value.append(QLatin1Char('\\'));
            value.append(next);
        }
    }
    return value;
}

QString escapeValue(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return value;
}

bool setChecked(VmxDocument &document, const QString &key, const QString &value, QString *error)
{
    if (!document.setValue(key, value, error))
        return false;
    return true;
}

QString vmwareNetworkType(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::Nat: return QStringLiteral("nat");
    case NetworkMode::Bridged: return QStringLiteral("bridged");
    case NetworkMode::HostOnly: return QStringLiteral("hostonly");
    case NetworkMode::Internal: return QStringLiteral("custom");
    case NetworkMode::Disconnected: return QStringLiteral("nat");
    }
    return QStringLiteral("nat");
}

} // namespace

std::optional<VmxDocument> VmxDocument::parse(const QByteArray &bytes, QString *error)
{
    if (bytes.contains('\0')) {
        setError(error, QStringLiteral("VMX contains a NUL byte."));
        return std::nullopt;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        setError(error, QStringLiteral("VMX must be valid UTF-8."));
        return std::nullopt;
    }
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);

    VmxDocument document;
    document.m_lineEnding = bytes.contains("\r\n") ? QByteArray("\r\n") : QByteArray("\n");
    document.m_finalNewline = text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r'));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (document.m_finalNewline && !lines.isEmpty() && lines.constLast().isEmpty())
        lines.removeLast();

    static const QRegularExpression assignment(
        QStringLiteral("^\\s*([A-Za-z0-9_.:-]+)\\s*=\\s*\"((?:\\\\.|[^\"])*)\"\\s*$"));
    for (qsizetype index = 0; index < lines.size(); ++index) {
        Line line;
        line.raw = lines.at(index);
        const QString trimmed = line.raw.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('#'))
            && !trimmed.startsWith(QLatin1Char(';')) && !trimmed.startsWith(QStringLiteral("//"))) {
            const QRegularExpressionMatch match = assignment.match(line.raw);
            if (match.hasMatch()) {
                line.assignment = true;
                line.key = match.captured(1);
                line.value = unescapeValue(match.captured(2));
            } else if (line.raw.contains(QLatin1Char('='))) {
                setError(error, QStringLiteral("Malformed VMX assignment on line %1.").arg(index + 1));
                return std::nullopt;
            }
        }
        document.m_lines.append(std::move(line));
    }
    setError(error, {});
    return document;
}

std::optional<VmxDocument> VmxDocument::load(const QString &path, QString *error)
{
    if (!QFileInfo(path).isAbsolute()) {
        setError(error, QStringLiteral("VMX path must be absolute."));
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return std::nullopt;
    }
    return parse(file.readAll(), error);
}

std::optional<VmxDocument> VmxDocument::fromCreateSpec(const CreateSpec &spec,
                                                       const QString &diskPath,
                                                       QString *error)
{
    if ((spec.providerId != vmwareWorkstationProviderId()
         && spec.providerId != vmwarePlayerProviderId())
        || !isSafeMachineFileStem(spec.name) || spec.guestType.trimmed().isEmpty()
        || !QFileInfo(spec.directory).isAbsolute() || !QFileInfo(diskPath).isAbsolute()) {
        setError(error, QStringLiteral("VMware create specification is incomplete."));
        return std::nullopt;
    }
    if (spec.cpuCount < 1 || spec.cpuCount > 64 || spec.memoryMiB < 256) {
        setError(error, QStringLiteral("VMware CPU or memory setting is outside the safe range."));
        return std::nullopt;
    }
    if (spec.secureBoot && spec.firmware != Firmware::Efi) {
        setError(error, QStringLiteral("Secure Boot requires EFI firmware."));
        return std::nullopt;
    }
    const int hardwareVersion = spec.virtualHardwareVersion > 0
        ? spec.virtualHardwareVersion : 10;
    if (spec.secureBoot && hardwareVersion < 14) {
        setError(error, QStringLiteral("VMware Secure Boot requires virtual hardware version 14 or newer."));
        return std::nullopt;
    }
    if (spec.tpm) {
        setError(error, QStringLiteral("Automatic VMware TPM creation is unavailable without a declared encrypted-VM capability."));
        return std::nullopt;
    }
    if (!spec.isoPath.isEmpty() && !QFileInfo(spec.isoPath).isAbsolute()) {
        setError(error, QStringLiteral("VMware ISO path must be absolute."));
        return std::nullopt;
    }

    VmxDocument document;
    document.m_lines = {
        {QStringLiteral("# Generated atomically by WimForge VM Lab"), {}, {}, false},
        {QStringLiteral(".encoding = \"UTF-8\""), QStringLiteral(".encoding"), QStringLiteral("UTF-8"), true},
    };
    const QList<QPair<QString, QString>> values{
        {QStringLiteral("config.version"), QStringLiteral("8")},
        {QStringLiteral("virtualHW.version"), QString::number(hardwareVersion)},
        {QStringLiteral("displayName"), spec.name},
        {QStringLiteral("guestOS"), spec.guestType},
        {QStringLiteral("memsize"), QString::number(spec.memoryMiB)},
        {QStringLiteral("numvcpus"), QString::number(spec.cpuCount)},
        {QStringLiteral("firmware"), firmwareName(spec.firmware)},
        {QStringLiteral("uefi.secureBoot.enabled"), spec.secureBoot ? QStringLiteral("TRUE")
                                                                   : QStringLiteral("FALSE")},
        {QStringLiteral("sata0.present"), QStringLiteral("TRUE")},
        {QStringLiteral("sata0:0.present"), QStringLiteral("TRUE")},
        {QStringLiteral("sata0:0.fileName"), diskPath},
        {QStringLiteral("ethernet0.present"), QStringLiteral("TRUE")},
        {QStringLiteral("ethernet0.connectionType"), vmwareNetworkType(spec.networkMode)},
        {QStringLiteral("ethernet0.startConnected"),
         spec.networkMode == NetworkMode::Disconnected ? QStringLiteral("FALSE")
                                                        : QStringLiteral("TRUE")},
    };
    for (const auto &entry : values)
        if (!document.setValue(entry.first, entry.second, error))
            return std::nullopt;
    if (spec.networkMode == NetworkMode::Internal && !spec.bridgedInterface.isEmpty())
        document.setValue(QStringLiteral("ethernet0.vnet"), spec.bridgedInterface, error);
    if (!spec.isoPath.isEmpty()) {
        document.setValue(QStringLiteral("sata0:1.present"), QStringLiteral("TRUE"), error);
        document.setValue(QStringLiteral("sata0:1.deviceType"), QStringLiteral("cdrom-image"), error);
        document.setValue(QStringLiteral("sata0:1.fileName"), spec.isoPath, error);
        document.setValue(QStringLiteral("sata0:1.startConnected"), QStringLiteral("TRUE"), error);
    }
    if (spec.unattendedBoot)
        document.setValue(QStringLiteral("bios.bootOrder"), QStringLiteral("cdrom,hdd"), error);
    setError(error, {});
    return document;
}

QString VmxDocument::value(const QString &key) const
{
    for (auto iterator = m_lines.crbegin(); iterator != m_lines.crend(); ++iterator)
        if (iterator->assignment && iterator->key.compare(key, Qt::CaseInsensitive) == 0)
            return iterator->value;
    return {};
}

bool VmxDocument::contains(const QString &key) const
{
    return std::any_of(m_lines.cbegin(), m_lines.cend(), [&key](const Line &line) {
        return line.assignment && line.key.compare(key, Qt::CaseInsensitive) == 0;
    });
}

QStringList VmxDocument::storagePaths(const QString &baseDirectory) const
{
    QHash<QString, QString> values;
    for (const Line &line : m_lines) {
        if (line.assignment)
            values.insert(line.key.toLower(), line.value);
    }
    static const QRegularExpression fileKey(
        QStringLiteral("^(?:ide|sata|scsi|nvme)[0-9]+:[0-9]+\\.filename$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression diskExtension(
        QStringLiteral("\\.(?:vmdk|vhd|vhdx|vdi|hdd|qcow2?|raw)$"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList result;
    for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
        if (!fileKey.match(iterator.key()).hasMatch())
            continue;
        const QString prefix = iterator.key().left(iterator.key().size()
                                                   - QStringLiteral(".filename").size());
        if (values.value(prefix + QStringLiteral(".present"))
                .compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0
            || values.value(prefix + QStringLiteral(".devicetype"))
                   .contains(QStringLiteral("cdrom"), Qt::CaseInsensitive)
            || !diskExtension.match(iterator.value()).hasMatch()) {
            continue;
        }
        const QFileInfo storage(iterator.value());
        const QString absolute = storage.isAbsolute()
            ? storage.absoluteFilePath()
            : QFileInfo(QDir(baseDirectory).filePath(iterator.value())).absoluteFilePath();
        if (!result.contains(absolute, Qt::CaseInsensitive))
            result.append(absolute);
    }
    return result;
}

bool VmxDocument::setValue(const QString &key, const QString &value, QString *error)
{
    if (!validKey(key) || value.contains(QChar::Null) || value.contains(QLatin1Char('\r'))
        || value.contains(QLatin1Char('\n'))) {
        setError(error, QStringLiteral("VMX key or value is unsafe."));
        return false;
    }
    const QString raw = QStringLiteral("%1 = \"%2\"").arg(key, escapeValue(value));
    for (auto iterator = m_lines.rbegin(); iterator != m_lines.rend(); ++iterator) {
        if (!iterator->assignment || iterator->key.compare(key, Qt::CaseInsensitive) != 0)
            continue;
        iterator->raw = raw;
        iterator->key = key;
        iterator->value = value;
        setError(error, {});
        return true;
    }
    m_lines.append(Line{raw, key, value, true});
    setError(error, {});
    return true;
}

bool VmxDocument::remove(const QString &key)
{
    const qsizetype before = m_lines.size();
    m_lines.erase(std::remove_if(m_lines.begin(), m_lines.end(), [&key](const Line &line) {
        return line.assignment && line.key.compare(key, Qt::CaseInsensitive) == 0;
    }), m_lines.end());
    return before != m_lines.size();
}

QByteArray VmxDocument::serialize() const
{
    QByteArray result;
    for (qsizetype index = 0; index < m_lines.size(); ++index) {
        result += m_lines.at(index).raw.toUtf8();
        if (index + 1 < m_lines.size() || m_finalNewline)
            result += m_lineEnding;
    }
    return result;
}

bool VmxDocument::saveAtomic(const QString &path, QString *error) const
{
    if (!QFileInfo(path).isAbsolute()) {
        setError(error, QStringLiteral("VMX path must be absolute."));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("Could not create VMX directory."));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    const QByteArray bytes = serialize();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    setError(error, {});
    return true;
}

bool applyConfigPatch(VmxDocument &document, const ConfigPatch &patch, QString *error)
{
    if (patch.cpuCount && (*patch.cpuCount < 1 || *patch.cpuCount > 64)) {
        setError(error, QStringLiteral("CPU count must be between 1 and 64."));
        return false;
    }
    if (patch.memoryMiB && (*patch.memoryMiB < 256 || *patch.memoryMiB > 1024 * 1024)) {
        setError(error, QStringLiteral("Memory must be between 256 MiB and 1 TiB."));
        return false;
    }
    if (patch.tpm && *patch.tpm) {
        setError(error, QStringLiteral("VMware TPM edits require a declared encrypted-VM capability."));
        return false;
    }
    const Firmware effectiveFirmware = patch.firmware.value_or(
        document.value(QStringLiteral("firmware")).compare(QStringLiteral("efi"), Qt::CaseInsensitive) == 0
            ? Firmware::Efi : Firmware::Bios);
    if (patch.secureBoot && *patch.secureBoot && effectiveFirmware != Firmware::Efi) {
        setError(error, QStringLiteral("Secure Boot requires EFI firmware."));
        return false;
    }
    const bool existingSecureBoot = document.value(QStringLiteral("uefi.secureBoot.enabled"))
        .compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    if (effectiveFirmware == Firmware::Bios
        && patch.secureBoot.value_or(existingSecureBoot)) {
        setError(error, QStringLiteral("Disable Secure Boot in the same reviewed patch before switching to BIOS."));
        return false;
    }
    if (patch.cpuCount
        && !setChecked(document, QStringLiteral("numvcpus"), QString::number(*patch.cpuCount), error))
        return false;
    if (patch.memoryMiB
        && !setChecked(document, QStringLiteral("memsize"), QString::number(*patch.memoryMiB), error))
        return false;
    if (patch.firmware
        && !setChecked(document, QStringLiteral("firmware"), firmwareName(*patch.firmware), error))
        return false;
    if (patch.secureBoot
        && !setChecked(document, QStringLiteral("uefi.secureBoot.enabled"),
                       *patch.secureBoot ? QStringLiteral("TRUE") : QStringLiteral("FALSE"), error))
        return false;
    if (patch.networkMode) {
        if (!setChecked(document, QStringLiteral("ethernet0.connectionType"),
                        vmwareNetworkType(*patch.networkMode), error)
            || !setChecked(document, QStringLiteral("ethernet0.startConnected"),
                           *patch.networkMode == NetworkMode::Disconnected ? QStringLiteral("FALSE")
                                                                          : QStringLiteral("TRUE"), error))
            return false;
    }
    if (patch.bridgedInterface) {
        if (patch.bridgedInterface->isEmpty())
            document.remove(QStringLiteral("ethernet0.vnet"));
        else {
            if (!patch.networkMode
                || (*patch.networkMode != NetworkMode::Bridged
                    && *patch.networkMode != NetworkMode::HostOnly
                    && *patch.networkMode != NetworkMode::Internal)) {
                setError(error, QStringLiteral("A VMware vnet requires an explicit bridged, host-only, or internal mode."));
                return false;
            }
            if (!setChecked(document, QStringLiteral("ethernet0.vnet"),
                            *patch.bridgedInterface, error))
                return false;
        }
    }
    if (patch.isoPath) {
        if (patch.isoPath->isEmpty()) {
            if (!setChecked(document, QStringLiteral("sata0:1.present"), QStringLiteral("FALSE"), error))
                return false;
            document.remove(QStringLiteral("sata0:1.deviceType"));
            document.remove(QStringLiteral("sata0:1.fileName"));
            document.remove(QStringLiteral("sata0:1.startConnected"));
        } else {
            if (!QFileInfo(*patch.isoPath).isAbsolute()) {
                setError(error, QStringLiteral("ISO path must be absolute."));
                return false;
            }
            if (!setChecked(document, QStringLiteral("sata0:1.present"), QStringLiteral("TRUE"), error)
                || !setChecked(document, QStringLiteral("sata0:1.deviceType"), QStringLiteral("cdrom-image"), error)
                || !setChecked(document, QStringLiteral("sata0:1.fileName"), *patch.isoPath, error)
                || !setChecked(document, QStringLiteral("sata0:1.startConnected"), QStringLiteral("TRUE"), error))
                return false;
        }
    }
    setError(error, {});
    return true;
}

} // namespace wimforge::vmlab
