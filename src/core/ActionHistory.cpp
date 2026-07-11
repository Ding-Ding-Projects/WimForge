#include "ActionHistory.h"

#include "GitHistory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace wimforge {
namespace {

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

QString hashEvent(const ActionEvent &event)
{
    const QByteArray bytes = QJsonDocument(event.toJson(false)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool writeJournal(const QString &path, const QList<ActionEvent> &events, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not open the action journal: %1").arg(file.errorString()));
        return false;
    }

    for (const ActionEvent &event : events) {
        const QByteArray line = QJsonDocument(event.toJson()).toJson(QJsonDocument::Compact) + '\n';
        if (file.write(line) != line.size()) {
            setError(error, QStringLiteral("Could not write the action journal: %1").arg(file.errorString()));
            file.cancelWriting();
            return false;
        }
    }

    if (!file.commit()) {
        setError(error, QStringLiteral("Could not atomically save the action journal: %1")
                            .arg(file.errorString()));
        return false;
    }
    return true;
}

const ActionEvent *findEvent(const QList<ActionEvent> &events, const QString &id)
{
    const auto found = std::find_if(events.cbegin(), events.cend(), [&id](const ActionEvent &event) {
        return event.id == id;
    });
    return found == events.cend() ? nullptr : &*found;
}

QString rootIdFor(const ActionEvent &event)
{
    if (event.type == ActionEventType::Action)
        return event.id;
    if (event.type == ActionEventType::Compensation)
        return event.rootActionId;
    return {};
}

QList<const ActionEvent *> chainFor(const QList<ActionEvent> &events, const QString &rootId)
{
    QList<const ActionEvent *> chain;
    for (const ActionEvent &event : events) {
        if ((event.type == ActionEventType::Action && event.id == rootId)
            || (event.type == ActionEventType::Compensation && event.rootActionId == rootId)) {
            chain.append(&event);
        }
    }
    return chain;
}

bool validLaneName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._/-]{0,63}$"));
    return expression.match(name).hasMatch() && !name.contains(QStringLiteral(".."))
        && !name.endsWith(QLatin1Char('/')) && !name.contains(QStringLiteral("//"));
}

QStringList pathsFromObject(const QJsonObject &object)
{
    QStringList result;
    const QJsonArray explicitPaths = object.value(QStringLiteral("paths")).toArray();
    for (const QJsonValue &path : explicitPaths) {
        if (path.isString() && !path.toString().trimmed().isEmpty())
            result.append(path.toString().trimmed());
    }

    const QString explicitPath = object.value(QStringLiteral("path")).toString().trimmed();
    if (!explicitPath.isEmpty())
        result.append(explicitPath);

    if (result.isEmpty()) {
        for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
            if (iterator.key() != QStringLiteral("before")
                && iterator.key() != QStringLiteral("after")
                && iterator.key() != QStringLiteral("summary")) {
                result.append(iterator.key());
            }
        }
    }
    result.removeDuplicates();
    return result;
}

QString jsonPointerChild(const QString &parent, QString key)
{
    key.replace(QLatin1Char('~'), QStringLiteral("~0"));
    key.replace(QLatin1Char('/'), QStringLiteral("~1"));
    return parent + QLatin1Char('/') + key;
}

QJsonObject createMergePatchObject(const QJsonObject &before, const QJsonObject &after)
{
    QJsonObject patch;

    for (auto iterator = before.constBegin(); iterator != before.constEnd(); ++iterator) {
        if (!after.contains(iterator.key()))
            patch.insert(iterator.key(), QJsonValue::Null);
    }

    for (auto iterator = after.constBegin(); iterator != after.constEnd(); ++iterator) {
        const QString &key = iterator.key();
        const QJsonValue afterValue = iterator.value();
        const bool existedBefore = before.contains(key);
        const QJsonValue beforeValue = before.value(key);

        if (existedBefore && beforeValue == afterValue)
            continue;

        if (afterValue.isObject()) {
            const bool wasObject = existedBefore && beforeValue.isObject();
            const QJsonObject nestedPatch = createMergePatchObject(
                wasObject ? beforeValue.toObject() : QJsonObject{}, afterValue.toObject());

            // An empty object is still a meaningful replacement when the old
            // value was absent or was not an object.
            if (!nestedPatch.isEmpty() || !wasObject)
                patch.insert(key, nestedPatch);
        } else if (!afterValue.isNull() || existedBefore) {
            // RFC 7396 reserves null for deletion. A null target member that
            // was already absent therefore needs no patch entry.
            patch.insert(key, afterValue);
        }
    }

    return patch;
}

