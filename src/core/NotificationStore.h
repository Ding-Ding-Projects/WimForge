#pragma once

#include "GitHistory.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace wimforge {

struct Notification
{
    QString id;
    QString title;
    QString message;
    QString severity = QStringLiteral("info");
    QString source;
    QDateTime createdAt;
    QDateTime updatedAt;
    bool isRead = false;
    bool isDismissed = false;
    bool isDeleted = false; // tombstone: the record remains in notifications.json
    QDateTime deletedAt;
    QJsonObject data;
};

struct NotificationEvent
{
    QString eventId;
    QString notificationId;
    QString action;
    QDateTime occurredAt;
    QJsonObject details;
};

class NotificationStore
{
public:
    explicit NotificationStore(QString storeDirectory = defaultStoreDirectory());

    static QString defaultStoreDirectory();
    [[nodiscard]] QString storeDirectory() const;
    [[nodiscard]] QString stateFilePath() const;
    [[nodiscard]] QString eventFilePath() const;

    // Creates an empty, committed baseline when this is a new store.
    bool initialize(QString *error = nullptr) const;

    // Returns the generated stable id, or an empty string on failure.
    QString addNotification(const QString &title,
                            const QString &message,
                            const QString &severity = QStringLiteral("info"),
                            const QString &source = {},
                            const QJsonObject &data = {},
                            QString *error = nullptr);

    bool markRead(const QString &id, QString *error = nullptr);
    bool markUnread(const QString &id, QString *error = nullptr);
    bool dismiss(const QString &id, QString *error = nullptr);
    bool restore(const QString &id, QString *error = nullptr);
    bool softDelete(const QString &id, QString *error = nullptr);

    [[nodiscard]] QList<Notification> list(bool includeDismissed = false,
                                           bool includeDeleted = false,
                                           QString *error = nullptr) const;
    [[nodiscard]] std::optional<Notification> find(const QString &id,
                                                   QString *error = nullptr) const;
    [[nodiscard]] QList<NotificationEvent> events(int maximumCount = 1'000,
                                                  QString *error = nullptr) const;
    [[nodiscard]] QList<GitCommit> history(int maximumCount = 100,
                                           QString *error = nullptr) const;

    // Reverting the latest revert commit performs redo.
    bool revertLatest(QString *error = nullptr);

private:
    bool mutate(const QString &id,
                const QString &action,
                const std::function<void(Notification &)> &change,
                QString *error);

    QString m_storeDirectory;
};

} // namespace wimforge
