# Application Tour

WimForge presents one Material desktop shell around twelve workflow pages. The pages edit a declarative project, manage reviewed VM operations, or host an explicit in-app shell; image-changing commands appear only after the project is converted into a reviewable servicing plan.

For a safe tour that does not require production media, start the populated demo project:

```powershell
.\WimForge.exe --demo --language bilingual --page overview
```

The accepted language values are `en`, `zh-HK`, and `bilingual`. Accepted page IDs are `overview`, `source`, `customize`, `gpo`, `unattended`, `packages`, `winforge`, `vmlab`, `plan`, `history`, `settings`, and `terminal`. `vm-lab` is accepted as an alias for `vmlab` at startup.

## Global shell

The navigation rail remains available throughout the desktop workflow. At narrower desktop widths it becomes a compact icon rail; each control retains a name and tooltip. The header provides:

- a search field; in the current desktop implementation, submitting a query opens Group Policy Studio and searches its catalog;
- a job indicator that opens **Review & run**;
- a notification bell with unread count and the recoverable notification drawer;
- the current project control, which opens the project/import sheet; and
- an interrupted-run banner when recovery state is detected.

The project summary at the bottom of the rail shows the active project, Git status, and job progress. Informational sheets, snackbars, the notification drawer, and contextual history are in-app surfaces rather than native blocking dialogs.

The tab strip below the header works like a browser: navigation opens or activates a project-local page tab, and each tab can be moved, closed, renamed, or given its own font family, size, color, bold, italic, and strikeout styling. The tab menu imports or exports portable definitions and complete Git-backed tab repositories. See [Workspace Tabs](Workspace-Tabs).

## The twelve pages

| Route | What to do there | Detailed guide |
| --- | --- | --- |
| **Overview** | See the current project, operation/history counts, running jobs, build flow, safety reminders, and current job progress. | [Getting Started](Getting-Started) |
| **Source & editions** | Select and inspect media, choose the working image/index, keep cloning enabled, and select an output. | [Projects and Sources](Projects-and-Sources) |
| **Customize** | Queue update, driver, feature, Appx, component, setting, answer-file, and post-setup intent. | [Customize](Customize) |
| **Group Policy Studio** | Load installed ADMX/ADML definitions, search them, edit schema-backed values, and commit registry policy state. | [Group Policy Studio](Group-Policy-Studio) |
| **Unattended Studio** | Build or import a portable profile and export Windows answer-file XML. | [Unattended Studio](Unattended-Studio) |
| **Package Studio** | Select first-logon software, validate provider/trust metadata, and stage a resumable bundle. | [Package Studio](Package-Studio) |
| **WinForge Bridge** | Review typed actions, detect a runtime contract, and stage a verified OEM payload. | [WinForge Bridge](WinForge-Bridge) |
| **Virtual Machine Lab** | Discover installed VMware/VirtualBox providers, create or load VMs, review lifecycle and device operations, manage snapshots, and retain validation evidence. | [Virtual Machine Lab](Virtual-Machine-Lab) |
| **Review & run** | Inspect commands, dependencies, risk markers, elevation, checkpoints, and execution state. | [Review and Run](Review-and-Run) |
| **History & recovery** | Selectively undo/redo project intent, inspect commits/diffs, and review notification history. | [History Time Machine](History-Time-Machine) |
| **Settings** | Choose language/theme, job limits, failsafes, project automation, and inspect notification storage. | [Settings](Settings) |
| **Embedded terminal** | Run an explicitly started elevated PowerShell or Command Prompt session through ConPTY with bounded in-app output and no external console. | [Embedded Terminal](Embedded-Terminal) |

## Keyboard and pointer actions

| Gesture | Behavior |
| --- | --- |
| `Ctrl+Z` | Undo in the active page context, unless focus is in a text editor |
| `Ctrl+Shift+Z` | Open the active-page/global contextual history manager |
| Right-click outside a text editor | Open that contextual manager at the pointer |
| `Ctrl+Enter` | Request confirmation for the current reviewed plan |
| `Ctrl+W` | Close the active workspace tab |
| `Ctrl+Tab` | Activate the next workspace tab |
| `Ctrl+Shift+Tab` | Activate the previous workspace tab |

Undo appends compensating history; it does not rewrite the past or reverse external bytes already committed by DISM or an installer. See [History Time Machine](History-Time-Machine) and [Safety and Recovery](Safety-and-Recovery).

## Suggested first tour

1. Open the demo on **Overview** and inspect the project/job metrics.
2. Visit **Source & editions** to see the separate source, image, mount, and output fields.
3. Add or remove one reversible item in **Customize**.
4. Open **Review & run** and inspect the resulting command and dependency change without running it.
5. Use `Ctrl+Z`, then inspect the new action in **History & recovery**.
6. Create a test notification from **Settings** and exercise read, dismiss, restore, delete, and undo in the bell drawer.
7. Open **Virtual Machine Lab** to inspect provider discovery and exact-preview safety; no hypervisor is installed by WimForge.
8. Open **Embedded terminal** and read its elevation/bounds notice. Start a session only if you intend to run an administrator command.

The current checked-in image gallery is on [Screenshots](Screenshots).

## 香港粵語導覽

開 app 先會見到工程起始頁，可以建立、開啟、匯入或繼續最近工程。入到工程後，左邊 rail 有十二個功能頁；上面分頁列好似 browser 咁，可以開啟、排位、關閉、改名同改字體樣式。`Ctrl+W` 關而家呢個分頁，`Ctrl+Tab` / `Ctrl+Shift+Tab` 前後切換；工程內 `.wimforge/tabs/.git` 會記錄每次變更。所有確認、通知、復原同 history 都係 app 內非封鎖畫面，唔會將整個 job queue 卡死。

---

[← Getting Started](Getting-Started) · [Projects and Sources →](Projects-and-Sources)
