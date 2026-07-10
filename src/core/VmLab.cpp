#include "VmLab.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wimforge::vmlab {
namespace {

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString cleanAbsolute(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool samePath(const QString &left, const QString &right)
{
    return QDir::fromNativeSeparators(cleanAbsolute(left)).compare(
               QDir::fromNativeSeparators(cleanAbsolute(right)), pathCaseSensitivity()) == 0;
}

bool containedPath(const QString &root, const QString &candidate)
{
    const QString normalizedRoot = QDir::fromNativeSeparators(cleanAbsolute(root));
    const QString normalizedCandidate = QDir::fromNativeSeparators(cleanAbsolute(candidate));
    if (normalizedRoot.isEmpty() || normalizedCandidate.isEmpty())
        return false;
    return normalizedCandidate.startsWith(normalizedRoot + QLatin1Char('/'), pathCaseSensitivity());
}

#ifdef Q_OS_WIN

class ScopedWinHandle
{
public:
    explicit ScopedWinHandle(HANDLE value = INVALID_HANDLE_VALUE) : m_value(value) {}
    ~ScopedWinHandle()
    {
        if (m_value != INVALID_HANDLE_VALUE)
            CloseHandle(m_value);
    }
    ScopedWinHandle(const ScopedWinHandle &) = delete;
    ScopedWinHandle &operator=(const ScopedWinHandle &) = delete;
    [[nodiscard]] HANDLE get() const { return m_value; }
    [[nodiscard]] bool valid() const { return m_value != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_value;
};

QString winApiPath(const QString &path)
{
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

QString winErrorMessage(const QString &operation, DWORD code = GetLastError())
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    const QString detail = length > 0 && buffer
        ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Windows error %1").arg(code);
    if (buffer)
        LocalFree(buffer);
    return QStringLiteral("%1: %2").arg(operation, detail);
}

QString identityForHandle(HANDLE handle, QString *error)
{
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        setError(error, winErrorMessage(QStringLiteral("Could not read file identity")));
        return {};
    }
    const quint64 fileId = (static_cast<quint64>(information.nFileIndexHigh) << 32)
        | information.nFileIndexLow;
    setError(error, {});
    return QStringLiteral("%1:%2")
        .arg(information.dwVolumeSerialNumber, 8, 16, QLatin1Char('0'))
        .arg(fileId, 16, 16, QLatin1Char('0'))
        .toLower();
}

bool queryHandleAttributes(HANDLE handle, FILE_ATTRIBUTE_TAG_INFO *attributes,
                           QString *error)
{
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, attributes,
                                      sizeof(*attributes))) {
        setError(error, winErrorMessage(QStringLiteral("Could not read file attributes")));
        return false;
    }
    if (attributes->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        setError(error, QStringLiteral("Managed deletion refuses Windows reparse tag 0x%1.")
                            .arg(attributes->ReparseTag, 8, 16, QLatin1Char('0')));
        return false;
    }
    return true;
}

ScopedWinHandle openNoFollowForDelete(const QString &path, QString *error)
{
    const QString native = winApiPath(path);
    const HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        setError(error, winErrorMessage(QStringLiteral("Could not bind managed path %1").arg(path)));
    return ScopedWinHandle(handle);
}

bool markHandleForDeletion(HANDLE handle, QString *error)
{
#ifdef FILE_DISPOSITION_FLAG_DELETE
    FILE_DISPOSITION_INFO_EX extendedDisposition{};
    extendedDisposition.Flags = FILE_DISPOSITION_FLAG_DELETE
        | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
        | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (SetFileInformationByHandle(handle, FileDispositionInfoEx, &extendedDisposition,
                                   sizeof(extendedDisposition))) {
        return true;
    }
#endif
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        && (basic.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic));
    }
    FILE_DISPOSITION_INFO basicDisposition{};
    basicDisposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &basicDisposition,
                                    sizeof(basicDisposition))) {
        setError(error, winErrorMessage(QStringLiteral("Could not delete bound managed path")));
        return false;
    }
    return true;
}

