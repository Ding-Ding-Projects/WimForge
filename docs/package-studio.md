# Package Studio

Package Studio describes software that WimForge can add to a Windows image and produces a safe, repeatable first-logon installer. A profile is portable JSON; ISO staging is a separate manifest, so a profile never needs to contain credentials or machine-specific state.

The built-in **Full AI Development** profile is available at [`templates/ai-development.json`](../templates/ai-development.json). It combines native build tools, common runtimes, desktop developer tools, and AI coding clients.

## Supported package sources

| Provider | Package Studio behavior |
|---|---|
| `winget` | Runs `winget install` with an exact ID, noninteractive agreement flags, architecture/scope, optional version, and optional vendor silent override. |
| `npm` | Installs a global npm package after its Node.js dependency. |
| `pip` | Runs pip through the Python launcher and supports pinned versions and user scope. |
| `direct-signed-installer` | Uses a staged copy when present or downloads over HTTPS, then verifies both SHA256 and Authenticode signer before execution. |
| `offline-payload` | Requires a safe path relative to the first-logon script and verifies SHA256 plus Authenticode signer before execution. It never requires a network. |
| `custom-command` | Runs an explicitly structured executable and argument list. Shell and script-host wrappers are rejected. |

Every entry can record a stable ID, display name and description, provider identifier, version, `x64`/`x86`/`arm64`/`any` architecture, user/machine/either scope, dependencies, silent arguments, expected SHA256 and publisher, license, homepage, install command, verification command, release page, and notes. Disabled optional entries stay in the profile without becoming executable plans.

Commands are not shell strings. They are an executable plus an array of arguments, and the generator invokes them with PowerShell argument splatting. Validation rejects control characters, shell operators, inline interpreter flags, script hosts such as `cmd.exe` and `powershell.exe`, URL credentials, and values that look like embedded tokens or passwords.

## Network modes

- `online` waits for HTTPS connectivity before the first network package.
- `prefer-offline` consumes a staged direct-installer payload first and downloads only if that payload is absent.
- `offline` rejects the profile if any enabled entry still requires the network.

The wait duration and retry count are profile settings. Network waiting is bounded; a failed package gets a durable failure marker and stops the dependency-ordered run rather than allowing dependent software to install into a broken environment.

## Dependency ordering

Dependencies use profile-local package IDs, not provider IDs. Package Studio performs a stable topological sort, reports unknown or disabled dependencies, rejects self-dependencies and duplicates, and prints a readable path for a cycle. Only enabled entries appear in the generated first-logon run.

For example, the Full AI Development profile installs Node.js before OpenCode and Codex, CMake before the C++ Build Tools workload, and Git before Claude Code.

## Crash-safe, idempotent first logon

`PackageStudio::generateFirstLogonPowerShell()` emits PowerShell 5.1-compatible script content with:

- a live verification command before every install, making every package install-if-missing even if state was deleted;
- one JSON state file per package in `%ProgramData%\WimForge\PackageStudio\state`;
- `installing`, `installed`, and `failed` transitions written through a same-directory temporary file;
- a package fingerprint, attempt number, UTC timestamp, and status message in every marker;
- corrupt marker quarantine rather than destructive recovery;
- bounded retries with backoff, bounded network wait, per-package logs, and a run log;
- process `PATH` refresh after installs so npm installed by Node.js is immediately available;
- dependency order and stop-on-failure behavior, allowing a later run to resume safely;
- SHA256 and Authenticode publisher verification for direct and offline executables; and
- no API keys, passwords, access tokens, login automation, or other secrets.

Verification is authoritative. If an `installed` marker exists but the application is missing, Package Studio reinstalls it. If the app exists but its marker was lost, it writes a fresh installed marker without reinstalling. An interrupted `installing` marker is simply retried on the next launch.

OpenCode therefore installs automatically only when `opencode --version` fails. Its exact install command is `npm install -g opencode-ai@latest`. Codex likewise uses `npm install -g @openai/codex` and verifies `codex --version`; no user credential is placed into the image.

## Host OpenCode verification

The desktop's host helper is separate from the target-image package plan. Shortly after WimForge starts, it locates OpenCode in `PATH` and common npm installation locations and runs `opencode --version` asynchronously. An existing executable is not considered ready until that process exits normally with code zero and nonempty output.

If no executable is found, WimForge uses an existing npm or installs `OpenJS.NodeJS.LTS` through WinGet, then runs `npm install -g opencode-ai@latest`. It locates the resulting executable and performs the same live verification before reporting installation success or allowing an OpenCode-assisted studio action to continue. A missing executable, failed start, nonzero exit, or empty version output becomes non-modal in-app error feedback; the rest of WimForge remains usable.

## Verified ISO staging bundle

`PackageStudio::generateIsoStagingManifest()` returns `wimforge.package-staging` JSON. It names:

- `WimForge/PackageStudio/first-logon.ps1`;
- `WimForge/PackageStudio/register-first-logon.ps1`;
- `WimForge/PackageStudio/package-profile.json` and `staging-manifest.json`;
- the durable state and log roots;
- enabled online package IDs; and
- every selected or optional payload with source path, ISO-relative path, required flag, expected SHA256, and expected publisher.

The desktop **Stage profile** action calls `PackageStudio::materializeFirstLogonBundle()`. It atomically writes those four generated files, streams each available enabled offline payload into the bundle while calculating and checking SHA-256, and refuses required missing payloads, mismatches, links/reparse traversal, and paths outside the project root. Each resulting file and hash is added to the project's validated staged-file model for the target path `%ProgramData%\WimForge\PackageStudio`. Target execution checks the payload's SHA-256 and Authenticode publisher again immediately before launch.

