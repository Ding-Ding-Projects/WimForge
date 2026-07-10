#include "NotificationStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace wimforge {
namespace {

constexpr auto StateFileName = "notifications.json";
constexpr auto EventFileName = "events.jsonl";
constexpr auto StoreSchema = "wimforge.notifications";
constexpr int StoreVersion = 1;

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

QString timestamp(const QDateTime &value)
{
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime parseTimestamp(const QJsonObject &object,
                         const QString &key,
                         QStringList *errors,
                         bool required)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined() && !required)
        return {};
    if (!value.isString()) {
        errors->append(QStringLiteral("'%1' must be an ISO-8601 timestamp.").arg(key));
        return {};
    }
    if (!required && value.toString().trimmed().isEmpty())
        return {};
    const QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
    if (!parsed.isValid())
        errors->append(QStringLiteral("'%1' is not a valid ISO-8601 timestamp.").arg(key));
    return parsed;
}

QJsonObject notificationJson(const Notification &notification)
{
    return QJsonObject{
        {QStringLiteral("id"), notification.id},
        {QStringLiteral("title"), notification.title},
        {QStringLiteral("message"), notification.message},
        {QStringLiteral("severity"), notification.severity},
        {QStringLiteral("source"), notification.source},
        {QStringLiteral("createdAt"), timestamp(notification.createdAt)},
        {QStringLiteral("updatedAt"), timestamp(notification.updatedAt)},
        {QStringLiteral("read"), notification.isRead},
        {QStringLiteral("dismissed"), notification.isDismissed},
        {QStringLiteral("deleted"), notification.isDeleted},
        {QStringLiteral("deletedAt"), timestamp(notification.deletedAt)},
        {QStringLiteral("data"), notification.data},
    };
}

std::optional<Notification> parseNotification(const QJsonObject &object,
                                              const QString &context,
                                              QStringList *errors)
{
    auto stringField = [&object, errors, &context](const QString &key, bool required = false) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined() && !required)
            return QString();
        if (!value.isString()) {
            errors->append(QStringLiteral("%1.%2 must be a string.").arg(context, key));
            return QString();
        }
        return value.toString();
    };
    auto boolField = [&object, errors, &context](const QString &key) {
        const QJsonValue value = object.value(key);
        if (!value.isBool()) {
            errors->append(QStringLiteral("%1.%2 must be true or false.").arg(context, key));
            return false;
        }
        return value.toBool();
    };

    Notification notification;
    notification.id = stringField(QStringLiteral("id"), true);
    notification.title = stringField(QStringLiteral("title"), true);
    notification.message = stringField(QStringLiteral("message"));
    notification.severity = stringField(QStringLiteral("severity"));
    notification.source = stringField(QStringLiteral("source"));
    notification.createdAt = parseTimestamp(object, QStringLiteral("createdAt"), errors, true);
    notification.updatedAt = parseTimestamp(object, QStringLiteral("updatedAt"), errors, true);
    notification.isRead = boolField(QStringLiteral("read"));
    notification.isDismissed = boolField(QStringLiteral("dismissed"));
    notification.isDeleted = boolField(QStringLiteral("deleted"));
    notification.deletedAt = parseTimestamp(object, QStringLiteral("deletedAt"), errors, false);

    const QJsonValue data = object.value(QStringLiteral("data"));
    if (!data.isUndefined() && !data.isObject())
        errors->append(QStringLiteral("%1.data must be an object.").arg(context));
    else
        notification.data = data.toObject();

    return notification;
}

QJsonObject stateJson(const QList<Notification> &notifications)
{
    QJsonArray items;
    for (const Notification &notification : notifications)
        items.append(notificationJson(notification));
    return QJsonObject{
        {QStringLiteral("schema"), QString::fromLatin1(StoreSchema)},
        {QStringLiteral("version"), StoreVersion},
        {QStringLiteral("notifications"), items},
    };
}

