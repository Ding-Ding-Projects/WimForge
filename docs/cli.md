# WimForge CLI

The WimForge command runner exposes project authoring, servicing plans, local
Git history, the notification centre, unattended setup, Package Studio, and the
installed Group Policy catalogue without starting Qt Quick. It uses the same
validated core objects as the desktop app. `CliRunner::run()` accepts arguments
without `argv[0]` and returns stdout, stderr, and a stable exit code; the app can
therefore invoke it before creating a GUI application.

## Output and exit codes

Human-readable output is the default. `--json` returns exactly one compact JSON
object followed by one newline:

```json
{"command":"project","ok":true,"result":{"name":"Example"}}
```

Errors use the same envelope with `ok: false` and an `error` object. Object keys,
array ordering, line endings, and command results are deterministic for the same
on-disk state. Timestamps and Git hashes naturally reflect the state being read.

| Exit | Meaning |
| ---: | --- |
| 0 | Success |
| 2 | Invalid command or arguments |
| 3 | Invalid project/profile/policy data |
| 4 | Requested file, project, or record was not found |
| 5 | Explicit noninteractive confirmation is required |
| 6 | DISM, an installer, or another child process failed |
| 7 | Undo/redo or another state transition conflicts with current state |
| 8 | File or local-repository I/O failed |
| 10 | An invalid internally generated plan was detected |

The runner never waits for terminal input. A destructive servicing plan or a
missing OpenCode installation returns exit 5 until the caller reviews a dry run
and repeats the command with `--yes`.

## Projects and complete configuration coverage

```powershell
WimForge project create C:\Images\MyProject --name "My Windows"
WimForge --project C:\Images\MyProject project open --json
WimForge --project C:\Images\MyProject project validate
WimForge --project C:\Images\MyProject project validate --execution
WimForge --project C:\Images\MyProject project export project-copy.json
WimForge project import project-copy.json C:\Images\ImportedProject
```

`project validate` accepts a draft with payloads that will be added later.
`--execution` requires the source image and every selected payload to exist.
Creating, importing, and editing a project goes through `ProjectConfig::save()`,
so every successful user action creates a commit in that project's own `.git`.

Every current and future `ProjectConfig` field is reachable through generic
JSON editing:

```powershell
WimForge --project C:\Images\MyProject config set /image/index 6
WimForge --project C:\Images\MyProject config set paths.source '"D:\\Media"'
WimForge --project C:\Images\MyProject config add /drivers '"D:\\Drivers"'
WimForge --project C:\Images\MyProject config remove /drivers '"D:\\Old"'
WimForge --project C:\Images\MyProject config add /settings '{"telemetry":false}'
WimForge --project C:\Images\MyProject config erase /settings/obsolete
```

Paths beginning with `/` are RFC 6901 JSON Pointers (`~0` means `~`, and `~1`
means `/`). Dot paths such as `features.enable` are a convenience when keys do
not contain dots. Values that parse as JSON retain their type; otherwise they
are strings. Quote a value explicitly as JSON when a string resembles a number,
boolean, or JSON document.

- `set PATH VALUE` replaces a scalar, object, array, or array index; a final `-`
  pointer segment appends to an array.
- `add PATH VALUE` appends to an array. When PATH selects an object, VALUE must
  be an object and its keys are merged.
- `remove PATH VALUE` removes every equal array item, or removes the object key
  named by a string VALUE.
- `erase PATH` removes the selected property or array index.
- `config edit` applies multiple `--set`, `--add`, `--remove`, and `--erase`
  operations atomically and creates one Git commit.

The serialized field map is:

| JSON path | Type and purpose |
| --- | --- |
| `/name`, `/description` | Project text |
| `/paths/source` | Source ISO, image, media folder, or online target context |
| `/paths/image`, `/paths/mount`, `/paths/output` | Image servicing paths |
| `/paths/unattendedXml` | Generated or imported Windows answer file |
| `/image/index` | One-based image index |
| `/image/outputFormat` | `wim`, `esd`, `swm`, or `iso` |
| `/image/isoLabel`, `/image/cloneSource` | ISO label and source cloning |
| `/drivers`, `/updates`, `/packages` | Driver folders/files and servicing payloads |
| `/features/enable`, `/features/disable` | Optional Windows feature names |
| `/capabilities/add`, `/capabilities/remove` | Capability/FOD identities |
| `/appx/remove`, `/appx/provision` | Provisioned package identities/payloads |
| `/components/remove` | Component package identities |
| `/unattendedFiles`, `/postSetupItems` | Extra answer files and post-setup actions |
| `/registry` | Objects with `hive`, `key`, `name`, `type`, `value`, and `delete` |
| `/settings` | Extensible Windows setting map |
| `/automation/autoImport` | Automatically ingest supported source metadata |
| `/automation/autoExport`, `/automation/autoExportPath` | Automatic project JSON copy |
| `/options/verifyPayloads`, `/options/mountReadOnly` | Payload and mount safety |
| `/options/cleanupComponentStore`, `/options/resetBase` | Component cleanup policy |
| `/options/optimizeImage`, `/options/rebuildImage` | Output optimization |
| `/options/createIso`, `/options/keepMountOnFailure`, `/options/dryRun` | Execution/output behavior |
| `/options/compression`, `/options/scratch` | DISM compression and scratch folder |
| `/options/maximumParallelOperations` | `0` for automatic, otherwise 1–64 |
| Any other `/options/*` key | Extensible servicing option such as `targetOnline`, `mediaWorkspace`, `discardChanges`, or `splitSizeMb` |

## Plans, apply, and injected execution

```powershell
WimForge --project C:\Images\MyProject plan
WimForge --project C:\Images\MyProject dry-run --script review.ps1 --json
WimForge --project C:\Images\MyProject apply --yes
```

`plan` and `dry-run` return every executable and argument as separate JSON
tokens, dependencies, administrator/destructive flags, bilingual descriptions,
and the preview command. They never run a process. `--script` additionally
exports a reviewable PowerShell plan.

`apply` performs strict execution validation and runs the validated operations
without a shell. The default adapter is deliberately synchronous. The desktop
host can supply `CliDependencies::processInvoker` to route operations through
its concurrent `JobEngine`; tests inject a no-op runner, so no DISM command or
installer is ever launched by the test suite.

## Git history and notification history

```powershell
WimForge --project C:\Images\MyProject history log --limit 50
WimForge --project C:\Images\MyProject history undo
WimForge --project C:\Images\MyProject history redo

WimForge --store C:\State\Notifications notifications new `
  --title "ISO complete" --message "Boot-test me before celebrating." `
  --severity success --data '{"project":"MyProject"}'
WimForge --store C:\State\Notifications notifications read NOTIFICATION_ID
WimForge --store C:\State\Notifications notifications dismiss NOTIFICATION_ID
WimForge --store C:\State\Notifications notifications restore NOTIFICATION_ID
WimForge --store C:\State\Notifications notifications delete NOTIFICATION_ID
WimForge --store C:\State\Notifications notifications list --all --json
```

Project undo calls `GitHistory::revertLatest()`. Undoing an undo creates another
inverse commit, so the previous change becomes effective again. `history redo`
is the explicit spelling and is accepted only when the current head is a revert.

The notification store is a separate local Git repository. New, read, unread,
dismiss, restore, and delete actions each create a commit and an immutable event.
Delete is a tombstone; `list --all` can still recover it. Notification `history`,
`events`, `undo`, and `redo` expose both audit layers.

## Unattended setup

```powershell
WimForge unattend template full --output profile.json
WimForge unattend template ai-development --output autounattend.xml
WimForge unattend import vendor.xml --output editable.json
WimForge unattend export editable.json --output autounattend.xml
WimForge unattend computer-name editable.json --mode prompt --output autounattend.xml
WimForge unattend computer-name editable.json --mode fixed --value BUILD-PC
WimForge unattend computer-name editable.json --mode serial --prefix AI
WimForge unattend gvlk list --edition Enterprise --json
WimForge unattend gvlk set editable.json `
  --edition "Windows 11/10 Enterprise" --output volume.json
