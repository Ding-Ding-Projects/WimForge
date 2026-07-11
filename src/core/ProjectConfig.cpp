#include "ProjectConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <utility>

namespace wimforge {
namespace {

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

QJsonArray stringArray(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.append(value);
    return result;
}

QStringList readStringArray(const QJsonObject &object,
                            const QString &key,
                            QStringList *errors,
                            bool required = false)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        if (required)
            errors->append(QStringLiteral("Missing '%1' array.").arg(key));
        return {};
    }
    if (!value.isArray()) {
        errors->append(QStringLiteral("'%1' must be an array of strings.").arg(key));
        return {};
    }

    QStringList result;
    const QJsonArray array = value.toArray();
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (!array.at(index).isString()) {
            errors->append(QStringLiteral("'%1[%2]' must be a string.").arg(key).arg(index));
            continue;
        }
        result.append(array.at(index).toString());
    }
    return result;
}

QString readString(const QJsonObject &object,
                   const QString &key,
                   QStringList *errors,
                   bool required = false,
                   const QString &fallback = {})
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        if (required)
            errors->append(QStringLiteral("Missing '%1'.").arg(key));
        return fallback;
    }
    if (!value.isString()) {
        errors->append(QStringLiteral("'%1' must be a string.").arg(key));
        return fallback;
    }
    return value.toString();
}

QJsonObject requiredObject(const QJsonObject &parent, const QString &key, QStringList *errors)
{
    const QJsonValue value = parent.value(key);
    if (!value.isObject()) {
        errors->append(QStringLiteral("'%1' must be an object.").arg(key));
        return {};
    }
    return value.toObject();
}

bool writeJson(const QString &filePath, const QJsonObject &json, QString *error)
{
    const QFileInfo destination(filePath);
    if (!QDir().mkpath(destination.absolutePath())) {
        setError(error, QStringLiteral("Could not create folder: %1").arg(destination.absolutePath()));
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not open %1 for writing: %2")
                            .arg(filePath, file.errorString()));
        return false;
    }

    const QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        setError(error, QStringLiteral("Could not write %1: %2").arg(filePath, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QStringLiteral("Could not finish writing %1: %2").arg(filePath, file.errorString()));
        return false;
    }

    setError(error, {});
    return true;
}

std::optional<QJsonObject> readJsonObject(const QString &filePath, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open %1: %2").arg(filePath, file.errorString()));
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, QStringLiteral("Invalid JSON in %1 at offset %2: %3")
                            .arg(filePath)
                            .arg(parseError.offset)
                            .arg(parseError.errorString()));
        return std::nullopt;
    }
    if (!document.isObject()) {
        setError(error, QStringLiteral("%1 must contain one JSON object.").arg(filePath));
        return std::nullopt;
    }
    setError(error, {});
    return document.object();
}

enum class PathKind { Any, File, Directory };

void validatePath(QStringList *errors,
                  const QString &label,
                  const QString &path,
                  bool mustExist,
                  PathKind kind = PathKind::Any)
{
    if (path.trimmed().isEmpty()) {
        errors->append(QStringLiteral("%1 path is required.").arg(label));
        return;
    }

    const QFileInfo info(path);
    if (!info.isAbsolute()) {
        errors->append(QStringLiteral("%1 path must be absolute: %2").arg(label, path));
        return;
    }
    if (mustExist && !info.exists()) {
        errors->append(QStringLiteral("%1 path does not exist: %2").arg(label, path));
        return;
    }
    if (info.exists() && kind == PathKind::File && !info.isFile())
        errors->append(QStringLiteral("%1 must be a file: %2").arg(label, path));
    if (info.exists() && kind == PathKind::Directory && !info.isDir())
        errors->append(QStringLiteral("%1 must be a folder: %2").arg(label, path));
}

void validateDraftPath(QStringList *errors,
                       const QString &label,
                       const QString &path,
                       PathKind kind = PathKind::Any)
{
    if (!path.trimmed().isEmpty())
        validatePath(errors, label, path, false, kind);
}

void validatePathList(QStringList *errors,
                      const QString &label,
                      const QStringList &paths,
                      PathKind kind,
                      bool mustExist)
{
    for (qsizetype index = 0; index < paths.size(); ++index)
        validatePath(errors, QStringLiteral("%1[%2]").arg(label).arg(index), paths.at(index), mustExist, kind);
}

void rejectOverlap(QStringList *errors,
                   const QString &leftLabel,
                   const QStringList &left,
                   const QString &rightLabel,
                   const QStringList &right)
{
    QSet<QString> normalized;
    for (const QString &value : left)
        normalized.insert(value.trimmed().toCaseFolded());
    for (const QString &value : right) {
        if (normalized.contains(value.trimmed().toCaseFolded()))
            errors->append(QStringLiteral("'%1' cannot be in both %2 and %3.")
                               .arg(value, leftLabel, rightLabel));
    }
}

QString payloadScopeName(PayloadScope scope)
{
    return scope == PayloadScope::Media ? QStringLiteral("media") : QStringLiteral("image");
}

std::optional<PayloadScope> parsePayloadScope(const QString &value)
{
    if (value.compare(QStringLiteral("image"), Qt::CaseInsensitive) == 0)
        return PayloadScope::Image;
    if (value.compare(QStringLiteral("media"), Qt::CaseInsensitive) == 0)
        return PayloadScope::Media;
    return std::nullopt;
}

QString taskDispositionName(ScheduledTaskDisposition disposition)
{
    switch (disposition) {
    case ScheduledTaskDisposition::Disable: return QStringLiteral("disable");
    case ScheduledTaskDisposition::Enable: return QStringLiteral("enable");
    case ScheduledTaskDisposition::Remove: return QStringLiteral("remove");
    }
    return {};
}

