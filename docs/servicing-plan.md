# Servicing-plan safety model

`ServicingPlan` turns a project into a dependency graph of direct executable and argument-array invocations. It does not concatenate user paths into a shell command line. PowerShell is used only for file-system operations that need `try/finally`, rollback, or atomic publication; every embedded path is a single-quoted literal with apostrophes doubled.

## Immutable-source contract

Offline servicing clones by default. The original ISO, media folder, WIM, ESD, or SWM set is only read by verification and preparation operations.

| Source | Project-owned working form |
| --- | --- |
| ISO file | ISO is mounted read-only, copied to `.wimforge/work/media`, and dismounted in `finally` |
| Media folder | Recursive media clone in `.wimforge/work/media`; junctions are excluded |
| WIM or ESD | Recoverable file clone in `.wimforge/work/images` |
| SWM set | Selected index is exported from the entire SWM set to a working WIM |

Inspect, mount, export, and split operations address `ServicingPlanResult::workingImagePath`, never the pristine image. ISO creation reads only `ServicingPlanResult::mediaWorkspace`. The final output cannot equal the source/image, live inside source media, live inside the media workspace, or overlap the mount.

Setting `cloneSource=false` is refused unless `options.extra.allowInPlaceSourceModification=true`; this dangerous opt-in is supported only for raw image sources. ISO/media sources are always cloned.

## Integrity and ordering

With the default `options.verifyPayloads=true`, the plan hashes the source, external image, drivers, updates, packages, provisioned apps, answer files, unattended payloads, and staged files. File hashing uses `System.Security.Cryptography.SHA256`. A directory hash is the SHA-256 of a stable, sorted manifest containing each relative path and file SHA-256. Optional expected values come from `options.extra.payloadHashes` (full path or file-name key) or a staged file's `sha256` field.

Every first image/media write depends on every verification operation. Subsequent writes are serial chains, so a failed hash skips all downstream work. The `oscdimg` operation also has a direct dependency on every operation marked as an image or media write; this remains safe even if an imported plan is reordered.

The offline sequence is:

1. Hash all selected inputs in parallel.
2. Clone/extract/convert into project-owned workspace paths.
3. Inspect and mount the working image.
4. Run serialized DISM, registry, staged-file, unattended, and post-setup writes.
5. Health-scan, commit, and unmount the working image.
6. Export/split as needed.
7. Build an ISO only after the complete image/media write barrier.
8. Atomically publish final files.

Online servicing omits source-workspace, mount, image-export, split, and ISO operations. It produces `/Online` operations only.

## Staged-file schema

`options.extra.stagedFiles` is an array of objects:

```json
{
  "source": "C:/absolute/path/runtime",
  "destination": "ProgramData/WimForge/runtime",
  "scope": "image",
  "role": "winforge-runtime",
  "sha256": "optional expected file or directory-manifest hash"
}
```

`scope` is exactly `image` or `media`. Sources must be absolute and exist. Destinations are relative paths. Drive roots, UNC/absolute paths, parent or dot segments, alternate-data-stream colons, wildcard/invalid characters, trailing dots/spaces, and Windows device names are rejected. Existing destination parents are checked at execution time and a copy is refused if it would cross a reparse point.

Directory sources copy their contents recursively to the exact destination. `unattendedFiles` are also staged into `Windows/Setup/Scripts/WimForge`; explicit media/image answer-file staging remains intact, and `unattendedXmlPath` is still applied through DISM.

## Interruption behavior

Workspace clones and final outputs use sibling `.wimforge-partial` and `.wimforge-backup` paths. A previous backup is restored before retry, the new partial is completed first, and the destination is swapped only on success. ISO and image exports target same-volume partial files. SWM sets are built in a temporary directory and published as a complete set with rollback of prior parts. ISO mounting always dismounts in `finally`.

The job engine's recovery journal records operation/dependency state. These file-level publication rules ensure a crash exposes either the prior completed artifact or a recoverable partial, rather than presenting an incomplete artifact as final.

## Test coverage

`tests/servicing_plan_tests.cpp` covers ISO folders, ISO files, WIM, ESD, and SWM input; working-path consistency; source immutability; recursive image/media staging; package and unattended preservation; transitive hash gates; direct ISO write barriers; online servicing; unsafe destinations and missing inputs; quoting; atomic partial outputs; and actual execution of the generated directory-stage and self-contained SHA-256 programs.