bool deleteTreeNoFollowWindows(const QString &path, const QString &expectedIdentity,
                               QString *error)
{
    ScopedWinHandle handle = openNoFollowForDelete(path, error);
    if (!handle.valid())
        return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!queryHandleAttributes(handle.get(), &attributes, error))
        return false;
    const QString identity = identityForHandle(handle.get(), error);
    if (identity.isEmpty())
        return false;
    if (!expectedIdentity.isEmpty() && identity != expectedIdentity) {
        setError(error, QStringLiteral("Managed path identity changed after preview."));
        return false;
    }

    if (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        const QString searchPath = winApiPath(QDir(path).filePath(QStringLiteral("*")));
        WIN32_FIND_DATAW entry{};
        const HANDLE find = FindFirstFileW(
            reinterpret_cast<LPCWSTR>(searchPath.utf16()), &entry);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) {
                setError(error, winErrorMessage(
                                    QStringLiteral("Could not enumerate managed directory %1").arg(path),
                                    code));
                return false;
            }
        } else {
            bool ok = true;
            do {
                const QString name = QString::fromWCharArray(entry.cFileName);
                if (name == QStringLiteral(".") || name == QStringLiteral(".."))
                    continue;
                if (!deleteTreeNoFollowWindows(QDir(path).filePath(name), {}, error)) {
                    ok = false;
                    break;
                }
            } while (FindNextFileW(find, &entry));
            const DWORD enumerationError = GetLastError();
            FindClose(find);
            if (!ok)
                return false;
            if (enumerationError != ERROR_NO_MORE_FILES) {
                setError(error, winErrorMessage(
                                    QStringLiteral("Managed directory enumeration failed"),
                                    enumerationError));
                return false;
            }
        }
    }
    if (!markHandleForDeletion(handle.get(), error))
        return false;
    setError(error, {});
    return true;
}

QString pathIdentity(const QString &path, QString *error)
{
    const QString native = winApiPath(path);
    ScopedWinHandle handle(CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        setError(error, winErrorMessage(QStringLiteral("Could not read managed path identity")));
        return {};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!queryHandleAttributes(handle.get(), &attributes, error))
        return {};
    return identityForHandle(handle.get(), error);
}

#else

QString pathIdentity(const QString &path, QString *error)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        setError(error, QStringLiteral("Could not resolve managed path identity."));
        return {};
    }
    setError(error, {});
    return QStringLiteral("%1:%2:%3")
        .arg(canonical)
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(info.size());
}

#endif

QString machineDirectory(const Machine &machine)
{
    const QFileInfo config(machine.configPath);
    return config.isDir() ? config.absoluteFilePath() : config.absolutePath();
}

bool hasReparsePoint(const QString &root, const QString &directory, QString *detail)
{
    QDir rootDir(root);
    const QString relative = rootDir.relativeFilePath(directory);
    QString cursor = root;
    for (const QString &segment : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        cursor = QDir(cursor).filePath(segment);
        const QFileInfo info(cursor);
        if (info.isSymLink()
#ifdef Q_OS_WIN
            || info.isJunction()
#endif
        ) {
            setError(detail, QStringLiteral("Managed deletion refuses reparse point: %1").arg(cursor));
            return true;
        }
    }
    return false;
}

bool containsReparsePoint(const QString &directory, QString *detail)
{
    QDirIterator iterator(directory,
                          QDir::AllEntries | QDir::Hidden | QDir::System
                              | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isDir() && info.fileName().endsWith(QStringLiteral(".lck"),
                                                     Qt::CaseInsensitive)) {
            setError(detail, QStringLiteral("Managed deletion refuses active provider lock: %1")
                                 .arg(info.absoluteFilePath()));
            return true;
        }
        if (info.isSymLink()
#ifdef Q_OS_WIN
            || info.isJunction()
#endif
        ) {
            setError(detail, QStringLiteral("Managed deletion refuses nested reparse point: %1")
                                 .arg(info.absoluteFilePath()));
            return true;
        }
    }
    return false;
}

QJsonObject machineJson(const Machine &machine)
{
    return QJsonObject{
        {QStringLiteral("providerId"), machine.ref.providerId},
        {QStringLiteral("id"), machine.ref.id},
        {QStringLiteral("name"), machine.ref.name},
        {QStringLiteral("configPath"), machine.configPath},
        {QStringLiteral("ownership"), ownershipName(machine.ownership)},
    };
}

QJsonObject catalogJson(const QList<Machine> &machines)
{
    QJsonArray entries;
    for (const Machine &machine : machines)
        entries.append(machineJson(machine));
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("wimforge.vm-catalog")},
        {QStringLiteral("version"), Catalog::CurrentVersion},
        {QStringLiteral("machines"), entries},
    };
}