std::optional<ScheduledTaskDisposition> parseTaskDisposition(const QString &value)
{
    if (value.compare(QStringLiteral("disable"), Qt::CaseInsensitive) == 0)
        return ScheduledTaskDisposition::Disable;
    if (value.compare(QStringLiteral("enable"), Qt::CaseInsensitive) == 0)
        return ScheduledTaskDisposition::Enable;
    if (value.compare(QStringLiteral("remove"), Qt::CaseInsensitive) == 0)
        return ScheduledTaskDisposition::Remove;
    return std::nullopt;
}

QString answerFileModeName(AnswerFileMode mode)
{
    switch (mode) {
    case AnswerFileMode::Apply: return QStringLiteral("apply");
    case AnswerFileMode::Place: return QStringLiteral("place");
    case AnswerFileMode::Remove: return QStringLiteral("remove");
    }
    return {};
}

std::optional<AnswerFileMode> parseAnswerFileMode(const QString &value)
{
    if (value.compare(QStringLiteral("apply"), Qt::CaseInsensitive) == 0)
        return AnswerFileMode::Apply;
    if (value.compare(QStringLiteral("place"), Qt::CaseInsensitive) == 0)
        return AnswerFileMode::Place;
    if (value.compare(QStringLiteral("remove"), Qt::CaseInsensitive) == 0)
        return AnswerFileMode::Remove;
    return std::nullopt;
}

bool unsafeRelativePath(const QString &value)
{
    const QString portable = QDir::fromNativeSeparators(value.trimmed());
    if (portable.isEmpty() || QDir::isAbsolutePath(portable))
        return true;
    const QStringList segments = portable.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    return std::any_of(segments.cbegin(), segments.cend(), [](const QString &segment) {
        return segment.isEmpty() || segment == QStringLiteral(".")
            || segment == QStringLiteral("..");
    });
}

} // namespace

bool CustomizeSettings::value(const QString &id) const
{
    if (id == QStringLiteral("disableTelemetry")) return disableTelemetry;
    if (id == QStringLiteral("localAccountOobe")) return localAccountOobe;
    if (id == QStringLiteral("showFileExtensions")) return showFileExtensions;
    if (id == QStringLiteral("classicContextMenu")) return classicContextMenu;
    if (id == QStringLiteral("disableConsumerFeatures")) return disableConsumerFeatures;
    if (id == QStringLiteral("enableLongPaths")) return enableLongPaths;
    if (id == QStringLiteral("performanceVisuals")) return performanceVisuals;
    if (id == QStringLiteral("disableRecall")) return disableRecall;
    return false;
}

bool CustomizeSettings::setValue(const QString &id, bool enabled)
{
    if (id == QStringLiteral("disableTelemetry")) disableTelemetry = enabled;
    else if (id == QStringLiteral("localAccountOobe")) localAccountOobe = enabled;
    else if (id == QStringLiteral("showFileExtensions")) showFileExtensions = enabled;
    else if (id == QStringLiteral("classicContextMenu")) classicContextMenu = enabled;
    else if (id == QStringLiteral("disableConsumerFeatures")) disableConsumerFeatures = enabled;
    else if (id == QStringLiteral("enableLongPaths")) enableLongPaths = enabled;
    else if (id == QStringLiteral("performanceVisuals")) performanceVisuals = enabled;
    else if (id == QStringLiteral("disableRecall")) disableRecall = enabled;
    else return false;
    return true;
}

QString ProjectConfig::projectFilePath() const
{
    return QDir(projectDirectory).filePath(QString::fromLatin1(FileName));
}

