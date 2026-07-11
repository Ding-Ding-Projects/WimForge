#include "CliRunner.h"

#include "core/GitHistory.h"
#include "core/GpoCatalog.h"
#include "core/ActionHistory.h"
#include "core/NotificationStore.h"
#include "core/PackageStudio.h"
#include "core/ProcessLaunch.h"
#include "core/ProjectBundle.h"
#include "core/ProjectConfig.h"
#include "core/ServicingPlan.h"
#include "core/UnattendBuilder.h"
#include "core/VmLabCliService.h"
#include "core/VmLabScope.h"
#include "core/VmValidationStore.h"
#include "core/WinForgeBridge.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace wimforge {
namespace {

constexpr qsizetype MaximumResponseFileBytes = 4 * 1024 * 1024;
constexpr qsizetype MaximumExpandedArguments = 10'000;
constexpr int MaximumResponseDepth = 8;

struct CommandContext
{
    bool json = false;
    QString projectDirectory;
    QString notificationDirectory;
};

struct EditOperation
{
    enum class Kind { Set, Add, Remove, Erase };

    Kind kind = Kind::Set;
    QString path;
    std::optional<QString> rawValue;
};

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

QString withNewline(QString value)
{
    while (value.endsWith(QLatin1Char('\n')) || value.endsWith(QLatin1Char('\r')))
        value.chop(1);
    return value.isEmpty() ? QString() : value + QLatin1Char('\n');
}

QString exitCodeName(CliExitCode code)
{
    switch (code) {
    case CliExitCode::Success: return QStringLiteral("success");
    case CliExitCode::Usage: return QStringLiteral("usage");
    case CliExitCode::Validation: return QStringLiteral("validation");
    case CliExitCode::NotFound: return QStringLiteral("not-found");
    case CliExitCode::ConfirmationRequired: return QStringLiteral("confirmation-required");
    case CliExitCode::ExternalProcessFailed: return QStringLiteral("external-process-failed");
    case CliExitCode::Conflict: return QStringLiteral("conflict");
    case CliExitCode::IoError: return QStringLiteral("io-error");
    case CliExitCode::InternalError: return QStringLiteral("internal-error");
    }
    return QStringLiteral("internal-error");
}

CliResult successResult(const CommandContext &context,
                        const QString &command,
                        const QJsonValue &value,
                        const QString &humanText)
{
    CliResult result;
    if (context.json) {
        QJsonObject envelope{
            {QStringLiteral("command"), command},
            {QStringLiteral("ok"), true},
            {QStringLiteral("result"), value},
        };
        result.standardOutput = withNewline(
            QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact)));
    } else {
        result.standardOutput = withNewline(humanText);
    }
    return result;
}

CliResult failureResult(const CommandContext &context,
                        CliExitCode code,
                        const QString &command,
                        const QString &message,
                        const QJsonValue &details = {})
{
    CliResult result;
    result.code = code;
    if (context.json) {
        QJsonObject error{
            {QStringLiteral("code"), exitCodeName(code)},
            {QStringLiteral("message"), message},
        };
        if (!details.isUndefined() && !details.isNull())
            error.insert(QStringLiteral("details"), details);
        const QJsonObject envelope{
            {QStringLiteral("command"), command},
            {QStringLiteral("error"), error},
            {QStringLiteral("ok"), false},
        };
        result.standardOutput = withNewline(
            QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact)));
    } else {
        result.standardError = withNewline(message);
    }
    return result;
}

CliProcessResult runProcessDefault(const QString &executable,
                                   const QStringList &arguments,
                                   const QString &workingDirectory)
{
    QProcess process;
    configureProcessWithoutConsole(process);
    if (!workingDirectory.trimmed().isEmpty())
        process.setWorkingDirectory(workingDirectory);
    process.start(resolveExecutableForLaunch(executable), arguments, QIODevice::ReadOnly);

    CliProcessResult result;
    result.started = process.waitForStarted(15'000);
    if (!result.started) {
        result.standardError = process.errorString().toUtf8();
        return result;
    }

    // Servicing an image can take hours. QProcess buffers both channels while
    // this synchronous CLI adapter waits, so no shell or nested event loop is
    // required. JobEngine can be injected by the application for live output.
    result.finished = process.waitForFinished(-1);
    result.exitCode = process.exitCode();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    return result;
}

bool readLimitedFile(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open response file %1: %2")
                            .arg(path, file.errorString()));
        return false;
    }
    if (file.size() > MaximumResponseFileBytes) {
        setError(error, QStringLiteral("Response file is larger than %1 bytes: %2")
                            .arg(MaximumResponseFileBytes)
                            .arg(path));
        return false;
    }
    *bytes = file.readAll();
    setError(error, {});
    return true;
}

std::optional<QStringList> tokenizeResponseText(const QString &text, QString *error)
{
    QString filtered;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (!line.trimmed().startsWith(QLatin1Char('#')))
            filtered += line + QLatin1Char('\n');
    }

    QStringList result;
    QString token;
    QChar quote;
    bool tokenStarted = false;
    for (qsizetype index = 0; index < filtered.size(); ++index) {
        const QChar character = filtered.at(index);
        if (!quote.isNull()) {
            if (character == quote) {
                quote = {};
                tokenStarted = true;
            } else if (character == QLatin1Char('\\') && index + 1 < filtered.size()
                       && filtered.at(index + 1) == quote) {
                token += quote;
                ++index;
            } else {
                token += character;
            }
            continue;
        }

        if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
            quote = character;
            tokenStarted = true;
        } else if (character.isSpace()) {
            if (tokenStarted || !token.isEmpty()) {
                result.append(token);
                token.clear();
                tokenStarted = false;
            }
        } else {
            token += character;
            tokenStarted = true;
        }
    }
    if (!quote.isNull()) {
        setError(error, QStringLiteral("Unterminated quote in response file."));
        return std::nullopt;
    }
    if (tokenStarted || !token.isEmpty())
        result.append(token);
    setError(error, {});
    return result;
}

std::optional<QStringList> parseResponseFile(const QString &path, QString *error)
{
    QByteArray bytes;
    if (!readLimitedFile(path, &bytes, error))
        return std::nullopt;

    const QByteArray trimmed = bytes.trimmed();
    if (trimmed.startsWith('[') || trimmed.startsWith('{')) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            setError(error, QStringLiteral("Invalid JSON response file %1 at offset %2: %3")
                                .arg(path)
                                .arg(parseError.offset)
                                .arg(parseError.errorString()));
            return std::nullopt;
        }

        QJsonArray argumentArray;
        QString configuredProject;
        bool configuredJson = false;
        if (document.isArray()) {
            argumentArray = document.array();
        } else if (document.isObject()) {
            const QJsonObject object = document.object();
            if (!object.value(QStringLiteral("arguments")).isArray()) {
                setError(error, QStringLiteral(
                                    "JSON response object must contain an 'arguments' array: %1")
                                    .arg(path));
                return std::nullopt;
            }
            argumentArray = object.value(QStringLiteral("arguments")).toArray();
            configuredProject = object.value(QStringLiteral("project")).toString();
            configuredJson = object.value(QStringLiteral("output")).toString()
                                 .compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
        } else {
            setError(error, QStringLiteral("Response file must contain a JSON array or object: %1")
                                .arg(path));
            return std::nullopt;
        }

        QStringList arguments;
        if (!configuredProject.isEmpty())
            arguments << QStringLiteral("--project") << configuredProject;
        if (configuredJson)
            arguments << QStringLiteral("--json");
        for (qsizetype index = 0; index < argumentArray.size(); ++index) {
            if (!argumentArray.at(index).isString()) {
                setError(error, QStringLiteral("Response argument %1 is not a string in %2")
                                    .arg(index)
                                    .arg(path));
                return std::nullopt;
            }
            arguments.append(argumentArray.at(index).toString());
        }
        setError(error, {});
        return arguments;
    }

    return tokenizeResponseText(QString::fromUtf8(bytes), error);
}

std::optional<QStringList> expandArguments(const QStringList &input,
                                           const QString &baseDirectory,
                                           QSet<QString> *activeFiles,
                                           int depth,
                                           QString *error)
{
    if (depth > MaximumResponseDepth) {
        setError(error, QStringLiteral("Response files are nested more than %1 levels.")
                            .arg(MaximumResponseDepth));
        return std::nullopt;
    }

    QStringList expanded;
    for (qsizetype index = 0; index < input.size(); ++index) {
        QString responsePath;
        const QString argument = input.at(index);
        if (argument == QStringLiteral("--response-file")
            || argument == QStringLiteral("--config")) {
            if (index + 1 >= input.size()) {
                setError(error, QStringLiteral("%1 requires a file path.").arg(argument));
                return std::nullopt;
            }
            responsePath = input.at(++index);
        } else if (argument.startsWith(QLatin1Char('@')) && argument.size() > 1) {
            const QString candidate = QDir(baseDirectory).absoluteFilePath(argument.mid(1));
            if (QFileInfo::exists(candidate))
                responsePath = argument.mid(1);
        }

        if (responsePath.isEmpty()) {
            expanded.append(argument);
        } else {
            const QString absolutePath = QFileInfo(
                QDir(baseDirectory).absoluteFilePath(responsePath)).canonicalFilePath();
            if (absolutePath.isEmpty()) {
                setError(error, QStringLiteral("Response file does not exist: %1")
                                    .arg(QDir(baseDirectory).absoluteFilePath(responsePath)));
                return std::nullopt;
            }
            if (activeFiles->contains(absolutePath)) {
                setError(error, QStringLiteral("Response-file cycle detected at %1")
                                    .arg(absolutePath));
                return std::nullopt;
            }
            activeFiles->insert(absolutePath);
            const auto nestedInput = parseResponseFile(absolutePath, error);
            if (!nestedInput)
                return std::nullopt;
            const auto nested = expandArguments(*nestedInput,
                                                QFileInfo(absolutePath).absolutePath(),
                                                activeFiles,
                                                depth + 1,
                                                error);
            activeFiles->remove(absolutePath);
            if (!nested)
                return std::nullopt;
            expanded.append(*nested);
        }

        if (expanded.size() > MaximumExpandedArguments) {
            setError(error, QStringLiteral("Expanded invocation exceeds %1 arguments.")
                                .arg(MaximumExpandedArguments));
            return std::nullopt;
        }
    }
    setError(error, {});
    return expanded;
}

bool takeFlag(QStringList *arguments, const QString &name)
{
    bool found = false;
    for (qsizetype index = arguments->size() - 1; index >= 0; --index) {
        if (arguments->at(index) == name) {
            arguments->removeAt(index);
            found = true;
        }
    }
    return found;
}

bool takeOption(QStringList *arguments,
                const QString &name,
                QString *value,
                QString *error,
                bool required = false)
{
    std::optional<QString> result;
    for (qsizetype index = 0; index < arguments->size();) {
        const QString argument = arguments->at(index);
        if (argument == name) {
            if (index + 1 >= arguments->size()) {
                setError(error, QStringLiteral("%1 requires a value.").arg(name));
                return false;
            }
            if (result) {
                setError(error, QStringLiteral("%1 may only be supplied once.").arg(name));
                return false;
            }
            result = arguments->at(index + 1);
            arguments->removeAt(index + 1);
            arguments->removeAt(index);
            continue;
        }
        const QString prefix = name + QLatin1Char('=');
        if (argument.startsWith(prefix)) {
            if (result) {
                setError(error, QStringLiteral("%1 may only be supplied once.").arg(name));
                return false;
            }
            result = argument.mid(prefix.size());
            arguments->removeAt(index);
            continue;
        }
        ++index;
    }
    if (required && !result) {
        setError(error, QStringLiteral("%1 is required.").arg(name));
        return false;
    }
    if (result && value)
        *value = *result;
    setError(error, {});
    return true;
}

std::optional<QStringList> takeRepeatedOption(QStringList *arguments,
                                              const QString &name,
                                              QString *error)
{
    QStringList values;
    for (qsizetype index = 0; index < arguments->size();) {
        const QString argument = arguments->at(index);
        if (argument == name) {
            if (index + 1 >= arguments->size()) {
                setError(error, QStringLiteral("%1 requires a value.").arg(name));
                return std::nullopt;
            }
            values.append(arguments->at(index + 1));
            arguments->removeAt(index + 1);
            arguments->removeAt(index);
            continue;
        }
        const QString prefix = name + QLatin1Char('=');
        if (argument.startsWith(prefix)) {
            values.append(argument.mid(prefix.size()));
            arguments->removeAt(index);
            continue;
        }
        ++index;
    }
    setError(error, {});
    return values;
}

std::optional<int> positiveInteger(const QString &text,
                                   int minimum,
                                   int maximum,
                                   QString *error)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok || value < minimum || value > maximum) {
        setError(error, QStringLiteral("Expected an integer from %1 through %2, got '%3'.")
                            .arg(minimum)
                            .arg(maximum)
                            .arg(text));
        return std::nullopt;
    }
    setError(error, {});
    return value;
}

QJsonArray stringsToJson(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.append(value);
    return result;
}

QJsonObject commitToJson(const GitCommit &commit)
{
    return QJsonObject{
        {QStringLiteral("authoredAt"), commit.authoredAt.toString(Qt::ISODateWithMs)},
        {QStringLiteral("hash"), commit.hash},
        {QStringLiteral("isRevert"), commit.isRevert()},
        {QStringLiteral("shortHash"), commit.shortHash},
        {QStringLiteral("subject"), commit.subject},
    };
}

QJsonArray commitsToJson(const QList<GitCommit> &commits)
{
    QJsonArray result;
    for (const GitCommit &commit : commits)
        result.append(commitToJson(commit));
    return result;
}

QString humanCommits(const QList<GitCommit> &commits)
{
    QStringList lines;
    for (const GitCommit &commit : commits) {
        lines.append(QStringLiteral("%1  %2  %3")
                         .arg(commit.shortHash,
                              commit.authoredAt.toString(Qt::ISODate),
                              commit.subject));
    }
    return lines.isEmpty() ? QStringLiteral("No history entries.") : lines.join(QLatin1Char('\n'));
}

std::optional<ProjectConfig> loadProject(const CommandContext &context,
                                         const QString &command,
                                         CliResult *failure)
{
    if (context.projectDirectory.trimmed().isEmpty()) {
        *failure = failureResult(context,
                                 CliExitCode::Usage,
                                 command,
                                 QStringLiteral("Select a project with --project <folder>."));
        return std::nullopt;
    }
    QString error;
    const auto project = ProjectConfig::load(context.projectDirectory, &error);
    if (!project) {
        *failure = failureResult(context,
                                 QFileInfo::exists(context.projectDirectory)
                                     ? CliExitCode::Validation
                                     : CliExitCode::NotFound,
                                 command,
                                 error);
        return std::nullopt;
    }
    return project;
}

QString decodePointerToken(QString value)
{
    value.replace(QStringLiteral("~1"), QStringLiteral("/"));
    value.replace(QStringLiteral("~0"), QStringLiteral("~"));
    return value;
}

std::optional<QStringList> pathTokens(QString path, QString *error)
{
    path = path.trimmed();
    if (path.isEmpty()) {
        setError(error, QStringLiteral("A field path cannot be empty."));
        return std::nullopt;
    }
    QStringList result;
    if (path.startsWith(QLatin1Char('/'))) {
        const QStringList encoded = path.mid(1).split(QLatin1Char('/'), Qt::KeepEmptyParts);
        for (const QString &token : encoded)
            result.append(decodePointerToken(token));
    } else {
        result = path.split(QLatin1Char('.'), Qt::KeepEmptyParts);
        if (std::any_of(result.cbegin(), result.cend(), [](const QString &token) {
                return token.isEmpty();
            })) {
            setError(error, QStringLiteral("Dot field paths cannot contain empty segments: %1")
                                .arg(path));
            return std::nullopt;
        }
    }
    setError(error, {});
    return result;
}

std::optional<int> arrayIndex(const QString &token,
                              qsizetype size,
                              bool allowAppend,
                              QString *error)
{
    if (allowAppend && token == QStringLiteral("-"))
        return static_cast<int>(size);
    bool ok = false;
    const int index = token.toInt(&ok);
    if (!ok || index < 0 || index > size || (!allowAppend && index == size)) {
        setError(error, QStringLiteral("Array index '%1' is outside 0..%2.")
                            .arg(token)
                            .arg(allowAppend ? size : size - 1));
        return std::nullopt;
    }
    setError(error, {});
    return index;
}

bool valueAt(const QJsonValue &root,
             const QStringList &tokens,
             QJsonValue *value,
             QString *error)
{
    QJsonValue current = root;
    for (const QString &token : tokens) {
        if (current.isObject()) {
            const QJsonObject object = current.toObject();
            if (!object.contains(token)) {
                setError(error, QStringLiteral("Field path does not exist at '%1'.").arg(token));
                return false;
            }
            current = object.value(token);
        } else if (current.isArray()) {
            const QJsonArray array = current.toArray();
            const auto index = arrayIndex(token, array.size(), false, error);
            if (!index)
                return false;
            current = array.at(*index);
        } else {
            setError(error, QStringLiteral("Cannot descend through a scalar at '%1'.").arg(token));
            return false;
        }
    }
    *value = current;
    setError(error, {});
    return true;
}

bool replaceAt(QJsonValue *node,
               const QStringList &tokens,
               qsizetype depth,
               const QJsonValue &replacement,
               bool createFinal,
               QString *error)
{
    if (depth == tokens.size()) {
        *node = replacement;
        setError(error, {});
        return true;
    }

    const QString &token = tokens.at(depth);
    if (node->isObject()) {
        QJsonObject object = node->toObject();
        if (!object.contains(token) && (!createFinal || depth + 1 != tokens.size())) {
            setError(error, QStringLiteral("Field path does not exist at '%1'.").arg(token));
            return false;
        }
        QJsonValue child = object.value(token);
        if (!replaceAt(&child, tokens, depth + 1, replacement, createFinal, error))
            return false;
        object.insert(token, child);
        *node = object;
        return true;
    }
    if (node->isArray()) {
        QJsonArray array = node->toArray();
        const auto index = arrayIndex(token,
                                      array.size(),
                                      createFinal && depth + 1 == tokens.size(),
                                      error);
        if (!index)
            return false;
        if (*index == array.size()) {
            if (depth + 1 != tokens.size()) {
                setError(error, QStringLiteral("Only a final '-' segment may append an array."));
                return false;
            }
            array.append(replacement);
        } else {
            QJsonValue child = array.at(*index);
            if (!replaceAt(&child, tokens, depth + 1, replacement, createFinal, error))
                return false;
            array.replace(*index, child);
        }
        *node = array;
        return true;
    }
    setError(error, QStringLiteral("Cannot descend through a scalar at '%1'.").arg(token));
    return false;
}

bool eraseAt(QJsonValue *node,
             const QStringList &tokens,
             qsizetype depth,
             QString *error)
{
    if (tokens.isEmpty()) {
        setError(error, QStringLiteral("The project document root cannot be erased."));
        return false;
    }
    const QString &token = tokens.at(depth);
    const bool final = depth + 1 == tokens.size();
    if (node->isObject()) {
        QJsonObject object = node->toObject();
        if (!object.contains(token)) {
            setError(error, QStringLiteral("Field path does not exist at '%1'.").arg(token));
            return false;
        }
        if (final) {
            object.remove(token);
        } else {
            QJsonValue child = object.value(token);
            if (!eraseAt(&child, tokens, depth + 1, error))
                return false;
            object.insert(token, child);
        }
        *node = object;
        setError(error, {});
        return true;
    }
    if (node->isArray()) {
        QJsonArray array = node->toArray();
        const auto index = arrayIndex(token, array.size(), false, error);
        if (!index)
            return false;
        if (final) {
            array.removeAt(*index);
        } else {
            QJsonValue child = array.at(*index);
            if (!eraseAt(&child, tokens, depth + 1, error))
                return false;
            array.replace(*index, child);
        }
        *node = array;
        setError(error, {});
        return true;
    }
    setError(error, QStringLiteral("Cannot descend through a scalar at '%1'.").arg(token));
    return false;
}

QJsonValue parseCliValue(const QString &raw)
{
    QJsonParseError parseError;
    const QByteArray wrapped = QByteArrayLiteral("[") + raw.toUtf8() + QByteArrayLiteral("]");
    const QJsonDocument document = QJsonDocument::fromJson(wrapped, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isArray()
        && document.array().size() == 1) {
        return document.array().first();
    }
    return raw;
}

