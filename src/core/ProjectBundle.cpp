#include "ProjectBundle.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <vector>

namespace wimforge {
namespace {

constexpr quint32 HeaderFlags = 0;
constexpr quint64 HeaderSize = 16 + 4 + 4 + 8 + 8 + 32;
const QByteArray ArchiveMagic = QByteArrayLiteral("WIMFORGE-BUNDLE\x1a");
const QString FormatIdentifier = QStringLiteral("org.wimforge.project-bundle");
constexpr qsizetype CopyBufferSize = 1024 * 1024;

struct ArchiveEntry
{
    QString path;
    QString sourcePath;
    bool directory = false;
    quint64 size = 0;
    quint64 offset = 0;
    QByteArray sha256;
    int permissions = 0;
    qint64 modifiedMs = 0;
};

struct ParsedManifest
{
    QJsonObject json;
    std::vector<ArchiveEntry> entries;
    QMap<QString, QString> repositories;
    QStringList standaloneFiles;
    quint64 payloadBytes = 0;
};

class StagingDirectoryGuard
{
public:
    explicit StagingDirectoryGuard(QString path)
        : m_path(std::move(path))
    {
    }

    ~StagingDirectoryGuard()
    {
        if (m_active && !m_path.isEmpty())
            QDir(m_path).removeRecursively();
    }

    void release() { m_active = false; }

private:
    QString m_path;
    bool m_active = true;
};

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool fail(QString *error, const QString &message)
{
    setError(error, message);
    return false;
}

bool isLinkLike(const QFileInfo &info)
{
    if (info.isSymLink())
        return true;
#ifdef Q_OS_WIN
    if (info.isJunction())
        return true;
#endif
    return false;
}

bool isPathInside(const QString &childPath, const QString &parentPath)
{
    const QString child = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(childPath).absoluteFilePath()));
    QString parent = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(parentPath).absoluteFilePath()));
    if (child.compare(parent, Qt::CaseInsensitive) == 0)
        return true;
    if (!parent.endsWith(QLatin1Char('/')))
        parent.append(QLatin1Char('/'));
    return child.startsWith(parent, Qt::CaseInsensitive);
}

bool isPortableSegment(const QString &segment)
{
    if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral(".."))
        return false;
    if (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' ')))
        return false;

    for (const QChar character : segment) {
        if (character.unicode() < 0x20 || character == QLatin1Char(':')
            || character == QLatin1Char('*') || character == QLatin1Char('?')
            || character == QLatin1Char('"') || character == QLatin1Char('<')
            || character == QLatin1Char('>') || character == QLatin1Char('|')) {
            return false;
        }
    }

    QString deviceName = segment;
    const qsizetype dot = deviceName.indexOf(QLatin1Char('.'));
    if (dot >= 0)
        deviceName.truncate(dot);
    deviceName = deviceName.toUpper();
    static const QSet<QString> reservedNames = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9"),
    };
    return !reservedNames.contains(deviceName);
}

bool normaliseArchivePath(const QString &input,
                          QString *normalised,
                          QString *reason,
                          bool requireCanonicalSpelling)
{
    if (input.isEmpty()) {
        setError(reason, QStringLiteral("Archive paths cannot be empty."));
        return false;
    }
    if (input.contains(QChar::Null) || input.startsWith(QLatin1Char('/'))
        || input.startsWith(QLatin1Char('\\')) || QDir::isAbsolutePath(input)) {
        setError(reason, QStringLiteral("Archive paths must be relative."));
        return false;
    }

    QString path = input;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &segment : segments) {
        if (!isPortableSegment(segment)) {
            setError(reason,
                     QStringLiteral("Archive path contains an unsafe or non-portable segment."));
            return false;
        }
    }

    const QString clean = QDir::cleanPath(path);
    if (clean == QStringLiteral(".") || clean.startsWith(QStringLiteral("../"))
        || clean == QStringLiteral("..")) {
        setError(reason, QStringLiteral("Archive path escapes the bundle root."));
        return false;
    }
    if (requireCanonicalSpelling && clean != input) {
        setError(reason, QStringLiteral("Archive path is not in canonical forward-slash form."));
        return false;
    }

    if (normalised)
        *normalised = clean;
    return true;
}

bool parseUnsignedString(const QJsonValue &value, quint64 *number)
{
    if (!value.isString())
        return false;
    bool ok = false;
    const quint64 parsed = value.toString().toULongLong(&ok, 10);
    if (!ok)
        return false;
    if (number)
        *number = parsed;
    return true;
}

bool parseSignedString(const QJsonValue &value, qint64 *number)
{
    if (!value.isString())
        return false;
    bool ok = false;
    const qint64 parsed = value.toString().toLongLong(&ok, 10);
    if (!ok)
        return false;
    if (number)
        *number = parsed;
    return true;
}