ProjectValidation ProjectConfig::validate() const
{
    ProjectValidation result;

    if (projectDirectory.trimmed().isEmpty()) {
        result.errors.append(QStringLiteral("Project folder is required."));
    } else {
        const QFileInfo projectInfo(projectDirectory);
        if (!projectInfo.isAbsolute())
            result.errors.append(QStringLiteral("Project folder must be absolute: %1").arg(projectDirectory));
        if (projectInfo.exists() && !projectInfo.isDir())
            result.errors.append(QStringLiteral("Project folder points to a file: %1").arg(projectDirectory));
    }

    if (projectName.trimmed().isEmpty())
        result.errors.append(QStringLiteral("Project name is required."));
    if (selectedImageIndex < 1)
        result.errors.append(QStringLiteral("Selected image index must be 1 or greater."));

    validateDraftPath(&result.errors, QStringLiteral("Source"), sourcePath);
    validateDraftPath(&result.errors, QStringLiteral("Image"), imagePath, PathKind::File);
    validateDraftPath(&result.errors, QStringLiteral("Mount"), mountPath, PathKind::Directory);
    validateDraftPath(&result.errors, QStringLiteral("Output"), outputPath);
    validatePathList(&result.errors, QStringLiteral("Driver"), drivers, PathKind::Any, false);
    validatePathList(&result.errors, QStringLiteral("Update"), updates, PathKind::File, false);
    validatePathList(&result.errors, QStringLiteral("Package"), packages, PathKind::File, false);
    validatePathList(&result.errors, QStringLiteral("Appx provision"), appxPackagesToProvision,
                     PathKind::File, false);
    validatePathList(&result.errors, QStringLiteral("Unattended file"), unattendedFiles,
                     PathKind::File, false);

    for (qsizetype index = 0; index < stagedPayloads.size(); ++index) {
        const StagedPayload &payload = stagedPayloads.at(index);
        validateDraftPath(&result.errors, QStringLiteral("Staged payload[%1]").arg(index),
                          payload.sourcePath);
        if (unsafeRelativePath(payload.destinationPath)) {
            result.errors.append(QStringLiteral("Staged payload[%1] destination must be a safe relative path: %2")
                                     .arg(index).arg(payload.destinationPath));
        }
        if (payload.role.trimmed().isEmpty())
            result.errors.append(QStringLiteral("Staged payload[%1] needs a role.").arg(index));
    }

    for (qsizetype index = 0; index < scheduledTaskChanges.size(); ++index) {
        if (unsafeRelativePath(scheduledTaskChanges.at(index).taskPath)) {
            result.errors.append(QStringLiteral("Scheduled task[%1] must be relative to Windows/System32/Tasks: %2")
                                     .arg(index).arg(scheduledTaskChanges.at(index).taskPath));
        }
    }

    for (qsizetype index = 0; index < answerFileActions.size(); ++index) {
        const AnswerFileAction &action = answerFileActions.at(index);
        if (action.mode != AnswerFileMode::Remove) {
            validateDraftPath(&result.errors, QStringLiteral("Answer file[%1]").arg(index),
                              action.sourcePath, PathKind::File);
            if (!action.sourcePath.trimmed().isEmpty()
                && QFileInfo(action.sourcePath).suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) != 0) {
                result.errors.append(QStringLiteral("Answer file[%1] must use the .xml extension: %2")
                                         .arg(index).arg(action.sourcePath));
            }
        }
        if (action.mode != AnswerFileMode::Apply && unsafeRelativePath(action.destinationPath)) {
            result.errors.append(QStringLiteral("Answer file[%1] destination must be a safe relative path: %2")
                                     .arg(index).arg(action.destinationPath));
        }
    }

    for (qsizetype index = 0; index < postSetupCommands.size(); ++index) {
        const QString command = postSetupCommands.at(index).command;
        if (command.trimmed().isEmpty())
            result.errors.append(QStringLiteral("Post-setup command[%1] is empty.").arg(index));
        if (command.contains(QLatin1Char('\r')) || command.contains(QLatin1Char('\n'))
            || command.contains(QChar::Null)) {
            result.errors.append(QStringLiteral("Post-setup command[%1] must be one literal command line.").arg(index));
        }
    }

    if (!unattendedXmlPath.trimmed().isEmpty()) {
        validateDraftPath(&result.errors, QStringLiteral("Unattended XML"), unattendedXmlPath,
                          PathKind::File);
        if (QFileInfo(unattendedXmlPath).suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) != 0)
            result.errors.append(QStringLiteral("Unattended answer file must use the .xml extension: %1")
                                     .arg(unattendedXmlPath));
    }

    rejectOverlap(&result.errors, QStringLiteral("features to enable"), featuresToEnable,
                  QStringLiteral("features to disable"), featuresToDisable);
    rejectOverlap(&result.errors, QStringLiteral("capabilities to add"), capabilitiesToAdd,
                  QStringLiteral("capabilities to remove"), capabilitiesToRemove);

    static const QSet<QString> validHives{
        QStringLiteral("HKLM"), QStringLiteral("HKEY_LOCAL_MACHINE"),
        QStringLiteral("HKCU"), QStringLiteral("HKEY_CURRENT_USER"),
        QStringLiteral("HKCR"), QStringLiteral("HKEY_CLASSES_ROOT"),
        QStringLiteral("HKU"), QStringLiteral("HKEY_USERS"),
        QStringLiteral("HKCC"), QStringLiteral("HKEY_CURRENT_CONFIG"),
    };
    static const QSet<QString> validRegistryTypes{
        QStringLiteral("REG_SZ"), QStringLiteral("REG_EXPAND_SZ"), QStringLiteral("REG_MULTI_SZ"),
        QStringLiteral("REG_DWORD"), QStringLiteral("REG_QWORD"), QStringLiteral("REG_BINARY"),
        QStringLiteral("REG_NONE"),
    };
    for (qsizetype index = 0; index < registryTweaks.size(); ++index) {
        const RegistryTweak &tweak = registryTweaks.at(index);
        if (!validHives.contains(tweak.hive.trimmed().toUpper()))
            result.errors.append(QStringLiteral("Registry tweak %1 has an unsupported hive: %2")
                                     .arg(index).arg(tweak.hive));
        if (tweak.key.trimmed().isEmpty())
            result.errors.append(QStringLiteral("Registry tweak %1 needs a key path.").arg(index));
        if (tweak.deleteValue && tweak.deleteAllValues)
            result.errors.append(QStringLiteral("Registry tweak %1 cannot delete one value and all values at the same time.")
                                     .arg(index));
        if (tweak.deleteAllValues && !tweak.valueName.isEmpty())
            result.errors.append(QStringLiteral("Registry tweak %1 must not name a value when deleting all values.")
                                     .arg(index));
        if (!tweak.deleteValue && !tweak.deleteAllValues
            && !validRegistryTypes.contains(tweak.type.trimmed().toUpper()))
            result.errors.append(QStringLiteral("Registry tweak %1 has an unsupported type: %2")
                                     .arg(index).arg(tweak.type));
    }

    static const QSet<QString> compressions{
        QStringLiteral("none"), QStringLiteral("fast"), QStringLiteral("max"), QStringLiteral("recovery")};
    if (!compressions.contains(options.compression.trimmed().toLower()))
        result.errors.append(QStringLiteral("Compression must be none, fast, max, or recovery."));
    if (options.maximumParallelOperations < 0 || options.maximumParallelOperations > 64)
        result.errors.append(QStringLiteral("Maximum parallel operations must be between 0 and 64."));
    if (options.splitSizeMb < 100 || options.splitSizeMb > 4095)
        result.errors.append(QStringLiteral("Split size must be between 100 and 4095 MiB."));
    validateDraftPath(&result.errors, QStringLiteral("Scratch"), options.scratchDirectory,
                      PathKind::Directory);

    if (targetBuildNumber < 0)
        result.errors.append(QStringLiteral("Target build number cannot be negative."));

    static const QSet<QString> outputFormats{
        QStringLiteral("wim"), QStringLiteral("esd"), QStringLiteral("swm"), QStringLiteral("iso")};
    if (!outputFormats.contains(outputFormat.trimmed().toLower()))
        result.errors.append(QStringLiteral("Output format must be wim, esd, swm, or iso."));
    if (isoLabel.size() > 32)
        result.errors.append(QStringLiteral("ISO label cannot exceed 32 characters."));
    if (autoExport && autoExportPath.trimmed().isEmpty())
        result.errors.append(QStringLiteral("Auto-export needs a destination path."));
    validateDraftPath(&result.errors, QStringLiteral("Auto-export"), autoExportPath);

    const QJsonValue imageRelativeValue = options.extra.value(QStringLiteral("imageRelativePath"));
    if (!imageRelativeValue.isUndefined()) {
        if (!imageRelativeValue.isString()) {
            result.errors.append(QStringLiteral("imageRelativePath must be a string."));
        } else if (unsafeRelativePath(imageRelativeValue.toString())) {
            result.errors.append(QStringLiteral(
                "imageRelativePath must identify a safe relative file inside installation media."));
        }
    }

    return result;
}