bool applyEdit(QJsonObject *document, const EditOperation &edit, QString *error)
{
    const auto tokens = pathTokens(edit.path, error);
    if (!tokens)
        return false;
    QJsonValue root(*document);

    if (edit.kind == EditOperation::Kind::Set) {
        if (!edit.rawValue) {
            setError(error, QStringLiteral("set requires a value."));
            return false;
        }
        if (!replaceAt(&root, *tokens, 0, parseCliValue(*edit.rawValue), true, error))
            return false;
    } else if (edit.kind == EditOperation::Kind::Erase) {
        if (!eraseAt(&root, *tokens, 0, error))
            return false;
    } else {
        QJsonValue container;
        if (!valueAt(root, *tokens, &container, error))
            return false;
        if (edit.kind == EditOperation::Kind::Add) {
            if (!edit.rawValue) {
                setError(error, QStringLiteral("add requires a value."));
                return false;
            }
            const QJsonValue addition = parseCliValue(*edit.rawValue);
            if (container.isArray()) {
                QJsonArray array = container.toArray();
                array.append(addition);
                container = array;
            } else if (container.isObject() && addition.isObject()) {
                QJsonObject object = container.toObject();
                const QJsonObject addedObject = addition.toObject();
                for (auto iterator = addedObject.constBegin(); iterator != addedObject.constEnd(); ++iterator)
                    object.insert(iterator.key(), iterator.value());
                container = object;
            } else {
                setError(error, QStringLiteral(
                                    "add targets an array, or an object with a JSON-object value."));
                return false;
            }
        } else {
            if (!edit.rawValue) {
                setError(error, QStringLiteral(
                                    "remove needs an array value or object key; use erase to remove a path."));
                return false;
            }
            const QJsonValue removal = parseCliValue(*edit.rawValue);
            if (container.isArray()) {
                QJsonArray array = container.toArray();
                bool found = false;
                for (qsizetype index = array.size() - 1; index >= 0; --index) {
                    if (array.at(index) == removal) {
                        array.removeAt(index);
                        found = true;
                    }
                }
                if (!found) {
                    setError(error, QStringLiteral("The array does not contain the requested value."));
                    return false;
                }
                container = array;
            } else if (container.isObject() && removal.isString()) {
                QJsonObject object = container.toObject();
                const QString key = removal.toString();
                if (!object.contains(key)) {
                    setError(error, QStringLiteral("The object does not contain key '%1'.").arg(key));
                    return false;
                }
                object.remove(key);
                container = object;
            } else {
                setError(error, QStringLiteral(
                                    "remove targets an array value or an object key string."));
                return false;
            }
        }
        if (!replaceAt(&root, *tokens, 0, container, false, error))
            return false;
    }

    if (!root.isObject()) {
        setError(error, QStringLiteral("A project document must remain a JSON object."));
        return false;
    }
    *document = root.toObject();
    setError(error, {});
    return true;
}

CliResult saveEditedProject(const CommandContext &context,
                            const QString &command,
                            const QList<EditOperation> &edits)
{
    CliResult failure;
    const auto project = loadProject(context, command, &failure);
    if (!project)
        return failure;
    if (edits.isEmpty())
        return failureResult(context,
                             CliExitCode::Usage,
                             command,
                             QStringLiteral("No edits were supplied."));

    QJsonObject document = project->toJson();
    QString error;
    for (const EditOperation &edit : edits) {
        if (!applyEdit(&document, edit, &error)) {
            return failureResult(context,
                                 CliExitCode::Validation,
                                 command,
                                 QStringLiteral("Could not edit %1: %2").arg(edit.path, error));
        }
    }

    const auto edited = ProjectConfig::fromJson(document, project->projectDirectory, &error);
    if (!edited) {
        return failureResult(context, CliExitCode::Validation, command, error);
    }
    const ProjectValidation validation = edited->validate();
    if (!validation.ok()) {
        return failureResult(context,
                             CliExitCode::Validation,
                             command,
                             validation.message(),
                             stringsToJson(validation.errors));
    }

    const QString commitMessage = QStringLiteral("CLI: update %1 field%2 / CLI：更新 %1 個欄位")
                                      .arg(edits.size())
                                      .arg(edits.size() == 1 ? QString() : QStringLiteral("s"));
    if (!edited->save(&error, commitMessage))
        return failureResult(context, CliExitCode::IoError, command, error);

    return successResult(context,
                         command,
                         edited->toJson(),
                         QStringLiteral("Saved %1 edit%2 to %3")
                             .arg(edits.size())
                             .arg(edits.size() == 1 ? QString() : QStringLiteral("s"))
                             .arg(edited->projectFilePath()));
}

CliResult runProjectCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("project");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("project requires create, open, import, export, or validate."));
    const QString action = arguments.takeFirst().toLower();
    QString error;

    if (action == QStringLiteral("create")) {
        QString directory = context.projectDirectory;
        if (directory.isEmpty() && !arguments.isEmpty() && !arguments.first().startsWith(QLatin1Char('-')))
            directory = arguments.takeFirst();
        QString name;
        QString description;
        if (!takeOption(&arguments, QStringLiteral("--name"), &name, &error, true)
            || !takeOption(&arguments, QStringLiteral("--description"), &description, &error)) {
            return failureResult(context, CliExitCode::Usage, command, error);
        }
        if (directory.trimmed().isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("project create requires a folder or --project."));
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected project create argument: %1")
                                     .arg(arguments.first()));

        ProjectConfig project;
        project.projectDirectory = QDir(directory).absolutePath();
        project.projectName = name;
        project.description = description;
        if (!project.save(&error,
                          QStringLiteral("Create project from CLI / 用 CLI 建立工程")))
            return failureResult(context, CliExitCode::Validation, command, error);
        return successResult(context,
                             command,
                             project.toJson(),
                             QStringLiteral("Created %1").arg(project.projectFilePath()));
    }

    if (action == QStringLiteral("open")) {
        CommandContext selected = context;
        if (selected.projectDirectory.isEmpty() && !arguments.isEmpty())
            selected.projectDirectory = arguments.takeFirst();
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("project open accepts one folder."));
        CliResult failure;
        const auto project = loadProject(selected, command, &failure);
        return project ? successResult(context,
                                       command,
                                       project->toJson(),
                                       QStringLiteral("Opened %1\n%2")
                                           .arg(project->projectName, project->projectFilePath()))
                       : failure;
    }

    if (action == QStringLiteral("import")) {
        if (arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("project import requires a source JSON file."));
        const QString source = arguments.takeFirst();
        QString destination = context.projectDirectory;
        if (destination.isEmpty() && !arguments.isEmpty())
            destination = arguments.takeFirst();
        if (destination.isEmpty() || !arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("project import requires source and destination paths."));
        const auto project = ProjectConfig::importJson(source, destination, &error);
        if (!project)
            return failureResult(context, CliExitCode::Validation, command, error);
        return successResult(context,
                             command,
                             project->toJson(),
                             QStringLiteral("Imported project to %1").arg(project->projectFilePath()));
    }

    CommandContext selected = context;
    if (selected.projectDirectory.isEmpty() && !arguments.isEmpty()
        && action == QStringLiteral("validate")) {
        selected.projectDirectory = arguments.takeFirst();
    }
    CliResult failure;
    const auto project = loadProject(selected, command, &failure);
    if (!project)
        return failure;

    if (action == QStringLiteral("export")) {
        if (arguments.size() != 1)
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("project export requires one destination JSON file."));
        if (!project->exportJson(arguments.first(), &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context,
                             command,
                             QJsonObject{{QStringLiteral("path"), QFileInfo(arguments.first()).absoluteFilePath()}},
                             QStringLiteral("Exported %1").arg(QFileInfo(arguments.first()).absoluteFilePath()));
    }

    if (action == QStringLiteral("validate")) {
        const bool execution = takeFlag(&arguments, QStringLiteral("--execution"));
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected project validate argument: %1")
                                     .arg(arguments.first()));
        const ProjectValidation validation = execution
            ? project->validateForExecution() : project->validate();
        const QJsonObject value{
            {QStringLiteral("errors"), stringsToJson(validation.errors)},
            {QStringLiteral("mode"), execution ? QStringLiteral("execution")
                                                : QStringLiteral("draft")},
            {QStringLiteral("valid"), validation.ok()},
        };
        if (!validation.ok())
            return failureResult(context,
                                 CliExitCode::Validation,
                                 command,
                                 validation.message(),
                                 value);
        return successResult(context, command, value, QStringLiteral("Project is valid."));
    }

    return failureResult(context, CliExitCode::Usage, command,
                         QStringLiteral("Unknown project action: %1").arg(action));
}

CliResult runConfigCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("config");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("config requires show, set, add, remove, erase, or edit."));
    const QString action = arguments.takeFirst().toLower();
    if (action == QStringLiteral("show")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("config show does not accept positional arguments."));
        CliResult failure;
        const auto project = loadProject(context, command, &failure);
        return project ? successResult(context,
                                       command,
                                       project->toJson(),
                                       QString::fromUtf8(QJsonDocument(project->toJson())
                                                             .toJson(QJsonDocument::Indented)).trimmed())
                       : failure;
    }

    QList<EditOperation> edits;
    if (action == QStringLiteral("set") || action == QStringLiteral("add")
        || action == QStringLiteral("remove") || action == QStringLiteral("erase")) {
        const int expectedMinimum = action == QStringLiteral("erase") ? 1 : 2;
        const int expectedMaximum = action == QStringLiteral("remove") ? 2 : expectedMinimum;
        if (arguments.size() < expectedMinimum || arguments.size() > expectedMaximum) {
            return failureResult(context,
                                 CliExitCode::Usage,
                                 command,
                                 QStringLiteral("config %1 received the wrong number of arguments.")
                                     .arg(action));
        }
        EditOperation edit;
        edit.path = arguments.at(0);
        if (arguments.size() > 1)
            edit.rawValue = arguments.at(1);
        edit.kind = action == QStringLiteral("set") ? EditOperation::Kind::Set
                  : action == QStringLiteral("add") ? EditOperation::Kind::Add
                  : action == QStringLiteral("remove") ? EditOperation::Kind::Remove
                                                        : EditOperation::Kind::Erase;
        edits.append(edit);
    } else if (action == QStringLiteral("edit")) {
        for (qsizetype index = 0; index < arguments.size();) {
            const QString option = arguments.at(index++);
            EditOperation edit;
            if (option == QStringLiteral("--erase")) {
                edit.kind = EditOperation::Kind::Erase;
                if (index >= arguments.size())
                    return failureResult(context, CliExitCode::Usage, command,
                                         QStringLiteral("--erase requires a path."));
                edit.path = arguments.at(index++);
            } else if (option == QStringLiteral("--set") || option == QStringLiteral("--add")
                       || option == QStringLiteral("--remove")) {
                if (index + 1 >= arguments.size())
                    return failureResult(context, CliExitCode::Usage, command,
                                         QStringLiteral("%1 requires a path and value.").arg(option));
                edit.kind = option == QStringLiteral("--set") ? EditOperation::Kind::Set
                          : option == QStringLiteral("--add") ? EditOperation::Kind::Add
                                                               : EditOperation::Kind::Remove;
                edit.path = arguments.at(index++);
                edit.rawValue = arguments.at(index++);
            } else {
                return failureResult(context, CliExitCode::Usage, command,
                                     QStringLiteral("Unknown config edit option: %1").arg(option));
            }
            edits.append(edit);
        }
    } else {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Unknown config action: %1").arg(action));
    }
    return saveEditedProject(context, command, edits);
}

QJsonObject operationToJson(const ServicingOperation &operation)
{
    QJsonObject result = operation.toJson();
    result.insert(QStringLiteral("checkpointBefore"), operation.checkpointBefore);
    result.insert(QStringLiteral("command"), operation.previewCommand());
    result.insert(QStringLiteral("description"), operation.descriptionEn);
    result.insert(QStringLiteral("descriptionZhHk"), operation.descriptionZh);
    result.insert(QStringLiteral("mayRunInParallel"), operation.mayRunInParallel);
    result.insert(QStringLiteral("rebootRequired"), operation.rebootRequired);
    result.insert(QStringLiteral("requiresAdministrator"), operation.requiresAdministrator);
    result.insert(QStringLiteral("title"), operation.titleEn);
    result.insert(QStringLiteral("titleZhHk"), operation.titleZh);
    result.insert(QStringLiteral("writesMountedImage"), operation.writesMountedImage);
    return result;
}

QJsonArray operationsToJson(const QList<ServicingOperation> &operations)
{
    QJsonArray result;
    for (const ServicingOperation &operation : operations)
        result.append(operationToJson(operation));
    return result;
}

QString humanOperations(const QList<ServicingOperation> &operations,
                        const QStringList &warnings)
{
    QStringList lines;
    for (qsizetype index = 0; index < operations.size(); ++index) {
        const ServicingOperation &operation = operations.at(index);
        lines.append(QStringLiteral("%1. [%2] %3\n   %4")
                         .arg(index + 1)
                         .arg(ServicingPlan::operationKindName(operation.kind),
                              operation.titleEn,
                              operation.previewCommand()));
    }
    if (!warnings.isEmpty()) {
        lines.append(QStringLiteral("Warnings:"));
        for (const QString &warning : warnings)
            lines.append(QStringLiteral("- %1").arg(warning));
    }
    return lines.isEmpty() ? QStringLiteral("The plan has no operations.")
                           : lines.join(QLatin1Char('\n'));
}

QByteArray boundedTail(QByteArray value)
{
    constexpr qsizetype maximumBytes = 64 * 1024;
    if (value.size() > maximumBytes)
        value = QByteArrayLiteral("[earlier output omitted]\n") + value.right(maximumBytes);
    return value;
}

CliResult runPlanCommand(const CommandContext &context,
                         QStringList arguments,
                         const QString &action,
                         const CliDependencies &dependencies)
{
    const QString command = action;
    const bool applyRequested = action == QStringLiteral("apply");
    const bool dryRunFlag = action == QStringLiteral("dry-run")
        || takeFlag(&arguments, QStringLiteral("--dry-run"));
    const bool confirmed = takeFlag(&arguments, QStringLiteral("--yes"));
    QString scriptPath;
    QString error;
    if (!takeOption(&arguments, QStringLiteral("--script"), &scriptPath, &error))
        return failureResult(context, CliExitCode::Usage, command, error);
    if (!arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Unexpected %1 argument: %2")
                                 .arg(action, arguments.first()));

    CliResult failure;
    const auto project = loadProject(context, command, &failure);
    if (!project)
        return failure;

    if (applyRequested && !dryRunFlag && !project->options.dryRun) {
        const ProjectValidation validation = project->validateForExecution();
        if (!validation.ok()) {
            return failureResult(context,
                                 CliExitCode::Validation,
                                 command,
                                 validation.message(),
                                 stringsToJson(validation.errors));
        }
    }

    const ServicingPlanResult plan = ServicingPlan::build(*project);
    if (!plan.ok()) {
        return failureResult(context,
                             CliExitCode::Validation,
                             command,
                             plan.errors.join(QLatin1Char('\n')),
                             stringsToJson(plan.errors));
    }
    if (!scriptPath.isEmpty()
        && !ServicingPlan::exportPowerShell(*project, plan.operations, scriptPath, &error)) {
        return failureResult(context, CliExitCode::IoError, command, error);
    }

    const bool previewOnly = !applyRequested || dryRunFlag || project->options.dryRun;
    QJsonObject result{
        {QStringLiteral("destructiveCount"), plan.destructiveCount()},
        {QStringLiteral("dryRun"), previewOnly},
        {QStringLiteral("operations"), operationsToJson(plan.operations)},
        {QStringLiteral("script"), scriptPath.isEmpty()
             ? QString() : QFileInfo(scriptPath).absoluteFilePath()},
        {QStringLiteral("warnings"), stringsToJson(plan.warnings)},
    };
    if (previewOnly) {
        return successResult(context,
                             command,
                             result,
                             humanOperations(plan.operations, plan.warnings));
    }

    if (plan.destructiveCount() > 0 && !confirmed) {
        return failureResult(
            context,
            CliExitCode::ConfirmationRequired,
            command,
            QStringLiteral(
                "This plan has %1 destructive operation(s). Re-run noninteractively with --yes after reviewing 'dry-run'.")
                .arg(plan.destructiveCount()),
            result);
    }

    QSet<QString> completed;
    QJsonArray executed;
    for (const ServicingOperation &operation : plan.operations) {
        for (const QString &dependency : operation.dependsOn) {
            if (!completed.contains(dependency)) {
                return failureResult(context,
                                     CliExitCode::InternalError,
                                     command,
                                     QStringLiteral("Plan dependency %1 for %2 was not completed.")
                                         .arg(dependency, operation.id));
            }
        }

        const CliProcessResult process = dependencies.processInvoker(
            operation.executable, operation.arguments, operation.workingDirectory);
        QJsonObject execution{
            {QStringLiteral("exitCode"), process.exitCode},
            {QStringLiteral("finished"), process.finished},
            {QStringLiteral("id"), operation.id},
            {QStringLiteral("started"), process.started},
            {QStringLiteral("stderr"), QString::fromUtf8(boundedTail(process.standardError))},
            {QStringLiteral("stdout"), QString::fromUtf8(boundedTail(process.standardOutput))},
        };
        executed.append(execution);
        if (!process.ok()) {
            result.insert(QStringLiteral("executed"), executed);
            return failureResult(context,
                                 CliExitCode::ExternalProcessFailed,
                                 command,
                                 QStringLiteral("Operation '%1' failed with exit code %2.")
                                     .arg(operation.titleEn)
                                     .arg(process.exitCode),
                                 result);
        }
        completed.insert(operation.id);
    }

    result.insert(QStringLiteral("dryRun"), false);
    result.insert(QStringLiteral("executed"), executed);
    return successResult(context,
                         command,
                         result,
                         QStringLiteral("Completed %1 servicing operation(s).")
                             .arg(plan.operations.size()));
}

CliResult runHistoryCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("history");
    if (context.projectDirectory.trimmed().isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Select a project with --project <folder>."));
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("history requires log, undo, or redo."));

    const QString action = arguments.takeFirst().toLower();
    GitHistory history(QDir(context.projectDirectory).absolutePath(),
                       {QString::fromLatin1(ProjectConfig::FileName)});
    QString error;
    if (action == QStringLiteral("log")) {
        QString limitText;
        if (!takeOption(&arguments, QStringLiteral("--limit"), &limitText, &error))
            return failureResult(context, CliExitCode::Usage, command, error);
        int limit = 100;
        if (!limitText.isEmpty()) {
            const auto parsed = positiveInteger(limitText, 1, 10'000, &error);
            if (!parsed)
                return failureResult(context, CliExitCode::Usage, command, error);
            limit = *parsed;
        }
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected history log argument: %1")
                                     .arg(arguments.first()));
        const QList<GitCommit> commits = history.history(limit, &error);
        if (!error.isEmpty())
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context, command, commitsToJson(commits), humanCommits(commits));
    }

    if (action != QStringLiteral("undo") && action != QStringLiteral("redo"))
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Unknown history action: %1").arg(action));
    if (!arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("history %1 does not accept arguments.").arg(action));

    const QList<GitCommit> before = history.history(1, &error);
    if (!error.isEmpty())
        return failureResult(context, CliExitCode::IoError, command, error);
    if (action == QStringLiteral("redo")
        && (before.isEmpty() || !before.first().isRevert())) {
        return failureResult(context,
                             CliExitCode::Conflict,
                             command,
                             QStringLiteral("The latest action is not an undo, so there is nothing to redo."));
    }
    if (!history.revertLatest(&error))
        return failureResult(context, CliExitCode::Conflict, command, error);
    const QList<GitCommit> after = history.history(1, &error);
    if (!error.isEmpty() || after.isEmpty())
        return failureResult(context, CliExitCode::IoError, command,
                             error.isEmpty() ? QStringLiteral("History changed but could not be read.") : error);
    return successResult(context,
                         command,
                         commitToJson(after.first()),
                         QStringLiteral("%1: %2")
                             .arg(action == QStringLiteral("undo") ? QStringLiteral("Undone")
                                                                    : QStringLiteral("Redone"),
                                  after.first().subject));
}

