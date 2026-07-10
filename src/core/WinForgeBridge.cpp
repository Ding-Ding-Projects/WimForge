#include "WinForgeBridge.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QVersionNumber>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace wimforge {
namespace {

const QString RecipeFormat = QStringLiteral("org.wimforge.winforge-recipe");
const QString ContractFormat = QStringLiteral("org.winforge.runtime-contract");
const QString StageFormat = QStringLiteral("org.wimforge.winforge-iso-stage");
constexpr qsizetype MaximumJsonBytes = 16 * 1024 * 1024;
constexpr qsizetype CopyBufferSize = 1024 * 1024;

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

bool hasControlCharacter(const QString &value)
{
    return std::any_of(value.cbegin(), value.cend(), [](const QChar character) {
        return character == QChar::Null || character.unicode() < 0x20;
    });
}

bool isSafeIdentifier(const QString &value)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$)"));
    return expression.match(value).hasMatch();
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

Qt::CaseSensitivity fileSystemCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString cleanAbsolutePath(const QString &path)
{
    return QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

bool isPathInside(const QString &childPath, const QString &parentPath)
{
    const Qt::CaseSensitivity sensitivity = fileSystemCaseSensitivity();
    const QString child = cleanAbsolutePath(childPath);
    QString parent = cleanAbsolutePath(parentPath);
    if (child.compare(parent, sensitivity) == 0)
        return true;
    if (!parent.endsWith(QLatin1Char('/')))
        parent.append(QLatin1Char('/'));
    return child.startsWith(parent, sensitivity);
}

bool pathChainHasNoLinks(const QString &rootPath, const QString &targetPath, QString *error)
{
    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.isDir() || isLinkLike(rootInfo))
        return fail(error, QStringLiteral("Staging root is missing or is a link/reparse point."));
    if (!isPathInside(targetPath, rootPath))
        return fail(error, QStringLiteral("Staging target escapes its declared root."));
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return fail(error, QStringLiteral("Cannot resolve staging root."));

    const QString relative = QDir::fromNativeSeparators(
        QDir(rootPath).relativeFilePath(QFileInfo(targetPath).absoluteFilePath()));
    QString current = QFileInfo(rootPath).absoluteFilePath();
    for (const QString &segment : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(segment);
        const QFileInfo info(current);
        if (!info.exists())
            continue;
        if (isLinkLike(info))
            return fail(error, QStringLiteral("Staging path contains a link/reparse point: %1").arg(current));
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !isPathInside(canonical, canonicalRoot))
            return fail(error, QStringLiteral("Staging path resolves outside its root: %1").arg(current));
    }
    return true;
}

bool isPortableRelativePath(const QString &input, QString *normalised = nullptr)
{
    if (input.isEmpty() || input.contains(QChar::Null) || QDir::isAbsolutePath(input)
        || input.startsWith(QLatin1Char('/')) || input.startsWith(QLatin1Char('\\')))
        return false;

    QString path = input;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &segment : segments) {
        if (segment.isEmpty() || segment == QStringLiteral(".")
            || segment == QStringLiteral("..") || segment.endsWith(QLatin1Char('.'))
            || segment.endsWith(QLatin1Char(' '))) {
            return false;
        }
        for (const QChar character : segment) {
            if (character.unicode() < 0x20 || character == QLatin1Char(':')
                || character == QLatin1Char('*') || character == QLatin1Char('?')
                || character == QLatin1Char('"') || character == QLatin1Char('<')
                || character == QLatin1Char('>') || character == QLatin1Char('|')) {
                return false;
            }
        }
    }
    const QString clean = QDir::cleanPath(path);
    if (clean == QStringLiteral("..") || clean.startsWith(QStringLiteral("../")))
        return false;
    if (normalised)
        *normalised = clean;
    return true;
}

bool hasOnlyKeys(const QJsonObject &object,
                 const QSet<QString> &allowed,
                 const QString &context,
                 QString *error)
{
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!allowed.contains(iterator.key())) {
            return fail(error,
                        QStringLiteral("%1 contains unknown field '%2'.")
                            .arg(context, iterator.key()));
        }
    }
    return true;
}

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool hashFile(const QString &path, QString *digest, quint64 *size, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error,
                    QStringLiteral("Cannot read '%1': %2").arg(path, file.errorString()));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 total = 0;
    QByteArray buffer(CopyBufferSize, Qt::Uninitialized);
    while (true) {
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0)
            return fail(error, QStringLiteral("Cannot hash '%1'.").arg(path));
        if (count == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), count));
        total += static_cast<quint64>(count);
    }
    if (digest)
        *digest = QString::fromLatin1(hash.result().toHex());
    if (size)
        *size = total;
    return true;
}

bool writeBytesAtomically(const QString &path, const QByteArray &bytes, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QStringLiteral("Cannot create '%1'.").arg(QFileInfo(path).absolutePath()));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(error, QStringLiteral("Cannot write '%1': %2").arg(path, file.errorString()));
    }
    if (!file.commit())
        return fail(error, QStringLiteral("Cannot commit '%1': %2").arg(path, file.errorString()));
    return true;
}

std::optional<WinForgeActionKind> parseKind(const QString &name)
{
    if (name == QStringLiteral("module"))
        return WinForgeActionKind::Module;
    if (name == QStringLiteral("page"))
        return WinForgeActionKind::Page;
    if (name == QStringLiteral("tweak"))
        return WinForgeActionKind::Tweak;
    if (name == QStringLiteral("command"))
        return WinForgeActionKind::Command;
    if (name == QStringLiteral("registry"))
        return WinForgeActionKind::Registry;
    if (name == QStringLiteral("copy"))
        return WinForgeActionKind::Copy;
    return std::nullopt;
}

std::optional<WinForgeActionPhase> parsePhase(const QString &name)
{
    if (name == QStringLiteral("machine"))
        return WinForgeActionPhase::Machine;
    if (name == QStringLiteral("user"))
        return WinForgeActionPhase::User;
    return std::nullopt;
}

QString requiredCapability(WinForgeActionKind kind)
{
    switch (kind) {
    case WinForgeActionKind::Module:
        return QStringLiteral("apply.module.v1");
    case WinForgeActionKind::Page:
        return QStringLiteral("launch.page.v1");
    case WinForgeActionKind::Tweak:
        return QStringLiteral("apply.tweak.v1");
    case WinForgeActionKind::Command:
    case WinForgeActionKind::Registry:
    case WinForgeActionKind::Copy:
        return {};
    }
    return {};
}

QJsonArray stringArray(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.append(value);
    return result;
}

QJsonObject actionJsonWithoutDigest(const WinForgeAction &action)
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), action.enabled);
    object.insert(QStringLiteral("id"), action.id);
    object.insert(QStringLiteral("idempotencyKey"),
                  action.idempotencyKey.isEmpty() ? action.id : action.idempotencyKey);
    object.insert(QStringLiteral("kind"), WinForgeBridge::actionKindName(action.kind));
    object.insert(QStringLiteral("phase"), WinForgeBridge::actionPhaseName(action.phase));
    switch (action.kind) {
    case WinForgeActionKind::Module:
    case WinForgeActionKind::Page:
        object.insert(QStringLiteral("target"), action.target);
        break;
    case WinForgeActionKind::Tweak:
        object.insert(QStringLiteral("target"), action.target);
        object.insert(QStringLiteral("value"), action.value);
        break;
    case WinForgeActionKind::Command: {
        object.insert(QStringLiteral("executable"), action.executable);
        object.insert(QStringLiteral("arguments"), stringArray(action.arguments));
        if (!action.workingDirectory.isEmpty())
            object.insert(QStringLiteral("workingDirectory"), action.workingDirectory);
        QJsonArray exitCodes;
        for (const int code : action.successExitCodes)
            exitCodes.append(code);
        object.insert(QStringLiteral("successExitCodes"), exitCodes);
        break;
    }
    case WinForgeActionKind::Registry:
        object.insert(QStringLiteral("hive"), action.registryHive);
        object.insert(QStringLiteral("path"), action.registryPath);
        object.insert(QStringLiteral("name"), action.registryValueName);
        object.insert(QStringLiteral("type"), action.registryValueType);
        object.insert(QStringLiteral("value"), action.registryValue);
        break;
    case WinForgeActionKind::Copy:
        object.insert(QStringLiteral("source"), action.sourceRelative);
        object.insert(QStringLiteral("destination"), action.destination);
        object.insert(QStringLiteral("sha256"), action.sha256.toLower());
        object.insert(QStringLiteral("overwrite"), action.overwrite);
        break;
    }
    return object;
}

QString actionDigest(const WinForgeAction &action)
{
    return sha256(QJsonDocument(actionJsonWithoutDigest(action)).toJson(QJsonDocument::Compact));
}

QJsonObject actionJson(const WinForgeAction &action)
{
    QJsonObject object = actionJsonWithoutDigest(action);
    object.insert(QStringLiteral("digest"), actionDigest(action));
    return object;
}

