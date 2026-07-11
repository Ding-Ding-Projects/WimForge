#include "core/ActionHistory.h"
#include "core/GitHistory.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
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
            QTextStream(stdout) << "action_history_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

int gitCommitCount(const QString &repository)
{
    QProcess process;
    process.setWorkingDirectory(repository);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("HEAD")});
    if (!process.waitForStarted(5'000) || !process.waitForFinished(10'000)
        || process.exitCode() != 0) {
        return -1;
    }
    bool ok = false;
    const int count = QString::fromUtf8(process.readAllStandardOutput()).trimmed().toInt(&ok);
    return ok ? count : -1;
}

ActionDraft editionAction(int before, int after)
{
    ActionDraft draft;
    draft.title = QStringLiteral("Select Windows edition");
    draft.description = QStringLiteral("Use image index %1 instead of %2.").arg(after).arg(before);
    draft.icon = QStringLiteral("select_all");
    draft.contextKey = QStringLiteral("source");
    draft.elementId = QStringLiteral("edition-index");
    draft.forwardDiff = QJsonObject{{QStringLiteral("selectedImageIndex"), after}};
    draft.inverseDiff = QJsonObject{{QStringLiteral("selectedImageIndex"), before}};
    draft.metadata = QJsonObject{
        {QStringLiteral("diffSummary"),
         QStringLiteral("Edition index %1 → %2").arg(before).arg(after)},
        {QStringLiteral("source"), QStringLiteral("SourcePage")},
        {QStringLiteral("stateFormat"), QStringLiteral("merge-patch")},
    };
    return draft;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeActionHistoryTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    QString error;
    test.check(GitHistory::gitAvailable(&error), QStringLiteral("Git is available: %1").arg(error));

    const QString projectDirectory = QDir(temporary.path()).filePath(QStringLiteral("project"));
    ActionHistory history(projectDirectory);

    ActionEvent edition;
    test.check(history.record(editionAction(1, 6), &edition, &error),
               QStringLiteral("first action is recorded: %1").arg(error));
    test.check(edition.sequence == 1 && !edition.id.isEmpty() && edition.isAction(),
               QStringLiteral("recorded action has identity, sequence and type"));
    test.check(QFile::exists(history.journalPath()), QStringLiteral("project journal was created"));
    test.check(QFile::exists(QDir(projectDirectory).filePath(QStringLiteral(".git"))),
               QStringLiteral("action history initializes the project's local Git repository"));
    test.check(gitCommitCount(projectDirectory) == 1,
               QStringLiteral("one action creates exactly one Git commit"));
    QList<GitCommit> actionCommits = GitHistory(projectDirectory).history(1, &error);
    test.check(error.isEmpty() && actionCommits.size() == 1
                   && actionCommits.first().subject
                          == QStringLiteral("History #1: Select Windows edition / 歷程 #1：記錄咗呢個動作"),
               QStringLiteral("an English-only action receives a natural Cantonese commit half"));

    ActionDraft outputAction;
    outputAction.title = QStringLiteral("Choose ISO output / 揀 ISO 輸出");
    outputAction.icon = QStringLiteral("album");
    outputAction.contextKey = QStringLiteral("output");
    outputAction.elementId = QStringLiteral("format");
    outputAction.forwardDiff = QJsonObject{{QStringLiteral("outputFormat"), QStringLiteral("iso")}};
    outputAction.inverseDiff = QJsonObject{{QStringLiteral("outputFormat"), QStringLiteral("wim")}};
    outputAction.metadata = QJsonObject{{QStringLiteral("diffSummary"),
                                         QStringLiteral("Output WIM → ISO")}};
    ActionEvent output;
    test.check(history.record(outputAction, &output, &error),
               QStringLiteral("independent second action is recorded: %1").arg(error));
    test.check(gitCommitCount(projectDirectory) == 2,
               QStringLiteral("every ordinary action is a separate Git commit"));
    actionCommits = GitHistory(projectDirectory).history(1, &error);
    test.check(error.isEmpty() && actionCommits.size() == 1
                   && actionCommits.first().subject
                          == QStringLiteral("History #2: Choose ISO output / 歷程 #2：揀 ISO 輸出"),
               QStringLiteral("a bilingual action is split into exactly two commit halves"));

    const QList<ActionEvent> editionEvents =
        history.recentForElement(QStringLiteral("source"), QStringLiteral("edition-index"), 20, &error);
    test.check(error.isEmpty() && editionEvents.size() == 1 && editionEvents.first().id == edition.id,
               QStringLiteral("history can be queried by stable context and element ID"));
    test.check(edition.changedPaths().contains(QStringLiteral("selectedImageIndex"))
                   && edition.diffSummary().contains(QStringLiteral("Edition index")),
               QStringLiteral("action exposes changed paths and human-readable diff metadata"));

    ActionEvent undo;
    test.check(history.undoAction(edition.id, &undo, &error),
               QStringLiteral("older action can be selectively undone: %1").arg(error));
    test.check(undo.isCompensation() && undo.targetEventId == edition.id
                   && undo.rootActionId == edition.id
                   && undo.forwardDiff.value(QStringLiteral("selectedImageIndex")).toInt() == 1
                   && undo.metadata.value(QStringLiteral("stateFormat")).toString()
                          == QStringLiteral("merge-patch"),
               QStringLiteral("undo is a compensating event carrying the inverse diff"));
    test.check(!history.isEffective(edition.id, &error) && error.isEmpty(),
               QStringLiteral("selectively undone action becomes ineffective"));
    test.check(history.isEffective(output.id, &error) && error.isEmpty(),
               QStringLiteral("selective undo does not affect a later independent action"));
    test.check(gitCommitCount(projectDirectory) == 3,
               QStringLiteral("undo creates its own Git commit"));

    ActionEvent undoOfUndo;
    test.check(history.compensateEvent(undo.id, &undoOfUndo, &error),
               QStringLiteral("the undo event can itself be undone: %1").arg(error));
    test.check(undoOfUndo.forwardDiff.value(QStringLiteral("selectedImageIndex")).toInt() == 6
                   && history.isEffective(edition.id, &error),
               QStringLiteral("undo-of-undo reapplies the original forward diff (redo)"));
    test.check(gitCommitCount(projectDirectory) == 4,
               QStringLiteral("undo-of-undo is another durable Git commit"));

    ActionEvent secondUndo;
    ActionEvent explicitRedo;
    test.check(history.undoAction(edition.id, &secondUndo, &error),
               QStringLiteral("action can be undone again: %1").arg(error));
    test.check(history.redoAction(secondUndo.id, &explicitRedo, &error),
               QStringLiteral("redo convenience API appends compensation: %1").arg(error));
    test.check(history.isEffective(edition.id, &error),
               QStringLiteral("redo returns the action to effective state"));
    test.check(!history.redoAction(edition.id, nullptr, &error)
                   && error.contains(QStringLiteral("already active")),
               QStringLiteral("invalid duplicate redo is rejected without a commit"));
    test.check(gitCommitCount(projectDirectory) == 6,
               QStringLiteral("only successful history mutations create commits"));

    ActionEvent branch;
    test.check(history.createBranch(QStringLiteral("experiment/ai"), edition.id, &branch, &error),
               QStringLiteral("history branch is created: %1").arg(error));
    test.check(history.currentBranch(&error) == QStringLiteral("experiment/ai")
                   && history.branchNames(&error).contains(QStringLiteral("experiment/ai")),
               QStringLiteral("new branch becomes the current append-only history lane"));

    ActionEvent bookmark;
    test.check(history.createBookmark(QStringLiteral("Before AI packages"), edition.id, &bookmark, &error),
               QStringLiteral("history point can be bookmarked: %1").arg(error));
    test.check(bookmark.type == ActionEventType::Bookmark && bookmark.targetEventId == edition.id,
               QStringLiteral("bookmark targets the selected event"));
    test.check(history.currentBranch(&error) == QStringLiteral("experiment/ai"),
               QStringLiteral("bookmarking an older event does not silently switch branches"));

    ActionEvent branchSwitch;
    test.check(history.switchBranch(QStringLiteral("main"), &branchSwitch, &error),
               QStringLiteral("existing branch can be selected: %1").arg(error));
    test.check(history.currentBranch(&error) == QStringLiteral("main"),
               QStringLiteral("branch switch is represented by the newest event"));
    test.check(gitCommitCount(projectDirectory) == 9,
               QStringLiteral("branch, bookmark and branch switch each create a Git commit"));

    const QList<ActionEvent> newest = history.events(3, &error);
    test.check(error.isEmpty() && newest.size() == 3 && newest.first().id == branchSwitch.id,
               QStringLiteral("bounded global query returns newest events first"));
    const QList<ActionEvent> allEditionEvents =
        history.recentForElement(QStringLiteral("source"), QStringLiteral("edition-index"), 100, &error);
    test.check(allEditionEvents.size() == 7,
               QStringLiteral("context query includes the action, compensations, bookmark and branch"));

    QJsonObject state{
        {QStringLiteral("selectedImageIndex"), 1},
        {QStringLiteral("nested"), QJsonObject{{QStringLiteral("keep"), true},
                                                {QStringLiteral("remove"), 9}}},
    };
    const QJsonObject patched = ActionHistory::applyMergePatch(
        state,
        QJsonObject{
            {QStringLiteral("selectedImageIndex"), 6},
            {QStringLiteral("nested"), QJsonObject{{QStringLiteral("remove"), QJsonValue()}}},
        });
    test.check(patched.value(QStringLiteral("selectedImageIndex")).toInt() == 6
                   && patched.value(QStringLiteral("nested")).toObject().value(QStringLiteral("keep")).toBool()
                   && !patched.value(QStringLiteral("nested")).toObject().contains(QStringLiteral("remove")),
               QStringLiteral("JSON merge utility applies nested changes and removals"));

    const QJsonObject mergeBefore{
        {QStringLiteral("stable"), 7},
        {QStringLiteral("removed"), QStringLiteral("old")},
        {QStringLiteral("settings"),
         QJsonObject{
             {QStringLiteral("theme"), QStringLiteral("light")},
             {QStringLiteral("font"),
              QJsonObject{{QStringLiteral("family"), QStringLiteral("Inter")},
                          {QStringLiteral("size"), 12}}},
             {QStringLiteral("tags"),
              QJsonArray{QStringLiteral("base"), QStringLiteral("safe")}},
         }},
    };
    const QJsonObject mergeAfter{
        {QStringLiteral("stable"), 7},
        {QStringLiteral("added"), true},
        {QStringLiteral("settings"),
         QJsonObject{
             {QStringLiteral("theme"), QStringLiteral("dark")},
             {QStringLiteral("font"),
              QJsonObject{{QStringLiteral("family"), QStringLiteral("Inter")},
                          {QStringLiteral("size"), 14}}},
             {QStringLiteral("tags"), QJsonArray{QStringLiteral("base"), QStringLiteral("ai")}},
         }},
    };
    const QJsonObject expectedForwardPatch{
        {QStringLiteral("added"), true},
        {QStringLiteral("removed"), QJsonValue::Null},
        {QStringLiteral("settings"),
         QJsonObject{
             {QStringLiteral("theme"), QStringLiteral("dark")},
             {QStringLiteral("font"), QJsonObject{{QStringLiteral("size"), 14}}},
             {QStringLiteral("tags"), QJsonArray{QStringLiteral("base"), QStringLiteral("ai")}},
         }},
    };
    const QJsonObject forwardPatch = ActionHistory::createMergePatch(mergeBefore, mergeAfter);
    const QJsonObject inversePatch = ActionHistory::createMergePatch(mergeAfter, mergeBefore);
    test.check(forwardPatch == expectedForwardPatch,
               QStringLiteral("merge-patch generation is minimal across add, delete, nested and array changes"));
    test.check(ActionHistory::createMergePatch(mergeBefore, mergeBefore).isEmpty(),
               QStringLiteral("identical objects generate an empty merge patch"));
    test.check(ActionHistory::applyMergePatch(mergeBefore, forwardPatch) == mergeAfter,
               QStringLiteral("a generated forward patch exactly produces the target object"));
    test.check(ActionHistory::applyMergePatch(mergeAfter, inversePatch) == mergeBefore,
               QStringLiteral("a generated inverse patch exactly restores the source object"));

    const MergePatchApplyResult guardedUndo =
        ActionHistory::applyMergePatchGuarded(mergeAfter, inversePatch, forwardPatch);
    test.check(guardedUndo.applied && guardedUndo.conflicts.isEmpty()
                   && guardedUndo.state == mergeBefore,
               QStringLiteral("guarded undo accepts the forward patch as its optimistic precondition"));
    const MergePatchApplyResult guardedRedo =
        ActionHistory::applyMergePatchGuarded(mergeBefore, forwardPatch, inversePatch);
    test.check(guardedRedo.applied && guardedRedo.conflicts.isEmpty()
                   && guardedRedo.state == mergeAfter,
               QStringLiteral("guarded redo accepts the inverse patch as its optimistic precondition"));

    QJsonObject laterSiblingEdit = mergeAfter;
    QJsonObject laterSettings = laterSiblingEdit.value(QStringLiteral("settings")).toObject();
    QJsonObject laterFont = laterSettings.value(QStringLiteral("font")).toObject();
    laterFont.insert(QStringLiteral("family"), QStringLiteral("JetBrains Mono"));
    laterSettings.insert(QStringLiteral("font"), laterFont);
    laterSiblingEdit.insert(QStringLiteral("settings"), laterSettings);
    const MergePatchApplyResult siblingSafeUndo =
        ActionHistory::applyMergePatchGuarded(laterSiblingEdit, inversePatch, forwardPatch);
    test.check(siblingSafeUndo.applied
                   && siblingSafeUndo.state.value(QStringLiteral("settings"))
                              .toObject()
                              .value(QStringLiteral("font"))
                              .toObject()
                              .value(QStringLiteral("family"))
                              .toString()
                          == QStringLiteral("JetBrains Mono")
                   && siblingSafeUndo.state.value(QStringLiteral("settings"))
                              .toObject()
                              .value(QStringLiteral("font"))
                              .toObject()
                              .value(QStringLiteral("size"))
                              .toInt()
                          == 12,
               QStringLiteral("selective undo preserves unrelated later edits in nested sibling paths"));

    QJsonObject laterSamePathEdit = mergeAfter;
    QJsonObject conflictingSettings = laterSamePathEdit.value(QStringLiteral("settings")).toObject();
    conflictingSettings.insert(QStringLiteral("theme"), QStringLiteral("midnight"));
    conflictingSettings.insert(QStringLiteral("tags"), QJsonArray{QStringLiteral("later")});
    laterSamePathEdit.insert(QStringLiteral("settings"), conflictingSettings);
    const MergePatchApplyResult rejectedSamePath =
        ActionHistory::applyMergePatchGuarded(laterSamePathEdit, inversePatch, forwardPatch);
    test.check(!rejectedSamePath.applied && rejectedSamePath.state == laterSamePathEdit
                   && rejectedSamePath.conflicts.contains(QStringLiteral("/settings/theme"))
                   && rejectedSamePath.conflicts.contains(QStringLiteral("/settings/tags")),
               QStringLiteral("guarded undo is atomic and reports every later same-path scalar or array edit"));

    const QJsonObject addedBefore;
    const QJsonObject addedAfter{{QStringLiteral("tool"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("OpenCode")}}}};
    const QJsonObject addPatch = ActionHistory::createMergePatch(addedBefore, addedAfter);
    const QJsonObject removeAddedPatch = ActionHistory::createMergePatch(addedAfter, addedBefore);
    QJsonObject subsequentlyExtended = addedAfter;
    subsequentlyExtended.insert(
        QStringLiteral("tool"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("OpenCode")},
                    {QStringLiteral("configuredLater"), true}});
    const MergePatchApplyResult rejectedWholeRemoval = ActionHistory::applyMergePatchGuarded(
        subsequentlyExtended, removeAddedPatch, addPatch);
    test.check(!rejectedWholeRemoval.applied
                   && rejectedWholeRemoval.conflicts == QStringList{QStringLiteral("/tool")},
               QStringLiteral("undoing an added object cannot erase fields subsequently added inside it"));
    const MergePatchApplyResult acceptedWholeRemoval =
        ActionHistory::applyMergePatchGuarded(addedAfter, removeAddedPatch, addPatch);
    test.check(acceptedWholeRemoval.applied && acceptedWholeRemoval.state.isEmpty(),
               QStringLiteral("an unchanged added object can be removed by guarded undo"));

    const QJsonObject deletedBefore{{QStringLiteral("legacy"), 42}};
    const QJsonObject deletedAfter;
    const QJsonObject deletePatch = ActionHistory::createMergePatch(deletedBefore, deletedAfter);
    const QJsonObject restorePatch = ActionHistory::createMergePatch(deletedAfter, deletedBefore);
    const MergePatchApplyResult acceptedRestore =
        ActionHistory::applyMergePatchGuarded(deletedAfter, restorePatch, deletePatch);
    test.check(acceptedRestore.applied && acceptedRestore.state == deletedBefore,
               QStringLiteral("guarded undo restores a key only while it remains absent"));
    const QJsonObject independentlyReadded{{QStringLiteral("legacy"), 99}};
    const MergePatchApplyResult rejectedRestore =
        ActionHistory::applyMergePatchGuarded(independentlyReadded, restorePatch, deletePatch);
    test.check(!rejectedRestore.applied
                   && rejectedRestore.conflicts == QStringList{QStringLiteral("/legacy")}
                   && rejectedRestore.state == independentlyReadded,
               QStringLiteral("guarded undo will not overwrite an independently re-added key"));

    const QJsonObject scalarBefore{{QStringLiteral("mode"), QStringLiteral("legacy")}};
    const QJsonObject emptyObjectAfter{{QStringLiteral("mode"), QJsonObject{}}};
    const QJsonObject scalarToObject =
        ActionHistory::createMergePatch(scalarBefore, emptyObjectAfter);
    const QJsonObject objectToScalar =
        ActionHistory::createMergePatch(emptyObjectAfter, scalarBefore);
    test.check(scalarToObject
                       == QJsonObject{{QStringLiteral("mode"), QJsonObject{}}}
                   && ActionHistory::applyMergePatch(scalarBefore, scalarToObject)
                          == emptyObjectAfter,
               QStringLiteral("an empty object patch replaces an absent or scalar value"));
    const MergePatchApplyResult acceptedObjectReplacement =
        ActionHistory::applyMergePatchGuarded(emptyObjectAfter, objectToScalar, scalarToObject);
    test.check(acceptedObjectReplacement.applied
                   && acceptedObjectReplacement.state == scalarBefore,
               QStringLiteral("whole object-to-scalar replacement succeeds when the object is unchanged"));
    const QJsonObject extendedEmptyObject{
        {QStringLiteral("mode"), QJsonObject{{QStringLiteral("later"), true}}}};
    const MergePatchApplyResult rejectedObjectReplacement = ActionHistory::applyMergePatchGuarded(
        extendedEmptyObject, objectToScalar, scalarToObject);
    test.check(!rejectedObjectReplacement.applied
                   && rejectedObjectReplacement.conflicts
                          == QStringList{QStringLiteral("/mode")},
               QStringLiteral("whole object replacement rejects any later descendant edit"));

    const MergePatchApplyResult missingPrecondition = ActionHistory::applyMergePatchGuarded(
        QJsonObject{{QStringLiteral("unsafe"), 2}},
        QJsonObject{{QStringLiteral("unsafe"), 1}},
        QJsonObject{});
    test.check(!missingPrecondition.applied
                   && missingPrecondition.conflicts
                          == QStringList{QStringLiteral("/unsafe")},
               QStringLiteral("a changed path without an optimistic precondition is rejected"));

    const QString escapedKey = QStringLiteral("package/name~channel");
    const MergePatchApplyResult escapedConflict = ActionHistory::applyMergePatchGuarded(
        QJsonObject{{escapedKey, QStringLiteral("nightly")}},
        QJsonObject{{escapedKey, QStringLiteral("stable")}},
        QJsonObject{{escapedKey, QStringLiteral("preview")}});
    test.check(!escapedConflict.applied
                   && escapedConflict.conflicts
                          == QStringList{QStringLiteral("/package~1name~0channel")},
               QStringLiteral("conflict paths use escaped RFC 6901 JSON Pointer syntax"));

    const MergePatchApplyResult storedNullIsPresent = ActionHistory::applyMergePatchGuarded(
        QJsonObject{{QStringLiteral("nullable"), QJsonValue::Null}},
        QJsonObject{{QStringLiteral("nullable"), QStringLiteral("restored")}},
        QJsonObject{{QStringLiteral("nullable"), QJsonValue::Null}});
    test.check(!storedNullIsPresent.applied
                   && storedNullIsPresent.conflicts
                          == QStringList{QStringLiteral("/nullable")},
               QStringLiteral("an expected deletion requires absence, not a stored JSON null"));

    // Integrity is tested in a separate project so intentional corruption does
    // not interfere with the commit-count assertions above.
    const QString tamperedDirectory = QDir(temporary.path()).filePath(QStringLiteral("tampered"));
    ActionHistory tampered(tamperedDirectory);
    ActionEvent tamperedEvent;
    test.check(tampered.record(editionAction(1, 2), &tamperedEvent, &error),
               QStringLiteral("tamper fixture was recorded: %1").arg(error));
    QFile journal(tampered.journalPath());
    test.check(journal.open(QIODevice::ReadWrite), QStringLiteral("tamper fixture journal opens"));
    if (journal.isOpen()) {
        QByteArray bytes = journal.readAll();
        bytes.replace("Select Windows edition", "Select Windows EDITION");
        journal.resize(0);
        journal.write(bytes);
        journal.close();
    }
    const QList<ActionEvent> rejected = tampered.events(20, &error);
    test.check(rejected.isEmpty() && error.contains(QStringLiteral("integrity check failed")),
               QStringLiteral("hash chain rejects modified history before another action is accepted"));

    return test.result();
}