void collectMergePatchConflicts(const QJsonObject &state,
                                const QJsonObject &patch,
                                const QJsonObject &expectedPatch,
                                const QString &parentPath,
                                QStringList *conflicts)
{
    for (auto iterator = patch.constBegin(); iterator != patch.constEnd(); ++iterator) {
        const QString &key = iterator.key();
        const QString path = jsonPointerChild(parentPath, key);
        if (!expectedPatch.contains(key)) {
            conflicts->append(path);
            continue;
        }

        const QJsonValue change = iterator.value();
        const QJsonValue expected = expectedPatch.value(key);
        const bool exists = state.contains(key);
        const QJsonValue current = state.value(key);

        // When both patches merge an existing object, only their leaf paths
        // are changed and therefore only those leaves are guarded. A later
        // edit to an unrelated sibling remains intact.
        if (change.isObject() && expected.isObject()) {
            if (!exists || !current.isObject()) {
                conflicts->append(path);
                continue;
            }
            collectMergePatchConflicts(current.toObject(),
                                       change.toObject(),
                                       expected.toObject(),
                                       path,
                                       conflicts);
            continue;
        }

        // At a replacement/removal boundary the complete current value must
        // still match. In particular, expected null means the member must be
        // absent; a stored JSON null is not equivalent to absence here.
        const bool matches = expected.isNull() ? !exists : (exists && current == expected);
        if (!matches)
            conflicts->append(path);
    }
}

ActionEventType typeFromName(const QString &name, bool *ok)
{
    if (name == QStringLiteral("action")) {
        *ok = true;
        return ActionEventType::Action;
    }
    if (name == QStringLiteral("compensation")) {
        *ok = true;
        return ActionEventType::Compensation;
    }
    if (name == QStringLiteral("bookmark")) {
        *ok = true;
        return ActionEventType::Bookmark;
    }
    if (name == QStringLiteral("branch")) {
        *ok = true;
        return ActionEventType::Branch;
    }
    *ok = false;
    return ActionEventType::Action;
}

} // namespace

QString actionEventTypeName(ActionEventType type)
{
    switch (type) {
    case ActionEventType::Action:
        return QStringLiteral("action");
    case ActionEventType::Compensation:
        return QStringLiteral("compensation");
    case ActionEventType::Bookmark:
        return QStringLiteral("bookmark");
    case ActionEventType::Branch:
        return QStringLiteral("branch");
    }
    return QStringLiteral("action");
}

QStringList ActionEvent::changedPaths() const
{
    QStringList result = pathsFromObject(forwardDiff);
    result.append(pathsFromObject(inverseDiff));
    result.removeDuplicates();
    return result;
}

QString ActionEvent::diffSummary() const
{
    const QString summary = metadata.value(QStringLiteral("diffSummary")).toString().trimmed();
    if (!summary.isEmpty())
        return summary;
    const QStringList paths = changedPaths();
    if (paths.isEmpty())
        return description;
    if (paths.size() == 1)
        return QStringLiteral("Changed %1").arg(paths.first());
    return QStringLiteral("Changed %1 fields").arg(paths.size());
}