ProjectValidation ProjectConfig::validateForExecution() const
{
    ProjectValidation result = validate();

    validatePath(&result.errors, QStringLiteral("Source"), sourcePath, true);
    const QString imageRelativePath = options.extra.value(
        QStringLiteral("imageRelativePath")).toString().trimmed();
    const QFileInfo sourceInfo(sourcePath);
    const bool isoContainedImage = sourceInfo.isFile()
        && sourceInfo.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) == 0
        && !imageRelativePath.isEmpty() && !unsafeRelativePath(imageRelativePath);
    if (!isoContainedImage)
        validatePath(&result.errors, QStringLiteral("Image"), imagePath, true, PathKind::File);
    validatePath(&result.errors, QStringLiteral("Mount"), mountPath, false, PathKind::Directory);
    validatePath(&result.errors, QStringLiteral("Output"), outputPath, false);
    validatePathList(&result.errors, QStringLiteral("Driver"), drivers, PathKind::Any, true);
    validatePathList(&result.errors, QStringLiteral("Update"), updates, PathKind::File, true);
    validatePathList(&result.errors, QStringLiteral("Package"), packages, PathKind::File, true);
    validatePathList(&result.errors, QStringLiteral("Appx provision"), appxPackagesToProvision,
                     PathKind::File, true);
    validatePathList(&result.errors, QStringLiteral("Unattended file"), unattendedFiles,
                     PathKind::File, true);
    for (qsizetype index = 0; index < stagedPayloads.size(); ++index) {
        validatePath(&result.errors, QStringLiteral("Staged payload[%1]").arg(index),
                     stagedPayloads.at(index).sourcePath, true);
    }
    for (qsizetype index = 0; index < answerFileActions.size(); ++index) {
        const AnswerFileAction &action = answerFileActions.at(index);
        if (action.mode != AnswerFileMode::Remove) {
            validatePath(&result.errors, QStringLiteral("Answer file[%1]").arg(index),
                         action.sourcePath, true, PathKind::File);
        }
    }
    if (!unattendedXmlPath.trimmed().isEmpty())
        validatePath(&result.errors, QStringLiteral("Unattended XML"), unattendedXmlPath, true,
                     PathKind::File);

    result.errors.removeDuplicates();
    return result;
}

bool ProjectConfig::customizeSettingEnabled(const QString &id) const
{
    const QJsonValue legacy = settings.value(id);
    return legacy.isBool() ? legacy.toBool() : customize.value(id);
}

bool ProjectConfig::setCustomizeSetting(const QString &id, bool enabled)
{
    if (!customize.setValue(id, enabled))
        return false;
    settings.insert(id, enabled);
    return true;
}