bool hashFile(const QString &path, QByteArray *digest, quint64 *size, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error,
                    QStringLiteral("Cannot read '%1': %2").arg(path, file.errorString()));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 total = 0;
    QByteArray buffer(CopyBufferSize, Qt::Uninitialized);
    while (true) {
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0)
            return fail(error,
                        QStringLiteral("Cannot read '%1': %2").arg(path, file.errorString()));
        if (count == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), count));
        total += static_cast<quint64>(count);
    }
    if (digest)
        *digest = hash.result();
    if (size)
        *size = total;
    return true;
}

bool makeEntry(const QFileInfo &info,
               const QString &archivePath,
               ArchiveEntry *entry,
               QString *error)
{
    if (!entry)
        return fail(error, QStringLiteral("Internal error while creating a bundle entry."));
    if (!info.exists())
        return fail(error, QStringLiteral("Source path does not exist: %1").arg(info.filePath()));
    if (isLinkLike(info))
        return fail(error,
                    QStringLiteral("Symbolic links and reparse-point directories are not allowed: %1")
                        .arg(info.filePath()));
    if (!info.isDir() && !info.isFile())
        return fail(error,
                    QStringLiteral("Only ordinary files and directories can be bundled: %1")
                        .arg(info.filePath()));

    entry->path = archivePath;
    entry->sourcePath = info.absoluteFilePath();
    entry->directory = info.isDir();
    entry->permissions = info.permissions().toInt();
    entry->modifiedMs = info.lastModified().toMSecsSinceEpoch();
    if (!entry->directory && !hashFile(entry->sourcePath, &entry->sha256, &entry->size, error))
        return false;
    return true;
}

bool collectDirectory(const QString &sourceDirectory,
                      const QString &archiveDirectory,
                      const QString &canonicalRoot,
                      std::vector<ArchiveEntry> *entries,
                      QString *error)
{
    const QFileInfo directoryInfo(sourceDirectory);
    ArchiveEntry directoryEntry;
    if (!makeEntry(directoryInfo, archiveDirectory, &directoryEntry, error))
        return false;

    const QString canonicalDirectory = directoryInfo.canonicalFilePath();
    if (canonicalDirectory.isEmpty() || !isPathInside(canonicalDirectory, canonicalRoot)) {
        return fail(error,
                    QStringLiteral("A source directory escapes its declared root: %1")
                        .arg(sourceDirectory));
    }
    entries->push_back(std::move(directoryEntry));

    QDir directory(sourceDirectory);
    const QFileInfoList children = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &child : children) {
        if (isLinkLike(child)) {
            return fail(error,
                        QStringLiteral("Symbolic links and reparse points are refused: %1")
                            .arg(child.absoluteFilePath()));
        }
        const QString childArchive = archiveDirectory + QLatin1Char('/') + child.fileName();
        if (child.isDir()) {
            if (!collectDirectory(child.absoluteFilePath(),
                                  childArchive,
                                  canonicalRoot,
                                  entries,
                                  error)) {
                return false;
            }
        } else {
            ArchiveEntry fileEntry;
            if (!makeEntry(child, childArchive, &fileEntry, error))
                return false;
            entries->push_back(std::move(fileEntry));
        }
    }
    return true;
}

void addVirtualParentDirectories(std::vector<ArchiveEntry> *entries)
{
    QSet<QString> existing;
    for (const ArchiveEntry &entry : *entries)
        existing.insert(entry.path.toCaseFolded());

    std::vector<ArchiveEntry> parents;
    const qint64 timestamp = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    for (const ArchiveEntry &entry : *entries) {
        QString parent = entry.path.section(QLatin1Char('/'), 0, -2);
        while (!parent.isEmpty()) {
            const QString key = parent.toCaseFolded();
            if (!existing.contains(key)) {
                ArchiveEntry virtualDirectory;
                virtualDirectory.path = parent;
                virtualDirectory.directory = true;
                virtualDirectory.permissions =
                    (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                     | QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser)
                        .toInt();
                virtualDirectory.modifiedMs = timestamp;
                parents.push_back(std::move(virtualDirectory));
                existing.insert(key);
            }
            parent = parent.section(QLatin1Char('/'), 0, -2);
        }
    }
    entries->insert(entries->end(), parents.begin(), parents.end());
}

bool validateEntryLayout(const std::vector<ArchiveEntry> &entries, QString *error)
{
    QMap<QString, bool> kinds;
    for (const ArchiveEntry &entry : entries) {
        QString normalised;
        QString reason;
        if (!normaliseArchivePath(entry.path, &normalised, &reason, true)) {
            return fail(error,
                        QStringLiteral("Unsafe archive path '%1': %2").arg(entry.path, reason));
        }
        const QString key = normalised.toCaseFolded();
        if (kinds.contains(key)) {
            return fail(error,
                        QStringLiteral("Archive paths collide on a case-insensitive file system: %1")
                            .arg(normalised));
        }
        kinds.insert(key, entry.directory);
    }

    for (const ArchiveEntry &entry : entries) {
        QString parent = entry.path.section(QLatin1Char('/'), 0, -2);
        while (!parent.isEmpty()) {
            const auto found = kinds.constFind(parent.toCaseFolded());
            if (found == kinds.cend() || !found.value()) {
                return fail(error,
                            QStringLiteral("Archive entry has a missing or non-directory parent: %1")
                                .arg(entry.path));
            }
            parent = parent.section(QLatin1Char('/'), 0, -2);
        }
    }
    return true;
}

