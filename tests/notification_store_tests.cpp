#include "core/NotificationStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

using namespace wimforge;

namespace {

class TestRun
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int result() const
    {
        if (m_failures == 0)
            QTextStream(stdout) << "notification_store_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

int nonEmptyLineCount(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    int count = 0;
    while (!file.atEnd()) {
        if (!file.readLine().trimmed().isEmpty())
            ++count;
    }
    return count;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeNotificationStoreTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    const QString storePath = QDir(temporary.path()).filePath(QStringLiteral("notification-center"));
    NotificationStore store(storePath);
    QString error;
    test.check(store.initialize(&error), QStringLiteral("notification store initializes: %1").arg(error));
    test.check(QFileInfo::exists(store.stateFilePath()), QStringLiteral("notifications.json exists"));
    test.check(QFileInfo::exists(store.eventFilePath()), QStringLiteral("events.jsonl exists"));
    test.check(QFileInfo::exists(QDir(storePath).filePath(QStringLiteral(".git"))),
               QStringLiteral("notification center owns a Git repository"));
    test.check(!QFileInfo::exists(QDir(temporary.path()).filePath(QStringLiteral(".git"))),
               QStringLiteral("notification Git repository does not leak into its parent"));
    test.check(store.history(100, &error).size() == 1,
               QStringLiteral("new store has one committed baseline"));

    const QString id = store.addNotification(
        QStringLiteral("Image mounted"),
        QStringLiteral("install.wim index 2 is ready"),
        QStringLiteral("success"),
        QStringLiteral("mount"),
        QJsonObject{{QStringLiteral("imageIndex"), 2}},
        &error);
    test.check(!id.isEmpty(), QStringLiteral("new notification succeeds: %1").arg(error));
    test.check(store.history(100, &error).size() == 2,
               QStringLiteral("new action creates a Git commit"));
    test.check(nonEmptyLineCount(store.eventFilePath()) == 1,
               QStringLiteral("new action appends one event record"));

    QList<Notification> visible = store.list(false, false, &error);
    test.check(visible.size() == 1 && visible.first().id == id,
               QStringLiteral("active notification is listed"));
    std::optional<Notification> found = store.find(id, &error);
    test.check(found.has_value() && found->data.value(QStringLiteral("imageIndex")).toInt() == 2,
               QStringLiteral("notification payload round-trips"));

    int expectedCommits = 2;
    int expectedEvents = 1;
    auto action = [&](bool ok, const QString &label) {
        test.check(ok, QStringLiteral("%1 succeeds: %2").arg(label, error));
        ++expectedCommits;
        ++expectedEvents;
        test.check(store.history(100, &error).size() == expectedCommits,
                   QStringLiteral("%1 creates exactly one Git commit").arg(label));
        test.check(nonEmptyLineCount(store.eventFilePath()) == expectedEvents,
                   QStringLiteral("%1 appends exactly one event").arg(label));
    };

    action(store.markRead(id, &error), QStringLiteral("mark read"));
    found = store.find(id, &error);
    test.check(found && found->isRead, QStringLiteral("read state persisted"));

    action(store.markUnread(id, &error), QStringLiteral("mark unread"));
    found = store.find(id, &error);
    test.check(found && !found->isRead, QStringLiteral("unread state persisted"));

    action(store.dismiss(id, &error), QStringLiteral("dismiss"));
    test.check(store.list(false, false, &error).isEmpty(),
               QStringLiteral("dismissed notification is hidden from the active list"));
    test.check(store.list(true, false, &error).size() == 1,
               QStringLiteral("dismissed notification remains stored"));

    action(store.restore(id, &error), QStringLiteral("restore"));
    found = store.find(id, &error);
    test.check(found && !found->isDismissed && !found->isDeleted,
               QStringLiteral("restore returns notification to the active list"));

    action(store.softDelete(id, &error), QStringLiteral("soft delete"));
    test.check(store.list(true, false, &error).isEmpty(),
               QStringLiteral("deleted tombstone is hidden by default"));
    found = store.find(id, &error);
    test.check(found && found->isDeleted && found->deletedAt.isValid(),
               QStringLiteral("soft delete retains a timestamped tombstone"));
    test.check(store.list(true, true, &error).size() == 1,
               QStringLiteral("including deleted records proves nothing was erased"));

    const QList<NotificationEvent> beforeUndoEvents = store.events(100, &error);
    test.check(beforeUndoEvents.size() == expectedEvents,
               QStringLiteral("event history lists every action"));
    test.check(!beforeUndoEvents.isEmpty() && beforeUndoEvents.first().action == QStringLiteral("delete"),
               QStringLiteral("event history is newest first"));

    test.check(store.revertLatest(&error), QStringLiteral("undo delete succeeds: %1").arg(error));
    found = store.find(id, &error);
    test.check(found && !found->isDeleted && !found->isDismissed,
               QStringLiteral("undo restored the pre-delete state"));
    test.check(store.events(100, &error).size() == expectedEvents - 1,
               QStringLiteral("undo restored the append-only log to its prior committed state"));

    test.check(store.revertLatest(&error),
               QStringLiteral("reverting the undo (redo) succeeds: %1").arg(error));
    found = store.find(id, &error);
    test.check(found && found->isDeleted,
               QStringLiteral("reverting an undo reapplies the tombstone"));
    test.check(store.events(100, &error).size() == expectedEvents,
               QStringLiteral("redo reapplied the delete event"));

    const QList<GitCommit> history = store.history(100, &error);
    test.check(history.size() == expectedCommits + 2,
               QStringLiteral("undo and redo each created their own Git commit"));
    test.check(history.size() >= 2 && history.at(0).isRevert() && history.at(1).isRevert(),
               QStringLiteral("undo and redo commits are visible as reverts"));

    test.check(!store.markRead(QStringLiteral("missing-id"), &error)
                   && error.contains(QStringLiteral("not found")),
               QStringLiteral("missing notification errors are readable"));

    const QString projectFolder = QDir(temporary.path()).filePath(QStringLiteral("project-folder"));
    QDir().mkpath(projectFolder);
    QFile projectMarker(QDir(projectFolder).filePath(QStringLiteral("project.json")));
    projectMarker.open(QIODevice::WriteOnly);
    projectMarker.write("{}");
    projectMarker.close();
    NotificationStore invalidStore(projectFolder);
    test.check(!invalidStore.initialize(&error) && error.contains(QStringLiteral("own folder")),
               QStringLiteral("notification history refuses to share a project repository"));

    return test.result();
}
