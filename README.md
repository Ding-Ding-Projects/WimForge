# WimForge

WimForge is a standalone, open-source Windows image customization studio built with C++20, Qt 6.8, and Qt Quick Controls. It combines safe offline DISM orchestration with package, Group Policy, unattended-setup, history, notification, and WinForge-family workflows in one Material desktop application.

It is an independent alternative to tools such as NTLite. WimForge is not affiliated with or endorsed by NTLite, and this early release does not claim one-for-one parity with a mature commercial product. Its distinguishing contract is that configuration changes remain inspectable, Git-backed, portable, and reversible.

[Download the latest Windows x64 release](https://github.com/codingmachineedge/WimForge/releases/latest) · [Read the wiki](docs/wiki/Home.md) · [Report an issue](https://github.com/codingmachineedge/WimForge/issues)

## A tour of the app

![WimForge overview](docs/screenshots/overview.png)

![Package Studio](docs/screenshots/package-studio.png)

![History Time Machine](docs/screenshots/history.png)

The desktop interface is available in English, Hong Kong Cantonese, or a bilingual mode. Controls use icons and text, and consequential feedback stays inside the app as snackbars, a notification drawer, inline validation, recovery sheets, and non-modal Material popups. Servicing jobs keep running while those surfaces are open.

## What is implemented

- Safe image workflow — accept ISO/media/WIM/ESD/SWM sources, inspect indexes from extracted media or image files, clone into a project-owned workspace, review a dependency graph, mount, service, validate, commit, export or split, and optionally create an ISO.
- Broad servicing configuration — drivers, updates and CAB/MSU packages, optional features, capabilities, provisioned Appx packages, component package identifiers, offline registry changes, answer files, staged files, and post-setup work.
- Multi-job execution — independent verification work can run in parallel; writes remain dependency-ordered. The job engine journals transitions for interruption recovery.
- Project Git repository — each project is its own local Git repository. Every successful configuration mutation creates a normal project commit, then attempts the separate action-history append/commit; a secondary history failure leaves the safe project commit in place and raises a persistent warning.
- History Time Machine — append-only, hash-chained action events; guarded selective undo that preserves unrelated later edits; undo-of-undo/redo; restore actions; bookmarks; lightweight history lanes; Git log inspection; and A/B diff viewing.
- Contextual undo anywhere — `Ctrl+Z` undoes in the active context. `Ctrl+Shift+Z`, or a right-click anywhere in the desktop, opens the non-modal active-page/global mini history manager. Element-specific filtering is available in the history core and CLI, but is not wired to every desktop control in this release.
- Git-backed notification center — a separate local repository commits new, read, unread, dismiss, restore, and tombstoned-delete events. Its own latest change can be undone.
- Complete project saves — a `.wimforge` file contains complete project and notification repositories, including hidden `.git` object databases, refs, reflogs, configuration, and undo commits.
- Package Studio — validated profiles for WinGet, npm, pip, signed direct installers, offline payloads, and structured custom executables; dependency ordering; offline/online modes; trust checks; and a resumable first-logon installer.
- Full AI Development template — Git/Git LFS, Node/npm, Python, .NET, Java, Go, Rust, LLVM, CMake, Ninja, Visual Studio Build Tools, VS Code, PowerShell, 7-Zip, Docker, OpenCode, Codex CLI, Claude Code, and Claude Desktop. Desktop payloads without a trustworthy package identity remain disabled slots until the ISO author supplies the official file, hash, signer, and reviewed command.
- Automatic OpenCode setup — shortly after startup, WimForge live-verifies an existing `opencode --version`. If OpenCode is missing, setup runs asynchronously: Node.js LTS is installed through WinGet when npm is absent, followed by `npm install -g opencode-ai@latest`. WimForge reports success only after the executable is found and that live verification exits successfully with nonempty output; progress and failure stay non-modal.
- Group Policy Studio — reads all ADMX definitions and installed ADML languages from the selected PolicyDefinitions store, retains schema constraints and registry actions, creates schema-driven Material editors, supports text/validated-regex search, exports bilingual documentation, and can ask OpenCode to propose a search.
- Unattended Studio — portable JSON profiles, Windows answer-file XML import/export, the seven setup passes, Full Automation and AI Development templates, Random/Fixed/Prompt/Serial computer-name modes, Microsoft-published GVLK selection with licensing warnings, and OpenCode-assisted fills that are validated before commit.
- WinForge Bridge — records approved typed actions, can include a complete self-contained WinForge runtime, and stages a verified, resumable OEM bundle into installation media. Runtime capabilities are contract-checked; WimForge never guesses unsupported WinForge command-line switches.
- Complete CLI — project/config editing, plan/dry-run/apply, Git and contextual history, notification events, `.wimforge` bundles, unattended profiles, Package Studio, installed GPO catalogs, WinForge recipe validation/staging, deterministic JSON output, response files, and stable exit codes.

## The core workflow

1. Create or open a project. WimForge initializes its local Git history.
2. Select a legally obtained Windows ISO, media folder, WIM, ESD, or SWM source. For immediate index inspection, point the Image path at `sources\install.wim`, `.esd`, or the first `.swm`; a raw ISO is mounted/copied only by the reviewed servicing plan.
3. Configure image changes in Customize, Group Policy Studio, Unattended Studio, Package Studio, and WinForge Bridge.
4. Open Review & Run. Inspect every executable, argument token, dependency, destructive flag, and bilingual description.
5. Run only after validation succeeds. Keep the original source unchanged and test the output in a disposable virtual machine.
6. Export a complete `.wimforge` save when the project and all local histories need to travel together.

WimForge uses Windows' servicing tools rather than replacing them. DISM performs image servicing, and `oscdimg` is required when the selected plan creates bootable ISO media. Review the generated plan before elevation.

## Safety and recovery model

Offline projects clone by default. The selected ISO, media tree, WIM, ESD, or SWM set is read for verification and preparation while image writes target project-owned working paths. Input and staged-file hashes form gates in the operation graph. Final image, split-image, workspace, and ISO publication uses partial and backup paths so an interrupted copy is not presented as a completed output.

Commands are stored as an executable plus an argument array. Package and WinForge profiles reject shell wrappers, embedded command strings, traversal, unsafe script hosts, and missing trust data where it is required. Elevated servicing is explicit; the per-user installer itself requests no administrator rights.

The recovery journal detects interrupted work and preserves operation/dependency state for review. WimForge deliberately rebuilds the plan instead of blindly skipping an external step whose completion cannot be proven. Its elevated safe-unmount action runs DISM `/Unmount-Image /Discard` against the mount path captured by the interrupted journal—not a subsequently edited project path. A start or DISM failure leaves that journal untouched; success atomically closes it as recovered/discarded. Configuration undo remains separate and does not claim to reverse external side effects. Keep pristine source media, test backups, and a disposable VM.

Read [Image Servicing](docs/wiki/Image-Servicing.md) and [Safety and Recovery](docs/wiki/Safety-and-Recovery.md) before using a production image.

## History that does not erase history

WimForge has two complementary project timelines:

- `project.json` and related project files are saved into the project's normal Git repository.
- `.wimforge/action-history.jsonl` is an immutable event journal in that repository. Every event links to the previous event by SHA-256 and gets its own Git commit.

An ordinary desktop project action stores minimal forward and inverse JSON merge patches. Undo appends a compensation event; it does not delete the original. Undoing that compensation appends another event that reapplies the original change. Selective undo uses the same model for an older effective action, applies its patch to the current project, preserves unrelated later edits, and refuses the operation if a later change touched the same target path. An explicit restore point remains available when replacing the complete project state is intentional. Bookmarks and history lanes are append-only too.

The notification center has a separate Git repository because notification lifecycle changes should survive independently from the open project. A delete is a recoverable tombstone. Complete `.wimforge` exports carry both repositories.

This history covers WimForge configuration state. It does not claim to undo side effects that have already escaped the project transaction boundary. See [History Time Machine](docs/wiki/History-Time-Machine.md), [Notification Center](docs/wiki/Notification-Center.md), and [Project Bundles](docs/wiki/Project-Bundles.md).

## Package Studio and AI development media

Package Studio exports portable JSON and stages a verified first-logon bundle: profile, manifest, runner, elevated-task registration script, and selected offline payloads. SetupComplete only registers an `AtLogOn` scheduled task for the local Administrators group at highest run level. The first account expected to run the package plan must therefore be a local administrator. The task verifies software live, writes atomic per-package state, retries with bounded backoff, refreshes `PATH`, and stops on failure; it remains registered so a later administrator logon retries, and unregisters only after every enabled package succeeds. Signed direct/offline installers require SHA-256 and Authenticode publisher checks.

The Full AI Development profile includes verified package identities where one exists. OpenCode and Codex CLI use their vendors' npm packages. Claude Code and Claude Desktop use the exact WinGet identities recorded in the profile. OpenCode Desktop, the Codex app, and ChatGPT Desktop remain optional official-payload slots: WimForge intentionally does not invent WinGet identifiers or redistribute unreviewed installers.

Package metadata is not a redistribution license and does not bypass vendor sign-in, subscriptions, activation, hardware requirements, or terms. See [Package Studio](docs/wiki/Package-Studio.md).

## Group Policy and unattended setup

Group Policy Studio reads a real installed or copied ADMX/ADML store instead of shipping a guessed policy list. Browsing, searching, and documentation export are read-only. Applying a selected policy translates the exact enabled/disabled registry operations and validated element values into project registry changes.

Unattended Studio separates editable WimForge JSON from exported Windows answer-file XML. Its validation catches WimForge schema errors, but only Windows System Image Manager can validate a file against the exact target image/catalog. Prompt and serial computer names are implemented as explicit first-logon behavior; invalid NTLite-style literals such as `[Prompt]` are never written as Microsoft `ComputerName` values.

See [Group Policy Studio](docs/wiki/Group-Policy-Studio.md) and [Unattended Studio](docs/wiki/Unattended-Studio.md).

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
- Git on `PATH` for project and notification repositories
- DISM, included with Windows
- administrative access when an image operation needs it
- enough free storage for source clones, the mount, scratch data, staged payloads, and output
- WinGet plus network access if automatic Node/OpenCode or online package installation is wanted
- Windows ADK Deployment Tools (`oscdimg`) when creating ISO output

The portable zip includes deployed Qt and MSVC runtime files. The installer is per-user and does not permanently elevate WimForge.

## Build from source

Build prerequisites are Visual Studio 2022 Build Tools with Desktop development with C++, CMake 3.24+, Qt 6.8 for MSVC 2022 x64 (Core, Gui, Qml, Quick, Quick Controls 2), Git, and PowerShell. Ninja is needed by the release script.

Using the Visual Studio generator:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 `
  -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\WimForge.exe --demo
```

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
| Source/index inspection | Implemented for media/WIM/ESD/SWM; raw ISO is a servicing-plan source | Point inspection at the ISO's mounted/extracted `sources\install.*`; not commercial-parity inspection |
| Driver, update and package integration | Implemented as reviewed DISM operations | Payload acquisition is the user's responsibility |
| Features and capabilities | Implemented | Uses Windows identities; no curated compatibility recommendations |
| Appx provisioning/removal | Implemented | No store browser or live application inventory equivalent |
| Component removal | Low-level identifier workflow | No NTLite-equivalent component database, dependency intelligence, or compatibility guarantees |
| Registry and installed GPO policies | Implemented | GPO catalog is based on the ADMX/ADML store supplied to WimForge |
| Unattended setup and post-setup | Implemented, with templates and XML/JSON round trips | Must still be validated in Windows SIM and a VM |
| WIM/ESD/SWM export and ISO creation | Implemented plan/pipeline | ISO output needs ADK `oscdimg`; production breadth is still growing |
| Live/online servicing | Core plan supports `/Online` operations | Desktop workflow is primarily designed around offline, cloned media |
| Presets/project portability | Git-backed JSON plus complete `.wimforge` saves | Bundle format is WimForge-specific and deliberately uncompressed |
| Undo/history | Event-sourced project configuration and notification history | Cannot undo external bytes after they are committed/applied |
| Integrated update downloader, host refresh, compatibility database | Not implemented | Supply payloads and validate applicability yourself |
| Licensing | MIT open source | NTLite uses its own commercial/free licensing model |

Read the expanded [NTLite Feature Comparison](docs/wiki/NTLite-Feature-Comparison.md). NTLite's current product claims belong to its [official feature page](https://www.ntlite.com/features/) and [documentation](https://www.ntlite.com/docs/); consult those sources rather than treating this table as purchasing advice.

## Known limitations

- WimForge is an early project and has not accumulated NTLite's compatibility database, device testing, or years of production edge cases.
- The app orchestrates external Windows tools. DISM, `oscdimg`, WinGet, installer, vendor, network, and target-image failures remain possible.
- Component/package names can be edition- and build-specific. A syntactically valid plan is not proof that Windows will accept every payload.
- Unattended XML requires Windows SIM validation against the exact image and an end-to-end VM install test.
- The history manager reverses recorded project state; it is not a filesystem snapshot engine and cannot rewind arbitrary external side effects.
- Automatic OpenCode installation changes the host's global Node/npm tool set. It reports progress asynchronously, but managed environments should review that policy before first launch.
- Package profiles never supply credentials or bypass vendor authentication. Optional desktop payload slots need author-supplied official installers and trust metadata.
- The audited legacy WinForge runtime supports page deep-links only. Module/tweak replay requires a declared runtime contract.
- `.wimforge` v1 preserves ordinary files and complete Git topology, not NTFS ACLs, alternate data streams, sparse allocation, or extended attributes.
- Release executables and installers are not yet code-signed.

## Documentation map

- [Getting Started](docs/wiki/Getting-Started.md)
- [Image Servicing](docs/wiki/Image-Servicing.md)
- [Package Studio](docs/wiki/Package-Studio.md)
- [Group Policy Studio](docs/wiki/Group-Policy-Studio.md)
- [Unattended Studio](docs/wiki/Unattended-Studio.md)
- [History Time Machine](docs/wiki/History-Time-Machine.md)
- [Notification Center](docs/wiki/Notification-Center.md)
- [Project Bundles](docs/wiki/Project-Bundles.md)
- [CLI](docs/wiki/CLI.md)
- [WinForge Bridge](docs/wiki/WinForge-Bridge.md)
- [Safety and Recovery](docs/wiki/Safety-and-Recovery.md)
- [Building and Releases](docs/wiki/Building-and-Releases.md)
- [NTLite Feature Comparison](docs/wiki/NTLite-Feature-Comparison.md)

## Primary references

- Microsoft: [What is DISM?](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/what-is-dism?view=windows-11), [DISM command-line options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/deployment-image-servicing-and-management--dism--command-line-options?view=windows-10), [answer files overview](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/answer-files-overview), [answer-file authoring practices](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/best-practices-for-authoring-answer-files), [WinGet](https://learn.microsoft.com/en-us/windows/package-manager/winget/), and [KMS client keys](https://learn.microsoft.com/en-us/windows-server/get-started/kms-client-activation-keys)
- AI tools: [OpenCode](https://github.com/anomalyco/opencode), [OpenAI Codex](https://github.com/openai/codex), [Codex app](https://developers.openai.com/codex/app), and [Claude Code](https://code.claude.com/docs/en/getting-started)
- Qt: [Qt deployment for Windows](https://doc.qt.io/qt-6/windows-deployment.html)
- Comparison context: [NTLite features](https://www.ntlite.com/features/) and [NTLite documentation](https://www.ntlite.com/docs/)

## Contributing

Issues and focused pull requests are welcome. Keep destructive operations explicit, retain the no-shell structured-command model, preserve both Git history contracts, add tests for non-UI logic, and do not turn a failed safety gate into a warning-only path.

## License

WimForge is provided under the [MIT License](LICENSE), without warranty. ISO authors remain responsible for Windows licensing, third-party redistribution rights, activation, deployment security, and testing.