QJsonObject ProjectConfig::toJson() const
{
    QJsonArray registry;
    for (const RegistryTweak &tweak : registryTweaks) {
        registry.append(QJsonObject{
            {QStringLiteral("hive"), tweak.hive},
            {QStringLiteral("key"), tweak.key},
            {QStringLiteral("name"), tweak.valueName},
            {QStringLiteral("type"), tweak.type},
            {QStringLiteral("value"), tweak.value},
            {QStringLiteral("delete"), tweak.deleteValue},
            {QStringLiteral("deleteAllValues"), tweak.deleteAllValues},
            {QStringLiteral("owner"), tweak.ownerId},
        });
    }

    QJsonArray scheduledTasks;
    for (const ScheduledTaskChange &change : scheduledTaskChanges) {
        scheduledTasks.append(QJsonObject{
            {QStringLiteral("path"), change.taskPath},
            {QStringLiteral("disposition"), taskDispositionName(change.disposition)},
            {QStringLiteral("compatibilityOverride"), change.compatibilityOverride},
        });
    }

    QJsonArray answerFiles;
    for (const AnswerFileAction &action : answerFileActions) {
        answerFiles.append(QJsonObject{
            {QStringLiteral("mode"), answerFileModeName(action.mode)},
            {QStringLiteral("source"), action.sourcePath},
            {QStringLiteral("destination"), action.destinationPath},
            {QStringLiteral("scope"), payloadScopeName(action.scope)},
        });
    }

    QJsonArray commands;
    for (const PostSetupCommand &command : postSetupCommands) {
        commands.append(QJsonObject{
            {QStringLiteral("command"), command.command},
            {QStringLiteral("label"), command.label},
        });
    }

    QJsonArray payloads;
    for (const StagedPayload &payload : stagedPayloads) {
        payloads.append(QJsonObject{
            {QStringLiteral("source"), payload.sourcePath},
            {QStringLiteral("destination"), payload.destinationPath},
            {QStringLiteral("scope"), payloadScopeName(payload.scope)},
            {QStringLiteral("role"), payload.role},
            {QStringLiteral("sha256"), payload.expectedSha256},
        });
    }

    QJsonObject settingJson = settings;
    for (const QString &id : {
             QStringLiteral("disableTelemetry"), QStringLiteral("localAccountOobe"),
             QStringLiteral("showFileExtensions"), QStringLiteral("classicContextMenu"),
             QStringLiteral("disableConsumerFeatures"), QStringLiteral("enableLongPaths"),
             QStringLiteral("performanceVisuals"), QStringLiteral("disableRecall")}) {
        settingJson.insert(id, customizeSettingEnabled(id));
    }

    QJsonObject optionJson = options.extra;
    optionJson.insert(QStringLiteral("verifyPayloads"), options.verifyPayloads);
    optionJson.insert(QStringLiteral("mountReadOnly"), options.mountReadOnly);
    optionJson.insert(QStringLiteral("cleanupComponentStore"), options.cleanupComponentStore);
    optionJson.insert(QStringLiteral("resetBase"), options.resetBase);
    optionJson.insert(QStringLiteral("optimizeImage"), options.optimizeImage);
    optionJson.insert(QStringLiteral("rebuildImage"), options.rebuildImage);
    optionJson.insert(QStringLiteral("createIso"), options.createIso);
    optionJson.insert(QStringLiteral("keepMountOnFailure"), options.keepMountOnFailure);
    optionJson.insert(QStringLiteral("dryRun"), options.dryRun);
    optionJson.insert(QStringLiteral("compression"), options.compression);
    optionJson.insert(QStringLiteral("scratch"), options.scratchDirectory);
    optionJson.insert(QStringLiteral("splitSizeMb"),
                      options.extra.value(QStringLiteral("splitSizeMb")).isDouble()
                          ? options.extra.value(QStringLiteral("splitSizeMb")).toInt(options.splitSizeMb)
                          : options.splitSizeMb);
    optionJson.insert(QStringLiteral("maximumParallelOperations"), options.maximumParallelOperations);

    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("wimforge.project")},
        {QStringLiteral("version"), CurrentSchemaVersion},
        {QStringLiteral("name"), projectName},
        {QStringLiteral("description"), description},
        {QStringLiteral("paths"), QJsonObject{
             {QStringLiteral("source"), sourcePath},
             {QStringLiteral("image"), imagePath},
             {QStringLiteral("mount"), mountPath},
             {QStringLiteral("output"), outputPath},
             {QStringLiteral("unattendedXml"), unattendedXmlPath},
         }},
        {QStringLiteral("image"), QJsonObject{
             {QStringLiteral("index"), selectedImageIndex},
             {QStringLiteral("outputFormat"), outputFormat},
             {QStringLiteral("isoLabel"), isoLabel},
             {QStringLiteral("cloneSource"), cloneSource},
             {QStringLiteral("targetBuild"), targetBuildNumber},
         }},
        {QStringLiteral("drivers"), stringArray(drivers)},
        {QStringLiteral("updates"), stringArray(updates)},
        {QStringLiteral("packages"), stringArray(packages)},
        {QStringLiteral("features"), QJsonObject{
             {QStringLiteral("enable"), stringArray(featuresToEnable)},
             {QStringLiteral("disable"), stringArray(featuresToDisable)},
         }},
        {QStringLiteral("capabilities"), QJsonObject{
             {QStringLiteral("add"), stringArray(capabilitiesToAdd)},
             {QStringLiteral("remove"), stringArray(capabilitiesToRemove)},
         }},
        {QStringLiteral("appx"), QJsonObject{
             {QStringLiteral("remove"), stringArray(appxPackagesToRemove)},
             {QStringLiteral("provision"), stringArray(appxPackagesToProvision)},
         }},
        {QStringLiteral("components"), QJsonObject{
             {QStringLiteral("remove"), stringArray(componentsToRemove)},
         }},
        {QStringLiteral("scheduledTasks"), scheduledTasks},
        {QStringLiteral("unattendedFiles"), stringArray(unattendedFiles)},
        {QStringLiteral("answerFiles"), answerFiles},
        {QStringLiteral("postSetupItems"), stringArray(postSetupItems)},
        {QStringLiteral("postSetupCommands"), commands},
        {QStringLiteral("stagedPayloads"), payloads},
        {QStringLiteral("registry"), registry},
        {QStringLiteral("settings"), settingJson},
        {QStringLiteral("automation"), QJsonObject{
             {QStringLiteral("autoImport"), autoImport},
             {QStringLiteral("autoExport"), autoExport},
             {QStringLiteral("autoExportPath"), autoExportPath},
         }},
        {QStringLiteral("options"), optionJson},
    };
}