QJsonObject notificationToJson(const Notification &notification)
{
    return QJsonObject{
        {QStringLiteral("createdAt"), notification.createdAt.toString(Qt::ISODateWithMs)},
        {QStringLiteral("data"), notification.data},
        {QStringLiteral("deletedAt"), notification.deletedAt.toString(Qt::ISODateWithMs)},
        {QStringLiteral("dismissed"), notification.isDismissed},
        {QStringLiteral("id"), notification.id},
        {QStringLiteral("message"), notification.message},
        {QStringLiteral("read"), notification.isRead},
        {QStringLiteral("severity"), notification.severity},
        {QStringLiteral("softDeleted"), notification.isDeleted},
        {QStringLiteral("source"), notification.source},
        {QStringLiteral("title"), notification.title},
        {QStringLiteral("updatedAt"), notification.updatedAt.toString(Qt::ISODateWithMs)},
    };
}

QJsonArray notificationsToJson(const QList<Notification> &notifications)
{
    QJsonArray result;
    for (const Notification &notification : notifications)
        result.append(notificationToJson(notification));
    return result;
}

QJsonObject notificationEventToJson(const NotificationEvent &event)
{
    return QJsonObject{
        {QStringLiteral("action"), event.action},
        {QStringLiteral("details"), event.details},
        {QStringLiteral("eventId"), event.eventId},
        {QStringLiteral("notificationId"), event.notificationId},
        {QStringLiteral("occurredAt"), event.occurredAt.toString(Qt::ISODateWithMs)},
    };
}

QString humanNotifications(const QList<Notification> &notifications)
{
    QStringList lines;
    for (const Notification &notification : notifications) {
        QStringList flags;
        flags.append(notification.isRead ? QStringLiteral("read") : QStringLiteral("unread"));
        if (notification.isDismissed)
            flags.append(QStringLiteral("dismissed"));
        if (notification.isDeleted)
            flags.append(QStringLiteral("deleted"));
        lines.append(QStringLiteral("%1  [%2/%3] %4 — %5")
                         .arg(notification.id,
                              notification.severity,
                              flags.join(QLatin1Char(',')),
                              notification.title,
                              notification.message));
    }
    return lines.isEmpty() ? QStringLiteral("No notifications.") : lines.join(QLatin1Char('\n'));
}

CliResult runNotificationCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("notifications");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("notifications requires list, new, read, unread, dismiss, restore, delete, events, history, undo, or redo."));
    const QString action = arguments.takeFirst().toLower();
    NotificationStore store(context.notificationDirectory.isEmpty()
                                ? NotificationStore::defaultStoreDirectory()
                                : context.notificationDirectory);
    QString error;
    if (!store.initialize(&error))
        return failureResult(context, CliExitCode::IoError, command, error);

    if (action == QStringLiteral("list")) {
        const bool includeAll = takeFlag(&arguments, QStringLiteral("--all"));
        const bool includeDismissed = takeFlag(&arguments, QStringLiteral("--include-dismissed"))
            || includeAll;
        const bool includeDeleted = takeFlag(&arguments, QStringLiteral("--include-deleted"))
            || includeAll;
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected notifications list argument: %1")
                                     .arg(arguments.first()));
        const QList<Notification> notifications = store.list(includeDismissed, includeDeleted, &error);
        if (!error.isEmpty())
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context,
                             command,
                             notificationsToJson(notifications),
                             humanNotifications(notifications));
    }

    if (action == QStringLiteral("new")) {
        QString title;
        QString message;
        QString severity = QStringLiteral("info");
        QString source;
        QString dataText;
        if (!takeOption(&arguments, QStringLiteral("--title"), &title, &error, true)
            || !takeOption(&arguments, QStringLiteral("--message"), &message, &error, true)
            || !takeOption(&arguments, QStringLiteral("--severity"), &severity, &error)
            || !takeOption(&arguments, QStringLiteral("--source"), &source, &error)
            || !takeOption(&arguments, QStringLiteral("--data"), &dataText, &error)) {
            return failureResult(context, CliExitCode::Usage, command, error);
        }
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected notifications new argument: %1")
                                     .arg(arguments.first()));
        QJsonObject data;
        if (!dataText.isEmpty()) {
            const QJsonValue parsed = parseCliValue(dataText);
            if (!parsed.isObject())
                return failureResult(context, CliExitCode::Validation, command,
                                     QStringLiteral("--data must be one JSON object."));
            data = parsed.toObject();
        }
        const QString id = store.addNotification(title, message, severity, source, data, &error);
        if (id.isEmpty())
            return failureResult(context, CliExitCode::Validation, command, error);
        const auto notification = store.find(id, &error);
        if (!notification)
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context,
                             command,
                             notificationToJson(*notification),
                             QStringLiteral("Created notification %1").arg(id));
    }

    if (action == QStringLiteral("events")) {
        QString limitText;
        if (!takeOption(&arguments, QStringLiteral("--limit"), &limitText, &error))
            return failureResult(context, CliExitCode::Usage, command, error);
        int limit = 1'000;
        if (!limitText.isEmpty()) {
            const auto parsed = positiveInteger(limitText, 1, 10'000, &error);
            if (!parsed)
                return failureResult(context, CliExitCode::Usage, command, error);
            limit = *parsed;
        }
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected notifications events argument."));
        const QList<NotificationEvent> events = store.events(limit, &error);
        if (!error.isEmpty())
            return failureResult(context, CliExitCode::IoError, command, error);
        QJsonArray values;
        QStringList lines;
        for (const NotificationEvent &event : events) {
            values.append(notificationEventToJson(event));
            lines.append(QStringLiteral("%1  %2  %3")
                             .arg(event.occurredAt.toString(Qt::ISODate),
                                  event.action,
                                  event.notificationId));
        }
        return successResult(context, command, values,
                             lines.isEmpty() ? QStringLiteral("No notification events.")
                                             : lines.join(QLatin1Char('\n')));
    }

    if (action == QStringLiteral("history")) {
        const QList<GitCommit> commits = store.history(100, &error);
        if (!error.isEmpty())
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context, command, commitsToJson(commits), humanCommits(commits));
    }

    if (action == QStringLiteral("undo") || action == QStringLiteral("redo")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("notifications %1 does not accept arguments.").arg(action));
        const QList<GitCommit> before = store.history(1, &error);
        if (!error.isEmpty())
            return failureResult(context, CliExitCode::IoError, command, error);
        if (action == QStringLiteral("redo")
            && (before.isEmpty() || !before.first().isRevert())) {
            return failureResult(context, CliExitCode::Conflict, command,
                                 QStringLiteral("The latest notification action is not an undo."));
        }
        if (!store.revertLatest(&error))
            return failureResult(context, CliExitCode::Conflict, command, error);
        const QList<GitCommit> after = store.history(1, &error);
        return after.isEmpty()
            ? failureResult(context, CliExitCode::IoError, command,
                            QStringLiteral("Notification history changed but could not be read."))
            : successResult(context, command, commitToJson(after.first()), after.first().subject);
    }

    static const QSet<QString> mutations{
        QStringLiteral("read"), QStringLiteral("unread"), QStringLiteral("dismiss"),
        QStringLiteral("restore"), QStringLiteral("delete")};
    if (!mutations.contains(action))
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Unknown notifications action: %1").arg(action));
    if (arguments.size() != 1)
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("notifications %1 requires one notification ID.").arg(action));
    const QString id = arguments.first();
    const bool changed = action == QStringLiteral("read") ? store.markRead(id, &error)
                       : action == QStringLiteral("unread") ? store.markUnread(id, &error)
                       : action == QStringLiteral("dismiss") ? store.dismiss(id, &error)
                       : action == QStringLiteral("restore") ? store.restore(id, &error)
                                                               : store.softDelete(id, &error);
    if (!changed)
        return failureResult(context,
                             error.contains(QStringLiteral("not found"), Qt::CaseInsensitive)
                                 ? CliExitCode::NotFound : CliExitCode::Conflict,
                             command,
                             error);
    const auto notification = store.find(id, &error);
    if (!notification)
        return failureResult(context, CliExitCode::IoError, command, error);
    return successResult(context,
                         command,
                         notificationToJson(*notification),
                         QStringLiteral("Notification %1: %2").arg(action, id));
}

std::optional<UnattendProfile> loadUnattendProfile(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path)) {
        setError(error, QStringLiteral("Unattended profile does not exist: %1").arg(path));
        return std::nullopt;
    }
    if (QFileInfo(path).suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
        return UnattendProfile::importXml(path, error);
    return UnattendProfile::importJson(path, error);
}

bool writeUnattendProfile(const UnattendProfile &profile,
                          const QString &path,
                          QString format,
                          QString *error)
{
    if (path.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Choose an unattended output file."));
        return false;
    }
    if (format.isEmpty())
        format = QFileInfo(path).suffix();
    format = format.trimmed().toLower();
    if (format == QStringLiteral("xml"))
        return profile.exportXml(path, error);
    if (format == QStringLiteral("json"))
        return profile.exportJson(path, error);
    setError(error, QStringLiteral("Unattended output format must be json or xml."));
    return false;
}

QJsonObject validationToJson(const UnattendValidation &validation)
{
    return QJsonObject{
        {QStringLiteral("errors"), stringsToJson(validation.errors)},
        {QStringLiteral("valid"), validation.ok()},
        {QStringLiteral("warnings"), stringsToJson(validation.warnings)},
    };
}

QString humanUnattend(const UnattendProfile &profile)
{
    return QString::fromUtf8(QJsonDocument(profile.toJson()).toJson(QJsonDocument::Indented)).trimmed();
}

QJsonObject gvlkToJson(const ProductKeyEntry &entry)
{
    return QJsonObject{
        {QStringLiteral("channel"), entry.channel},
        {QStringLiteral("documentationUrl"), entry.documentationUrl},
        {QStringLiteral("edition"), entry.edition},
        {QStringLiteral("key"), entry.key},
        {QStringLiteral("licensingNotice"), entry.licensingNotice},
    };
}

std::optional<ProductKeyEntry> findGvlk(const QString &edition, QString *error)
{
    const QList<ProductKeyEntry> entries = UnattendBuilder::microsoftPublishedGvlks();
    for (const ProductKeyEntry &entry : entries) {
        if (entry.edition.compare(edition, Qt::CaseInsensitive) == 0) {
            setError(error, {});
            return entry;
        }
    }
    QList<ProductKeyEntry> matches;
    for (const ProductKeyEntry &entry : entries) {
        if (entry.edition.contains(edition, Qt::CaseInsensitive))
            matches.append(entry);
    }
    if (matches.size() == 1) {
        setError(error, {});
        return matches.first();
    }
    if (matches.isEmpty())
        setError(error, QStringLiteral("No Microsoft-published GVLK matches '%1'.").arg(edition));
    else
        setError(error, QStringLiteral("GVLK edition '%1' is ambiguous; use the complete edition name.")
                            .arg(edition));
    return std::nullopt;
}

CliResult finishUnattendProfile(const CommandContext &context,
                                const QString &command,
                                const UnattendProfile &profile,
                                const QString &output,
                                const QString &format,
                                const QJsonObject &extra = {})
{
    const UnattendValidation validation = profile.validate();
    if (!validation.ok()) {
        return failureResult(context,
                             CliExitCode::Validation,
                             command,
                             validation.errors.join(QLatin1Char('\n')),
                             validationToJson(validation));
    }
    QString error;
    if (!output.isEmpty() && !writeUnattendProfile(profile, output, format, &error))
        return failureResult(context, CliExitCode::IoError, command, error);

    QJsonObject result = extra;
    result.insert(QStringLiteral("output"), output.isEmpty()
        ? QString() : QFileInfo(output).absoluteFilePath());
    result.insert(QStringLiteral("profile"), profile.toJson());
    result.insert(QStringLiteral("validation"), validationToJson(validation));
    const QString human = output.isEmpty()
        ? humanUnattend(profile)
        : QStringLiteral("Wrote %1").arg(QFileInfo(output).absoluteFilePath());
    return successResult(context, command, result, human);
}

CliResult runUnattendCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("unattend");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("unattend requires template, import, export, validate, computer-name, or gvlk."));
    const QString action = arguments.takeFirst().toLower();
    QString error;

    if (action == QStringLiteral("template")) {
        if (arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("unattend template requires full or ai-development."));
        const QString templateName = arguments.takeFirst().toLower();
        UnattendProfile profile;
        if (templateName == QStringLiteral("full")
            || templateName == QStringLiteral("full-automation")) {
            profile = UnattendBuilder::fullAutomationTemplate();
        } else if (templateName == QStringLiteral("ai-development")
                   || templateName == QStringLiteral("ai")) {
            profile = UnattendBuilder::aiDevelopmentTemplate();
        } else {
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unknown unattended template: %1").arg(templateName));
        }
        QString output;
        QString format;
        if (!takeOption(&arguments, QStringLiteral("--output"), &output, &error)
            || !takeOption(&arguments, QStringLiteral("--format"), &format, &error)) {
            return failureResult(context, CliExitCode::Usage, command, error);
        }
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected unattended template argument: %1")
                                     .arg(arguments.first()));
        return finishUnattendProfile(context, command, profile, output, format);
    }

    if (action == QStringLiteral("gvlk")) {
        QString subcommand = QStringLiteral("list");
        if (!arguments.isEmpty()
            && (arguments.first() == QStringLiteral("list")
                || arguments.first() == QStringLiteral("set"))) {
            subcommand = arguments.takeFirst();
        }
        if (subcommand == QStringLiteral("list")) {
            QString edition;
            if (!takeOption(&arguments, QStringLiteral("--edition"), &edition, &error))
                return failureResult(context, CliExitCode::Usage, command, error);
            if (!arguments.isEmpty())
                return failureResult(context, CliExitCode::Usage, command,
                                     QStringLiteral("Unexpected unattended gvlk list argument."));
            QJsonArray values;
            QStringList lines;
            for (const ProductKeyEntry &entry : UnattendBuilder::microsoftPublishedGvlks()) {
                if (!edition.isEmpty() && !entry.edition.contains(edition, Qt::CaseInsensitive))
                    continue;
                values.append(gvlkToJson(entry));
                lines.append(QStringLiteral("%1  %2\n  %3")
                                 .arg(entry.key, entry.edition, entry.licensingNotice));
            }
            return successResult(context,
                                 command,
                                 values,
                                 lines.isEmpty() ? QStringLiteral("No matching published GVLKs.")
                                                 : lines.join(QLatin1Char('\n')));
        }

        if (arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("unattend gvlk set requires a profile file."));
        const QString input = arguments.takeFirst();
        QString edition;
        QString output;
        QString format;
        if (!takeOption(&arguments, QStringLiteral("--edition"), &edition, &error, true)
            || !takeOption(&arguments, QStringLiteral("--output"), &output, &error)
            || !takeOption(&arguments, QStringLiteral("--format"), &format, &error)) {
            return failureResult(context, CliExitCode::Usage, command, error);
        }
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected unattended gvlk set argument."));
        auto profile = loadUnattendProfile(input, &error);
        if (!profile)
            return failureResult(context, CliExitCode::Validation, command, error);
        const auto entry = findGvlk(edition, &error);
        if (!entry)
            return failureResult(context, CliExitCode::Validation, command, error);
        profile->setValue(SetupPass::WindowsPE,
                          QStringLiteral("Microsoft-Windows-Setup"),
                          {QStringLiteral("UserData"), QStringLiteral("ProductKey"),
                           QStringLiteral("Key")},
                          entry->key);
        if (output.isEmpty())
            output = input;
        return finishUnattendProfile(context,
                                     command,
                                     *profile,
                                     output,
                                     format,
                                     QJsonObject{{QStringLiteral("gvlk"), gvlkToJson(*entry)}});
    }

    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("unattend %1 requires an input profile file.").arg(action));
    const QString input = arguments.takeFirst();
    auto profile = loadUnattendProfile(input, &error);
    if (!profile)
        return failureResult(context,
                             QFileInfo::exists(input) ? CliExitCode::Validation
                                                      : CliExitCode::NotFound,
                             command,
                             error);

    if (action == QStringLiteral("validate")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("unattend validate accepts one file."));
        const UnattendValidation validation = profile->validate();
        if (!validation.ok())
            return failureResult(context,
                                 CliExitCode::Validation,
                                 command,
                                 validation.errors.join(QLatin1Char('\n')),
                                 validationToJson(validation));
        QStringList lines{QStringLiteral("Unattended profile is valid.")};
        for (const QString &warning : validation.warnings)
            lines.append(QStringLiteral("Warning: %1").arg(warning));
        return successResult(context, command, validationToJson(validation),
                             lines.join(QLatin1Char('\n')));
    }

    QString output;
    QString format;
    if (!takeOption(&arguments, QStringLiteral("--output"), &output, &error)
        || !takeOption(&arguments, QStringLiteral("--format"), &format, &error)) {
        return failureResult(context, CliExitCode::Usage, command, error);
    }
    if (action == QStringLiteral("import") || action == QStringLiteral("export")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected unattended %1 argument.").arg(action));
        if (action == QStringLiteral("export") && output.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("unattend export requires --output."));
        return finishUnattendProfile(context, command, *profile, output, format);
    }

    if (action == QStringLiteral("computer-name")) {
        QString mode;
        QString value;
        QString prefix;
        if (!takeOption(&arguments, QStringLiteral("--mode"), &mode, &error, true)
            || !takeOption(&arguments, QStringLiteral("--value"), &value, &error)
            || !takeOption(&arguments, QStringLiteral("--prefix"), &prefix, &error)) {
            return failureResult(context, CliExitCode::Usage, command, error);
        }
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected unattended computer-name argument."));
        mode = mode.trimmed().toLower();
        if (mode == QStringLiteral("random")) {
            profile->computerNameMode = ComputerNameMode::Random;
        } else if (mode == QStringLiteral("fixed")) {
            if (value.isEmpty())
                return failureResult(context, CliExitCode::Usage, command,
                                     QStringLiteral("Fixed computer-name mode requires --value."));
            profile->computerNameMode = ComputerNameMode::Fixed;
            profile->computerName = value;
        } else if (mode == QStringLiteral("prompt")) {
            profile->computerNameMode = ComputerNameMode::Prompt;
        } else if (mode == QStringLiteral("serial")
                   || mode == QStringLiteral("serial-number")) {
            profile->computerNameMode = ComputerNameMode::SerialNumber;
            profile->serialPrefix = prefix;
        } else {
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Computer-name mode must be random, fixed, prompt, or serial."));
        }
        profile->applyComputerNameBehavior();
        if (output.isEmpty())
            output = input;
        return finishUnattendProfile(context, command, *profile, output, format);
    }

    return failureResult(context, CliExitCode::Usage, command,
                         QStringLiteral("Unknown unattended action: %1").arg(action));
}

bool writeBytesAtomic(const QString &path, const QByteArray &bytes, QString *error)
{
    const QFileInfo destination(path);
    if (!QDir().mkpath(destination.absolutePath())) {
        setError(error, QStringLiteral("Could not create output directory: %1")
                            .arg(destination.absolutePath()));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not open %1: %2").arg(path, file.errorString()));
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        setError(error, QStringLiteral("Could not write %1: %2").arg(path, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QStringLiteral("Could not finish %1: %2").arg(path, file.errorString()));
        return false;
    }
    setError(error, {});
    return true;
}

QJsonObject packageCommandToJson(const PackageCommand &command)
{
    return QJsonObject{
        {QStringLiteral("arguments"), stringsToJson(command.arguments)},
        {QStringLiteral("executable"), command.executable},
    };
}

QString packageCommandPreview(const PackageCommand &command)
{
    QStringList tokens{ServicingPlan::quoteWindowsArgument(command.executable)};
    for (const QString &argument : command.arguments)
        tokens.append(ServicingPlan::quoteWindowsArgument(argument));
    return tokens.join(QLatin1Char(' '));
}

QJsonObject packagePlanEntryToJson(const PackageEntry &package)
{
    return QJsonObject{
        {QStringLiteral("dependencies"), stringsToJson(package.dependencies)},
        {QStringLiteral("displayName"), package.displayName},
        {QStringLiteral("enabled"), package.enabled},
        {QStringLiteral("id"), package.id},
        {QStringLiteral("install"), packageCommandToJson(
             PackageStudio::effectiveInstallCommand(package))},
        {QStringLiteral("optional"), package.optional},
        {QStringLiteral("provider"), PackageStudio::providerName(package.provider)},
        {QStringLiteral("requiresNetwork"), package.requiresNetwork},
        {QStringLiteral("verify"), packageCommandToJson(package.verifyCommand)},
    };
}

std::optional<PackageProfile> loadPackageProfile(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path)) {
        setError(error, QStringLiteral("Package profile does not exist: %1").arg(path));
        return std::nullopt;
    }
    return PackageStudio::importJson(path, error);
}