QJsonObject ActionEvent::toJson(bool includeHash) const
{
    QJsonObject json{
        {QStringLiteral("schemaVersion"), schemaVersion},
        {QStringLiteral("sequence"), sequence},
        {QStringLiteral("id"), id},
        {QStringLiteral("timestamp"), timestamp.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("type"), actionEventTypeName(type)},
        {QStringLiteral("title"), title},
        {QStringLiteral("description"), description},
        {QStringLiteral("icon"), icon},
        {QStringLiteral("contextKey"), contextKey},
        {QStringLiteral("elementId"), elementId},
        {QStringLiteral("branch"), branch},
        {QStringLiteral("destructive"), destructive},
        {QStringLiteral("forwardDiff"), forwardDiff},
        {QStringLiteral("inverseDiff"), inverseDiff},
        {QStringLiteral("metadata"), metadata},
        {QStringLiteral("targetEventId"), targetEventId},
        {QStringLiteral("rootActionId"), rootActionId},
        {QStringLiteral("previousHash"), previousHash},
    };
    if (includeHash)
        json.insert(QStringLiteral("eventHash"), eventHash);
    return json;
}

QVariantMap ActionEvent::toVariantMap(bool effective) const
{
    QVariantMap result = toJson().toVariantMap();
    result.insert(QStringLiteral("effective"), effective);
    const bool toggleable = isAction() || isCompensation();
    result.insert(QStringLiteral("canUndo"), toggleable && effective);
    result.insert(QStringLiteral("canRedo"), toggleable && !effective);
    result.insert(QStringLiteral("changedPaths"), changedPaths());
    result.insert(QStringLiteral("diffSummary"), diffSummary());
    return result;
}

bool ActionEvent::fromJson(const QJsonObject &json, ActionEvent *event, QString *error)
{
    if (!event) {
        setError(error, QStringLiteral("Action event output is null."));
        return false;
    }

    bool typeOk = false;
    const ActionEventType parsedType = typeFromName(json.value(QStringLiteral("type")).toString(), &typeOk);
    ActionEvent parsed;
    parsed.schemaVersion = json.value(QStringLiteral("schemaVersion")).toInt();
    parsed.sequence = json.value(QStringLiteral("sequence")).toInteger();
    parsed.id = json.value(QStringLiteral("id")).toString();
    parsed.timestamp = QDateTime::fromString(json.value(QStringLiteral("timestamp")).toString(),
                                             Qt::ISODateWithMs);
    parsed.type = parsedType;
    parsed.title = json.value(QStringLiteral("title")).toString();
    parsed.description = json.value(QStringLiteral("description")).toString();
    parsed.icon = json.value(QStringLiteral("icon")).toString();
    parsed.contextKey = json.value(QStringLiteral("contextKey")).toString();
    parsed.elementId = json.value(QStringLiteral("elementId")).toString();
    parsed.branch = json.value(QStringLiteral("branch")).toString();
    parsed.destructive = json.value(QStringLiteral("destructive")).toBool();
    parsed.forwardDiff = json.value(QStringLiteral("forwardDiff")).toObject();
    parsed.inverseDiff = json.value(QStringLiteral("inverseDiff")).toObject();
    parsed.metadata = json.value(QStringLiteral("metadata")).toObject();
    parsed.targetEventId = json.value(QStringLiteral("targetEventId")).toString();
    parsed.rootActionId = json.value(QStringLiteral("rootActionId")).toString();
    parsed.previousHash = json.value(QStringLiteral("previousHash")).toString();
    parsed.eventHash = json.value(QStringLiteral("eventHash")).toString();

    if (parsed.schemaVersion != ActionHistory::CurrentSchemaVersion || parsed.sequence < 1
        || parsed.id.trimmed().isEmpty() || !parsed.timestamp.isValid() || !typeOk
        || parsed.branch.trimmed().isEmpty() || parsed.eventHash.size() != 64) {
        setError(error, QStringLiteral("Action event has missing or unsupported required fields."));
        return false;
    }
    if (parsed.type == ActionEventType::Action && parsed.title.trimmed().isEmpty()) {
        setError(error, QStringLiteral("An action event must have a title."));
        return false;
    }
    if (parsed.type == ActionEventType::Compensation
        && (parsed.targetEventId.isEmpty() || parsed.rootActionId.isEmpty())) {
        setError(error, QStringLiteral("A compensation event must identify its target and root action."));
        return false;
    }

    *event = std::move(parsed);
    setError(error, {});
    return true;
}