bool writeAtomic(const AtomicWrite &write, QString *error)
{
    if (!QFileInfo(write.path).isAbsolute()) {
        setError(error, QStringLiteral("Atomic write destination must be absolute: %1").arg(write.path));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(write.path).absolutePath())) {
        setError(error, QStringLiteral("Could not create VM configuration directory."));
        return false;
    }
    QSaveFile file(write.path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    if (file.write(write.contents) != write.contents.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    setError(error, {});
    return true;
}

bool atomicWriteRevisionMatches(const AtomicWrite &write, QString *error)
{
    const bool exists = QFileInfo::exists(write.path);
    if (write.expectedSha256.isEmpty()) {
        if (exists) {
            setError(error, QStringLiteral("VM configuration appeared after preview: %1").arg(write.path));
            return false;
        }
        return true;
    }
    if (!exists) {
        setError(error, QStringLiteral("VM configuration disappeared after preview: %1").arg(write.path));
        return false;
    }
    QString hashError;
    const QString actual = fileSha256(write.path, &hashError);
    if (!hashError.isEmpty()) {
        setError(error, hashError);
        return false;
    }
    if (actual.compare(write.expectedSha256, Qt::CaseInsensitive) != 0) {
        setError(error, QStringLiteral("VM configuration changed after preview: %1").arg(write.path));
        return false;
    }
    return true;
}

} // namespace

QString virtualBoxProviderId() { return QStringLiteral("virtualbox"); }
QString vmwareWorkstationProviderId() { return QStringLiteral("vmware-workstation"); }
QString vmwarePlayerProviderId() { return QStringLiteral("vmware-player"); }

bool isKnownProviderId(const QString &id)
{
    return id == virtualBoxProviderId() || id == vmwareWorkstationProviderId()
        || id == vmwarePlayerProviderId();
}

QString powerStateName(PowerState state)
{
    switch (state) {
    case PowerState::Unknown: return QStringLiteral("unknown");
    case PowerState::Inaccessible: return QStringLiteral("inaccessible");
    case PowerState::PoweredOff: return QStringLiteral("powered-off");
    case PowerState::Running: return QStringLiteral("running");
    case PowerState::Paused: return QStringLiteral("paused");
    case PowerState::Suspended: return QStringLiteral("suspended");
    case PowerState::Saved: return QStringLiteral("saved");
    case PowerState::Aborted: return QStringLiteral("aborted");
    }
    return QStringLiteral("unknown");
}

QString ownershipName(Ownership ownership)
{
    return ownership == Ownership::Managed ? QStringLiteral("managed") : QStringLiteral("external");
}

QString riskName(Risk risk)
{
    switch (risk) {
    case Risk::ReadOnly: return QStringLiteral("read-only");
    case Risk::Reversible: return QStringLiteral("reversible");
    case Risk::Disruptive: return QStringLiteral("disruptive");
    case Risk::Destructive: return QStringLiteral("destructive");
    }
    return QStringLiteral("read-only");
}

QString firmwareName(Firmware firmware)
{
    return firmware == Firmware::Efi ? QStringLiteral("efi") : QStringLiteral("bios");
}

QString networkModeName(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::Nat: return QStringLiteral("nat");
    case NetworkMode::Bridged: return QStringLiteral("bridged");
    case NetworkMode::HostOnly: return QStringLiteral("host-only");
    case NetworkMode::Internal: return QStringLiteral("internal");
    case NetworkMode::Disconnected: return QStringLiteral("disconnected");
    }
    return QStringLiteral("nat");
}

