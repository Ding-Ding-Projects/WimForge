# Package Studio

Package Studio lets an ISO author choose software for the installed Windows system. It stores the selection as portable, validated JSON and stages a crash-safe first-logon installer rather than pretending that every desktop application can be injected into an offline WIM.

## Package providers

| Provider | Behavior |
| --- | --- |
| `winget` | Exact package ID, noninteractive agreement flags, optional version/architecture/scope, and optional reviewed silent arguments |
| `npm` | Global npm package with an explicit Node.js dependency |
| `pip` | Invokes pip through the Python launcher, with optional version pin and user scope |
| `direct-signed-installer` | Staged file or HTTPS download, followed by SHA-256 and Authenticode publisher verification |
| `offline-payload` | Media-relative executable with required SHA-256 and Authenticode publisher; no network needed |
| `custom-command` | Explicit executable and argument array; shell/script wrappers are rejected |

Entries retain identifiers, descriptions, provider/version, architecture, scope, dependencies, verification commands, payload location, trust anchors, homepage, license, release page, and notes. Disabled optional entries stay visible without becoming executable work.

Commands are never arbitrary shell strings. Validation rejects command separators, control characters, inline interpreter switches, script hosts such as `cmd.exe`/`powershell.exe`, credentials embedded in URLs, and token/password-looking values.

## Full AI Development template

Select **Full AI Development** to start from a maintained development workstation profile. It includes:

- Git and Git LFS
- Node.js LTS and npm
- Python 3.13
- .NET SDK
- Eclipse Temurin JDK 21
- Go and Rustup
- LLVM/Clang, CMake, and Ninja
- Visual Studio 2022 Build Tools
- Visual Studio Code
- PowerShell 7 and 7-Zip
- Docker Desktop
- OpenCode CLI
- OpenAI Codex CLI
- Claude Code and Claude Desktop
- disabled official-payload slots for OpenCode Desktop, the Codex app, and ChatGPT Desktop

