#include "VmLab.h"
#include "ProcessLaunch.h"

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
#include <limits>
#include <vector>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#else
#include <sys/stat.h>
#endif

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
    ScopedWinHandle(ScopedWinHandle &&other) noexcept : m_value(other.m_value)
    {
        other.m_value = INVALID_HANDLE_VALUE;
    }
    ScopedWinHandle &operator=(ScopedWinHandle &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_value = other.m_value;
            other.m_value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return m_value; }
    [[nodiscard]] bool valid() const { return m_value != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE)
    {
        if (m_value != INVALID_HANDLE_VALUE)
            CloseHandle(m_value);
        m_value = value;
    }

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
                               QString *error,
                               HANDLE retainedHandle = INVALID_HANDLE_VALUE)
{
    ScopedWinHandle opened;
    HANDLE handle = retainedHandle;
    if (handle == INVALID_HANDLE_VALUE) {
        opened = openNoFollowForDelete(path, error);
        if (!opened.valid())
            return false;
        handle = opened.get();
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!queryHandleAttributes(handle, &attributes, error))
        return false;
    const QString identity = identityForHandle(handle, error);
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
                if (name.endsWith(QStringLiteral(".lck"), Qt::CaseInsensitive)) {
                    setError(error, QStringLiteral(
                        "Managed deletion refuses an active provider lock: %1")
                                        .arg(QDir(path).filePath(name)));
                    ok = false;
                    break;
                }
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
    if (!markHandleForDeletion(handle, error))
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
    const QByteArray encodedPath = QFile::encodeName(path);
    struct stat metadata {};
    if (::lstat(encodedPath.constData(), &metadata) != 0) {
        setError(error, QStringLiteral("Could not read managed path identity."));
        return {};
    }
    if (S_ISLNK(metadata.st_mode)) {
        setError(error, QStringLiteral("Managed path identity refuses a symbolic link."));
        return {};
    }
    setError(error, {});
    return QStringLiteral("%1:%2")
        .arg(static_cast<qulonglong>(metadata.st_dev), 0, 16)
        .arg(static_cast<qulonglong>(metadata.st_ino), 0, 16);
}

#endif

#ifdef Q_OS_WIN

class ManagedPathLease
{
public:
    bool reserveCreate(const QString &rootPath, const QString &targetPath,
                       const QString &expectedRootIdentity, QString *error)
    {
        if (!acquireRoot(rootPath, expectedRootIdentity, error))
            return false;
        const QFileInfo target(targetPath);
        if (!target.isAbsolute() || target.exists()
            || !samePath(target.absolutePath(), m_rootPath)
            || !isSafeMachineFileStem(target.fileName())) {
            setError(error, QStringLiteral(
                "Managed VM creation must reserve a new direct child of the leased root."));
            return false;
        }
        Entry created;
        created.path = target.absoluteFilePath();
        created.handle = createDirectChild(
            m_ancestors.front().handle.get(), target.fileName(), error);
        if (!created.handle.valid())
            return false;
        if (!readEntryIdentity(created, error))
            return false;
        const QString canonical = QFileInfo(created.path).canonicalFilePath();
        if (canonical.isEmpty() || !samePath(canonical, created.path)
            || !containedPath(m_rootPath, canonical)) {
            setError(error, QStringLiteral(
                "Reserved managed VM directory did not resolve beneath the leased root."));
            return false;
        }
        m_target = std::move(created);
        return validate(error);
    }

    bool acquireDeletion(const QString &rootPath, const QString &targetPath,
                         const QString &expectedRootIdentity,
                         const QString &expectedTargetIdentity, QString *error)
    {
        if (!acquireRoot(rootPath, expectedRootIdentity, error))
            return false;
        const QString canonicalTarget = QFileInfo(targetPath).canonicalFilePath();
        if (canonicalTarget.isEmpty() || !containedPath(m_rootPath, canonicalTarget)) {
            setError(error, QStringLiteral(
                "Managed deletion target is outside the leased root."));
            return false;
        }
        const QString relative = QDir(m_rootPath).relativeFilePath(canonicalTarget);
        const QStringList segments = QDir::fromNativeSeparators(relative).split(
            QLatin1Char('/'), Qt::SkipEmptyParts);
        if (segments.isEmpty()) {
            setError(error, QStringLiteral("Managed deletion cannot lease the root itself."));
            return false;
        }
        QString cursor = m_rootPath;
        for (qsizetype index = 0; index < segments.size(); ++index) {
            cursor = QDir(cursor).filePath(segments.at(index));
            Entry entry;
            entry.path = cursor;
            const bool target = index == segments.size() - 1;
            entry.handle = openDirectory(entry.path, target, error);
            if (!entry.handle.valid() || !readEntryIdentity(entry, error))
                return false;
            if (target) {
                if (expectedTargetIdentity.isEmpty()
                    || entry.identity != expectedTargetIdentity) {
                    setError(error, QStringLiteral(
                        "Managed VM directory identity changed after preview."));
                    return false;
                }
                m_target = std::move(entry);
            } else {
                m_ancestors.push_back(std::move(entry));
            }
        }
        return validate(error);
    }