bool isSafeMachineFileStem(const QString &value)
{
    if (value.isEmpty() || value.trimmed() != value || value == QStringLiteral(".")
        || value == QStringLiteral("..") || value.endsWith(QLatin1Char('.'))
        || value.endsWith(QLatin1Char(' '))) {
        return false;
    }
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || forbidden.contains(character))
            return false;
    }
    static const QRegularExpression reserved(
        QStringLiteral("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return !reserved.match(value).hasMatch();
}

bool ProviderInfo::supports(const QString &capability) const
{
    return available && capabilities.contains(capability);
}

bool VmRef::valid() const
{
    return isKnownProviderId(providerId) && !id.trimmed().isEmpty() && !name.trimmed().isEmpty();
}

bool ConfigPatch::empty() const
{
    return !cpuCount && !memoryMiB && !firmware && !secureBoot && !tpm && !networkMode
        && !bridgedInterface && !isoPath;
}

bool Command::valid(QString *error) const
{
    if (executable.trimmed().isEmpty() || !QFileInfo(executable).isAbsolute()) {
        setError(error, QStringLiteral("Provider command executable must be absolute."));
        return false;
    }
    const QString baseName = QFileInfo(executable).fileName().toLower();
    static const QSet<QString> shells{
        QStringLiteral("cmd.exe"), QStringLiteral("powershell.exe"), QStringLiteral("pwsh.exe"),
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh")};
    if (shells.contains(baseName)) {
        setError(error, QStringLiteral("VM Lab never executes provider operations through a shell."));
        return false;
    }
    if (!workingDirectory.isEmpty() && !QFileInfo(workingDirectory).isAbsolute()) {
        setError(error, QStringLiteral("Provider command working directory must be absolute."));
        return false;
    }
    if (timeoutMs <= 0 || timeoutMs > 30 * 60 * 1000) {
        setError(error, QStringLiteral("Provider command timeout is outside the safe range."));
        return false;
    }
    if (executable.contains(QChar::Null)) {
        setError(error, QStringLiteral("Provider command contains a NUL character."));
        return false;
    }
    for (const QString &argument : arguments) {
        if (argument.contains(QChar::Null)) {
            setError(error, QStringLiteral("Provider argument contains a NUL character."));
            return false;
        }
    }
    setError(error, {});
    return true;
}

bool OperationPreview::expired(const QDateTime &now) const
{
    return !expiry.isValid() || now.toUTC() > expiry.toUTC();
}

bool ProcessResult::ok() const
{
    return started && !timedOut && exitCode == 0 && error.isEmpty();
}

ProcessResult ProcessCommandRunner::run(const Command &command)
{
    ProcessResult result;
    if (!command.valid(&result.error))
        return result;
    QProcess process;
    process.setProgram(command.executable);
    process.setArguments(command.arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!command.workingDirectory.isEmpty())
        process.setWorkingDirectory(command.workingDirectory);
    QElapsedTimer elapsed;
    elapsed.start();
    process.start();
    if (!process.waitForStarted(std::min(command.timeoutMs, 10000))) {
        result.error = process.errorString();
        return result;
    }
    result.started = true;
    const int remaining = std::max(1, command.timeoutMs - static_cast<int>(elapsed.elapsed()));
    if (!process.waitForFinished(remaining)) {
        result.timedOut = true;
        result.error = QStringLiteral("Provider command timed out after %1 ms.").arg(command.timeoutMs);
        process.kill();
        process.waitForFinished(5000);
    }
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    if (!result.timedOut) {
        result.exitCode = process.exitCode();
        if (process.exitStatus() != QProcess::NormalExit)
            result.error = QStringLiteral("Provider command crashed.");
    }
    return result;
}

DeletionGuard PathPolicy::managedDeletionGuard(const Machine &machine,
                                               const QString &managedRoot,
                                               const QList<Machine> &catalogMachines)
{
    DeletionGuard result;
    if (machine.ownership != Ownership::Managed) {
        result.error = QStringLiteral("External VMs can be unregistered but their files are never deleted.");
        return result;
    }
    if (!machine.inventoryComplete || machine.stateRevision.isEmpty()) {
        result.error = QStringLiteral("Refresh complete live VM inventory before deletion.");
        return result;
    }
    const QFileInfo rootInfo(managedRoot);
    const QFileInfo directoryInfo(machineDirectory(machine));
    if (!rootInfo.isAbsolute() || !rootInfo.exists() || !rootInfo.isDir()) {
        result.error = QStringLiteral("Managed VM root is missing or invalid.");
        return result;
    }
    if (rootInfo.isSymLink()
#ifdef Q_OS_WIN
        || rootInfo.isJunction()
#endif
    ) {
        result.error = QStringLiteral("Managed VM root cannot be a reparse point.");
        return result;
    }
    if (!directoryInfo.isAbsolute() || !directoryInfo.exists() || !directoryInfo.isDir()) {
        result.error = QStringLiteral("Managed VM directory is missing or invalid.");
        return result;
    }
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    const QString canonicalDirectory = directoryInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalDirectory.isEmpty()
        || samePath(canonicalRoot, canonicalDirectory)
        || !containedPath(canonicalRoot, canonicalDirectory)) {
        result.error = QStringLiteral("VM directory is outside the canonical managed root.");
        return result;
    }
    QString reparseError;
    if (hasReparsePoint(canonicalRoot, directoryInfo.absoluteFilePath(), &reparseError)) {
        result.error = reparseError;
        return result;
    }
    if (containsReparsePoint(canonicalDirectory, &reparseError)) {
        result.error = reparseError;
        return result;
    }
    for (const QString &storagePath : machine.storagePaths) {
        const QFileInfo storage(storagePath);
        const QString canonicalStorage = storage.canonicalFilePath();
        if (!storage.isAbsolute() || !storage.exists() || canonicalStorage.isEmpty()
            || !containedPath(canonicalDirectory, canonicalStorage)) {
            result.error = QStringLiteral("Attached storage is outside the managed VM directory: %1")
                               .arg(storagePath);
            return result;
        }
        if (storage.isSymLink()
#ifdef Q_OS_WIN
            || storage.isJunction()
#endif
        ) {
            result.error = QStringLiteral("Managed deletion refuses reparse storage: %1").arg(storagePath);
            return result;
        }
    }
    bool targetPresent = false;
    for (const Machine &other : catalogMachines) {
        if (!other.inventoryComplete || other.stateRevision.isEmpty()) {
            result.error = QStringLiteral("Deletion requires refreshed topology for every catalog VM.");
            return result;
        }
        if (other.ref.providerId == machine.ref.providerId && other.ref.id == machine.ref.id)
            targetPresent = true;
        if (other.ref.providerId == machine.ref.providerId && other.ref.id == machine.ref.id)
            continue;
        const QFileInfo otherInfo(machineDirectory(other));
        if (otherInfo.exists()) {
            const QString otherCanonical = otherInfo.canonicalFilePath();
            if (samePath(otherCanonical, canonicalDirectory)
                || containedPath(canonicalDirectory, otherCanonical)) {
                result.error = QStringLiteral("Managed directory is shared with catalog VM '%1'.")
                                   .arg(other.ref.name);
                return result;
            }
        }
        for (const QString &storagePath : machine.storagePaths) {
            for (const QString &otherStoragePath : other.storagePaths) {
                if (samePath(storagePath, otherStoragePath)) {
                    result.error = QStringLiteral("Attached storage is shared with catalog VM '%1'.")
                                       .arg(other.ref.name);
                    return result;
                }
            }
        }
        for (const QString &otherStoragePath : other.storagePaths) {
            const QFileInfo otherStorage(otherStoragePath);
            if (otherStorage.exists()
                && containedPath(canonicalDirectory, otherStorage.canonicalFilePath())) {
                result.error = QStringLiteral("Managed directory contains storage used by catalog VM '%1'.")
                                   .arg(other.ref.name);
                return result;
            }
        }
    }
    if (!targetPresent) {
        result.error = QStringLiteral("Deletion target is absent from the refreshed VM inventory.");
        return result;
    }
    QString identityError;
    result.identity = pathIdentity(canonicalDirectory, &identityError);
    if (result.identity.isEmpty()) {
        result.error = identityError.isEmpty()
            ? QStringLiteral("Could not bind the managed VM directory identity.")
            : identityError;
        return result;
    }
    result.allowed = true;
    result.canonicalDirectory = canonicalDirectory;
    return result;
}

bool PathPolicy::deleteManagedDirectory(const Machine &machine,
                                        const QString &managedRoot,
                                        const QList<Machine> &catalogMachines,
                                        const QString &expectedIdentity,
                                        QString *error)
{
    const DeletionGuard guard = managedDeletionGuard(machine, managedRoot, catalogMachines);
    if (!guard.allowed) {
        setError(error, guard.error);
        return false;
    }
    if (expectedIdentity.isEmpty() || guard.identity != expectedIdentity) {
        setError(error, QStringLiteral("Managed VM directory identity changed after preview."));
        return false;
    }
#ifdef Q_OS_WIN
    if (!deleteTreeNoFollowWindows(guard.canonicalDirectory, expectedIdentity, error))
        return false;
#else
    QDir directory(guard.canonicalDirectory);
    if (!directory.removeRecursively()) {
        setError(error, QStringLiteral("Provider files could not be removed from %1.")
                            .arg(guard.canonicalDirectory));
        return false;
    }
#endif
    setError(error, {});
    return true;
}

Catalog::Catalog(QString path) : m_path(std::move(path)) {}
QString Catalog::path() const { return m_path; }
QList<Machine> Catalog::machines() const { return m_machines; }

QString Catalog::revision() const
{
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(catalogJson(m_machines)).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

void Catalog::setMachines(const QList<Machine> &machines) { m_machines = machines; }

bool Catalog::upsert(const Machine &machine, QString *error)
{
    if (!machine.ref.valid() || !QFileInfo(machine.configPath).isAbsolute()) {
        setError(error, QStringLiteral("Catalog machine identity and configuration path must be valid."));
        return false;
    }
    for (Machine &existing : m_machines) {
        if (existing.ref.providerId == machine.ref.providerId && existing.ref.id == machine.ref.id) {
            existing = machine;
            existing.powerState = PowerState::Unknown;
            existing.inaccessibleReason.clear();
            existing.warnings.clear();
            existing.storagePaths.clear();
            existing.stateRevision.clear();
            existing.inventoryComplete = false;
            setError(error, {});
            return true;
        }
    }
    Machine portable = machine;
    portable.powerState = PowerState::Unknown;
    portable.inaccessibleReason.clear();
    portable.warnings.clear();
    portable.storagePaths.clear();
    portable.stateRevision.clear();
    portable.inventoryComplete = false;
    m_machines.append(portable);
    setError(error, {});
    return true;
}

bool Catalog::remove(const VmRef &reference)
{
    const auto found = std::find_if(m_machines.begin(), m_machines.end(), [&reference](const Machine &item) {
        return item.ref.providerId == reference.providerId && item.ref.id == reference.id;
    });
    if (found == m_machines.end())
        return false;
    m_machines.erase(found);
    return true;
}

bool Catalog::load(QString *error)
{
    if (m_path.trimmed().isEmpty() || !QFileInfo(m_path).isAbsolute()) {
        setError(error, QStringLiteral("VM catalog path must be absolute."));
        return false;
    }
    if (!QFileInfo::exists(m_path)) {
        m_machines.clear();
        m_expectedDiskSha256.clear();
        m_diskStateKnown = true;
        m_expectedMissing = true;
        setError(error, {});
        return true;
    }
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("VM catalog JSON is invalid: %1").arg(parseError.errorString()));
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toString() != QStringLiteral("wimforge.vm-catalog")
        || root.value(QStringLiteral("version")).toInt(-1) != CurrentVersion
        || !root.value(QStringLiteral("machines")).isArray()) {
        setError(error, QStringLiteral("VM catalog schema or version is unsupported."));
        return false;
    }
    QList<Machine> loaded;
    QSet<QString> identities;
    for (const QJsonValue &value : root.value(QStringLiteral("machines")).toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("VM catalog contains a non-object machine entry."));
            return false;
        }
        const QJsonObject object = value.toObject();
        Machine machine;
        machine.ref.providerId = object.value(QStringLiteral("providerId")).toString();
        machine.ref.id = object.value(QStringLiteral("id")).toString();
        machine.ref.name = object.value(QStringLiteral("name")).toString();
        machine.configPath = object.value(QStringLiteral("configPath")).toString();
        const QString ownership = object.value(QStringLiteral("ownership")).toString();
        if (ownership == QStringLiteral("managed"))
            machine.ownership = Ownership::Managed;
        else if (ownership == QStringLiteral("external"))
            machine.ownership = Ownership::External;
        else {
            setError(error, QStringLiteral("VM catalog contains an invalid ownership value."));
            return false;
        }
        const QString identity = machine.ref.providerId + QChar::Null + machine.ref.id;
        if (!machine.ref.valid() || !QFileInfo(machine.configPath).isAbsolute()
            || identities.contains(identity)) {
            setError(error, QStringLiteral("VM catalog contains an invalid or duplicate machine."));
            return false;
        }
        identities.insert(identity);
        loaded.append(machine);
    }
    m_machines = std::move(loaded);
    m_expectedDiskSha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    m_diskStateKnown = true;
    m_expectedMissing = false;
    setError(error, {});
    return true;
}

