# Projects and Sources

A WimForge project is a normal directory containing declarative configuration and local Git history. Selecting a source or customization changes project intent; it does not mount or service an image until a plan has been reviewed and confirmed.

## Create, open, and import

Use **New project** to choose a project name and directory. WimForge creates `project.json`, initializes the project repository, and creates the action-history journal under `.wimforge`.

The project sheet accepts three forms:

- an existing project directory containing `project.json`;
- a portable JSON configuration, imported into a destination directory; or
- a complete `.wimforge` bundle, validated and restored into a destination directory.

JSON is configuration interchange only. A [Project Bundle](Project-Bundles) carries the complete project tree—including hardened nested workspace-tab history—and the notification repository, including Git objects and undo history.

Successful output-affecting changes use the canonical project save path: write `project.json`, create a project commit, then append the corresponding contextual action. If the secondary action-history append fails, the already-safe project commit is retained and WimForge raises a warning.

## Supported source forms

The **Source & editions** page accepts:

- an ISO file;
- an extracted Windows media directory;
- a WIM or ESD image; or
- the first part of a split SWM set.

Use **Browse ISO / image** or **Browse media folder** instead of typing paths. Selecting or dropping a source immediately inventories it. For a raw ISO, WimForge mounts the file read-only, discovers `sources\install.wim`, `install.esd`, or `install.swm`, runs DISM inventory, and confirms dismount. The project stores only the stable internal relative path—not the temporary drive letter—so the servicing plan can extract the ISO into its project-owned media tree later.

## Source, image, mount, and output paths

These fields have different responsibilities:

| Field | Meaning |
| --- | --- |
| **Source path** | The original ISO, media directory, or image supplied by the operator |
| **Image path** | The WIM/ESD/SWM that DISM addresses; raw ISO sources retain a stable internal `sources/install.*` mapping instead |
| **Mount path** | An empty directory used for an offline mount |
| **Output path** | The final WIM, ESD, SWM, or ISO destination |

Keep **Clone source before editing** enabled. Offline planning then creates project-owned image/media workspaces rather than using the original as the default write target. ISO and media sources are cloned even when low-level in-place behavior is requested.

Validation rejects paths that overlap source, image, mount, working media, or output boundaries. Do not work around that check with junctions or reparse points; trust-boundary staging rejects them where traversal could escape the expected root.

## Select an edition

After inspection, choose the target edition from the discovered names or enter its one-based image index. WimForge stores the discovered edition names and bilingual inventory summary in the project options, so reopening the project restores the same inventory without inventing a placeholder edition. Inspection clamps an older selection to the returned edition count; loading imported or restored history applies the same in-memory safety clamp before planning. Edition names and available indexes vary by source, so do not assume an index copied from another ISO still identifies the same edition.

檢查完成之後，可以由已發現版本名揀目標，亦可以輸入由 1 開始嘅映像索引。WimForge 會將版本名同雙語 inventory 摘要儲存喺工程 options，重開工程就會還原同一份清單，唔會作一個假版本出嚟。如果舊選擇超出今次版本數量，檢查時會夾返入有效範圍；匯入或者還原歷史之後，載入時亦會先做同一個記憶體安全夾限，先至建立計劃。唔同來源嘅版本名同索引可以完全唔同，唔好假設另一隻 ISO 嘅同一個索引仍然係同一版本。

## Choose an output

The desktop offers WIM, ESD, SWM, and ISO output. ISO creation also needs the Windows ADK Deployment Tools program `oscdimg`. The volume-label field is limited to 32 characters in the desktop.

WimForge builds output into project-owned work paths and promotes validated final files rather than presenting a partial file as complete. Output planning, split-image behavior, media staging, and ISO ordering are described in [Image Servicing](Image-Servicing).

## Before customization

Before adding payloads:

1. Record the source origin and SHA-256.
2. Inspect the exact image and architecture.
3. Keep source, mount, scratch, and output on storage with sufficient free space.
4. Preserve a pristine source outside the project work tree.
5. Decide whether the final artifact is an image or complete bootable media.

Then continue to [Customize](Customize). Before execution, review [Safety and Recovery](Safety-and-Recovery).

## 香港粵語快速版

工程起始頁有四條清晰路：建新工程、開現有 `project.json` 資料夾、匯入 `.json` / `.wimforge`，或開最近工程。Source 頁要分清 **Source path**、**Image path**、**Mount path** 同 **Output path**；輸出/掛載同來源重疊會被拒絕。請用 picker，唔好靠手打光碟機字母；ISO 檢查會唯讀掛載、搜 `sources/install.*`、做 DISM inventory，再確認 dismount。版本清單同雙語摘要會跟工程保存，超出範圍嘅舊索引會先夾返有效值。預設保持 clone source，正式輸出前先去 Customize 同 Review & Run。

---

[← Application Tour](Application-Tour) · [Customize →](Customize)