QString humanPackageProfile(const PackageProfile &profile)
{
    return QString::fromUtf8(QJsonDocument(PackageStudio::toJson(profile))
                                 .toJson(QJsonDocument::Indented)).trimmed();
}

bool safeRelativePayload(const QString &path)
{
    if (path.trimmed().isEmpty() || QFileInfo(path).isAbsolute())
        return false;
    const QString clean = QDir::cleanPath(path);
    return clean != QStringLiteral("..")
        && !clean.startsWith(QStringLiteral("../"))
        && !clean.startsWith(QStringLiteral("..\\"));
}

std::optional<QString> sha256File(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not hash %1: %2").arg(path, file.errorString()));
        return std::nullopt;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        setError(error, QStringLiteral("Could not read all bytes while hashing %1.").arg(path));
        return std::nullopt;
    }
    setError(error, {});
    return QString::fromLatin1(hash.result().toHex());
}

bool stageOfflinePayloads(const PackageProfile &profile,
                          const QString &profilePath,
                          const QString &stageDirectory,
                          QJsonArray *staged,
                          QString *error)
{
    const QString sourceDirectory = QFileInfo(profilePath).absolutePath();
    for (const PackageEntry &package : profile.packages) {
        if (!package.enabled || package.offlinePayload.trimmed().isEmpty())
            continue;
        if (!safeRelativePayload(package.offlinePayload)) {
            setError(error, QStringLiteral("Package '%1' offline payload must stay relative to the profile: %2")
                                .arg(package.id, package.offlinePayload));
            return false;
        }
        const QString source = QDir(sourceDirectory).absoluteFilePath(package.offlinePayload);
        const QString destination = QDir(stageDirectory).absoluteFilePath(package.offlinePayload);
        if (!QFileInfo::exists(source)) {
            setError(error, QStringLiteral("Package '%1' offline payload is missing: %2")
                                .arg(package.id, source));
            return false;
        }
        const auto actualHash = sha256File(source, error);
        if (!actualHash)
            return false;
        if (!package.expectedSha256.isEmpty()
            && actualHash->compare(package.expectedSha256, Qt::CaseInsensitive) != 0) {
            setError(error, QStringLiteral("Package '%1' SHA-256 mismatch. Expected %2, got %3.")
                                .arg(package.id, package.expectedSha256, *actualHash));
            return false;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            setError(error, QStringLiteral("Could not create payload staging folder: %1")
                                .arg(QFileInfo(destination).absolutePath()));
            return false;
        }
        if (QFileInfo(source).absoluteFilePath() != QFileInfo(destination).absoluteFilePath()) {
            QFile::remove(destination);
            if (!QFile::copy(source, destination)) {
                setError(error, QStringLiteral("Could not stage payload %1 to %2")
                                    .arg(source, destination));
                return false;
            }
        }
        staged->append(QJsonObject{
            {QStringLiteral("id"), package.id},
            {QStringLiteral("path"), destination},
            {QStringLiteral("sha256"), *actualHash},
        });
    }
    setError(error, {});
    return true;
}

CliResult runPackageCommand(const CommandContext &context,
                            QStringList arguments,
                            const CliDependencies &dependencies)
{
    const QString command = QStringLiteral("package");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("package requires catalog, template, import, export, validate, plan, stage, or ensure-opencode."));
    const QString action = arguments.takeFirst().toLower();
    QString error;

    if (action == QStringLiteral("catalog")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("package catalog does not accept arguments."));
        QJsonArray packages;
        QStringList lines;
        for (const PackageEntry &package : PackageStudio::builtInCatalog()) {
            packages.append(packagePlanEntryToJson(package));
            lines.append(QStringLiteral("%1  [%2] %3")
                             .arg(package.id,
                                  PackageStudio::providerName(package.provider),
                                  package.displayName));
        }
        return successResult(context, command, packages, lines.join(QLatin1Char('\n')));
    }

    if (action == QStringLiteral("template")) {
        if (arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("package template requires ai-development."));
        const QString templateName = arguments.takeFirst().toLower();
        if (templateName != QStringLiteral("ai-development")
            && templateName != QStringLiteral("ai")
            && templateName != QStringLiteral("full-ai-development")) {
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unknown package template: %1").arg(templateName));
        }
        QString output;
        if (!takeOption(&arguments, QStringLiteral("--output"), &output, &error))
            return failureResult(context, CliExitCode::Usage, command, error);
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected package template argument."));
        const PackageProfile profile = PackageStudio::fullAiDevelopmentTemplate();
        if (!output.isEmpty() && !PackageStudio::exportJson(profile, output, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context,
                             command,
                             PackageStudio::toJson(profile),
                             output.isEmpty() ? humanPackageProfile(profile)
                                              : QStringLiteral("Wrote %1")
                                                    .arg(QFileInfo(output).absoluteFilePath()));
    }

    if (action == QStringLiteral("ensure-opencode")) {
        const bool dryRun = takeFlag(&arguments, QStringLiteral("--dry-run"));
        const bool confirmed = takeFlag(&arguments, QStringLiteral("--yes"));
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected package ensure-opencode argument."));

        const QList<PackageEntry> catalog = PackageStudio::builtInCatalog();
        PackageProfile profile;
        profile.name = QStringLiteral("Ensure OpenCode");
        for (const PackageEntry &entry : catalog) {
            if (entry.id == QStringLiteral("nodejs-lts")
                || entry.id == QStringLiteral("opencode-cli")) {
                profile.packages.append(entry);
            }
        }
        const auto ordered = PackageStudio::dependencyOrder(profile, &error);
        if (!ordered)
            return failureResult(context, CliExitCode::InternalError, command, error);

        QJsonArray status;
        QList<PackageEntry> missing;
        for (const PackageEntry &package : *ordered) {
            const CliProcessResult verification = dependencies.processInvoker(
                package.verifyCommand.executable, package.verifyCommand.arguments, {});
            const bool installed = verification.ok();
            status.append(QJsonObject{
                {QStringLiteral("id"), package.id},
                {QStringLiteral("install"), packageCommandToJson(
                     PackageStudio::effectiveInstallCommand(package))},
                {QStringLiteral("installed"), installed},
                {QStringLiteral("verify"), packageCommandToJson(package.verifyCommand)},
            });
            if (!installed)
                missing.append(package);
        }

        QJsonObject result{
            {QStringLiteral("dryRun"), dryRun},
            {QStringLiteral("packages"), status},
        };
        if (missing.isEmpty()) {
            result.insert(QStringLiteral("installed"), true);
            return successResult(context, command, result,
                                 QStringLiteral("OpenCode and its runtime are already installed."));
        }
        if (dryRun) {
            QStringList lines{QStringLiteral("Would install:")};
            for (const PackageEntry &package : missing)
                lines.append(QStringLiteral("- %1: %2")
                                 .arg(package.displayName,
                                      packageCommandPreview(
                                          PackageStudio::effectiveInstallCommand(package))));
            return successResult(context, command, result, lines.join(QLatin1Char('\n')));
        }
        if (!confirmed) {
            return failureResult(context,
                                 CliExitCode::ConfirmationRequired,
                                 command,
                                 QStringLiteral("OpenCode or its Node.js runtime is missing. Re-run with --yes to install the missing packages."),
                                 result);
        }

        QJsonArray installedEntries;
        for (const PackageEntry &package : missing) {
            const PackageCommand install = PackageStudio::effectiveInstallCommand(package);
            const CliProcessResult installation = dependencies.processInvoker(
                install.executable, install.arguments, {});
            if (!installation.ok()) {
                result.insert(QStringLiteral("installedEntries"), installedEntries);
                result.insert(QStringLiteral("failedPackage"), package.id);
                result.insert(QStringLiteral("stderr"),
                              QString::fromUtf8(boundedTail(installation.standardError)));
                return failureResult(context,
                                     CliExitCode::ExternalProcessFailed,
                                     command,
                                     QStringLiteral("Could not install %1 (exit code %2).")
                                         .arg(package.displayName)
                                         .arg(installation.exitCode),
                                     result);
            }
            const CliProcessResult verification = dependencies.processInvoker(
                package.verifyCommand.executable, package.verifyCommand.arguments, {});
            if (!verification.ok()) {
                result.insert(QStringLiteral("installedEntries"), installedEntries);
                result.insert(QStringLiteral("failedPackage"), package.id);
                return failureResult(context,
                                     CliExitCode::ExternalProcessFailed,
                                     command,
                                     QStringLiteral("%1 installer completed, but live verification failed.")
                                         .arg(package.displayName),
                                     result);
            }
            installedEntries.append(package.id);
        }
        result.insert(QStringLiteral("dryRun"), false);
        result.insert(QStringLiteral("installed"), true);
        result.insert(QStringLiteral("installedEntries"), installedEntries);
        return successResult(context, command, result,
                             QStringLiteral("OpenCode and its required runtime are installed."));
    }

    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("package %1 requires a profile JSON file.").arg(action));
    const QString input = arguments.takeFirst();
    auto profile = loadPackageProfile(input, &error);
    if (!profile)
        return failureResult(context,
                             QFileInfo::exists(input) ? CliExitCode::Validation
                                                      : CliExitCode::NotFound,
                             command,
                             error);

    if (action == QStringLiteral("validate")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("package validate accepts one file."));
        const PackageStudioValidation validation = PackageStudio::validate(*profile);
        if (!validation.ok())
            return failureResult(context,
                                 CliExitCode::Validation,
                                 command,
                                 validation.message(),
                                 stringsToJson(validation.errors));
        return successResult(context,
                             command,
                             QJsonObject{{QStringLiteral("valid"), true}},
                             QStringLiteral("Package profile is valid."));
    }

    if (action == QStringLiteral("import") || action == QStringLiteral("export")) {
        QString output;
        if (!takeOption(&arguments, QStringLiteral("--output"), &output, &error))
            return failureResult(context, CliExitCode::Usage, command, error);
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected package %1 argument.").arg(action));
        if (action == QStringLiteral("export") && output.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("package export requires --output."));
        if (!output.isEmpty() && !PackageStudio::exportJson(*profile, output, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context,
                             command,
                             PackageStudio::toJson(*profile),
                             output.isEmpty() ? humanPackageProfile(*profile)
                                              : QStringLiteral("Wrote %1")
                                                    .arg(QFileInfo(output).absoluteFilePath()));
    }

    if (action == QStringLiteral("plan")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("package plan accepts one file."));
        const auto ordered = PackageStudio::dependencyOrder(*profile, &error);
        if (!ordered)
            return failureResult(context, CliExitCode::Validation, command, error);
        QJsonArray plan;
        QStringList lines;
        for (const PackageEntry &package : *ordered) {
            plan.append(packagePlanEntryToJson(package));
            lines.append(QStringLiteral("%1. %2\n   %3")
                             .arg(lines.size() + 1)
                             .arg(package.displayName,
                                  packageCommandPreview(
                                      PackageStudio::effectiveInstallCommand(package))));
        }
        return successResult(context, command, plan,
                             lines.isEmpty() ? QStringLiteral("No enabled packages.")
                                             : lines.join(QLatin1Char('\n')));
    }

    if (action == QStringLiteral("stage")) {
        QString directory;
        if (!takeOption(&arguments, QStringLiteral("--directory"), &directory, &error, true))
            return failureResult(context, CliExitCode::Usage, command, error);
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected package stage argument."));
        directory = QDir(directory).absolutePath();
        const PackageStudioValidation validation = PackageStudio::validate(*profile);
        if (!validation.ok())
            return failureResult(context, CliExitCode::Validation, command,
                                 validation.message(), stringsToJson(validation.errors));
        const QString script = PackageStudio::generateFirstLogonPowerShell(*profile, &error);
        if (script.isEmpty() && !error.isEmpty())
            return failureResult(context, CliExitCode::Validation, command, error);
        const QJsonObject manifest = PackageStudio::generateIsoStagingManifest(*profile, &error);
        if (manifest.isEmpty() && !error.isEmpty())
            return failureResult(context, CliExitCode::Validation, command, error);

        QJsonArray stagedPayloads;
        if (!stageOfflinePayloads(*profile, input, directory, &stagedPayloads, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        const QString profileOutput = QDir(directory).filePath(QStringLiteral("package-profile.json"));
        const QString manifestOutput = QDir(directory).filePath(QStringLiteral("staging-manifest.json"));
        const QString scriptOutput = QDir(directory).filePath(QStringLiteral("first-logon.ps1"));
        if (!PackageStudio::exportJson(*profile, profileOutput, &error)
            || !writeBytesAtomic(manifestOutput,
                                 QJsonDocument(manifest).toJson(QJsonDocument::Indented),
                                 &error)
            || !writeBytesAtomic(scriptOutput,
                                 QByteArrayLiteral("\xEF\xBB\xBF") + script.toUtf8(),
                                 &error)) {
            return failureResult(context, CliExitCode::IoError, command, error);
        }
        const QJsonObject result{
            {QStringLiteral("directory"), directory},
            {QStringLiteral("manifest"), manifestOutput},
            {QStringLiteral("profile"), profileOutput},
            {QStringLiteral("script"), scriptOutput},
            {QStringLiteral("stagedPayloads"), stagedPayloads},
        };
        return successResult(context, command, result,
                             QStringLiteral("Staged package studio files in %1").arg(directory));
    }

    return failureResult(context, CliExitCode::Usage, command,
                         QStringLiteral("Unknown package action: %1").arg(action));
}

QJsonObject stringMapToJson(const QMap<QString, QString> &values)
{
    QJsonObject result;
    for (auto iterator = values.constBegin(); iterator != values.constEnd(); ++iterator)
        result.insert(iterator.key(), iterator.value());
    return result;
}

QJsonObject gpoRegistryValueToJson(const GpoRegistryValue &value)
{
    QString kind;
    switch (value.kind) {
    case GpoValueKind::None: kind = QStringLiteral("none"); break;
    case GpoValueKind::Decimal: kind = QStringLiteral("decimal"); break;
    case GpoValueKind::String: kind = QStringLiteral("string"); break;
    case GpoValueKind::Delete: kind = QStringLiteral("delete"); break;
    }
    return QJsonObject{
        {QStringLiteral("kind"), kind},
        {QStringLiteral("value"), value.value},
    };
}

QJsonObject gpoAssignmentToJson(const GpoRegistryAssignment &assignment)
{
    return QJsonObject{
        {QStringLiteral("key"), assignment.key},
        {QStringLiteral("value"), gpoRegistryValueToJson(assignment.value)},
        {QStringLiteral("valueName"), assignment.valueName},
    };
}

QJsonArray gpoAssignmentsToJson(const QList<GpoRegistryAssignment> &assignments)
{
    QJsonArray result;
    for (const GpoRegistryAssignment &assignment : assignments)
        result.append(gpoAssignmentToJson(assignment));
    return result;
}

QJsonObject gpoElementToJson(const GpoElement &element)
{
    QJsonArray options;
    for (const GpoEnumOption &option : element.options) {
        options.append(QJsonObject{
            {QStringLiteral("assignments"), gpoAssignmentsToJson(option.assignments)},
            {QStringLiteral("displayName"), option.displayName},
            {QStringLiteral("localizedDisplayNames"),
             stringMapToJson(option.localizedDisplayNames)},
            {QStringLiteral("value"), gpoRegistryValueToJson(option.value)},
        });
    }
    QJsonObject result{
        {QStringLiteral("additive"), element.additive},
        {QStringLiteral("control"), element.materialControl()},
        {QStringLiteral("explicitValue"), element.explicitValue},
        {QStringLiteral("falseAssignments"), gpoAssignmentsToJson(element.falseAssignments)},
        {QStringLiteral("falseValue"), gpoRegistryValueToJson(element.falseValue)},
        {QStringLiteral("id"), element.id},
        {QStringLiteral("kind"), gpoElementKindName(element.kind)},
        {QStringLiteral("localizedPresentationLabels"),
         stringMapToJson(element.localizedPresentationLabels)},
        {QStringLiteral("maximumLength"), element.maximumLength ? *element.maximumLength : -1},
        {QStringLiteral("maximumStrings"), element.maximumStrings ? *element.maximumStrings : -1},
        {QStringLiteral("maximumValue"), element.maximumValue
             ? QJsonValue(static_cast<double>(*element.maximumValue)) : QJsonValue()},
        {QStringLiteral("minimumLength"), element.minimumLength ? *element.minimumLength : -1},
        {QStringLiteral("minimumValue"), element.minimumValue
             ? QJsonValue(static_cast<double>(*element.minimumValue)) : QJsonValue()},
        {QStringLiteral("options"), options},
        {QStringLiteral("presentationDefault"), element.presentationDefaultValue},
        {QStringLiteral("presentationLabel"), element.presentationLabel},
        {QStringLiteral("registryKey"), element.registryKey},
        {QStringLiteral("registryValueName"), element.registryValueName},
        {QStringLiteral("required"), element.required},
        {QStringLiteral("trueAssignments"), gpoAssignmentsToJson(element.trueAssignments)},
        {QStringLiteral("trueValue"), gpoRegistryValueToJson(element.trueValue)},
        {QStringLiteral("valuePrefix"), element.valuePrefix},
    };
    if (element.presentationSpinStep)
        result.insert(QStringLiteral("presentationSpinStep"),
                      static_cast<double>(*element.presentationSpinStep));
    return result;
}

QJsonObject gpoPolicyToJson(const GpoPolicy &policy)
{
    QJsonArray elements;
    for (const GpoElement &element : policy.elements)
        elements.append(gpoElementToJson(element));
    return QJsonObject{
        {QStringLiteral("categoryHierarchy"), stringsToJson(policy.categoryHierarchy)},
        {QStringLiteral("class"), gpoPolicyClassName(policy.policyClass)},
        {QStringLiteral("disabledAssignments"), gpoAssignmentsToJson(policy.disabledAssignments)},
        {QStringLiteral("disabledValue"), gpoRegistryValueToJson(policy.disabledValue)},
        {QStringLiteral("displayName"), policy.displayName},
        {QStringLiteral("elements"), elements},
        {QStringLiteral("enabledAssignments"), gpoAssignmentsToJson(policy.enabledAssignments)},
        {QStringLiteral("enabledValue"), gpoRegistryValueToJson(policy.enabledValue)},
        {QStringLiteral("explainText"), policy.explainText},
        {QStringLiteral("id"), policy.id},
        {QStringLiteral("localizedDisplayNames"), stringMapToJson(policy.localizedDisplayNames)},
        {QStringLiteral("localizedExplainTexts"), stringMapToJson(policy.localizedExplainTexts)},
        {QStringLiteral("namespace"), policy.policyNamespace},
        {QStringLiteral("qualifiedId"), policy.qualifiedId()},
        {QStringLiteral("registryKey"), policy.registryKey},
        {QStringLiteral("registryValueName"), policy.registryValueName},
        {QStringLiteral("sourceFile"), policy.sourceFile},
        {QStringLiteral("supportedOn"), policy.supportedOn},
    };
}

QJsonArray gpoPoliciesToJson(QList<GpoPolicy> policies)
{
    std::sort(policies.begin(), policies.end(), [](const GpoPolicy &left, const GpoPolicy &right) {
        return left.qualifiedId().compare(right.qualifiedId(), Qt::CaseInsensitive) < 0;
    });
    QJsonArray result;
    for (const GpoPolicy &policy : policies)
        result.append(gpoPolicyToJson(policy));
    return result;
}

bool loadGpoCatalog(QStringList *arguments,
                    GpoCatalog *catalog,
                    QString *error)
{
    QString path;
    if (!takeOption(arguments, QStringLiteral("--path"), &path, error))
        return false;
    const auto locales = takeRepeatedOption(arguments, QStringLiteral("--locale"), error);
    if (!locales)
        return false;
    return path.isEmpty() ? catalog->loadInstalled(*locales, error)
                          : catalog->loadFromDirectory(path, *locales, error);
}

QString humanPolicies(const QList<GpoPolicy> &policies)
{
    QList<GpoPolicy> sorted = policies;
    std::sort(sorted.begin(), sorted.end(), [](const GpoPolicy &left, const GpoPolicy &right) {
        return left.qualifiedId().compare(right.qualifiedId(), Qt::CaseInsensitive) < 0;
    });
    QStringList lines;
    for (const GpoPolicy &policy : sorted) {
        lines.append(QStringLiteral("%1  [%2] %3")
                         .arg(policy.qualifiedId(),
                              gpoPolicyClassName(policy.policyClass),
                              policy.displayName));
    }
    return lines.isEmpty() ? QStringLiteral("No policies.") : lines.join(QLatin1Char('\n'));
}

CliResult runGpoCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("gpo");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("gpo requires catalog, search, or export."));
    const QString action = arguments.takeFirst().toLower();
    QString error;
    GpoCatalog catalog;

    if (action == QStringLiteral("catalog")) {
        const bool summaryOnly = takeFlag(&arguments, QStringLiteral("--summary"));
        if (!loadGpoCatalog(&arguments, &catalog, &error))
            return failureResult(context, CliExitCode::Validation, command, error);
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected gpo catalog argument: %1")
                                     .arg(arguments.first()));
        QJsonObject result{
            {QStringLiteral("categories"), catalog.categories().size()},
            {QStringLiteral("locales"), stringsToJson(catalog.locales())},
            {QStringLiteral("path"), catalog.policyDefinitionsPath()},
            {QStringLiteral("policyCount"), catalog.policies().size()},
            {QStringLiteral("supportedDefinitions"), catalog.supportedDefinitions().size()},
            {QStringLiteral("warnings"), stringsToJson(catalog.warnings())},
        };
        if (!summaryOnly)
            result.insert(QStringLiteral("policies"), gpoPoliciesToJson(catalog.policies()));
        const QString human = QStringLiteral("Loaded %1 policies, %2 categories, and %3 support definitions from %4.%5")
                                  .arg(catalog.policies().size())
                                  .arg(catalog.categories().size())
                                  .arg(catalog.supportedDefinitions().size())
                                  .arg(catalog.policyDefinitionsPath())
                                  .arg(summaryOnly ? QString()
                                                   : QLatin1Char('\n') + humanPolicies(catalog.policies()));
        return successResult(context, command, result, human);
    }

    if (action == QStringLiteral("search")) {
        const bool regex = takeFlag(&arguments, QStringLiteral("--regex"));
        if (!loadGpoCatalog(&arguments, &catalog, &error))
            return failureResult(context, CliExitCode::Validation, command, error);
        if (arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("gpo search requires a query."));
        const QString query = arguments.join(QLatin1Char(' '));
        const QList<GpoPolicy> policies = catalog.search(
            query,
            regex ? GpoSearchMode::RegularExpression : GpoSearchMode::PlainText,
            &error);
        if (!error.isEmpty())
            return failureResult(context, CliExitCode::Validation, command, error);
        const QJsonObject result{
            {QStringLiteral("mode"), regex ? QStringLiteral("regex")
                                            : QStringLiteral("plain")},
            {QStringLiteral("policies"), gpoPoliciesToJson(policies)},
            {QStringLiteral("query"), query},
            {QStringLiteral("warnings"), stringsToJson(catalog.warnings())},
        };
        return successResult(context, command, result, humanPolicies(policies));
    }

    if (action == QStringLiteral("export")) {
        if (arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("gpo export requires a Markdown destination."));
        const QString output = arguments.takeFirst();
        QString primary;
        QString secondary;
        if (!takeOption(&arguments, QStringLiteral("--primary"), &primary, &error)
            || !takeOption(&arguments, QStringLiteral("--secondary"), &secondary, &error)
            || !loadGpoCatalog(&arguments, &catalog, &error)) {
            return failureResult(context, CliExitCode::Validation, command, error);
        }
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected gpo export argument: %1")
                                     .arg(arguments.first()));
        if (!catalog.exportMarkdown(output, primary, secondary, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context,
                             command,
                             QJsonObject{
                                 {QStringLiteral("path"), QFileInfo(output).absoluteFilePath()},
                                 {QStringLiteral("policies"), catalog.policies().size()},
                             },
                             QStringLiteral("Exported policy documentation to %1")
                                 .arg(QFileInfo(output).absoluteFilePath()));
    }

    return failureResult(context, CliExitCode::Usage, command,
                         QStringLiteral("Unknown gpo action: %1").arg(action));
}