bool Catalog::save(QString *error)
{
    if (m_path.trimmed().isEmpty() || !QFileInfo(m_path).isAbsolute()) {
        setError(error, QStringLiteral("VM catalog path must be absolute."));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        setError(error, QStringLiteral("Could not create VM catalog directory."));
        return false;
    }
    QLockFile lock(m_path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(5000)) {
        setError(error, QStringLiteral("VM catalog is locked by another writer: %1")
                            .arg(lock.error()));
        return false;
    }
    const bool exists = QFileInfo::exists(m_path);
    if (!m_diskStateKnown && exists) {
        setError(error, QStringLiteral("VM catalog appeared before save; reload and retry."));
        return false;
    }
    if (m_diskStateKnown && exists == m_expectedMissing) {
        setError(error, QStringLiteral("VM catalog existence changed after load; reload and retry."));
        return false;
    }
    if (m_diskStateKnown && exists) {
        QString hashError;
        const QString currentHash = fileSha256(m_path, &hashError);
        if (!hashError.isEmpty() || currentHash != m_expectedDiskSha256) {
            setError(error, hashError.isEmpty()
                                ? QStringLiteral("VM catalog changed on disk; reload and retry.")
                                : hashError);
            return false;
        }
    }
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    const QByteArray bytes = QJsonDocument(catalogJson(m_machines)).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    m_expectedDiskSha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    m_diskStateKnown = true;
    m_expectedMissing = false;
    setError(error, {});
    return true;
}

