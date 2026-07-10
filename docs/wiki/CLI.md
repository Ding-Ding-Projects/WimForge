# Command-Line Interface

Release packages include `WimForgeCli.exe`, a console-subsystem executable using the same validated C++ core as the desktop application. Prefer it over launching the GUI-subsystem `WimForge.exe` from scripts because console attachment and exit behavior are predictable.

## Global form

```text
WimForgeCli [--json] [--project FOLDER] [--store FOLDER] <command> ...
WimForgeCli @arguments.rsp
WimForgeCli --config invocation.json
```

The runner never asks an interactive terminal question. Destructive servicing and software installation require `--yes`; otherwise they return the confirmation-required exit code after showing the proposed work.

`--json` writes exactly one compact JSON object and one newline:

```json
{"command":"project","ok":true,"result":{"name":"Example"}}
```

Errors use the same envelope with `ok: false` and an `error` object. Output is deterministic for the same on-disk state except naturally changing timestamps and Git hashes.

## Project and configuration

```powershell
.\WimForgeCli.exe project create C:\Images\MyProject --name "My Windows"
.\WimForgeCli.exe --project C:\Images\MyProject project open --json
.\WimForgeCli.exe --project C:\Images\MyProject project validate
.\WimForgeCli.exe --project C:\Images\MyProject project validate --execution
.\WimForgeCli.exe --project C:\Images\MyProject project export project-copy.json
.\WimForgeCli.exe project import project-copy.json C:\Images\ImportedProject
```

Draft validation allows payloads that will be supplied later; `--execution` requires selected source and payload paths to exist.

Every serialized `ProjectConfig` field can be edited through RFC 6901 JSON pointers or convenient dot paths:

```powershell
.\WimForgeCli.exe --project C:\Images\MyProject config set /image/index 6
.\WimForgeCli.exe --project C:\Images\MyProject config add /drivers '"D:\\Drivers"'
.\WimForgeCli.exe --project C:\Images\MyProject config remove /drivers '"D:\\Old"'
.\WimForgeCli.exe --project C:\Images\MyProject config set /options/maximumParallelOperations 4
.\WimForgeCli.exe --project C:\Images\MyProject config erase /settings/obsolete
```

Values that parse as JSON retain their type; other values are strings. `config edit` accepts repeated `--set`, `--add`, `--remove`, and `--erase` operations and commits them atomically as one user action.

## Plan, dry run, and apply

```powershell
.\WimForgeCli.exe --project C:\Images\MyProject plan --json
.\WimForgeCli.exe --project C:\Images\MyProject dry-run --script review.ps1
.\WimForgeCli.exe --project C:\Images\MyProject apply --yes
```

`plan` and `dry-run` never execute a process. Results include executable, argument tokens, dependencies, administration/destructive flags, bilingual descriptions, and preview commands. `apply` performs execution validation and invokes operations without a general shell.

The CLI adapter is deliberately synchronous. The desktop host routes compatible operations through its concurrent dependency-aware job engine.

## History and complete saves

```powershell
.\WimForgeCli.exe --project C:\Images\MyProject history log --limit 50
.\WimForgeCli.exe --project C:\Images\MyProject history undo
.\WimForgeCli.exe --project C:\Images\MyProject history redo

.\WimForgeCli.exe --project C:\Images\MyProject action-history list --context packages --json
.\WimForgeCli.exe --project C:\Images\MyProject action-history undo EVENT_ID
.\WimForgeCli.exe --project C:\Images\MyProject action-history redo EVENT_ID
.\WimForgeCli.exe --project C:\Images\MyProject action-history bookmark baseline --event EVENT_ID

.\WimForgeCli.exe --project C:\Images\MyProject bundle export save.wimforge
.\WimForgeCli.exe bundle import save.wimforge C:\Images\Restored
```

Bundle export accepts `--notifications FOLDER` or the global `--store`; otherwise it uses WimForge's default notification store. Import accepts `--overwrite` only as an explicit request.

## Notification center

```powershell
.\WimForgeCli.exe --store C:\State\Notifications notifications new `
  --title "ISO complete" --message "Test it in a VM." --severity success
