# WinForge Bridge

WimForge can carry an approved, declarative set of WinForge-family actions into Windows installation media. The bridge exports the recipe, an optional complete self-contained WinForge runtime, verified offline payloads, a resumable bootstrap, and an integration manifest. The resulting machine replays only the actions present in that recipe.

The bridge is deliberately a contract boundary, not a guessed command-line wrapper.

## Current WinForge capability audit

The adjacent WinForge source was inspected on 2026-07-10 at tag `v1.0.177` / commit `27f343be170c43675e4a97f3de152eafb6c99e20`. Its application argument parser explicitly supports `--page <alias>` (plus launch-oriented flags such as `--path`, `--minimized`, and reactor flags). It does **not** currently expose a documented headless command for applying a tweak, module, or bridge recipe.

Accordingly, an uncontracted runtime is reported as a legacy runtime with exactly one bridge capability:

```text
launch.page.v1 -> ["--page", "{target}"]
```

WimForge does not claim or invoke `--apply-recipe`, `--apply-tweak`, or any similar imaginary command. A module or tweak action fails compatibility validation until that particular runtime ships a `winforge-contract.json` declaring a supported invocation.

## Recipe format

Recipes use the identifier `org.wimforge.winforge-recipe`, currently at format version 1. Each action has:

- a unique stable `id` and `idempotencyKey`;
- an explicit `machine` or `user` phase;
- exactly one typed action shape;
- a SHA-256 digest over its canonical typed fields.

Supported action kinds are:

| Kind | Typed data | Execution |
|---|---|---|
| `page` | WinForge page alias | Uses declared `launch.page.v1`; the audited legacy mapping is `--page <alias>` |
| `module` | Stable module ID | Requires declared `apply.module.v1` |
| `tweak` | Stable tweak ID and JSON value | Requires declared `apply.tweak.v1` |
| `command` | Executable, argument array, working directory, accepted exit codes | Starts the executable directly with `UseShellExecute=false`; no shell command line |
| `registry` | Hive, key, value name, registry type, typed JSON value | Uses .NET registry APIs |
| `copy` | Relative payload path, destination, SHA-256, overwrite policy | Verifies the source hash before every copy |

Shell and script interpreters are refused for `command` actions. `.cmd`, `.bat`, `.ps1`, `.vbs`, and similar script paths are also refused. If a workflow needs registry or file changes, it must use those typed actions instead of hiding them in a shell string. Arguments may contain punctuation because they remain separate tokens and are quoted using the Windows native argument algorithm.

Unknown JSON fields, unknown recipe versions, duplicate idempotency keys, tampered action digests, path traversal, invalid registry types, and mismatched payload hashes fail closed.

## Runtime contract

A self-contained WinForge publish directory can include this authoritative file:

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

The executable must be an ordinary file inside the runtime directory. Invocation entries are argument arrays, not command strings. Placeholders must occupy a whole token. The bridge currently understands:

- `launch.page.v1`: `{target}`
- `apply.module.v1`: `{target}`, `{action-id}`
- `apply.tweak.v1`: `{target}`, `{value-json}`, `{action-id}`

The staged contract also records the runtime executable SHA-256. The bootstrap verifies it before launching WinForge.

When no contract file exists but an ordinary `WinForge.exe` is present, detection returns version `unknown`, contract version 0, source `legacy-observed-cli-2026-07-10`, and only the audited page capability. A recipe that requires a minimum runtime version cannot use an unknown version.

## ISO staging layout

`WinForgeBridge::stageForIso()` writes a verified bundle into the conventional OEM-copy tree:

```text
sources/$OEM$/$1/ProgramData/WimForge/WinForgeBridge/<recipe-id>/
  manifest.json
  recipe.json
  runtime-contract.json
  bootstrap.ps1
  Payload/                 # only referenced, checksum-matched copy sources
  Runtime/                 # optional complete self-contained runtime
```

It also writes a recipe-specific SetupComplete fragment under:

```text
sources/$OEM$/$$/Setup/Scripts/WimForgeBridge.<recipe-id>.cmd
```

If no `SetupComplete.cmd` exists, the bridge creates one that calls the fragment. If an ordinary text `SetupComplete.cmd` already exists without the marker, WimForge atomically prepends the guarded call while preserving every existing byte. If the marker already exists, staging leaves the hook idempotently reachable. Oversized or UTF-16/NUL-containing files fail staging with an explicit error instead of reporting a hook that Windows would never invoke; there is no successful "manual merge still required" state.

`manifest.json` identifies the format, recipe hash, runtime contract, installed location, exact bootstrap command, every staged file's size and SHA-256, and the total size. Runtime directories, payload roots, files, symbolic links, and NTFS junctions are checked; links/reparse points are never followed. Staging occurs in a sibling temporary directory and is promoted only after verification. An unchanged bundle is idempotent. Replacing a different bundle requires an explicit overwrite option and uses a sibling backup during promotion.

## Bootstrap and resume behavior

The generated Windows PowerShell is parsed data, not evaluated code:

- no `Invoke-Expression`, `iex`, dynamic script block, or shell command string;
- command arguments are supplied as distinct tokens to `ProcessStartInfo`;
- every manifest-listed bundle file is verified, followed by the embedded recipe and runtime-executable trust checks;
- state is saved atomically after every successful action;
- completed `idempotencyKey -> action digest` entries are skipped on resume;
- changing an action digest causes only that action to run again;
- a failure records `lastError`, exits, and resumes from the first incomplete action next time;
- machine actions run from SetupComplete;
- approved user actions are registered through a single HKLM RunOnce entry and run after an interactive sign-in.

Page launches are non-blocking. Contracted module/tweak operations and direct commands wait for their exit code. Direct commands define their accepted exit-code set (for example `0` and `3010`).

## UI integration surface

`WinForgeBridgePage.qml` is non-modal and expects the root controller to expose these deliberately named hooks:

### Properties

- `winForgeBridgeActions`
- `winForgeBridgeIncludeRuntime`
- `winForgeBridgeRuntimePath`
- `winForgeBridgeRuntimeStatus`
- `winForgeBridgeStatus`

### Methods

- `proposeWinForgeBridgeActions(intent)`
- `addWinForgeBridgeAction(kind, target, executable, argumentsJson, phase)`
- `removeWinForgeBridgeAction(id)`
- `setWinForgeBridgeActionEnabled(id, enabled)`
- `setWinForgeBridgeIncludeRuntime(enabled)`
- `setWinForgeBridgeRuntimePath(path)`
- `detectWinForgeBridgeRuntime()`
- `importWinForgeBridgeRecipe(path)`
- `exportWinForgeBridgeRecipe(path)`
- `stageWinForgeBridgeIntoIso(isoStagingPath)`

The controller should route every mutating hook through WimForge's action-history transaction so Ctrl+Z, the contextual mini history manager, undo-of-undo, project Git history, and portable `.wimforge` save bundles all retain the bridge edits. Intent-generated actions must remain drafts until the user approves them.

## Test coverage

`tests/winforge_bridge_tests.cpp` compiles against Qt 6.8 Core with `/W4` and covers:

- canonical JSON round trips and digest tampering;
- unknown-field rejection and stable argument tokens;
- shell/interpreter and bundled-command-line refusal;
- copy path traversal and checksum rejection;
- legacy capability detection without invented CLI commands;
- symlink/NTFS junction refusal while preserving the outside target;
- verified payload, manifest, bootstrap, SetupComplete, and idempotent restaging;
- generated PowerShell non-eval invariants;
- PowerShell parser AST validation when Windows PowerShell is available.