ActionHistory::ActionHistory(QString projectDirectory)
{
    m_projectDirectory = projectDirectory.trimmed().isEmpty()
        ? QString()
        : QDir(projectDirectory).absolutePath();
}

QString ActionHistory::projectDirectory() const
{
    return m_projectDirectory;
}

QString ActionHistory::journalPath() const
{
    return QDir(m_projectDirectory).filePath(QString::fromLatin1(RelativeJournalPath));
}

bool ActionHistory::initialize(QString *error) const
{
    if (m_projectDirectory.isEmpty() || !QFileInfo(m_projectDirectory).isAbsolute()) {
        setError(error, QStringLiteral("Project directory must be an absolute path."));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(journalPath()).absolutePath())) {
        setError(error, QStringLiteral("Could not create the project history folder."));
        return false;
    }
    GitHistory git(m_projectDirectory, {QString::fromLatin1(RelativeJournalPath)});
    return git.initialize(error);
}

QList<ActionEvent> ActionHistory::loadAll(QString *error) const
{
    QList<ActionEvent> result;
    QFile file(journalPath());
    if (!file.exists()) {
        setError(error, {});
        return result;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not read the action journal: %1").arg(file.errorString()));
        return {};
    }

    QString expectedPreviousHash;
    QSet<QString> ids;
    qint64 expectedSequence = 1;
    qint64 lineNumber = 0;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        ++lineNumber;
        if (line.trimmed().isEmpty())
            continue;

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            setError(error, QStringLiteral("Action journal line %1 is invalid JSON: %2")
                                .arg(lineNumber)
                                .arg(parseError.errorString()));
            return {};
        }

        ActionEvent event;
        QString eventError;
        if (!ActionEvent::fromJson(document.object(), &event, &eventError)) {
            setError(error, QStringLiteral("Action journal line %1 is invalid: %2")
                                .arg(lineNumber)
                                .arg(eventError));
            return {};
        }
        if (event.sequence != expectedSequence || event.previousHash != expectedPreviousHash
            || ids.contains(event.id) || hashEvent(event) != event.eventHash) {
            setError(error, QStringLiteral("Action journal integrity check failed at line %1.")
                                .arg(lineNumber));
            return {};
        }

        ids.insert(event.id);
        expectedPreviousHash = event.eventHash;
        ++expectedSequence;
        result.append(std::move(event));
    }

    setError(error, {});
    return result;
}