.\WimForgeCli.exe --store C:\State\Notifications notifications read ID
.\WimForgeCli.exe --store C:\State\Notifications notifications unread ID
.\WimForgeCli.exe --store C:\State\Notifications notifications dismiss ID
.\WimForgeCli.exe --store C:\State\Notifications notifications restore ID
.\WimForgeCli.exe --store C:\State\Notifications notifications delete ID
.\WimForgeCli.exe --store C:\State\Notifications notifications list --all --json
.\WimForgeCli.exe --store C:\State\Notifications notifications events --limit 100
.\WimForgeCli.exe --store C:\State\Notifications notifications undo
```

## Studios

```powershell
# Unattended
.\WimForgeCli.exe unattend template ai-development --output autounattend.xml
.\WimForgeCli.exe unattend validate editable.json
.\WimForgeCli.exe unattend gvlk list --edition Enterprise --json

# Packages
.\WimForgeCli.exe package template ai-development --output ai-packages.json
.\WimForgeCli.exe package validate ai-packages.json
.\WimForgeCli.exe package plan ai-packages.json --json
.\WimForgeCli.exe package stage ai-packages.json --directory D:\IsoRoot\WimForge\PackageStudio
.\WimForgeCli.exe package ensure-opencode --yes

# Installed GPO definitions
.\WimForgeCli.exe gpo catalog --summary
.\WimForgeCli.exe gpo search "restart.*deadline" --regex --json
.\WimForgeCli.exe gpo export policies.md --primary en-US --secondary zh-HK
```

See the individual studio wiki pages for validation and safety details.

## WinForge Bridge

```powershell
# Audit a self-contained runtime and its declared/legacy capabilities
.\WimForgeCli.exe winforge detect C:\Tools\WinForge\publish --json

# Create a safe page-deep-link recipe
.\WimForgeCli.exe winforge template page packages --output page.winforge.json

# Validate a recipe alone or against a selected runtime
.\WimForgeCli.exe winforge validate page.winforge.json
.\WimForgeCli.exe winforge status page.winforge.json `
  --runtime C:\Tools\WinForge\publish --json

# Commit a recipe into a project, or export the project's current recipe
.\WimForgeCli.exe winforge import page.winforge.json `
  --project C:\Images\MyProject
.\WimForgeCli.exe winforge export `
  --project C:\Images\MyProject --output committed.winforge.json

# Stage a verified OEM bundle. Recipe may instead come from --project.
.\WimForgeCli.exe winforge stage page.winforge.json `
  --iso D:\IsoWorkspace `
  --runtime C:\Tools\WinForge\publish `
  --payload C:\ApprovedPayloads `
  --include-runtime
```

`winforge stage` also accepts `--without-runtime` and explicit `--overwrite`. When `--project` supplies the recipe, the command commits runtime/include settings and the staged-media entry into that project. Without a recipe path, `validate`, `status`, and `stage` require `--project`.

These commands validate the WimForge Bridge recipe/contract and stage its OEM bundle. They do not invoke guessed legacy switches such as `--apply-recipe` or `--apply-tweak`. The audited legacy WinForge runtime itself supports only `--page <alias>` as a relevant public capability. See [WinForge Bridge](WinForge-Bridge).

## Response and invocation files

A quoted response file can hold a long command:

```text
# build.rsp
--json
--project "C:\Images\My Project"
config edit
--add /features/enable "HypervisorPlatform"
--set /options/maximumParallelOperations 4
```

Run it with `WimForgeCli @build.rsp` or `WimForgeCli --response-file build.rsp`. A JSON string array is also accepted. A JSON invocation object can set globals:

```json
{
  "project": "C:\\Images\\My Project",
  "output": "json",
  "arguments": ["config", "set", "/image/index", "4"]
}
```

Nested response files resolve relative to their containing file, are cycle-checked, and are limited to eight levels, 4 MiB per file, and 10,000 expanded arguments.

## Exit codes

| Exit | Meaning |
| ---: | --- |
| 0 | Success |
| 2 | Invalid command or arguments |
| 3 | Invalid project/profile/policy data |
| 4 | Requested file/project/record not found |
| 5 | Explicit noninteractive confirmation required |
| 6 | DISM, installer, or child process failed |
| 7 | Undo/redo or another state transition conflicts |
| 8 | File or repository I/O failed |
| 10 | Invalid internally generated plan/internal error |

The full command/field reference lives in [`docs/cli.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/cli.md). Run `.\WimForgeCli.exe help` for the exact command set compiled into the current binary.

---

[← Project Bundles](Project-Bundles) · [WinForge Bridge →](WinForge-Bridge)
