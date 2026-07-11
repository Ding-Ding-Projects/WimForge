# WimForge Unattended Studio / WimForge 無人值守安裝工作室

Unattended Studio builds portable WimForge profiles and Microsoft Windows answer files. It is designed to keep editor intent separate from the XML that Windows Setup actually consumes: editor conveniences live in the versioned JSON profile, while exported XML uses Microsoft-valid settings and configuration passes.

無人值守安裝工作室會建立可攜式 WimForge 設定檔，同埋 Windows Setup 真正會讀取嘅答案檔。編輯器功能放喺 JSON；交畀 Windows 嘅就係標準 XML。兩樣嘢分清楚，安裝時就少啲「估你唔到」意外。

> [!IMPORTANT]
> WimForge validation is an early safety check, not a replacement for Windows System Image Manager (Windows SIM). Always validate the exported XML against the exact Windows image or catalog that will be deployed.
>
> WimForge 內置檢查只係第一道防線，唔可以代替 Windows SIM。一定要用實際部署嗰個 Windows 映像或 catalog 再驗證。

## Profile and answer-file model / 設定檔與答案檔

An unattended setting has these parts:

- a setup pass;
- a component name and architecture;
- the component identity fields (`publicKeyToken`, `language`, and `versionScope`);
- an XML path, including attributes such as `wcm:action` and `wcm:keyValue`;
- a string value.

The portable JSON profile also records the profile name and description, deployment-placement choices, computer-name mode, serial prefix, and editor metadata. The XML export contains only Windows answer-file settings. Placement choices tell the later media-building stage where an answer file should be copied; they are not XML elements.

JSON 設定檔會保存編輯器資料；XML 就只保存 Windows Setup 認得嘅設定。簡單講：JSON 係「工程檔」，XML 係「交貨檔」。

## Computer names: Microsoft values versus editor conventions / 電腦名稱：Microsoft 規則同編輯器暗號

### Microsoft's `ComputerName` rules

Microsoft documents `Microsoft-Windows-Shell-Setup/ComputerName` as follows:

- If the setting is omitted, Windows generates a random name.
- `*` or an empty value asks Windows to generate a random 15-character name.
- A supplied name has a maximum of **15 bytes**, not 15 Unicode characters.
- It cannot contain spaces or Microsoft's listed forbidden punctuation, and it must pass Windows DNS-name validation. A numeric-only name is invalid.
- The setting is supported in `offlineServicing` and `specialize`; WimForge emits it in `specialize`.

See Microsoft's [`ComputerName` reference](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-computername).

WimForge v1 deliberately applies a stricter, predictable fixed-name rule: 1–15 ASCII bytes, using only `A-Z`, `a-z`, `0-9`, and `-`; no leading or trailing hyphen; and not numeric-only. This conservative subset avoids byte-count and compatibility surprises. For example, `PC-123456789012` is valid at exactly 15 bytes, while `PC-1234567890123` is too long.

香港粵語速讀：最多係 **15 bytes**，唔係「睇落十五隻字就算」。WimForge 暫時只收英文字母、數字同連字號，穩陣行先。

### Why `[Prompt]` is not written literally