bool hasPathPrefix(const QString &path, const QString &parent)
{
    return path.compare(parent, Qt::CaseInsensitive) == 0
        || path.startsWith(parent + QLatin1Char('/'), Qt::CaseInsensitive);
}

QJsonObject createManifest(const std::vector<ArchiveEntry> &entries,
                           const QList<ProjectBundleRepository> &repositories,
                           const QList<ProjectBundleFile> &standaloneFiles,
                           quint64 payloadBytes)
{
    QJsonArray repositoryArray;
    for (const ProjectBundleRepository &repository : repositories) {
        QJsonObject object;
        object.insert(QStringLiteral("role"), repository.role);
        object.insert(QStringLiteral("root"), repository.archivePath);
        repositoryArray.append(object);
    }

    QJsonArray standaloneArray;
    for (const ProjectBundleFile &file : standaloneFiles)
        standaloneArray.append(file.archivePath);

    QJsonArray entryArray;
    for (const ArchiveEntry &entry : entries) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), entry.path);
        object.insert(QStringLiteral("kind"),
                      entry.directory ? QStringLiteral("directory") : QStringLiteral("file"));
        object.insert(QStringLiteral("permissions"), QString::number(entry.permissions));
        object.insert(QStringLiteral("modifiedMs"), QString::number(entry.modifiedMs));
        if (!entry.directory) {
            object.insert(QStringLiteral("offset"), QString::number(entry.offset));
            object.insert(QStringLiteral("size"), QString::number(entry.size));
            object.insert(QStringLiteral("sha256"),
                          QString::fromLatin1(entry.sha256.toHex()));
        }
        entryArray.append(object);
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), FormatIdentifier);
    manifest.insert(QStringLiteral("formatVersion"), ProjectBundle::CurrentFormatVersion);
    manifest.insert(QStringLiteral("createdUtc"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("generator"), QStringLiteral("WimForge"));
    manifest.insert(QStringLiteral("payloadBytes"), QString::number(payloadBytes));
    manifest.insert(QStringLiteral("repositories"), repositoryArray);
    manifest.insert(QStringLiteral("standaloneFiles"), standaloneArray);
    manifest.insert(QStringLiteral("entries"), entryArray);
    return manifest;
}

bool writeHeader(QIODevice *device,
                 quint64 manifestBytes,
                 quint64 payloadBytes,
                 const QByteArray &manifestDigest,
                 QString *error)
{
    if (device->write(ArchiveMagic) != ArchiveMagic.size())
        return fail(error, QStringLiteral("Could not write the bundle header."));
    QDataStream stream(device);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(ProjectBundle::CurrentFormatVersion) << HeaderFlags << manifestBytes
           << payloadBytes;
    if (stream.status() != QDataStream::Ok || manifestDigest.size() != 32
        || device->write(manifestDigest) != manifestDigest.size()) {
        return fail(error, QStringLiteral("Could not write the bundle header."));
    }
    return true;
}

bool copyAndVerifyFile(const ArchiveEntry &entry, QIODevice *output, QString *error)
{
    QFile input(entry.sourcePath);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(error,
                    QStringLiteral("Cannot reopen '%1' for export: %2")
                        .arg(entry.sourcePath, input.errorString()));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 total = 0;
    QByteArray buffer(CopyBufferSize, Qt::Uninitialized);
    while (true) {
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count < 0) {
            return fail(error,
                        QStringLiteral("Cannot read '%1': %2")
                            .arg(entry.sourcePath, input.errorString()));
        }
        if (count == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), count));
        qint64 written = 0;
        while (written < count) {
            const qint64 amount = output->write(buffer.constData() + written, count - written);
            if (amount <= 0)
                return fail(error, QStringLiteral("Could not write the bundle payload."));
            written += amount;
        }
        total += static_cast<quint64>(count);
    }
    if (total != entry.size || hash.result() != entry.sha256) {
        return fail(error,
                    QStringLiteral("Source changed during export; retry after pausing writers: %1")
                        .arg(entry.sourcePath));
    }
    return true;
}

bool readExactly(QIODevice *device, char *data, quint64 size, QString *error)
{
    quint64 readTotal = 0;
    while (readTotal < size) {
        const quint64 remaining = size - readTotal;
        const qint64 requested = static_cast<qint64>(
            std::min<quint64>(remaining, static_cast<quint64>(std::numeric_limits<qint64>::max())));
        const qint64 count = device->read(data + static_cast<qsizetype>(readTotal), requested);
        if (count <= 0)
            return fail(error, QStringLiteral("Bundle is truncated or unreadable."));
        readTotal += static_cast<quint64>(count);
    }
    return true;
}