bool Executor::validate(const Plan &plan,
                        const QString &currentRevision,
                        const QString &typedConfirmation,
                        const QDateTime &now,
                        QString *error)
{
    if (!plan.ok()) {
        setError(error, plan.errors.join(QStringLiteral("\n")));
        return false;
    }
    const OperationPreview &preview = plan.preview;
    if (preview.id.isNull() || preview.action.trimmed().isEmpty() || !preview.target.valid()) {
        setError(error, QStringLiteral("VM operation preview is incomplete."));
        return false;
    }
    if (preview.revision != currentRevision) {
        setError(error, QStringLiteral("VM state changed after preview; refresh and review again."));
        return false;
    }
    if (preview.expired(now)) {
        setError(error, QStringLiteral("VM operation preview expired; review it again."));
        return false;
    }
    if (preview.risk == Risk::Destructive
        && (preview.confirmation.isEmpty() || typedConfirmation != preview.confirmation)) {
        setError(error, QStringLiteral("Type the exact destructive confirmation token."));
        return false;
    }
    for (const Command &command : preview.commands) {
        QString commandError;
        if (!command.valid(&commandError)) {
            setError(error, commandError);
            return false;
        }
    }
    for (const CommandEvidence &evidence : plan.preflight) {
        QString commandError;
        if (!evidence.command.valid(&commandError) || evidence.expected.isEmpty()) {
            setError(error, commandError.isEmpty()
                                ? QStringLiteral("VM preflight evidence is incomplete.")
                                : commandError);
            return false;
        }
    }
    for (const AtomicWrite &write : plan.atomicWritesAfterCommands) {
        QString revisionError;
        if (!atomicWriteRevisionMatches(write, &revisionError)) {
            setError(error, revisionError);
            return false;
        }
    }
    if (plan.managedDeletionAfterCommands) {
        const ManagedDeletion &deletion = *plan.managedDeletionAfterCommands;
        const DeletionGuard guard = PathPolicy::managedDeletionGuard(
            deletion.machine, deletion.managedRoot, deletion.catalogMachines);
        if (!guard.allowed) {
            setError(error, guard.error);
            return false;
        }
        if (deletion.expectedIdentity.isEmpty()
            || guard.identity != deletion.expectedIdentity) {
            setError(error, QStringLiteral("Managed VM directory identity changed after preview."));
            return false;
        }
    }
    setError(error, {});
    return true;
}