`[Prompt]` is an **NTLite editor convention**, not a value defined by Microsoft for `ComputerName`. NTLite's developer describes selecting Prompt—or typing `[Prompt]`—as an instruction to make NTLite add its own prompting behavior. NTLite community explanations further clarify that dynamic values depend on a companion script and scheduling inserted into the image; copying the token into an ordinary answer file is not sufficient. See the [NTLite developer's Prompt explanation](https://www.ntlite.com/community/index.php?threads/wmi-issues-am-i-using-ntlite-properly.1464/) and the [dynamic-name script explanation](https://www.ntlite.com/community/index.php?threads/issue-with-serial-computername-and-windows-11-24h2.5125/).

The brackets in `[Prompt]` are forbidden in a native Microsoft computer name. WimForge therefore rejects the literal string. Prompt mode instead:

1. exports `<ComputerName>*</ComputerName>` so the answer file remains valid;
2. adds an `oobeSystem` `FirstLogonCommands/SynchronousCommand` with `wcm:action="add"`;
3. marks `RequiresUserInput` as `true`;
4. runs a generated PowerShell prompt at first administrative logon;
5. accepts only the same 1–15-byte safe subset, then calls `Rename-Computer -Restart`.

The prompt is a first-logon operation, not a native Windows Setup computer-name prompt and not an exact timing clone of NTLite. Until it completes and restarts, earlier setup work—such as a domain join—sees the Windows-generated temporary name. First-logon commands also have account, UAC, audit-mode, and Windows S mode constraints; review Microsoft's [`FirstLogonCommands`](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-firstlogoncommands) and [`RequiresUserInput`](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-firstlogoncommands-synchronouscommand-requiresuserinput) documentation before deployment.

一句講晒：`[Prompt]` 係 NTLite 暗號，唔係 Microsoft 魔法咒語。直接塞落 XML 會「炒車」；WimForge 會輸出合法嘅 `*`，再安排自己嘅改名命令。

### `%SERIAL%` and Serial mode

`%SERIAL%` is likewise an NTLite-side dynamic convention, not a Microsoft `ComputerName` variable. NTLite's generated image logic must resolve it; Windows Setup does not expand a literal `%SERIAL%`. The NTLite forum documents both [using `%SERIAL%` in NTLite's Unattended mode](https://ntlite.com/community/threads/how-do-you-standardize-custom-computer-names.4158/) and the fact that the [companion script must be installed/scheduled](https://www.ntlite.com/community/index.php?threads/issue-with-serial-computername-and-windows-11-24h2.5125/).

WimForge Serial mode never writes `%SERIAL%` into `ComputerName`. It exports `*`, then generates a first-logon PowerShell command that:

- reads `Win32_BIOS.SerialNumber` through CIM;
- removes everything except letters, digits, and hyphens;
- uppercases the result;
- sanitizes the optional prefix and limits it to eight characters;
- trims the complete name to 15 characters and removes a trailing hyphen;
- skips renaming if the result is empty or numeric-only;
- restarts only after a valid rename.

Virtual machines and some firmware return empty, generic, duplicated, or unexpectedly long serial numbers. Test the exact hardware fleet and define an operational fallback; a serial-derived value is not guaranteed to be unique.

`%SERIAL%` 唔係 Windows 環境變數。WimForge 會自己讀 BIOS serial、清理、截短；如果攞唔到可靠結果，就唔會亂改名。冇 serial 都唔應該靠想像力作一個出嚟。

### Mode summary / 模式總覽

| Studio mode | XML `ComputerName` | Additional behavior |
|---|---|---|
| Random / 隨機 | `*` | Windows generates a name. |
| Fixed / 固定 | Validated literal | No rename command. |
| Prompt / 詢問 | `*` | Validated first-logon prompt, then rename and restart. |
| Serial / 序號 | `*` | Sanitized BIOS-serial first-logon rename and restart. |

Changing from Prompt or Serial to Fixed or Random removes WimForge's generated first-logon rename command. This prevents an old command from overriding the newly selected mode.

The [Docker provisioning service](docker-provisioning.md) resolves central hardware inventory to this same Fixed mode before Windows Setup starts. The name is applied during `specialize`, before OOBE. Domain-join settings may run in `offlineServicing` or `specialize`, so validate ordering against the exact join workflow rather than assuming the name precedes every join action. The hosted path requires an explicit WinPE/PXE download and `setup.exe /unattend` handoff; a container URL is not itself an answer-file discovery location.

## The seven configuration passes / 七個設定階段

Windows Setup processes settings in phases. A component setting is legal only in the passes listed for that setting in Microsoft's unattended reference; merely choosing a pass in WimForge does not make an unsupported combination valid. Microsoft's overview is [Windows Setup Configuration Passes](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/windows-setup-configuration-passes?view=windows-11).

| Pass | Purpose | 香港粵語提示 |
|---|---|---|
| `windowsPE` | Configures Windows PE and Windows Setup before the target image is applied: UI language, disks, image selection, setup credentials, and related setup choices. | 開機安裝環境嗰一關；揀碟、語言等通常喺呢度。 |
| `offlineServicing` | Applies supported packages, drivers, updates, and settings to an offline Windows image. | Windows 未開機，先幫個映像「執貨」。 |
| `generalize` | Runs with Sysprep generalization to remove machine-specific state so an image can be reused. | 封裝前清走每部機獨有資料，唔好影印埋身份證。 |
| `specialize` | Applies machine-specific configuration after the image is associated with the target computer. | 呢部機正式「認頭」；固定電腦名通常放呢度。 |
| `auditSystem` | Applies system-context audit-mode settings before audit-mode user logon. | Audit mode 系統層先做。 |
| `auditUser` | Applies user-context settings after audit-mode logon. | Audit mode 登入後再做用戶層設定。 |
| `oobeSystem` | Applies settings before and during OOBE/Windows Welcome, including supported OOBE and first-logon configuration. | 交機前最後一關；OOBE 同首次登入相關設定喺呢度。 |

Microsoft's [Answer Files Overview](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/answer-files-overview) lists the same seven passes and explains that a setting may be valid in one or more of them.

Architecture matters. Microsoft recommends separate answer files for each architecture. A common mixed deployment uses x86 components in `windowsPE` when booting 32-bit Windows PE and amd64 components for a 64-bit target in later passes. Do not duplicate x86 and amd64 components blindly: settings may run twice or incorrectly. See Microsoft's [answer-file authoring best practices](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/best-practices-for-authoring-answer-files).

## Import, export, and preservation / 匯入、匯出與保留範圍

### Portable JSON

JSON is the editable project representation. Schema `wimforge.unattend`, version `1`, preserves every v1 field represented by the engine:

- profile name and description;
- all settings, component identity values, paths, attributes, and values;
- media/install/boot placement flags, dual-architecture intent, and edition-selection intent;
- computer-name mode, fixed value, and serial prefix;
- the free-form `metadata` object.

Unknown schema names, versions, setup-pass names, or structurally incomplete settings are rejected on import. Future extension data should be stored under `metadata` or in path-attribute maps; unknown top-level JSON properties are not promised to survive a load/save cycle. JSON export may be used for an incomplete draft, so run validation before producing deployment XML.

### Windows answer-file XML

XML import/export preserves the represented component settings rather than the source file's bytes. The exporter:

- emits the Microsoft unattended namespace as the default namespace;
- emits the seven passes in Windows Setup order;
- groups settings by component identity;
- rebuilds nested XML and escapes values;
- preserves ordinary unknown path attributes and namespace-correct `wcm:*` attributes;
- keeps repeated list items distinct when their path attributes distinguish them, such as unique `wcm:keyValue` values.

The following are intentionally **not** byte-for-byte preserved:

- comments, whitespace, original element order within grouped components, and namespace-prefix spelling;
- profile description, placement switches, computer-name editor mode, serial prefix, and metadata, because these are not Windows answer-file elements;
- sections outside recognized `settings/component` trees, including package sections;
- indistinguishable repeated sibling elements that have identical names and identical attribute maps;
- arbitrary extension namespaces other than the supported WCM namespace.

After XML import, the profile name defaults to the source filename. Existing `ComputerName` XML remains a normal imported setting; XML does not carry the Studio mode that originally produced it. Save a JSON profile alongside XML whenever future editing fidelity matters.

香港粵語速讀：JSON 先係完整工程檔；XML 係重新砌出嚟嘅 Windows 答案檔。註解、排版同編輯器資料唔會扮識飛，自動由 XML 飛返入 JSON。

### Safe round-trip workflow

1. Save or export the portable JSON profile.
2. Validate in WimForge and resolve every error. Review warnings rather than automatically ignoring them.
3. Export XML to a new path. Export uses an atomic save, so a failed write does not intentionally leave a half-written answer file.
4. Open and validate that XML in Windows SIM against the exact target image.
5. Test the resulting media in a disposable virtual machine and, for serial-dependent behavior, representative physical hardware.
6. Keep the JSON and the validated XML together, but keep secrets out of both whenever possible.

## Product keys and licensing / 產品金鑰與授權

WimForge exposes a curated set of Microsoft-published Generic Volume License Keys (GVLKs) for supported Windows client volume editions. Every catalog entry carries the Microsoft documentation URL and a licensing notice.

A GVLK is a **KMS client configuration key**. It:

- does not grant a Windows license;
- does not prove entitlement;
- does not activate Windows by itself;
- is not a substitute for a retail key or a Multiple Activation Key (MAK);
- requires a properly licensed volume edition and an authorized, compatible KMS or Active Directory-based activation environment.

Microsoft explicitly cautions that KMS client keys are intended for volume-licensing scenarios and cannot activate or serve as retail license keys. Read [KMS client activation and product keys](https://learn.microsoft.com/en-us/windows-server/get-started/kms-client-activation-keys) before selecting one.

Do not confuse a published GVLK with your organization's private KMS host key or MAK. Never commit private activation material to a profile repository.

粵語版：公開 GVLK 只係「叫部機去搵公司 KMS」嘅設定，唔係免費 Windows。見到鎖匙個樣，唔代表你已經有樓契。

## Passwords and answer-file security / 密碼與答案檔安全

Treat every answer file and portable profile as sensitive, even when it contains no password today. Product keys, local or domain credentials, Wi-Fi secrets, join information, and command-line arguments can all become exposed through the file, copied media, logs, backups, or Git history.

Minimum safeguards:

- Prefer provisioning mechanisms that retrieve short-lived secrets at deployment time instead of embedding reusable passwords.
- Restrict file and directory ACLs to approved operators and deployment services.
- Never publish a secret-bearing profile, answer file, ISO, build log, screenshot, or Git repository.
- Remember that removing a secret in a later Git commit does **not** erase it from earlier commits. Rotate an exposed credential first, then rewrite history according to your incident process.
- Review generated commands for secrets in arguments and environment variables.
- Protect deployment media physically and cryptographically where appropriate.
- Remove cached or embedded answer files after their final required pass. Do not delete them before pending `oobeSystem` settings run.

Windows SIM can “hide” certain local-account passwords, but Microsoft states that this is **not encryption** and provides no general security benefit; domain passwords, product keys, and other data may remain readable. See [Hide Sensitive Data in an Answer File](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/hide-sensitive-data-in-an-answer-file). Microsoft also recommends restricting access and removing the cached answer file from `%WINDIR%\Panther` before delivery, while preserving it until all required passes have completed; see [Best Practices for Authoring Answer Files](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/best-practices-for-authoring-answer-files).

Git 真係好記性：你今日刪咗個密碼，尋日嗰個 commit 仲記得。先換密碼，再處理歷史；唔好同 Git 比記性。

## Validation in Windows SIM / 用 Windows SIM 驗證

WimForge checks its own invariants: profile schema/version, known pass names in JSON, component/path presence, fixed computer-name safety, and rejection of literal `[Prompt]`. It cannot prove that a component exists in a particular Windows build, that a setting is valid in the selected pass, or that edition- and architecture-specific dependencies are satisfied.

Windows SIM validates an answer file against a Windows image or catalog. Microsoft recommends validating manually authored files and revalidating whenever they are reused because available settings and defaults can change. Follow this release gate:

1. Install the Windows Assessment and Deployment Kit components that provide Windows SIM.
2. Open the exact target `install.wim` image or its matching catalog. Select the correct edition and architecture.
3. Open the exported answer file.
4. Choose **Tools → Validate Answer File**.
5. Resolve every error in the Messages pane. Investigate warnings and document any deliberate exception.
6. Revalidate each architecture-specific answer file and every time the base Windows image changes.
7. Perform a clean virtual-machine installation, then inspect Windows Setup and Panther logs. Exercise OOBE, first logon, restart, and any prompt/serial failure path.

Microsoft's [Validate an Answer File](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/validate-an-answer-file) procedure explains that Windows SIM compares the answer-file settings with those available in the loaded image. If every setting appears not to exist, verify that the catalog and `processorArchitecture` match before rewriting the file; Microsoft's [Create or Open an Answer File](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/wsim/create-or-open-an-answer-file) troubleshooting guide calls out this common mismatch.

通過 WimForge 檢查，只代表「格式同基本規則冇爆」。通過 Windows SIM，再加乾淨 VM 真機測試，先至叫做有部署證據。綠色剔號唔係護身符，但係冇剔號就更加唔好出貨。

## Built-in starting profiles / 內置起步設定

`Full automation` supplies a conservative baseline with Windows PE language/input settings, EULA acceptance, Dynamic Update, registered owner, Hong Kong user locale, China Standard Time, OOBE choices, and Random computer-name mode.

`AI development workstation` starts from that baseline, selects Serial mode with the `AI` prefix, and records the `ai-development` package-template identifier in JSON metadata. Package installation itself belongs to the package/bundle pipeline; the metadata marker alone does not install software.

Templates are starting points, not compliance declarations. Review every value, remove settings you do not require, validate against the target image, and test the resulting media before deployment.
