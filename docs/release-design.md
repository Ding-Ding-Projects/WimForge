# Release design

WimForge ships two Windows x64 release assets from every successful build of every push to `main`. The same workflow is available through `workflow_dispatch`, but its release job is intentionally restricted to `main`:

- `WimForge-Setup-x64-0.1.N.exe` is the per-user Inno Setup installer.
- `WimForge-portable-x64-0.1.N.zip` is the self-contained portable application.

`N` is the immutable GitHub Actions `run_number`. The matching Git tag is `v0.1.N`, so push and manual runs cannot reuse a version.

## Pipeline invariants

The release workflow has no path filters: every push to `main` gets its own run, even when pushes arrive faster than Windows builds finish. Distinct `run_number` values keep different runs from reusing a tag or asset name. A concurrency group based on `run_id` serializes attempts of the *same* run without coalescing or cancelling different pushes.

The Windows job performs these operations in one workspace:

1. Check out the exact triggering commit and enter an x64 MSVC environment.
2. Install Qt 6.8.3 and Inno Setup 6.7.1. All referenced Actions are pinned to reviewed commit SHAs instead of mutable tags or branches.
3. Configure CMake with Ninja, compile a Release build, require at least one registered CTest test, and run the full test set.
4. Copy `WimForge.exe`, `WimForgeCli.exe`, and target-produced companions to a clean staging directory, then run `windeployqt --release --compiler-runtime --qmldir ...` using the same Qt root as CMake.
5. Fail unless the binary carries the requested version, required Qt/QML/platform/MSVC runtime files are present, and `README.md` plus `LICENSE` are included.
6. Zip that tree, inspect the zip for its required entries, and compile `installer/WimForge.iss` against the same verified tree.
7. Fail unless the installer carries the requested version and the only top-level package files are the expected setup executable and portable zip.
8. Create a temporary **draft** GitHub Release with both direct assets and `--target` set to the triggering commit SHA.
9. Verify the draft has exactly two non-empty, fully uploaded assets whose GitHub SHA-256 digests match the local package files, then publish it and re-check the same asset contract, non-draft/non-prerelease flags, and tag target.

Publishing through a verified draft prevents an upload failure from exposing a partial public release. A retry accepts an already-valid final release, can finish a complete interrupted draft, and replaces an incomplete release only after confirming that its tag does not belong to another commit.

The build and packaging implementation is shared with local development through `scripts/build-release.ps1`.

## No Actions artifacts

Release assets and Actions artifacts are different GitHub storage systems. WimForge intentionally uses only GitHub Release assets. No artifact upload/download action is part of the workflow. The installer and portable zip go straight from the runner workspace to the release API through `gh`; they are never staged in Actions artifact storage.

GitHub's automatically generated source-code zip and tarball links are not direct release assets and do not appear in the release `assets` array. The workflow contract is exactly two entries in that array: the setup executable and portable zip.

This can be audited without downloading a release:

```powershell
gh api repos/codingmachineedge/WimForge/actions/artifacts --jq '.total_count'
gh release view --repo codingmachineedge/WimForge --json tagName,isDraft,isPrerelease,assets
```

The first command should print `0`. The second should show `false` for both release flags and exactly the setup executable and portable zip.

## Local release build

Prerequisites:

- Windows x64 with Visual Studio 2022 Build Tools and the MSVC x64 environment active
- CMake and Ninja on `PATH`
- Qt 6.8 x64 for MSVC 2022 on `PATH` (`QT_ROOT_DIR` is also honored)
- Inno Setup 6 (`ISCC.exe` on `PATH` or in its standard installation directory)
- Git and PowerShell 7

From the repository root:

```powershell
./scripts/build-release.ps1 -Version 0.1.0
```

The script deliberately deletes and recreates `build/release` and `dist`; both locations are constrained to children of the repository and may not contain one another. It treats `README.md`, `LICENSE`, the GUI and CLI binaries, version metadata, Qt/QML runtime files, the platform plugin, and MSVC runtime as release gates rather than optional files. Pass `-SkipTests` only for local packaging diagnostics. A normal CI release always runs registered tests.

## Safety and trust

The installer is per-user and does not itself require elevation. WimForge requests administrative authority only when an image-servicing operation needs it. The current servicing backend is Windows DISM, so release builds are Windows-only and should be exercised against disposable image copies before production use.

The release pipeline does not currently sign the executable or installer. GitHub publishes a SHA-256 digest for each uploaded release asset, but code signing remains a future hardening step. `windeployqt` handles Qt dependencies; any future non-Qt dynamic dependency must also be staged beside `WimForge.exe` and verified in a clean Windows VM.

Reference documentation:

- [Qt for Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html)
- [`install-qt-action` inputs](https://github.com/jurplel/install-qt-action)
- [`gh release create`](https://cli.github.com/manual/gh_release_create)
- [GitHub REST release-asset fields and SHA-256 digest](https://docs.github.com/en/rest/releases/assets)
- [GitHub Actions concurrency behavior](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency)