Result Executor::execute(const Plan &plan,
                         const QString &currentRevision,
                         const QString &typedConfirmation,
                         const QDateTime &now,
                         CommandRunner &runner)
{
    Result result;
    if (!validate(plan, currentRevision, typedConfirmation, now, &result.error))
        return result;
    for (const CommandEvidence &evidence : plan.preflight) {
        ProcessResult process = runner.run(evidence.command);
        result.processes.append(process);
        if (!process.ok()) {
            const QString detail = QString::fromLocal8Bit(process.standardError).trimmed();
            result.error = process.error.isEmpty()
                ? QStringLiteral("Provider preflight failed with exit code %1%2")
                      .arg(process.exitCode)
                      .arg(detail.isEmpty() ? QStringLiteral(".")
                                            : QStringLiteral(": %1").arg(detail.left(1024)))
                : process.error;
            return result;
        }
        QString evidenceError;
        const QString current = commandEvidence(
            evidence.format, process.standardOutput, &evidenceError);
        if (!evidenceError.isEmpty() || current != evidence.expected) {
            result.error = evidenceError.isEmpty()
                ? QStringLiteral("Provider state changed after preview: %1")
                      .arg(evidence.description)
                : evidenceError;
            return result;
        }
    }
    for (const Command &command : plan.preview.commands) {
        ProcessResult process = runner.run(command);
        result.processes.append(process);
        if (!process.ok()) {
            if (!process.error.isEmpty()) {
                result.error = process.error;
            } else {
                QString providerDetail = QString::fromLocal8Bit(process.standardError).trimmed();
                if (providerDetail.size() > 1024)
                    providerDetail = providerDetail.left(1021) + QStringLiteral("...");
                result.error = QStringLiteral("Provider command failed with exit code %1%2")
                                   .arg(process.exitCode)
                                   .arg(providerDetail.isEmpty()
                                            ? QStringLiteral(".")
                                            : QStringLiteral(": %1").arg(providerDetail));
            }
            return result;
        }
    }
    for (const AtomicWrite &write : plan.atomicWritesAfterCommands) {
        if (!atomicWriteRevisionMatches(write, &result.error) || !writeAtomic(write, &result.error))
            return result;
    }
    if (plan.managedDeletionAfterCommands) {
        const ManagedDeletion &deletion = *plan.managedDeletionAfterCommands;
        if (!PathPolicy::deleteManagedDirectory(
                deletion.machine, deletion.managedRoot, deletion.catalogMachines,
                deletion.expectedIdentity, &result.error)) {
            return result;
        }
    }
    result.success = true;
    return result;
}

