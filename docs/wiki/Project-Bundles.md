# Project Bundles

A `.wimforge` file is the complete portable save format. It carries working trees and hidden `.git` directories—not just `project.json`—so project actions, notification actions, undo commits, refs, reflogs, tags, and objects survive export/import. Project and notification repository settings/hooks are preserved; the app-owned nested tab repository is deliberately hardened on open as described below.

In the desktop app, contextual action history lives inside the project repository, browser-style workspace tabs live in the nested `.wimforge/tabs` Git repository, and the notification center is a separate per-user repository. A complete save carries all of them: the tab repository is captured as part of the project tree, while the notification repository is carried as its own bundle role. When the restored project opens, tab working-tree contents are restricted to `tabs.json` and imported hooks, executable configuration, index, and alternate-object controls are neutralized before any elevated commit. The core format can also represent additional repository roles and standalone support files.

## What is preserved

- ordinary project files and `.wimforge/action-history.jsonl`;
- complete project `.git` directory;
- workspace tab state and complete nested `.wimforge/tabs/.git` directory;
- notification state, events, and complete notification `.git` directory;
- repository roles and relative roots;
- standalone supporting files selected by the caller;
- file bytes, Qt permissions, modification time, size, and SHA-256;
- directory topology, including hidden/system entries.

Version 1 does not preserve NTFS ACLs, alternate data streams, sparse allocation, extended attributes, or linked-worktree object databases. A repository whose `.git` is only a worktree pointer file is refused.

## Container format

Version 1 is an uncompressed streaming format built with public Qt Core APIs:

| Part | Encoding |
| --- | --- |
| Magic | `WIMFORGE-BUNDLE` plus `0x1a` (16 bytes total) |
| Format/flags | unsigned 32-bit big-endian values |
| Manifest/payload sizes | unsigned 64-bit big-endian values |
| Manifest digest | 32-byte SHA-256 |
| Manifest | compact UTF-8 JSON |
| Payload | file bytes in manifest order |

Large sizes are stored as decimal strings in JSON to avoid floating-point rounding. Uncompressed payloads keep recovery and verification simple; a transport layer may compress the completed `.wimforge` file.

## Atomic export

Export uses `QSaveFile`. It enumerates and hashes every file, then verifies file bytes again while streaming the payload. If a source file changes, disappears, becomes unreadable, or violates the no-link rule during export, the new save is cancelled and an existing save remains intact.

Writers should be paused briefly while repositories are captured so all repositories describe the same application moment.

## Safe import

Import validates before the active destination changes:

1. Check header, version/flags, exact total length, limits, schema, and manifest SHA-256.
2. Validate every relative path, repository root, payload range, collision, and size.
3. Extract into a random sibling staging directory.
4. Verify every file SHA-256 and canonical parent containment.
5. Promote the validated staging directory by same-volume rename.

Existing destinations are refused by default. With explicit overwrite, the old destination is first renamed to a safety backup. Promotion failure rolls it back. If cleanup of a successful backup is blocked, the retained backup path is returned instead of hiding it.

## Path defenses

The importer rejects:

- absolute, empty, `.`/`..`, or backslash-separated archive paths;
- Windows reserved device names and alternate-data-stream syntax;
- control characters and trailing dots/spaces;
- case-insensitive path collisions;
- files that are ancestors of other entries;
- overlapping repository roots or standalone files placed inside them;
- missing complete `.git` directory declarations;
- symbolic links and Windows junctions/reparse points;
- manifest, entry, file, and total sizes over configured limits.

Unknown versions and nonzero flags fail closed.

## Desktop workflow

- **Export:** use Complete save or enter a destination ending in `.wimforge`.
- **Import:** provide the `.wimforge` path and a destination directory in Open/Import Project.
- WimForge validates and restores the project tree, nested tab history, and notification repository, then reconnects contextual history and the notification ledger.

A legacy `.json` project export/import is still available for configuration interchange, but it is not a complete history save.

## CLI examples

```powershell
.\WimForgeCli.exe --project C:\Images\MyProject `
  bundle export C:\Backups\MyProject.wimforge

.\WimForgeCli.exe --project C:\Images\MyProject `
  --store C:\State\Notifications `
  bundle export C:\Backups\MyProject.wimforge

.\WimForgeCli.exe bundle import C:\Backups\MyProject.wimforge `
  C:\Images\RestoredProject

.\WimForgeCli.exe bundle import save.wimforge C:\Images\Restored --overwrite --json
```

Use `--overwrite` only after reviewing the destination. The safe importer protects the old destination, but a retained backup still consumes disk space and must be handled deliberately.

Implementation detail lives in [`docs/project-bundles.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/project-bundles.md).

## 香港粵語重點

Complete save `.wimforge` 會打包工程工作樹同 Git、工程內 `.wimforge/tabs` 分頁 Git，以及獨立通知 Git；普通 `.json` 匯入就只係設定交換，唔包歷史。安全 importer 會拒絕走出目的地嘅路徑、link/reparse point、collision 同唔完整 Git 宣告；匯入分頁 repo 還會清 hooks、attributes、index、alternate objects 同可執行 config。格式保留 Git 拓撲，但唔保證 NTFS ACL、ADS 或 sparse allocation 一樣；放秘密入 Git 之前請當佢會長久保留。

---

[← Notification Center](Notification-Center) · [CLI →](CLI)