std::optional<ProjectConfig> ProjectConfig::fromJson(const QJsonObject &json,
                                                     const QString &directory,
                                                     QString *error)
{
    QStringList errors;
    const QString schema = readString(json, QStringLiteral("schema"), &errors, true);
    if (schema != QStringLiteral("wimforge.project"))
        errors.append(QStringLiteral("Unsupported project schema '%1'.").arg(schema));

    const QJsonValue versionValue = json.value(QStringLiteral("version"));
    if (!versionValue.isDouble() || versionValue.toInt(-1) != CurrentSchemaVersion) {
        errors.append(QStringLiteral("Unsupported project version. Expected %1.").arg(CurrentSchemaVersion));
    }

    ProjectConfig config;
    config.projectDirectory = directory.trimmed().isEmpty()
        ? QString()
        : QDir(directory).absolutePath();
    config.projectName = readString(json, QStringLiteral("name"), &errors, true);
    config.description = readString(json, QStringLiteral("description"), &errors);

    const QJsonObject paths = requiredObject(json, QStringLiteral("paths"), &errors);
    config.sourcePath = readString(paths, QStringLiteral("source"), &errors, true);
    config.imagePath = readString(paths, QStringLiteral("image"), &errors, true);
    config.mountPath = readString(paths, QStringLiteral("mount"), &errors, true);
    config.outputPath = readString(paths, QStringLiteral("output"), &errors, true);
    config.unattendedXmlPath = readString(paths, QStringLiteral("unattendedXml"), &errors);

    const QJsonObject image = requiredObject(json, QStringLiteral("image"), &errors);
    const QJsonValue imageIndex = image.value(QStringLiteral("index"));
    if (!imageIndex.isDouble())
        errors.append(QStringLiteral("'image.index' must be a number."));
    else
        config.selectedImageIndex = imageIndex.toInt();
    config.outputFormat = readString(image, QStringLiteral("outputFormat"), &errors, false,
                                     QStringLiteral("wim"));
    config.isoLabel = readString(image, QStringLiteral("isoLabel"), &errors, false,
                                 QStringLiteral("WIMFORGE"));
    const QJsonValue cloneSource = image.value(QStringLiteral("cloneSource"));
    if (!cloneSource.isUndefined() && !cloneSource.isBool())
        errors.append(QStringLiteral("'image.cloneSource' must be true or false."));
    config.cloneSource = cloneSource.toBool(true);
    const QJsonValue targetBuild = image.value(QStringLiteral("targetBuild"));
    if (!targetBuild.isUndefined() && !targetBuild.isDouble())
        errors.append(QStringLiteral("'image.targetBuild' must be a number."));
    config.targetBuildNumber = targetBuild.toInt(0);

    config.drivers = readStringArray(json, QStringLiteral("drivers"), &errors);
    config.updates = readStringArray(json, QStringLiteral("updates"), &errors);
    config.packages = readStringArray(json, QStringLiteral("packages"), &errors);

    const QJsonObject features = requiredObject(json, QStringLiteral("features"), &errors);
    config.featuresToEnable = readStringArray(features, QStringLiteral("enable"), &errors);
    config.featuresToDisable = readStringArray(features, QStringLiteral("disable"), &errors);
    const QJsonObject capabilities = requiredObject(json, QStringLiteral("capabilities"), &errors);
    config.capabilitiesToAdd = readStringArray(capabilities, QStringLiteral("add"), &errors);
    config.capabilitiesToRemove = readStringArray(capabilities, QStringLiteral("remove"), &errors);
    const QJsonObject appx = requiredObject(json, QStringLiteral("appx"), &errors);
    config.appxPackagesToRemove = readStringArray(appx, QStringLiteral("remove"), &errors);
    config.appxPackagesToProvision = readStringArray(appx, QStringLiteral("provision"), &errors);
    const QJsonObject components = requiredObject(json, QStringLiteral("components"), &errors);
    config.componentsToRemove = readStringArray(components, QStringLiteral("remove"), &errors);

    const QJsonValue scheduledTaskValue = json.value(QStringLiteral("scheduledTasks"));
    if (!scheduledTaskValue.isUndefined() && !scheduledTaskValue.isArray()) {
        errors.append(QStringLiteral("'scheduledTasks' must be an array."));
    } else {
        const QJsonArray scheduledTasks = scheduledTaskValue.toArray();
        for (qsizetype index = 0; index < scheduledTasks.size(); ++index) {
            if (!scheduledTasks.at(index).isObject()) {
                errors.append(QStringLiteral("'scheduledTasks[%1]' must be an object.").arg(index));
                continue;
            }
            const QJsonObject item = scheduledTasks.at(index).toObject();
            ScheduledTaskChange change;
            change.taskPath = readString(item, QStringLiteral("path"), &errors, true);
            const QString disposition = readString(item, QStringLiteral("disposition"), &errors,
                                                    false, QStringLiteral("disable"));
            const std::optional<ScheduledTaskDisposition> parsed = parseTaskDisposition(disposition);
            if (!parsed)
                errors.append(QStringLiteral("'scheduledTasks[%1].disposition' is invalid.").arg(index));
            else
                change.disposition = *parsed;
            const QJsonValue overrideValue = item.value(QStringLiteral("compatibilityOverride"));
            if (!overrideValue.isUndefined() && !overrideValue.isBool()) {
                errors.append(QStringLiteral("'scheduledTasks[%1].compatibilityOverride' must be true or false.")
                                  .arg(index));
            }
            change.compatibilityOverride = overrideValue.toBool(false);
            config.scheduledTaskChanges.append(std::move(change));
        }
    }

    config.unattendedFiles = readStringArray(json, QStringLiteral("unattendedFiles"), &errors);
    const QJsonValue answerFileValue = json.value(QStringLiteral("answerFiles"));
    if (!answerFileValue.isUndefined() && !answerFileValue.isArray()) {
        errors.append(QStringLiteral("'answerFiles' must be an array."));
    } else {
        const QJsonArray answerFiles = answerFileValue.toArray();
        for (qsizetype index = 0; index < answerFiles.size(); ++index) {
            if (!answerFiles.at(index).isObject()) {
                errors.append(QStringLiteral("'answerFiles[%1]' must be an object.").arg(index));
                continue;
            }
            const QJsonObject item = answerFiles.at(index).toObject();
            AnswerFileAction action;
            const QString mode = readString(item, QStringLiteral("mode"), &errors,
                                            false, QStringLiteral("apply"));
            const std::optional<AnswerFileMode> parsedMode = parseAnswerFileMode(mode);
            if (!parsedMode)
                errors.append(QStringLiteral("'answerFiles[%1].mode' is invalid.").arg(index));
            else
                action.mode = *parsedMode;
            action.sourcePath = readString(item, QStringLiteral("source"), &errors);
            action.destinationPath = readString(item, QStringLiteral("destination"), &errors,
                                                false, QStringLiteral("Windows/Panther/unattend.xml"));
            const QString scope = readString(item, QStringLiteral("scope"), &errors,
                                             false, QStringLiteral("image"));
            const std::optional<PayloadScope> parsedScope = parsePayloadScope(scope);
            if (!parsedScope)
                errors.append(QStringLiteral("'answerFiles[%1].scope' is invalid.").arg(index));
            else
                action.scope = *parsedScope;
            config.answerFileActions.append(std::move(action));
        }
    }
    config.postSetupItems = readStringArray(json, QStringLiteral("postSetupItems"), &errors);

    const QJsonValue postSetupValue = json.value(QStringLiteral("postSetupCommands"));
    if (!postSetupValue.isUndefined() && !postSetupValue.isArray()) {
        errors.append(QStringLiteral("'postSetupCommands' must be an array."));
    } else {
        const QJsonArray commands = postSetupValue.toArray();
        for (qsizetype index = 0; index < commands.size(); ++index) {
            if (!commands.at(index).isObject()) {
                errors.append(QStringLiteral("'postSetupCommands[%1]' must be an object.").arg(index));
                continue;
            }
            const QJsonObject item = commands.at(index).toObject();
            config.postSetupCommands.append(PostSetupCommand{
                readString(item, QStringLiteral("command"), &errors, true),
                readString(item, QStringLiteral("label"), &errors)});
        }
    }

    const QJsonValue stagedPayloadValue = json.value(QStringLiteral("stagedPayloads"));
    if (!stagedPayloadValue.isUndefined() && !stagedPayloadValue.isArray()) {
        errors.append(QStringLiteral("'stagedPayloads' must be an array."));
    } else {
        const QJsonArray payloads = stagedPayloadValue.toArray();
        for (qsizetype index = 0; index < payloads.size(); ++index) {
            if (!payloads.at(index).isObject()) {
                errors.append(QStringLiteral("'stagedPayloads[%1]' must be an object.").arg(index));
                continue;
            }
            const QJsonObject item = payloads.at(index).toObject();
            StagedPayload payload;
            payload.sourcePath = readString(item, QStringLiteral("source"), &errors, true);
            payload.destinationPath = readString(item, QStringLiteral("destination"), &errors, true);
            payload.role = readString(item, QStringLiteral("role"), &errors,
                                      false, QStringLiteral("payload"));
            payload.expectedSha256 = readString(item, QStringLiteral("sha256"), &errors);
            const QString scope = readString(item, QStringLiteral("scope"), &errors,
                                             false, QStringLiteral("image"));
            const std::optional<PayloadScope> parsedScope = parsePayloadScope(scope);
            if (!parsedScope)
                errors.append(QStringLiteral("'stagedPayloads[%1].scope' is invalid.").arg(index));
            else
                payload.scope = *parsedScope;
            config.stagedPayloads.append(std::move(payload));
        }
    }

    const QJsonValue registryValue = json.value(QStringLiteral("registry"));
    if (!registryValue.isUndefined() && !registryValue.isArray()) {
        errors.append(QStringLiteral("'registry' must be an array."));
    } else {
        const QJsonArray registry = registryValue.toArray();
        for (qsizetype index = 0; index < registry.size(); ++index) {
            if (!registry.at(index).isObject()) {
                errors.append(QStringLiteral("'registry[%1]' must be an object.").arg(index));
                continue;
            }
            const QJsonObject item = registry.at(index).toObject();
            RegistryTweak tweak;
            tweak.hive = readString(item, QStringLiteral("hive"), &errors, true);
            tweak.key = readString(item, QStringLiteral("key"), &errors, true);
            tweak.valueName = readString(item, QStringLiteral("name"), &errors);
            tweak.type = readString(item, QStringLiteral("type"), &errors, false, QStringLiteral("REG_SZ"));
            tweak.value = readString(item, QStringLiteral("value"), &errors);
            const QJsonValue deleteValue = item.value(QStringLiteral("delete"));
            if (!deleteValue.isUndefined() && !deleteValue.isBool())
                errors.append(QStringLiteral("'registry[%1].delete' must be true or false.").arg(index));
            tweak.deleteValue = deleteValue.toBool(false);
            const QJsonValue deleteAllValues = item.value(QStringLiteral("deleteAllValues"));
            if (!deleteAllValues.isUndefined() && !deleteAllValues.isBool()) {
                errors.append(QStringLiteral("'registry[%1].deleteAllValues' must be true or false.")
                                  .arg(index));
            }
            tweak.deleteAllValues = deleteAllValues.toBool(false);
            tweak.ownerId = readString(item, QStringLiteral("owner"), &errors);
            config.registryTweaks.append(std::move(tweak));
        }
    }

    const QJsonObject options = requiredObject(json, QStringLiteral("options"), &errors);
    auto readBool = [&errors, &options](const QString &key, bool fallback) {
        const QJsonValue value = options.value(key);
        if (value.isUndefined())
            return fallback;
        if (!value.isBool()) {
            errors.append(QStringLiteral("'options.%1' must be true or false.").arg(key));
            return fallback;
        }
        return value.toBool();
    };
    config.options.verifyPayloads = readBool(QStringLiteral("verifyPayloads"), true);
    config.options.mountReadOnly = readBool(QStringLiteral("mountReadOnly"), false);
    config.options.cleanupComponentStore = readBool(QStringLiteral("cleanupComponentStore"), true);
    config.options.resetBase = readBool(QStringLiteral("resetBase"), false);
    config.options.optimizeImage = readBool(QStringLiteral("optimizeImage"), true);
    config.options.rebuildImage = readBool(QStringLiteral("rebuildImage"), true);
    config.options.createIso = readBool(QStringLiteral("createIso"), false);
    config.options.keepMountOnFailure = readBool(QStringLiteral("keepMountOnFailure"), false);
    config.options.dryRun = readBool(QStringLiteral("dryRun"), false);
    config.options.compression = readString(options, QStringLiteral("compression"), &errors, false,
                                            QStringLiteral("max"));
    config.options.scratchDirectory = readString(options, QStringLiteral("scratch"), &errors);
    const QJsonValue splitSize = options.value(QStringLiteral("splitSizeMb"));
    if (!splitSize.isUndefined() && !splitSize.isDouble())
        errors.append(QStringLiteral("'options.splitSizeMb' must be a number."));
    config.options.splitSizeMb = splitSize.toInt(3800);
    const QJsonValue parallel = options.value(QStringLiteral("maximumParallelOperations"));
    if (!parallel.isUndefined() && !parallel.isDouble())
        errors.append(QStringLiteral("'options.maximumParallelOperations' must be a number."));
    config.options.maximumParallelOperations = parallel.toInt(0);

    config.options.extra = options;
    for (const QString &known : {
             QStringLiteral("verifyPayloads"), QStringLiteral("mountReadOnly"),
             QStringLiteral("cleanupComponentStore"), QStringLiteral("resetBase"),
             QStringLiteral("optimizeImage"), QStringLiteral("rebuildImage"),
             QStringLiteral("createIso"), QStringLiteral("keepMountOnFailure"),
             QStringLiteral("dryRun"), QStringLiteral("compression"), QStringLiteral("scratch"),
             QStringLiteral("splitSizeMb"),
             QStringLiteral("maximumParallelOperations")}) {
        config.options.extra.remove(known);
    }

    const QJsonValue settings = json.value(QStringLiteral("settings"));
    if (!settings.isUndefined() && !settings.isObject())
        errors.append(QStringLiteral("'settings' must be an object."));
    else {
        config.settings = settings.toObject();
        for (const QString &id : {
                 QStringLiteral("disableTelemetry"), QStringLiteral("localAccountOobe"),
                 QStringLiteral("showFileExtensions"), QStringLiteral("classicContextMenu"),
                 QStringLiteral("disableConsumerFeatures"), QStringLiteral("enableLongPaths"),
                 QStringLiteral("performanceVisuals"), QStringLiteral("disableRecall")}) {
            const QJsonValue value = config.settings.value(id);
            if (!value.isUndefined() && !value.isBool()) {
                errors.append(QStringLiteral("'settings.%1' must be true or false.").arg(id));
            } else if (value.isBool()) {
                config.customize.setValue(id, value.toBool());
            }
        }
    }

    const QJsonValue automationValue = json.value(QStringLiteral("automation"));
    if (!automationValue.isUndefined() && !automationValue.isObject()) {
        errors.append(QStringLiteral("'automation' must be an object."));
    } else {
        const QJsonObject automation = automationValue.toObject();
        auto readAutomationBool = [&automation, &errors](const QString &key, bool fallback) {
            const QJsonValue value = automation.value(key);
            if (value.isUndefined())
                return fallback;
            if (!value.isBool()) {
                errors.append(QStringLiteral("'automation.%1' must be true or false.").arg(key));
                return fallback;
            }
            return value.toBool();
        };
        config.autoImport = readAutomationBool(QStringLiteral("autoImport"), false);
        config.autoExport = readAutomationBool(QStringLiteral("autoExport"), false);
        config.autoExportPath = readString(automation, QStringLiteral("autoExportPath"), &errors);
    }

    if (!errors.isEmpty()) {
        setError(error, errors.join(QLatin1Char('\n')));
        return std::nullopt;
    }

    setError(error, {});
    return config;
}