    bool validate(QString *error) const
    {
        for (const Entry &entry : m_ancestors) {
            if (!validateEntry(entry, error))
                return false;
        }
        if (m_target && !validateEntry(*m_target, error))
            return false;
        setError(error, {});
        return true;
    }

    [[nodiscard]] HANDLE targetHandle() const
    {
        return m_target ? m_target->handle.get() : INVALID_HANDLE_VALUE;
    }

    void closeTarget()
    {
        if (m_target)
            m_target->handle.reset();
    }

private:
    struct Entry
    {
        QString path;
        QString identity;
        ScopedWinHandle handle;
    };

    static ScopedWinHandle openDirectory(const QString &path, bool deleteAccess,
                                         QString *error)
    {
        const QString native = winApiPath(path);
        DWORD access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        if (deleteAccess)
            access |= DELETE;
        const HANDLE handle = CreateFileW(
            reinterpret_cast<LPCWSTR>(native.utf16()), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            setError(error, winErrorMessage(
                                QStringLiteral("Could not acquire managed directory lease %1")
                                    .arg(path)));
        }
        return ScopedWinHandle(handle);
    }

    static ScopedWinHandle createDirectChild(HANDLE rootHandle,
                                             const QString &childName,
                                             QString *error)
    {
        using NtCreateFileFunction = NTSTATUS(NTAPI *)(
            PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        using NtStatusToDosErrorFunction = ULONG(WINAPI *)(NTSTATUS);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto ntCreateFile = ntdll
            ? reinterpret_cast<NtCreateFileFunction>(
                  GetProcAddress(ntdll, "NtCreateFile"))
            : nullptr;
        const auto ntStatusToDosError = ntdll
            ? reinterpret_cast<NtStatusToDosErrorFunction>(
                  GetProcAddress(ntdll, "RtlNtStatusToDosError"))
            : nullptr;
        if (!ntCreateFile || childName.isEmpty()
            || childName.size() > (std::numeric_limits<USHORT>::max() / 2) - 1) {
            setError(error, QStringLiteral(
                "Windows cannot safely acquire an atomic managed-directory reservation handle."));
            return ScopedWinHandle{};
        }

        UNICODE_STRING name{};
        name.Buffer = const_cast<PWSTR>(
            reinterpret_cast<const wchar_t *>(childName.utf16()));
        name.Length = static_cast<USHORT>(childName.size() * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(
            &attributes, &name, OBJ_CASE_INSENSITIVE, rootHandle, nullptr);
        IO_STATUS_BLOCK statusBlock{};
        HANDLE created = INVALID_HANDLE_VALUE;
        const NTSTATUS status = ntCreateFile(
            &created,
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            &attributes, &statusBlock, nullptr, FILE_ATTRIBUTE_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
                | FILE_OPEN_REPARSE_POINT,
            nullptr, 0);
        if (status < 0 || created == INVALID_HANDLE_VALUE) {
            const DWORD code = ntStatusToDosError
                ? static_cast<DWORD>(ntStatusToDosError(status))
                : ERROR_CANNOT_MAKE;
            setError(error, winErrorMessage(
                                QStringLiteral(
                                    "Could not atomically reserve managed VM directory"),
                                code));
            return ScopedWinHandle{};
        }
        return ScopedWinHandle(created);
    }

    static bool readEntryIdentity(Entry &entry, QString *error)
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!queryHandleAttributes(entry.handle.get(), &attributes, error))
            return false;
        if (!(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            setError(error, QStringLiteral("Managed path lease is not a directory: %1")
                                .arg(entry.path));
            return false;
        }
        entry.identity = identityForHandle(entry.handle.get(), error);
        return !entry.identity.isEmpty();
    }

    static bool validateEntry(const Entry &entry, QString *error)
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!entry.handle.valid()
            || !queryHandleAttributes(entry.handle.get(), &attributes, error)
            || !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("Managed directory lease became invalid: %1")
                             .arg(entry.path);
            return false;
        }
        const QString boundIdentity = identityForHandle(entry.handle.get(), error);
        if (boundIdentity.isEmpty() || boundIdentity != entry.identity)
            return false;
        QString pathError;
        const QString currentIdentity = pathIdentity(entry.path, &pathError);
        if (!pathError.isEmpty() || currentIdentity != entry.identity) {
            setError(error, pathError.isEmpty()
                                ? QStringLiteral("Managed path was substituted during execution: %1")
                                      .arg(entry.path)
                                : pathError);
            return false;
        }
        return true;
    }

    bool acquireRoot(const QString &rootPath, const QString &expectedIdentity,
                     QString *error)
    {
        const QString canonical = QFileInfo(rootPath).canonicalFilePath();
        if (canonical.isEmpty()) {
            setError(error, QStringLiteral("Managed root could not be resolved for leasing."));
            return false;
        }
        Entry root;
        root.path = canonical;
        root.handle = openDirectory(root.path, false, error);
        if (!root.handle.valid() || !readEntryIdentity(root, error))
            return false;
        if (expectedIdentity.isEmpty() || root.identity != expectedIdentity) {
            setError(error, QStringLiteral("Managed root identity changed after preview."));
            return false;
        }
        m_rootPath = canonical;
        m_ancestors.push_back(std::move(root));
        return true;
    }

    QString m_rootPath;
    std::vector<Entry> m_ancestors;
    std::optional<Entry> m_target;
};

