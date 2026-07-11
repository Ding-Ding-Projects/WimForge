# WimForge

WimForge is a standalone, open-source Windows image customization studio built with C++20, Qt 6.8, and Qt Quick Controls. It combines safe offline DISM orchestration with package, Group Policy, unattended-setup, history, notification, and WinForge-family workflows in one Material desktop application.

It is an independent alternative to tools such as NTLite. WimForge is not affiliated with or endorsed by NTLite, and this early release does not claim one-for-one parity with a mature commercial product. Its distinguishing contract is that configuration changes remain inspectable, Git-backed, portable, and reversible.

[Open the WimForge website](https://codingmachineedge.github.io/WimForge/) · [Read the Material documentation](https://codingmachineedge.github.io/WimForge/docs/) · [Search the full Wiki](https://codingmachineedge.github.io/WimForge/docs/wiki/) · [Browse Windows releases](https://github.com/codingmachineedge/WimForge/releases) · [Report an issue](https://github.com/codingmachineedge/WimForge/issues)

## A tour of the app

![WimForge overview](docs/screenshots/overview.png)

![Package Studio](docs/screenshots/package-studio.png)

![Virtual Machine Lab](docs/screenshots/virtual-machine-lab.png)

![History Time Machine](docs/screenshots/history.png)

![Embedded terminal](docs/screenshots/embedded-terminal.png)

The desktop interface is available in English, Hong Kong Cantonese, or a bilingual mode. Controls use icons and text, and consequential feedback stays inside the app as snackbars, a notification drawer, inline validation, recovery sheets, and non-modal Material popups. Servicing jobs keep running while those surfaces are open.

## What is implemented

- Safe image workflow — accept ISO/media/WIM/ESD/SWM sources, inspect indexes from extracted media or image files, clone into a project-owned workspace, review a dependency graph, mount, service, validate, commit, export or split, and optionally create an ISO.
- Broad servicing configuration — drivers, updates and CAB/MSU packages, optional features, capabilities, provisioned Appx packages, component package identifiers, offline registry changes, answer files, staged files, and post-setup work.
- Multi-job execution — independent verification work can run in parallel; writes remain dependency-ordered. The job engine journals transitions for interruption recovery.
- Project Git repository — each project is its own local Git repository. Every successful configuration mutation creates a normal project commit, then attempts the separate action-history append/commit; a secondary history failure leaves the safe project commit in place and raises a persistent warning.
- History Time Machine — append-only, hash-chained action events; guarded selective undo that preserves unrelated later edits; undo-of-undo/redo; restore actions; bookmarks; lightweight history lanes; Git log inspection; and A/B diff viewing.
- Contextual undo anywhere — `Ctrl+Z` undoes in the active context. `Ctrl+Shift+Z`, or a right-click anywhere in the desktop, opens the non-modal active-page/global mini history manager. Element-specific filtering is available in the history core and CLI, but is not wired to every desktop control in this release.
- Git-backed notification center — a separate local repository commits new, read, unread, dismiss, restore, and tombstoned-delete events. Its own latest change can be undone.
- Complete project saves — a `.wimforge` file carries the project repository, its nested workspace-tab repository, and the separate notification repository. Project and notification history retain their hidden Git databases; imported tab repositories retain history while executable Git controls are neutralized before the elevated app uses them.
- Browser-style project tabs — every page can live in a movable, closable, renameable tab with per-tab font family, size, color, bold, italic, and strikeout. A dedicated local Git repository records every tab change; portable definitions and the complete tab repository can each be exported or imported as one file.
- Structured diagnostics — GUI and CLI sessions, project actions, source inspection, host-driver export, scheduler transitions, and Job Engine child-process lifecycle/output are written as rotating JSONL with session/sequence/thread/source metadata and recursive secret-pattern redaction; Settings opens the active log or folder.
- Package Studio — validated profiles for WinGet, npm, pip, signed direct installers, offline payloads, and structured custom executables; dependency ordering; offline/online modes; trust checks; and a resumable first-logon installer.
- Full AI Development template — Git/Git LFS, Node/npm, Python, .NET, Java, Go, Rust, LLVM, CMake, Ninja, Visual Studio Build Tools, VS Code, PowerShell, 7-Zip, Docker, OpenCode, Codex CLI, Claude Code, and Claude Desktop. Desktop payloads without a trustworthy package identity remain disabled slots until the ISO author supplies the official file, hash, signer, and reviewed command.
- Explicit OpenCode host setup — the elevated desktop does not discover or launch PATH/user-profile developer tools at startup. After the operator selects **Verify / install now** in Package Studio, WimForge live-verifies `opencode --version`; if approved setup is needed, it can install Node.js LTS through WinGet and then `opencode-ai@latest` through npm. Progress and failure stay non-modal, and assisted actions remain disabled until verification succeeds.
- Group Policy Studio — reads all ADMX definitions and installed ADML languages from the selected PolicyDefinitions store, retains schema constraints and registry actions, creates schema-driven Material editors, supports text/validated-regex search, exports bilingual documentation, and can ask OpenCode to propose a search.
- Unattended Studio — portable JSON profiles, Windows answer-file XML import/export with native file pickers, the seven setup passes, Full Automation and AI Development templates, Random/Fixed/Prompt/Serial computer-name modes, typed convenience cards for OOBE screens, regional and language settings, owner/time-zone/licensing, OEM information, and Narrator sign-in autostart, Microsoft-published GVLK selection with licensing warnings, and OpenCode-assisted fills that are validated before commit.
- Docker provisioning — a non-root Linux service and one-shot renderer that maps UUID/serial/MAC inventory to a validated fixed pre-OOBE computer name, an operator profile, and typed locale/time-zone/OOBE overrides; an included fail-closed WinPE client supplies the result to `setup.exe /unattend` before installation.
- Virtual Machine Lab — discovers VMware Workstation/Player and VirtualBox, manages project-scoped and external machines, powers and snapshots them, attaches installation media, and records profile-aware validation milestones behind immutable previews and typed destructive confirmations.
- Embedded terminal — hosts trusted PowerShell or Command Prompt shells through the documented Windows ConPTY API, keeps bounded output and task status inside WimForge, resizes with the UI, and contains the shell process tree without opening a separate console window.
- Elevated desktop — the GUI manifest requests administrator rights at launch because image servicing and VM workflows require them; embedded-terminal commands inherit that elevation and remain explicitly operator-controlled.
- WinForge Bridge — records approved typed actions, can include a complete self-contained WinForge runtime, and stages a verified, resumable OEM bundle into installation media. Runtime capabilities are contract-checked; WimForge never guesses unsupported WinForge command-line switches.
- Complete CLI — project/config editing, plan/dry-run/apply, Git and contextual history, notification events, `.wimforge` bundles, unattended profiles, Package Studio, installed GPO catalogs, WinForge recipe validation/staging, deterministic JSON output, response files, and stable exit codes.

## The core workflow

1. Create or open a project. WimForge initializes its local Git history.
2. Use **Browse ISO / image** or **Browse media folder** to select a legally obtained Windows ISO, media folder, WIM, ESD, or SWM source. A raw ISO is mounted read-only for immediate DISM inventory, dismounted after inspection, and recorded by its stable internal `sources/install.*` path; the reviewed servicing plan later extracts it into the project-owned workspace and converts ESD/SWM input to a serviceable WIM before mounting.
3. Configure image changes in Customize, Group Policy Studio, Unattended Studio, Package Studio, and WinForge Bridge.
4. Open Review & Run. Inspect every executable, argument token, dependency, destructive flag, and bilingual description.
5. Run only after validation succeeds. Keep the original source unchanged and test the output in a disposable virtual machine.
6. Export a complete `.wimforge` save when the project and all local histories need to travel together.

WimForge uses Windows' servicing tools rather than replacing them. DISM performs image servicing, and `oscdimg` is required when the selected plan creates bootable ISO media. The desktop requests elevation when it launches; review the generated plan before execution.

## Safety and recovery model

Offline projects clone by default. The selected ISO, media tree, WIM, ESD, or SWM set is read for verification and preparation while image writes target project-owned working paths. Input and staged-file hashes form gates in the operation graph. Final image, split-image, workspace, and ISO publication uses partial and backup paths so an interrupted copy is not presented as a completed output.

Commands are stored as an executable plus an argument array. Package and WinForge profiles reject shell wrappers, embedded command strings, traversal, unsafe script hosts, and missing trust data where it is required. Windows inbox tools resolve through protected System32 paths rather than the current directory. Because the desktop always elevates, the installer requires administrator approval and installs under protected Program Files.

The recovery journal detects interrupted work and preserves operation/dependency state for review. WimForge deliberately rebuilds the plan instead of blindly skipping an external step whose completion cannot be proven. Its elevated safe-unmount action runs DISM `/Unmount-Image /Discard` against the mount path captured by the interrupted journal—not a subsequently edited project path. A start or DISM failure leaves that journal untouched; success atomically closes it as recovered/discarded. Configuration undo remains separate and does not claim to reverse external side effects. Keep pristine source media, test backups, and a disposable VM.

Read [Image Servicing](docs/wiki/Image-Servicing.md) and [Safety and Recovery](docs/wiki/Safety-and-Recovery.md) before using a production image.

## History that does not erase history

WimForge has two complementary project timelines:

- `project.json` and related project files are saved into the project's normal Git repository.
- `.wimforge/action-history.jsonl` is an immutable event journal in that repository. Every event links to the previous event by SHA-256 and gets its own Git commit.

An ordinary desktop project action stores minimal forward and inverse JSON merge patches. Undo appends a compensation event; it does not delete the original. Undoing that compensation appends another event that reapplies the original change. Selective undo uses the same model for an older effective action, applies its patch to the current project, preserves unrelated later edits, and refuses the operation if a later change touched the same target path. An explicit restore point remains available when replacing the complete project state is intentional. Bookmarks and history lanes are append-only too.

The notification center has a separate Git repository because notification lifecycle changes should survive independently from the open project. A delete is a recoverable tombstone. Complete `.wimforge` exports carry the project, nested workspace-tab, and notification repositories.

This history covers WimForge configuration state. It does not claim to undo side effects that have already escaped the project transaction boundary. See [History Time Machine](docs/wiki/History-Time-Machine.md), [Notification Center](docs/wiki/Notification-Center.md), and [Project Bundles](docs/wiki/Project-Bundles.md).

## Package Studio and AI development media

Package Studio exports portable JSON and stages a verified first-logon bundle: profile, manifest, runner, elevated-task registration script, and selected offline payloads. SetupComplete only registers an `AtLogOn` scheduled task for the local Administrators group at highest run level. The first account expected to run the package plan must therefore be a local administrator. The task verifies software live, writes atomic per-package state, retries with bounded backoff, refreshes `PATH`, and stops on failure; it remains registered so a later administrator logon retries, and unregisters only after every enabled package succeeds. Signed direct/offline installers require SHA-256 and Authenticode publisher checks.

The Full AI Development profile includes verified package identities where one exists. OpenCode and Codex CLI use their vendors' npm packages. Claude Code and Claude Desktop use the exact WinGet identities recorded in the profile. OpenCode Desktop, the Codex app, and ChatGPT Desktop remain optional official-payload slots: WimForge intentionally does not invent WinGet identifiers or redistribute unreviewed installers.

Package metadata is not a redistribution license and does not bypass vendor sign-in, subscriptions, activation, hardware requirements, or terms. See [Package Studio](docs/wiki/Package-Studio.md).

## Group Policy and unattended setup

Group Policy Studio reads a real installed or copied ADMX/ADML store instead of shipping a guessed policy list. Browsing, searching, and documentation export are read-only. Applying a selected policy translates the exact enabled/disabled registry operations and validated element values into project registry changes.

Unattended Studio separates editable WimForge JSON from exported Windows answer-file XML. Its validation catches WimForge schema errors, but only Windows System Image Manager can validate a file against the exact target image/catalog. Prompt and serial computer names are implemented as explicit first-logon behavior; invalid NTLite-style literals such as `[Prompt]` are never written as Microsoft `ComputerName` values.

See [Group Policy Studio](docs/wiki/Group-Policy-Studio.md) and [Unattended Studio](docs/wiki/Unattended-Studio.md).

For central per-device assignment, [Docker Provisioning](docs/docker-provisioning.md) documents the container, inventory/API, token/TLS boundary, one-shot rendering, and WinPE/PXE handoff. A remote service alone is not an answer-file discovery path: the included bootstrap downloads the rendered file before launching Windows Setup. The built-in baseline does not choose a disk, edition, or account, so a genuinely no-touch deployment still needs an exact-image operator profile plus Windows SIM and clean-VM validation.

## WinForge family integration

The bridge converts approved intent into a declarative recipe whose actions are typed as page, module, tweak, direct command, registry, or verified copy operations. Recipes have stable IDs, idempotency keys, canonical digests, machine/user phases, and strict runtime capability checks. Staging writes the recipe, manifest, bootstrap, payloads, optional runtime, and SetupComplete integration beneath the conventional `sources/$OEM$` media tree. A missing SetupComplete file is created; an existing ordinary text file is atomically hooked while preserving its prior bytes. Unsupported oversized or NUL/UTF-16 content fails staging instead of reporting an unreachable hook as success.

The audited adjacent WinForge `v1.0.177` runtime exposes only `--page <alias>` as a relevant documented capability. Module and tweak replay therefore remains disabled for that legacy runtime. A future runtime must publish `winforge-contract.json` before WimForge will invoke additional capabilities. Direct typed registry/copy/command actions remain governed by the bridge's own validator.

See [WinForge Bridge](docs/wiki/WinForge-Bridge.md).

## Command line

Release packages include the console-subsystem `WimForgeCli.exe`. The GUI binary also recognizes CLI command names, but the dedicated executable gives scripts reliable console attachment and exit behavior.

```powershell
.\WimForgeCli.exe --project C:\Images\MyProject project validate --execution
.\WimForgeCli.exe --project C:\Images\MyProject dry-run --script review.ps1 --json
.\WimForgeCli.exe --project C:\Images\MyProject apply --yes

.\WimForgeCli.exe package template ai-development --output ai-packages.json
.\WimForgeCli.exe gpo search "restart deadline" --json
.\WimForgeCli.exe --project C:\Images\MyProject bundle export MyProject.wimforge
.\WimForgeCli.exe winforge detect C:\Tools\WinForge\publish --json
.\WimForgeCli.exe winforge stage workstation.winforge.json `
  --iso D:\IsoWorkspace --runtime C:\Tools\WinForge\publish --include-runtime
```

CLI commands never wait for interactive terminal input. Destructive apply and software installation require `--yes`; without it they return the confirmation-required exit code. `--json` emits one deterministic JSON envelope. See [CLI](docs/wiki/CLI.md) and the detailed [CLI reference](docs/cli.md).

## Requirements

To run a release:

- Windows 10 version 1809 or newer, or Windows 11, x64
- a trusted machine-wide Git for Windows installation under protected Program Files for the project, nested workspace-tab, and notification repositories; user-profile/PATH-only Git copies are rejected by the elevated desktop
- DISM, included with Windows
- administrator approval at desktop launch (and when installing the protected Program Files package)
- enough free storage for source clones, the mount, scratch data, staged payloads, and output
- WinGet plus network access if the operator explicitly approves host Node/OpenCode setup or wants online package installation
- Windows ADK Deployment Tools (`oscdimg`) when creating ISO output

The portable zip includes deployed Qt and MSVC runtime files. The installer requires administrator approval and places those binaries under protected Program Files; launching the desktop invokes the normal Windows UAC consent flow because its manifest requires administrator rights. A portable copy cannot protect its adjacent DLLs: extract it only into a trusted, access-controlled folder that unprivileged processes cannot modify, and never elevate a loose copy from Downloads, Temp, a shared folder, or another writable location.

## Build from source

Build prerequisites are Visual Studio 2022 Build Tools with Desktop development with C++, an x64 Windows SDK including `rc.exe` and `mt.exe`, CMake 3.24+, Qt 6.8 for MSVC 2022 x64 (Core, Gui, Qml, Quick, Quick Controls 2, and Quick Dialogs 2), Python 3.10+ for the provisioning tests, Git, and 64-bit Windows PowerShell 5.1+. Ninja and Inno Setup 6 are needed by the release script.

On Windows 10/11 x64, the maintained bootstrap can inspect or repair that release toolchain, detect the current clean checkout or clone canonical WimForge when none is present, run the release tests, and verify both packaged artifacts:

```powershell
.\scripts\bootstrap-build.ps1 -Plan
.\scripts\bootstrap-build.ps1
```

Start the real run from a normal, non-administrator PowerShell session. Per-user Ninja/aqt repair happens under that original identity; the script requests UAC only when a bounded machine package-repair child is needed and passes it the already validated, signed WinGet path. That child exits before Qt archives are installed into the user profile or any repository, build, test, or packaging work begins, including when UAC uses separate administrator credentials. Automatic installation requires Microsoft App Installer/WinGet, network access, vendor availability, and adequate disk space. It refuses dirty source because a commit-only `build-info.json` could not describe such an artifact, builds from a unique local clone pinned to the verified commit so ignored files and stale outputs cannot leak into the release, never resets or cleans source, retains a transcript, and prints SHA-256 for the final installer and portable zip. WinGet package IDs are exact but catalog versions can change over time, so the log and hashes provide traceability rather than identical toolchains across dates. See [Building and Releases](docs/wiki/Building-and-Releases.md) and review the script before elevation.

Using the Visual Studio generator:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 `
  -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
$runtime = Join-Path (Resolve-Path .).Path 'build\dev-runtime'
cmake --install build --config Debug --prefix $runtime
& "$runtime\WimForge.exe" --demo
```

The install step deploys the matching Qt/MSVC runtime beside the developer executable. A bare Visual Studio output such as `build\Debug\WimForge.exe` is not self-contained and will report a missing `Qt6Guid.dll` unless the matching Qt `bin` directory is already on `PATH`.

Or set `QT_ROOT_DIR`, put Qt's `bin` directory on `PATH`, and use Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="$env:QT_ROOT_DIR" -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Installer, portable build, and automatic releases

Install Inno Setup 6, set `QT_ROOT_DIR`, and run:

```powershell
.\scripts\build-release.ps1 -Version 0.1.0
```

The script rebuilds, runs CTest, stages both executables and Qt dependencies with `windeployqt`, and creates these files under `dist/`:

- `WimForge-Setup-x64-0.1.0.exe`
- `WimForge-portable-x64-0.1.0.zip`

Every push to `main` (and a manual release-workflow run launched from `main`) builds on Windows and publishes a final GitHub Release tagged `v0.1.<run-number>`. The two assets go directly to a verified draft and are published only after their GitHub SHA-256 digests match the local setup executable and portable zip; the workflow never uploads Actions artifacts. Releases are not currently code-signed. See [Building and Releases](docs/wiki/Building-and-Releases.md).

## Honest NTLite comparison

| Workflow | WimForge status | Important difference |
| --- | --- | --- |
| Source/index inspection | Implemented for ISO/media/WIM/ESD/SWM | Native source pickers; raw ISO is mounted read-only for inventory and dismounted before its stable internal image path is saved |
| Driver, update and package integration | Implemented as reviewed DISM operations | Payload acquisition is the user's responsibility, except updates and drivers fetched via the in-app Microsoft Update Catalog |
| Features and capabilities | Implemented | Uses Windows identities; no curated compatibility recommendations |
| Appx provisioning/removal | Implemented | No store browser or live application inventory equivalent |
| Component removal | Low-level identifier workflow | No NTLite-equivalent component database, dependency intelligence, or compatibility guarantees |
| Registry and installed GPO policies | Implemented | GPO catalog is based on the ADMX/ADML store supplied to WimForge |
| Unattended setup and post-setup | Implemented, with templates and XML/JSON round trips | Must still be validated in Windows SIM and a VM |
| WIM/ESD/SWM export and ISO creation | Implemented plan/pipeline | ISO output needs ADK `oscdimg`; production breadth is still growing |
| Live/online servicing | Core plan supports `/Online` operations | Desktop workflow is primarily designed around offline, cloned media |
| Presets/project portability | Git-backed JSON plus complete `.wimforge` saves | Bundle format is WimForge-specific and deliberately uncompressed |
| Undo/history | Event-sourced project configuration and notification history | Cannot undo external bytes after they are committed/applied |
| Integrated update downloader | In-app Microsoft Update Catalog search and download | Restricted to trusted Microsoft hosts; files land in the reviewed update/driver queue; no applicability resolver or persistent cache yet |
| Host refresh, compatibility database | Not implemented | Validate applicability yourself |
| Licensing | MIT open source | NTLite uses its own commercial/free licensing model |

Read the expanded [NTLite Feature Comparison](docs/wiki/NTLite-Feature-Comparison.md). NTLite's current product claims belong to its [official feature page](https://www.ntlite.com/features/) and [documentation](https://www.ntlite.com/docs/); consult those sources rather than treating this table as purchasing advice.

## Known limitations

- WimForge is an early project and has not accumulated NTLite's compatibility database, device testing, or years of production edge cases.
- The app orchestrates external Windows tools. DISM, `oscdimg`, WinGet, installer, vendor, network, and target-image failures remain possible.
- Component/package names can be edition- and build-specific. A syntactically valid plan is not proof that Windows will accept every payload.
- Unattended XML requires Windows SIM validation against the exact image and an end-to-end VM install test.
- The history manager reverses recorded project state; it is not a filesystem snapshot engine and cannot rewind arbitrary external side effects.
- Operator-approved OpenCode host installation changes the global Node/npm tool set. Nothing is discovered or launched automatically at desktop startup; managed environments should review the action before selecting **Verify / install now**.
- Package profiles never supply credentials or bypass vendor authentication. Optional desktop payload slots need author-supplied official installers and trust metadata.
- The audited legacy WinForge runtime supports page deep-links only. Module/tweak replay requires a declared runtime contract.
- `.wimforge` v1 preserves ordinary files and complete Git topology, not NTFS ACLs, alternate data streams, sparse allocation, or extended attributes.
- Release executables and installers are not yet code-signed.

## Documentation map

- [Material documentation site](https://codingmachineedge.github.io/WimForge/)
- [Search the full Wiki](https://codingmachineedge.github.io/WimForge/wiki-search/)
- [Application Tour](docs/wiki/Application-Tour.md)
- [Getting Started](docs/wiki/Getting-Started.md)
- [Projects and Sources](docs/wiki/Projects-and-Sources.md)
- [Customize](docs/wiki/Customize.md)
- [Image Servicing](docs/wiki/Image-Servicing.md)
- [Package Studio](docs/wiki/Package-Studio.md)
- [Group Policy Studio](docs/wiki/Group-Policy-Studio.md)
- [Unattended Studio](docs/wiki/Unattended-Studio.md)
- [Docker Provisioning](docs/docker-provisioning.md)
- [Virtual Machine Lab](docs/wiki/Virtual-Machine-Lab.md)
- [Embedded Terminal](docs/wiki/Embedded-Terminal.md)
- [Review and Run](docs/wiki/Review-and-Run.md)
- [History Time Machine](docs/wiki/History-Time-Machine.md)
- [Notification Center](docs/wiki/Notification-Center.md)
- [Project Bundles](docs/wiki/Project-Bundles.md)
- [CLI](docs/wiki/CLI.md)
- [WinForge Bridge](docs/wiki/WinForge-Bridge.md)
- [Settings](docs/wiki/Settings.md)
- [Safety and Recovery](docs/wiki/Safety-and-Recovery.md)
- [Troubleshooting](docs/wiki/Troubleshooting.md)
- [Architecture and Data Layout](docs/wiki/Architecture-and-Data-Layout.md)
- [Building and Releases](docs/wiki/Building-and-Releases.md)
- [Contributing](docs/wiki/Contributing.md)
- [NTLite Feature Comparison](docs/wiki/NTLite-Feature-Comparison.md)

## Primary references

- Microsoft: [What is DISM?](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/what-is-dism?view=windows-11), [DISM command-line options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/deployment-image-servicing-and-management--dism--command-line-options?view=windows-10), [answer files overview](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/answer-files-overview), [answer-file authoring practices](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/best-practices-for-authoring-answer-files), [WinGet](https://learn.microsoft.com/en-us/windows/package-manager/winget/), and [KMS client keys](https://learn.microsoft.com/en-us/windows-server/get-started/kms-client-activation-keys)
- AI tools: [OpenCode](https://github.com/anomalyco/opencode), [OpenAI Codex](https://github.com/openai/codex), [Codex app](https://developers.openai.com/codex/app), and [Claude Code](https://code.claude.com/docs/en/getting-started)
- Qt: [Qt deployment for Windows](https://doc.qt.io/qt-6/windows-deployment.html)
- Comparison context: [NTLite features](https://www.ntlite.com/features/) and [NTLite documentation](https://www.ntlite.com/docs/)

## 香港粵語重點

- WimForge 開啟時會先請求管理員權限；安裝版會放去受保護嘅 Program Files。可攜式版請解壓去只有你可以改嘅資料夾，唔好由 Downloads、Temp 或 shared folder 直接提權。
- 開 app 先會見到類似 Visual Studio 嘅工程管理頁，可以建立、開啟、匯入工程，亦可以由最近清單繼續。
- ISO、WIM、ESD、SWM 同 Windows media folder 都有 file/folder picker；原始 ISO 會唯讀掛載做 inventory，完成後會確認已 dismount。
- Drivers 同 Updates 會顯示實際 INF、CAB、MSU 資料，亦有 Microsoft 官方 Update Catalog 入口；用邊個 payload 同適用性仍然要由映像作者核實。
- 每個頁面都可以做 browser-style 分頁，可改名、排位、改字體/字號/顏色/粗體/斜體/刪除線。分頁會另外寫入工程內 Git，並且可以匯出 `.wftabs` 或完整 `.wftabrepo`。
- 診斷資料會寫入會 rotate 嘅 JSONL log，秘密格式會先遮蔽；分享 log 前仍然要自己審閱一次。
- 界面、文件、release notes 同工程內產生嘅 commit subject 以 English / 香港粵語雙語呈現。

## Contributing

Issues and focused pull requests are welcome. Keep destructive operations explicit, retain the no-shell structured-command model, preserve both Git history contracts, add tests for non-UI logic, and do not turn a failed safety gate into a warning-only path.

Every completed task must keep this README and the canonical Wiki synchronized, refresh and visually verify all thirteen application captures plus both documentation-site screenshots, then land on `main` through an English / Hong Kong Cantonese bilingual commit and verified push. 每個完成嘅 task 都要同步 README 同 canonical Wiki、重新產生兼逐張核對十三張 app 截圖同兩張文件網站截圖，之後用 English / 香港粵語雙語 commit 推上去並落到 `main`，再確認相應 workflow。

## License

WimForge is provided under the [MIT License](LICENSE), without warranty. ISO authors remain responsible for Windows licensing, third-party redistribution rights, activation, deployment security, and testing.