bool writeBytes(const QString &filePath, const QByteArray &bytes, QString *error)
{
    if (!QDir().mkpath(QFileInfo(filePath).absolutePath())) {
        setError(error, QStringLiteral("Could not create notification folder: %1")
                            .arg(QFileInfo(filePath).absolutePath()));
        return false;
    }
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not open %1: %2").arg(filePath, file.errorString()));
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
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

bool writeState(const QString &filePath, const QList<Notification> &notifications, QString *error)
{
    return writeBytes(filePath, QJsonDocument(stateJson(notifications)).toJson(QJsonDocument::Indented), error);
}

std::optional<QList<Notification>> readState(const QString &filePath, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open notification state: %1").arg(file.errorString()));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("notifications.json is invalid at offset %1: %2")
                            .arg(parseError.offset)
                            .arg(parseError.errorString()));
        return std::nullopt;
    }

    const QJsonObject root = document.object();
    QStringList errors;
    if (root.value(QStringLiteral("schema")).toString() != QString::fromLatin1(StoreSchema))
        errors.append(QStringLiteral("Unsupported notification store schema."));
    if (root.value(QStringLiteral("version")).toInt(-1) != StoreVersion)
        errors.append(QStringLiteral("Unsupported notification store version."));
    if (!root.value(QStringLiteral("notifications")).isArray())
        errors.append(QStringLiteral("'notifications' must be an array."));

    QList<Notification> notifications;
    QSet<QString> ids;
    const QJsonArray array = root.value(QStringLiteral("notifications")).toArray();
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (!array.at(index).isObject()) {
            errors.append(QStringLiteral("notifications[%1] must be an object.").arg(index));
            continue;
        }
        std::optional<Notification> notification = parseNotification(
            array.at(index).toObject(), QStringLiteral("notifications[%1]").arg(index), &errors);
        if (!notification)
            continue;
        if (notification->id.isEmpty())
            errors.append(QStringLiteral("notifications[%1].id cannot be empty.").arg(index));
        else if (ids.contains(notification->id))
            errors.append(QStringLiteral("Duplicate notification id: %1").arg(notification->id));
        else
            ids.insert(notification->id);
        notifications.append(std::move(*notification));
    }

    if (!errors.isEmpty()) {
        setError(error, errors.join(QLatin1Char('\n')));
        return std::nullopt;
    }
    setError(error, {});
    return notifications;
}

QJsonObject eventJson(const NotificationEvent &event)
{
    return QJsonObject{
        {QStringLiteral("eventId"), event.eventId},
        {QStringLiteral("notificationId"), event.notificationId},
        {QStringLiteral("action"), event.action},
        {QStringLiteral("occurredAt"), timestamp(event.occurredAt)},
        {QStringLiteral("details"), event.details},
    };
}

bool appendEvent(const QString &filePath, const NotificationEvent &event, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        setError(error, QStringLiteral("Could not append notification event: %1").arg(file.errorString()));
        return false;
    }
    QByteArray line = QJsonDocument(eventJson(event)).toJson(QJsonDocument::Compact);
    line.append('\n');
    if (file.write(line) != line.size() || !file.flush()) {
        setError(error, QStringLiteral("Could not append notification event: %1").arg(file.errorString()));
        return false;
    }
    setError(error, {});
    return true;
}

QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString cleanSeverity(QString severity)
{
    severity = severity.trimmed().toLower();
    static const QSet<QString> allowed{
        QStringLiteral("info"), QStringLiteral("success"), QStringLiteral("warning"),
        QStringLiteral("error"), QStringLiteral("progress")};
    return allowed.contains(severity) ? severity : QString();
}

} // namespace

NotificationStore::NotificationStore(QString storeDirectory)
    : m_storeDirectory(storeDirectory.trimmed().isEmpty()
          ? defaultStoreDirectory()
          : QDir(storeDirectory).absolutePath())
{
}

QString NotificationStore::defaultStoreDirectory()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.trimmed().isEmpty())
        root = QDir::home().filePath(QStringLiteral(".wimforge"));
    return QDir(root).filePath(QStringLiteral("notification-center"));
}

