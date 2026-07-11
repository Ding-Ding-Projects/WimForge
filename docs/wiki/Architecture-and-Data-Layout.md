# Architecture and Data Layout

WimForge is a Windows-native C++20/Qt 6 application. Its central architectural boundary is deliberate: QML presents and collects intent, `AppController` adapts that intent, core classes validate and model it, and external programs receive explicit executable/argument arrays only after review.

## Runtime entry points

| Entry point | Role |
| --- | --- |
| `WimForge.exe` | Qt Quick Material desktop application; also recognizes the command-line mode selected by the launcher |
| `WimForgeCli.exe` | Console-subsystem executable for predictable terminal attachment, JSON output, and exit codes |
| `WimForgeIconTool` | Build-time/developer icon utility; excluded from the normal application build |

Both user-facing executables share the core configuration, history, servicing, studio, and bridge code. The CLI never waits for interactive terminal input; destructive apply and software-install actions require explicit confirmation flags.

## Desktop data flow

1. `qml/Main.qml` owns the shell, navigation, sheets, notification drawer, contextual history surface, and the ten page components under `qml/pages`.
2. `AppController` exposes typed Qt properties, invokable actions, and signals to QML. It owns the current project, studio state, notification store, file watcher, and job engine.
3. Classes under `src/core` validate and serialize projects, build servicing plans, run operations, record Git/action history, manage notification events, create project bundles, and implement the Package/GPO/Unattended/WinForge domains.
4. External work uses `QProcess` with an executable and argument list. DISM, Git, `oscdimg`, WinGet, npm, installers, and vendor tools remain separate trust domains.

The command shown in the UI is a review preview. The underlying operation retains structured fields such as `executable`, `arguments`, `dependsOn`, `requiresAdministrator`, `destructive`, and write/concurrency flags.

## Repository source layout

| Path | Purpose |
| --- | --- |
| `qml/` | Material shell, shared components, and desktop pages |
| `src/AppController.*` | QML-facing application coordinator |
| `src/core/ProjectConfig.*` | Versioned `project.json`, validation, canonical save, project Git history |
| `src/core/ServicingPlan.*` | Dependency graph, path/workspace safety, command previews, script export |
| `src/core/JobEngine.*` | Dependency-aware scheduling, child processes, cancellation, logs, and journal |
| `src/core/StructuredLogger.*` | Redacted JSONL application/session, controller, Qt, and process diagnostics |
| `src/core/WorkspaceTabs.*` | Browser-style tab state, styling, dedicated Git history, and portable exports |
| `src/core/ActionHistory.*` | Append-only contextual actions and guarded compensation/redo |
| `src/core/NotificationStore.*` | Notification state/events in a separate Git repository |
| `src/core/ProjectBundle.*` | Complete `.wimforge` export/import |
| `src/core/Gpo*`, `PackageStudio.*`, `UnattendBuilder.*`, `WinForgeBridge.*` | Studio-specific parsers, validation, compilation, and staging |
| `src/cli/` | Command parsing, dependency injection, deterministic output, and exit codes |
| `tests/` | Core and CLI executable tests registered with CTest |
| `docs/wiki/` | Checked-in source for the GitHub Wiki |

## Project directory

A project begins with a small set of files and grows only as features are used. A representative layout is:

```text
<project>/
├── project.json
├── .git/
├── .wimforge/
│   ├── action-history.jsonl
│   ├── action-history.lock
│   ├── job-journal.json
│   ├── logs/<run-id>/
│   ├── tabs/
│   │   ├── tabs.json
│   │   └── .git/
│   ├── work/
│   ├── generated/
│   │   ├── unattended/autounattend.xml
│   │   └── packages/
│   │       ├── profile.json
│   │       └── bundle/
│   └── winforge-payloads/
└── payloads/
    └── host-drivers/
```

Not every path is always present. `project.json` is the versioned declarative model. `.git` is the project's complete configuration history. The action journal and job journal are separate because a user action and an external operation transition are different facts.

The planner normally uses `.wimforge/work` for project-owned image/media workspaces. Generated Package Studio, unattended, and WinForge payloads remain under the project so their later inclusion is explicit and reviewable.