Offline paths are relative to `$PSScriptRoot`, cannot be absolute, and cannot escape with `..`. A `prefer-offline` direct installer may omit its staged copy only when its validated HTTPS download remains available; a required offline payload fails staging when absent.

## SetupComplete registration and retry

Desktop project staging adds one transparent SetupComplete command. That command does not install the selected applications as SYSTEM. It runs `register-first-logon.ps1`, which registers the **WimForge Package Studio** scheduled task with:

- an `AtLogOn` trigger;
- the built-in local Administrators group (`S-1-5-32-544`) as its principal;
- highest run level, `StartWhenAvailable`, and overlapping runs set to `IgnoreNew`; and
- the structured `first-logon.ps1` runner under `%ProgramData%`.

The first account expected to execute the package plan must be a member of the local Administrators group; a standard-user logon does not satisfy that task principal. A global mutex also prevents two package runs from overlapping.

The runner unregisters the scheduled task only after every enabled package has installed and passed its live verification. If a package, network wait, payload check, or install verification fails, the runner exits before unregistering it. Its state/log files and the task remain, so a later administrator logon runs the plan again and resumes from live verification plus the durable package markers. This is retry/resume behavior, not rollback of an installer that already changed the machine.

The CLI `package stage` command is a lower-level folder export: it writes the profile, manifest, runner, and verified payload copies to the requested directory. It does not edit a project servicing plan or install a SetupComplete hook; desktop project staging performs that integration.

Before enabling a manual/offline package, select a current official payload, calculate its SHA256, inspect its Authenticode signer, record both expected values, and provide a silent structured install command. A direct or offline package cannot validate without these trust anchors.

## Import and export

Portable profiles use this root schema:

```json
{
  "schema": "wimforge.package-studio",
  "version": 1,
  "name": "My workstation",
  "networkMode": "online",
  "retryCount": 3,
  "networkWaitSeconds": 600,
  "packages": []
}
```

`PackageStudio::exportJson()` uses an atomic `QSaveFile`; `importJson()` parses, validates, checks dependency cycles, and rejects unsupported schema versions before returning a profile. The `toJson()` and `fromJson()` methods support integration with the project-local Git history, automatic import/export, and UI models without another serialization format.

## Full AI Development template provenance

The exact WinGet IDs below were queried from the live `winget` community source on **2026-07-10**. The observed versions are evidence for that dated check; the template intentionally requests `latest` so a newly built ISO receives a current release.

| Software | Exact ID | Observed version |
|---|---|---:|
| Git for Windows | `Git.Git` | 2.55.0.2 |
| Node.js LTS | `OpenJS.NodeJS.LTS` | 24.18.0 |
| Python 3.13 | `Python.Python.3.13` | 3.13.14 |
| CMake | `Kitware.CMake` | 4.3.4 |
| Eclipse Temurin JDK 21 | `EclipseAdoptium.Temurin.21.JDK` | 21.0.11.10 |
| Visual Studio 2022 Build Tools | `Microsoft.VisualStudio.2022.BuildTools` | 17.14.35 |
| Visual Studio Code | `Microsoft.VisualStudioCode` | 1.126.0 |
| Docker Desktop | `Docker.DockerDesktop` | 4.81.0 |
| Claude Code | `Anthropic.ClaudeCode` | 2.1.203 |
| Claude Desktop | `Anthropic.Claude` | 1.19367.0 |

The WinGet client documents exact-ID installs such as `winget install --id Git.Git -e`, and the Microsoft community manifest repository is the backing catalog: [WinGet install reference](https://github.com/microsoft/winget-cli/blob/master/doc/windows/package-manager/winget/install.md) and [WinGet package manifests](https://github.com/microsoft/winget-pkgs).

The non-WinGet commands come from their vendors:

- [OpenCode installation](https://opencode.ai/docs/) documents `npm install -g opencode-ai` and its [download page](https://opencode.ai/download) offers a Windows desktop build. The desktop entry remains optional and disabled until an exact release payload, hash, and signer are chosen.
- [OpenAI's Codex repository](https://github.com/openai/codex) and [official Codex announcement](https://openai.com/index/codex-now-generally-available/) document `npm i -g @openai/codex`.
- [Anthropic's Claude Code setup](https://docs.anthropic.com/en/docs/claude-code/getting-started) documents Windows support and verification/update guidance. The template uses the currently verified native WinGet ID requested by WimForge rather than an npm fallback.
- [Anthropic's desktop installation guide](https://support.anthropic.com/en/articles/10065433-installing-claude-for-desktop) documents the official Windows desktop app.

The Codex app and ChatGPT Desktop are deliberately represented as disabled official-payload entries. Package Studio does **not** invent WinGet identifiers for them. The ISO author must obtain current official packages from the [Codex app page](https://developers.openai.com/codex/app), [Microsoft Store listing](https://apps.microsoft.com/detail/9nt1r1c2hh7j), or [OpenAI desktop page](https://openai.com/chatgpt/desktop/), then supply the current hash, signer, and reviewed silent command before enabling them.

Licenses in the catalog are informational metadata for review; they do not grant redistribution rights. ISO authors remain responsible for each vendor's license, subscription, and redistribution terms, including the Microsoft, Docker, Anthropic, and OpenAI applications.
