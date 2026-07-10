#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace wimforge {

enum class ActionEventType
{
    Action,
    Compensation,
    Bookmark,
    Branch
};

struct ActionDraft
{
    QString title;
    QString description;
    QString icon = QStringLiteral("history");
    QString contextKey;
    QString elementId;
    QJsonObject forwardDiff;
    QJsonObject inverseDiff;
    QJsonObject metadata;
    QString branch;
    bool destructive = false;
};

// An immutable entry in .wimforge/action-history.jsonl. Undo and redo never
// mutate an older entry: they append a Compensation entry whose forward diff
// is the target entry's inverse diff.
struct ActionEvent
{
    int schemaVersion = 1;
    qint64 sequence = 0;
    QString id;
    QDateTime timestamp;
    ActionEventType type = ActionEventType::Action;
    QString title;
    QString description;
    QString icon;
    QString contextKey;
    QString elementId;
    QString branch = QStringLiteral("main");
    bool destructive = false;
    QJsonObject forwardDiff;
    QJsonObject inverseDiff;
    QJsonObject metadata;
    QString targetEventId;
    QString rootActionId;
    QString previousHash;
    QString eventHash;

    [[nodiscard]] bool isAction() const { return type == ActionEventType::Action; }
    [[nodiscard]] bool isCompensation() const
    {
        return type == ActionEventType::Compensation;
    }
    [[nodiscard]] QStringList changedPaths() const;
    [[nodiscard]] QString diffSummary() const;
    [[nodiscard]] QJsonObject toJson(bool includeHash = true) const;
    [[nodiscard]] QVariantMap toVariantMap(bool effective = true) const;

    static bool fromJson(const QJsonObject &json, ActionEvent *event, QString *error = nullptr);
};

// Result of an optimistic JSON Merge Patch application. A rejected patch
// leaves state unchanged and reports every conflicting RFC 6901 JSON Pointer.
struct MergePatchApplyResult
{
    bool applied = false;
    QJsonObject state;
    QStringList conflicts;
};

// Project-local, event-sourced history. Every public mutation appends exactly
// one immutable event and creates exactly one commit in the project's existing
// local Git repository. Query counts are deliberately bounded; the journal is
// not truncated, so older actions are never discarded.
class ActionHistory
{
public:
    static constexpr int CurrentSchemaVersion = 1;
    static constexpr auto RelativeJournalPath = ".wimforge/action-history.jsonl";

    explicit ActionHistory(QString projectDirectory);

    [[nodiscard]] QString projectDirectory() const;
    [[nodiscard]] QString journalPath() const;
    bool initialize(QString *error = nullptr) const;

    bool record(const ActionDraft &draft,
                ActionEvent *createdEvent = nullptr,
                QString *error = nullptr) const;

    // Selectively toggles one logical action. undoAction() accepts the ID of
    // the original action or any compensation in its chain. redoAction() does
    // the same. The returned event's forwardDiff is what the caller applies.
    bool undoAction(const QString &eventOrActionId,
                    ActionEvent *createdEvent = nullptr,
                    QString *error = nullptr) const;
    bool redoAction(const QString &eventOrActionId,
                    ActionEvent *createdEvent = nullptr,
                    QString *error = nullptr) const;

    // Compensate the current tail event directly. This makes "undo the undo"
    // a first-class operation rather than a mutable redo stack.
    bool compensateEvent(const QString &eventId,
                         ActionEvent *createdEvent = nullptr,
                         QString *error = nullptr) const;

    bool createBookmark(const QString &name,
                        const QString &eventId = {},
                        ActionEvent *createdEvent = nullptr,
                        QString *error = nullptr) const;
    bool createBranch(const QString &name,
                      const QString &fromEventId = {},
                      ActionEvent *createdEvent = nullptr,
                      QString *error = nullptr) const;
    bool switchBranch(const QString &name,
                      ActionEvent *createdEvent = nullptr,
                      QString *error = nullptr) const;

    [[nodiscard]] QList<ActionEvent> events(int maximumCount = 200,
                                            QString *error = nullptr) const;
    [[nodiscard]] QList<ActionEvent> recentForElement(const QString &contextKey,
                                                      const QString &elementId,
                                                      int maximumCount = 25,
                                                      QString *error = nullptr) const;
    [[nodiscard]] bool isEffective(const QString &eventOrActionId,
                                   QString *error = nullptr) const;
    [[nodiscard]] QString currentBranch(QString *error = nullptr) const;
    [[nodiscard]] QStringList branchNames(QString *error = nullptr) const;

    // Utilities for consumers that store state as RFC 7396-style JSON Merge
    // Patches. A null value removes a key, objects merge recursively, and an
    // array is replaced as one value.
    //
    // createMergePatch() returns the smallest patch that transforms before
    // into after (where null object members have the RFC 7396 meaning of
    // deletion rather than a storable JSON null).
    [[nodiscard]] static QJsonObject createMergePatch(const QJsonObject &before,
                                                      const QJsonObject &after);
    [[nodiscard]] static QJsonObject applyMergePatch(QJsonObject state,
                                                     const QJsonObject &patch);

    // Applies patch only when the values it will overwrite still match
    // expectedPatch. The expected patch is normally the action's opposite
    // patch: for undo, pass the original forward patch; for redo, pass the
    // original inverse patch. This makes selective undo optimistic: edits to
    // the same path made after the action become conflicts, while edits to
    // unrelated sibling paths are preserved.
    //
    // On conflict, applied is false, state is the unmodified input, and
    // conflicts contains stable RFC 6901 JSON Pointers for every rejected
    // path. Every changed path in patch must have a matching precondition in
    // expectedPatch; missing preconditions are conflicts rather than an
    // unguarded write.
    [[nodiscard]] static MergePatchApplyResult applyMergePatchGuarded(
        const QJsonObject &state,
        const QJsonObject &patch,
        const QJsonObject &expectedPatch);

private:
    QString m_projectDirectory;

    [[nodiscard]] QList<ActionEvent> loadAll(QString *error) const;
    bool append(ActionEvent event, ActionEvent *createdEvent, QString *error) const;
    bool toggle(const QString &eventOrActionId,
                bool requireCurrentlyEffective,
                const QString &operation,
                ActionEvent *createdEvent,
                QString *error) const;
};

[[nodiscard]] QString actionEventTypeName(ActionEventType type);

} // namespace wimforge
