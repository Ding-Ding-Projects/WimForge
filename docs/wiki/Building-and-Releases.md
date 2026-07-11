# Building and Releases

WimForge builds on Windows x64 with C++20, Qt 6.8.3, CMake, and MSVC. The repository produces a Material GUI (`WimForge.exe`) and a console CLI (`WimForgeCli.exe`).

## Development prerequisites

- Windows x64
- Visual Studio 2022 Build Tools with **Desktop development with C++**
- CMake 3.24 or newer
- an x64 Windows SDK
- Qt 6.8.3 for MSVC 2022 x64 with Core, Gui, Qml, Quick, Quick Controls 2, and Quick Dialogs 2
- Git
- PowerShell
- Ninja for the release script
- Inno Setup 6 for installer builds

## Configure and test with Visual Studio

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 `
  -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
$runtime = Join-Path (Resolve-Path .).Path 'build\dev-runtime'
cmake --install build --config Debug --prefix $runtime
```

Run a safe populated demo:

```powershell
& "$runtime\WimForge.exe" --demo --language bilingual --page overview
& "$runtime\WimForgeCli.exe" --json package template ai-development
```

The absolute install prefix is required by Qt's deploy script and produces a self-contained developer runtime. Launching the naked Visual Studio output without either this install step or the matching Qt `bin` directory on `PATH` can fail with a missing `Qt6Guid.dll`. The demo's `--page` option is useful for visual QA of individual studios. Use only IDs compiled into the build.

## Configure with Ninja

The shared release path expects `QT_ROOT_DIR` or Qt tools on `PATH`:

```powershell
$env:QT_ROOT_DIR = 'C:\Qt\6.8.3\msvc2022_64'
$env:PATH = "$env:QT_ROOT_DIR\bin;$env:PATH"

cmake -S . -B build/ninja -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH="$env:QT_ROOT_DIR" `
  -DBUILD_TESTING=ON
cmake --build build/ninja --parallel
ctest --test-dir build/ninja --output-on-failure
```

## Test coverage

Registered CTest executables cover:

- project configuration and Git revert history;
- notification state/events and notification Git history;
- hash-chained contextual action history;
- `.wimforge` bundle fidelity and hostile imports;
- servicing paths, source immutability, dependency/hash barriers, staging, publication, and online plans;
- package schema, trust, dependencies, first-logon generation, and AI template;
- installed-policy ADMX/ADML parsing, localization, regex, and documentation;
- unattended JSON/XML, passes, names, GVLKs, templates, and safety validation;
- WinForge recipe/contract validation, link/path defenses, staging, bootstrap, and PowerShell parser checks;
- CLI command, JSON-envelope, response-file, bundle, and history behavior.

Tests inject or inspect external operations; they do not intentionally service a real Windows image or install packages on the developer machine.

## Documentation screenshot build

The shipped desktop requests administrator rights by default. For automated
documentation only, configure the restricted capture harness:

```powershell
cmake -S . -B build-capture -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64 `
  -DWIMFORGE_DOCUMENTATION_CAPTURE=ON -DBUILD_TESTING=OFF
cmake --build build-capture --config Debug --target WimForge --parallel
./scripts/capture-documentation-screenshots.ps1
```

That build embeds `asInvoker` and exits unless it receives both `--demo` and
`--screenshot`. It is not a servicing build. The default, bootstrap, package,
and release configurations keep the audited `requireAdministrator` manifest.

## Bootstrap a release build

From a clean WimForge checkout, first inspect the no-change plan and then run the maintained bootstrap:

```powershell
.\scripts\bootstrap-build.ps1 -Plan
.\scripts\bootstrap-build.ps1
```

When the script is downloaded outside a checkout, it searches the current/script ancestry first and otherwise plans a clone of `https://github.com/codingmachineedge/WimForge.git` to the explicit `-RepositoryPath` or the default user source directory. Start it from a normal, non-administrator 64-bit Windows PowerShell session. Per-user Ninja and aqt repair runs first under that original identity. The script requests UAC only when a bounded machine package-repair child is needed and passes that child an allowlisted set of exact WinGet package IDs plus the already validated, signed App Installer executable from protected Program Files. This remains valid when UAC uses separate administrator credentials: the child never installs user-scoped tools, executes a user-profile Qt tool, or invokes source-controlled build logic. After it exits, the parent keeps aqt's Qt archive-hash verification enabled while installing Qt under the normal token, then verifies the live CMake, Git, Ninja, MSVC, x64 Windows SDK tools/libraries, Qt MSVC/x64 components, and Inno Setup evidence. Normal website usage therefore downloads Qt, clones, configures, compiles, tests, and packages without an administrator token before delegating to the release entrypoint below.

The bootstrap intentionally refuses a dirty checkout: the packaged `build-info.json` records a commit, so publishing bytes produced from uncommitted or untracked source would be unverifiable. It explicitly overrides Git settings that hide untracked files and rejects assume-unchanged or skip-worktree index flags. The release itself runs from a unique local clone pinned to that verified commit, which excludes ignored working-tree files and gives every run fresh build/output paths; tracked source and HEAD are checked again before artifacts are accepted. It never runs `git clean`, reset, checkout, stash, or force-update against the user's checkout. Build/output deletion is confined to a marker-owned `build-bootstrap` directory after reparse-point checks. Logs use unique names by default, an explicit existing log is appended rather than overwritten, and successful artifacts are checked for type/size and printed with SHA-256.