bool ActionHistory::append(ActionEvent event, ActionEvent *createdEvent, QString *error) const
{
    if (!initialize(error))
        return false;

    QLockFile lock(QDir(m_projectDirectory).filePath(QStringLiteral(".wimforge/action-history.lock")));
    lock.setStaleLockTime(30'000);
    if (!lock.tryLock(10'000)) {
        setError(error, QStringLiteral("Action history is busy. Try again in a moment."));
        return false;
    }

    QString loadError;
    QList<ActionEvent> all = loadAll(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }

    event.schemaVersion = CurrentSchemaVersion;
    event.sequence = all.size() + 1;
    event.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    event.timestamp = QDateTime::currentDateTimeUtc();
    if (event.branch.trimmed().isEmpty())
        event.branch = all.isEmpty() ? QStringLiteral("main") : all.constLast().branch;
    event.previousHash = all.isEmpty() ? QString() : all.constLast().eventHash;
    event.eventHash = hashEvent(event);
    all.append(event);

    const QString path = journalPath();
    QByteArray previousBytes;
    QFile previous(path);
    if (previous.open(QIODevice::ReadOnly)) {
        previousBytes = previous.readAll();
        previous.close();
    }

    if (!writeJournal(path, all, error))
        return false;

    GitHistory git(m_projectDirectory, {QString::fromLatin1(RelativeJournalPath)});
    const QString commitTitle = event.title.isEmpty()
        ? actionEventTypeName(event.type) : event.title;
    QString commitTitleEn = commitTitle;
    QString commitTitleZh = QStringLiteral("記錄咗呢個動作");
    const qsizetype bilingualSeparator = commitTitle.indexOf(QStringLiteral(" / "));
    if (bilingualSeparator > 0
        && bilingualSeparator + 3 < commitTitle.size()) {
        commitTitleEn = commitTitle.left(bilingualSeparator).trimmed();
        commitTitleZh = commitTitle.mid(bilingualSeparator + 3).trimmed();
    }
    const QString commitMessage = QStringLiteral("History #%1: %2 / 歷程 #%1：%3")
                                      .arg(event.sequence)
                                      .arg(commitTitleEn)
                                      .arg(commitTitleZh);
    QString commitError;
    if (!git.commit(commitMessage, &commitError)) {
        QSaveFile rollback(path);
        if (rollback.open(QIODevice::WriteOnly)) {
            rollback.write(previousBytes);
            rollback.commit();
        }
        setError(error, QStringLiteral("The action was not recorded because its Git commit failed: %1")
                            .arg(commitError));
        return false;
    }

    if (createdEvent)
        *createdEvent = event;
    setError(error, {});
    return true;
}

bool ActionHistory::record(const ActionDraft &draft,
                           ActionEvent *createdEvent,
                           QString *error) const
{
    if (draft.title.trimmed().isEmpty()) {
        setError(error, QStringLiteral("An undoable action needs a title."));
        return false;
    }
    if (draft.contextKey.trimmed().isEmpty() || draft.elementId.trimmed().isEmpty()) {
        setError(error, QStringLiteral("An undoable action needs a context key and element ID."));
        return false;
    }
    if (!draft.branch.isEmpty() && !validLaneName(draft.branch)) {
        setError(error, QStringLiteral("Branch name contains unsupported characters."));
        return false;
    }
    if (!draft.branch.isEmpty()) {
        QString branchError;
        const QStringList knownBranches = branchNames(&branchError);
        if (!branchError.isEmpty()) {
            setError(error, branchError);
            return false;
        }
        if (!knownBranches.contains(draft.branch)) {
            setError(error, QStringLiteral("Create the history branch before recording actions on it."));
            return false;
        }
    }

    ActionEvent event;
    event.type = ActionEventType::Action;
    event.title = draft.title.simplified();
    event.description = draft.description.trimmed();
    event.icon = draft.icon.trimmed().isEmpty() ? QStringLiteral("history") : draft.icon.trimmed();
    event.contextKey = draft.contextKey.trimmed();
    event.elementId = draft.elementId.trimmed();
    event.forwardDiff = draft.forwardDiff;
    event.inverseDiff = draft.inverseDiff;
    event.metadata = draft.metadata;
    event.branch = draft.branch.trimmed();
    event.destructive = draft.destructive;
    return append(std::move(event), createdEvent, error);
}

bool ActionHistory::toggle(const QString &eventOrActionId,
                           bool requireCurrentlyEffective,
                           const QString &operation,
                           ActionEvent *createdEvent,
                           QString *error) const
{
    QString loadError;
    const QList<ActionEvent> all = loadAll(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    const ActionEvent *selected = findEvent(all, eventOrActionId);
    if (!selected) {
        setError(error, QStringLiteral("The selected history event no longer exists."));
        return false;
    }
    const QString rootId = rootIdFor(*selected);
    const QList<const ActionEvent *> chain = chainFor(all, rootId);
    if (rootId.isEmpty() || chain.isEmpty()) {
        setError(error, QStringLiteral("Only actions and their undo events can be toggled."));
        return false;
    }

    const bool effective = (chain.size() % 2) == 1;
    if (effective != requireCurrentlyEffective) {
        setError(error, requireCurrentlyEffective
                            ? QStringLiteral("That action is already undone; use redo.")
                            : QStringLiteral("That action is already active; use undo."));
        return false;
    }

    const ActionEvent &root = *chain.first();
    const ActionEvent &tail = *chain.last();
    ActionEvent compensation;
    compensation.type = ActionEventType::Compensation;
    const bool redo = operation == QStringLiteral("redo");
    QString rootTitleEn = root.title;
    QString rootTitleZh = QStringLiteral("嗰個動作");
    const qsizetype rootTitleSeparator = root.title.indexOf(QStringLiteral(" / "));
    if (rootTitleSeparator > 0
        && rootTitleSeparator + 3 < root.title.size()) {
        rootTitleEn = root.title.left(rootTitleSeparator).trimmed();
        rootTitleZh = root.title.mid(rootTitleSeparator + 3).trimmed();
    }
    compensation.title = QStringLiteral("%1 %2 / %3 %4")
                             .arg(redo ? QStringLiteral("Redo") : QStringLiteral("Undo"),
                                  rootTitleEn,
                                  redo ? QStringLiteral("重做") : QStringLiteral("復原"),
                                  rootTitleZh);
    compensation.description = QStringLiteral(
        "Compensates history event #%1. / 補償歷程事件 #%1。").arg(tail.sequence);
    compensation.icon = operation;
    compensation.contextKey = root.contextKey;
    compensation.elementId = root.elementId;
    // Selective undo can target an action from another lane, but it is itself
    // an action on the lane the user is currently viewing.
    compensation.branch = all.isEmpty() ? QStringLiteral("main") : all.constLast().branch;
    compensation.forwardDiff = tail.inverseDiff;
    compensation.inverseDiff = tail.forwardDiff;
    compensation.targetEventId = tail.id;
    compensation.rootActionId = root.id;
    compensation.metadata = QJsonObject{
        {QStringLiteral("operation"), operation},
        {QStringLiteral("targetSequence"), tail.sequence},
        {QStringLiteral("targetTitle"), tail.title},
        {QStringLiteral("diffSummary"), root.diffSummary()},
    };
    if (root.metadata.contains(QStringLiteral("stateFormat")))
        compensation.metadata.insert(QStringLiteral("stateFormat"),
                                     root.metadata.value(QStringLiteral("stateFormat")));
    if (root.metadata.contains(QStringLiteral("fullProjectState")))
        compensation.metadata.insert(QStringLiteral("fullProjectState"),
                                     root.metadata.value(QStringLiteral("fullProjectState")));
    return append(std::move(compensation), createdEvent, error);
}

bool ActionHistory::undoAction(const QString &eventOrActionId,
                               ActionEvent *createdEvent,
                               QString *error) const
{
    return toggle(eventOrActionId, true, QStringLiteral("undo"), createdEvent, error);
}

bool ActionHistory::redoAction(const QString &eventOrActionId,
                               ActionEvent *createdEvent,
                               QString *error) const
{
    return toggle(eventOrActionId, false, QStringLiteral("redo"), createdEvent, error);
}

bool ActionHistory::compensateEvent(const QString &eventId,
                                    ActionEvent *createdEvent,
                                    QString *error) const
{
    QString loadError;
    const QList<ActionEvent> all = loadAll(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    const ActionEvent *selected = findEvent(all, eventId);
    if (!selected) {
        setError(error, QStringLiteral("The selected history event no longer exists."));
        return false;
    }
    const QString rootId = rootIdFor(*selected);
    const QList<const ActionEvent *> chain = chainFor(all, rootId);
    if (chain.isEmpty() || chain.constLast()->id != eventId) {
        setError(error, QStringLiteral("Only the latest event in an action chain can be compensated."));
        return false;
    }
    const bool effective = (chain.size() % 2) == 1;
    return toggle(eventId,
                  effective,
                  effective ? QStringLiteral("undo") : QStringLiteral("redo"),
                  createdEvent,
                  error);
}

bool ActionHistory::createBookmark(const QString &name,
                                   const QString &eventId,
                                   ActionEvent *createdEvent,
                                   QString *error) const
{
    const QString cleanName = name.simplified();
    if (cleanName.isEmpty() || cleanName.size() > 80) {
        setError(error, QStringLiteral("Bookmark name must contain 1 to 80 characters."));
        return false;
    }

    QString loadError;
    const QList<ActionEvent> all = loadAll(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    const ActionEvent *target = eventId.isEmpty() ? (all.isEmpty() ? nullptr : &all.constLast())
                                                  : findEvent(all, eventId);
    if (!eventId.isEmpty() && !target) {
        setError(error, QStringLiteral("The bookmarked event no longer exists."));
        return false;
    }

    ActionEvent bookmark;
    bookmark.type = ActionEventType::Bookmark;
    bookmark.title = QStringLiteral("Bookmark: %1 / 書籤：%1").arg(cleanName);
    bookmark.icon = QStringLiteral("bookmark");
    bookmark.contextKey = target ? target->contextKey : QStringLiteral("project");
    bookmark.elementId = target ? target->elementId : QStringLiteral("root");
    bookmark.branch = all.isEmpty() ? QStringLiteral("main") : all.constLast().branch;
    bookmark.targetEventId = target ? target->id : QString();
    bookmark.metadata = QJsonObject{{QStringLiteral("name"), cleanName}};
    return append(std::move(bookmark), createdEvent, error);
}

bool ActionHistory::createBranch(const QString &name,
                                 const QString &fromEventId,
                                 ActionEvent *createdEvent,
                                 QString *error) const
{
    const QString cleanName = name.trimmed();
    if (!validLaneName(cleanName)) {
        setError(error, QStringLiteral("Branch names use letters, numbers, '.', '_', '-', and '/'."));
        return false;
    }
    QString namesError;
    if (branchNames(&namesError).contains(cleanName)) {
        setError(error, QStringLiteral("That history branch already exists."));
        return false;
    }
    if (!namesError.isEmpty()) {
        setError(error, namesError);
        return false;
    }

    QString loadError;
    const QList<ActionEvent> all = loadAll(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    const ActionEvent *target = fromEventId.isEmpty() ? (all.isEmpty() ? nullptr : &all.constLast())
                                                      : findEvent(all, fromEventId);
    if (!fromEventId.isEmpty() && !target) {
        setError(error, QStringLiteral("The branch point no longer exists."));
        return false;
    }

    ActionEvent branch;
    branch.type = ActionEventType::Branch;
    branch.title = QStringLiteral("Create branch %1 / 建立分支 %1").arg(cleanName);
    branch.icon = QStringLiteral("fork_right");
    branch.contextKey = target ? target->contextKey : QStringLiteral("project");
    branch.elementId = target ? target->elementId : QStringLiteral("root");
    branch.branch = cleanName;
    branch.targetEventId = target ? target->id : QString();
    branch.metadata = QJsonObject{
        {QStringLiteral("operation"), QStringLiteral("create")},
        {QStringLiteral("fromBranch"), target ? target->branch : QStringLiteral("main")},
    };
    return append(std::move(branch), createdEvent, error);
}

bool ActionHistory::switchBranch(const QString &name,
                                 ActionEvent *createdEvent,
                                 QString *error) const
{
    const QString cleanName = name.trimmed();
    QString namesError;
    if (!branchNames(&namesError).contains(cleanName)) {
        setError(error, namesError.isEmpty() ? QStringLiteral("That history branch does not exist.")
                                             : namesError);
        return false;
    }

    ActionEvent branch;
    branch.type = ActionEventType::Branch;
    branch.title = QStringLiteral("Switch to branch %1 / 切換去分支 %1").arg(cleanName);
    branch.icon = QStringLiteral("fork_right");
    branch.contextKey = QStringLiteral("history");
    branch.elementId = QStringLiteral("branch-selector");
    branch.branch = cleanName;
    branch.metadata = QJsonObject{{QStringLiteral("operation"), QStringLiteral("switch")}};
    return append(std::move(branch), createdEvent, error);
}

QList<ActionEvent> ActionHistory::events(int maximumCount, QString *error) const
{
    QList<ActionEvent> result = loadAll(error);
    if (error && !error->isEmpty())
        return {};
    maximumCount = qBound(1, maximumCount, 2'000);
    if (result.size() > maximumCount)
        result = result.mid(result.size() - maximumCount);
    std::reverse(result.begin(), result.end());
    return result;
}

QList<ActionEvent> ActionHistory::recentForElement(const QString &contextKey,
                                                   const QString &elementId,
                                                   int maximumCount,
                                                   QString *error) const
{
    const QList<ActionEvent> all = loadAll(error);
    if (error && !error->isEmpty())
        return {};
    maximumCount = qBound(1, maximumCount, 500);
    QList<ActionEvent> result;
    for (auto iterator = all.crbegin(); iterator != all.crend() && result.size() < maximumCount;
         ++iterator) {
        if (iterator->contextKey == contextKey && iterator->elementId == elementId)
            result.append(*iterator);
    }
    return result;
}

bool ActionHistory::isEffective(const QString &eventOrActionId, QString *error) const
{
    const QList<ActionEvent> all = loadAll(error);
    if (error && !error->isEmpty())
        return false;
    const ActionEvent *event = findEvent(all, eventOrActionId);
    if (!event) {
        setError(error, QStringLiteral("The selected history event no longer exists."));
        return false;
    }
    const QList<const ActionEvent *> chain = chainFor(all, rootIdFor(*event));
    if (chain.isEmpty()) {
        setError(error, QStringLiteral("The selected event is not an undoable action."));
        return false;
    }
    setError(error, {});
    return (chain.size() % 2) == 1;
}

QString ActionHistory::currentBranch(QString *error) const
{
    const QList<ActionEvent> all = loadAll(error);
    if (error && !error->isEmpty())
        return {};
    return all.isEmpty() ? QStringLiteral("main") : all.constLast().branch;
}

QStringList ActionHistory::branchNames(QString *error) const
{
    const QList<ActionEvent> all = loadAll(error);
    if (error && !error->isEmpty())
        return {};
    QStringList result{QStringLiteral("main")};
    for (const ActionEvent &event : all) {
        if (!event.branch.isEmpty() && !result.contains(event.branch))
            result.append(event.branch);
    }
    setError(error, {});
    return result;
}

QJsonObject ActionHistory::applyMergePatch(QJsonObject state, const QJsonObject &patch)
{
    for (auto iterator = patch.constBegin(); iterator != patch.constEnd(); ++iterator) {
        if (iterator.value().isNull()) {
            state.remove(iterator.key());
        } else if (iterator.value().isObject() && state.value(iterator.key()).isObject()) {
            state.insert(iterator.key(),
                         applyMergePatch(state.value(iterator.key()).toObject(),
                                         iterator.value().toObject()));
        } else {
            state.insert(iterator.key(), iterator.value());
        }
    }
    return state;
}

QJsonObject ActionHistory::createMergePatch(const QJsonObject &before, const QJsonObject &after)
{
    return createMergePatchObject(before, after);
}

MergePatchApplyResult ActionHistory::applyMergePatchGuarded(const QJsonObject &state,
                                                            const QJsonObject &patch,
                                                            const QJsonObject &expectedPatch)
{
    MergePatchApplyResult result;
    result.state = state;
    collectMergePatchConflicts(state, patch, expectedPatch, {}, &result.conflicts);
    result.conflicts.removeDuplicates();
    if (!result.conflicts.isEmpty())
        return result;

    result.state = applyMergePatch(state, patch);
    result.applied = true;
    return result;
}

} // namespace wimforge