QJsonArray actionEventsToJson(const ActionHistory &history,
                              const QList<ActionEvent> &events)
{
    QJsonArray result;
    for (const ActionEvent &event : events) {
        bool effective = true;
        if (event.isAction() || event.isCompensation()) {
            QString error;
            effective = history.isEffective(event.id, &error);
        }
        result.append(QJsonObject::fromVariantMap(event.toVariantMap(effective)));
    }
    return result;
}

CliResult runActionHistoryCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("action-history");
    if (context.projectDirectory.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Select a project with --project <folder>."));
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("action-history requires list, record, undo, redo, bookmark, branch, or switch."));
    ActionHistory history(context.projectDirectory);
    const QString action = arguments.takeFirst().toLower();
    QString error;

    if (action == QStringLiteral("list")) {
        QString contextKey;
        QString elementId;
        QString limitText;
        if (!takeOption(&arguments, QStringLiteral("--context"), &contextKey, &error)
            || !takeOption(&arguments, QStringLiteral("--element"), &elementId, &error)
            || !takeOption(&arguments, QStringLiteral("--limit"), &limitText, &error)) {
            return failureResult(context, CliExitCode::Usage, command, error);
        }
        bool limitOk = limitText.isEmpty();
        const int limit = limitText.isEmpty() ? 100 : limitText.toInt(&limitOk);
        if (!limitOk || limit < 1 || !arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("--limit must be positive and no extra list arguments are accepted."));
        QList<ActionEvent> events = contextKey.isEmpty()
            ? history.events(limit, &error)
            : history.recentForElement(contextKey, elementId, limit, &error);
        if (!error.isEmpty())
            return failureResult(context, CliExitCode::Validation, command, error);
        const QJsonArray json = actionEventsToJson(history, events);
        QStringList lines;
        for (const ActionEvent &event : events)
            lines.append(QStringLiteral("#%1 %2 [%3/%4]")
                .arg(event.sequence).arg(event.title, event.contextKey, event.elementId));
        return successResult(context, command, json,
                             lines.isEmpty() ? QStringLiteral("No action events.")
                                             : lines.join(QLatin1Char('\n')));
    }

    if (action == QStringLiteral("record")) {
        QString title;
        QString description;
        QString contextKey;
        QString elementId;
        QString forwardText;
        QString inverseText;
        if (!takeOption(&arguments, QStringLiteral("--title"), &title, &error, true)
            || !takeOption(&arguments, QStringLiteral("--description"), &description, &error)
            || !takeOption(&arguments, QStringLiteral("--context"), &contextKey, &error, true)
            || !takeOption(&arguments, QStringLiteral("--element"), &elementId, &error, true)
            || !takeOption(&arguments, QStringLiteral("--forward"), &forwardText, &error, true)
            || !takeOption(&arguments, QStringLiteral("--inverse"), &inverseText, &error, true)
            || !arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, command,
                                 error.isEmpty() ? QStringLiteral("Unexpected action-history record argument.") : error);
        }
        const QJsonValue forward = parseCliValue(forwardText);
        const QJsonValue inverse = parseCliValue(inverseText);
        if (!forward.isObject() || !inverse.isObject())
            return failureResult(context, CliExitCode::Validation, command,
                                 QStringLiteral("--forward and --inverse must be JSON objects."));
        ActionDraft draft;
        draft.title = title;
        draft.description = description;
        draft.contextKey = contextKey;
        draft.elementId = elementId;
        draft.forwardDiff = forward.toObject();
        draft.inverseDiff = inverse.toObject();
        ActionEvent created;
        if (!history.record(draft, &created, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context, command, created.toJson(),
                             QStringLiteral("Recorded action #%1: %2").arg(created.sequence).arg(created.title));
    }

    if (action == QStringLiteral("undo") || action == QStringLiteral("redo")) {
        if (arguments.size() != 1)
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("action-history %1 requires one event ID.").arg(action));
        ActionEvent created;
        const bool ok = action == QStringLiteral("undo")
            ? history.undoAction(arguments.first(), &created, &error)
            : history.redoAction(arguments.first(), &created, &error);
        if (!ok)
            return failureResult(context, CliExitCode::Conflict, command, error);
        if (created.metadata.value(QStringLiteral("fullProjectState")).toBool(false)
            || created.forwardDiff.contains(QStringLiteral("schema"))) {
            const auto project = ProjectConfig::fromJson(created.forwardDiff,
                                                         context.projectDirectory, &error);
            if (!project)
                return failureResult(context, CliExitCode::Validation, command, error);
            // Undo/redo events are created with bilingual titles by ActionHistory.
            if (!project->save(&error, created.title))
                return failureResult(context, CliExitCode::IoError, command, error);
        }
        return successResult(context, command, created.toJson(), created.title);
    }

    if (action == QStringLiteral("bookmark") || action == QStringLiteral("branch")) {
        if (arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("action-history %1 requires a name.").arg(action));
        const QString name = arguments.takeFirst();
        QString eventId;
        if (!takeOption(&arguments, QStringLiteral("--event"), &eventId, &error)
            || !arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, command,
                                 error.isEmpty() ? QStringLiteral("Unexpected history lane argument.") : error);
        }
        ActionEvent created;
        const bool ok = action == QStringLiteral("bookmark")
            ? history.createBookmark(name, eventId, &created, &error)
            : history.createBranch(name, eventId, &created, &error);
        if (!ok)
            return failureResult(context, CliExitCode::Conflict, command, error);
        return successResult(context, command, created.toJson(), created.title);
    }

    if (action == QStringLiteral("switch")) {
        if (arguments.size() != 1)
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("action-history switch requires one branch name."));
        ActionEvent created;
        if (!history.switchBranch(arguments.first(), &created, &error))
            return failureResult(context, CliExitCode::Conflict, command, error);
        return successResult(context, command, created.toJson(), created.title);
    }

    return failureResult(context, CliExitCode::Usage, command,
                         QStringLiteral("Unknown action-history action: %1").arg(action));
}

CliResult runBundleCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("bundle");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("bundle requires export or import."));
    const QString action = arguments.takeFirst().toLower();
    QString error;
    if (action == QStringLiteral("export")) {
        if (context.projectDirectory.isEmpty() || arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("bundle export needs --project and an output .wimforge file."));
        const QString output = arguments.takeFirst();
        QString notifications = context.notificationDirectory;
        if (!takeOption(&arguments, QStringLiteral("--notifications"), &notifications, &error)
            || !arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, command,
                                 error.isEmpty() ? QStringLiteral("Unexpected bundle export argument.") : error);
        }
        if (notifications.isEmpty())
            notifications = NotificationStore::defaultStoreDirectory();
        const QList<ProjectBundleRepository> repositories{
            {ProjectBundle::ProjectRepositoryRole, context.projectDirectory, QStringLiteral("project")},
            {ProjectBundle::NotificationRepositoryRole, notifications, QStringLiteral("notifications")},
        };
        if (!ProjectBundle::exportToFile(output, repositories, {}, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        const QJsonObject value{{QStringLiteral("path"), QFileInfo(output).absoluteFilePath()},
                                {QStringLiteral("project"), context.projectDirectory},
                                {QStringLiteral("notifications"), notifications}};
        return successResult(context, command, value,
                             QStringLiteral("Exported complete save bundle to %1")
                                 .arg(QFileInfo(output).absoluteFilePath()));
    }
    if (action == QStringLiteral("import")) {
        if (arguments.size() < 2)
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("bundle import requires input and destination paths."));
        const QString input = arguments.takeFirst();
        const QString destination = arguments.takeFirst();
        ProjectBundleImportOptions options;
        options.overwriteExisting = takeFlag(&arguments, QStringLiteral("--overwrite"));
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected bundle import argument: %1").arg(arguments.first()));
        const auto imported = ProjectBundle::importFromFile(input, destination, options, &error);
        if (!imported)
            return failureResult(context, CliExitCode::Validation, command, error);
        QJsonObject paths;
        for (auto iterator = imported->repositoryPaths.cbegin();
             iterator != imported->repositoryPaths.cend(); ++iterator) {
            paths.insert(iterator.key(), iterator.value());
        }
        const QJsonObject value{
            {QStringLiteral("destination"), imported->destinationDirectory},
            {QStringLiteral("formatVersion"), imported->formatVersion},
            {QStringLiteral("manifestSha256"), imported->manifestSha256},
            {QStringLiteral("repositories"), paths},
            {QStringLiteral("retainedBackup"), imported->retainedBackupPath},
        };
        return successResult(context, command, value,
                             QStringLiteral("Imported complete save bundle to %1")
                                 .arg(imported->destinationDirectory));
    }
    return failureResult(context, CliExitCode::Usage, command,
                         QStringLiteral("Unknown bundle action: %1").arg(action));
}

QJsonObject winForgeContractToJson(const WinForgeRuntimeContract &contract)
{
    QJsonObject invocations;
    for (auto iterator = contract.invocations.cbegin(); iterator != contract.invocations.cend(); ++iterator)
        invocations.insert(iterator.key(), stringsToJson(iterator.value()));
    return QJsonObject{
        {QStringLiteral("runtimeFound"), contract.runtimeFound},
        {QStringLiteral("declaredContract"), contract.declaredContract},
        {QStringLiteral("contractVersion"), contract.contractVersion},
        {QStringLiteral("runtimeVersion"), contract.runtimeVersion},
        {QStringLiteral("executable"), contract.executableRelativePath},
        {QStringLiteral("detectionSource"), contract.detectionSource},
        {QStringLiteral("capabilities"), stringsToJson(contract.capabilities)},
        {QStringLiteral("invocations"), invocations},
    };
}

QJsonObject winForgeValidationToJson(const WinForgeBridgeValidation &validation)
{
    return QJsonObject{
        {QStringLiteral("valid"), validation.ok()},
        {QStringLiteral("errors"), stringsToJson(validation.errors)},
        {QStringLiteral("warnings"), stringsToJson(validation.warnings)},
    };
}

std::optional<WinForgeRecipe> projectWinForgeRecipe(const ProjectConfig &project, QString *error)
{
    const QJsonValue value = project.settings.value(QStringLiteral("_winForgeRecipe"));
    if (!value.isObject()) {
        setError(error, QStringLiteral("Project does not contain a WinForge bridge recipe."));
        return std::nullopt;
    }
    return WinForgeBridge::fromJson(value.toObject(), error);
}

CliResult runWinForgeCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("winforge");
    if (arguments.isEmpty())
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("winforge requires detect, template, validate, status, import, export, or stage."));
    const QString action = arguments.takeFirst().toLower();
    QString error;

    if (action == QStringLiteral("detect")) {
        if (arguments.size() != 1)
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("winforge detect requires one runtime folder."));
        const WinForgeRuntimeContract contract = WinForgeBridge::detectRuntimeContract(
            QDir(arguments.first()).absolutePath(), &error);
        if (!contract.runtimeFound)
            return failureResult(context, CliExitCode::NotFound, command, error);
        return successResult(context, command, winForgeContractToJson(contract),
                             QStringLiteral("Detected %1 with capabilities: %2")
                                 .arg(contract.declaredContract ? QStringLiteral("declared WinForge contract")
                                                                : QStringLiteral("legacy WinForge runtime"),
                                      contract.capabilities.join(QStringLiteral(", "))));
    }

    if (action == QStringLiteral("template")) {
        if (arguments.size() < 2 || arguments.takeFirst().toLower() != QStringLiteral("page"))
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("winforge template page TARGET [--output FILE]."));
        const QString target = arguments.takeFirst();
        QString output;
        if (!takeOption(&arguments, QStringLiteral("--output"), &output, &error)
            || !arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, command,
                                 error.isEmpty() ? QStringLiteral("Unexpected winforge template argument.") : error);
        }
        WinForgeRecipe recipe;
        recipe.id = QStringLiteral("winforge-page-recipe");
        recipe.name = QStringLiteral("WinForge page recipe");
        recipe.description = QStringLiteral("Open an approved WinForge page after Windows setup.");
        recipe.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        WinForgeAction page;
        page.id = QStringLiteral("open-page");
        page.idempotencyKey = page.id;
        page.kind = WinForgeActionKind::Page;
        page.phase = WinForgeActionPhase::User;
        page.target = target;
        recipe.actions.append(page);
        const WinForgeBridgeValidation validation = WinForgeBridge::validateRecipe(recipe);
        if (!validation.ok())
            return failureResult(context, CliExitCode::Validation, command,
                                 validation.message(), winForgeValidationToJson(validation));
        if (!output.isEmpty() && !WinForgeBridge::exportJson(recipe, output, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context, command, WinForgeBridge::toJson(recipe),
                             output.isEmpty()
                                 ? QString::fromUtf8(QJsonDocument(WinForgeBridge::toJson(recipe))
                                                         .toJson(QJsonDocument::Indented)).trimmed()
                                 : QStringLiteral("Wrote %1").arg(QFileInfo(output).absoluteFilePath()));
    }

    std::optional<ProjectConfig> project;
    if (!context.projectDirectory.isEmpty()) {
        project = ProjectConfig::load(context.projectDirectory, &error);
        if (!project)
            return failureResult(context, CliExitCode::NotFound, command, error);
    }

    if (action == QStringLiteral("import")) {
        if (!project || arguments.size() != 1)
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("winforge import requires --project FOLDER and one recipe file."));
        const auto recipe = WinForgeBridge::importJson(arguments.first(), &error);
        if (!recipe)
            return failureResult(context, QFileInfo::exists(arguments.first())
                                     ? CliExitCode::Validation : CliExitCode::NotFound,
                                 command, error);
        project->settings.insert(QStringLiteral("_winForgeRecipe"), WinForgeBridge::toJson(*recipe));
        if (!project->save(
                &error,
                QStringLiteral("winforge: import bridge recipe / WinForge：匯入橋接配方")))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context, command, WinForgeBridge::toJson(*recipe),
                             QStringLiteral("Imported and committed WinForge recipe '%1'.").arg(recipe->id));
    }

    if (action == QStringLiteral("export")) {
        QString output;
        if (!project || !takeOption(&arguments, QStringLiteral("--output"), &output, &error, true)
            || !arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, command,
                                 error.isEmpty()
                                     ? QStringLiteral("winforge export requires --project FOLDER --output FILE.")
                                     : error);
        }
        const auto recipe = projectWinForgeRecipe(*project, &error);
        if (!recipe)
            return failureResult(context, CliExitCode::NotFound, command, error);
        if (!WinForgeBridge::exportJson(*recipe, output, &error))
            return failureResult(context, CliExitCode::IoError, command, error);
        return successResult(context, command,
                             QJsonObject{{QStringLiteral("path"), QFileInfo(output).absoluteFilePath()},
                                         {QStringLiteral("recipe"), WinForgeBridge::toJson(*recipe)}},
                             QStringLiteral("Exported WinForge recipe to %1")
                                 .arg(QFileInfo(output).absoluteFilePath()));
    }

    QString runtime;
    if (!takeOption(&arguments, QStringLiteral("--runtime"), &runtime, &error))
        return failureResult(context, CliExitCode::Usage, command, error);
    std::optional<WinForgeRecipe> recipe;
    if (!arguments.isEmpty() && !arguments.first().startsWith(QStringLiteral("--"))) {
        const QString recipePath = arguments.takeFirst();
        recipe = WinForgeBridge::importJson(recipePath, &error);
        if (!recipe)
            return failureResult(context, QFileInfo::exists(recipePath)
                                     ? CliExitCode::Validation : CliExitCode::NotFound,
                                 command, error);
    } else if (project) {
        recipe = projectWinForgeRecipe(*project, &error);
        if (!recipe)
            return failureResult(context, CliExitCode::NotFound, command, error);
    } else {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Supply a recipe file or --project FOLDER."));
    }
    if (runtime.isEmpty() && project)
        runtime = project->settings.value(QStringLiteral("_winForgeRuntimePath")).toString();

    if (action == QStringLiteral("validate") || action == QStringLiteral("status")) {
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected winforge validation argument."));
        WinForgeBridgeValidation validation;
        QJsonObject result{{QStringLiteral("recipe"), WinForgeBridge::toJson(*recipe)}};
        if (runtime.isEmpty()) {
            validation = WinForgeBridge::validateRecipe(*recipe);
        } else {
            const WinForgeRuntimeContract contract = WinForgeBridge::detectRuntimeContract(runtime, &error);
            if (!contract.runtimeFound)
                return failureResult(context, CliExitCode::NotFound, command, error);
            validation = WinForgeBridge::validateAgainstRuntime(*recipe, contract);
            result.insert(QStringLiteral("runtime"), winForgeContractToJson(contract));
        }
        result.insert(QStringLiteral("validation"), winForgeValidationToJson(validation));
        if (!validation.ok())
            return failureResult(context, CliExitCode::Validation, command,
                                 validation.message(), result);
        QStringList lines{QStringLiteral("WinForge recipe is valid.")};
        for (const QString &warning : validation.warnings)
            lines.append(QStringLiteral("Warning: %1").arg(warning));
        return successResult(context, command, result, lines.join(QLatin1Char('\n')));
    }

    if (action == QStringLiteral("stage")) {
        QString iso;
        QString payload;
        if (!takeOption(&arguments, QStringLiteral("--iso"), &iso, &error, true)
            || !takeOption(&arguments, QStringLiteral("--payload"), &payload, &error)) {
            return failureResult(context, CliExitCode::Usage, command, error);
        }
        const bool withoutRuntime = takeFlag(&arguments, QStringLiteral("--without-runtime"));
        const bool forceRuntime = takeFlag(&arguments, QStringLiteral("--include-runtime"));
        const bool overwrite = takeFlag(&arguments, QStringLiteral("--overwrite"));
        if (!arguments.isEmpty())
            return failureResult(context, CliExitCode::Usage, command,
                                 QStringLiteral("Unexpected winforge stage argument: %1").arg(arguments.first()));
        WinForgeStageOptions options;
        options.includeRuntime = project
            ? project->settings.value(QStringLiteral("_winForgeIncludeRuntime")).toBool(true)
            : true;
        if (withoutRuntime) options.includeRuntime = false;
        if (forceRuntime) options.includeRuntime = true;
        options.overwriteExisting = overwrite;
        options.payloadDirectory = payload;
        const auto staged = WinForgeBridge::stageForIso(
            *recipe, runtime, QDir(iso).absolutePath(), options, &error);
        if (!staged)
            return failureResult(context, CliExitCode::Validation, command, error);
        if (project) {
            project->settings.insert(QStringLiteral("_winForgeRecipe"), WinForgeBridge::toJson(*recipe));
            project->settings.insert(QStringLiteral("_winForgeRuntimePath"), runtime);
            project->settings.insert(QStringLiteral("_winForgeIncludeRuntime"), options.includeRuntime);
            QJsonArray current = project->options.extra.value(QStringLiteral("stagedFiles")).toArray();
            QJsonArray updated;
            for (const QJsonValue &value : current) {
                if (!value.isObject()
                    || value.toObject().value(QStringLiteral("role")).toString()
                        != QStringLiteral("winforge-bridge"))
                    updated.append(value);
            }
            updated.append(QJsonObject{
                {QStringLiteral("source"), QDir(iso).filePath(QStringLiteral("sources/$OEM$"))},
                {QStringLiteral("destination"), QStringLiteral("sources/$OEM$")},
                {QStringLiteral("scope"), QStringLiteral("media")},
                {QStringLiteral("role"), QStringLiteral("winforge-bridge")},
            });
            project->options.extra.insert(QStringLiteral("stagedFiles"), updated);
            if (!project->save(
                    &error,
                    QStringLiteral("winforge: stage bridge into ISO plan / WinForge：將橋接內容加入 ISO 計劃")))
                return failureResult(context, CliExitCode::IoError, command, error);
        }
        const QJsonObject result{
            {QStringLiteral("bundleDirectory"), staged->bundleDirectory},
            {QStringLiteral("manifest"), staged->manifestPath},
            {QStringLiteral("manifestSha256"), staged->manifestSha256},
            {QStringLiteral("fileCount"), QString::number(staged->fileCount)},
            {QStringLiteral("totalBytes"), QString::number(staged->totalBytes)},
            {QStringLiteral("runtime"), winForgeContractToJson(staged->runtimeContract)},
        };
        return successResult(context, command, result,
                             QStringLiteral("Staged WinForge bridge in %1").arg(staged->bundleDirectory));
    }

    return failureResult(context, CliExitCode::Usage, command,
                         QStringLiteral("Unknown winforge action: %1").arg(action));
}