bool parseManifest(const QJsonObject &json,
                   quint64 headerPayloadBytes,
                   const ProjectBundleImportOptions &options,
                   ParsedManifest *parsed,
                   QString *error)
{
    if (json.value(QStringLiteral("format")).toString() != FormatIdentifier)
        return fail(error, QStringLiteral("This is not a WimForge project bundle manifest."));
    if (json.value(QStringLiteral("formatVersion")).toInt(-1)
        != ProjectBundle::CurrentFormatVersion) {
        return fail(error, QStringLiteral("The project bundle format version is not supported."));
    }

    quint64 manifestPayloadBytes = 0;
    if (!parseUnsignedString(json.value(QStringLiteral("payloadBytes")),
                             &manifestPayloadBytes)
        || manifestPayloadBytes != headerPayloadBytes
        || manifestPayloadBytes > options.maximumTotalBytes) {
        return fail(error, QStringLiteral("The manifest payload length is invalid."));
    }

    const QJsonValue repositoriesValue = json.value(QStringLiteral("repositories"));
    const QJsonValue standaloneValue = json.value(QStringLiteral("standaloneFiles"));
    const QJsonValue entriesValue = json.value(QStringLiteral("entries"));
    if (!repositoriesValue.isArray() || !standaloneValue.isArray() || !entriesValue.isArray())
        return fail(error, QStringLiteral("The bundle manifest is missing required arrays."));

    const QJsonArray entriesArray = entriesValue.toArray();
    if (static_cast<quint64>(entriesArray.size()) > options.maximumEntries)
        return fail(error, QStringLiteral("The bundle contains too many entries."));

    ParsedManifest result;
    result.json = json;
    result.payloadBytes = headerPayloadBytes;
    result.entries.reserve(static_cast<size_t>(entriesArray.size()));
    QSet<QString> paths;
    QMap<QString, bool> pathKinds;
    quint64 expectedOffset = 0;
    const QRegularExpression digestPattern(QStringLiteral("^[0-9a-fA-F]{64}$"));

    for (const QJsonValue &entryValue : entriesArray) {
        if (!entryValue.isObject())
            return fail(error, QStringLiteral("A bundle entry is not an object."));
        const QJsonObject object = entryValue.toObject();
        if (!object.value(QStringLiteral("path")).isString()
            || !object.value(QStringLiteral("kind")).isString()) {
            return fail(error, QStringLiteral("A bundle entry is missing its path or kind."));
        }

        ArchiveEntry entry;
        entry.path = object.value(QStringLiteral("path")).toString();
        QString pathReason;
        QString canonicalPath;
        if (!normaliseArchivePath(entry.path, &canonicalPath, &pathReason, true)) {
            return fail(error,
                        QStringLiteral("Unsafe bundle path '%1': %2").arg(entry.path, pathReason));
        }
        const QString key = canonicalPath.toCaseFolded();
        if (paths.contains(key)) {
            return fail(error,
                        QStringLiteral("Bundle paths collide on this platform: %1").arg(entry.path));
        }
        paths.insert(key);

        const QString kind = object.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("directory"))
            entry.directory = true;
        else if (kind == QStringLiteral("file"))
            entry.directory = false;
        else
            return fail(error, QStringLiteral("A bundle entry has an unknown kind."));

        bool permissionsOk = false;
        entry.permissions = object.value(QStringLiteral("permissions")).toString().toInt(
            &permissionsOk, 10);
        if (!permissionsOk
            || !parseSignedString(object.value(QStringLiteral("modifiedMs")), &entry.modifiedMs)) {
            return fail(error, QStringLiteral("A bundle entry has invalid metadata."));
        }

        if (!entry.directory) {
            if (!parseUnsignedString(object.value(QStringLiteral("offset")), &entry.offset)
                || !parseUnsignedString(object.value(QStringLiteral("size")), &entry.size)
                || !object.value(QStringLiteral("sha256")).isString()) {
                return fail(error, QStringLiteral("A file entry has invalid payload metadata."));
            }
            const QString digest = object.value(QStringLiteral("sha256")).toString();
            if (!digestPattern.match(digest).hasMatch() || entry.offset != expectedOffset
                || entry.size > options.maximumFileBytes
                || entry.size > headerPayloadBytes - expectedOffset) {
                return fail(error, QStringLiteral("A file entry has an invalid range or checksum."));
            }
            entry.sha256 = QByteArray::fromHex(digest.toLatin1());
            expectedOffset += entry.size;
        }
        pathKinds.insert(key, entry.directory);
        result.entries.push_back(std::move(entry));
    }
    if (expectedOffset != headerPayloadBytes)
        return fail(error, QStringLiteral("The manifest does not describe the complete payload."));

    for (const ArchiveEntry &entry : result.entries) {
        QString parent = entry.path.section(QLatin1Char('/'), 0, -2);
        while (!parent.isEmpty()) {
            const auto found = pathKinds.constFind(parent.toCaseFolded());
            if (found == pathKinds.cend() || !found.value()) {
                return fail(error,
                            QStringLiteral("Bundle entry has a missing or unsafe parent: %1")
                                .arg(entry.path));
            }
            parent = parent.section(QLatin1Char('/'), 0, -2);
        }
    }

    const QRegularExpression rolePattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    QStringList repositoryRoots;
    for (const QJsonValue &repositoryValue : repositoriesValue.toArray()) {
        if (!repositoryValue.isObject())
            return fail(error, QStringLiteral("A repository declaration is invalid."));
        const QJsonObject object = repositoryValue.toObject();
        const QString role = object.value(QStringLiteral("role")).toString();
        const QString root = object.value(QStringLiteral("root")).toString();
        QString canonicalRoot;
        QString pathReason;
        if (!rolePattern.match(role).hasMatch()
            || !normaliseArchivePath(root, &canonicalRoot, &pathReason, true)
            || result.repositories.contains(role)) {
            return fail(error, QStringLiteral("A repository role or root is invalid."));
        }
        const auto rootKind = pathKinds.constFind(canonicalRoot.toCaseFolded());
        const auto gitKind =
            pathKinds.constFind((canonicalRoot + QStringLiteral("/.git")).toCaseFolded());
        if (rootKind == pathKinds.cend() || !rootKind.value() || gitKind == pathKinds.cend()
            || !gitKind.value()) {
            return fail(error,
                        QStringLiteral("Repository '%1' does not contain a full .git directory.")
                            .arg(role));
        }
        for (const QString &otherRoot : repositoryRoots) {
            if (hasPathPrefix(canonicalRoot, otherRoot) || hasPathPrefix(otherRoot, canonicalRoot)) {
                return fail(error,
                            QStringLiteral("Repository roots cannot overlap inside a bundle."));
            }
        }
        repositoryRoots.append(canonicalRoot);
        result.repositories.insert(role, canonicalRoot);
    }
    if (result.repositories.isEmpty())
        return fail(error, QStringLiteral("A project bundle must contain at least one repository."));

    QSet<QString> standalonePaths;
    for (const QJsonValue &fileValue : standaloneValue.toArray()) {
        if (!fileValue.isString())
            return fail(error, QStringLiteral("A standalone file declaration is invalid."));
        const QString path = fileValue.toString();
        QString canonicalPath;
        QString pathReason;
        if (!normaliseArchivePath(path, &canonicalPath, &pathReason, true)
            || standalonePaths.contains(canonicalPath.toCaseFolded())) {
            return fail(error, QStringLiteral("A standalone file path is invalid."));
        }
        const auto kind = pathKinds.constFind(canonicalPath.toCaseFolded());
        if (kind == pathKinds.cend() || kind.value())
            return fail(error, QStringLiteral("A declared standalone file is missing."));
        for (const QString &repositoryRoot : repositoryRoots) {
            if (hasPathPrefix(canonicalPath, repositoryRoot)) {
                return fail(error,
                            QStringLiteral("Standalone files cannot overlap repository roots."));
            }
        }
        standalonePaths.insert(canonicalPath.toCaseFolded());
        result.standaloneFiles.append(canonicalPath);
    }

    for (const ArchiveEntry &entry : result.entries) {
        if (entry.directory)
            continue;
        bool declared = standalonePaths.contains(entry.path.toCaseFolded());
        for (const QString &repositoryRoot : repositoryRoots) {
            if (hasPathPrefix(entry.path, repositoryRoot)) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            return fail(error,
                        QStringLiteral("Bundle contains an undeclared file payload: %1")
                            .arg(entry.path));
        }
    }

    if (parsed)
        *parsed = std::move(result);
    return true;
}