Automation has external limits. Windows 10/11 x64, 64-bit PowerShell 5.1, UAC consent or administrator credentials, Microsoft App Installer/WinGet for the invoking user, internet access to GitHub/WinGet/Qt archives, vendor availability, and adequate disk space must be available. Enterprise proxy or installation policy, package-source outages, pending reboots, licensing decisions, and code signing cannot be bypassed responsibly; those conditions stop with a retained diagnostic log. Exact WinGet IDs still resolve catalog versions current at run time, so logs and artifact hashes provide traceability but cannot promise identical toolchain bytes across dates. The website convenience command follows mutable `main`, so replace `main` with a reviewed commit SHA when a reproducible bootstrap source is required.

## Local release build

With Qt, Ninja, and Inno Setup available:

```powershell
$env:QT_ROOT_DIR = 'C:\Qt\6.8.3\msvc2022_64'
.\scripts\build-release.ps1 -Version 0.1.0
```

The script constrains build/output paths to repository children and refuses paths that contain one another. It deletes/recreates those controlled directories, configures Ninja Release with `WIMFORGE_BUILD_VERSION`, compiles, and runs CTest unless `-SkipTests` was explicitly selected for diagnostics. A normal test run is also rejected if CTest registered zero tests.

Release gates require:

- the requested version stamped into `WimForge.exe` and the installer;
- both `WimForge.exe` and `WimForgeCli.exe`;
- `README.md`, `LICENSE`, and `build-info.json`;
- Qt Core/Gui/QML/Quick/Quick Controls 2 DLLs;
- deployed QML module plugins and `platforms/qwindows.dll`;
- the MSVC runtime;
- a portable zip whose entries are inspected after compression;
- exactly two nonempty top-level package outputs.

Successful output:

```text
dist/WimForge-Setup-x64-0.1.0.exe
dist/WimForge-portable-x64-0.1.0.zip
```

## GitHub release trigger

The workflow runs for every push to `main`. It also supports `workflow_dispatch`, but the release job is explicitly restricted to the `main` branch. Each run gets version/tag `0.1.<run_number>` / `v0.1.<run_number>`.

Different main pushes are not coalesced. The concurrency group is based on `run_id`, so it only prevents attempts of the same run from racing over one release tag.

CI checks out the exact triggering SHA, enters an x64 MSVC environment, installs Qt 6.8.3 and Inno Setup, then invokes the same local release script.

## Draft-first publication contract

Publication uses an explicit temporary draft:

1. Calculate local SHA-256 for installer and portable zip.
2. Create a draft release targeted at the triggering commit with exactly those two direct assets.
3. Require both assets to be fully uploaded, nonempty, correctly named, and no extras.
4. Compare GitHub's returned SHA-256 asset digests with the local files.
5. Publish the draft.
6. Recheck non-draft/non-prerelease state, the two-asset/digest contract, and concrete tag SHA.

An upload failure therefore cannot expose a partial public release. Retry behavior is idempotent: it accepts a valid final release, can publish a complete interrupted draft, and removes/replaces an incomplete same-commit release. A tag belonging to another commit is a hard collision and is never replaced.

## No Actions artifacts

The workflow contains no upload-artifact or download-artifact action. Installer and zip move directly from runner workspace to GitHub Release storage. GitHub-generated source archives are not entries in the release `assets` array.

Audit with:

```powershell
gh api repos/codingmachineedge/WimForge/actions/artifacts --jq '.total_count'
gh release view --repo codingmachineedge/WimForge `
  --json tagName,isDraft,isPrerelease,assets
```

The first result should be `0`; the release should be final/non-prerelease with exactly the versioned setup executable and portable zip.

## Signing and trust

Executables and installers are not currently code-signed. GitHub provides release-asset SHA-256 digests and the workflow verifies uploaded digests, but publisher identity through Authenticode remains a future hardening step. Test both installer and portable package in a clean Windows VM before broad use.

Read [`docs/release-design.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/release-design.md) for the pipeline contract. Primary references: [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html), [`install-qt-action`](https://github.com/jurplel/install-qt-action), and [`gh release create`](https://cli.github.com/manual/gh_release_create).

## 香港粵語建置提示

Visual Studio 直接產生嘅 `build\Debug\WimForge.exe` 唔係自我完備版；要先 `cmake --install` 去絕對 `dev-runtime` 路徑，Qt/MSVC runtime 先會對齊，否則可能出現少 `Qt6Guid.dll`。正常 release 會 build、跑全部 tests、編 installer、驗便攜 ZIP，再先上載去 draft release 做 hash/asset/commit 合約檢查。公開版現時未簽名，請在乾淨 VM 測 installer 同 portable，並核對 GitHub SHA-256。

---

[← Safety and Recovery](Safety-and-Recovery) · [NTLite Feature Comparison →](NTLite-Feature-Comparison)