std::optional<ProjectConfig> ProjectConfig::load(const QString &directory, QString *error)
{
    const QString absoluteDirectory = QDir(directory).absolutePath();
    const QString path = QDir(absoluteDirectory).filePath(QString::fromLatin1(FileName));
    const std::optional<QJsonObject> json = readJsonObject(path, error);
    if (!json)
        return std::nullopt;
    return fromJson(*json, absoluteDirectory, error);
}

bool ProjectConfig::save(QString *error, const QString &commitMessage) const
{
    const ProjectValidation validation = validate();
    if (!validation.ok()) {
        setError(error, QStringLiteral("Project cannot be saved:\n%1").arg(validation.message()));
        return false;
    }

    GitHistory localHistory(projectDirectory);
    if (!localHistory.initialize(error))
        return false;

    const bool existed = QFileInfo::exists(projectFilePath());
    if (!writeJson(projectFilePath(), toJson(), error))
        return false;

    const QString message = commitMessage.trimmed().isEmpty()
        ? QStringLiteral("%1 project: %2 / %3工程：%2")
              .arg(existed ? QStringLiteral("Save") : QStringLiteral("Create"),
                   projectName,
                   existed ? QStringLiteral("儲存") : QStringLiteral("建立"))
        : commitMessage;
    QString gitError;
    if (!localHistory.commit(message, &gitError)) {
        setError(error, QStringLiteral("project.json was written, but its local Git commit failed: %1")
                            .arg(gitError));
        return false;
    }

    if (autoExport && QFileInfo(autoExportPath).absoluteFilePath()
            != QFileInfo(projectFilePath()).absoluteFilePath()
        && !writeJson(autoExportPath, toJson(), &gitError)) {
        setError(error, QStringLiteral("Project was saved and committed, but auto-export failed: %1")
                            .arg(gitError));
        return false;
    }

    setError(error, {});
    return true;
}

