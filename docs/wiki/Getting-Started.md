# Getting Started

This guide takes a clean WimForge installation through a reviewable first project. Read [Safety and Recovery](Safety-and-Recovery) before using production media.

## Requirements

- Windows 10 version 1809 or newer, or Windows 11, x64
- Git available on `PATH`
- DISM, which is included with Windows
- administrator access for servicing operations that require it
- free space for an immutable source clone, working image, mount, scratch data, staged payloads, and output
- Windows ADK Deployment Tools when creating an ISO with `oscdimg`
- WinGet and network access when automatic OpenCode setup or online Package Studio entries are wanted

Use only legally obtained Windows images and software payloads. WimForge does not grant Windows, application, subscription, or redistribution rights.

## Install or run portable

Download one of the two x64 assets from the [latest release](https://github.com/codingmachineedge/WimForge/releases/latest):

- `WimForge-Setup-x64-<version>.exe` installs per user under Local AppData.
- `WimForge-portable-x64-<version>.zip` can be extracted and run in place.

The installer does not permanently request administrator rights. WimForge asks for elevation only when a selected image operation needs it. Releases are not currently code-signed, so verify the asset source and GitHub-provided digest before use.

## First-launch OpenCode behavior

WimForge locates `opencode` shortly after startup and runs `opencode --version` without blocking the interface. An existing executable is ready only after that check exits normally with code zero and nonempty output. If no executable is found:

1. An existing npm is used when available.
2. Otherwise, WimForge uses WinGet to install exact package ID `OpenJS.NodeJS.LTS` silently and noninteractively.
3. It runs `npm install -g opencode-ai@latest`.
4. It locates the installed executable and runs the same live verification.
5. Only verified completion—or the exact failure—is reported in-app.

This modifies the host's global Node/npm tool set. Managed environments should review that behavior before first launch. If npm and WinGet are both unavailable, the failure becomes a normal notification and the rest of WimForge remains usable.

## Create a project

1. Select **New project**.
2. Enter a project name and an empty/new project folder.
3. WimForge initializes `project.json`, `.wimforge/action-history.jsonl`, and a local `.git` repository.
4. The Source page opens.

The creation sheet is an in-app, non-modal Material popup. It never suspends the application process or an existing job queue.

Every successful configuration mutation is saved atomically and committed. Passive navigation and telemetry are not treated as output-changing actions.

## Select and inspect a source

The servicing source may be an ISO file, extracted Windows media directory, WIM, ESD, or SWM set. Direct index inspection uses a WIM/ESD/SWM path or an extracted media directory; for a raw ISO, mount or extract it and point **Image path** at `sources\install.wim`, `.esd`, or the first `.swm`. The reviewed servicing plan can later mount and clone the raw ISO itself. Keep **clone source** enabled unless you have a specific, tested low-level reason not to; ISO and media sources are always cloned.

Set separate mount and output locations. WimForge validation rejects output that equals or overlaps source, working media, image, or mount paths.

See [Image Servicing](Image-Servicing) for the exact workspace model.

## Configure the result

Use the studios in any useful order:

- **Customize** — drivers, update/package payloads, Windows features, capabilities, Appx, component identifiers, registry edits, answer files, and post-setup work.
- **Group Policy Studio** — search installed ADMX/ADML policies and commit selected registry-backed policy state.
- **Unattended Studio** — start from a template or import JSON/XML, then export a Microsoft answer file.
- **Package Studio** — choose software for first logon, including the Full AI Development profile.
- **WinForge Bridge** — approve declarative WinForge-family and typed deployment actions, then stage them into media.

Changes show a snackbar and become visible in History. Errors create persistent notification entries instead of stopping the program with a native modal dialog.

## Review and run

Open **Review & Run** or press `Ctrl+Enter`.

1. Refresh the plan after configuration changes.
2. Inspect each executable and argument list, dependencies, required elevation, and destructive marker.
3. Resolve missing sources, payloads, mounts, or unsafe path errors.
4. Export a PowerShell review script if a peer-review artifact is useful.
5. Confirm the run in the non-modal in-app sheet.

Independent input verification can run in parallel; writes remain dependency ordered. Do not assume that plan validation proves payload applicability to the selected Windows build.

## Undo and complete saves

| Gesture | Result |
| --- | --- |
| `Ctrl+Z` | Append a compensating action for the active page context |
| `Ctrl+Shift+Z` | Open the contextual mini history manager |
| Right-click anywhere in the desktop | Open the same active-page/global mini manager at the pointer |
| History page | Selective undo/redo, restore, bookmarks, lanes, A/B diffs, raw Git commits |
| Notification drawer | Read/unread, dismiss, restore, tombstone-delete, and undo latest notification event |

Use **Complete save** or export a path ending in `.wimforge` to carry the project and notification repositories together. See [History Time Machine](History-Time-Machine) and [Project Bundles](Project-Bundles).

Desktop project changes use guarded merge-patch undo: unrelated later edits survive a selective undo, while a later edit to the same target path causes a conflict instead of being overwritten. Per-element filtering exists in the history core and CLI; the current desktop right-click handler operates at active-page/global scope.

## Language and demo mode

The Settings page selects English, Hong Kong Cantonese, or bilingual presentation. For a safe populated evaluation project:

```powershell
.\WimForge.exe --demo --language bilingual --page overview
```

Recognized page IDs include `overview`, `source`, `customize`, `gpo`, `unattended`, `packages`, `winforge`, `plan`, `history`, and `settings` in builds where the matching page is linked.

## Before deployment

- Review every plan operation.
- Validate unattended XML in Windows SIM against the exact image/catalog.
- Check package hashes, Authenticode publishers, licenses, and network assumptions.
- For Package Studio media, ensure the first account intended to run the plan is a local administrator; SetupComplete registers an elevated Administrators-group task, which retries on later administrator logons and removes itself only after complete success.
- Keep the pristine source and the `.wimforge` project save.
- Perform a clean install in a disposable VM.
- Inspect Setup/Panther, Package Studio, and WinForge Bridge logs.
- Exercise restart, first-logon, offline-network, resume, and failure paths.

---

[← Home](Home) · [Image Servicing →](Image-Servicing)