bool extractFile(QIODevice *input,
                 const ArchiveEntry &entry,
                 const QString &targetPath,
                 QString *error)
{
    QSaveFile output(targetPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        return fail(error,
                    QStringLiteral("Cannot create '%1': %2").arg(targetPath, output.errorString()));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 remaining = entry.size;
    QByteArray buffer(CopyBufferSize, Qt::Uninitialized);
    while (remaining > 0) {
        const qint64 requested = static_cast<qint64>(
            std::min<quint64>(remaining, static_cast<quint64>(buffer.size())));
        const qint64 count = input->read(buffer.data(), requested);
        if (count <= 0) {
            output.cancelWriting();
            return fail(error, QStringLiteral("Bundle payload is truncated."));
        }
        hash.addData(QByteArrayView(buffer.constData(), count));
        if (output.write(buffer.constData(), count) != count) {
            output.cancelWriting();
            return fail(error,
                        QStringLiteral("Cannot write '%1': %2")
                            .arg(targetPath, output.errorString()));
        }
        remaining -= static_cast<quint64>(count);
    }
    if (hash.result() != entry.sha256) {
        output.cancelWriting();
        return fail(error,
                    QStringLiteral("Checksum mismatch while importing '%1'.").arg(entry.path));
    }
    if (!output.commit())
        return fail(error,
                    QStringLiteral("Cannot commit '%1': %2").arg(targetPath, output.errorString()));

    QFile timeFile(targetPath);
    if (timeFile.open(QIODevice::ReadWrite)) {
        timeFile.setFileTime(QDateTime::fromMSecsSinceEpoch(entry.modifiedMs, QTimeZone::UTC),
                             QFileDevice::FileModificationTime);
        timeFile.close();
    }
    QFile::setPermissions(targetPath, QFileDevice::Permissions::fromInt(entry.permissions));
    return true;
}

} // namespace

