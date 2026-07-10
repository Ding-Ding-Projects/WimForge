# Safety and Recovery

WimForge is intentionally conservative because image servicing, unattended installation, package execution, and registry changes can destroy deployable media or create insecure systems. A green UI state is evidence that WimForge's own checks passed, not proof that Windows or third-party installers will behave correctly.

## Safety layers

| Layer | Protection |
| --- | --- |
| Planning | Inspectable dependency graph with executable/arguments, elevation, destructive flags, and dry-run export |
| Source ownership | Offline sources clone into project-owned image/media workspaces by default |
| Integrity | SHA-256 gates for sources/payloads/staged files; Authenticode publisher checks for signed installers |
| Structured commands | Executable plus argument arrays; shell wrappers/evaluated strings refused in Package Studio and Bridge |
| Ordering | Parallel independent verification; serial image/media writes; ISO waits for every write |
| Atomic files | `QSaveFile`, sibling partial files, backups, and same-volume rename promotion |
| Crash state | Operation/dependency journal flushed through job transitions |
| History | Project/action/notification changes create local Git commits; undo appends history |
| Portable recovery | `.wimforge` saves include complete repositories and import into validated staging |
| User experience | Errors/recovery use persistent notifications and non-modal in-app sheets rather than blocking dialogs |

## Threat boundaries

WimForge protects its own project and staging contracts. It cannot guarantee or reverse:

- DISM behavior for a particular undocumented/unsupported component combination;
- bytes after an image has been committed outside the project transaction;
- an installer that changes the host or target machine;
- a remote download whose vendor content changes after review;
- Windows Setup behavior not validated against the exact target image;
- external `SetupComplete.cmd` logic WimForge did not author;
- credentials or product keys exposed through files, logs, Git history, media, or screenshots;
- firmware serial uniqueness or vendor driver quality.

Undo restores recorded configuration so the output can be rebuilt from a pristine source. It is not a VM snapshot or arbitrary filesystem time machine.

## Path and link defenses

Servicing staging, bundles, Package Studio, and WinForge Bridge reject unsafe relative paths, parent traversal, Windows reserved names, alternate data streams, unsafe trailing characters, and paths escaping their expected root. Symlinks, junctions, and reparse points are refused where following them could cross a trust boundary.

Output cannot overlap source, mount, working media, or working image paths.

## Crash recovery

When a job is interrupted, WimForge exposes an in-app recovery surface with:

- journaled operation/dependency state and a rebuilt plan for explicit review;
- undo of the latest project-configuration change, without claiming to reverse external side effects;
- an elevated safe discard-unmount for the absolute mount path and run ID captured by that interrupted journal; and
- the journal path and current recovery summary.

The safe-unmount action is available only while an interrupted run is pending, no other servicing job is active, and WimForge is elevated. It launches `dism.exe /English /Unmount-Image /MountDir:<journal-path> /Discard` directly. A failed start, abnormal exit, or nonzero exit leaves the original journal unchanged so the recovery surface remains available. After DISM succeeds, WimForge reopens the journal, verifies that its run ID was not replaced, and atomically marks it `recovered-discarded` with the recovery time and mount path. It never substitutes a mount path that the user edited in the project after the crash.

**Undo latest configuration** is a separate action. It changes Git-backed project configuration only; it does not claim to discard a mount, reverse a completed DISM write, or roll back a child installer.

Filesystem transactions may leave clearly named partial or backup paths for manual inspection. They are not silently presented as final output.

Windows itself tracks mounted images. If the app or OS dies during a DISM mount, inspect `dism /Get-MountedWimInfo` and use Microsoft-supported commit/discard/cleanup procedures. Never delete a mount tree blindly while DISM still owns it.

## Secrets and Git

Do not store passwords, API tokens, private KMS host keys, MAKs, domain join credentials, Wi-Fi secrets, or reusable installer credentials in project profiles, notifications, answer files, screenshots, or command arguments.

Deleting a secret in a later commit does not remove it from older Git objects. If exposure occurs:

1. Rotate/revoke the secret first.
2. Stop distributing affected repositories, bundles, media, and logs.
3. Follow the organization's incident and history-rewrite procedure.
4. Rebuild clean artifacts and verify that backups/caches are handled.

Microsoft notes that hiding some answer-file passwords is not encryption. Restrict file ACLs and remove cached answer files only after all required setup passes finish. See [Hide Sensitive Data in an Answer File](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/hide-sensitive-data-in-an-answer-file).

## Production deployment checklist

- [ ] Use a pristine, legally obtained source and preserve its hash.
- [ ] Keep clone-source mode and nonoverlapping work/output paths.
- [ ] Review every operation, argument, destructive marker, and external tool.
- [ ] Verify every driver/update/package is intended for the target build and architecture.
- [ ] Verify offline/direct installer SHA-256 and Authenticode publisher.
- [ ] Review vendor licenses, redistribution, sign-in, subscriptions, and hardware requirements.
- [ ] Keep credentials out of Git-backed state.
- [ ] Validate answer XML in Windows SIM against the exact WIM/catalog.
- [ ] Export a complete `.wimforge` save before execution.
- [ ] Boot and install in a disposable VM with networking both present and absent.
- [ ] Inspect DISM, Setup/Panther, Package Studio, and WinForge Bridge logs.
- [ ] Exercise cancellation, reboot, first-logon, resume, failure, and rollback paths.
- [ ] Test representative physical hardware before broad deployment.
- [ ] Document hashes, source versions, approvals, and known exceptions.

## Recovery is not backup

The journal helps WimForge identify and review an interrupted operation without guessing its external completion state. Git preserves configuration history. `.wimforge` preserves those repositories. None replaces offline backups, source media, VM snapshots, or organizational disaster recovery.

For implementation details, read [`docs/servicing-plan.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/servicing-plan.md), [`docs/context-history.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/context-history.md), and [`docs/project-bundles.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/project-bundles.md).

---

[← WinForge Bridge](WinForge-Bridge) · [Building and Releases →](Building-and-Releases)