The profile orders Git before Claude Code, Node before npm-based AI clients, and build-system dependencies before consumers. The committed [`templates/ai-development.json`](https://github.com/codingmachineedge/WimForge/blob/main/templates/ai-development.json) is the exact portable profile.

Desktop slots without a verified package identity are intentionally disabled. Enabling one requires an official payload, a current SHA-256, its expected Authenticode publisher, and a reviewed silent structured install command. WimForge does not invent a WinGet ID.

## Host OpenCode setup versus ISO OpenCode setup

These are separate workflows:

- **WimForge host helper:** the elevated desktop performs no PATH/user-profile discovery at startup. The operator selects **Verify / install now** to approve discovery and setup for the current session. WimForge then live-verifies `opencode --version`; if missing, the approved action can install Node.js LTS through WinGet and run `npm install -g opencode-ai@latest`. GPO and unattended helpers use only a verified host installation and never trigger setup implicitly.
- **Target machine package:** the Full AI Development profile puts OpenCode in the target's first-logon dependency plan. It first verifies `opencode --version` and installs only when absent.

Neither workflow embeds credentials or signs a user in. Installation success is verified after the installer returns; a zero installer exit code alone is not accepted when the executable remains missing.

## Network modes

- `online` waits for HTTPS connectivity before the first network package.
- `prefer-offline` uses a staged direct-installer payload first and downloads only when absent.
- `offline` rejects any enabled entry that still requires a network.

Network wait and retry counts are bounded. A failed package writes durable failure state and prevents dependent packages from running.

## Dependency and resume model

Package IDs form a stable directed graph. Validation rejects duplicate IDs, missing/disabled dependencies, self-dependencies, and cycles with a readable path.

The generated PowerShell 5.1-compatible runner:

1. Executes the live verification command before each install.
2. Writes an atomic per-package state file beneath `%ProgramData%\WimForge\PackageStudio\state`.
3. Records package fingerprint, attempt, status, UTC time, and message.
4. Quarantines corrupt markers rather than deleting evidence.
5. Refreshes process `PATH` after installs.
6. Retries with bounded backoff and logs every package/run.
7. Stops on failure so dependants do not install into a broken state.
8. Resumes on the next invocation and skips software already verified.

If an installed marker exists but the program is gone, verification causes a reinstall. If the program exists but its marker is gone, a new installed marker is written without reinstalling it.

## Verified bundle and first-administrator task

Desktop staging materializes a complete bundle beneath the project before adding it to the servicing plan:

- `package-profile.json`;
- `staging-manifest.json`;
- `first-logon.ps1`;
- `register-first-logon.ps1`; and
- safe copies of available enabled offline payloads.

Generated files are written atomically. Offline payloads are streamed into the bundle while their SHA-256 is calculated and compared with the profile; required missing payloads, mismatches, links/reparse traversal, or escaping paths stop staging. The resulting per-file hashes become staged-file verification gates, and direct/offline executables are checked again for both SHA-256 and Authenticode publisher on the target before execution.

SetupComplete runs only `register-first-logon.ps1`; it does not run the application installers as SYSTEM. The registration script creates an `AtLogOn` scheduled task for the built-in local Administrators group at highest run level, with `StartWhenAvailable` and `MultipleInstances=IgnoreNew`. The first account expected to run this package plan must therefore be a local administrator. A standard-user logon does not satisfy the task principal.

The task keeps its registration when any package, network wait, payload check, or live verification fails. Its durable markers and logs let a later administrator logon retry the dependency plan; a global mutex prevents overlapping attempts. The task unregisters itself only after every enabled package succeeds and passes live verification. This can resume configuration, but it cannot roll back side effects left by a vendor installer that failed partway through.

## Studio workflow

1. Open Package Studio and load the AI template or import a profile.
2. Enable/disable entries. Dependencies are validated when the profile is planned.
3. Supply and verify official offline payloads for any manual slots.
4. Choose the appropriate network mode.
5. Stage the profile. WimForge creates the verified four-file runner/registration bundle plus selected payload copies, then adds them and the SetupComplete registration command to the project plan.
6. Review the main servicing plan. Package staging is part of the media/image write graph.
7. After a test install, inspect state and logs under `%ProgramData%\WimForge\PackageStudio`.

## CLI examples

```powershell
.\WimForgeCli.exe package catalog --json
.\WimForgeCli.exe package template ai-development --output ai-packages.json
.\WimForgeCli.exe package validate ai-packages.json
.\WimForgeCli.exe package plan ai-packages.json --json
.\WimForgeCli.exe package stage ai-packages.json --directory D:\IsoRoot\WimForge\PackageStudio
.\WimForgeCli.exe package ensure-opencode --dry-run
.\WimForgeCli.exe package ensure-opencode --yes
```

`ensure-opencode` is noninteractive: without `--yes`, missing software returns the confirmation-required exit code and the exact proposed installs.

`package stage` is a lower-level folder export. It writes the profile, manifest, first-logon runner, and verified payload copies to `--directory`; it does not modify a project servicing plan or install the SetupComplete registration hook. Use desktop project staging for that integrated path.

## Provenance and legal review

The template's WinGet IDs were queried from the live community catalog on 2026-07-10; the profile requests current releases instead of freezing those observed versions. Recheck identifiers and vendor requirements before a production build.

Primary sources:

- Microsoft [WinGet documentation](https://learn.microsoft.com/en-us/windows/package-manager/winget/)
- [OpenCode repository](https://github.com/anomalyco/opencode)
- [OpenAI Codex repository](https://github.com/openai/codex) and [Codex app](https://developers.openai.com/codex/app)
- [Claude Code getting started](https://code.claude.com/docs/en/getting-started)

Package metadata is informational and does not grant redistribution rights. Review Microsoft, Docker, Anthropic, OpenAI, and every other vendor's license/terms before placing software on distributable media.

Implementation detail lives in [`docs/package-studio.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/package-studio.md).

## 香港粵語重點

Package Studio 將 WinGet、npm、pip、已簽名 installer、offline payload 同結構化 custom executable 放入可審閱依賴圖。佢唔會代你登入廠商、贈送 licence 或估 hash/signer；冇可信資料嘅 slot 預設關閉。Host OpenCode helper 喺提權 app 開啟時唔會自動搜 PATH/用戶 profile；你要喺 Package Studio 撳 **Verify / install now** 明確批准，驗到 `opencode --version` 正常先算 ready。

---

[← Image Servicing](Image-Servicing) · [Group Policy Studio →](Group-Policy-Studio)