bool isSafeCommand(const WinForgeAction &action, QString *reason)
{
    const QString executable = action.executable.trimmed();
    if (executable.isEmpty() || executable != action.executable || hasControlCharacter(executable))
        return fail(reason, QStringLiteral("Executable is empty or contains control/outer whitespace."));
    if (executable.contains(QLatin1Char('"')) || executable.contains(QLatin1Char('*'))
        || executable.contains(QLatin1Char('?')) || executable.contains(QLatin1Char('|'))
        || executable.contains(QLatin1Char('<')) || executable.contains(QLatin1Char('>')))
        return fail(reason, QStringLiteral("Executable contains shell or wildcard syntax."));
    if (executable.contains(QRegularExpression(QStringLiteral(R"(\s)")))
        && !QDir::isAbsolutePath(executable)
        && !executable.contains(QLatin1Char('/'))
        && !executable.contains(QLatin1Char('\\'))) {
        return fail(reason,
                    QStringLiteral("A relative executable cannot contain a bundled command line."));
    }

    const QString fileName = QFileInfo(executable).fileName().toLower();
    static const QSet<QString> interpreters = {
        QStringLiteral("cmd"),        QStringLiteral("cmd.exe"),
        QStringLiteral("powershell"), QStringLiteral("powershell.exe"),
        QStringLiteral("pwsh"),       QStringLiteral("pwsh.exe"),
        QStringLiteral("wscript"),    QStringLiteral("wscript.exe"),
        QStringLiteral("cscript"),    QStringLiteral("cscript.exe"),
        QStringLiteral("mshta"),      QStringLiteral("mshta.exe"),
        QStringLiteral("rundll32"),   QStringLiteral("rundll32.exe"),
        QStringLiteral("regsvr32"),   QStringLiteral("regsvr32.exe"),
        QStringLiteral("bash"),       QStringLiteral("bash.exe"),
        QStringLiteral("sh"),         QStringLiteral("sh.exe"),
    };
    if (interpreters.contains(fileName))
        return fail(reason, QStringLiteral("Shell and script interpreter executables are refused."));
    const QString suffix = QFileInfo(fileName).suffix();
    if (suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat")
        || suffix == QStringLiteral("ps1") || suffix == QStringLiteral("vbs")
        || suffix == QStringLiteral("js")) {
        return fail(reason, QStringLiteral("Script files are not executable command actions."));
    }
    if (action.arguments.size() > 512)
        return fail(reason, QStringLiteral("Command has too many arguments."));
    for (const QString &argument : action.arguments) {
        if (argument.size() > 32767 || hasControlCharacter(argument))
            return fail(reason, QStringLiteral("A command argument is too long or contains a control character."));
    }
    if (hasControlCharacter(action.workingDirectory)
        || action.workingDirectory.contains(QLatin1Char('*'))
        || action.workingDirectory.contains(QLatin1Char('?')))
        return fail(reason, QStringLiteral("Working directory is unsafe."));
    if (action.successExitCodes.isEmpty() || action.successExitCodes.size() > 128)
        return fail(reason, QStringLiteral("At least one bounded success exit code is required."));
    return true;
}

bool validateRegistryValue(const WinForgeAction &action, QString *reason)
{
    const QString type = action.registryValueType;
    if (type == QStringLiteral("String") || type == QStringLiteral("ExpandString")) {
        if (!action.registryValue.isString())
            return fail(reason, QStringLiteral("String registry values require a JSON string."));
        return true;
    }
    if (type == QStringLiteral("MultiString")) {
        if (!action.registryValue.isArray())
            return fail(reason, QStringLiteral("MultiString registry values require an array."));
        for (const QJsonValue &value : action.registryValue.toArray()) {
            if (!value.isString())
                return fail(reason, QStringLiteral("MultiString entries must be strings."));
        }
        return true;
    }
    if (type == QStringLiteral("DWord")) {
        if (!action.registryValue.isDouble()
            || action.registryValue.toDouble() < -2147483648.0
            || action.registryValue.toDouble() > 4294967295.0
            || std::trunc(action.registryValue.toDouble()) != action.registryValue.toDouble())
            return fail(reason, QStringLiteral("DWord requires a 32-bit JSON number."));
        return true;
    }
    if (type == QStringLiteral("QWord")) {
        bool ok = false;
        action.registryValue.toString().toULongLong(&ok, 10);
        if (!action.registryValue.isString() || !ok)
            return fail(reason, QStringLiteral("QWord requires an unsigned decimal JSON string."));
        return true;
    }
    if (type == QStringLiteral("Binary")) {
        if (!action.registryValue.isString())
            return fail(reason, QStringLiteral("Binary requires a hexadecimal JSON string."));
        static const QRegularExpression bytes(QStringLiteral(R"(^([0-9A-Fa-f]{2})*$)"));
        if (!bytes.match(action.registryValue.toString()).hasMatch())
            return fail(reason, QStringLiteral("Binary registry data must be even-length hexadecimal."));
        return true;
    }
    return fail(reason, QStringLiteral("Unsupported registry type '%1'.").arg(type));
}

QJsonObject contractJson(const WinForgeRuntimeContract &contract)
{
    QJsonObject object;
    object.insert(QStringLiteral("format"), ContractFormat);
    object.insert(QStringLiteral("formatVersion"), 1);
    object.insert(QStringLiteral("runtimeFound"), contract.runtimeFound);
    object.insert(QStringLiteral("declaredContract"), contract.declaredContract);
    object.insert(QStringLiteral("contractVersion"), contract.contractVersion);
    object.insert(QStringLiteral("runtimeVersion"), contract.runtimeVersion);
    object.insert(QStringLiteral("executable"), contract.executableRelativePath);
    object.insert(QStringLiteral("detectionSource"), contract.detectionSource);
    object.insert(QStringLiteral("capabilities"), stringArray(contract.capabilities));
    QJsonObject invocations;
    for (auto iterator = contract.invocations.cbegin(); iterator != contract.invocations.cend(); ++iterator)
        invocations.insert(iterator.key(), stringArray(iterator.value()));
    object.insert(QStringLiteral("invocations"), invocations);
    return object;
}

bool validateInvocation(const QString &capability,
                        const QStringList &tokens,
                        QString *error)
{
    if (tokens.isEmpty() || tokens.size() > 64)
        return fail(error, QStringLiteral("Invocation '%1' has an invalid token count.").arg(capability));
    QSet<QString> placeholders;
    if (capability == QStringLiteral("launch.page.v1"))
        placeholders = {QStringLiteral("{target}")};
    else if (capability == QStringLiteral("apply.module.v1"))
        placeholders = {QStringLiteral("{target}"), QStringLiteral("{action-id}")};
    else if (capability == QStringLiteral("apply.tweak.v1"))
        placeholders = {QStringLiteral("{target}"), QStringLiteral("{value-json}"),
                        QStringLiteral("{action-id}")};

    for (const QString &token : tokens) {
        if (token.isEmpty() || token.size() > 4096 || hasControlCharacter(token))
            return fail(error, QStringLiteral("Invocation '%1' contains an unsafe token.").arg(capability));
        if (token.contains(QLatin1Char('{')) || token.contains(QLatin1Char('}'))) {
            if (!placeholders.contains(token))
                return fail(error, QStringLiteral("Invocation '%1' uses an unsupported placeholder.").arg(capability));
        }
    }
    return true;
}

struct CopiedFile
{
    QString relativePath;
    quint64 size = 0;
    QString sha256;
};

bool copyOrdinaryFile(const QString &source,
                      const QString &destination,
                      const QString &relativePath,
                      QList<CopiedFile> *files,
                      quint64 *totalBytes,
                      const WinForgeStageOptions &options,
                      QString *error)
{
    const QFileInfo sourceInfo(source);
    if (!sourceInfo.isFile() || isLinkLike(sourceInfo))
        return fail(error, QStringLiteral("Refusing non-ordinary source file '%1'.").arg(source));
    const quint64 size = static_cast<quint64>(sourceInfo.size());
    if (static_cast<quint64>(files->size()) >= options.maximumFiles
        || size > options.maximumTotalBytes - std::min(*totalBytes, options.maximumTotalBytes)) {
        return fail(error, QStringLiteral("WinForge bundle exceeds its staging limits."));
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return fail(error, QStringLiteral("Cannot create a staging directory."));
    if (!QFile::copy(source, destination))
        return fail(error, QStringLiteral("Cannot copy '%1' into staging.").arg(source));
    QString digest;
    quint64 copiedSize = 0;
    if (!hashFile(destination, &digest, &copiedSize, error) || copiedSize != size)
        return fail(error, QStringLiteral("Copied file verification failed for '%1'.").arg(source));
    files->append({QDir::fromNativeSeparators(relativePath), copiedSize, digest});
    *totalBytes += copiedSize;
    return true;
}

bool copyTree(const QString &sourceRoot,
              const QString &destinationRoot,
              const QString &manifestPrefix,
              QList<CopiedFile> *files,
              quint64 *totalBytes,
              const WinForgeStageOptions &options,
              QString *error)
{
    const QFileInfo rootInfo(sourceRoot);
    if (!rootInfo.isDir() || isLinkLike(rootInfo))
        return fail(error, QStringLiteral("Runtime directory is missing or is a link/reparse point."));
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return fail(error, QStringLiteral("Cannot resolve the runtime directory."));

    QDirIterator iterator(sourceRoot,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    const QDir root(sourceRoot);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (isLinkLike(info))
            return fail(error, QStringLiteral("Runtime links/reparse points are refused: %1").arg(info.filePath()));
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !isPathInside(canonical, canonicalRoot))
            return fail(error, QStringLiteral("Runtime entry escapes its root: %1").arg(info.filePath()));
        const QString relative = QDir::fromNativeSeparators(root.relativeFilePath(info.filePath()));
        if (!isPortableRelativePath(relative))
            return fail(error, QStringLiteral("Runtime path is not portable: %1").arg(relative));
        const QString destination = QDir(destinationRoot).filePath(relative);
        if (info.isDir()) {
            if (!QDir().mkpath(destination))
                return fail(error, QStringLiteral("Cannot create runtime staging directory."));
        } else if (info.isFile()) {
            if (!copyOrdinaryFile(info.filePath(), destination,
                                  manifestPrefix + QLatin1Char('/') + relative,
                                  files, totalBytes, options, error))
                return false;
        } else {
            return fail(error, QStringLiteral("Unsupported runtime entry: %1").arg(info.filePath()));
        }
    }
    return true;
}

