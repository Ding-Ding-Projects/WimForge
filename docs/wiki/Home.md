# WimForge Wiki

WimForge is a standalone, MIT-licensed Windows image customization studio written in C++20 and Qt 6.8. It gives one desktop home to offline Windows servicing, software selection, installed Group Policy definitions, unattended setup, WinForge-family staging, non-blocking notifications, and Git-backed history.

[Source repository](https://github.com/codingmachineedge/WimForge) · [Latest Windows release](https://github.com/codingmachineedge/WimForge/releases/latest) · [Issues](https://github.com/codingmachineedge/WimForge/issues)

WimForge is an independent alternative to NTLite, not an NTLite product. It is early software and does not claim commercial feature parity. The central design is different: project state and user mutations are explicit, locally committed, exportable, and reversible, while the Windows image operation graph remains reviewable before it runs.

## Start here

| Page | What it covers |
| --- | --- |
| [Getting Started](Getting-Started) | Install, first launch, first project, shortcuts, and the end-to-end workflow |
| [Image Servicing](Image-Servicing) | Sources, immutable workspaces, DISM plan ordering, outputs, and online mode |
| [Package Studio](Package-Studio) | Software profiles, Full AI Development, OpenCode, trust, and first-logon staging |
| [Group Policy Studio](Group-Policy-Studio) | Installed ADMX/ADML catalog, search, schema controls, application, and documentation |
| [Unattended Studio](Unattended-Studio) | JSON profiles, Windows answer-file XML, templates, names, product keys, and validation |
| [History Time Machine](History-Time-Machine) | Contextual undo, selective compensation, redo-of-undo, bookmarks, lanes, and diffs |
| [Notification Center](Notification-Center) | The separate Git ledger for read, dismiss, delete, restore, and undo events |
| [Project Bundles](Project-Bundles) | Complete `.wimforge` saves with all local Git data |
| [CLI](CLI) | Headless automation, deterministic JSON, response files, and exit codes |
| [WinForge Bridge](WinForge-Bridge) | Typed recipes, runtime contracts, OEM staging, and resumable replay |
| [Safety and Recovery](Safety-and-Recovery) | Trust boundaries, atomic publication, crash recovery, and deployment checklist |
| [Building and Releases](Building-and-Releases) | Qt/CMake build, tests, installer, portable package, and release automation |
| [NTLite Feature Comparison](NTLite-Feature-Comparison) | An honest implemented/partial/not-implemented comparison |

## The application model

A project is a normal directory with a versioned `project.json`, supporting profiles, a `.wimforge` working area, and its own `.git` directory. Output-affecting UI mutations are first saved as project commits, then appended as action-history events and commits. If that secondary history append fails, WimForge preserves the already-safe project commit and creates a persistent warning. The notification center uses another local Git repository so notification state survives independently from whichever project is open.

The image pipeline is an explicit dependency graph. It verifies inputs, clones the source into project-owned work paths, performs ordered writes, validates and unmounts, then publishes output atomically. The original source is not the default write target.

Package, GPO, unattended, and WinForge configuration feeds that same project and history model. A complete `.wimforge` export packages the project and notification repositories, including their hidden Git object databases and undo commits.

## Interface promises

- Material-styled controls with icons and text
- English, Hong Kong Cantonese, and bilingual display modes
- Non-modal in-app sheets, snackbars, inline validation, and a notification drawer
- Jobs continue while informational or editing surfaces are open
- `Ctrl+Z` for the active context
- `Ctrl+Shift+Z` or a desktop right-click for the active-page/global mini history manager; element-level filtering is available in the core and CLI
- `Ctrl+Enter` to review the current run request

## Important boundary

WimForge history reverses recorded configuration state. Selective desktop undo guarded-applies a minimal merge patch, preserving unrelated later edits and rejecting same-path conflicts. It cannot rewind bytes after DISM commits an external image, uninstall arbitrary host software, revoke a leaked credential, or reverse an action outside its transaction boundary. Keep pristine media, validate answer files against the exact target, and boot-test output in a disposable virtual machine.

Microsoft's [DISM overview](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/what-is-dism?view=windows-11) and [answer-files overview](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/answer-files-overview) are the authoritative starting points for the Windows technologies WimForge orchestrates.

---

[Getting Started →](Getting-Started)
