# NTLite Feature Comparison

WimForge is an independent open-source alternative, not an NTLite clone and not an NTLite product. This page maps broad workflow areas honestly so users can decide whether WimForge is ready for a particular job.

NTLite changes over time. Its [official feature page](https://www.ntlite.com/features/) and [documentation](https://www.ntlite.com/docs/) are authoritative for NTLite; this page is authoritative only for WimForge's current implementation.

Status meanings:

- **Implemented** — core model, validation, plan, and/or UI exist in this repository.
- **Partial** — usable lower-level support exists, but important breadth/intelligence/UI is missing.
- **Not implemented** — do not infer support from a nearby generic field.

## Image and media

| Workflow | WimForge | Notes |
| --- | --- | --- |
| ISO/media/WIM/ESD/SWM input | **Implemented** | ISO/media clone, raw image clone, SWM index export to working WIM |
| Image-index inspection/selection | **Implemented** | DISM-based metadata and edition selection |
| Immutable working source | **Implemented** | Clone-by-default contract is stricter than in-place editing |
| WIM/ESD export | **Implemented** | Atomic partial/backup publication |
| SWM split output | **Implemented** | Complete set built/promoted together |
| BIOS + UEFI ISO creation | **Implemented** | Uses source boot files and ADK `oscdimg`; tool must be installed |
| Image conversion/recompression controls | **Partial** | Output format/compression and DISM export exist; no claim of every NTLite optimization/recompression mode |
| Live/online servicing | **Partial** | Core emits `/Online` operations; desktop workflow centers on offline cloned images |
| Remote-machine servicing | **Not implemented** | No remote administration surface |

## Drivers, updates, packages, and Windows features

| Workflow | WimForge | Notes |
| --- | --- | --- |
| Driver integration | **Implemented** | Driver files/folders, DISM plan, and host-driver export helper |
| Update/CAB/MSU integration | **Implemented** | User supplies payloads; hashing and dependency gates apply |
| Integrated update downloader/cache | **Not implemented** | No Windows Update catalog downloader or applicability resolver |
| Optional features enable/disable | **Implemented** | Windows feature identities through DISM |
| Capabilities/FOD add/remove | **Implemented** | Identity/payload correctness remains the user's responsibility |
| Provisioned Appx remove/provision | **Implemented** | Identity/payload workflow; no Store browser |
| Component-store cleanup/ResetBase | **Implemented** | ResetBase is explicitly destructive because installed updates become non-removable |
| Component removal | **Partial** | Low-level package identity removal; no mature component database, dependency intelligence, templates, or compatibility promises |
| Dedicated language-pack UI/intelligence | **Partial** | Packages/capabilities can represent payloads, but no specialized language workflow |

## Configuration and automation

| Workflow | WimForge | Notes |
| --- | --- | --- |
| Offline registry changes | **Implemented** | Typed set/delete state in servicing plan |
| Installed ADMX/ADML policy catalog | **Implemented** | Reads every definition/language in selected store; schema-driven controls and bilingual docs |
| Curated tweak/privacy compatibility library | **Partial** | Generic settings, registry, and installed GPO definitions exist; no NTLite-sized curated compatibility database |
| Windows services editor | **Not implemented** | No dedicated service inventory/dependency UI |
| Scheduled-tasks editor | **Not implemented** | No dedicated task inventory or safe disable/remove engine |
| Unattended Windows Setup | **Implemented** | JSON/XML, seven passes, templates, computer-name modes, GVLKs; Windows SIM validation still required |
| Post-setup commands | **Implemented** | Transparent SetupComplete entries plus structured Package/Bridge runners; review raw entries carefully |
| Presets/import/export | **Implemented** | WimForge JSON, package/unattend/recipe profiles, and complete `.wimforge` saves |
| NTLite preset import compatibility | **Not implemented** | WimForge does not claim to parse NTLite preset formats |
| CLI and deterministic JSON | **Implemented** | Project/config/plan/apply/history/notifications/studios/bundles; exact compiled help is authoritative |

## WimForge-specific workflow

| Workflow | WimForge | Notes |
| --- | --- | --- |
| Per-project local Git repository | **Implemented** | Configuration commits after successful user mutations |
| Append-only contextual history | **Implemented** | Hash chain, selective compensation, redo-of-undo, restore, bookmarks, lanes, diffs |
| Right-click mini history | **Implemented** | Non-modal active-page/global context surface plus `Ctrl+Shift+Z`; element IDs are available to the CLI/core |
| Separate Git notification ledger | **Implemented** | New/read/unread/dismiss/restore/delete and Git revert/redo |
| Complete-save file with `.git` databases | **Implemented** | Safe uncompressed `.wimforge` streaming container |
| Package-manager studio | **Implemented** | WinGet/npm/pip/signed direct/offline/structured custom providers, resume state |
| Full AI Development template | **Implemented** | Common toolchains plus OpenCode/Codex/Claude; unverified desktop slots remain disabled |
| Automatic host OpenCode setup | **Implemented** | Async Node/npm fallback and verified OpenCode install |
| OpenCode-assisted GPO/unattended intent | **Implemented** | AI output must parse and validate; no bypass of user approval/history |
| WinForge-family OEM bridge | **Implemented with contract limits** | Typed recipe/staging/bootstrap; audited legacy runtime supports only page deep-links |
| Non-modal in-app feedback | **Implemented** | Snackbars, drawer, recovery sheets, inline validation; jobs continue |
| English/HK Cantonese/bilingual UI | **Implemented** | Translation coverage is application-authored; installed GPO translations depend on ADML availability |

## Important parity gaps

WimForge should not be selected on the assumption that the following mature-tool capabilities already exist:

- a deeply curated, Windows-build-aware component removal database;
- dependency/compatibility recommendations derived from years of field testing;
- integrated Windows Update discovery/download/applicability workflows;
- dedicated service and scheduled-task editors;
- broad live-host and remote-host management UX;
- NTLite preset compatibility;
- commercial support guarantees, signed releases, or an established deployment certification matrix.

WimForge's tests validate its schemas, paths, graph barriers, atomic publication, and generated artifacts. They cannot test every Windows build, language, edition, driver, update, installer, or hardware fleet.

## Where WimForge intentionally differs

- History is first-class and append-only instead of an ephemeral undo stack.
- Notifications have an independent Git ledger.
- A single save can carry complete local repository topology.
- Package selection, installed GPO schema, unattended profiles, and WinForge-family replay live in the same project/history model.
- Source cloning, hash gates, typed commands, and draft-first release verification are explicit contracts.
- The source code and application are MIT-licensed.

These differences do not make every workload safer automatically. The ISO author still needs pristine source media, payload provenance, Windows SIM validation, VM/hardware testing, and operational approvals.

## Official references

- NTLite [features](https://www.ntlite.com/features/), [documentation](https://www.ntlite.com/docs/), [image](https://www.ntlite.com/docs/image/), [features configuration](https://www.ntlite.com/docs/features/), [unattended](https://www.ntlite.com/docs/unattended/), [updates](https://www.ntlite.com/docs/updates/), and [apply](https://www.ntlite.com/docs/apply/)
- Microsoft [DISM overview](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/what-is-dism?view=windows-11) and [answer-files overview](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/answer-files-overview)

---

[← Building and Releases](Building-and-Releases) · [Home](Home)