bool ProjectBundle::exportToFile(const QString &bundleFile,
                                 const QList<ProjectBundleRepository> &repositories,
                                 const QList<ProjectBundleFile> &standaloneFiles,
                                 QString *error)
{
    setError(error, {});
    const QFileInfo bundleInfo(bundleFile);
    if (!bundleInfo.isAbsolute())
        return fail(error, QStringLiteral("The bundle destination must be an absolute path."));
    if (bundleInfo.exists() && (bundleInfo.isDir() || isLinkLike(bundleInfo)))
        return fail(error, QStringLiteral("The bundle destination is not a safe ordinary file."));
    if (repositories.isEmpty())
        return fail(error, QStringLiteral("A project bundle must contain at least one repository."));

    QList<ProjectBundleRepository> normalisedRepositories;
    QSet<QString> roles;
    QStringList repositoryRoots;
    QStringList repositorySources;
    const QRegularExpression rolePattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    std::vector<ArchiveEntry> entries;

    for (const ProjectBundleRepository &input : repositories) {
        ProjectBundleRepository repository = input;
        QString pathReason;
        if (!rolePattern.match(repository.role).hasMatch() || roles.contains(repository.role))
            return fail(error, QStringLiteral("Repository roles must be unique portable identifiers."));
        if (!normaliseArchivePath(repository.archivePath,
                                  &repository.archivePath,
                                  &pathReason,
                                  false)) {
            return fail(error,
                        QStringLiteral("Invalid repository archive path: %1").arg(pathReason));
        }
        for (const QString &root : repositoryRoots) {
            if (hasPathPrefix(repository.archivePath, root)
                || hasPathPrefix(root, repository.archivePath)) {
                return fail(error, QStringLiteral("Repository archive roots cannot overlap."));
            }
        }

        const QFileInfo sourceInfo(repository.sourceDirectory);
        if (!sourceInfo.isAbsolute() || !sourceInfo.exists() || !sourceInfo.isDir()
            || isLinkLike(sourceInfo)) {
            return fail(error,
                        QStringLiteral("Repository source is not a safe absolute directory: %1")
                            .arg(repository.sourceDirectory));
        }
        const QString canonicalSource = sourceInfo.canonicalFilePath();
        const QFileInfo gitInfo(QDir(canonicalSource).filePath(QStringLiteral(".git")));
        if (!gitInfo.exists() || !gitInfo.isDir() || isLinkLike(gitInfo)) {
            return fail(error,
                        QStringLiteral("Repository '%1' must contain its complete .git directory.")
                            .arg(repository.role));
        }
        if (isPathInside(bundleInfo.absoluteFilePath(), canonicalSource)) {
            return fail(error,
                        QStringLiteral("The output bundle cannot be written inside a bundled repository."));
        }

        repository.sourceDirectory = canonicalSource;
        if (!collectDirectory(canonicalSource,
                              repository.archivePath,
                              canonicalSource,
                              &entries,
                              error)) {
            return false;
        }
        roles.insert(repository.role);
        repositoryRoots.append(repository.archivePath);
        repositorySources.append(canonicalSource);
        normalisedRepositories.append(std::move(repository));
    }

    QList<ProjectBundleFile> normalisedFiles;
    for (const ProjectBundleFile &input : standaloneFiles) {
        ProjectBundleFile file = input;
        QString pathReason;
        if (!normaliseArchivePath(file.archivePath, &file.archivePath, &pathReason, false))
            return fail(error, QStringLiteral("Invalid standalone archive path: %1").arg(pathReason));
        for (const QString &root : repositoryRoots) {
            if (hasPathPrefix(file.archivePath, root)) {
                return fail(error,
                            QStringLiteral("Standalone files cannot overlap repository roots."));
            }
        }
        const QFileInfo sourceInfo(file.sourcePath);
        if (!sourceInfo.isAbsolute() || !sourceInfo.exists() || !sourceInfo.isFile()
            || isLinkLike(sourceInfo)) {
            return fail(error,
                        QStringLiteral("Standalone source is not a safe ordinary file: %1")
                            .arg(file.sourcePath));
        }
        if (bundleInfo.absoluteFilePath().compare(sourceInfo.absoluteFilePath(),
                                                  Qt::CaseInsensitive)
            == 0) {
            return fail(error, QStringLiteral("The bundle cannot contain itself."));
        }
        ArchiveEntry entry;
        if (!makeEntry(sourceInfo, file.archivePath, &entry, error))
            return false;
        entries.push_back(std::move(entry));
        file.sourcePath = sourceInfo.canonicalFilePath();
        normalisedFiles.append(std::move(file));
    }

    addVirtualParentDirectories(&entries);
    std::sort(entries.begin(), entries.end(), [](const ArchiveEntry &left, const ArchiveEntry &right) {
        return left.path.compare(right.path, Qt::CaseSensitive) < 0;
    });
    if (!validateEntryLayout(entries, error))
        return false;

    quint64 payloadBytes = 0;
    for (ArchiveEntry &entry : entries) {
        if (entry.directory)
            continue;
        if (entry.size > std::numeric_limits<quint64>::max() - payloadBytes)
            return fail(error, QStringLiteral("The bundle payload is too large."));
        entry.offset = payloadBytes;
        payloadBytes += entry.size;
    }

    const QJsonObject manifest =
        createManifest(entries, normalisedRepositories, normalisedFiles, payloadBytes);
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    const QByteArray manifestDigest =
        QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256);

    const QString parentDirectory = bundleInfo.absoluteDir().absolutePath();
    if (!QDir().mkpath(parentDirectory))
        return fail(error, QStringLiteral("Cannot create the bundle destination directory."));

    QSaveFile output(bundleInfo.absoluteFilePath());
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return fail(error,
                    QStringLiteral("Cannot create bundle: %1").arg(output.errorString()));
    if (!writeHeader(&output,
                     static_cast<quint64>(manifestBytes.size()),
                     payloadBytes,
                     manifestDigest,
                     error)
        || output.write(manifestBytes) != manifestBytes.size()) {
        output.cancelWriting();
        if (error && error->isEmpty())
            *error = QStringLiteral("Could not write the bundle manifest.");
        return false;
    }
    for (const ArchiveEntry &entry : entries) {
        if (!entry.directory && !copyAndVerifyFile(entry, &output, error)) {
            output.cancelWriting();
            return false;
        }
    }
    if (!output.commit())
        return fail(error,
                    QStringLiteral("Could not atomically save the bundle: %1")
                        .arg(output.errorString()));
    return true;
}

