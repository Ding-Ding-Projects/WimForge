# Notification Center

The notification center is a durable, non-blocking activity ledger. It has its own local Git repository so status survives project changes and every notification lifecycle action remains auditable.

## Storage model

The default store is the application's local-data directory plus `notification-center`. The exact resolved path is shown on the History page. A custom CLI store can be selected with `--store`.

The repository contains:

- `notifications.json` — current notification state;
- `events.jsonl` — immutable event records;
- `.git` — the complete local history.

The store refuses to initialize inside a WimForge project directory. Keeping it separate prevents project commits and application-wide notifications from becoming one ambiguous history.

## Committed actions

Every successful action writes state/event data atomically and creates a Git commit:

| Action | Result |
| --- | --- |
| New | Stable notification ID, title, message, severity, source, timestamps, optional JSON data |
| Read | Marks the item read |
| Unread | Returns it to the unread count |
| Dismiss | Hides it from the normal active list without deleting the record |
| Restore | Makes a dismissed or deleted record active again |
| Delete | Writes a tombstone; the record and Git ancestry remain recoverable |

Supported severities are `info`, `success`, `warning`, `error`, and `progress`.

Undo uses `git revert` on the latest store commit. Reverting that revert performs redo, so undo-of-undo remains normal Git history rather than a destructive reset.

## Interface behavior

The bell opens a Material drawer inside the application. It shows unread count, icon/text state, and actions for read/unread, dismiss, restore, and recoverable delete. The drawer does not suspend the main event loop or servicing jobs.

Errors, completion, operator-approved OpenCode setup, and servicing status can create persistent entries. OpenCode discovery never starts merely because the elevated desktop launched. Short feedback can also appear as a snackbar. Recovery choices use in-app surfaces instead of native blocking dialogs.

The History page shows the repository path, explains tombstones, creates a test event, and can undo the latest notification change.

## CLI examples

```powershell
$store = 'C:\State\WimForgeNotifications'

.\WimForgeCli.exe --store $store notifications new `
  --title "ISO complete" `
  --message "Boot-test the image before deployment." `
  --severity success `
  --data '{"project":"MyProject"}'

.\WimForgeCli.exe --store $store notifications read NOTIFICATION_ID
.\WimForgeCli.exe --store $store notifications unread NOTIFICATION_ID
.\WimForgeCli.exe --store $store notifications dismiss NOTIFICATION_ID
.\WimForgeCli.exe --store $store notifications restore NOTIFICATION_ID
.\WimForgeCli.exe --store $store notifications delete NOTIFICATION_ID
.\WimForgeCli.exe --store $store notifications list --all --json
.\WimForgeCli.exe --store $store notifications events --limit 100
.\WimForgeCli.exe --store $store notifications history
.\WimForgeCli.exe --store $store notifications undo
.\WimForgeCli.exe --store $store notifications redo
```

## Complete-save behavior

GUI and CLI `.wimforge` exports include the notification repository beside the project repository. The hidden `.git` directory, events, current state, refs, objects, reflogs, and undo/redo commits are included. Import restores both histories before the project is opened.

The exporter should briefly pause repository writers to capture one coherent application moment; ordinary filesystem APIs cannot provide a transaction across independent repositories.

## Boundaries

- A notification records application state, not proof that an external deployment succeeded. Check logs and output independently.
- Tombstones preserve recovery evidence; they are not secure deletion.
- Git history can contain sensitive message/data fields. Do not put secrets in notifications.
- Undo operates on the latest repository commit. Use the project History Time Machine for selective project actions.

## 香港粵語速讀

通知中心有獨立 Git，所以讀/未讀、dismiss、restore 同可復原 delete 唔會跟開緊邊個工程一齊消失。Delete 係 tombstone，唔係安全銷毀；Git 仍然可能有舊訊息，所以通知唔好放密碼、token 或 product key。條 bell drawer 同 snackbar 都係 app 內非封鎖畫面；OpenCode 亦只會在你明確批准 setup 後先會有相關通知。

---

[← History Time Machine](History-Time-Machine) · [Project Bundles →](Project-Bundles)