QJsonArray fileManifest(const QList<CopiedFile> &files)
{
    QList<CopiedFile> ordered = files;
    std::sort(ordered.begin(), ordered.end(), [](const CopiedFile &left, const CopiedFile &right) {
        return left.relativePath.compare(right.relativePath, Qt::CaseInsensitive) < 0;
    });
    QJsonArray result;
    for (const CopiedFile &file : ordered) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), file.relativePath);
        object.insert(QStringLiteral("size"), QString::number(file.size));
        object.insert(QStringLiteral("sha256"), file.sha256);
        result.append(object);
    }
    return result;
}

class TemporaryDirectoryGuard
{
public:
    TemporaryDirectoryGuard(QString path, QString parent)
        : m_path(std::move(path)), m_parent(std::move(parent))
    {
    }
    ~TemporaryDirectoryGuard()
    {
        if (m_active && isPathInside(m_path, m_parent))
            QDir(m_path).removeRecursively();
    }
    void release() { m_active = false; }

private:
    QString m_path;
    QString m_parent;
    bool m_active = true;
};

} // namespace

QString WinForgeBridge::actionKindName(WinForgeActionKind kind)
{
    switch (kind) {
    case WinForgeActionKind::Module:
        return QStringLiteral("module");
    case WinForgeActionKind::Page:
        return QStringLiteral("page");
    case WinForgeActionKind::Tweak:
        return QStringLiteral("tweak");
    case WinForgeActionKind::Command:
        return QStringLiteral("command");
    case WinForgeActionKind::Registry:
        return QStringLiteral("registry");
    case WinForgeActionKind::Copy:
        return QStringLiteral("copy");
    }
    return QStringLiteral("unknown");
}

QString WinForgeBridge::actionPhaseName(WinForgeActionPhase phase)
{
    return phase == WinForgeActionPhase::Machine ? QStringLiteral("machine")
                                                  : QStringLiteral("user");
}

WinForgeBridgeValidation WinForgeBridge::validateRecipe(const WinForgeRecipe &recipe)
{
    WinForgeBridgeValidation result;
    if (!isSafeIdentifier(recipe.id))
        result.errors.append(QStringLiteral("Recipe ID must be a portable stable identifier."));
    if (recipe.name.trimmed().isEmpty() || hasControlCharacter(recipe.name))
        result.errors.append(QStringLiteral("Recipe name is required and cannot contain control characters."));
    if (recipe.name.size() > 256 || recipe.description.size() > 16384)
        result.errors.append(QStringLiteral("Recipe text exceeds its size limit."));
    if (recipe.requiredContractVersion < 0)
        result.errors.append(QStringLiteral("Required contract version cannot be negative."));
    if (!recipe.createdUtc.isEmpty()
        && !QDateTime::fromString(recipe.createdUtc, Qt::ISODateWithMs).isValid()
        && !QDateTime::fromString(recipe.createdUtc, Qt::ISODate).isValid()) {
        result.errors.append(QStringLiteral("createdUtc must be an ISO-8601 timestamp."));
    }
    if (recipe.createdUtc.isEmpty())
        result.warnings.append(QStringLiteral("Recipe has no creation timestamp."));
    if (recipe.actions.size() > 10000)
        result.errors.append(QStringLiteral("Recipe contains more than 10,000 actions."));

    QSet<QString> ids;
    QSet<QString> idempotencyKeys;
    for (qsizetype index = 0; index < recipe.actions.size(); ++index) {
        const WinForgeAction &action = recipe.actions.at(index);
        const QString prefix = QStringLiteral("Action %1: ").arg(index + 1);
        const QString key = action.idempotencyKey.isEmpty() ? action.id : action.idempotencyKey;
        if (!isSafeIdentifier(action.id))
            result.errors.append(prefix + QStringLiteral("invalid stable action ID."));
        if (!isSafeIdentifier(key))
            result.errors.append(prefix + QStringLiteral("invalid idempotency key."));
        if (ids.contains(action.id.toCaseFolded()))
            result.errors.append(prefix + QStringLiteral("duplicate action ID."));
        if (idempotencyKeys.contains(key.toCaseFolded()))
            result.errors.append(prefix + QStringLiteral("duplicate idempotency key."));
        ids.insert(action.id.toCaseFolded());
        idempotencyKeys.insert(key.toCaseFolded());

        switch (action.kind) {
        case WinForgeActionKind::Module:
        case WinForgeActionKind::Tweak:
            if (!isSafeIdentifier(action.target))
                result.errors.append(prefix + QStringLiteral("module/tweak target must be an identifier."));
            if (action.phase != WinForgeActionPhase::User)
                result.errors.append(prefix + QStringLiteral("WinForge UI/tweak actions must run in user phase."));
            break;
        case WinForgeActionKind::Page:
            if (action.target.trimmed().isEmpty() || action.target != action.target.trimmed()
                || action.target.size() > 2048 || hasControlCharacter(action.target))
                result.errors.append(prefix + QStringLiteral("page target is invalid."));
            if (action.phase != WinForgeActionPhase::User)
                result.errors.append(prefix + QStringLiteral("WinForge page actions must run in user phase."));
            break;
        case WinForgeActionKind::Command: {
            QString reason;
            if (!isSafeCommand(action, &reason))
                result.errors.append(prefix + reason);
            break;
        }
        case WinForgeActionKind::Registry: {
            static const QSet<QString> hives = {QStringLiteral("HKLM"), QStringLiteral("HKCU")};
            if (!hives.contains(action.registryHive))
                result.errors.append(prefix + QStringLiteral("registry hive must be HKLM or HKCU."));
            if (action.registryPath.isEmpty() || action.registryPath.startsWith(QLatin1Char('\\'))
                || action.registryPath.contains(QStringLiteral(".."))
                || action.registryPath.contains(QLatin1Char('*'))
                || action.registryPath.contains(QLatin1Char('?'))
                || hasControlCharacter(action.registryPath)
                || hasControlCharacter(action.registryValueName)) {
                result.errors.append(prefix + QStringLiteral("registry path/name is unsafe."));
            }
            if ((action.registryHive == QStringLiteral("HKLM"))
                != (action.phase == WinForgeActionPhase::Machine)) {
                result.errors.append(prefix + QStringLiteral("HKLM is machine phase and HKCU is user phase."));
            }
            QString reason;
            if (!validateRegistryValue(action, &reason))
                result.errors.append(prefix + reason);
            break;
        }
        case WinForgeActionKind::Copy:
            if (!isPortableRelativePath(action.sourceRelative))
                result.errors.append(prefix + QStringLiteral("copy source must be a safe relative payload path."));
            if (action.destination.trimmed().isEmpty() || hasControlCharacter(action.destination)
                || action.destination.contains(QLatin1Char('*'))
                || action.destination.contains(QLatin1Char('?')))
                result.errors.append(prefix + QStringLiteral("copy destination is invalid."));
            if (!QRegularExpression(
                     QStringLiteral(R"(^(?:[A-Za-z]:[\\/]|%[A-Za-z_][A-Za-z0-9_]*%[\\/]).+)"))
                     .match(action.destination)
                     .hasMatch()) {
                result.errors.append(prefix + QStringLiteral("copy destination must be an absolute drive or environment-rooted path."));
            }
            if (!QRegularExpression(QStringLiteral(R"(^[0-9a-fA-F]{64}$)"))
                     .match(action.sha256)
                     .hasMatch())
                result.errors.append(prefix + QStringLiteral("copy SHA-256 is required."));
            break;
        }
    }
    if (QJsonDocument(WinForgeBridge::toJson(recipe)).toJson(QJsonDocument::Compact).size()
        > MaximumJsonBytes) {
        result.errors.append(QStringLiteral("Recipe exceeds the 16 MiB serialized size limit."));
    }
    return result;
}

