# History Time Machine

WimForge treats undo as durable project data, not an in-memory stack that disappears when the app closes. An output-affecting configuration mutation is saved and committed first, then its immutable action event receives a second local commit. If the secondary event append fails, WimForge keeps the project commit and raises a persistent warning. Undo adds new history instead of rewriting old history.

## Two complementary timelines

1. **Project Git history** stores committed `project.json` and supporting project files. The raw commit tab shows hashes, timestamps, subjects, and revert commits.
2. **Action timeline** stores `.wimforge/action-history.jsonl` in that same project repository. It adds contexts, element IDs, minimal forward/inverse merge patches, stable action IDs, selective compensation, bookmarks, lanes, and full-snapshot UI/restore metadata.

Notification lifecycle history is deliberately separate; see [Notification Center](Notification-Center).

## Event durability

Each action event contains:

- a monotonically increasing sequence and stable UUID;
- UTC timestamp and event type;
- title, description, icon, and destructive flag;
- context key and stable element ID;
- current history lane;
- forward and inverse JSON merge patches for desktop project mutations;
- changed-path/diff metadata;
- target/root IDs for compensation chains;
- the preceding event's SHA-256 and its own canonical SHA-256.

The hash chain detects accidental edits, truncation, and reordering before another event is accepted. A lock serializes writers from multiple windows. UI queries are bounded for responsiveness, but the journal itself is never truncated.

## Undo, redo, and undo-of-undo

An ordinary action records the before and after state. To undo it, WimForge appends a compensation event whose forward state is the target action's inverse. The original entry stays intact.

Undoing that compensation appends another compensation whose forward state reapplies the original action. That is redo-of-undo without a mutable redo stack.

Selective undo can target an older action that is still effective. WimForge evaluates the compensation chain rather than requiring the target to be the last global event. The desktop guarded-applies that action's minimal merge patch to the current project: unrelated later edits are preserved, while a later edit to any same target path causes a conflict and leaves the project unchanged. Undo the conflicting newer action first, or use an explicit restore point when replacing the full project state is intentional. All successful state changes still use the normal validation, atomic save, and Git path.

## Contextual mini manager

- Press `Ctrl+Z` to undo in the active page context.
- Press `Ctrl+Shift+Z` to open the mini history manager without leaving the current page.
- Right-click anywhere in the desktop to open the same active-page/global non-modal popup at the pointer.

The panel can show recent actions for a `contextKey` and optional stable `elementId`. It exposes icon-plus-text actions for undo, redo, restore, bookmark, and branch/lane creation. It is a Qt Quick `Popup` with `modal: false`; image jobs continue while it is open.

The current desktop routes global right-clicks to the active page and falls back to the newest global action, so the shortcut works anywhere. The history core and CLI already accept stable element IDs for narrower timelines; wiring individual controls to those IDs is a documented extension point, not claimed as part of this release's global mouse handler.

## Full History page

The History Time Machine includes:

- searchable action timeline;
- effective/undone state labels;
- selective undo/redo/restore controls;
- branch/lane picker and creation;
- bookmarks;
- A/B selection with inverse/forward JSON diff inspection;
- raw project Git commits;
- crash-journal status and paths;
- notification-ledger status and latest-event undo.

History lanes are append-only labels in the action journal, not mutable Git branches. This keeps one simple crash-safe Git commit line while allowing experiments to be grouped and switched.

## What “every action” means

WimForge records user changes that affect project output or durable application state: source/index choices, paths/options, list changes, policies, package/unattended/WinForge profiles, plan order/skip state, templates, and similar mutations. Navigation, hovering, search typing before submission, telemetry, and passive refresh are not output actions.

Notification read/dismiss/delete actions go to their own repository. External side effects—committed image bytes, installed host software, or an arbitrary child process—cannot be physically rewound by changing JSON. History can restore the configuration needed to rebuild from the pristine source.

## CLI examples

```powershell
.\WimForgeCli.exe --project C:\Images\MyProject action-history list --limit 50 --json
.\WimForgeCli.exe --project C:\Images\MyProject action-history list --context packages
.\WimForgeCli.exe --project C:\Images\MyProject action-history undo EVENT_ID
.\WimForgeCli.exe --project C:\Images\MyProject action-history redo EVENT_ID
.\WimForgeCli.exe --project C:\Images\MyProject action-history bookmark "Known-good" --event EVENT_ID
.\WimForgeCli.exe --project C:\Images\MyProject action-history branch experiment --event EVENT_ID
.\WimForgeCli.exe --project C:\Images\MyProject action-history switch experiment

.\WimForgeCli.exe --project C:\Images\MyProject history log --limit 50
.\WimForgeCli.exe --project C:\Images\MyProject history undo
.\WimForgeCli.exe --project C:\Images\MyProject history redo
```

`history undo` operates on the project Git timeline. CLI `action-history undo EVENT_ID` and `redo` append compensation events to the selective journal; the desktop controller additionally guarded-applies merge-patch events it recorded to project configuration. Arbitrary application-defined CLI diff objects are audit data and are not promised to mutate `project.json`.

Implementation detail lives in [`docs/context-history.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/context-history.md).

---

[← Unattended Studio](Unattended-Studio) · [Notification Center →](Notification-Center)
