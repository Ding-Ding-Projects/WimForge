# Handoff

## Root build entry points

`build.bat` and `build-installer.bat` are thin Windows entry points over the existing reviewed `scripts/bootstrap-build.ps1` implementation. They accept `/s`, `--silent`, or `SILENT=1`, return the bootstrap exit code, and do not add a second dependency or packaging implementation.

The canonical bootstrap remains responsible for resolving its allowlisted toolchain, building from an isolated commit snapshot, producing the self-contained portable archive and Inno Setup installer, verifying both artifacts, and printing their SHA-256 digests. Code signing remains disabled.

### Verification

- Parse both batch files with `cmd.exe /d /c` in the focused root-build contract test.
- Run `scripts/bootstrap-build.ps1 -Plan -RepositoryPath <checkout>` for a non-mutating dependency and route audit.
- A complete `/s` build remains the decisive artifact check and can be lengthy because it provisions Qt and the MSVC toolchain when absent.
