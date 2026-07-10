# Unattended Studio

Unattended Studio keeps editor intent in portable WimForge JSON and exports the Microsoft Windows answer-file XML consumed by Setup. The separation matters: JSON is the complete editable project profile; XML is the deployment artifact.

WimForge validation is an early safety check, not a replacement for Windows System Image Manager (Windows SIM). Always validate against the exact target image or matching catalog.

## Built-in starting profiles

- **Full Automation** — a conservative baseline with Windows PE language/input, EULA acceptance, Dynamic Update, registered owner, Hong Kong locale, China Standard Time, OOBE choices, and random computer-name mode.
- **AI Development Workstation** — starts from Full Automation, selects serial-based naming with the `AI` prefix, and records the `ai-development` package-profile identifier in metadata.

The metadata marker does not install software by itself; Package Studio stages the software workflow.

Templates are starting points, not compliance declarations. Review every field.

## The seven setup passes

| Pass | Purpose |
| --- | --- |
| `windowsPE` | Windows PE and Setup choices before the image is applied |
| `offlineServicing` | Supported packages, drivers, updates, and settings applied offline |
| `generalize` | Sysprep removal of machine-specific state |
| `specialize` | Machine-specific configuration after the image is associated with the target |
| `auditSystem` | System-context audit-mode settings before audit logon |
| `auditUser` | User-context audit settings after audit logon |
| `oobeSystem` | OOBE/Windows Welcome and supported first-logon configuration |

Selecting a pass in WimForge does not make an unsupported component/setting combination valid. Microsoft recommends architecture-specific answer files and validation against the exact image.

## Computer-name modes

| Studio mode | Exported `ComputerName` | Extra behavior |
| --- | --- | --- |
| Random | `*` | Windows generates a name |
| Fixed | validated literal | No rename command |
| Prompt | `*` | First-logon prompt validates, renames, and restarts |
| Serial | `*` | First logon reads/sanitizes BIOS serial, renames, and restarts |

Fixed names use a conservative subset: 1–15 ASCII bytes, letters/digits/hyphen, no leading/trailing hyphen, and not numeric-only.

`[Prompt]` and `%SERIAL%` are not Microsoft `ComputerName` values. WimForge never writes them literally. Prompt/Serial generate explicit first-logon behavior and remove stale generated rename commands when the mode changes.

Prompt/Serial happen after setup has already used a generated temporary name. Domain join, audit mode, UAC, S mode, firmware serial quality, and first-logon availability all need environment-specific testing. 粵語速讀：方括號唔係魔法；真係要問人，就清清楚楚喺首次登入先問。

## Import and export

Portable schema `wimforge.unattend`, version 1, preserves:

- profile name/description and free-form metadata;
- settings, pass, component identity, architecture, path and WCM attributes, and values;
- answer-file placement choices;
- computer-name mode, fixed value, and serial prefix.

XML import/export preserves represented `settings/component` data, not byte-for-byte source formatting. Comments, whitespace, prefix spelling, arbitrary package sections, editor-only metadata, and indistinguishable repeated sibling nodes are not guaranteed to round-trip. Keep JSON beside the exported XML when future editing fidelity matters.

Exports are atomic. XML uses the Microsoft unattended namespace, setup-pass order, grouped components, escaped values, and supported WCM attributes.

## GVLK selection and licensing

The catalog contains Microsoft-published Generic Volume License Keys for supported Windows client volume editions. A GVLK configures a KMS client; it does not grant a license, prove entitlement, activate by itself, or replace a retail/MAK key.

Never put private KMS host keys, MAKs, passwords, tokens, or reusable organization credentials into a Git-backed project. If a secret is committed, rotate it first: deleting it in a later commit does not remove older Git objects.

Read Microsoft's [KMS client keys](https://learn.microsoft.com/en-us/windows-server/get-started/kms-client-activation-keys).

## OpenCode-assisted fill

The intent helper sends a bounded prompt to the local OpenCode CLI. Returned data must parse as a supported unattended JSON profile and pass WimForge validation before it is saved, exported, and committed. The generated proposal remains undoable with `Ctrl+Z`.

AI assistance is not Windows SIM validation and must not be used to invent credentials or licensing data.

## CLI examples

```powershell
.\WimForgeCli.exe unattend template full --output profile.json
.\WimForgeCli.exe unattend template ai-development --output autounattend.xml
.\WimForgeCli.exe unattend import vendor.xml --output editable.json
.\WimForgeCli.exe unattend export editable.json --output autounattend.xml
.\WimForgeCli.exe unattend computer-name editable.json --mode prompt --output autounattend.xml
.\WimForgeCli.exe unattend computer-name editable.json --mode fixed --value BUILD-PC
.\WimForgeCli.exe unattend computer-name editable.json --mode serial --prefix AI
.\WimForgeCli.exe unattend gvlk list --edition Enterprise --json
```

## Required release gate

1. Save the editable JSON.
2. Resolve WimForge validation errors and review warnings.
3. Export XML to a new path.
4. Load the exact target WIM/catalog and XML in Windows SIM.
5. Choose **Tools → Validate Answer File** and resolve every error.
6. Revalidate for each architecture and whenever the base image changes.
7. Perform a clean VM installation and inspect Windows Setup/Panther logs, OOBE, first logon, restart, prompt, and serial failure paths.

Primary Microsoft references: [Answer Files Overview](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/answer-files-overview), [`ComputerName`](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-computername), and [authoring best practices](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/best-practices-for-authoring-answer-files).

Implementation detail lives in [`docs/unattended-studio.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/unattended-studio.md).

---

[← Group Policy Studio](Group-Policy-Studio) · [History Time Machine →](History-Time-Machine)
