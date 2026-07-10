# Contextual action history

WimForge's contextual history is an append-only event journal inside each
project. It powers both the full History Manager and the small surface opened by
`Ctrl+Shift+Z` or by right-clicking anywhere in the desktop. The current desktop
opens that mini manager for the active page/global context. Stable element-level
filtering exists in the history core and CLI, but this release does not claim
that every individual QML control has its own right-click timeline.

## Durability model

The journal lives at `.wimforge/action-history.jsonl` in the project repository.
Every action appends one immutable JSON event and immediately creates one local
Git commit. Events include a monotonically increasing sequence, UTC timestamp,
context key, stable element ID, forward and inverse diffs, display metadata, and
a SHA-256 link to the preceding event. The hash chain detects accidental edits
or truncated/reordered history before another action is accepted.

The API bounds UI queries to 2,000 global or 500 contextual results. It never
truncates the underlying journal, so query performance has a guardrail without
discarding the user's older actions. A lock file serializes writers from
multiple windows.

## Undo, redo, and selective undo

An ordinary action stores both the forward and inverse diff. `undoAction(id)`
does not delete or rewrite it. Instead it appends a compensation event whose
forward diff is the target's inverse and whose inverse diff is the target's
forward. The application applies the newly returned event's `forwardDiff` to
its model.

Undoing that compensation appends another compensation, which applies the
original forward diff: undo-of-undo is therefore redo. `undoAction` can target
any currently effective action, even when it is not the most recent global
action, providing selective undo. Every compensation receives its own Git
commit and remains auditable.

Desktop project mutations store minimal JSON Merge Patch-shaped forward and
inverse payloads, with the complete before/after snapshots retained as metadata
for display and explicit restore. A selective compensation is applied to the
current project with `ActionHistory::applyMergePatchGuarded()`: paths outside the
target patch are preserved, while any target path whose current value no longer
matches the expected preimage is reported as a conflict and is not changed.
The user can undo the conflicting newer action first or deliberately choose a
full restore point.

For a change from image index 1 to 6, the stored patches are:

```json
{
  "forward": { "selectedImageIndex": 6 },
  "inverse": { "selectedImageIndex": 1 }
}
```

On undo, `{ "selectedImageIndex": 1 }` is applied only if the current value is
still 6. A later change to an unrelated field survives. A later change to
`selectedImageIndex` causes a conflict instead of silently erasing that work.
The event metadata also records a display summary and the full snapshots (reduced
to the relevant field in this example):

```json
{
  "diffSummary": "Changed edition from Pro to Enterprise",
  "stateFormat": "merge-patch",
  "beforeState": { "selectedImageIndex": 1 },
  "afterState": { "selectedImageIndex": 6 }
}
```

## Contexts, bookmarks, and branches

The core records stable identifiers such as `contextKey = "source"` and
`elementId = "edition-index"`. `recentForElement(contextKey, elementId)` and the
CLI `action-history list --context ... --element ...` can request that narrower
timeline. The current desktop's root right-click handler instead opens the
active-page/global timeline and falls back to the newest global action.

Bookmarks and branches are also append-only events and Git commits. History
branches are lightweight lanes within the project journal, not mutable Git
branches; this preserves a simple, crash-safe Git commit line while allowing
users to label experiments and switch lanes. The full manager can use
`branchNames()`, `createBranch()`, and `switchBranch()`.

## QML integration

`qml/components/ContextHistoryPanel.qml` is a Material-styled, non-modal
`Popup`. It never uses a native `Dialog` or `MessageDialog`, and therefore does
not pause servicing jobs. The application instantiates it at overlay scope;
the root shortcut/right-click handler supplies the current page/global events.

An individual control can opt into the existing element-level core later by
mapping its right-click point and opening the panel with
`recentForElement(contextKey, elementId)`, for example:

```qml
TapHandler {
    acceptedButtons: Qt.RightButton
    onTapped: function(eventPoint) {
        const p = parent.mapToItem(historyPanel.parent,
                                   eventPoint.position.x,
                                   eventPoint.position.y)
        historyPanel.openAt(p.x, p.y, qsTr("Edition"), controller.editionHistory)
    }
}
```

The panel's `undoRequested`, `redoRequested`, `restoreRequested`,
`bookmarkRequested`, and `branchRequested` signals connect to the controller.
Action rows use an icon and text, and bookmark/branch names are collected inline
rather than in blocking dialogs.

## Controller contract

For every user-originated mutation:

1. Capture the minimal forward/inverse merge patches and full before/after
   metadata before changing the UI model.
2. Save and Git-commit the validated project mutation through the normal project
   transaction.
3. Call `record` to append and commit its contextual event. The desktop keeps an
   already-safe project commit if this secondary append fails and emits a
   persistent history warning rather than pretending the event exists.
4. On undo or redo, guarded-apply the returned compensation event's
   `forwardDiff` to the current project and reject same-path conflicts.
5. Refresh both full and contextual history models.

System telemetry and passive view navigation do not need to be undoable, but
any user action that changes project output, package choice, policy,
notification state, queue order, template, or WinForge bundle configuration
should follow this contract.