## Per-user application state

Qt chooses the platform-specific application-local data and settings locations. WimForge uses them for:

- a `notification-center` directory containing `notifications.json`, `events.jsonl`, and its own `.git` repository;
- a rotating `logs/wimforge.jsonl` diagnostic stream plus five bounded archives;
- a recovery root exposed to the desktop; and
- QSettings-backed preferences such as language, theme, motion, concurrency, safety preferences, and the last project.

The active job journal itself is project-local at `.wimforge/job-journal.json`. The notification-store path is visible on the Settings page and may be reconnected from a complete imported bundle.

## Four histories, four meanings

| History | Storage | What it proves |
| --- | --- | --- |
| Project commits | Project `.git` | Snapshots of successfully saved project configuration |
| Contextual action events | `.wimforge/action-history.jsonl` plus project commits | User-intent events, target paths, compensation/redo relationships, bookmarks, and branches |
| Notification events | Notification store `events.jsonl` plus its `.git` | Read/unread, dismiss, delete/restore, and notification undo state |
| Workspace tabs | `.wimforge/tabs/tabs.json` plus `.wimforge/tabs/.git` | Tab open/close/order, active tab, rename, and per-tab typography changes |

Undo appends a compensating action. It does not erase history or prove that external side effects were reversed. A complete [Project Bundle](Project-Bundles) carries the project tree, nested workspace-tab history, and notification repository together.

## Servicing transaction boundary

`ServicingPlan` builds an explicit graph and rejects invalid or overlapping paths. `JobEngine` schedules dependency-ready work, serializes mounted-image writes, records transitions, and stores per-run output. Atomic file operations and same-volume promotion protect WimForge-owned final files.

That boundary ends at external behavior. DISM may have committed bytes, an installer may have changed a machine, a vendor download may change, and Windows Setup may reject a syntactically valid answer file. Recovery rebuilds from pristine inputs; it is not an arbitrary filesystem or VM snapshot.

## Formats

- `project.json`: schema-versioned declarative project configuration.
- Portable studio JSON: Package, Unattended, and WinForge editor intent.
- Answer-file XML: Windows Setup input exported by Unattended Studio.
- `.wimforge`: uncompressed, hash-verified complete-save container with full repository topology, including the nested tab repository.
- `.wftabs`: portable mergeable tab definitions without Git objects.
- `.wftabrepo`: one-file export of the complete dedicated tab repository.
- Exported PowerShell: review/automation artifact generated from a plan; edited copies are outside the application's validation boundary.

## Explicitly planned or incomplete areas

The current architecture does **not** imply that these are complete:

- a compatibility database or update applicability resolver (the in-app Microsoft Update Catalog downloader is implemented, but it does not resolve applicability or cache across projects);
- broad live-host inventory/refresh parity with mature commercial products;
- a VMware/VirtualBox VM lab and recorded validation-run manager;
- globally applied interface-density scaling and complete responsive/accessibility automation;
- code-signed release artifacts; or
- automatic main-repository-to-Wiki synchronization unless a dedicated workflow is added and verified.

Track repository claims against current tests and the [NTLite Feature Comparison](NTLite-Feature-Comparison), not against planned UI labels.

Implementation references: [`CMakeLists.txt`](https://github.com/codingmachineedge/WimForge/blob/main/CMakeLists.txt), [`src/AppController.h`](https://github.com/codingmachineedge/WimForge/blob/main/src/AppController.h), [`docs/servicing-plan.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/servicing-plan.md), and [`docs/context-history.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/context-history.md).

## 香港粵語架構重點

工程本身有頂層 Git 快照同 append-only action history；`.wimforge/tabs` 另外有一個已強化 Git 記分頁；通知中心又有自己嘅 Git；Job Engine journal 就記錄外部執行進度。四種歷史證明嘅事唔同，唔好將「撤銷設定」當成「已逆轉 DISM 寫入」。程式層 JSONL log 放喺 Qt application-local `logs`，5 MiB rotate，保留五份 archive；分享前仍然要人手檢查有冇私密資料。

---

[← Troubleshooting](Troubleshooting) · [Building and Releases →](Building-and-Releases)
