# Settings

The **Settings** page combines application preferences with project-scoped automation. Preference changes are persisted through Qt's platform settings store; automatic import/export values live in the open project's `project.json` and therefore create project history.

## Language

Choose one of three presentation modes:

- English;
- Hong Kong Cantonese; or
- English and Cantonese side by side.

The shell and pages use the selected mode immediately. External-tool output, source metadata, paths, and some technical identifiers are not translated.

## Appearance

| Setting | Current behavior |
| --- | --- |
| **Theme** | Follow the operating-system color scheme, force Material light, or force Material dark |
| **Interface density** | Persists a value from 0.8 to 1.25; in the current source it is not yet applied as one global scale across all QML dimensions |
| **Motion** | Persists the motion preference and is passed to shared overlay components; it does not change external process behavior |

Theme-aware colors belong to the UI only. They do not change output or servicing configuration.

## Jobs and concurrency

| Setting | Range | Current behavior |
| --- | --- | --- |
| **Maximum parallel jobs** | 1–16 | Passed to the job engine; independent eligible work may overlap while mounted-image writes remain serialized |
| **CPU thread ceiling** | 1–logical CPU count | Persisted and exposed in Settings; the current job-engine start path does not pass it to external tools |
| **Scratch-space reserve** | 5–500 GB | Persisted and exposed in Settings; the current planner does not enforce this value as a free-space gate |

Set concurrency conservatively when source, mount, scratch, and output share a slow disk. A larger value does not bypass dependencies or allow concurrent writes to the same mounted image.

## Failsafes

- **Flush crash journal after every state transition** persists the preference. The current job engine maintains its project journal as part of normal execution; do not treat this setting as permission to remove recovery state.
- **Hash source before apply** also updates the open project's payload-verification option.
- **Require checkpoint before destructive operations** controls destructive-operation checkpoint intent and is mirrored on **Review & run**.
- **Recoverable tombstones** is displayed as always enabled: notification deletion remains a recoverable event rather than immediate history erasure.

Disabling a configurable safety preference transfers risk to the operator; it does not expand what external tools can safely do. See [Safety and Recovery](Safety-and-Recovery).

## Automatic import and export

These controls require an open project:

- **Watch project config for external changes** watches `project.json` and reloads reviewed external changes through the controller's project path.
- **Export a portable config after every commit** writes JSON configuration to the chosen destination.

The automatic export is not a complete save. It does not carry `.git`, contextual action events, notification history, or hidden state. Use a [Project Bundle](Project-Bundles) for complete portability.

Avoid pointing automatic export inside a source, mount, or output tree. Keep the destination writable and under backup. If an external editor and WimForge change the same configuration path, inspect Git/action history rather than assuming last-writer-wins is safe.

## Notification center

Settings displays the active notification repository path and can create a test notification. The store contains notification state, an append-only event file, and its own local Git repository. Reading, marking unread, dismissing, tombstone-deleting, restoring, and undoing are committed independently of the current project.

See [Notification Center](Notification-Center) for lifecycle details and [Architecture and Data Layout](Architecture-and-Data-Layout) for storage boundaries.

## Diagnostics and logging

WimForge writes structured JSON Lines diagnostics to the application-local `logs/wimforge.jsonl` file. Settings shows the exact active path and can open either the current file or its folder. Records include a UTC timestamp, sequence, session, severity, category, event, process/thread identity, source location, message, and structured data.

Logging covers GUI and CLI startup/shutdown, Qt messages, controller mutations and outcomes, notifications, source inspection, host-driver export, servicing activity, scheduler state, every Job Engine child-process launch, redacted arguments and output, errors, completion, cancellation, and dependency blocks. The active file rotates at 5 MiB and retains five archives.

Secret-like JSON keys and textual password, bearer-token, API-key, credential, cookie, product-key, URL-password, GitHub-token, and OpenAI-key patterns are replaced with `[REDACTED]`, including separate sensitive command-line option values. Redaction is defense in depth, not permission to put secrets in project names, paths, scripts, or third-party output. Review diagnostic files before sharing them.

## 香港粵語重點

Settings 可以揀 English、香港粵語或雙語，同時控制 theme、density、motion、concurrency 同 safety 選項。Diagnostics 會顯示而家個 `wimforge.jsonl` 路徑，可以開 log 或所在資料夾；紀錄包括 GUI/CLI 開關、project mutation、source inspection、驅動匯出、scheduler 同每個子程式嘅生命週期/輸出。Logger 會遮蔽常見 secret pattern，但呢個只係防線；分享前仍然要自己逐行檢查，唔好將密碼放入路徑、工程名或第三方輸出。

---

[← Project Bundles](Project-Bundles) · [Troubleshooting →](Troubleshooting)