bool ProjectConfig::exportJson(const QString &destinationFile, QString *error) const
{
    if (destinationFile.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Export destination is empty."));
        return false;
    }

    if (QFileInfo(destinationFile).absoluteFilePath()
        == QFileInfo(projectFilePath()).absoluteFilePath()) {
        return save(error, QStringLiteral("Export project JSON / 匯出工程 JSON"));
    }

    const ProjectValidation validation = validate();
    if (!validation.ok()) {
        setError(error, QStringLiteral("Project cannot be exported:\n%1").arg(validation.message()));
        return false;
    }
    return writeJson(destinationFile, toJson(), error);
}

std::optional<ProjectConfig> ProjectConfig::importJson(const QString &sourceFile,
                                                       const QString &destinationProjectDirectory,
                                                       QString *error)
{
    const std::optional<QJsonObject> json = readJsonObject(sourceFile, error);
    if (!json)
        return std::nullopt;

    const QString destination = QDir(destinationProjectDirectory).absolutePath();
    std::optional<ProjectConfig> config = fromJson(*json, destination, error);
    if (!config)
        return std::nullopt;

    if (!config->save(error, QStringLiteral("Import project JSON / 匯入工程 JSON")))
        return std::nullopt;
    return config;
}

QList<GitCommit> ProjectConfig::history(int maximumCount, QString *error) const
{
    return GitHistory(projectDirectory).history(maximumCount, error);
}

bool ProjectConfig::revertLatest(QString *error) const
{
    return GitHistory(projectDirectory).revertLatest(error);
}

} // namespace wimforge
