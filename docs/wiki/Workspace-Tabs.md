# Workspace Tabs / 工作空間分頁

Every project has browser-style workspace tabs. Choosing a page from the navigation rail opens that page as a tab or activates its existing tab. Tabs can be closed, moved, renamed, and styled with a font family, font size, color, bold, italic, and strikeout. `Ctrl+W` closes the active tab; `Ctrl+Tab` and `Ctrl+Shift+Tab` move through the tab strip.

每個工程都有好似 browser 咁嘅工作空間分頁。由 navigation rail 揀功能頁時，WimForge 會開一個分頁，或者切去已經開咗嗰個。分頁可以關閉、排位、改名，亦可以分別改字體系列、字號、顏色、粗體、斜體同刪除線。`Ctrl+W` 關而家呢個分頁；`Ctrl+Tab` 同 `Ctrl+Shift+Tab` 前後切換。

## Project-local Git history / 工程內 Git 歷史

Tab state is stored in `.wimforge/tabs/tabs.json`. The containing `.wimforge/tabs` folder is its own local Git repository, separate from `project.json` history. Opening, activating, moving, closing, renaming, styling, and importing portable definitions creates a normal bilingual English / Hong Kong Cantonese commit in that repository. A complete-repository import restores its existing history instead of adding an artificial import commit. Closing the application or switching projects therefore does not lose the project workspace.

分頁狀態儲喺 `.wimforge/tabs/tabs.json`；`.wimforge/tabs` 本身係一個本機 Git，同 `project.json` 歷史分開。開啟、啟用、移動、關閉、改名、改樣式同匯入可攜式定義都會建立 English / 香港粵語雙語 commit。完整 repo 匯入會復原原有歷史，唔會假加一個 import commit；關 app 或轉工程亦唔會遺失工作佈局。

The state file uses a versioned `org.wimforge.workspace-tabs` JSON format. Page identifiers and style values are validated, titles and font families are bounded, colors accept only `#RRGGBB` or Qt's `#AARRGGBB`, and a project is limited to 200 tabs. These machine identifiers stay language-neutral even though user-visible commit subjects are bilingual.

狀態檔案用有版本嘅 `org.wimforge.workspace-tabs` JSON 格式。頁面 ID 同樣式值都會驗證，標題/字體名有長度上限，顏色只收 `#RRGGBB` 或 Qt `#AARRGGBB`，每個工程最多 200 個分頁。呢啲 machine identifier 會保持中性，只有人會見到嘅 commit subject 用雙語。

## Moving tabs between projects / 跨工程搬分頁

The tab-strip menu has two portability levels:

- **Portable tabs (`.wftabs`)** contain the tab definitions and styles. Import merges them into the current project and gives colliding IDs new values.
- **Complete tab Git repository (`.wftabrepo`)** carries current state plus commits, refs, reflogs, and Git objects. Import validates it in an adjacent random staging directory, rejects extra working-tree files/links, neutralizes hooks, executable Git configuration, alternate object paths, and the imported index, then initializes safe local configuration. Same-volume renames promote it while a temporary backup permits rollback if promotion fails; the backup is removed after success.

分頁列 menu 有兩種搬運方式：

- **可攜式分頁 (`.wftabs`)** 只有分頁定義同樣式。匯入會 merge 入現有工程；如果 ID 撞咗，會重新配一個新值。
- **完整分頁 Git 資料庫 (`.wftabrepo`)** 連而家嘅狀態、commits、refs、reflogs 同 Git objects 一齊搬。匯入會先喺旁邊隨機 staging 資料夾驗證，拒絕多餘工作樹檔案或 link，再中和 hooks、可執行 Git config、alternate object path 同匯入 index。同磁碟 rename 會提升新 repo，臨時 backup 可以喺失敗時 rollback；成功後 backup 會刪除。

A complete `.wimforge` project bundle also carries the project's `.wimforge/tabs` folder. Opening the restored project applies the same tab-repository hardening before any elevated automatic commit.

完整 `.wimforge` 工程 bundle 亦包含 `.wimforge/tabs`。開復原工程時，任何提權自動 commit 之前都會先用同一套分頁 repo 強化步驟。

Do not edit `.git` manually. If a portable import is all that is needed, prefer `.wftabs`; use a complete repository bundle when the audit history needs to travel too.

唔好手動改 `.git`。只需搬分頁同樣式就用 `.wftabs`；連 audit 歷史一齊搬先用完整 repo bundle。

---

[← Project Bundles / 工程組合檔](Project-Bundles) · [Settings / 設定 →](Settings)
