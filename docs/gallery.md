---
title: Screenshot Gallery
description: A route-by-route tour of the WimForge Material desktop application.
---

# Screenshot Gallery

These captures use the populated, non-destructive demo project in bilingual
English and Hong Kong Cantonese mode.
Paths are intentionally neutral, every route uses the same viewport and
scale, and no real Windows image, product key, credential, or private project is
shown.

呢套截圖用已填好、唔會實際改映像嘅 demo 工程，預設同時顯示 English 同自然香港粵語。所有路徑都係中性測試資料，唔會放真實 Windows 映像、product key、密碼或私人工程入鏡。

## Project Start / 工程起始頁
![WimForge Project Start page in bilingual English and Hong Kong Cantonese, with create, open, import, and recent-project actions](screenshots/project-start.png)

開啟 WimForge 會先到呢個類似 Visual Studio 嘅工程管理頁；你可以建立新工程、開啟現有資料夾、匯入 `.json` / `.wimforge`，或由最近工程清單繼續。

## Overview
![WimForge Overview with the project metrics, four-step build flow, safety rails, navigation, and current-job status](screenshots/overview.png)

## Source and editions
![Source and editions page with source inspection, clone-before-editing guidance, mount workspace, and output settings](screenshots/source.png)

## Customize
![Customize page with image change categories, reversible project controls, and configuration fields](screenshots/customize.png)

## Group Policy Studio
![Group Policy Studio with the installed-policy catalog, a selected Delivery Optimization policy, desired-state tabs, a schema-generated numeric editor, and a Git-backed commit action](screenshots/group-policy.png)

## Unattended Studio
![Unattended Studio with answer-file profile controls, setup passes, validation, and computer-name settings](screenshots/unattended.png)

## Package Studio
![Package Studio with the Full AI Development profile, software search, package providers, and enabled package cards](screenshots/package-studio.png)

## WinForge Bridge
![WinForge Bridge with typed recipe actions, runtime capability information, and verified OEM staging controls](screenshots/winforge-bridge.png)

## Virtual Machine Lab
![Virtual Machine Lab with installed-provider discovery, managed and external inventory, lifecycle controls, exact-operation review, and structured validation evidence](screenshots/virtual-machine-lab.png)

## Review and run
![Review and run page with operation validation, dependency-aware plan details, and explicit run controls](screenshots/review-run.png)

## History and recovery
![History Time Machine with the append-only action timeline, branch controls, undo and restore actions, and A/B comparison pane](screenshots/history.png)

## Settings
![Settings page with language, theme, density, motion, project, concurrency, and recovery preferences](screenshots/settings.png)

## Embedded terminal
![Embedded Terminal with a trusted elevated shell selector, working directory, bounded in-app ConPTY output, command input, and process-tree stop controls](screenshots/embedded-terminal.png)

!!! info "Reproduce the gallery"
    Configure `build-capture` with
    `-DWIMFORGE_DOCUMENTATION_CAPTURE=ON`, build its Debug `WimForge` target,
    then run `scripts/capture-documentation-screenshots.ps1`. This restricted
    as-invoker harness accepts only demo screenshot runs; normal and release
    builds remain elevated. The script launches each route and saves a frame
    directly from its Qt Quick window.

    標準畫廊以 `--language bilingual` 拍攝；全套截圖要用同一個 commit、theme、viewport 同 DPI，而且路徑一定要保持中性。
