# Customize

The **Customize** page records image intent in eight sections. Each successful edit is saved and committed before it can become a servicing operation. Use [Review and Run](Review-and-Run) to inspect the resulting executable, arguments, dependencies, and risk flags.

## Sections

| Section | Current desktop surface |
| --- | --- |
| **Updates** | Queue CAB/MSU paths, language packs, Features on Demand, and enablement packages. The servicing plan applies them in the stored, reviewed order; it does not infer SSU/LCU applicability or dependencies. |
| **Drivers** | Add one INF, a driver directory, or request import of the current host's third-party drivers. |
| **Features** | Set known Windows feature identities to Enable, Disable, or Unchanged, and queue exact capability/FOD identities for Add, Remove, or Unchanged. |
| **Apps** | Queue provisioned Appx/MSIX package-name removals separately from signed `.appx`, `.appxbundle`, `.msix`, and `.msixbundle` provisioning files. |
| **Components** | Queue low-level component-package removals or typed offline scheduled-task Enable, Disable, and guarded Delete actions. |
| **Settings** | Toggle the built-in registry/policy recipes listed below. |
| **Unattended** | Add an existing answer-file path or open Unattended Studio to build one. |
| **Post-setup** | Queue reviewed files, installers/scripts, REG files, or `$OEM$` content for later staging. |

Text-list sections support explicit add and remove actions. Passive navigation does not change the project.

## Typed feature, capability, app, and task changes

Optional features are tri-state. **Unchanged** removes the project override; it is not another spelling of Disable. Capability/FOD Add and Remove lists are also mutually exclusive, and clearing an identity restores Unchanged. Every successful mutation is saved through `ProjectConfig` and committed to the project's local Git repository.

The Apps surface keeps package-name removal separate from file-based provisioning. The file picker accepts existing `.appx`, `.appxbundle`, `.msix`, and `.msixbundle` files. WimForge validates the path and extension when queueing; DISM performs the package-signature and image-applicability checks during servicing. Queue signed framework dependencies before the main bundle, then verify the exact order in **Review & run**. WimForge does not provide a Store browser or dependency resolver.

Scheduled tasks use paths relative to `Windows\System32\Tasks`. Enable and Disable atomically edit the offline task XML. Delete removes the task definition, so the desktop and controller both require an explicit compatibility override; the servicing plan also requires a checkpoint. The editor does not claim to inventory every task or provide build-specific compatibility advice.

## 類型化功能、能力、App 同排程工作變更

選用功能係三態：**啟用**、**停用**同**不變**。揀「不變」係清除工程覆寫，唔係另一種停用。Capability/FOD 嘅加入同移除亦唔可以同時存在；清除 identity 就會回復不變。每次成功修改都會經 `ProjectConfig` 儲存，再 commit 入工程自己嘅本機 Git repository。

Apps 畫面會分開「按套件名移除」同「按檔案預載」。File picker 只收現有 `.appx`、`.appxbundle`、`.msix` 同 `.msixbundle`；WimForge 排隊時會驗路徑同副檔名，而套件簽署同映像適用性就由 DISM 喺維護期間驗。已簽署 framework 依賴要排喺主 bundle 前面，之後去 **Review & run** 對清楚次序。WimForge 暫時冇 Store browser，亦冇依賴 resolver。

排程工作路徑係相對於 `Windows\System32\Tasks`。啟用同停用會原子修改離線工作 XML；刪除會移走工作定義，所以畫面同 controller 都一定要你明確確認相容性解鎖，servicing plan 亦會要求檢查點。呢個 editor 唔會扮識晒每個 Windows build 嘅工作清單同相容性。

## Built-in settings

The current desktop exposes these named setting recipes:

- reduce diagnostics and advertising telemetry;
- allow a local account during OOBE;
- show known file extensions;
- use the classic context menu;
- disable consumer application suggestions;
- enable Win32 long paths;
- prefer performance-oriented visual effects; and
- disable Recall by policy.

These switches are declarative recipe inputs, not a guarantee that every Windows edition/build implements the same registry or policy behavior. Inspect the generated plan and validate the installed result.

## Feature and component caution

Feature identities are Windows component names, not friendly compatibility advice. SMB 1.0 is explicitly labeled legacy/risky. Component removal is a low-level identifier workflow; WimForge does not ship a mature component-dependency or compatibility database comparable to long-established commercial tooling.

Before enabling, disabling, or removing an item:

1. Confirm the identity exists in the selected image/build.
2. Review dependencies and servicing output.
3. Keep the destructive-operation checkpoint enabled.
4. Boot-test the result and the recovery environment in a disposable VM.

## Payload responsibility

WimForge does not acquire general driver, update, or application payloads for this page. The operator is responsible for source, architecture, applicability, licensing, redistribution, integrity, and signer review. Package Studio adds provider-aware first-logon profiles and trust metadata, but it also does not bypass vendor authentication, subscriptions, hardware requirements, or terms. See [Package Studio](Package-Studio).

## Answer files and post-setup work

An answer-file path here becomes servicing input. Use [Unattended Studio](Unattended-Studio) for profile editing and XML export, then validate the XML in Windows SIM against the exact image/catalog.

Post-setup work crosses from offline image construction into code that runs during setup or first logon. Review every executable and argument, keep secrets out of project state, and test both connected and offline network conditions. For the structured, resumable software path, prefer [Package Studio](Package-Studio). For typed WinForge-family actions, use [WinForge Bridge](WinForge-Bridge).

## Undo and review

`Ctrl+Z` from this page targets the `config` history context. Selective undo is guarded: unrelated later edits can survive, while a later change to the same target path produces a conflict rather than being silently overwritten.

After customization:

1. Open **Review & run**.
2. Rebuild the plan.
3. Resolve all validation errors.
4. Review destructive/admin/reboot markers and exact commands.
5. Export a review script or complete `.wimforge` save before execution.

## 香港粵語重點

Updates 同 Drivers 唔再係空白清單：可以用 picker 加 CAB/MSU、INF 或驅動資料夾，清單會顯示 KB、大小、provider、class 同 driver version。Microsoft Update Catalog 連結只幫你搜尋；WimForge 唔會估 SSU/LCU 依賴，亦唔會估某個 update 啱唔啱目標 build。Features 而家係真正三態，capability 可以加入／移除／回復不變；Apps 分開套件名移除同已簽署 bundle file picker。Scheduled Tasks 用 typed `enable` / `disable` / `remove` 變更，會做離線 XML 安全檢查；`remove` 一定要明確 compatibility override 同 checkpoint，而且仍然冇內置 task inventory 或 build-specific 建議。所有項目都要去 Review & Run 對指令同 destructive marker 先執行。

---

[← Projects and Sources](Projects-and-Sources) · [Image Servicing →](Image-Servicing)
