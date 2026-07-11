# Image Servicing

WimForge turns project configuration into a dependency graph of direct executable and argument-array invocations. DISM is the servicing engine; WimForge provides planning, workspace ownership, integrity gates, execution ordering, recovery journaling, and history around it.

## Supported source forms

| Source | Project-owned working form |
| --- | --- |
| ISO file | Mounted read-only, copied into `.wimforge/work/media`, dismounted in `finally` |
| Extracted media directory | Recursively cloned into `.wimforge/work/media`; junctions are excluded |
| WIM or ESD | Cloned into `.wimforge/work/images` through recoverable publication |
| SWM set | The selected index is exported from the complete set into a working WIM |

Mounting, servicing, export, splitting, and ISO generation target the working form. Direct index inspection accepts WIM/ESD/SWM and extracted media. Raw ISO inspection temporarily mounts the source read-only, records the discovered `sources/install.*` relative path, and confirms dismount before returning. The later servicing plan uses that path inside its extracted workspace, never a temporary optical-drive letter. Because recovery-compressed ESD and split SWM containers are not mounted directly, the selected source index is first exported to a project-owned working WIM; subsequent mount/export operations address index 1 of that converted WIM. ISO output recovery-compresses it back to `install.esd` or regenerates the complete `install*.swm` set before `oscdimg` runs.

Setting `cloneSource=false` is refused unless `options.extra.allowInPlaceSourceModification=true`, and that dangerous opt-in applies only to raw image sources. ISO and media inputs are always cloned.

## Configuration coverage

The current planner represents:

- source/index inspection and mounting;
- driver folders or files;
- update and package payloads;
- optional feature enable/disable;
- capability/FOD add/remove;
- provisioned Appx remove/provision;
- component package removal by identity;
- typed offline registry writes/deletes;
- unattend XML and additional unattended files;
- generic staged image/media files;
- post-setup items;
- component-store cleanup, health scan, commit/discard, and unmount;
- WIM/ESD export, SWM splitting, and ISO creation.

This is low-level configuration coverage, not a curated compatibility database. In particular, component removal identities and package applicability vary across Windows releases and editions.

## Integrity gates and dependencies

With payload verification enabled, the graph hashes the source, image, drivers, updates, packages, provisioned applications, answer files, unattended payloads, and staged files. File hashes use SHA-256. Directory hashes are derived from a stable sorted relative-path/file-hash manifest.

Every first write depends on all required verification nodes. Image and media writes form serialized chains. ISO creation depends on the complete image/media write barrier, including staged Package Studio and WinForge files. Reordering an imported plan cannot bypass those dependencies.

The normal offline sequence is:

1. Hash independent inputs in parallel.
2. Clone/extract/convert into the project workspace.
3. Inspect and mount the working image.
4. Apply serialized DISM, registry, staging, unattended, and post-setup writes.
5. Health-scan, commit, and unmount.
6. Export or split into the requested image format.
7. Create ISO media after all writes.
8. Promote final output atomically.

## Structured execution

DISM and other tools are launched as an executable with a tokenized argument array; user paths are not concatenated into a general shell command line. PowerShell is reserved for bounded filesystem transactions requiring `try/finally`, rollback, or atomic promotion, with paths encoded as literals.

The Review & Run page exposes the preview command, arguments, bilingual description, dependency status, administrative requirement, and destructive flag. The CLI can export the same plan as a reviewable script.

## Staged files

Package Studio and WinForge Bridge ultimately feed a validated staged-file model:

```json
{
  "source": "C:/absolute/path/runtime",
  "destination": "ProgramData/WimForge/runtime",
  "scope": "image",
  "role": "winforge-runtime",
  "sha256": "optional expected hash"
}
```

`scope` is `image` or `media`. Source paths are absolute and must exist; destinations are relative and cannot contain traversal, alternate-data-stream syntax, reserved Windows names, wildcard components, or unsafe trailing characters. Reparse-point parents are refused during execution.

## Outputs and tools

- WIM and ESD output uses DISM export operations.
- SWM output builds and promotes a complete split set.
- ISO output reads only the media workspace and requires `oscdimg`, normally installed with Windows ADK Deployment Tools.
- Partial files and sibling backups prevent an interrupted publish from masquerading as a completed artifact.

Output cannot equal the source/image, live under source media or the working media tree, or overlap the mount.

## Online servicing

The core planner supports a target-online mode that emits `/Online` operations and omits source cloning, mounting, image export/split, and ISO generation. The desktop experience is primarily designed around safer offline, cloned media. Treat online changes as host mutations that project history cannot physically rewind.

## Failure and recovery

The job engine journals operation and dependency state on transitions. Recovery appears inside the application with actions to rebuild and review the plan, undo the latest configuration change, or safely discard-unmount; it is not a blocking system dialog. Rebuild never guesses that an external step completed. Configuration undo changes only project state.

Safe unmount requires elevation and no active servicing job. It uses the absolute mount path recorded by the interrupted journal and runs DISM `/Unmount-Image /Discard` directly. A failed start or nonzero result leaves the original journal untouched. Only after DISM succeeds does WimForge verify the recorded run ID and atomically close that journal as `recovered-discarded`. Workspace/output transactions also keep partial and backup paths for retry.

WimForge cannot guarantee that Windows will accept an applicable-looking payload, and its journal does not replace Windows mount cleanup. Use `dism /Get-MountedWimInfo` and Microsoft-supported cleanup procedures when an interrupted mount remains registered.

## Authoritative references

- Microsoft [What is DISM?](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/what-is-dism?view=windows-11)
- Microsoft [DISM command-line options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/deployment-image-servicing-and-management--dism--command-line-options?view=windows-10)
- NTLite's independent [Image page](https://www.ntlite.com/docs/image/) for comparison context

The implementation-level contract is documented in [`docs/servicing-plan.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/servicing-plan.md).

## 香港粵語重點

WimForge 會將原始 ISO、media folder、WIM、ESD 或 SWM 複製去工程 workspace 先寫，預設唔會改你嘅來源。檢查 ISO 時只會唯讀掛載，搜到 `sources/install.*` 後確認 dismount；服務 plan 只記穩定相對路徑，唔會依賴臨時光碟機字母。ESD/SWM 需要先 export 成工程自己嘅 WIM 先掛載；輸出再按你揀嘅格式回復壓縮或分割。任何 DISM 成功都唔等於新映像一定開到機，最後一定要用乾淨 VM 安裝。

---

[← Getting Started](Getting-Started) · [Package Studio →](Package-Studio)