#else

class ManagedPathLease
{
public:
    bool reserveCreate(const QString &rootPath, const QString &targetPath,
                       const QString &expectedRootIdentity, QString *error)
    {
        if (!acquireRoot(rootPath, expectedRootIdentity, error))
            return false;
        const QFileInfo target(targetPath);
        if (target.exists() || !samePath(target.absolutePath(), m_rootPath)
            || !QDir(m_rootPath).mkdir(target.fileName())) {
            setError(error, QStringLiteral("Could not reserve managed VM directory."));
            return false;
        }
        m_targetPath = target.absoluteFilePath();
        m_targetIdentity = pathIdentity(m_targetPath, error);
        return !m_targetIdentity.isEmpty();
    }

    bool acquireDeletion(const QString &rootPath, const QString &targetPath,
                         const QString &expectedRootIdentity,
                         const QString &expectedTargetIdentity, QString *error)
    {
        if (!acquireRoot(rootPath, expectedRootIdentity, error))
            return false;
        m_targetPath = QFileInfo(targetPath).canonicalFilePath();
        m_targetIdentity = pathIdentity(m_targetPath, error);
        if (m_targetIdentity != expectedTargetIdentity) {
            setError(error, QStringLiteral("Managed VM directory identity changed after preview."));
            return false;
        }
        return true;
    }

    bool validate(QString *error) const
    {
        QString identityError;
        if (pathIdentity(m_rootPath, &identityError) != m_rootIdentity
            || (!m_targetPath.isEmpty()
                && pathIdentity(m_targetPath, &identityError) != m_targetIdentity)) {
            setError(error, identityError.isEmpty()
                                ? QStringLiteral("Managed path changed during execution.")
                                : identityError);
            return false;
        }
        setError(error, {});
        return true;
    }

    void closeTarget() {}

private:
    bool acquireRoot(const QString &rootPath, const QString &expectedIdentity,
                     QString *error)
    {
        m_rootPath = QFileInfo(rootPath).canonicalFilePath();
        m_rootIdentity = pathIdentity(m_rootPath, error);
        if (m_rootIdentity.isEmpty() || m_rootIdentity != expectedIdentity) {
            setError(error, QStringLiteral("Managed root identity changed after preview."));
            return false;
        }
        return true;
    }

    QString m_rootPath;
    QString m_rootIdentity;
    QString m_targetPath;
    QString m_targetIdentity;
};

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
        {QStringLiteral("ownershipToken"), machine.ownershipToken},
    };
}

bool managedMarkerMatches(const Machine &machine, const QString &directory,
                          QString *error)
{
    if (machine.ownershipToken.trimmed().isEmpty()) {
        setError(error, QStringLiteral(
            "Managed deletion requires a durable WimForge ownership token."));
        return false;
    }
    const QString markerPath = QDir(directory).filePath(managedOwnershipMarkerFileName());
    const QFileInfo markerInfo(markerPath);
    if (!markerInfo.isAbsolute() || !markerInfo.exists() || !markerInfo.isFile()
        || markerInfo.isSymLink()
#ifdef Q_OS_WIN
        || markerInfo.isJunction()
#endif
    ) {
        setError(error, QStringLiteral(
            "Managed deletion requires the original WimForge ownership marker."));
        return false;
    }
    QFile marker(markerPath);
    if (!marker.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not read the WimForge ownership marker: %1")
                            .arg(marker.errorString()));
        return false;
    }
    if (marker.size() < 0 || marker.size() > 4096) {
        setError(error, QStringLiteral("The WimForge ownership marker exceeds 4 KiB."));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(marker.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("The WimForge ownership marker is invalid."));
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toString()
            != QStringLiteral("wimforge.managed-vm")
        || object.value(QStringLiteral("version")).toInt(-1) != 1
        || object.value(QStringLiteral("providerId")).toString() != machine.ref.providerId
        || object.value(QStringLiteral("id")).toString() != machine.ref.id
        || object.value(QStringLiteral("token")).toString() != machine.ownershipToken) {
        setError(error, QStringLiteral(
            "The WimForge ownership marker does not match this catalog VM."));
        return false;
    }
    setError(error, {});
    return true;
}