WinForgeBridgeValidation WinForgeBridge::validateAgainstRuntime(
    const WinForgeRecipe &recipe,
    const WinForgeRuntimeContract &contract)
{
    WinForgeBridgeValidation result = validateRecipe(recipe);
    if (!result.ok())
        return result;

    if (recipe.requiredContractVersion > contract.contractVersion) {
        result.errors.append(
            QStringLiteral("Recipe needs WinForge bridge contract %1, but runtime provides %2.")
                .arg(recipe.requiredContractVersion)
                .arg(contract.contractVersion));
    }
    if (!recipe.minimumRuntimeVersion.isEmpty()) {
        const QVersionNumber required = QVersionNumber::fromString(recipe.minimumRuntimeVersion);
        const QVersionNumber actual = QVersionNumber::fromString(contract.runtimeVersion);
        if (required.isNull())
            result.errors.append(QStringLiteral("minimumRuntimeVersion is not a numeric version."));
        else if (actual.isNull())
            result.errors.append(QStringLiteral("Runtime version is unknown; minimum version cannot be proven."));
        else if (QVersionNumber::compare(actual, required) < 0)
            result.errors.append(QStringLiteral("WinForge runtime is older than the recipe minimum."));
    }

    for (const WinForgeAction &action : recipe.actions) {
        if (!action.enabled)
            continue;
        const QString capability = requiredCapability(action.kind);
        if (capability.isEmpty())
            continue;
        if (!contract.runtimeFound) {
            result.errors.append(QStringLiteral("Action '%1' needs a staged WinForge runtime.").arg(action.id));
        } else if (!contract.capabilities.contains(capability)) {
            result.errors.append(
                QStringLiteral("Action '%1' needs undeclared runtime capability '%2'.")
                    .arg(action.id, capability));
        } else if (!contract.invocations.contains(capability)) {
            result.errors.append(
                QStringLiteral("Runtime capability '%1' has no safe argument-vector invocation.")
                    .arg(capability));
        }
    }
    if (!contract.declaredContract && contract.runtimeFound) {
        result.warnings.append(
            QStringLiteral("Legacy runtime detected: only source-verified --page <alias> is available."));
    }
    return result;
}

QJsonObject WinForgeBridge::toJson(const WinForgeRecipe &recipe)
{
    QJsonArray actions;
    for (const WinForgeAction &action : recipe.actions)
        actions.append(actionJson(action));
    QJsonObject object;
    object.insert(QStringLiteral("format"), RecipeFormat);
    object.insert(QStringLiteral("formatVersion"), CurrentRecipeVersion);
    object.insert(QStringLiteral("id"), recipe.id);
    object.insert(QStringLiteral("name"), recipe.name);
    object.insert(QStringLiteral("description"), recipe.description);
    object.insert(QStringLiteral("createdUtc"), recipe.createdUtc);
    object.insert(QStringLiteral("requiredContractVersion"), recipe.requiredContractVersion);
    object.insert(QStringLiteral("minimumRuntimeVersion"), recipe.minimumRuntimeVersion);
    object.insert(QStringLiteral("actions"), actions);
    return object;
}

std::optional<WinForgeRecipe> WinForgeBridge::fromJson(const QJsonObject &json, QString *error)
{
    setError(error, {});
    static const QSet<QString> rootKeys = {
        QStringLiteral("format"), QStringLiteral("formatVersion"), QStringLiteral("id"),
        QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("createdUtc"),
        QStringLiteral("requiredContractVersion"), QStringLiteral("minimumRuntimeVersion"),
        QStringLiteral("actions"),
    };
    if (!hasOnlyKeys(json, rootKeys, QStringLiteral("Recipe"), error))
        return std::nullopt;
    if (json.value(QStringLiteral("format")).toString() != RecipeFormat
        || json.value(QStringLiteral("formatVersion")).toInt(-1) != CurrentRecipeVersion) {
        setError(error, QStringLiteral("Unsupported WinForge recipe format/version."));
        return std::nullopt;
    }
    if (!json.value(QStringLiteral("actions")).isArray()) {
        setError(error, QStringLiteral("Recipe actions must be an array."));
        return std::nullopt;
    }
    if (!json.value(QStringLiteral("format")).isString()
        || !json.value(QStringLiteral("formatVersion")).isDouble()
        || json.value(QStringLiteral("formatVersion")).toDouble()
            != static_cast<double>(CurrentRecipeVersion)
        || !json.value(QStringLiteral("id")).isString()
        || !json.value(QStringLiteral("name")).isString()
        || !json.value(QStringLiteral("description")).isString()
        || !json.value(QStringLiteral("createdUtc")).isString()
        || !json.value(QStringLiteral("requiredContractVersion")).isDouble()
        || json.value(QStringLiteral("requiredContractVersion")).toDouble()
            != static_cast<double>(json.value(QStringLiteral("requiredContractVersion")).toInt())
        || !json.value(QStringLiteral("minimumRuntimeVersion")).isString()) {
        setError(error, QStringLiteral("Recipe fields have invalid JSON types."));
        return std::nullopt;
    }

    WinForgeRecipe recipe;
    recipe.id = json.value(QStringLiteral("id")).toString();
    recipe.name = json.value(QStringLiteral("name")).toString();
    recipe.description = json.value(QStringLiteral("description")).toString();
    recipe.createdUtc = json.value(QStringLiteral("createdUtc")).toString();
    recipe.requiredContractVersion = json.value(QStringLiteral("requiredContractVersion")).toInt();
    recipe.minimumRuntimeVersion = json.value(QStringLiteral("minimumRuntimeVersion")).toString();

    const QSet<QString> baseKeys = {
        QStringLiteral("id"), QStringLiteral("idempotencyKey"), QStringLiteral("kind"),
        QStringLiteral("phase"), QStringLiteral("enabled"), QStringLiteral("digest"),
    };
    const QJsonArray actionArray = json.value(QStringLiteral("actions")).toArray();
    for (qsizetype index = 0; index < actionArray.size(); ++index) {
        if (!actionArray.at(index).isObject()) {
            setError(error, QStringLiteral("Action %1 is not an object.").arg(index + 1));
            return std::nullopt;
        }
        const QJsonObject object = actionArray.at(index).toObject();
        const std::optional<WinForgeActionKind> kind = parseKind(object.value(QStringLiteral("kind")).toString());
        const std::optional<WinForgeActionPhase> phase = parsePhase(object.value(QStringLiteral("phase")).toString());
        if (!kind || !phase) {
            setError(error, QStringLiteral("Action %1 has an unknown kind or phase.").arg(index + 1));
            return std::nullopt;
        }
        if (!object.value(QStringLiteral("id")).isString()
            || !object.value(QStringLiteral("idempotencyKey")).isString()
            || !object.value(QStringLiteral("kind")).isString()
            || !object.value(QStringLiteral("phase")).isString()
            || !object.value(QStringLiteral("enabled")).isBool()
            || !object.value(QStringLiteral("digest")).isString()) {
            setError(error, QStringLiteral("Action %1 base fields have invalid JSON types.").arg(index + 1));
            return std::nullopt;
        }
        QSet<QString> allowed = baseKeys;
        switch (*kind) {
        case WinForgeActionKind::Module:
        case WinForgeActionKind::Page:
            allowed.insert(QStringLiteral("target"));
            break;
        case WinForgeActionKind::Tweak:
            allowed.insert(QStringLiteral("target"));
            allowed.insert(QStringLiteral("value"));
            break;
        case WinForgeActionKind::Command:
            allowed.unite({QStringLiteral("executable"), QStringLiteral("arguments"),
                           QStringLiteral("workingDirectory"), QStringLiteral("successExitCodes")});
            break;
        case WinForgeActionKind::Registry:
            allowed.unite({QStringLiteral("hive"), QStringLiteral("path"),
                           QStringLiteral("name"), QStringLiteral("type"), QStringLiteral("value")});
            break;
        case WinForgeActionKind::Copy:
            allowed.unite({QStringLiteral("source"), QStringLiteral("destination"),
                           QStringLiteral("sha256"), QStringLiteral("overwrite")});
            break;
        }
        if (!hasOnlyKeys(object, allowed, QStringLiteral("Action %1").arg(index + 1), error))
            return std::nullopt;
        const bool targetKind = *kind == WinForgeActionKind::Module
            || *kind == WinForgeActionKind::Page || *kind == WinForgeActionKind::Tweak;
        if ((targetKind && !object.value(QStringLiteral("target")).isString())
            || (*kind == WinForgeActionKind::Tweak && !object.contains(QStringLiteral("value")))
            || (*kind == WinForgeActionKind::Command
                && (!object.value(QStringLiteral("executable")).isString()
                    || !object.value(QStringLiteral("arguments")).isArray()
                    || (object.contains(QStringLiteral("workingDirectory"))
                        && !object.value(QStringLiteral("workingDirectory")).isString())
                    || !object.value(QStringLiteral("successExitCodes")).isArray()))
            || (*kind == WinForgeActionKind::Registry
                && (!object.value(QStringLiteral("hive")).isString()
                    || !object.value(QStringLiteral("path")).isString()
                    || !object.value(QStringLiteral("name")).isString()
                    || !object.value(QStringLiteral("type")).isString()
                    || !object.contains(QStringLiteral("value"))))
            || (*kind == WinForgeActionKind::Copy
                && (!object.value(QStringLiteral("source")).isString()
                    || !object.value(QStringLiteral("destination")).isString()
                    || !object.value(QStringLiteral("sha256")).isString()
                    || !object.value(QStringLiteral("overwrite")).isBool()))) {
            setError(error, QStringLiteral("Action %1 typed fields have invalid JSON types.").arg(index + 1));
            return std::nullopt;
        }

        WinForgeAction action;
        action.id = object.value(QStringLiteral("id")).toString();
        action.idempotencyKey = object.value(QStringLiteral("idempotencyKey")).toString();
        action.kind = *kind;
        action.phase = *phase;
        action.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        action.target = object.value(QStringLiteral("target")).toString();
        action.value = object.value(QStringLiteral("value"));
        action.executable = object.value(QStringLiteral("executable")).toString();
        action.workingDirectory = object.value(QStringLiteral("workingDirectory")).toString();
        action.registryHive = object.value(QStringLiteral("hive")).toString();
        action.registryPath = object.value(QStringLiteral("path")).toString();
        action.registryValueName = object.value(QStringLiteral("name")).toString();
        action.registryValueType = object.value(QStringLiteral("type")).toString();
        action.registryValue = object.value(QStringLiteral("value"));
        action.sourceRelative = object.value(QStringLiteral("source")).toString();
        action.destination = object.value(QStringLiteral("destination")).toString();
        action.sha256 = object.value(QStringLiteral("sha256")).toString();
        action.overwrite = object.value(QStringLiteral("overwrite")).toBool(false);
        const QJsonArray arguments = object.value(QStringLiteral("arguments")).toArray();
        for (const QJsonValue &argument : arguments) {
            if (!argument.isString()) {
                setError(error, QStringLiteral("Command arguments must be strings."));
                return std::nullopt;
            }
            action.arguments.append(argument.toString());
        }
        if (*kind == WinForgeActionKind::Command) {
            if (!object.value(QStringLiteral("successExitCodes")).isArray()) {
                setError(error, QStringLiteral("Command successExitCodes must be an array."));
                return std::nullopt;
            }
            action.successExitCodes.clear();
            for (const QJsonValue &code : object.value(QStringLiteral("successExitCodes")).toArray()) {
                if (!code.isDouble() || code.toDouble() != static_cast<double>(code.toInt())) {
                    setError(error, QStringLiteral("Command success exit codes must be integers."));
                    return std::nullopt;
                }
                action.successExitCodes.append(code.toInt());
            }
        }
        const QString declaredDigest = object.value(QStringLiteral("digest")).toString();
        if (!QRegularExpression(QStringLiteral(R"(^[0-9a-f]{64}$)"))
                 .match(declaredDigest)
                 .hasMatch()
            || declaredDigest != actionDigest(action)) {
            setError(error, QStringLiteral("Action %1 digest is missing or invalid.").arg(index + 1));
            return std::nullopt;
        }
        recipe.actions.append(action);
    }

    const WinForgeBridgeValidation validation = validateRecipe(recipe);
    if (!validation.ok()) {
        setError(error, validation.message());
        return std::nullopt;
    }
    return recipe;
}

