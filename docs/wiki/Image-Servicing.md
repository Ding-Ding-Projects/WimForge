# Image Servicing

WimForge turns project configuration into a dependency graph of direct executable and argument-array invocations. DISM is the servicing engine; WimForge provides planning, workspace ownership, integrity gates, execution ordering, recovery journaling, and history around it.

## Supported source forms

| Source | Project-owned working form |
| --- | --- |
| ISO file | Mounted read-only, copied into `.wimforge/work/media`, dismounted in `finally` |
| Extracted media directory | Recursively cloned into `.wimforge/work/media`; junctions are excluded |
| WIM or ESD | Cloned into `.wimforge/work/images` through recoverable publication |
| SWM set | The selected index is exported from the complete set into a working WIM |

Mounting, servicing, export, splitting, and ISO generation target the working form. Direct index inspection accepts WIM/ESD/SWM or extracted media; a raw ISO must first be mounted/extracted so **Image path** can identify `sources\install.*`. The pristine source is used for verification/preparation.

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

---

[← Getting Started](Getting-Started) · [Package Studio →](Package-Studio)