bool prepareExclusiveDirectory(const QString &path, QString *error)
{
    const QFileInfo target(path);
    const QFileInfo parent(target.absolutePath());
    if (!target.isAbsolute() || target.exists() || target.fileName().trimmed().isEmpty()
        || !parent.exists() || !parent.isDir() || parent.isSymLink()
#ifdef Q_OS_WIN
        || parent.isJunction()
#endif
    ) {
        setError(error, QStringLiteral(
            "Managed VM directory must be a new direct child of an existing real directory: %1")
                            .arg(path));
        return false;
    }
    QDir parentDirectory(parent.absoluteFilePath());
    if (!parentDirectory.mkdir(target.fileName())) {
        setError(error, QStringLiteral(
            "Could not reserve the dedicated managed VM directory: %1").arg(path));
        return false;
    }
    setError(error, {});
    return true;
}

bool exclusiveDirectoryIsAvailable(const QString &path, QString *error)
{
    const QFileInfo target(path);
    const QFileInfo parent(target.absolutePath());
    if (!target.isAbsolute() || target.exists() || target.fileName().trimmed().isEmpty()
        || !parent.exists() || !parent.isDir() || parent.isSymLink()
#ifdef Q_OS_WIN
        || parent.isJunction()
#endif
    ) {
        setError(error, QStringLiteral(
            "Managed VM directory is no longer available for exclusive creation: %1")
                            .arg(path));
        return false;
    }
    setError(error, {});
    return true;
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

QString managedOwnershipMarkerFileName()
{
    return QStringLiteral(".wimforge-managed-vm.json");
}

QByteArray managedOwnershipMarkerContents(const VmRef &reference, const QString &token)
{
    QJsonObject object{
        {QStringLiteral("schema"), QStringLiteral("wimforge.managed-vm")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("providerId"), reference.providerId},
        {QStringLiteral("id"), reference.id},
        {QStringLiteral("token"), token},
    };
    QByteArray result = QJsonDocument(object).toJson(QJsonDocument::Compact);
    result.append('\n');
    return result;
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
    if (!process.waitForStarted(std::min(command.timeoutMs, 10000))) {
        result.error = process.errorString();
        return result;
    }
    result.started = true;
    bool deadlineExceeded = false;
    while (process.state() != QProcess::NotRunning) {
        drainProcess(process, result);
        if (elapsed.elapsed() >= command.timeoutMs) {
            deadlineExceeded = true;
            if (command.interruptible)
                break;
        }
        process.waitForFinished(50);
    }
    if (process.state() != QProcess::NotRunning) {
        result.timedOut = true;
        result.error = QStringLiteral("Provider command timed out after %1 ms.").arg(command.timeoutMs);
        process.kill();
        process.waitForFinished(5000);
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

CreationGuard PathPolicy::managedCreateGuard(const QString &managedRoot,
                                             const QString &targetDirectory)
{
    CreationGuard result;
    const QFileInfo rootInfo(managedRoot);
    const QFileInfo targetInfo(targetDirectory);
    if (!rootInfo.isAbsolute() || !rootInfo.exists() || !rootInfo.isDir()
        || rootInfo.isSymLink()
#ifdef Q_OS_WIN
        || rootInfo.isJunction()
#endif
    ) {
        result.error = QStringLiteral("Managed VM root is missing, invalid, or a reparse point.");
        return result;
    }
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || !targetInfo.isAbsolute() || targetInfo.exists()
        || !isSafeMachineFileStem(targetInfo.fileName())) {
        result.error = QStringLiteral(
            "Managed VM target must be a new safe directory beneath the managed root.");
        return result;
    }
    const QFileInfo parent(targetInfo.absolutePath());
    const QString canonicalParent = parent.canonicalFilePath();
    if (!parent.exists() || !parent.isDir() || parent.isSymLink()
#ifdef Q_OS_WIN
        || parent.isJunction()
#endif
        || canonicalParent.isEmpty() || !samePath(canonicalParent, canonicalRoot)) {
        result.error = QStringLiteral(
            "Managed VM target must be a direct child of the canonical managed root.");
        return result;
    }
    QString identityError;
    const QString rootIdentity = pathIdentity(canonicalRoot, &identityError);
    if (rootIdentity.isEmpty()) {
        result.error = identityError.isEmpty()
            ? QStringLiteral("Could not bind managed root identity for creation.")
            : identityError;
        return result;
    }
    result.allowed = true;
    result.canonicalRoot = canonicalRoot;
    result.targetDirectory = targetInfo.absoluteFilePath();
    result.rootIdentity = rootIdentity;
    return result;
}

namespace {

enum class DeletionTargetPresence { MustBePresent, MustBeAbsent };

DeletionGuard managedDeletionGuardImpl(const Machine &machine,
                                       const QString &managedRoot,
                                       const QList<Machine> &catalogMachines,
                                       DeletionTargetPresence targetPresence)
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
    QString markerError;
    if (!managedMarkerMatches(machine, canonicalDirectory, &markerError)) {
        result.error = markerError;
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
    const QString expectedConfiguration = QFileInfo(machine.configPath).canonicalFilePath();
    QDirIterator configurationIterator(
        canonicalDirectory, {QStringLiteral("*.vmx"), QStringLiteral("*.vbox")},
        QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (configurationIterator.hasNext()) {
        const QString discovered = QFileInfo(configurationIterator.next()).canonicalFilePath();
        if (expectedConfiguration.isEmpty() || discovered.isEmpty()
            || !samePath(discovered, expectedConfiguration)) {
            result.error = QStringLiteral(
                "Managed deletion found an untracked VM configuration inside the target: %1")
                               .arg(configurationIterator.filePath());
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
    if (targetPresence == DeletionTargetPresence::MustBePresent && !targetPresent) {
        result.error = QStringLiteral("Deletion target is absent from the refreshed VM inventory.");
        return result;
    }
    if (targetPresence == DeletionTargetPresence::MustBeAbsent && targetPresent) {
        result.error = QStringLiteral(
            "Provider inventory still reports the VM after unregister; local files were preserved.");
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
    result.rootIdentity = pathIdentity(canonicalRoot, &identityError);
    if (result.rootIdentity.isEmpty()) {
        result.error = identityError.isEmpty()
            ? QStringLiteral("Could not bind the managed root identity.")
            : identityError;
        return result;
    }
    result.allowed = true;
    result.canonicalRoot = canonicalRoot;
    result.canonicalDirectory = canonicalDirectory;
    return result;
}

bool deleteManagedDirectoryWithLease(const DeletionGuard &guard,
                                     ManagedPathLease &lease,
                                     QString *error)
{
    if (!lease.validate(error))
        return false;
#ifdef Q_OS_WIN
    if (lease.targetHandle() == INVALID_HANDLE_VALUE) {
        setError(error, QStringLiteral("Managed deletion target lease is missing."));
        return false;
    }
    if (!deleteTreeNoFollowWindows(guard.canonicalDirectory, guard.identity, error,
                                   lease.targetHandle())) {
        return false;
    }
    lease.closeTarget();
    if (QFileInfo::exists(guard.canonicalDirectory)) {
        setError(error, QStringLiteral(
            "Managed VM directory remained after handle-bound deletion."));
        return false;
    }
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

} // namespace

DeletionGuard PathPolicy::managedDeletionGuard(const Machine &machine,
                                               const QString &managedRoot,
                                               const QList<Machine> &catalogMachines)
{
    return managedDeletionGuardImpl(machine, managedRoot, catalogMachines,
                                    DeletionTargetPresence::MustBePresent);
}

DeletionGuard PathPolicy::managedDeletionGuardAfterUnregister(
    const Machine &machine,
    const QString &managedRoot,
    const QList<Machine> &catalogMachines)
{
    return managedDeletionGuardImpl(machine, managedRoot, catalogMachines,
                                    DeletionTargetPresence::MustBeAbsent);
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
    ManagedPathLease lease;
    if (!lease.acquireDeletion(guard.canonicalRoot, guard.canonicalDirectory,
                               guard.rootIdentity, expectedIdentity, error)) {
        return false;
    }
    return deleteManagedDirectoryWithLease(guard, lease, error);
}

Catalog::Catalog(QString path) : m_path(std::move(path)) {}
Catalog::~Catalog() = default;
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
            existing.storageDevices.clear();
            existing.networkDevices.clear();
            existing.cpuCount.reset();
            existing.memoryMiB.reset();
            existing.firmware.reset();
            existing.secureBoot.reset();
            existing.tpm.reset();
            existing.stateRevision.clear();
            existing.inventoryComplete = false;
            existing.hardwareInventoryComplete = false;
            setError(error, {});
            return true;
        }
    }
    Machine portable = machine;
    portable.powerState = PowerState::Unknown;
    portable.inaccessibleReason.clear();
    portable.warnings.clear();
    portable.storagePaths.clear();
    portable.storageDevices.clear();
    portable.networkDevices.clear();
    portable.cpuCount.reset();
    portable.memoryMiB.reset();
    portable.firmware.reset();
    portable.secureBoot.reset();
    portable.tpm.reset();
    portable.stateRevision.clear();
    portable.inventoryComplete = false;
    portable.hardwareInventoryComplete = false;
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
    constexpr qint64 MaxCatalogBytes = 4 * 1024 * 1024;
    if (file.size() < 0 || file.size() > MaxCatalogBytes) {
        setError(error, QStringLiteral("VM catalog exceeds the 4 MiB safety limit."));
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
    constexpr qsizetype MaxCatalogMachines = 2048;
    if (root.value(QStringLiteral("machines")).toArray().size() > MaxCatalogMachines) {
        setError(error, QStringLiteral("VM catalog contains too many machine entries."));
        return false;
    }
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
        machine.ownershipToken = object.value(QStringLiteral("ownershipToken")).toString();
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
        constexpr qsizetype MaxCatalogText = 32768;
        if (machine.ref.providerId.size() > MaxCatalogText
            || machine.ref.id.size() > MaxCatalogText
            || machine.ref.name.size() > MaxCatalogText
            || machine.configPath.size() > MaxCatalogText
            || machine.ownershipToken.size() > 128
            || !machine.ref.valid() || !QFileInfo(machine.configPath).isAbsolute()
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
    std::unique_ptr<QLockFile> localLock;
    QLockFile *lock = m_transactionLock.get();
    if (!lock) {
        localLock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
        localLock->setStaleLockTime(30000);
        lock = localLock.get();
    }
    if (!lock->isLocked() && !lock->tryLock(5000)) {
        setError(error, QStringLiteral("VM catalog is locked by another writer: %1")
                            .arg(lock->error()));
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

bool Catalog::beginTransaction(const QString &expectedRevision, QString *error)
{
    if (m_transactionLock) {
        setError(error, QStringLiteral("A VM catalog transaction is already active."));
        return false;
    }
    if (expectedRevision.trimmed().isEmpty() || m_path.trimmed().isEmpty()
        || !QFileInfo(m_path).isAbsolute()
        || !QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        setError(error, QStringLiteral("VM catalog transaction path or revision is invalid."));
        return false;
    }
    auto lock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
    lock->setStaleLockTime(30000);
    if (!lock->tryLock(5000)) {
        setError(error, QStringLiteral("VM catalog is locked by another writer: %1")
                            .arg(lock->error()));
        return false;
    }
    QString loadError;
    if (!load(&loadError)) {
        setError(error, loadError);
        return false;
    }
    if (revision() != expectedRevision) {
        setError(error, QStringLiteral(
            "VM catalog changed after preview; refresh inventory and review again."));
        return false;
    }
    m_transactionLock = std::move(lock);
    setError(error, {});
    return true;
}

void Catalog::endTransaction()
{
    m_transactionLock.reset();
}

bool Catalog::transactionActive() const
{
    return m_transactionLock && m_transactionLock->isLocked();
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
    for (const FileEvidence &evidence : plan.filePreflight) {
        const QFileInfo file(evidence.path);
        if (!file.isAbsolute() || !file.exists() || !file.isFile()
            || evidence.expectedSize < 0 || file.size() != evidence.expectedSize
            || evidence.expectedLastModifiedMs < 0
            || file.lastModified().toMSecsSinceEpoch() != evidence.expectedLastModifiedMs
            || evidence.expectedIdentity.isEmpty()) {
            setError(error, QStringLiteral("Reviewed file changed or disappeared: %1")
                                .arg(evidence.description));
            return false;
        }
        QString identityError;
        const QString currentIdentity = pathIdentity(file.absoluteFilePath(), &identityError);
        if (!identityError.isEmpty() || currentIdentity != evidence.expectedIdentity) {
            setError(error, identityError.isEmpty()
                                ? QStringLiteral("Reviewed file identity changed: %1")
                                      .arg(evidence.description)
                                : identityError);
            return false;
        }
    }
    if (plan.managedCreateReservation && plan.managedDeletionAfterCommands) {
        setError(error, QStringLiteral(
            "A VM plan cannot create and delete managed paths in one execution."));
        return false;
    }
    if (plan.managedCreateReservation) {
        const ManagedCreateReservation &reservation = *plan.managedCreateReservation;
        const CreationGuard guard = PathPolicy::managedCreateGuard(
            reservation.managedRoot, reservation.targetDirectory);
        if (!guard.allowed) {
            setError(error, guard.error);
            return false;
        }
        if (reservation.expectedRootIdentity.isEmpty()
            || guard.rootIdentity != reservation.expectedRootIdentity
            || !samePath(guard.targetDirectory, reservation.targetDirectory)) {
            setError(error, QStringLiteral(
                "Managed create root or target changed after preview."));
            return false;
        }
        if (plan.managedOwnershipToken.isEmpty()) {
            setError(error, QStringLiteral(
                "Managed create plan lacks its durable ownership token."));
            return false;
        }
        const QString markerPath = QDir(guard.targetDirectory).filePath(
            managedOwnershipMarkerFileName());
        const QByteArray markerContents = managedOwnershipMarkerContents(
            preview.target, plan.managedOwnershipToken);
        const bool hasExactMarker = std::any_of(
            plan.atomicWritesAfterCommands.cbegin(),
            plan.atomicWritesAfterCommands.cend(),
            [&markerPath, &markerContents](const AtomicWrite &write) {
                return samePath(write.path, markerPath)
                    && write.expectedSha256.isEmpty()
                    && write.contents == markerContents;
            });
        if (!hasExactMarker) {
            setError(error, QStringLiteral(
                "Managed create plan lacks its exact ownership marker write."));
            return false;
        }
        for (const AtomicWrite &write : plan.atomicWritesAfterCommands) {
            if (!containedPath(guard.targetDirectory, write.path)) {
                setError(error, QStringLiteral(
                    "Managed create refuses an atomic write outside its leased target: %1")
                                    .arg(write.path));
                return false;
            }
        }
    }
    for (const QString &directory : plan.directoriesBeforeCommands) {
        QString directoryError;
        if (!exclusiveDirectoryIsAvailable(directory, &directoryError)) {
            setError(error, directoryError);
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
        if (preview.risk != Risk::Destructive
            || !plan.atomicWritesAfterCommands.isEmpty()
            || !plan.directoriesBeforeCommands.isEmpty()
            || (deletion.expectTargetAbsentAfterCommands
                && preview.commands.isEmpty())) {
            setError(error, QStringLiteral(
                "Managed deletion plan has an unsafe execution shape."));
            return false;
        }
        const DeletionGuard guard = PathPolicy::managedDeletionGuard(
            deletion.machine, deletion.managedRoot, deletion.catalogMachines);
        if (!guard.allowed) {
            setError(error, guard.error);
            return false;
        }
        if (deletion.expectedIdentity.isEmpty()
            || deletion.expectedRootIdentity.isEmpty()
            || guard.identity != deletion.expectedIdentity
            || guard.rootIdentity != deletion.expectedRootIdentity) {
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
                         CommandRunner &runner,
                         const ManagedInventoryRefresh &managedInventoryRefresh)
{
    Result result;
    if (!validate(plan, currentRevision, typedConfirmation, now, &result.error))
        return result;
    std::unique_ptr<ManagedPathLease> managedLease;
    if (plan.managedCreateReservation) {
        const ManagedCreateReservation &reservation = *plan.managedCreateReservation;
        managedLease = std::make_unique<ManagedPathLease>();
        if (!managedLease->reserveCreate(
                reservation.managedRoot, reservation.targetDirectory,
                reservation.expectedRootIdentity, &result.error)) {
            return result;
        }
    } else if (plan.managedDeletionAfterCommands) {
        const ManagedDeletion &deletion = *plan.managedDeletionAfterCommands;
        const DeletionGuard guard = PathPolicy::managedDeletionGuard(
            deletion.machine, deletion.managedRoot, deletion.catalogMachines);
        if (!guard.allowed) {
            result.error = guard.error;
            return result;
        }
        managedLease = std::make_unique<ManagedPathLease>();
        if (!managedLease->acquireDeletion(
                guard.canonicalRoot, guard.canonicalDirectory,
                deletion.expectedRootIdentity, deletion.expectedIdentity,
                &result.error)) {
            return result;
        }
    }
    const auto validateLease = [&managedLease, &result]() {
        return !managedLease || managedLease->validate(&result.error);
    };
    for (const FileEvidence &evidence : plan.filePreflight) {
        if (!validateLease())
            return result;
        FileEvidence verified = evidence;
        verified.expectedSha256 = fileSha256(evidence.path, &result.error);
        if (!result.error.isEmpty() || verified.expectedSha256.size() != 64)
            return result;
        result.verifiedFiles.append(std::move(verified));
    }
    for (const QString &directory : plan.directoriesBeforeCommands) {
        if (!validateLease())
            return result;
        if (!prepareExclusiveDirectory(directory, &result.error))
            return result;
    }
    for (const CommandEvidence &evidence : plan.preflight) {
        if (!validateLease())
            return result;
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
        if (!validateLease())
            return result;
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
        if (!validateLease())
            return result;
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
        if (!validateLease())
            return result;
    }
    std::optional<DeletionGuard> finalDeletionGuard;
    if (plan.managedDeletionAfterCommands) {
        if (!managedInventoryRefresh) {
            result.error = QStringLiteral(
                "Managed deletion requires a second complete provider inventory after provider commands.");
            return result;
        }
        QList<Machine> refreshedMachines;
        if (!managedInventoryRefresh(runner, &refreshedMachines, &result.error)) {
            if (result.error.isEmpty()) {
                result.error = QStringLiteral(
                    "Post-command provider inventory was incomplete; managed files were preserved.");
            }
            return result;
        }
        if (!validateLease())
            return result;
        const ManagedDeletion &deletion = *plan.managedDeletionAfterCommands;
        const DeletionGuard guard = deletion.expectTargetAbsentAfterCommands
            ? PathPolicy::managedDeletionGuardAfterUnregister(
                  deletion.machine, deletion.managedRoot, refreshedMachines)
            : PathPolicy::managedDeletionGuard(
                  deletion.machine, deletion.managedRoot, refreshedMachines);
        if (!guard.allowed) {
            result.error = guard.error;
            return result;
        }
        if (guard.identity != deletion.expectedIdentity
            || guard.rootIdentity != deletion.expectedRootIdentity) {
            result.error = QStringLiteral(
                "Managed root or target identity changed after provider commands.");
            return result;
        }
        finalDeletionGuard = guard;
    }
    for (const AtomicWrite &write : plan.atomicWritesAfterCommands) {
        if (!validateLease())
            return result;
        if (!atomicWriteRevisionMatches(write, &result.error) || !writeAtomic(write, &result.error))
            return result;
        if (!validateLease())
            return result;
    }
    if (finalDeletionGuard) {
        if (!managedLease
            || !deleteManagedDirectoryWithLease(*finalDeletionGuard, *managedLease,
                                                &result.error)) {
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

Plan makeForgetCatalogPlan(const Machine &machine,
                           const QString &revision,
                           const QDateTime &now)
{
    Plan plan;
    if (!machine.ref.valid()) {
        plan.errors.append(QStringLiteral(
            "A valid catalog VM identity is required before forgetting an entry."));
        return plan;
    }
    if (machine.ownership != Ownership::External) {
        plan.errors.append(QStringLiteral(
            "WimForge-managed VMs cannot be forgotten while their owned files remain. "
            "Delete the managed VM through the reviewed file-deletion workflow instead."));
        return plan;
    }
    if (revision.isEmpty() || !now.isValid()) {
        plan.errors.append(QStringLiteral(
            "Refresh catalog state before forgetting a VM entry."));
        return plan;
    }
    plan.preview = makePreview(
        QStringLiteral("forget"), machine.ref, Risk::Reversible,
        {QStringLiteral(
            "Remove only the external VM's WimForge catalog entry; provider registration and every VM file remain unchanged.")},
        {QStringLiteral(
            "This provider-independent repair action is available even when the VM configuration or provider is missing.")},
        {}, revision, now);
    return plan;
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

bool addFileEvidence(Plan &plan, const QString &path, const QString &description,
                     QString *error)
{
    const QFileInfo file(path);
    if (!file.isAbsolute() || !file.exists() || !file.isFile()) {
        setError(error, QStringLiteral("Reviewed file must be an existing absolute file: %1")
                            .arg(path));
        return false;
    }
    QString identityError;
    const QString identity = pathIdentity(file.absoluteFilePath(), &identityError);
    if (!identityError.isEmpty() || identity.isEmpty()) {
        setError(error, identityError.isEmpty()
                            ? QStringLiteral("Could not bind reviewed file identity.")
                            : identityError);
        return false;
    }
    plan.filePreflight.append(FileEvidence{
        file.absoluteFilePath(), {}, file.size(),
        file.lastModified().toMSecsSinceEpoch(), identity, description});
    setError(error, {});
    return true;
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