QString vmHelpText()
{
    return QStringLiteral(R"HELP(VM Lab (VMware Workstation/Player and VirtualBox)

All mutating commands produce a review preview by default. To perform the
freshly reviewed operation in the same invocation, add both --execute and
--yes. Destructive operations also require --confirm "EXACT TOKEN" from the
preview. Provider commands are launched directly; no shell is used.

State and discovery:
  vm paths [--catalog FILE] [--managed-root FOLDER]
  vm catalog [--catalog FILE] [--managed-root FOLDER]
  vm detect
  vm inventory

Validation runs (requires --project FOLDER):
  vm validation start --iso FILE --image FILE --provider ID --vm ID
            --vm-name NAME --vm-config FILE [--provider-version TEXT]
            [--iso-sha256 HEX] [--image-sha256 HEX]
            [--config-json JSON_OBJECT] [--started-at ISO_8601]
            [--store-revision REV]
  vm validation update RUN_ID --revision N
            [--milestone-phase boot|install --milestone-name NAME
             --milestone-status reached|failed|skipped
             --milestone-note TEXT --milestone-data JSON_OBJECT]
            [--log-message TEXT --log-channel NAME]
            [--evidence-path FILE --evidence-label TEXT
             --evidence-kind screenshot|log|report|other
             --external --external-metadata JSON_OBJECT]
            [--store-revision REV]
  vm validation finish RUN_ID --revision N --status passed|failed|cancelled
            [--note TEXT] [--ended-at ISO_8601] [--store-revision REV]
  vm validation show RUN_ID
  vm validation list|history [--provider ID] [--vm ID]
            [--status running|passed|failed|cancelled] [--text QUERY]
            [--limit N] [--oldest-first]

Create or import:
  vm create --provider ID --name NAME --iso FILE [--directory FOLDER]
            [--guest-type ID] [--firmware bios|efi]
            [--cpu N] [--memory-mib N] [--disk-mib N]
            [--network-mode MODE] [--bridge NAME]
            [--secure-boot|--no-secure-boot] [--tpm|--no-tpm]
            [--hardware-version N] [--unattended-boot]
  vm import --provider ID --config FILE [--name NAME]

Lifecycle and configuration:
  vm lifecycle open-console|start|shutdown|power-off|pause|resume|reset|save-state
               --provider ID --vm ID [--headless]
  vm configure --provider ID --vm ID
               [--cpu N] [--memory-mib N] [--firmware bios|efi]
               [--secure-boot|--no-secure-boot] [--tpm|--no-tpm]
               [--network-mode MODE] [--bridge NAME] [--iso FILE]

Devices (storage topology and network slots are explicit):
  vm device attach-iso --provider ID --vm ID --iso FILE
            --bus ide|sata|scsi|nvme --controller N --port N --device N
            [--controller-name NAME]
  vm device detach-iso --provider ID --vm ID
            --bus ide|sata|scsi|nvme --controller N --port N --device N
            [--controller-name NAME]
  vm device attach-storage|detach-storage --provider ID --vm ID
            --bus ide|sata|scsi|nvme --controller N --port N --device N
            [--controller-name NAME] --optical|--disk [--path FILE]
  vm device attach-network --provider ID --vm ID --slot N
            --network-mode MODE [--interface NAME] --connected|--disconnected
  vm device detach-network --provider ID --vm ID --slot N

Snapshots and removal:
  vm snapshot list --provider ID --vm ID
  vm snapshot take --provider ID --vm ID --name NAME [--description TEXT]
  vm snapshot restore|delete --provider ID --vm ID
              [--snapshot-id ID] [--snapshot-name NAME]
  vm forget|unregister --provider ID --vm ID
  vm delete --provider ID --vm ID

Provider IDs: virtualbox, vmware-workstation, vmware-player. MODE is nat,
bridged, host-only, internal, or disconnected. Imports are always external;
only VMs created inside the managed root can become WimForge-managed.

Without explicit paths, multi-gigabyte VM state is stored in WimForge's local
application-data directory. A selected project uses an isolated path-derived
scope; the global scope is used when no project is selected.)HELP");
}

bool containsOption(const QStringList &arguments, const QString &name)
{
    const QString prefix = name + QLatin1Char('=');
    return std::any_of(arguments.cbegin(), arguments.cend(),
                       [&name, &prefix](const QString &argument) {
        return argument == name || argument.startsWith(prefix);
    });
}

bool takeVmString(QStringList *arguments,
                  const QString &option,
                  const QString &parameter,
                  QJsonObject *parameters,
                  QString *error,
                  bool required = false)
{
    const bool present = containsOption(*arguments, option);
    QString value;
    if (!takeOption(arguments, option, &value, error, required))
        return false;
    if (required && value.trimmed().isEmpty()) {
        setError(error, QStringLiteral("%1 cannot be empty.").arg(option));
        return false;
    }
    if (present)
        parameters->insert(parameter, value);
    return true;
}

bool takeVmInteger(QStringList *arguments,
                   const QString &option,
                   const QString &parameter,
                   int minimum,
                   int maximum,
                   QJsonObject *parameters,
                   QString *error,
                   bool required = false)
{
    const bool present = containsOption(*arguments, option);
    QString value;
    if (!takeOption(arguments, option, &value, error, required))
        return false;
    if (!present)
        return true;
    bool ok = false;
    const qlonglong parsed = value.toLongLong(&ok);
    if (!ok || parsed < minimum || parsed > maximum) {
        setError(error, QStringLiteral("%1 must be an integer from %2 through %3.")
                            .arg(option).arg(minimum).arg(maximum));
        return false;
    }
    parameters->insert(parameter, static_cast<int>(parsed));
    return true;
}

bool takeVmBoolean(QStringList *arguments,
                   const QString &positive,
                   const QString &negative,
                   const QString &parameter,
                   QJsonObject *parameters,
                   QString *error,
                   bool required = false)
{
    const bool enabled = takeFlag(arguments, positive);
    const bool disabled = !negative.isEmpty() && takeFlag(arguments, negative);
    if (enabled && disabled) {
        setError(error, QStringLiteral("%1 and %2 cannot be combined.")
                            .arg(positive, negative));
        return false;
    }
    if (required && !enabled && !disabled) {
        setError(error, QStringLiteral("Supply either %1 or %2.")
                            .arg(positive, negative));
        return false;
    }
    if (enabled || disabled)
        parameters->insert(parameter, enabled);
    return true;
}

QString normalizedVmProvider(QString provider)
{
    provider = provider.trimmed().toLower();
    if (provider == QStringLiteral("vbox"))
        return vmlab::virtualBoxProviderId();
    if (provider == QStringLiteral("vmware")
        || provider == QStringLiteral("workstation")) {
        return vmlab::vmwareWorkstationProviderId();
    }
    if (provider == QStringLiteral("player"))
        return vmlab::vmwarePlayerProviderId();
    return provider;
}

QString resolveVmAction(QStringList *arguments, QString *error)
{
    if (arguments->isEmpty()) {
        setError(error, QStringLiteral("A VM Lab action is required. Run 'WimForge vm help'."));
        return {};
    }

    QString action = arguments->takeFirst().trimmed().toLower();
    if (action == QStringLiteral("providers"))
        action = QStringLiteral("detect");
    if (action == QStringLiteral("import"))
        action = QStringLiteral("register");

    if (action == QStringLiteral("lifecycle")) {
        if (arguments->isEmpty()) {
            setError(error, QStringLiteral("vm lifecycle requires an action."));
            return {};
        }
        action = arguments->takeFirst().trimmed().toLower();
        if (action == QStringLiteral("shutdown"))
            action = QStringLiteral("graceful-shutdown");
        const QSet<QString> allowed{
            QStringLiteral("open-console"), QStringLiteral("start"),
            QStringLiteral("graceful-shutdown"), QStringLiteral("power-off"),
            QStringLiteral("pause"), QStringLiteral("resume"),
            QStringLiteral("reset"), QStringLiteral("save-state"),
        };
        if (!allowed.contains(action)) {
            setError(error, QStringLiteral("Unknown VM lifecycle action '%1'.").arg(action));
            return {};
        }
    } else if (action == QStringLiteral("device") || action == QStringLiteral("devices")) {
        if (arguments->isEmpty()) {
            setError(error, QStringLiteral("vm device requires an action."));
            return {};
        }
        action = arguments->takeFirst().trimmed().toLower();
        const QSet<QString> allowed{
            QStringLiteral("attach-iso"), QStringLiteral("detach-iso"),
            QStringLiteral("attach-storage"), QStringLiteral("detach-storage"),
            QStringLiteral("attach-network"), QStringLiteral("detach-network"),
        };
        if (!allowed.contains(action)) {
            setError(error, QStringLiteral("Unknown VM device action '%1'.").arg(action));
            return {};
        }
    } else if (action == QStringLiteral("snapshot") || action == QStringLiteral("snapshots")) {
        if (arguments->isEmpty()) {
            setError(error, QStringLiteral("vm snapshot requires list, take, restore, or delete."));
            return {};
        }
        const QString verb = arguments->takeFirst().trimmed().toLower();
        if (verb == QStringLiteral("list")) action = QStringLiteral("list-snapshots");
        else if (verb == QStringLiteral("take")) action = QStringLiteral("take-snapshot");
        else if (verb == QStringLiteral("restore")) action = QStringLiteral("restore-snapshot");
        else if (verb == QStringLiteral("delete")) action = QStringLiteral("delete-snapshot");
        else {
            setError(error, QStringLiteral("Unknown VM snapshot action '%1'.").arg(verb));
            return {};
        }
    }

    // ISO actions are exact optical-storage aliases. They deliberately share
    // the same controller/port/device contract as the desktop manager so an
    // imported VM can never fall back to a guessed provider-default slot.
    if (action == QStringLiteral("attach-iso")
        || action == QStringLiteral("detach-iso")) {
        const bool attach = action == QStringLiteral("attach-iso");
        for (QString &argument : *arguments) {
            if (argument == QStringLiteral("--iso"))
                argument = QStringLiteral("--path");
        }
        arguments->append(QStringLiteral("--optical"));
        action = attach ? QStringLiteral("attach-storage")
                        : QStringLiteral("detach-storage");
    }

    if (!vmlab::VmLabCliService::supportedActions().contains(action)) {
        setError(error, QStringLiteral("Unknown VM Lab action '%1'.").arg(action));
        return {};
    }
    setError(error, {});
    return action;
}

void makeVmPathAbsolute(QJsonObject *parameters, const QString &key)
{
    if (!parameters->contains(key))
        return;
    const QString path = parameters->value(key).toString();
    if (!path.isEmpty())
        parameters->insert(key, QFileInfo(path).absoluteFilePath());
}

bool parseVmParameters(const QString &action,
                       QStringList *arguments,
                       QJsonObject *parameters,
                       QString *error)
{
    const QSet<QString> inspectionActions{
        QStringLiteral("paths"), QStringLiteral("catalog"),
        QStringLiteral("detect"), QStringLiteral("inventory"),
    };
    if (inspectionActions.contains(action))
        return true;

    QString provider;
    if (!takeVmString(arguments, QStringLiteral("--provider"),
                      QStringLiteral("providerId"), parameters, error, true)) {
        return false;
    }
    provider = normalizedVmProvider(parameters->value(QStringLiteral("providerId")).toString());
    parameters->insert(QStringLiteral("providerId"), provider);

    if (action == QStringLiteral("create")) {
        if (!takeVmString(arguments, QStringLiteral("--name"), QStringLiteral("name"),
                          parameters, error, true)
            || !takeVmString(arguments, QStringLiteral("--id"), QStringLiteral("id"),
                             parameters, error)
            || !takeVmString(arguments, QStringLiteral("--directory"),
                             QStringLiteral("directory"), parameters, error)
            || !takeVmString(arguments, QStringLiteral("--guest-type"),
                             QStringLiteral("guestType"), parameters, error)
            || !takeVmString(arguments, QStringLiteral("--firmware"),
                             QStringLiteral("firmware"), parameters, error)
            || !takeVmInteger(arguments, QStringLiteral("--hardware-version"),
                              QStringLiteral("virtualHardwareVersion"), 0, 100,
                              parameters, error)
            || !takeVmInteger(arguments, QStringLiteral("--cpu"),
                              QStringLiteral("cpuCount"), 1, 256, parameters, error)
            || !takeVmInteger(arguments, QStringLiteral("--memory-mib"),
                              QStringLiteral("memoryMiB"), 128, 1'048'576,
                              parameters, error)
            || !takeVmInteger(arguments, QStringLiteral("--disk-mib"),
                              QStringLiteral("diskMiB"), 1, 2'097'152,
                              parameters, error)
            || !takeVmString(arguments, QStringLiteral("--network-mode"),
                             QStringLiteral("networkMode"), parameters, error)
            || !takeVmString(arguments, QStringLiteral("--bridge"),
                             QStringLiteral("bridgedInterface"), parameters, error)
            || !takeVmString(arguments, QStringLiteral("--iso"),
                             QStringLiteral("isoPath"), parameters, error, true)
            || !takeVmBoolean(arguments, QStringLiteral("--secure-boot"),
                              QStringLiteral("--no-secure-boot"),
                              QStringLiteral("secureBoot"), parameters, error)
            || !takeVmBoolean(arguments, QStringLiteral("--tpm"),
                              QStringLiteral("--no-tpm"), QStringLiteral("tpm"),
                              parameters, error)
            || !takeVmBoolean(arguments, QStringLiteral("--unattended-boot"),
                              QStringLiteral("--no-unattended-boot"),
                              QStringLiteral("unattendedBoot"), parameters, error)) {
            return false;
        }
        makeVmPathAbsolute(parameters, QStringLiteral("directory"));
        makeVmPathAbsolute(parameters, QStringLiteral("isoPath"));
        return true;
    }

    if (action == QStringLiteral("register")) {
        if (!takeVmString(arguments, QStringLiteral("--config"),
                          QStringLiteral("configPath"), parameters, error, true)
            || !takeVmString(arguments, QStringLiteral("--name"),
                             QStringLiteral("name"), parameters, error)) {
            return false;
        }
        // Importing is deliberately cataloged as external. Managed ownership
        // can only be established by WimForge's guarded create workflow.
        parameters->insert(QStringLiteral("ownership"), QStringLiteral("external"));
        makeVmPathAbsolute(parameters, QStringLiteral("configPath"));
        return true;
    }

    if (!takeVmString(arguments, QStringLiteral("--vm"), QStringLiteral("vmId"),
                      parameters, error, true)) {
        return false;
    }

    if (action == QStringLiteral("start")) {
        return takeVmBoolean(arguments, QStringLiteral("--headless"),
                             QStringLiteral("--gui"), QStringLiteral("headless"),
                             parameters, error);
    }
    if (action == QStringLiteral("configure")) {
        if (!takeVmInteger(arguments, QStringLiteral("--cpu"),
                           QStringLiteral("cpuCount"), 1, 256, parameters, error)
            || !takeVmInteger(arguments, QStringLiteral("--memory-mib"),
                              QStringLiteral("memoryMiB"), 128, 1'048'576,
                              parameters, error)
            || !takeVmString(arguments, QStringLiteral("--firmware"),
                             QStringLiteral("firmware"), parameters, error)
            || !takeVmBoolean(arguments, QStringLiteral("--secure-boot"),
                              QStringLiteral("--no-secure-boot"),
                              QStringLiteral("secureBoot"), parameters, error)
            || !takeVmBoolean(arguments, QStringLiteral("--tpm"),
                              QStringLiteral("--no-tpm"), QStringLiteral("tpm"),
                              parameters, error)
            || !takeVmString(arguments, QStringLiteral("--network-mode"),
                             QStringLiteral("networkMode"), parameters, error)
            || !takeVmString(arguments, QStringLiteral("--bridge"),
                             QStringLiteral("bridgedInterface"), parameters, error)
            || !takeVmString(arguments, QStringLiteral("--iso"),
                             QStringLiteral("isoPath"), parameters, error)) {
            return false;
        }
        makeVmPathAbsolute(parameters, QStringLiteral("isoPath"));
        return true;
    }
    if (action == QStringLiteral("attach-iso")) {
        if (!takeVmString(arguments, QStringLiteral("--iso"),
                          QStringLiteral("isoPath"), parameters, error, true)) {
            return false;
        }
        makeVmPathAbsolute(parameters, QStringLiteral("isoPath"));
        return true;
    }
    if (action == QStringLiteral("attach-storage")
        || action == QStringLiteral("detach-storage")) {
        const bool attaching = action == QStringLiteral("attach-storage");
        if (!takeVmString(arguments, QStringLiteral("--bus"), QStringLiteral("bus"),
                          parameters, error, true)
            || !takeVmInteger(arguments, QStringLiteral("--controller"),
                              QStringLiteral("controller"), 0, 3, parameters, error, true)
            || !takeVmInteger(arguments, QStringLiteral("--port"),
                              QStringLiteral("port"), 0, 29, parameters, error, true)
            || !takeVmInteger(arguments, QStringLiteral("--device"),
                              QStringLiteral("device"), 0, 1, parameters, error, true)
            || !takeVmString(arguments, QStringLiteral("--controller-name"),
                             QStringLiteral("controllerName"), parameters, error)
            || !takeVmBoolean(arguments, QStringLiteral("--optical"),
                              QStringLiteral("--disk"), QStringLiteral("optical"),
                              parameters, error, true)
            || (attaching
                && !takeVmString(arguments, QStringLiteral("--path"),
                                 QStringLiteral("path"), parameters, error, true))) {
            return false;
        }
        makeVmPathAbsolute(parameters, QStringLiteral("path"));
        return true;
    }
    if (action == QStringLiteral("attach-network")) {
        if (!takeVmInteger(arguments, QStringLiteral("--slot"),
                           QStringLiteral("slot"), 1, 10, parameters, error, true)
            || !takeVmString(arguments, QStringLiteral("--network-mode"),
                             QStringLiteral("networkMode"), parameters, error, true)
            || !takeVmString(arguments, QStringLiteral("--interface"),
                             QStringLiteral("interfaceName"), parameters, error)
            || !takeVmBoolean(arguments, QStringLiteral("--connected"),
                              QStringLiteral("--disconnected"), QStringLiteral("connected"),
                              parameters, error, true)) {
            return false;
        }
        return true;
    }
    if (action == QStringLiteral("detach-network")) {
        return takeVmInteger(arguments, QStringLiteral("--slot"),
                             QStringLiteral("slot"), 1, 10, parameters, error, true);
    }
    if (action == QStringLiteral("take-snapshot")) {
        return takeVmString(arguments, QStringLiteral("--name"), QStringLiteral("name"),
                            parameters, error, true)
            && takeVmString(arguments, QStringLiteral("--description"),
                            QStringLiteral("description"), parameters, error);
    }
    if (action == QStringLiteral("restore-snapshot")
        || action == QStringLiteral("delete-snapshot")) {
        if (!takeVmString(arguments, QStringLiteral("--snapshot-id"),
                          QStringLiteral("snapshotId"), parameters, error)
            || !takeVmString(arguments, QStringLiteral("--snapshot-name"),
                             QStringLiteral("snapshotName"), parameters, error)) {
            return false;
        }
        if (!parameters->contains(QStringLiteral("snapshotId"))
            && !parameters->contains(QStringLiteral("snapshotName"))) {
            setError(error, QStringLiteral(
                "Supply --snapshot-id or --snapshot-name from a fresh 'vm snapshot list'."));
            return false;
        }
    }
    return true;
}