```

Import detects XML by extension and otherwise expects the lossless WimForge JSON
profile. Export detects the requested extension unless `--format` is explicit.
Prompt mode uses WimForge's first-logon name prompt rather than writing the
invalid literal `[Prompt]` into Microsoft's `ComputerName` setting. Serial mode
generates the name at first logon. GVLK selection is limited to entries published
by Microsoft; every result repeats that a GVLK grants no licence and performs no
activation by itself.

## Package Studio and automatic OpenCode setup

```powershell
WimForge package catalog
WimForge package template ai-development --output ai-packages.json
WimForge package validate ai-packages.json
WimForge package plan ai-packages.json --json
WimForge package stage ai-packages.json --directory D:\IsoRoot\WimForge\PackageStudio
WimForge package ensure-opencode --dry-run
WimForge package ensure-opencode --yes
```

`template ai-development` includes the maintained full development profile:
Git, Node/npm, Python, CMake, Java, build tools, VS Code, Docker, Codex CLI,
Claude Code, OpenCode, and the explicitly supported/optional desktop payloads.
`plan` validates dependencies and returns the exact tokenized install and verify
commands. `stage` writes `package-profile.json`, `staging-manifest.json`, and the
resumable `first-logon.ps1`; enabled relative offline payloads are copied beside
them and SHA-256 checked before staging.

`ensure-opencode` first runs the profile's live verification commands. If Node.js
or OpenCode is absent, a dry run reports the exact missing installs. `--yes`
installs the Node.js LTS dependency through WinGet when necessary, installs
`opencode-ai@latest` through npm, and verifies both again. An installer exit code
of zero is not considered success if verification still fails.

## Installed ADMX/ADML policy catalogue

```powershell
WimForge gpo catalog --summary
WimForge gpo catalog --locale en-US --locale zh-HK --json
WimForge gpo search "Windows Update restart"
WimForge gpo search 'restart.*deadline' --regex --json
WimForge gpo export policies.md --primary en-US --secondary zh-HK
WimForge gpo search demo --path D:\PolicyDefinitions --locale en-US
```

Without `--path`, commands load `%WINDIR%\PolicyDefinitions`. Plain search is
case-insensitive AND-token search. `--regex` uses the bounded, validated regular
expression path in `GpoCatalog`; invalid or resource-limit override expressions
return exit 3. Structured results include localization, explanation, supported-on
text, registry assignments, element constraints, enum values, and the Material
control selected for each ADMX presentation type. `gpo export` writes complete
Markdown documentation.

## WinForge Bridge

```powershell
WimForgeCli winforge detect D:\Apps\WinForge
WimForgeCli winforge template page ai --output winforge-recipe.json
WimForgeCli winforge validate winforge-recipe.json --runtime D:\Apps\WinForge
WimForgeCli --project C:\Images\MyProject winforge import winforge-recipe.json
WimForgeCli --project C:\Images\MyProject winforge export --output portable-recipe.json
WimForgeCli --project C:\Images\MyProject winforge stage `
  --iso C:\Images\MyProject\.wimforge\generated\winforge-stage `
  --runtime D:\Apps\WinForge --overwrite
```

Recipes preserve typed actions, argument arrays, phases, idempotency keys and
per-action digests. `stage` writes the runtime, recipe, contract, checksum
manifest, resumable bootstrap and Windows OEM setup hook. With `--project`, it
also commits the media staging entry so the reviewed ISO plan copies the exact
verified OEM tree. The observed legacy WinForge interface supports only
`--page <alias>`; module and tweak replay fail closed unless a runtime supplies
a compatible `winforge-contract.json`.

## Response and invocation files

Long commands may be stored as quoted text:

```text
# build.rsp
--json
--project "C:\Images\My Project"
config edit
--add /features/enable "HypervisorPlatform"
--set /options/maximumParallelOperations 4
```

Invoke it with `WimForge @build.rsp` or `WimForge --response-file build.rsp`.
Nested response files are resolved relative to their containing file, limited to
eight levels, checked for cycles, limited to 4 MiB each, and capped at 10,000
expanded arguments.

A JSON string array is also accepted. A JSON invocation object can add global
defaults:

```json
{
  "project": "C:\\Images\\My Project",
  "output": "json",
  "arguments": ["config", "set", "/image/index", "4"]
}
```

`--config invocation.json` and `@invocation.json` are equivalent.

## Complete state namespaces

`bundle`, `action-history`, and `winforge` are linked into the same validated
core used by the desktop application. Unknown actions fail with exit 2; none of
these namespaces silently ignores future or misspelled operations.