QString NotificationStore::storeDirectory() const
{
    return m_storeDirectory;
}

QString NotificationStore::stateFilePath() const
{
    return QDir(m_storeDirectory).filePath(QString::fromLatin1(StateFileName));
}

QString NotificationStore::eventFilePath() const
{
    return QDir(m_storeDirectory).filePath(QString::fromLatin1(EventFileName));
}

bool NotificationStore::initialize(QString *error) const
{
    if (QFileInfo::exists(QDir(m_storeDirectory).filePath(QStringLiteral("project.json")))) {
        setError(error, QStringLiteral("Notification history needs its own folder, not a project folder."));
        return false;
    }

    GitHistory git(m_storeDirectory,
                   {QString::fromLatin1(StateFileName), QString::fromLatin1(EventFileName)});
    if (!git.initialize(error))
        return false;

    bool created = false;
    if (!QFileInfo::exists(stateFilePath())) {
        if (!writeState(stateFilePath(), {}, error))
            return false;
        created = true;
    }
    if (!QFileInfo::exists(eventFilePath())) {
        if (!writeBytes(eventFilePath(), {}, error))
            return false;
        created = true;
    }

    QString historyError;
    const QList<GitCommit> commits = git.history(1, &historyError);
    if (!historyError.isEmpty()) {
        setError(error, historyError);
        return false;
    }
    if (commits.isEmpty()) {
        if (!git.commit(QStringLiteral("Initialize notification center"), error))
            return false;
    } else if (created) {
        if (!git.commit(QStringLiteral("Repair notification center state files"), error))
            return false;
    }

    setError(error, {});
    return true;
}