CliExitCode vmExitCode(int code)
{
    switch (code) {
    case vmlab::VmLabCliResult::Ok: return CliExitCode::Success;
    case vmlab::VmLabCliResult::InvalidRequest: return CliExitCode::Validation;
    case vmlab::VmLabCliResult::ConfirmationRequired:
        return CliExitCode::ConfirmationRequired;
    case vmlab::VmLabCliResult::ProviderFailure:
        return CliExitCode::ExternalProcessFailed;
    case vmlab::VmLabCliResult::CatalogFailure: return CliExitCode::IoError;
    default: return CliExitCode::InternalError;
    }
}

QString vmScopeRoot(const CommandContext &context, QString *error)
{
    return vmlab::resolveVmLabScope(context.projectDirectory, error).root;
}

QString vmValidationHelpText()
{
    return QStringLiteral(R"HELP(VM validation run tracking

Every command requires --project FOLDER so validation evidence stays attached
to the same project identity used by VM Lab. Commands never prompt.

  vm validation start --iso FILE --image FILE --provider ID --vm ID
            --vm-name NAME --vm-config FILE [--provider-version TEXT]
            [--iso-sha256 HEX] [--image-sha256 HEX]
            [--config-json JSON_OBJECT] [--started-at ISO_8601]
            [--store-revision REV]
  vm validation update RUN_ID --revision N
            [--milestone-phase boot|install --milestone-name NAME
             --milestone-status reached|failed|skipped
             --milestone-note TEXT --milestone-data JSON_OBJECT
             --milestone-at ISO_8601]
            [--log-message TEXT --log-channel NAME --log-at ISO_8601]
            [--evidence-path FILE --evidence-label TEXT
             --evidence-kind screenshot|log|report|other
             --evidence-at ISO_8601
             --external --external-metadata JSON_OBJECT]
            [--store-revision REV]
  vm validation finish RUN_ID --revision N --status passed|failed|cancelled
            [--note TEXT] [--ended-at ISO_8601] [--store-revision REV]
  vm validation show RUN_ID
  vm validation list|history [--provider ID] [--vm ID]
            [--status running|passed|failed|cancelled] [--text QUERY]
            [--started-after ISO_8601] [--started-before ISO_8601]
            [--limit N] [--oldest-first]

Run revisions are mandatory for update and finish. --store-revision adds a
second compare-and-swap guard. To pass, --config-json must select one of the
installation, first-boot, upgrade, customization, or full-smoke profiles; every
exact milestone required by that profile must be reached, no milestone may
have failed, and hashed evidence is required.)HELP");
}

std::optional<QDateTime> parseVmValidationTimestamp(const QString &text,
                                                     const QString &option,
                                                     QString *error)
{
    if (text.trimmed().isEmpty())
        return QDateTime{};
    QDateTime value = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!value.isValid())
        value = QDateTime::fromString(text, Qt::ISODate);
    if (!value.isValid()) {
        setError(error, QStringLiteral("%1 must be an ISO-8601 timestamp.").arg(option));
        return std::nullopt;
    }
    return value.toUTC();
}

std::optional<QJsonObject> parseVmValidationObject(const QString &text,
                                                   const QString &option,
                                                   QString *error)
{
    if (text.trimmed().isEmpty())
        return QJsonObject{};
    const QJsonValue value = parseCliValue(text);
    if (!value.isObject()) {
        setError(error, QStringLiteral("%1 must be a JSON object.").arg(option));
        return std::nullopt;
    }
    return value.toObject();
}

std::optional<vmvalidation::RunStatus> vmValidationRunStatus(const QString &text,
                                                             bool completionOnly)
{
    const QString value = text.trimmed().toLower();
    if (!completionOnly && value == QStringLiteral("running"))
        return vmvalidation::RunStatus::Running;
    if (value == QStringLiteral("passed"))
        return vmvalidation::RunStatus::Passed;
    if (value == QStringLiteral("failed"))
        return vmvalidation::RunStatus::Failed;
    if (value == QStringLiteral("cancelled") || value == QStringLiteral("canceled"))
        return vmvalidation::RunStatus::Cancelled;
    return std::nullopt;
}

std::optional<vmvalidation::MilestonePhase> vmValidationMilestonePhase(const QString &text)
{
    const QString value = text.trimmed().toLower();
    if (value == QStringLiteral("boot"))
        return vmvalidation::MilestonePhase::Boot;
    if (value == QStringLiteral("install") || value == QStringLiteral("installation"))
        return vmvalidation::MilestonePhase::Install;
    return std::nullopt;
}

std::optional<vmvalidation::MilestoneStatus> vmValidationMilestoneStatus(const QString &text)
{
    const QString value = text.trimmed().toLower();
    if (value == QStringLiteral("reached"))
        return vmvalidation::MilestoneStatus::Reached;
    if (value == QStringLiteral("failed"))
        return vmvalidation::MilestoneStatus::Failed;
    if (value == QStringLiteral("skipped"))
        return vmvalidation::MilestoneStatus::Skipped;
    return std::nullopt;
}

std::optional<vmvalidation::EvidenceKind> vmValidationEvidenceKind(const QString &text)
{
    const QString value = text.trimmed().toLower();
    if (value == QStringLiteral("screenshot"))
        return vmvalidation::EvidenceKind::Screenshot;
    if (value == QStringLiteral("log"))
        return vmvalidation::EvidenceKind::Log;
    if (value == QStringLiteral("report"))
        return vmvalidation::EvidenceKind::Report;
    if (value == QStringLiteral("other"))
        return vmvalidation::EvidenceKind::Other;
    return std::nullopt;
}

CliExitCode vmValidationExitCode(const QString &error)
{
    if (error.contains(QStringLiteral("not found"), Qt::CaseInsensitive))
        return CliExitCode::NotFound;
    if (error.contains(QStringLiteral("changed"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("busy"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("revision"), Qt::CaseInsensitive)) {
        return CliExitCode::Conflict;
    }
    if (error.contains(QStringLiteral("could not create"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("could not open"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("could not write"), Qt::CaseInsensitive)) {
        return CliExitCode::IoError;
    }
    return CliExitCode::Validation;
}

QJsonObject vmValidationMutationJson(const vmvalidation::MutationResult &mutation)
{
    return QJsonObject{
        {QStringLiteral("run"), mutation.run.toJson()},
        {QStringLiteral("schema"), QStringLiteral("wimforge.vm-validation-cli")},
        {QStringLiteral("storeRevision"), mutation.storeRevision},
    };
}

QString vmValidationMutationText(const vmvalidation::MutationResult &mutation)
{
    return QStringLiteral("Validation run %1 is %2 at revision %3 (store %4).")
        .arg(mutation.run.id, vmvalidation::runStatusName(mutation.run.status))
        .arg(mutation.run.revision)
        .arg(mutation.storeRevision);
}

CliResult runVmValidationCommand(const CommandContext &context, QStringList arguments)
{
    const QString command = QStringLiteral("vm validation");
    if (!arguments.isEmpty()
        && (arguments.first().compare(QStringLiteral("help"), Qt::CaseInsensitive) == 0
            || arguments.first() == QStringLiteral("--help")
            || arguments.first() == QStringLiteral("-h"))) {
        return successResult(context, command,
                             QJsonObject{{QStringLiteral("text"), vmValidationHelpText()}},
                             vmValidationHelpText());
    }
    if (context.projectDirectory.trimmed().isEmpty()) {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("VM validation requires --project <folder>."));
    }
    if (arguments.isEmpty()) {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("A validation action is required. Run 'WimForge vm validation help'."));
    }

    QString error;
    const vmlab::VmLabScope scope = vmlab::resolveVmLabScope(context.projectDirectory, &error);
    if (scope.root.isEmpty())
        return failureResult(context, CliExitCode::IoError, command, error);
    const QFileInfo projectInfo(context.projectDirectory);
    const QString projectRoot = projectInfo.canonicalFilePath();
    if (projectRoot.isEmpty()) {
        return failureResult(context, CliExitCode::NotFound, command,
                             QStringLiteral("The selected project folder does not exist."));
    }

    vmvalidation::VmValidationStore store(projectRoot);
    const QString action = arguments.takeFirst().trimmed().toLower();
    const QString routedCommand = command + QLatin1Char(' ') + action;

    if (action == QStringLiteral("start")) {
        QString isoPath;
        QString isoSha256;
        QString imagePath;
        QString imageSha256;
        QString providerId;
        QString providerVersion;
        QString vmId;
        QString vmName;
        QString vmConfigPath;
        QString configText;
        QString startedText;
        QString expectedStoreRevision;
        if (!takeOption(&arguments, QStringLiteral("--iso"), &isoPath, &error, true)
            || !takeOption(&arguments, QStringLiteral("--iso-sha256"), &isoSha256, &error)
            || !takeOption(&arguments, QStringLiteral("--image"), &imagePath, &error, true)
            || !takeOption(&arguments, QStringLiteral("--image-sha256"), &imageSha256, &error)
            || !takeOption(&arguments, QStringLiteral("--provider"), &providerId, &error, true)
            || !takeOption(&arguments, QStringLiteral("--provider-version"), &providerVersion, &error)
            || !takeOption(&arguments, QStringLiteral("--vm"), &vmId, &error, true)
            || !takeOption(&arguments, QStringLiteral("--vm-name"), &vmName, &error, true)
            || !takeOption(&arguments, QStringLiteral("--vm-config"), &vmConfigPath, &error, true)
            || !takeOption(&arguments, QStringLiteral("--config-json"), &configText, &error)
            || !takeOption(&arguments, QStringLiteral("--started-at"), &startedText, &error)
            || !takeOption(&arguments, QStringLiteral("--store-revision"),
                           &expectedStoreRevision, &error)) {
            return failureResult(context, CliExitCode::Usage, routedCommand, error);
        }
        if (!arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("Unexpected validation start argument: %1")
                                     .arg(arguments.first()));
        }
        const auto config = parseVmValidationObject(configText, QStringLiteral("--config-json"), &error);
        const auto startedAt = parseVmValidationTimestamp(
            startedText, QStringLiteral("--started-at"), &error);
        if (!config || !startedAt)
            return failureResult(context, CliExitCode::Usage, routedCommand, error);

        vmvalidation::RunStart start;
        start.isoPath = isoPath;
        start.isoSha256 = isoSha256;
        start.imagePath = imagePath;
        start.imageSha256 = imageSha256;
        start.providerId = normalizedVmProvider(providerId);
        start.providerVersion = providerVersion;
        start.vmId = vmId;
        start.vmName = vmName;
        start.vmConfigPath = vmConfigPath;
        start.configSnapshot = *config;
        start.startedAt = *startedAt;
        vmvalidation::MutationResult mutation;
        if (!store.appendRun(start, &mutation, &error, expectedStoreRevision)) {
            return failureResult(context, vmValidationExitCode(error), routedCommand, error);
        }
        return successResult(context, routedCommand, vmValidationMutationJson(mutation),
                             vmValidationMutationText(mutation));
    }

    if (action == QStringLiteral("update")) {
        if (arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("validation update requires RUN_ID."));
        }
        const QString runId = arguments.takeFirst();
        QString revisionText;
        QString expectedStoreRevision;
        QString milestonePhaseText;
        QString milestoneStatusText;
        QString milestoneName;
        QString milestoneNote;
        QString milestoneDataText;
        QString milestoneAtText;
        QString logMessage;
        QString logChannel;
        QString logAtText;
        QString evidencePath;
        QString evidenceLabel;
        QString evidenceKindText;
        QString evidenceAtText;
        QString externalMetadataText;
        const bool external = takeFlag(&arguments, QStringLiteral("--external"));
        if (!takeOption(&arguments, QStringLiteral("--revision"), &revisionText, &error, true)
            || !takeOption(&arguments, QStringLiteral("--store-revision"),
                           &expectedStoreRevision, &error)
            || !takeOption(&arguments, QStringLiteral("--milestone-phase"),
                           &milestonePhaseText, &error)
            || !takeOption(&arguments, QStringLiteral("--milestone-status"),
                           &milestoneStatusText, &error)
            || !takeOption(&arguments, QStringLiteral("--milestone-name"),
                           &milestoneName, &error)
            || !takeOption(&arguments, QStringLiteral("--milestone-note"),
                           &milestoneNote, &error)
            || !takeOption(&arguments, QStringLiteral("--milestone-data"),
                           &milestoneDataText, &error)
            || !takeOption(&arguments, QStringLiteral("--milestone-at"),
                           &milestoneAtText, &error)
            || !takeOption(&arguments, QStringLiteral("--log-message"), &logMessage, &error)
            || !takeOption(&arguments, QStringLiteral("--log-channel"), &logChannel, &error)
            || !takeOption(&arguments, QStringLiteral("--log-at"), &logAtText, &error)
            || !takeOption(&arguments, QStringLiteral("--evidence-path"), &evidencePath, &error)
            || !takeOption(&arguments, QStringLiteral("--evidence-label"), &evidenceLabel, &error)
            || !takeOption(&arguments, QStringLiteral("--evidence-kind"), &evidenceKindText, &error)
            || !takeOption(&arguments, QStringLiteral("--evidence-at"), &evidenceAtText, &error)
            || !takeOption(&arguments, QStringLiteral("--external-metadata"),
                           &externalMetadataText, &error)) {
            return failureResult(context, CliExitCode::Usage, routedCommand, error);
        }
        if (!arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("Unexpected validation update argument: %1")
                                     .arg(arguments.first()));
        }
        const auto revision = positiveInteger(revisionText, 1,
                                              std::numeric_limits<int>::max(), &error);
        if (!revision)
            return failureResult(context, CliExitCode::Usage, routedCommand, error);

        vmvalidation::RunUpdate update;
        const bool hasMilestone = !milestonePhaseText.isEmpty() || !milestoneStatusText.isEmpty()
            || !milestoneName.isEmpty() || !milestoneNote.isEmpty()
            || !milestoneDataText.isEmpty() || !milestoneAtText.isEmpty();
        if (hasMilestone) {
            const auto phase = vmValidationMilestonePhase(milestonePhaseText);
            const QString statusText = milestoneStatusText.isEmpty()
                ? QStringLiteral("reached") : milestoneStatusText;
            const auto status = vmValidationMilestoneStatus(statusText);
            const auto data = parseVmValidationObject(
                milestoneDataText, QStringLiteral("--milestone-data"), &error);
            const auto occurredAt = parseVmValidationTimestamp(
                milestoneAtText, QStringLiteral("--milestone-at"), &error);
            if (!phase || !status || milestoneName.trimmed().isEmpty() || !data || !occurredAt) {
                if (error.isEmpty()) {
                    error = QStringLiteral("A milestone requires --milestone-phase boot|install, "
                                           "--milestone-name, and a valid milestone status.");
                }
                return failureResult(context, CliExitCode::Usage, routedCommand, error);
            }
            vmvalidation::MilestoneDraft draft;
            draft.phase = *phase;
            draft.name = milestoneName;
            draft.status = *status;
            draft.occurredAt = *occurredAt;
            draft.note = milestoneNote;
            draft.data = *data;
            update.milestones.append(std::move(draft));
        }

        const bool hasLog = !logMessage.isEmpty() || !logChannel.isEmpty() || !logAtText.isEmpty();
        if (hasLog) {
            const auto occurredAt = parseVmValidationTimestamp(
                logAtText, QStringLiteral("--log-at"), &error);
            if (logMessage.isEmpty() || !occurredAt) {
                if (error.isEmpty())
                    error = QStringLiteral("A log update requires --log-message.");
                return failureResult(context, CliExitCode::Usage, routedCommand, error);
            }
            vmvalidation::LogDraft draft;
            draft.occurredAt = *occurredAt;
            draft.channel = logChannel.isEmpty() ? QStringLiteral("vm") : logChannel;
            draft.message = logMessage;
            update.logs.append(std::move(draft));
        }

        const bool hasEvidence = !evidencePath.isEmpty() || !evidenceLabel.isEmpty()
            || !evidenceKindText.isEmpty() || !evidenceAtText.isEmpty()
            || external || !externalMetadataText.isEmpty();
        if (hasEvidence) {
            const QString kindText = evidenceKindText.isEmpty()
                ? QStringLiteral("other") : evidenceKindText;
            const auto kind = vmValidationEvidenceKind(kindText);
            const auto capturedAt = parseVmValidationTimestamp(
                evidenceAtText, QStringLiteral("--evidence-at"), &error);
            const auto metadata = parseVmValidationObject(
                externalMetadataText, QStringLiteral("--external-metadata"), &error);
            if (!kind || evidencePath.trimmed().isEmpty()
                || evidenceLabel.trimmed().isEmpty() || !capturedAt || !metadata
                || (external && metadata->isEmpty())
                || (!external && !metadata->isEmpty())) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "Evidence requires --evidence-path, --evidence-label, and a valid kind. "
                        "Outside-project evidence also requires --external and non-empty "
                        "--external-metadata; metadata is invalid without --external.");
                }
                return failureResult(context, CliExitCode::Usage, routedCommand, error);
            }
            vmvalidation::EvidenceDraft draft;
            draft.kind = *kind;
            draft.label = evidenceLabel;
            draft.path = evidencePath;
            draft.capturedAt = *capturedAt;
            draft.external = external;
            draft.externalMetadata = *metadata;
            update.evidence.append(std::move(draft));
        }

        if (update.milestones.isEmpty() && update.logs.isEmpty() && update.evidence.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("validation update requires a milestone, log, or evidence group."));
        }
        vmvalidation::MutationResult mutation;
        if (!store.updateRun(runId, *revision, update, &mutation, &error,
                             expectedStoreRevision)) {
            return failureResult(context, vmValidationExitCode(error), routedCommand, error);
        }
        return successResult(context, routedCommand, vmValidationMutationJson(mutation),
                             vmValidationMutationText(mutation));
    }

    if (action == QStringLiteral("finish") || action == QStringLiteral("complete")) {
        if (arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("validation finish requires RUN_ID."));
        }
        const QString runId = arguments.takeFirst();
        QString revisionText;
        QString statusText;
        QString note;
        QString endedAtText;
        QString expectedStoreRevision;
        if (!takeOption(&arguments, QStringLiteral("--revision"), &revisionText, &error, true)
            || !takeOption(&arguments, QStringLiteral("--status"), &statusText, &error, true)
            || !takeOption(&arguments, QStringLiteral("--note"), &note, &error)
            || !takeOption(&arguments, QStringLiteral("--ended-at"), &endedAtText, &error)
            || !takeOption(&arguments, QStringLiteral("--store-revision"),
                           &expectedStoreRevision, &error)) {
            return failureResult(context, CliExitCode::Usage, routedCommand, error);
        }
        if (!arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("Unexpected validation finish argument: %1")
                                     .arg(arguments.first()));
        }
        const auto revision = positiveInteger(revisionText, 1,
                                              std::numeric_limits<int>::max(), &error);
        const auto status = vmValidationRunStatus(statusText, true);
        const auto endedAt = parseVmValidationTimestamp(
            endedAtText, QStringLiteral("--ended-at"), &error);
        if (!revision || !status || !endedAt) {
            if (error.isEmpty())
                error = QStringLiteral("--status must be passed, failed, or cancelled.");
            return failureResult(context, CliExitCode::Usage, routedCommand, error);
        }
        vmvalidation::MutationResult mutation;
        if (!store.completeRun(runId, *revision, *status, note, *endedAt,
                               &mutation, &error, expectedStoreRevision)) {
            return failureResult(context, vmValidationExitCode(error), routedCommand, error);
        }
        return successResult(context, routedCommand, vmValidationMutationJson(mutation),
                             vmValidationMutationText(mutation));
    }

    if (action == QStringLiteral("show")) {
        if (arguments.size() != 1) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("validation show requires exactly one RUN_ID."));
        }
        vmvalidation::StoreSnapshot snapshot;
        if (!store.load(&snapshot, &error))
            return failureResult(context, vmValidationExitCode(error), routedCommand, error);
        const QString runId = arguments.first();
        const auto found = std::find_if(snapshot.runs.cbegin(), snapshot.runs.cend(),
                                        [&runId](const vmvalidation::ValidationRun &run) {
                                            return run.id == runId;
                                        });
        if (found == snapshot.runs.cend()) {
            return failureResult(context, CliExitCode::NotFound, routedCommand,
                                 QStringLiteral("VM validation run '%1' was not found.").arg(runId));
        }
        const QJsonObject result{
            {QStringLiteral("run"), found->toJson()},
            {QStringLiteral("schema"), QStringLiteral("wimforge.vm-validation-cli")},
            {QStringLiteral("storeRevision"), snapshot.revision},
        };
        return successResult(context, routedCommand, result,
                             QString::fromUtf8(QJsonDocument(found->toJson())
                                                   .toJson(QJsonDocument::Indented)).trimmed());
    }

    if (action == QStringLiteral("list") || action == QStringLiteral("history")) {
        QString providerId;
        QString vmId;
        QString statusText;
        QString text;
        QString startedAfterText;
        QString startedBeforeText;
        QString limitText;
        const bool oldestFirst = takeFlag(&arguments, QStringLiteral("--oldest-first"));
        if (!takeOption(&arguments, QStringLiteral("--provider"), &providerId, &error)
            || !takeOption(&arguments, QStringLiteral("--vm"), &vmId, &error)
            || !takeOption(&arguments, QStringLiteral("--status"), &statusText, &error)
            || !takeOption(&arguments, QStringLiteral("--text"), &text, &error)
            || !takeOption(&arguments, QStringLiteral("--started-after"),
                           &startedAfterText, &error)
            || !takeOption(&arguments, QStringLiteral("--started-before"),
                           &startedBeforeText, &error)
            || !takeOption(&arguments, QStringLiteral("--limit"), &limitText, &error)) {
            return failureResult(context, CliExitCode::Usage, routedCommand, error);
        }
        if (!arguments.isEmpty()) {
            return failureResult(context, CliExitCode::Usage, routedCommand,
                                 QStringLiteral("Unexpected validation history argument: %1")
                                     .arg(arguments.first()));
        }
        const auto limit = positiveInteger(limitText.isEmpty() ? QStringLiteral("100") : limitText,
                                           1, vmvalidation::VmValidationStore::MaxRuns, &error);
        const auto startedAfter = parseVmValidationTimestamp(
            startedAfterText, QStringLiteral("--started-after"), &error);
        const auto startedBefore = parseVmValidationTimestamp(
            startedBeforeText, QStringLiteral("--started-before"), &error);
        std::optional<vmvalidation::RunStatus> status;
        if (!statusText.isEmpty())
            status = vmValidationRunStatus(statusText, false);
        if (!limit || !startedAfter || !startedBefore
            || (!statusText.isEmpty() && !status)) {
            if (error.isEmpty())
                error = QStringLiteral("--status must be running, passed, failed, or cancelled.");
            return failureResult(context, CliExitCode::Usage, routedCommand, error);
        }
        vmvalidation::StoreSnapshot snapshot;
        if (!store.load(&snapshot, &error))
            return failureResult(context, vmValidationExitCode(error), routedCommand, error);
        QList<vmvalidation::ValidationRun> runs;
        const QString normalizedProvider = providerId.isEmpty()
            ? QString() : normalizedVmProvider(providerId);
        for (const vmvalidation::ValidationRun &run : snapshot.runs) {
            if (!normalizedProvider.isEmpty() && run.vm.providerId != normalizedProvider)
                continue;
            if (!vmId.isEmpty() && run.vm.vmId != vmId)
                continue;
            if (status && run.status != *status)
                continue;
            if (startedAfter->isValid() && run.startedAt < *startedAfter)
                continue;
            if (startedBefore->isValid() && run.startedAt >= *startedBefore)
                continue;
            if (!text.trimmed().isEmpty()) {
                const QString haystack = QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6")
                    .arg(run.id, run.vm.providerId, run.vm.providerVersion,
                         run.vm.vmId, run.vm.vmName, run.completionNote);
                if (!haystack.contains(text.trimmed(), Qt::CaseInsensitive))
                    continue;
            }
            runs.append(run);
        }
        if (!oldestFirst)
            std::reverse(runs.begin(), runs.end());
        if (runs.size() > *limit)
            runs.resize(*limit);
        QJsonArray array;
        for (const vmvalidation::ValidationRun &run : runs)
            array.append(run.toJson());
        const QJsonObject result{
            {QStringLiteral("count"), array.size()},
            {QStringLiteral("runs"), array},
            {QStringLiteral("schema"), QStringLiteral("wimforge.vm-validation-cli")},
            {QStringLiteral("storeRevision"), snapshot.revision},
        };
        return successResult(context, routedCommand, result,
                             QString::fromUtf8(QJsonDocument(array)
                                                   .toJson(QJsonDocument::Indented)).trimmed());
    }

    return failureResult(context, CliExitCode::Usage, routedCommand,
                         QStringLiteral("Unknown VM validation action '%1'. Run "
                                        "'WimForge vm validation help'.").arg(action));
}