bool WinForgeBridge::exportJson(const WinForgeRecipe &recipe,
                                const QString &destinationFile,
                                QString *error)
{
    setError(error, {});
    const WinForgeBridgeValidation validation = validateRecipe(recipe);
    if (!validation.ok())
        return fail(error, validation.message());
    return writeBytesAtomically(destinationFile,
                                QJsonDocument(toJson(recipe)).toJson(QJsonDocument::Indented),
                                error);
}

std::optional<WinForgeRecipe> WinForgeBridge::importJson(const QString &sourceFile,
                                                         QString *error)
{
    setError(error, {});
    QFile file(sourceFile);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read recipe '%1': %2").arg(sourceFile, file.errorString()));
        return std::nullopt;
    }
    if (file.size() > MaximumJsonBytes) {
        setError(error, QStringLiteral("Recipe JSON exceeds the size limit."));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("Invalid recipe JSON: %1").arg(parseError.errorString()));
        return std::nullopt;
    }
    return fromJson(document.object(), error);
}

WinForgeRuntimeContract WinForgeBridge::detectRuntimeContract(const QString &runtimeDirectory,
                                                               QString *error)
{
    setError(error, {});
    WinForgeRuntimeContract result;
    const QFileInfo root(runtimeDirectory);
    if (!root.isDir() || isLinkLike(root)) {
        setError(error, QStringLiteral("WinForge runtime directory is missing or is a link/reparse point."));
        return result;
    }
    const QString contractPath = QDir(runtimeDirectory).filePath(QStringLiteral("winforge-contract.json"));
    if (QFileInfo::exists(contractPath)) {
        const QFileInfo contractInfo(contractPath);
        if (!contractInfo.isFile() || isLinkLike(contractInfo) || contractInfo.size() > 1024 * 1024) {
            setError(error, QStringLiteral("WinForge runtime contract is not an ordinary bounded file."));
            return {};
        }
        QFile file(contractPath);
        if (!file.open(QIODevice::ReadOnly)) {
            setError(error, QStringLiteral("Cannot read WinForge runtime contract."));
            return {};
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            setError(error, QStringLiteral("Invalid WinForge runtime contract JSON."));
            return {};
        }
        const QJsonObject object = document.object();
        static const QSet<QString> allowed = {
            QStringLiteral("format"), QStringLiteral("formatVersion"),
            QStringLiteral("contractVersion"), QStringLiteral("runtimeVersion"),
            QStringLiteral("executable"), QStringLiteral("capabilities"),
            QStringLiteral("invocations"),
        };
        if (!hasOnlyKeys(object, allowed, QStringLiteral("Runtime contract"), error)
            || !object.value(QStringLiteral("format")).isString()
            || object.value(QStringLiteral("format")).toString() != ContractFormat
            || !object.value(QStringLiteral("formatVersion")).isDouble()
            || object.value(QStringLiteral("formatVersion")).toDouble() != 1.0
            || !object.value(QStringLiteral("contractVersion")).isDouble()
            || object.value(QStringLiteral("contractVersion")).toDouble()
                != static_cast<double>(object.value(QStringLiteral("contractVersion")).toInt(-1))
            || object.value(QStringLiteral("contractVersion")).toInt(-1) < 1
            || !object.value(QStringLiteral("runtimeVersion")).isString()
            || QVersionNumber::fromString(object.value(QStringLiteral("runtimeVersion")).toString()).isNull()
            || !object.value(QStringLiteral("executable")).isString()
            || !object.value(QStringLiteral("capabilities")).isArray()
            || !object.value(QStringLiteral("invocations")).isObject()) {
            if (!error || error->isEmpty())
                setError(error, QStringLiteral("Unsupported or incomplete WinForge runtime contract."));
            return {};
        }
        QString executable;
        if (!isPortableRelativePath(object.value(QStringLiteral("executable")).toString(), &executable)) {
            setError(error, QStringLiteral("Runtime contract executable path is unsafe."));
            return {};
        }
        const QString executablePath = QDir(runtimeDirectory).filePath(executable);
        const QFileInfo executableInfo(executablePath);
        if (!executableInfo.isFile() || isLinkLike(executableInfo)
            || !isPathInside(executableInfo.canonicalFilePath(), root.canonicalFilePath())) {
            setError(error, QStringLiteral("Declared WinForge executable is missing or escapes the runtime."));
            return {};
        }
        result.runtimeFound = true;
        result.declaredContract = true;
        result.contractVersion = object.value(QStringLiteral("contractVersion")).toInt();
        result.runtimeVersion = object.value(QStringLiteral("runtimeVersion")).toString();
        result.executableRelativePath = executable;
        result.detectionSource = QStringLiteral("declared-winforge-contract.json");
        QSet<QString> seenCapabilities;
        for (const QJsonValue &capabilityValue : object.value(QStringLiteral("capabilities")).toArray()) {
            const QString capability = capabilityValue.toString();
            if (!capabilityValue.isString() || !isSafeIdentifier(capability)
                || seenCapabilities.contains(capability)) {
                setError(error, QStringLiteral("Runtime contract has an invalid capability."));
                return {};
            }
            seenCapabilities.insert(capability);
            result.capabilities.append(capability);
        }
        const QJsonObject invocationObject = object.value(QStringLiteral("invocations")).toObject();
        for (auto iterator = invocationObject.constBegin(); iterator != invocationObject.constEnd(); ++iterator) {
            if (!seenCapabilities.contains(iterator.key()) || !iterator.value().isArray()) {
                setError(error, QStringLiteral("Runtime invocation is undeclared or not an array."));
                return {};
            }
            QStringList tokens;
            for (const QJsonValue &token : iterator.value().toArray()) {
                if (!token.isString()) {
                    setError(error, QStringLiteral("Runtime invocation tokens must be strings."));
                    return {};
                }
                tokens.append(token.toString());
            }
            if (!validateInvocation(iterator.key(), tokens, error))
                return {};
            result.invocations.insert(iterator.key(), tokens);
        }
        return result;
    }

    const QString executable = QDir(runtimeDirectory).filePath(QStringLiteral("WinForge.exe"));
    const QFileInfo executableInfo(executable);
    if (!executableInfo.isFile() || isLinkLike(executableInfo)
        || !isPathInside(executableInfo.canonicalFilePath(), root.canonicalFilePath())) {
        setError(error, QStringLiteral("No WinForge.exe or declared winforge-contract.json was found."));
        return {};
    }
    result.runtimeFound = true;
    result.declaredContract = false;
    result.contractVersion = 0;
    result.runtimeVersion = QStringLiteral("unknown");
    result.executableRelativePath = QStringLiteral("WinForge.exe");
    result.detectionSource = QStringLiteral("legacy-observed-cli-2026-07-10");
    result.capabilities = {QStringLiteral("launch.page.v1")};
    result.invocations.insert(QStringLiteral("launch.page.v1"),
                              {QStringLiteral("--page"), QStringLiteral("{target}")});
    return result;
}

