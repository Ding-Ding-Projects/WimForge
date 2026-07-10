# WinForge Bridge

WinForge Bridge carries an approved declarative recipe—and optionally a complete self-contained WinForge runtime—into Windows installation media. The installed machine replays only the actions present in that recipe and saves resume state after each successful action.

The bridge is a strict contract boundary, not a guessed command-line wrapper.

## Studio workflow

1. Describe the desired post-install result or choose **Add typed action**.
2. Review proposals. Intent-generated entries remain drafts/disabled until approved.
3. Choose machine or user phase and enable only intended actions.
4. Select an optional published WinForge runtime folder and detect its contract.
5. Resolve every unsupported-capability warning.
6. Import/export the portable recipe when needed.
7. Stage the recipe, optional runtime, and payloads into the ISO workspace.
8. Review the main project history and servicing plan; every recipe edit is a project mutation and is undoable.

The page uses non-modal Material surfaces. Invalid input becomes inline/snackbar/notification feedback while other work continues.

## Recipe actions

Recipes use schema `org.wimforge.winforge-recipe`, version 1. Every action has a unique stable ID, idempotency key, explicit `machine` or `user` phase, exactly one typed shape, and a SHA-256 digest over canonical typed fields.

| Kind | Typed data | Execution |
| --- | --- | --- |
| `page` | WinForge page alias | Requires `launch.page.v1`; audited legacy mapping is `--page <alias>` |
| `module` | Stable module ID | Requires runtime capability `apply.module.v1` |
| `tweak` | Stable tweak ID and JSON value | Requires `apply.tweak.v1` |
| `command` | Executable, argument array, working directory, accepted exit codes | Direct process launch; no shell string |
| `registry` | Hive, key, name, type, typed JSON value | .NET registry API |
| `copy` | Relative payload, destination, SHA-256, overwrite policy | Hash verified before every copy |

Shell/script interpreters and `.cmd`, `.bat`, `.ps1`, `.vbs`, and similar command payloads are refused. Registry and file work must use typed actions rather than hiding behavior in an evaluated command line.

Unknown fields/versions, duplicate idempotency keys, tampered digests, path traversal, unsupported registry types, and mismatched payload hashes fail closed.

## Runtime capability audit

The adjacent WinForge source audited on 2026-07-10 was tag `v1.0.177`, commit `27f343be170c43675e4a97f3de152eafb6c99e20`. Its parser documents `--page <alias>` plus launch-oriented flags. It does not document headless module, tweak, or recipe application.

Therefore a runtime without a contract is classified as legacy with one relevant bridge capability:

```text
launch.page.v1 -> ["--page", "{target}"]
```

WimForge does not invoke imaginary switches such as `--apply-recipe` or `--apply-tweak`.

## Runtime contract

A future/published self-contained runtime can provide `winforge-contract.json`:

```json
{
  "format": "org.winforge.runtime-contract",
  "formatVersion": 1,
  "contractVersion": 1,
  "runtimeVersion": "1.1.0",
  "executable": "WinForge.exe",
  "capabilities": ["launch.page.v1"],
  "invocations": {
    "launch.page.v1": ["--page", "{target}"]
  }
}
```

The executable must be an ordinary file within the runtime directory. Invocation templates are argument arrays; placeholders occupy whole tokens. The staged contract records the runtime executable SHA-256 and the bootstrap verifies it before launch.

A legacy ordinary `WinForge.exe` is detected as version `unknown`, contract version 0, with only the observed page capability. A recipe requiring a minimum runtime version cannot use an unknown version.

## ISO staging layout

The verified bundle is placed under the conventional OEM-copy tree:

```text
sources/$OEM$/$1/ProgramData/WimForge/WinForgeBridge/<recipe-id>/
  manifest.json
  recipe.json
  runtime-contract.json
  bootstrap.ps1
  Payload/
  Runtime/

sources/$OEM$/$$/Setup/Scripts/
  WimForgeBridge.<recipe-id>.cmd
```

If no `SetupComplete.cmd` exists, the bridge creates one that calls the recipe fragment. If an ordinary text file already exists without the marker, WimForge atomically prepends a guarded call and preserves its prior bytes; an existing marker is already idempotently reachable. Unsupported oversized or NUL/UTF-16 files stop staging with an explicit error. Success therefore means the hook is reachable, never that a manual merge is still pending.

The manifest records format, recipe hash, runtime contract, installed location, bootstrap command, each file size/SHA-256, and total size. Source/payload/runtime trees reject symbolic links, junctions, and reparse points. Staging occurs in a sibling temporary directory and is promoted only after verification.

An unchanged bundle is idempotent. Replacing a different one requires explicit overwrite and uses a sibling backup during promotion.

## Bootstrap and resume

The generated Windows PowerShell parses data; it does not evaluate code:

- no `Invoke-Expression`, dynamic script block, or shell command string;
- process arguments remain separate tokens;
- manifest, recipe, payload, and runtime executable hashes are verified;
- state is atomically saved after every successful action;
- completed `idempotencyKey -> action digest` entries are skipped;
- changing a digest reruns only that logical action;
- failure records `lastError` and resumes at the first incomplete action later;
- machine actions run through SetupComplete;
- approved user actions use one HKLM RunOnce entry after interactive sign-in.

Page launches are non-blocking. Contracted module/tweak and direct-command actions wait for accepted exit codes such as `0` or `3010`.

## History and complete saves

Recipe JSON, runtime-selection settings, enabled state, and staging choices are stored as project configuration mutations. They receive action-history events and project Git commits, so `Ctrl+Z`, the active-page/global right-click mini history, selective undo, redo-of-undo, bookmarks, and `.wimforge` saves retain them.

The project save carries configuration and Git history. The ISO staging bundle carries the files needed on the installed machine. Those are related but distinct artifacts.

## CLI examples

```powershell
.\WimForgeCli.exe winforge detect C:\Tools\WinForge\publish --json
.\WimForgeCli.exe winforge template page packages --output page.winforge.json
.\WimForgeCli.exe winforge validate page.winforge.json `
  --runtime C:\Tools\WinForge\publish
.\WimForgeCli.exe winforge status page.winforge.json `
  --runtime C:\Tools\WinForge\publish --json

.\WimForgeCli.exe winforge import page.winforge.json `
  --project C:\Images\MyProject
.\WimForgeCli.exe winforge export `
  --project C:\Images\MyProject --output committed.winforge.json

.\WimForgeCli.exe winforge stage page.winforge.json `
  --iso D:\IsoWorkspace `
  --runtime C:\Tools\WinForge\publish `
  --payload C:\ApprovedPayloads `
  --include-runtime --overwrite
```

`validate`/`status`/`stage` can omit the recipe path when `--project` selects a project containing one. `stage` accepts `--include-runtime` or `--without-runtime`; an existing different bundle requires `--overwrite`. Project-backed staging commits the recipe/runtime choice and adds the staged OEM tree to the main servicing plan.

Implementation detail lives in [`docs/winforge-bridge.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/winforge-bridge.md).

---

[← CLI](CLI) · [Safety and Recovery →](Safety-and-Recovery)