QString NotificationStore::addNotification(const QString &title,
                                           const QString &message,
                                           const QString &severity,
                                           const QString &source,
                                           const QJsonObject &data,
                                           QString *error)
{
    if (title.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Notification title is required."));
        return {};
    }
    const QString normalizedSeverity = cleanSeverity(severity);
    if (normalizedSeverity.isEmpty()) {
        setError(error, QStringLiteral("Severity must be info, success, warning, error, or progress."));
        return {};
    }
    if (!initialize(error))
        return {};

    QLockFile lock(QDir(m_storeDirectory).filePath(QStringLiteral(".notification-store.lock")));
    lock.setStaleLockTime(30'000);
    if (!lock.tryLock(10'000)) {
        setError(error, QStringLiteral("Notification center is busy. Try again in a moment."));
        return {};
    }

    std::optional<QList<Notification>> state = readState(stateFilePath(), error);
    if (!state)
        return {};

    const QDateTime now = QDateTime::currentDateTimeUtc();
    Notification notification;
    notification.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    notification.title = title.trimmed();
    notification.message = message;
    notification.severity = normalizedSeverity;
    notification.source = source.trimmed();
    notification.createdAt = now;
    notification.updatedAt = now;
    notification.data = data;
    state->append(notification);

    const QByteArray previousState = fileBytes(stateFilePath());
    const QByteArray previousEvents = fileBytes(eventFilePath());
    NotificationEvent event{
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        notification.id,
        QStringLiteral("new"),
        now,
        QJsonObject{{QStringLiteral("severity"), normalizedSeverity}},
    };

    QString actionError;
    if (!writeState(stateFilePath(), *state, &actionError)
        || !appendEvent(eventFilePath(), event, &actionError)) {
        writeBytes(stateFilePath(), previousState, nullptr);
        writeBytes(eventFilePath(), previousEvents, nullptr);
        setError(error, actionError);
        return {};
    }

    GitHistory git(m_storeDirectory,
                   {QString::fromLatin1(StateFileName), QString::fromLatin1(EventFileName)});
    if (!git.commit(QStringLiteral("notifications: new %1").arg(notification.id), &actionError)) {
        writeBytes(stateFilePath(), previousState, nullptr);
        writeBytes(eventFilePath(), previousEvents, nullptr);
        setError(error, QStringLiteral("Notification was not recorded because its history commit failed: %1")
                            .arg(actionError));
        return {};
    }

    setError(error, {});
    return notification.id;
}

bool NotificationStore::mutate(const QString &id,
                               const QString &action,
                               const std::function<void(Notification &)> &change,
                               QString *error)
{
    if (id.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Notification id is required."));
        return false;
    }
    if (!initialize(error))
        return false;

    QLockFile lock(QDir(m_storeDirectory).filePath(QStringLiteral(".notification-store.lock")));
    lock.setStaleLockTime(30'000);
    if (!lock.tryLock(10'000)) {
        setError(error, QStringLiteral("Notification center is busy. Try again in a moment."));
        return false;
    }

    std::optional<QList<Notification>> state = readState(stateFilePath(), error);
    if (!state)
        return false;

    auto item = std::find_if(state->begin(), state->end(), [&id](const Notification &candidate) {
        return candidate.id == id;
    });
    if (item == state->end()) {
        setError(error, QStringLiteral("Notification '%1' was not found.").arg(id));
        return false;
    }
    if (item->isDeleted && action != QStringLiteral("restore") && action != QStringLiteral("delete")) {
        setError(error, QStringLiteral("Notification '%1' is deleted. Restore it before changing it.").arg(id));
        return false;
    }

    const bool beforeRead = item->isRead;
    const bool beforeDismissed = item->isDismissed;
    const bool beforeDeleted = item->isDeleted;
    change(*item);
    item->updatedAt = QDateTime::currentDateTimeUtc();

    const NotificationEvent event{
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        id,
        action,
        item->updatedAt,
        QJsonObject{
            {QStringLiteral("beforeRead"), beforeRead},
            {QStringLiteral("beforeDismissed"), beforeDismissed},
            {QStringLiteral("beforeDeleted"), beforeDeleted},
            {QStringLiteral("afterRead"), item->isRead},
            {QStringLiteral("afterDismissed"), item->isDismissed},
            {QStringLiteral("afterDeleted"), item->isDeleted},
        },
    };

    const QByteArray previousState = fileBytes(stateFilePath());
    const QByteArray previousEvents = fileBytes(eventFilePath());
    QString actionError;
    if (!writeState(stateFilePath(), *state, &actionError)
        || !appendEvent(eventFilePath(), event, &actionError)) {
        writeBytes(stateFilePath(), previousState, nullptr);
        writeBytes(eventFilePath(), previousEvents, nullptr);
        setError(error, actionError);
        return false;
    }

    GitHistory git(m_storeDirectory,
                   {QString::fromLatin1(StateFileName), QString::fromLatin1(EventFileName)});
    if (!git.commit(QStringLiteral("notifications: %1 %2").arg(action, id), &actionError)) {
        writeBytes(stateFilePath(), previousState, nullptr);
        writeBytes(eventFilePath(), previousEvents, nullptr);
        setError(error, QStringLiteral("Notification action was rolled back because its history commit failed: %1")
                            .arg(actionError));
        return false;
    }

    setError(error, {});
    return true;
}

bool NotificationStore::markRead(const QString &id, QString *error)
{
    return mutate(id, QStringLiteral("read"), [](Notification &item) { item.isRead = true; }, error);
}

bool NotificationStore::markUnread(const QString &id, QString *error)
{
    return mutate(id, QStringLiteral("unread"), [](Notification &item) { item.isRead = false; }, error);
}

bool NotificationStore::dismiss(const QString &id, QString *error)
{
    return mutate(id, QStringLiteral("dismiss"), [](Notification &item) { item.isDismissed = true; }, error);
}

bool NotificationStore::restore(const QString &id, QString *error)
{
    return mutate(id, QStringLiteral("restore"), [](Notification &item) {
        item.isDismissed = false;
        item.isDeleted = false;
        item.deletedAt = {};
    }, error);
}

bool NotificationStore::softDelete(const QString &id, QString *error)
{
    return mutate(id, QStringLiteral("delete"), [](Notification &item) {
        item.isDeleted = true;
        item.isDismissed = true;
        item.deletedAt = QDateTime::currentDateTimeUtc();
    }, error);
}

QList<Notification> NotificationStore::list(bool includeDismissed,
                                            bool includeDeleted,
                                            QString *error) const
{
    if (!initialize(error))
        return {};
    std::optional<QList<Notification>> state = readState(stateFilePath(), error);
    if (!state)
        return {};

    QList<Notification> result;
    for (const Notification &notification : *state) {
        if (!includeDeleted && notification.isDeleted)
            continue;
        if (!includeDismissed && notification.isDismissed)
            continue;
        result.append(notification);
    }
    std::sort(result.begin(), result.end(), [](const Notification &left, const Notification &right) {
        return left.createdAt > right.createdAt;
    });
    setError(error, {});
    return result;
}

std::optional<Notification> NotificationStore::find(const QString &id, QString *error) const
{
    const QList<Notification> all = list(true, true, error);
    if (error && !error->isEmpty())
        return std::nullopt;
    const auto found = std::find_if(all.cbegin(), all.cend(), [&id](const Notification &item) {
        return item.id == id;
    });
    if (found == all.cend()) {
        setError(error, QStringLiteral("Notification '%1' was not found.").arg(id));
        return std::nullopt;
    }
    setError(error, {});
    return *found;
}

QList<NotificationEvent> NotificationStore::events(int maximumCount, QString *error) const
{
    if (!initialize(error))
        return {};
    QFile file(eventFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open notification event history: %1").arg(file.errorString()));
        return {};
    }

    QList<NotificationEvent> result;
    QStringList errors;
    int lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            errors.append(QStringLiteral("events.jsonl line %1 is invalid: %2")
                              .arg(lineNumber).arg(parseError.errorString()));
            continue;
        }
        const QJsonObject object = document.object();
        const QDateTime occurredAt = QDateTime::fromString(
            object.value(QStringLiteral("occurredAt")).toString(), Qt::ISODate);
        if (!occurredAt.isValid()) {
            errors.append(QStringLiteral("events.jsonl line %1 has an invalid timestamp.").arg(lineNumber));
            continue;
        }
        result.append(NotificationEvent{
            object.value(QStringLiteral("eventId")).toString(),
            object.value(QStringLiteral("notificationId")).toString(),
            object.value(QStringLiteral("action")).toString(),
            occurredAt,
            object.value(QStringLiteral("details")).toObject(),
        });
    }

    if (!errors.isEmpty()) {
        setError(error, errors.join(QLatin1Char('\n')));
        return {};
    }
    std::reverse(result.begin(), result.end());
    maximumCount = qBound(0, maximumCount, 100'000);
    if (result.size() > maximumCount)
        result = result.mid(0, maximumCount);
    setError(error, {});
    return result;
}

QList<GitCommit> NotificationStore::history(int maximumCount, QString *error) const
{
    if (!initialize(error))
        return {};
    return GitHistory(m_storeDirectory,
                      {QString::fromLatin1(StateFileName), QString::fromLatin1(EventFileName)})
        .history(maximumCount, error);
}

bool NotificationStore::revertLatest(QString *error)
{
    if (!initialize(error))
        return false;
    QLockFile lock(QDir(m_storeDirectory).filePath(QStringLiteral(".notification-store.lock")));
    lock.setStaleLockTime(30'000);
    if (!lock.tryLock(10'000)) {
        setError(error, QStringLiteral("Notification center is busy. Try again in a moment."));
        return false;
    }

    GitHistory git(m_storeDirectory,
                   {QString::fromLatin1(StateFileName), QString::fromLatin1(EventFileName)});
    if (!git.revertLatest(error))
        return false;

    // Refuse to report success if the resulting state cannot be read.
    if (!readState(stateFilePath(), error))
        return false;
    setError(error, {});
    return true;
}

} // namespace wimforge
