# Getting Started

This guide takes a clean WimForge installation through a reviewable first project. Read [Safety and Recovery](Safety-and-Recovery) before using production media.

## Requirements

- Windows 10 version 1809 or newer, or Windows 11, x64
- a trusted machine-wide Git for Windows installation under protected Program Files; the elevated desktop rejects user-profile/PATH-only Git copies
- DISM, which is included with Windows
- administrator access; normal desktop launches request elevation before the application starts
- free space for an immutable source clone, working image, mount, scratch data, staged payloads, and output
- Windows ADK Deployment Tools when creating an ISO with `oscdimg`
- WinGet and network access when operator-approved OpenCode host setup or online Package Studio entries are wanted

Use only legally obtained Windows images and software payloads. WimForge does not grant Windows, application, subscription, or redistribution rights.

## Install or run portable

Open the [Windows releases page](https://github.com/Ding-Ding-Projects/WimForge/releases) and choose one of the two x64 assets from the newest successful release:

- `WimForge-Setup-x64-<version>.exe` requests administrator approval and installs under protected Program Files.
- `WimForge-portable-x64-<version>.zip` can be extracted into a trusted, access-controlled directory.

The shipped desktop executable declares `requireAdministrator`; Windows shows the UAC consent prompt before the GUI starts. Its installer therefore uses protected Program Files so an unelevated process cannot replace an adjacent Qt DLL before consent. The desktop also retains a runtime self-elevation fallback for incorrectly copied or stale binaries. A portable folder has no installer ACL guarantee: do not launch it from Downloads, Temp, a shared folder, or any location other users/processes can modify. The companion console-subsystem CLI remains available for terminal automation and reports when a selected Windows operation lacks required authority. Releases are not currently code-signed, so verify the asset source and GitHub-provided digest before use.

## Explicit OpenCode host setup

WimForge's desktop is elevated, so it does not locate or launch PATH/user-profile developer tools during startup. Open **Package Studio** and select **Verify / install now** to approve discovery and setup for the current session. An existing executable is ready only after `opencode --version` exits normally with code zero and nonempty output. If no executable is found after that explicit action:

1. An existing npm is used when available.
2. Otherwise, WimForge uses WinGet to install exact package ID `OpenJS.NodeJS.LTS` silently and noninteractively.
3. It runs `npm install -g opencode-ai@latest`.
4. It locates the installed executable and runs the same live verification.
5. Only verified completion—or the exact failure—is reported in-app.

This modifies the host's global Node/npm tool set under the elevated desktop token. Managed environments should review the exact action before approving it. If npm and WinGet are both unavailable, the failure becomes normal in-app feedback and the rest of WimForge remains usable. GPO and unattended assistants never trigger installation implicitly; they ask you to complete the explicit Package Studio action first.

## Create a project

1. Select **New project**.
2. Enter a project name and an empty/new project folder.
3. WimForge initializes `project.json`, `.wimforge/action-history.jsonl`, the project `.git`, and a dedicated `.wimforge/tabs/.git` repository for browser-style workspace tabs.
4. The Source page opens.

Project folder pickers display native Windows paths with backslashes, including drive and UNC paths. Spaces and Unicode names are preserved. WimForge still normalizes paths internally for Qt's file APIs.

工程資料夾 picker 會用 Windows 原生反斜線顯示路徑，包括磁碟機同 UNC 路徑。空格同中文等 Unicode 名稱會保留；WimForge 內部仍然會按 Qt 檔案 API 嘅需要整理路徑。

The creation sheet is an in-app, non-modal Material popup. Creating, opening, importing, and exporting projects run in the background, so the shell remains responsive while Git and bundle work completes. The project rail reports the current background step and progress.

工程 sheet 係 app 內非 modal Material popup。建立、開啟、匯入同匯出工程會喺後台做，所以 Git 或 bundle 仲處理緊時，介面仍然可以回應。工程 rail 會顯示而家做緊邊一步同進度。

Every successful configuration mutation is saved atomically and committed. Passive navigation and telemetry are not treated as output-changing actions.

## Select and inspect a source

The servicing source may be an ISO file, extracted Windows media directory, WIM, ESD, or SWM set. Use **Choose and inspect ISO / image…** or the media-folder picker on the Source page; choosing or finishing a typed source starts inspection automatically. For a raw ISO, WimForge mounts it read-only, discovers `sources\install.wim`, `.esd`, or the first `.swm`, reads its editions plus architecture, full version, and build, stores only the stable media-relative path, and attempts dismount in a `finally` cleanup path; inspection fails if Windows reports that the ISO remains attached. WimForge then builds a source-specific Microsoft Update Catalog query and searches matching Updates automatically; the Drivers section reuses the same ISO profile with a driver-specific query. Catalog results remain a review list, not an applicability verdict.

來源可以係 ISO、解壓咗嘅 Windows media 資料夾、WIM、ESD 或 SWM。用 Source 頁嘅 **揀 ISO／映像並自動檢查……** 或 media-folder picker；揀完檔案／資料夾，或者完成輸入路徑，就會自動檢查。WimForge 會讀 edition、架構、完整版本同 build，再用 ISO 設定檔自動搜尋 Microsoft Update Catalog 嘅 Updates；Drivers 頁會用同一份資料加 driver 搜尋條件。搜尋結果只係畀你審閱，唔代表已證明適用。

The reviewed plan clones the media into project-owned working space and converts non-mountable ESD/SWM input to a working WIM before servicing. Keep **clone source** enabled unless you have a specific, tested low-level reason not to; ISO and media sources are always cloned. Image, mount, output, and ISO-label controls are under **Show advanced paths** so the first source choice stays clear; each file or directory path has the appropriate picker.

正式 plan 會將 media 複製去工程自己嘅工作區，ESD／SWM 亦會先轉做可維護 WIM。除非你有明確而測試過嘅低階理由，請保持 clone source。Image、mount、output 同 ISO label 收喺 **顯示進階路徑** 入面；每個檔案或資料夾路徑都有相應 picker，唔使靠估同手打。

Set separate mount and output locations. WimForge validation rejects output that equals or overlaps source, working media, image, or mount paths.

See [Image Servicing](Image-Servicing) for the exact workspace model.

## Configure the result

The Overview page keeps the main route explicit: **Choose source → Customize → Review → Run → Validate**. Use the studios in any useful order during Customize:

Overview 會清楚顯示主流程：**揀來源 → 自訂 → 審閱 → 執行 → 驗證**。去到自訂階段，各個 Studio 可以按需要用：

- **Customize** — review automatic ISO-matched Updates/Drivers, choose or scan local payloads, and configure Windows features, capabilities, Appx, component identifiers, registry edits, answer files, and post-setup work.
- **Group Policy Studio** — search installed ADMX/ADML policies and commit selected registry-backed policy state.
- **Unattended Studio** — start from a template or import JSON/XML, then export a Microsoft answer file.
- **Package Studio** — choose software for first logon, including the Full AI Development profile.
- **WinForge Bridge** — approve declarative WinForge-family and typed deployment actions, then stage them into media.

Changes show a snackbar and become visible in History. Project saves, workspace-tab commits, notification events, history loading, plan building, payload scans, and catalog discovery are serialized background work rather than UI-thread waits. If a project/tab save fails, the queue pauses and **Retry save** appears in the project rail; resolve the underlying Git/path problem before retrying. Errors create persistent notification entries instead of stopping the program with a native modal dialog.

變更會用 snackbar 提示，亦會出現喺 History。工程同 workspace-tab 儲存、通知事件、history 載入、plan 建立、payload 掃描同 catalog 搜尋都係有次序嘅後台工作，唔會叫 UI 主執行緒企喺度等。工程／分頁儲存失敗時，隊列會暫停，工程 rail 會出現 **再試儲存**；先修好 Git 或路徑問題再試。錯誤會保留做通知，唔會彈原生 modal 對話框卡住程式。

Path fields across these studios have matching non-modal pickers: Source file/folder/mount/output, Settings automatic export, Package profile open/save, GPO documentation export, VM ISO/configuration/disk/media/evidence, and WinForge runtime/recipe/staging. Use the picker beside the specific field instead of guessing a path; its accessible name states exactly what it opens or saves.

各個 Studio 嘅路徑欄都有相應非 modal picker：Source 檔案／資料夾／mount／output、Settings 自動匯出、Package profile open／save、GPO 文件匯出、VM ISO／設定／磁碟／媒體／evidence，同 WinForge runtime／recipe／staging。請用嗰個欄位旁邊嘅 picker，唔使估路徑；無障礙名稱亦會講明究竟係開邊樣定儲邊樣。

## Review and run

Open **Review & Run** or press `Ctrl+Enter`.

1. Refresh the plan after configuration changes. **Ready for review** means generation succeeded; it does not mean anyone reviewed or approved it.
2. Inspect each executable and argument list, dependencies, required elevation, and destructive marker.
3. Resolve missing sources, payloads, mounts, or unsafe path errors.
4. Export a PowerShell review script if a peer-review artifact is useful.
5. Only after review, use the separate **Run reviewed plan** action and confirm in the in-app sheet.

Plan 顯示「準備好俾你檢查」只代表產生成功，唔代表有人審閱或批准。逐項對完 executable、arguments、依賴、權限同破壞標記，先用獨立 **執行已檢查計劃** 動作，再喺 app 內 sheet 確認。

Independent input verification can run in parallel; writes remain dependency ordered. Do not assume that plan validation proves payload applicability to the selected Windows build.

## Undo and complete saves

| Gesture | Result |
| --- | --- |
| `Ctrl+Z` | Append a compensating action for the active page context |
| `Ctrl+Shift+Z` | Open the contextual mini history manager |
| Right-click anywhere in the desktop | Open the same active-page/global mini manager at the pointer |
| History page | Selective undo/redo, restore, bookmarks, lanes, A/B diffs, raw Git commits |
| Notification drawer | Read/unread, dismiss, restore, tombstone-delete, and undo latest notification event |

Use **Complete save** or export a path ending in `.wimforge` to carry the project, nested workspace-tab history, and notification repository together. See [History Time Machine](History-Time-Machine) and [Project Bundles](Project-Bundles).

Desktop project changes use guarded merge-patch undo: unrelated later edits survive a selective undo, while a later edit to the same target path causes a conflict instead of being overwritten. Per-element filtering exists in the history core and CLI; the current desktop right-click handler operates at active-page/global scope.

## Language and demo mode

The Settings page selects English, Hong Kong Cantonese, or bilingual presentation. For a safe populated evaluation project:

```powershell
.\WimForge.exe --demo --language bilingual --page overview
```

The twelve recognized page IDs are `overview`, `source`, `customize`, `gpo`, `unattended`, `packages`, `winforge`, `plan`, `history`, `vmlab`, `settings`, and `terminal` in builds where the matching page is linked.

目前十二個有效 page ID 係 `overview`、`source`、`customize`、`gpo`、`unattended`、`packages`、`winforge`、`plan`、`history`、`vmlab`、`settings` 同 `terminal`，但 build 要有連結對應頁面先用得。

At the supported 900×640 minimum window, use vertical scrolling to reach actions below the fold. Search results activate with Enter and close with Escape; VM inventory rows select with Enter/Space; the notification drawer puts focus on its close control, closes with Escape, and returns focus to the bell. Settings categories, color schemes, and GPO policy results expose visible keyboard focus and selected-state semantics.

去到支援嘅最細 900×640 視窗，請向下捲去搵 fold 以下嘅操作。搜尋結果用 Enter 開、Escape 關；VM 清單項目用 Enter／Space 揀；通知 drawer 開啟會將 focus 放去關閉掣，Escape 關閉後 focus 返去 bell。Settings 類別、配色同 GPO policy 結果都有清楚鍵盤 focus 同已選狀態。

## Before deployment

- Review every plan operation.
- Validate unattended XML in Windows SIM against the exact image/catalog.
- Check package hashes, Authenticode publishers, licenses, and network assumptions.
- For Package Studio media, ensure the first account intended to run the plan is a local administrator; SetupComplete registers an elevated Administrators-group task, which retries on later administrator logons and removes itself only after complete success.
- Keep the pristine source and the `.wimforge` project save.
- Perform a clean install in a disposable VM.
- Inspect Setup/Panther, Package Studio, and WinForge Bridge logs.
- Exercise restart, first-logon, offline-network, resume, and failure paths.

## 香港粵語快速版

1. 用安裝程式裝去受保護 Program Files，或將 portable 解壓去受控資料夾；開啟時 Windows 會先問 UAC。
2. 在工程起始頁建立、開啟、匯入，或由最近清單繼續。新工程會建 project Git、action history 同 workspace-tab Git。
3. 用 **揀 ISO／映像並自動檢查……** 或 media-folder picker 揀來源。ISO 會唯讀掛載做 DISM inventory，確認 dismount 後只保留 `sources/install.*` 穩定相對路徑；架構、版本同 build 會自動帶去 Update Catalog 搜尋。
4. 跟住 **揀來源 → 自訂 → 審閱 → 執行 → 驗證**；進階路徑平時收起，唔使一開始就處理一大堆設定。
5. 後台儲存期間可以繼續用介面；如果見到 **再試儲存**，先處理 Git／路徑錯誤再重試。輸出後一定要在一次性 VM 做 clean install，並保留 log、hash 同復原資料。

---

[← Home](Home) · [Image Servicing →](Image-Servicing)