QString WinForgeBridge::generateBootstrapPowerShell(const WinForgeRecipe &recipe,
                                                     const WinForgeRuntimeContract &contract,
                                                     QString *error)
{
    setError(error, {});
    const WinForgeBridgeValidation validation = validateAgainstRuntime(recipe, contract);
    if (!validation.ok()) {
        setError(error, validation.message());
        return {};
    }
    const QByteArray recipeBytes = QJsonDocument(toJson(recipe)).toJson(QJsonDocument::Compact);
    const QString recipeHash = sha256(recipeBytes);
    QString script = QString::fromUtf8(R"WIMFORGEPS(# WimForge WinForge Bridge bootstrap. Generated; do not hand-edit.
[CmdletBinding()]
param([ValidateSet('Machine','User')][string]$Phase = 'Machine')
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$BundleRoot = Split-Path -LiteralPath $PSCommandPath -Parent
$RecipePath = Join-Path $BundleRoot 'recipe.json'
$ContractPath = Join-Path $BundleRoot 'runtime-contract.json'
$ManifestPath = Join-Path $BundleRoot 'manifest.json'
$StatePath = Join-Path $BundleRoot 'state.json'
$ExpectedRecipeSha256 = '@@RECIPE_SHA@@'
$RecipeId = '@@RECIPE_ID@@'

function Read-JsonFile([string]$Path) {
    return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
}
function Test-BundleFiles {
    $manifest = Read-JsonFile $ManifestPath
    if ([string]$manifest.format -ne 'org.wimforge.winforge-iso-stage' -or [int]$manifest.formatVersion -ne 1) { throw 'Unsupported bridge manifest' }
    $root = [IO.Path]::GetFullPath($BundleRoot)
    foreach ($entry in $manifest.files) {
        $path = [IO.Path]::GetFullPath((Join-Path $root ([string]$entry.path)))
        if (-not $path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escapes bundle' }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Manifest file is missing: $($entry.path)" }
        $info = Get-Item -LiteralPath $path
        if ([uint64]$info.Length -ne [uint64]([string]$entry.size)) { throw "Manifest size mismatch: $($entry.path)" }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne ([string]$entry.sha256).ToLowerInvariant()) { throw "Manifest checksum mismatch: $($entry.path)" }
    }
}
function Save-State([hashtable]$Completed, [string]$LastError) {
    $state = [ordered]@{ format='org.wimforge.winforge-bridge-state'; formatVersion=1; recipeSha256=$ExpectedRecipeSha256; updatedUtc=[DateTime]::UtcNow.ToString('o'); completed=$Completed; lastError=$LastError }
    $json = $state | ConvertTo-Json -Depth 20
    $temp = $StatePath + '.tmp'
    [IO.File]::WriteAllText($temp, $json, (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temp -Destination $StatePath -Force
}
function ConvertTo-NativeArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (($slashes * 2) + 1)))
            [void]$builder.Append('"'); $slashes = 0; continue
        }
        if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes)); $slashes = 0 }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}
function Invoke-Native([string]$Executable, [object[]]$Arguments, [string]$WorkingDirectory, [int[]]$SuccessCodes, [bool]$Wait) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = [Environment]::ExpandEnvironmentVariables($Executable)
    $info.UseShellExecute = $false
    if ($WorkingDirectory) { $info.WorkingDirectory = [Environment]::ExpandEnvironmentVariables($WorkingDirectory) }
    $quoted = New-Object Collections.Generic.List[string]
    foreach ($argument in $Arguments) { $quoted.Add((ConvertTo-NativeArgument ([string]$argument))) }
    $info.Arguments = [string]::Join(' ', $quoted.ToArray())
    $process = [Diagnostics.Process]::Start($info)
    if (-not $process) { throw "Could not start $Executable" }
    if (-not $Wait) { return }
    $process.WaitForExit()
    if ($SuccessCodes -notcontains $process.ExitCode) { throw "$Executable exited with $($process.ExitCode)" }
}
function Get-ContractInvocation([string]$Capability, [object]$Action) {
    if ($Contract.capabilities -notcontains $Capability) { throw "Runtime does not declare $Capability" }
    $property = $Contract.invocations.PSObject.Properties[$Capability]
    if (-not $property) { throw "Runtime declares $Capability without an invocation" }
    $tokens = New-Object Collections.Generic.List[object]
    foreach ($tokenObject in $property.Value) {
        $token = [string]$tokenObject
        switch ($token) {
            '{target}' { $tokens.Add([string]$Action.target); continue }
            '{action-id}' { $tokens.Add([string]$Action.id); continue }
            '{value-json}' { $tokens.Add(($Action.value | ConvertTo-Json -Depth 30 -Compress)); continue }
            default {
                if ($token.Contains('{') -or $token.Contains('}')) { throw "Unsupported runtime placeholder" }
                $tokens.Add($token)
            }
        }
    }
    return $tokens.ToArray()
}
function Invoke-WinForge([string]$Capability, [object]$Action, [bool]$Wait) {
    if (-not $Contract.runtimeFound) { throw 'WinForge runtime is not bundled' }
    $runtimeRoot = [IO.Path]::GetFullPath((Join-Path $BundleRoot 'Runtime'))
    $runtimeExe = [IO.Path]::GetFullPath((Join-Path $runtimeRoot ([string]$Contract.executable)))
    if (-not $runtimeExe.StartsWith($runtimeRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime executable escapes bundle' }
    if (-not (Test-Path -LiteralPath $runtimeExe -PathType Leaf)) { throw 'Runtime executable is missing' }
    if ($Contract.executableSha256) {
        $runtimeHash = (Get-FileHash -LiteralPath $runtimeExe -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($runtimeHash -ne ([string]$Contract.executableSha256).ToLowerInvariant()) { throw 'Runtime executable checksum mismatch' }
    }
    $arguments = Get-ContractInvocation $Capability $Action
    Invoke-Native $runtimeExe $arguments $runtimeRoot @(0) $Wait
}
function Apply-Copy([object]$Action) {
    $payloadRoot = [IO.Path]::GetFullPath((Join-Path $BundleRoot 'Payload'))
    $source = [IO.Path]::GetFullPath((Join-Path $payloadRoot ([string]$Action.source)))
    if (-not $source.StartsWith($payloadRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Copy source escapes payload' }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw 'Copy payload is missing' }
    $actual = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne ([string]$Action.sha256).ToLowerInvariant()) { throw 'Copy payload checksum mismatch' }
    $destination = [Environment]::ExpandEnvironmentVariables([string]$Action.destination)
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $existing = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($existing -eq $actual) { return }
        if (-not [bool]$Action.overwrite) { throw 'Copy destination exists with different bytes' }
    }
    $parent = Split-Path -LiteralPath $destination -Parent
    if ($parent) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
    Copy-Item -LiteralPath $source -Destination $destination -Force:$([bool]$Action.overwrite)
}
function Apply-Registry([object]$Action) {
    if ([string]$Action.hive -eq 'HKLM') { $base = [Microsoft.Win32.Registry]::LocalMachine }
    elseif ([string]$Action.hive -eq 'HKCU') { $base = [Microsoft.Win32.Registry]::CurrentUser }
    else { throw 'Unsupported registry hive' }
    $key = $base.CreateSubKey([string]$Action.path, $true)
    if (-not $key) { throw 'Cannot open registry key' }
    try {
        $kind = [Enum]::Parse([Microsoft.Win32.RegistryValueKind], [string]$Action.type, $true)
        $value = $Action.value
        if ([string]$Action.type -eq 'DWord') {
            $unsigned = [uint32][uint64]$Action.value
            $value = [BitConverter]::ToInt32([BitConverter]::GetBytes($unsigned), 0)
        }
        elseif ([string]$Action.type -eq 'QWord') {
            $unsigned = [uint64]([string]$Action.value)
            $value = [BitConverter]::ToInt64([BitConverter]::GetBytes($unsigned), 0)
        }
        elseif ([string]$Action.type -eq 'MultiString') { $value = [string[]]$Action.value }
        elseif ([string]$Action.type -eq 'Binary') {
            $hex = [string]$Action.value; $bytes = New-Object byte[] ($hex.Length / 2)
            for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16) }
            $value = $bytes
        }
        $key.SetValue([string]$Action.name, $value, $kind)
    } finally { $key.Dispose() }
}

Test-BundleFiles
if ((Get-FileHash -LiteralPath $RecipePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedRecipeSha256) { throw 'Recipe checksum mismatch' }
$Recipe = Read-JsonFile $RecipePath
$Contract = Read-JsonFile $ContractPath
if ([string]$Recipe.format -ne 'org.wimforge.winforge-recipe' -or [int]$Recipe.formatVersion -ne 1) { throw 'Unsupported recipe format' }
$completed = @{}
if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
    $oldState = Read-JsonFile $StatePath
    if ($oldState.completed) {
        foreach ($property in $oldState.completed.PSObject.Properties) { $completed[$property.Name] = [string]$property.Value }
    }
}

foreach ($action in $Recipe.actions) {
    if (-not [bool]$action.enabled -or [string]$action.phase -ne $Phase) { continue }
    $key = [string]$action.idempotencyKey; $digest = [string]$action.digest
    if ($completed[$key] -eq $digest) { continue }
    try {
        switch ([string]$action.kind) {
            'copy' { Apply-Copy $action }
            'registry' { Apply-Registry $action }
            'command' { Invoke-Native ([string]$action.executable) @($action.arguments) ([string]$action.workingDirectory) @($action.successExitCodes) $true }
            'page' { Invoke-WinForge 'launch.page.v1' $action $false }
            'module' { Invoke-WinForge 'apply.module.v1' $action $true }
            'tweak' { Invoke-WinForge 'apply.tweak.v1' $action $true }
            default { throw "Unknown typed action $($action.kind)" }
        }
        $completed[$key] = $digest
        Save-State $completed ''
    } catch {
        Save-State $completed $_.Exception.Message
        throw
    }
}

if ($Phase -eq 'Machine' -and @($Recipe.actions | Where-Object { $_.enabled -and $_.phase -eq 'user' -and $completed[[string]$_.idempotencyKey] -ne [string]$_.digest }).Count -gt 0) {
    $powerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $command = '"' + $powerShell + '" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Phase User'
    $runOnce = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\RunOnce', $true)
    try { $runOnce.SetValue(('WimForgeBridge.' + $RecipeId), $command, [Microsoft.Win32.RegistryValueKind]::String) } finally { $runOnce.Dispose() }
}
)WIMFORGEPS");
    script.replace(QStringLiteral("@@RECIPE_SHA@@"), recipeHash);
    script.replace(QStringLiteral("@@RECIPE_ID@@"), recipe.id);
    return script;
}