OperationPreview makePreview(const QString &action,
                             const VmRef &target,
                             Risk risk,
                             const QStringList &effects,
                             const QStringList &warnings,
                             const QList<Command> &commands,
                             const QString &revision,
                             const QDateTime &now)
{
    OperationPreview preview;
    preview.id = QUuid::createUuid();
    preview.action = action;
    preview.target = target;
    preview.risk = risk;
    preview.effects = effects;
    preview.warnings = warnings;
    preview.commands = commands;
    preview.revision = revision;
    preview.expiry = now.toUTC().addSecs(5 * 60);
    if (risk == Risk::Destructive)
        preview.confirmation = QStringLiteral("DELETE %1").arg(target.name);
    return preview;
}

QString fileSha256(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        setError(error, QStringLiteral("Could not hash VM configuration: %1").arg(path));
        return {};
    }
    setError(error, {});
    return QString::fromLatin1(hash.result().toHex());
}

QString commandEvidence(EvidenceFormat format, const QByteArray &standardOutput,
                        QString *error)
{
    QByteArray canonical;
    if (format == EvidenceFormat::RawSha256) {
        canonical = standardOutput;
    } else {
        const QStringList lines = QString::fromLocal8Bit(standardOutput).split(
            QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        if (lines.isEmpty()) {
            setError(error, QStringLiteral("VMware live-state preflight output is empty."));
            return {};
        }
        const QRegularExpression header(
            QStringLiteral("^Total running VMs:\\s*([0-9]+)$"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = header.match(lines.first().trimmed());
        if (!match.hasMatch()) {
            setError(error, QStringLiteral("VMware live-state preflight lacks its count header."));
            return {};
        }
        QStringList paths;
        for (qsizetype index = 1; index < lines.size(); ++index) {
            const QFileInfo path(lines.at(index).trimmed());
            if (!path.isAbsolute()) {
                setError(error, QStringLiteral("VMware live-state preflight returned a relative VMX path."));
                return {};
            }
            paths.append(QDir::fromNativeSeparators(path.absoluteFilePath()).toCaseFolded());
        }
        if (match.captured(1).toLongLong() != paths.size()) {
            setError(error, QStringLiteral("VMware live-state preflight count does not match its paths."));
            return {};
        }
        std::sort(paths.begin(), paths.end());
        canonical = paths.join(QLatin1Char('\n')).toUtf8();
    }
    setError(error, {});
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

} // namespace wimforge::vmlab