CliResult runVmCommand(const CommandContext &context,
                       QStringList arguments,
                       const CliDependencies &dependencies)
{
    const QString command = QStringLiteral("vm");
    if (!arguments.isEmpty()
        && (arguments.first().compare(QStringLiteral("help"), Qt::CaseInsensitive) == 0
            || arguments.first() == QStringLiteral("--help")
            || arguments.first() == QStringLiteral("-h"))) {
        return successResult(context, command,
                             QJsonObject{{QStringLiteral("text"), vmHelpText()}},
                             vmHelpText());
    }

    if (!arguments.isEmpty()
        && (arguments.first().compare(QStringLiteral("validation"), Qt::CaseInsensitive) == 0
            || arguments.first().compare(QStringLiteral("validate"), Qt::CaseInsensitive) == 0)) {
        arguments.removeFirst();
        return runVmValidationCommand(context, arguments);
    }

    QString error;
    QString catalogPath;
    QString managedRoot;
    if (!takeOption(&arguments, QStringLiteral("--catalog"), &catalogPath, &error)
        || !takeOption(&arguments, QStringLiteral("--managed-root"), &managedRoot, &error)) {
        return failureResult(context, CliExitCode::Usage, command, error);
    }

    const bool execute = takeFlag(&arguments, QStringLiteral("--execute"));
    const bool explicitPreview = takeFlag(&arguments, QStringLiteral("--preview"));
    const bool yes = takeFlag(&arguments, QStringLiteral("--yes"));
    const bool confirmationSupplied = containsOption(arguments, QStringLiteral("--confirm"));
    QString confirmation;
    if (!takeOption(&arguments, QStringLiteral("--confirm"), &confirmation, &error))
        return failureResult(context, CliExitCode::Usage, command, error);
    if (execute && explicitPreview) {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("--preview and --execute cannot be combined."));
    }
    if (!execute && yes) {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("--yes is only valid with --execute."));
    }
    if (!execute && confirmationSupplied) {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("--confirm is only valid with --execute."));
    }

    const QString action = resolveVmAction(&arguments, &error);
    if (action.isEmpty())
        return failureResult(context, CliExitCode::Usage, command, error);

    const QSet<QString> inspectionActions{
        QStringLiteral("paths"), QStringLiteral("catalog"),
        QStringLiteral("detect"), QStringLiteral("inventory"),
    };
    if (inspectionActions.contains(action)
        && (execute || yes || !confirmation.isEmpty())) {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Read-only VM inspection does not accept execution options."));
    }

    QJsonObject parameters;
    if (!parseVmParameters(action, &arguments, &parameters, &error))
        return failureResult(context, CliExitCode::Usage, command, error);
    if (!arguments.isEmpty()) {
        return failureResult(context, CliExitCode::Usage, command,
                             QStringLiteral("Unexpected VM Lab argument: %1")
                                 .arg(arguments.first()));
    }

    const QString stateRoot = vmScopeRoot(context, &error);
    if (stateRoot.isEmpty())
        return failureResult(context, CliExitCode::IoError, command, error);
    if (catalogPath.trimmed().isEmpty())
        catalogPath = QDir(stateRoot).filePath(QStringLiteral("catalog.json"));
    else
        catalogPath = QFileInfo(catalogPath).absoluteFilePath();
    if (managedRoot.trimmed().isEmpty())
        managedRoot = QDir(stateRoot).filePath(QStringLiteral("machines"));
    else
        managedRoot = QFileInfo(managedRoot).absoluteFilePath();

    vmlab::VmLabCliRequest request;
    request.action = action;
    request.parameters = parameters;
    request.execute = execute;
    request.yes = yes;
    request.typedConfirmation = confirmation;

    vmlab::ProcessCommandRunner nativeRunner;
    vmlab::CommandRunner &processRunner = dependencies.vmCommandRunner
        ? *dependencies.vmCommandRunner : static_cast<vmlab::CommandRunner &>(nativeRunner);
    vmlab::VmLabCliService service(catalogPath, managedRoot,
                                   dependencies.vmProviderAdapter);
    const vmlab::VmLabCliResult vmResult = service.handle(request, processRunner);
    const QString routedCommand = command + QLatin1Char(' ') + action;
    if (vmResult.success) {
        const QString human = QString::fromUtf8(
            QJsonDocument(vmResult.output).toJson(QJsonDocument::Indented)).trimmed();
        return successResult(context, routedCommand, vmResult.output, human);
    }

    QString message = vmResult.error;
    const QString token = vmResult.output.value(QStringLiteral("preview"))
                              .toObject().value(QStringLiteral("confirmation"))
                              .toObject().value(QStringLiteral("typedToken")).toString();
    if (!token.isEmpty()) {
        message += QStringLiteral("\nExact confirmation token: %1").arg(token);
    }
    return failureResult(context, vmExitCode(vmResult.exitCode), routedCommand,
                         message, vmResult.output);
}

} // namespace

CliRunner::CliRunner(CliDependencies dependencies)
    : m_dependencies(std::move(dependencies))
{
    if (!m_dependencies.processInvoker)
        m_dependencies.processInvoker = runProcessDefault;
}

QString CliRunner::helpText()
{
    return QStringLiteral(R"HELP(WimForge command line

Usage:
  WimForge [--json] [--project FOLDER] [--store FOLDER] <command> ...
  WimForge @arguments.rsp
  WimForge --config invocation.json

Project and configuration:
  project create FOLDER --name NAME [--description TEXT]
  project open [FOLDER]
  project import PROJECT.json DESTINATION
  project export DESTINATION.json
  project validate [--execution]
  config show
  config set PATH JSON_VALUE
  config add PATH JSON_VALUE
  config remove PATH JSON_VALUE
  config erase PATH
  config edit [--set PATH VALUE] [--add PATH VALUE]
              [--remove PATH VALUE] [--erase PATH] ...

Servicing and history:
  plan [--script FILE.ps1]
  dry-run [--script FILE.ps1]
  apply [--dry-run] [--script FILE.ps1] [--yes]
  history log [--limit N]
  history undo | history redo
  action-history list [--context KEY] [--element ID] [--limit N]
  action-history record --title TEXT --context KEY --element ID
                        --forward JSON_OBJECT --inverse JSON_OBJECT
  action-history undo|redo EVENT_ID
  action-history bookmark|branch NAME [--event EVENT_ID]
  action-history switch BRANCH
  bundle export SAVE.wimforge --project FOLDER [--notifications FOLDER]
  bundle import SAVE.wimforge DESTINATION [--overwrite]

Notification center:
  notifications list [--all|--include-dismissed] [--include-deleted]
  notifications new --title TEXT --message TEXT [--severity LEVEL]
                    [--source NAME] [--data JSON_OBJECT]
  notifications read|unread|dismiss|restore|delete ID
  notifications events [--limit N]
  notifications history | notifications undo | notifications redo

Unattended setup:
  unattend template full|ai-development [--output FILE] [--format json|xml]
  unattend import FILE [--output FILE] [--format json|xml]
  unattend export FILE --output FILE [--format json|xml]
  unattend validate FILE
  unattend computer-name FILE --mode random|fixed|prompt|serial
                           [--value NAME] [--prefix TEXT] [--output FILE]
  unattend gvlk [list] [--edition TEXT]
  unattend gvlk set FILE --edition EDITION [--output FILE]

Package Studio and Group Policy:
  package catalog
  package template ai-development [--output PROFILE.json]
  package import PROFILE.json [--output FILE]
  package export PROFILE.json --output FILE
  package validate PROFILE.json | package plan PROFILE.json
  package stage PROFILE.json --directory ISO-STAGING-FOLDER
  package ensure-opencode [--dry-run|--yes]
  gpo catalog [--path PolicyDefinitions] [--locale NAME ...] [--summary]
  gpo search QUERY [--regex] [--path PolicyDefinitions] [--locale NAME ...]
  gpo export FILE.md [--path PolicyDefinitions] [--primary LOCALE]
             [--secondary LOCALE]

VM Lab (VMware and VirtualBox):
  vm help
  vm paths | vm catalog | vm detect | vm inventory
  vm create --provider ID --name NAME --iso FILE [hardware options]
  vm import --provider ID --config FILE [--name NAME]
  vm lifecycle ACTION --provider ID --vm ID
  vm configure --provider ID --vm ID [configuration options]
  vm device ACTION --provider ID --vm ID [device topology]
  vm snapshot ACTION --provider ID --vm ID [snapshot selector]
  vm forget|unregister|delete --provider ID --vm ID
  Add --execute --yes to a reviewed mutation; destructive actions also need
  --confirm "EXACT TOKEN". Run 'WimForge vm help' for the complete contract.

WinForge Bridge:
  winforge detect RUNTIME-FOLDER
  winforge template page TARGET [--output RECIPE.json]
  winforge validate [RECIPE.json] [--runtime RUNTIME-FOLDER]
  winforge status [RECIPE.json] [--runtime RUNTIME-FOLDER]
  winforge import RECIPE.json --project FOLDER
  winforge export --project FOLDER --output RECIPE.json
  winforge stage [RECIPE.json] --iso ISO-STAGING-FOLDER
                 [--runtime FOLDER] [--payload FOLDER]
                 [--include-runtime|--without-runtime] [--overwrite]

Field paths are RFC 6901 JSON Pointers such as /features/enable, or dot paths
such as features.enable. Values that parse as JSON retain their JSON type;
other values are strings. Add appends to an array or merges a JSON object.
Remove deletes matching array values or an object key; erase deletes the path.

Commands never ask an interactive terminal question. Destructive apply and
software installation return exit 5 until the caller supplies --yes. Use
--json for one deterministic JSON envelope. Response files may be quoted text,
a JSON string array, or {"project":"...","output":"json","arguments":[...]}.

Exit codes: 0 success, 2 usage, 3 validation, 4 not found, 5 confirmation
required, 6 external process failure, 7 history/state conflict, 8 I/O, and
10 internal error.

WinForge recipes are strictly typed. Legacy runtimes expose only the verified
--page alias contract; module and tweak replay require a declared runtime
contract. Project bundle and action-history commands are available in this build.)HELP");
}

CliResult CliRunner::run(const QStringList &arguments) const
{
    CommandContext context;
    QString error;
    QSet<QString> activeFiles;
    const auto expanded = expandArguments(arguments,
                                          QDir::currentPath(),
                                          &activeFiles,
                                          0,
                                          &error);
    if (!expanded)
        return failureResult(context, CliExitCode::Usage, QStringLiteral("arguments"), error);

    QStringList commandArguments = *expanded;
    context.json = takeFlag(&commandArguments, QStringLiteral("--json"));
    takeFlag(&commandArguments, QStringLiteral("--human"));
    if (!takeOption(&commandArguments,
                    QStringLiteral("--project"),
                    &context.projectDirectory,
                    &error)
        || !takeOption(&commandArguments,
                       QStringLiteral("--store"),
                       &context.notificationDirectory,
                       &error)) {
        return failureResult(context, CliExitCode::Usage, QStringLiteral("arguments"), error);
    }
    if (!context.projectDirectory.isEmpty())
        context.projectDirectory = QDir(context.projectDirectory).absolutePath();
    if (!context.notificationDirectory.isEmpty())
        context.notificationDirectory = QDir(context.notificationDirectory).absolutePath();

    const bool help = takeFlag(&commandArguments, QStringLiteral("--help"))
        || takeFlag(&commandArguments, QStringLiteral("-h"));
    if (commandArguments.isEmpty() || help) {
        return successResult(context,
                             QStringLiteral("help"),
                             QJsonObject{{QStringLiteral("text"), helpText()}},
                             helpText());
    }

    const QString command = commandArguments.takeFirst().toLower();
    if (command == QStringLiteral("help"))
        return successResult(context,
                             QStringLiteral("help"),
                             QJsonObject{{QStringLiteral("text"), helpText()}},
                             helpText());
    if (command == QStringLiteral("project"))
        return runProjectCommand(context, commandArguments);
    if (command == QStringLiteral("config"))
        return runConfigCommand(context, commandArguments);
    if (command == QStringLiteral("plan") || command == QStringLiteral("dry-run")
        || command == QStringLiteral("apply")) {
        return runPlanCommand(context, commandArguments, command, m_dependencies);
    }
    if (command == QStringLiteral("history"))
        return runHistoryCommand(context, commandArguments);
    if (command == QStringLiteral("notification")
        || command == QStringLiteral("notifications")) {
        return runNotificationCommand(context, commandArguments);
    }
    if (command == QStringLiteral("unattend"))
        return runUnattendCommand(context, commandArguments);
    if (command == QStringLiteral("package")
        || command == QStringLiteral("packages")
        || command == QStringLiteral("package-manager")) {
        return runPackageCommand(context, commandArguments, m_dependencies);
    }
    if (command == QStringLiteral("gpo"))
        return runGpoCommand(context, commandArguments);
    if (command == QStringLiteral("bundle"))
        return runBundleCommand(context, commandArguments);
    if (command == QStringLiteral("action-history"))
        return runActionHistoryCommand(context, commandArguments);
    if (command == QStringLiteral("winforge"))
        return runWinForgeCommand(context, commandArguments);
    if (command == QStringLiteral("vm") || command == QStringLiteral("vm-lab"))
        return runVmCommand(context, commandArguments, m_dependencies);
    return failureResult(context,
                         CliExitCode::Usage,
                         command,
                         QStringLiteral("Unknown command '%1'. Run 'WimForge help'.").arg(command));
}

} // namespace wimforge