std::optional<ProjectBundleImportResult>
ProjectBundle::importFromFile(const QString &bundleFile,
                              const QString &destinationDirectory,
                              const ProjectBundleImportOptions &options,
                              QString *error)
{
    setError(error, {});
    const QFileInfo bundleInfo(bundleFile);
    if (!bundleInfo.isAbsolute() || !bundleInfo.exists() || !bundleInfo.isFile()
        || isLinkLike(bundleInfo)) {
        fail(error, QStringLiteral("The bundle must be a safe, existing absolute file."));
        return std::nullopt;
    }
    QFileInfo destinationInfo(destinationDirectory);
    if (!destinationInfo.isAbsolute()) {
        fail(error, QStringLiteral("The import destination must be an absolute path."));
        return std::nullopt;
    }
    const QString destination = QDir::cleanPath(destinationInfo.absoluteFilePath());
    if (destinationInfo.fileName().isEmpty()
        || destination.compare(destinationInfo.absoluteDir().absolutePath(),
                               Qt::CaseInsensitive)
            == 0) {
        fail(error, QStringLiteral("A filesystem root cannot be used as an import destination."));
        return std::nullopt;
    }
    if (destinationInfo.exists()
        && (!destinationInfo.isDir() || isLinkLike(destinationInfo))) {
        fail(error, QStringLiteral("The import destination is not a safe ordinary directory."));
        return std::nullopt;
    }
    if (destinationInfo.exists() && !options.overwriteExisting) {
        fail(error, QStringLiteral("The import destination already exists."));
        return std::nullopt;
    }

    QFile input(bundleInfo.absoluteFilePath());
    if (!input.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("Cannot read bundle: %1").arg(input.errorString()));
        return std::nullopt;
    }
    if (static_cast<quint64>(input.size()) < HeaderSize) {
        fail(error, QStringLiteral("The bundle is truncated."));
        return std::nullopt;
    }
    const QByteArray magic = input.read(ArchiveMagic.size());
    if (magic != ArchiveMagic) {
        fail(error, QStringLiteral("The file is not a WimForge project bundle."));
        return std::nullopt;
    }

    quint32 version = 0;
    quint32 flags = 0;
    quint64 manifestLength = 0;
    quint64 payloadLength = 0;
    QDataStream stream(&input);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream >> version >> flags >> manifestLength >> payloadLength;
    if (stream.status() != QDataStream::Ok || version != CurrentFormatVersion
        || flags != HeaderFlags || manifestLength > options.maximumManifestBytes
        || manifestLength > static_cast<quint64>(std::numeric_limits<qsizetype>::max())
        || payloadLength > options.maximumTotalBytes
        || manifestLength > std::numeric_limits<quint64>::max() - HeaderSize
        || payloadLength > std::numeric_limits<quint64>::max() - HeaderSize - manifestLength
        || HeaderSize + manifestLength + payloadLength != static_cast<quint64>(input.size())) {
        fail(error, QStringLiteral("The bundle header or file length is invalid."));
        return std::nullopt;
    }

    QByteArray expectedManifestDigest(32, Qt::Uninitialized);
    if (!readExactly(&input,
                     expectedManifestDigest.data(),
                     static_cast<quint64>(expectedManifestDigest.size()),
                     error)) {
        return std::nullopt;
    }
    QByteArray manifestBytes(static_cast<qsizetype>(manifestLength), Qt::Uninitialized);
    if (!readExactly(&input, manifestBytes.data(), manifestLength, error))
        return std::nullopt;
    const QByteArray actualManifestDigest =
        QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256);
    if (actualManifestDigest != expectedManifestDigest) {
        fail(error, QStringLiteral("The bundle manifest checksum does not match."));
        return std::nullopt;
    }

    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error,
             QStringLiteral("The bundle manifest is invalid JSON: %1")
                 .arg(jsonError.errorString()));
        return std::nullopt;
    }
    ParsedManifest parsed;
    if (!parseManifest(document.object(), payloadLength, options, &parsed, error))
        return std::nullopt;

    destinationInfo = QFileInfo(destination);
    const QString parentDirectory = destinationInfo.absoluteDir().absolutePath();
    if (!QDir().mkpath(parentDirectory)) {
        fail(error, QStringLiteral("Cannot create the import destination parent."));
        return std::nullopt;
    }
    const QString staging = QDir(parentDirectory)
                                .filePath(QStringLiteral(".%1.wimforge-import-%2")
                                              .arg(destinationInfo.fileName(),
                                                   QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (QFileInfo::exists(staging) || !QDir().mkpath(staging)) {
        fail(error, QStringLiteral("Cannot create a private import staging directory."));
        return std::nullopt;
    }
    StagingDirectoryGuard stagingGuard(staging);
    const QString canonicalStaging = QFileInfo(staging).canonicalFilePath();

    // Create every directory first, but do not apply restrictive permissions
    // until all children have been written.
    for (const ArchiveEntry &entry : parsed.entries) {
        if (!entry.directory)
            continue;
        const QString target = QDir(staging).filePath(entry.path);
        if (!isPathInside(target, canonicalStaging) || !QDir().mkpath(target)) {
            fail(error, QStringLiteral("Cannot safely create directory '%1'.").arg(entry.path));
            return std::nullopt;
        }
    }

    for (const ArchiveEntry &entry : parsed.entries) {
        if (entry.directory)
            continue;
        const QString target = QDir(staging).filePath(entry.path);
        const QString parent = QFileInfo(target).absolutePath();
        const QString canonicalParent = QFileInfo(parent).canonicalFilePath();
        if (canonicalParent.isEmpty() || !isPathInside(canonicalParent, canonicalStaging)
            || isLinkLike(QFileInfo(parent)) || !extractFile(&input, entry, target, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("Refused an unsafe extraction path: %1").arg(entry.path);
            return std::nullopt;
        }
    }
    if (static_cast<quint64>(input.pos()) != HeaderSize + manifestLength + payloadLength) {
        fail(error, QStringLiteral("The bundle payload length did not match the manifest."));
        return std::nullopt;
    }

    std::vector<const ArchiveEntry *> directories;
    for (const ArchiveEntry &entry : parsed.entries) {
        if (entry.directory)
            directories.push_back(&entry);
    }
    std::sort(directories.begin(), directories.end(), [](const ArchiveEntry *left,
                                                          const ArchiveEntry *right) {
        return left->path.count(QLatin1Char('/')) > right->path.count(QLatin1Char('/'));
    });
    for (const ArchiveEntry *entry : directories) {
        QFile::setPermissions(QDir(staging).filePath(entry->path),
                              QFileDevice::Permissions::fromInt(entry->permissions));
    }

    QString backup;
    destinationInfo = QFileInfo(destination);
    if (destinationInfo.exists()) {
        if (!options.overwriteExisting || !destinationInfo.isDir() || isLinkLike(destinationInfo)) {
            fail(error, QStringLiteral("The destination changed during import; it was not replaced."));
            return std::nullopt;
        }
        backup = QDir(parentDirectory)
                     .filePath(QStringLiteral(".%1.wimforge-backup-%2")
                                   .arg(destinationInfo.fileName(),
                                        QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().rename(destination, backup)) {
            fail(error, QStringLiteral("Cannot move the existing destination to a safety backup."));
            return std::nullopt;
        }
    }

    if (!QDir().rename(staging, destination)) {
        const bool rolledBack = backup.isEmpty() || QDir().rename(backup, destination);
        fail(error,
             rolledBack
                 ? QStringLiteral("Cannot promote the validated import into place.")
                 : QStringLiteral("Cannot promote the import or restore the old destination; the old data remains at '%1'.")
                       .arg(backup));
        return std::nullopt;
    }
    stagingGuard.release();

    ProjectBundleImportResult result;
    result.formatVersion = static_cast<int>(version);
    result.destinationDirectory = destination;
    result.manifestSha256 = QString::fromLatin1(actualManifestDigest.toHex());
    result.manifest = parsed.json;
    for (auto iterator = parsed.repositories.cbegin(); iterator != parsed.repositories.cend();
         ++iterator) {
        result.repositoryPaths.insert(iterator.key(), QDir(destination).filePath(iterator.value()));
    }
    for (const QString &file : std::as_const(parsed.standaloneFiles))
        result.standaloneFiles.append(QDir(destination).filePath(file));

    if (!backup.isEmpty() && !QDir(backup).removeRecursively())
        result.retainedBackupPath = backup;
    return result;
}

} // namespace wimforge