std::optional<WinForgeStageResult> WinForgeBridge::stageForIso(
    const WinForgeRecipe &recipe,
    const QString &runtimeDirectory,
    const QString &isoStagingDirectory,
    const WinForgeStageOptions &options,
    QString *error)
{
    setError(error, {});
    WinForgeRuntimeContract contract;
    const bool needsRuntime = std::any_of(recipe.actions.cbegin(), recipe.actions.cend(), [](const WinForgeAction &action) {
        return action.enabled && !requiredCapability(action.kind).isEmpty();
    });
    if (options.includeRuntime || needsRuntime) {
        contract = detectRuntimeContract(runtimeDirectory, error);
        if (!contract.runtimeFound)
            return std::nullopt;
    }
    WinForgeBridgeValidation validation = validateAgainstRuntime(recipe, contract);
    if (!validation.ok()) {
        setError(error, validation.message());
        return std::nullopt;
    }
    if (needsRuntime && !options.includeRuntime) {
        setError(error, QStringLiteral("Enabled WinForge actions require includeRuntime=true."));
        return std::nullopt;
    }
    if (options.maximumFiles == 0 || options.maximumTotalBytes == 0) {
        setError(error, QStringLiteral("Staging limits must be positive."));
        return std::nullopt;
    }
    const QFileInfo isoInfo(isoStagingDirectory);
    if (isoStagingDirectory.trimmed().isEmpty() || isLinkLike(isoInfo)) {
        setError(error, QStringLiteral("ISO staging directory is invalid or linked."));
        return std::nullopt;
    }
    if (!QDir().mkpath(isoStagingDirectory)) {
        setError(error, QStringLiteral("Cannot create ISO staging directory."));
        return std::nullopt;
    }

    const QString installedRelative = QStringLiteral("sources/$OEM$/$1/ProgramData/WimForge/WinForgeBridge/%1")
                                          .arg(recipe.id);
    const QString finalBundle = QDir(isoStagingDirectory).filePath(installedRelative);
    const QString bundleParent = QFileInfo(finalBundle).absolutePath();
    if (!pathChainHasNoLinks(isoStagingDirectory, bundleParent, error)
        || !QDir().mkpath(bundleParent)
        || !pathChainHasNoLinks(isoStagingDirectory, bundleParent, error)
        || !pathChainHasNoLinks(isoStagingDirectory, finalBundle, error)) {
        if (!error || error->isEmpty())
            setError(error, QStringLiteral("Cannot create a safe bundle path inside ISO staging."));
        return std::nullopt;
    }
    const QString temporaryBundle = QDir(bundleParent).filePath(
        QStringLiteral(".wimforge-stage-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!isPathInside(temporaryBundle, bundleParent) || !QDir().mkpath(temporaryBundle)) {
        setError(error, QStringLiteral("Cannot create safe temporary staging directory."));
        return std::nullopt;
    }
    TemporaryDirectoryGuard temporaryGuard(temporaryBundle, bundleParent);

    QList<CopiedFile> files;
    quint64 totalBytes = 0;
    if (options.includeRuntime
        && !copyTree(runtimeDirectory,
                     QDir(temporaryBundle).filePath(QStringLiteral("Runtime")),
                     QStringLiteral("Runtime"), &files, &totalBytes, options, error)) {
        return std::nullopt;
    }

    QMap<QString, QString> copiedPayloads;
    for (const WinForgeAction &action : recipe.actions) {
        if (!action.enabled || action.kind != WinForgeActionKind::Copy)
            continue;
        const QString payloadKey = action.sourceRelative.toCaseFolded();
        if (copiedPayloads.contains(payloadKey)) {
            if (copiedPayloads.value(payloadKey).compare(action.sha256, Qt::CaseInsensitive) != 0) {
                setError(error, QStringLiteral("The same copy payload has conflicting checksums."));
                return std::nullopt;
            }
            continue;
        }
        if (options.payloadDirectory.trimmed().isEmpty()) {
            setError(error, QStringLiteral("Copy actions require a payloadDirectory."));
            return std::nullopt;
        }
        QString relative;
        if (!isPortableRelativePath(action.sourceRelative, &relative)) {
            setError(error, QStringLiteral("Unsafe copy payload path."));
            return std::nullopt;
        }
        const QFileInfo payloadRoot(options.payloadDirectory);
        const QString source = QDir(options.payloadDirectory).filePath(relative);
        const QFileInfo sourceInfo(source);
        if (!payloadRoot.isDir() || isLinkLike(payloadRoot) || !sourceInfo.isFile()
            || isLinkLike(sourceInfo)
            || !isPathInside(sourceInfo.canonicalFilePath(), payloadRoot.canonicalFilePath())) {
            setError(error, QStringLiteral("Copy payload is missing, linked, or escapes payloadDirectory: %1").arg(relative));
            return std::nullopt;
        }
        QString sourceHash;
        if (!hashFile(source, &sourceHash, nullptr, error)
            || sourceHash.compare(action.sha256, Qt::CaseInsensitive) != 0) {
            setError(error, QStringLiteral("Copy payload checksum does not match recipe: %1").arg(relative));
            return std::nullopt;
        }
        if (!copyOrdinaryFile(source,
                              QDir(temporaryBundle).filePath(QStringLiteral("Payload/%1").arg(relative)),
                              QStringLiteral("Payload/%1").arg(relative),
                              &files, &totalBytes, options, error))
            return std::nullopt;
        copiedPayloads.insert(payloadKey, action.sha256);
    }

    const QByteArray recipeBytes = QJsonDocument(toJson(recipe)).toJson(QJsonDocument::Compact);
    const QString recipePath = QDir(temporaryBundle).filePath(QStringLiteral("recipe.json"));
    if (!writeBytesAtomically(recipePath, recipeBytes, error))
        return std::nullopt;
    QString digest;
    quint64 size = 0;
    if (!hashFile(recipePath, &digest, &size, error))
        return std::nullopt;
    files.append({QStringLiteral("recipe.json"), size, digest});
    totalBytes += size;

    QJsonObject stagedContract = contractJson(contract);
    if (contract.runtimeFound && options.includeRuntime) {
        QString executableDigest;
        if (!hashFile(QDir(temporaryBundle).filePath(
                          QStringLiteral("Runtime/%1").arg(contract.executableRelativePath)),
                      &executableDigest, nullptr, error))
            return std::nullopt;
        stagedContract.insert(QStringLiteral("executableSha256"), executableDigest);
    }
    const QString contractPath = QDir(temporaryBundle).filePath(QStringLiteral("runtime-contract.json"));
    if (!writeBytesAtomically(contractPath,
                              QJsonDocument(stagedContract).toJson(QJsonDocument::Indented),
                              error)
        || !hashFile(contractPath, &digest, &size, error))
        return std::nullopt;
    files.append({QStringLiteral("runtime-contract.json"), size, digest});
    totalBytes += size;

    const QString script = generateBootstrapPowerShell(recipe, contract, error);
    if (script.isEmpty())
        return std::nullopt;
    const QString bootstrapPath = QDir(temporaryBundle).filePath(QStringLiteral("bootstrap.ps1"));
    const QByteArray scriptBytes = QString(QChar(0xFEFF)).toUtf8() + script.toUtf8();
    if (!writeBytesAtomically(bootstrapPath, scriptBytes, error)
        || !hashFile(bootstrapPath, &digest, &size, error))
        return std::nullopt;
    files.append({QStringLiteral("bootstrap.ps1"), size, digest});
    totalBytes += size;

    QJsonObject integration;
    integration.insert(QStringLiteral("oemSourceRelative"), installedRelative);
    integration.insert(QStringLiteral("installedRoot"),
                       QStringLiteral("C:/ProgramData/WimForge/WinForgeBridge/%1").arg(recipe.id));
    integration.insert(QStringLiteral("machineBootstrap"),
                       QStringLiteral("powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"C:\\ProgramData\\WimForge\\WinForgeBridge\\%1\\bootstrap.ps1\" -Phase Machine")
                           .arg(recipe.id));
    integration.insert(QStringLiteral("userPhase"),
                       QStringLiteral("Registered through HKLM RunOnce only when approved user actions exist."));

    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), StageFormat);
    manifest.insert(QStringLiteral("formatVersion"), CurrentStageManifestVersion);
    manifest.insert(QStringLiteral("createdUtc"), recipe.createdUtc);
    manifest.insert(QStringLiteral("recipeId"), recipe.id);
    manifest.insert(QStringLiteral("recipeSha256"), sha256(QJsonDocument(toJson(recipe)).toJson(QJsonDocument::Compact)));
    manifest.insert(QStringLiteral("runtimeIncluded"), options.includeRuntime);
    manifest.insert(QStringLiteral("runtimeContract"), stagedContract);
    manifest.insert(QStringLiteral("integration"), integration);
    manifest.insert(QStringLiteral("files"), fileManifest(files));
    manifest.insert(QStringLiteral("totalBytes"), QString::number(totalBytes));
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    const QString manifestDigest = sha256(manifestBytes);
    const QString manifestPath = QDir(temporaryBundle).filePath(QStringLiteral("manifest.json"));
    if (!writeBytesAtomically(manifestPath, manifestBytes, error))
        return std::nullopt;

    bool reuseExisting = false;
    if (QFileInfo::exists(finalBundle)) {
        const QString existingManifest = QDir(finalBundle).filePath(QStringLiteral("manifest.json"));
        QString existingDigest;
        if (QFileInfo::exists(existingManifest)
            && hashFile(existingManifest, &existingDigest, nullptr, nullptr)
            && existingDigest == manifestDigest) {
            reuseExisting = true;
        }
        if (!reuseExisting && !options.overwriteExisting) {
            setError(error, QStringLiteral("A different WinForge bundle is already staged for this recipe ID."));
            return std::nullopt;
        }
        if (!reuseExisting) {
            const QString backup = QDir(bundleParent).filePath(
                QStringLiteral(".wimforge-backup-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            if (!isPathInside(finalBundle, bundleParent) || !isPathInside(backup, bundleParent)
                || !QDir().rename(finalBundle, backup)) {
                setError(error, QStringLiteral("Cannot move existing staged bundle to a safe backup."));
                return std::nullopt;
            }
            if (!QDir().rename(temporaryBundle, finalBundle)) {
                QDir().rename(backup, finalBundle);
                setError(error, QStringLiteral("Cannot promote the verified WinForge staging bundle."));
                return std::nullopt;
            }
            temporaryGuard.release();
            if (isPathInside(backup, bundleParent))
                QDir(backup).removeRecursively();
        }
    } else {
        if (!QDir().rename(temporaryBundle, finalBundle)) {
            setError(error, QStringLiteral("Cannot promote the verified WinForge staging bundle."));
            return std::nullopt;
        }
        temporaryGuard.release();
    }

    WinForgeStageResult result;
    result.bundleDirectory = finalBundle;
    result.manifestPath = QDir(finalBundle).filePath(QStringLiteral("manifest.json"));
    result.recipePath = QDir(finalBundle).filePath(QStringLiteral("recipe.json"));
    result.bootstrapPath = QDir(finalBundle).filePath(QStringLiteral("bootstrap.ps1"));
    result.manifestSha256 = manifestDigest;
    result.fileCount = static_cast<quint64>(files.size() + 1);
    result.totalBytes = totalBytes + static_cast<quint64>(manifestBytes.size());
    result.runtimeContract = contract;

    if (options.writeSetupCompleteHook) {
        const QString scriptsDirectory = QDir(isoStagingDirectory).filePath(
            QStringLiteral("sources/$OEM$/$$/Setup/Scripts"));
        if (!pathChainHasNoLinks(isoStagingDirectory, scriptsDirectory, error)
            || !QDir().mkpath(scriptsDirectory)
            || !pathChainHasNoLinks(isoStagingDirectory, scriptsDirectory, error)) {
            if (!error || error->isEmpty())
                setError(error, QStringLiteral("Bundle staged, but Setup scripts directory could not be created."));
            return std::nullopt;
        }
        const QString fragmentName = QStringLiteral("WimForgeBridge.%1.cmd").arg(recipe.id);
        const QString fragmentPath = QDir(scriptsDirectory).filePath(fragmentName);
        const QString setupComplete = QDir(scriptsDirectory).filePath(QStringLiteral("SetupComplete.cmd"));
        if (!pathChainHasNoLinks(isoStagingDirectory, fragmentPath, error)
            || !pathChainHasNoLinks(isoStagingDirectory, setupComplete, error))
            return std::nullopt;
        const QByteArray fragment = QStringLiteral(
            "@echo off\r\n"
            "\"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"C:\\ProgramData\\WimForge\\WinForgeBridge\\%1\\bootstrap.ps1\" -Phase Machine\r\n"
            "exit /b %ERRORLEVEL%\r\n")
                                        .arg(recipe.id)
                                        .toUtf8();
        if (!writeBytesAtomically(fragmentPath, fragment, error))
            return std::nullopt;
        result.setupCompletePath = fragmentPath;

        const QByteArray marker = QStringLiteral("WimForgeBridge.%1.cmd").arg(recipe.id).toUtf8();
        if (!QFileInfo::exists(setupComplete)) {
            const QByteArray hook = QByteArrayLiteral("@echo off\r\ncall \"%~dp0") + marker
                + QByteArrayLiteral("\"\r\nexit /b %ERRORLEVEL%\r\n");
            if (!writeBytesAtomically(setupComplete, hook, error))
                return std::nullopt;
            result.setupCompletePath = setupComplete;
        } else {
            QFile existing(setupComplete);
            if (!existing.open(QIODevice::ReadOnly)) {
                setError(error, QStringLiteral("Cannot inspect existing SetupComplete.cmd."));
                return std::nullopt;
            }
            const QByteArray existingBytes = existing.readAll();
            // Windows does not allow QSaveFile's atomic replacement while the
            // destination still has an open read handle.
            existing.close();
            if (!existingBytes.contains(marker)) {
                if (existingBytes.size() > 1024 * 1024 || existingBytes.contains('\0')) {
                    setError(error, QStringLiteral(
                        "Existing SetupComplete.cmd uses an unsupported size or encoding; "
                        "WimForge did not claim the bridge hook was installed."));
                    return std::nullopt;
                }
                const QByteArray merged = QByteArrayLiteral("@call \"%~dp0") + marker
                    + QByteArrayLiteral("\"\r\n@if errorlevel 1 exit /b %ERRORLEVEL%\r\n")
                    + existingBytes;
                if (!writeBytesAtomically(setupComplete, merged, error))
                    return std::nullopt;
            }
            result.setupCompletePath = setupComplete;
            result.setupCompleteNeedsMerge = false;
        }
    }
    return result;
}

} // namespace wimforge
