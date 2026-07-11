# Screenshots / 截圖

The canonical gallery covers Project Start plus all twelve desktop routes. Every image comes from the
same build, populated non-production demo, bilingual English/Hong Kong Cantonese UI mode, 1,440×900 Qt Quick
client area, and 96-DPI PNG output. A route-specific public fixture keeps paths,
Git history, settings, and notification state deterministic without exposing a
real Windows image, private project, account name, or secret.

標準畫廊包括工程起始頁同全部十二個桌面功能頁。每幅圖都由同一個 build、同一套無害 demo 資料、English / 香港粵語雙語模式、1,440×900 client area 同 96 DPI 產生。公開 fixture 只用中性路徑同虛構資料，唔會露出真實映像、帳戶、秘密或私人工程。

## Complete application gallery

### Project Start / 工程起始頁

![WimForge Project Start showing bilingual create, open, import, and recent-project actions](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/project-start.png)

呢個係 app 開啟後嘅第一個畫面：可以建立新工程、開啟現有資料夾、匯入 `.json` / `.wimforge`，或由最近清單繼續。

### Overview

![WimForge Overview showing project metrics, the four-step build flow, safety rails, and current-job status](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/overview.png)

### Source and editions

![Source and editions showing neutral ISO and working-image paths, clone-before-editing, edition selection, mount path, and output format](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/source.png)

### Customize

![Customize showing the update and language-package queue, navigation across all configuration categories, and a neutral demo payload](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/customize.png)

### Group Policy Studio

![Group Policy Studio showing the installed ADMX catalog, a selected Delivery Optimization policy, three-state draft controls, a schema-generated numeric editor, registry target, and Git-backed commit action](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/group-policy.png)

### Unattended Studio

![Unattended Studio showing computer-name behavior, Microsoft-published installation keys, the generic answer-file editor, and setup-pass values](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/unattended.png)

### Package Studio

![Package Studio showing the Full AI Development profile, provider-backed software cards, selection count, and staging controls](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/package-studio.png)

### WinForge Bridge

![WinForge Bridge showing typed recipe actions, runtime contract detection, portable recipe controls, and verified ISO staging](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/winforge-bridge.png)

### Virtual Machine Lab

![Virtual Machine Lab showing installed VMware and VirtualBox provider discovery, managed and external inventory, lifecycle controls, exact-operation review, and validation evidence](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/virtual-machine-lab.png)

### Review and run

![Review and run showing the reviewed operation list, deterministic verification steps, concurrency control, checkpoint setting, and explicit run button](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/review-run.png)

### History and recovery

![History Time Machine showing append-only actions, branch and bookmark controls, guarded undo and restore actions, and the A/B comparison pane](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/history.png)

### Settings

![Settings showing language, Material theme, interface density, motion, concurrency, scratch-space reserve, and failsafe controls](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/settings.png)

### Embedded terminal

![Embedded Terminal showing a trusted administrator shell selector, project working directory, bounded in-app ConPTY output, command input, and process-tree stop actions](https://raw.githubusercontent.com/codingmachineedge/WimForge/main/docs/screenshots/embedded-terminal.png)

## Reproduce the gallery

Build the restricted documentation harness, then run the committed capture
script from the repository root. The harness uses an `asInvoker` manifest so a
12-route automation run does not display 12 UAC prompts, and it refuses to run
unless both `--demo` and `--screenshot` are present. Normal and release builds
still embed `requireAdministrator`.

```powershell
cmake -S . -B build-capture -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 `
  -DWIMFORGE_DOCUMENTATION_CAPTURE=ON -DBUILD_TESTING=OFF
cmake --build build-capture --config Debug --target WimForge --parallel
./scripts/capture-documentation-screenshots.ps1
```

The script launches each route with `--demo --language bilingual --page <id>`, gives
every route a clean public fixture and notification ledger, waits for the window
to settle, and uses WimForge's `--screenshot` option to save the normalized Qt
Quick client area. It fails if a route exits unsuccessfully, omits its image, or
produces dimensions inconsistent with the rest of the set.

| Page | Page ID | Image |
| --- | --- | --- |
| Project Start / 工程起始頁 | startup capture | `project-start.png` |
| Overview | `overview` | `overview.png` |
| Source and editions | `source` | `source.png` |
| Customize | `customize` | `customize.png` |
| Group Policy Studio | `gpo` | `group-policy.png` |
| Unattended Studio | `unattended` | `unattended.png` |
| Package Studio | `packages` | `package-studio.png` |
| WinForge Bridge | `winforge` | `winforge-bridge.png` |
| Virtual Machine Lab | `vmlab` | `virtual-machine-lab.png` |
| Review and run | `plan` | `review-run.png` |
| History and recovery | `history` | `history.png` |
| Settings | `settings` | `settings.png` |
| Embedded terminal | `terminal` | `embedded-terminal.png` |

## Capture contract

- Use one application commit, theme, bilingual language mode, viewport, and DPI for the
  primary set.
- Keep source, project, output, profile, notification, and application-data
  paths under the public screenshot fixture.
- Never include credentials, private keys, customer names, private hostnames,
  organization data, or proprietary payload names.
- Capture the client area directly; do not include desktop clutter, another
  window, cursor effects, capture gutters, or post-capture rescaling.
- Regenerate every route when the shared shell, primary data fixture, capture
  normalization, or screenshot contract changes.
- Use descriptive alt text that names the route and the meaningful visible
  state.

Secondary dark-theme, single-language English/Cantonese, density, minimum-viewport, and
overlay matrices are useful visual-regression work, but they should remain
clearly named QA sets rather than replacing this stable primary tour.

截圖合約重點：主畫廊一定要用 `--language bilingual`，全套保持同 commit、theme、viewport 同 DPI。路徑、通知、工程名同 payload 都要用公開 fixture；唔好放客戶名、私人 hostname、credential、product key 或專有 payload 入鏡。如果 shared shell、主 fixture 或 capture normalization 有改，就要重拍工程起始頁同全部十二個功能頁。

## What a screenshot does not prove

A screenshot demonstrates layout and populated state at one instant. It does
not prove that a control is keyboard or screen-reader accessible, that every
theme/scale/language is responsive, that a servicing plan succeeded, that the
resulting image boots, or that an answer file passed Windows SIM validation.
Pair images with tests, logs, hashes, and disposable-VM evidence appropriate to
the claim.

See [Safety and Recovery](Safety-and-Recovery), [Troubleshooting](Troubleshooting),
and [Contributing](Contributing).

---

[← Application Tour](Application-Tour) · [Troubleshooting →](Troubleshooting)
