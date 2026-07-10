# Group Policy Studio

Group Policy Studio turns the Administrative Template policy definitions actually installed on a machine—or copied into a selected PolicyDefinitions directory—into a searchable, documented, schema-driven editor. Browsing never mutates the host policy store.

## Catalog source

The default source is `%WINDIR%\PolicyDefinitions`. A copied policy store or domain Central Store can be supplied for deterministic/offline work.

Loading enumerates every top-level `.admx` file and joins each one to requested installed `.adml` language resources. It resolves:

- namespaces and cross-file `using` prefixes;
- categories and complete root-to-leaf paths;
- supported-on definitions;
- User, Machine, and Both policy classes;
- localized names, explanations, presentations, and enum labels;
- direct enabled/disabled registry values and registry lists;
- delete actions and inherited keys;
- Boolean, enum, decimal, text, multi-text, and list element constraints.

Malformed XML returns the file, line, column, and error while leaving the previous catalog available. Missing translations produce warnings and stable schema identifiers, not invented text.

## Search, regex, and intent help

Plain search is case-insensitive AND-token search over identifiers, every loaded translation, explanations, categories, support text, registry destinations, element constraints, choices, and presentation labels.

Regex mode uses validated, bounded `QRegularExpression`/PCRE2 behavior:

- maximum pattern input of 2,048 UTF-16 code units;
- invalid syntax is an error, not an empty successful search;
- NUL is rejected;
- backtracking/depth limits are prepended;
- user attempts to override those limits are rejected.

The regex wizard helps assemble a pattern and still passes it through the same validator. The OpenCode helper converts an intent into a proposed query; it cannot bypass parsing, regex validation, or final user selection.

## Schema-driven Material controls

| ADMX element | Editor |
| --- | --- |
| Boolean | `Switch` |
| Enum | `ComboBox` |
| Decimal | bounded `SpinBox` |
| Text | constrained `TextField` |
| Multi-text | `TextArea` |
| List | list editor |

The selected control is based on ADMX type, not a localized label. Defaults, required flags, numeric/length bounds, enum values, registry side effects, and ADML presentation order remain in the model.

## Applying a policy

The catalog is read-only. When the user applies a selected state, the controller translates that policy into project registry tweaks:

- **Enabled** applies the direct enabled value, enabled lists, and validated element values.
- **Disabled** applies the direct disabled value and disabled lists.
- **Not configured** emits deletes for the policy's direct value, element values, and enabled/disabled/enum/boolean assignment values declared by the loaded schema.
- Delete remains an explicit registry action; it is not confused with an empty string.

Element keys override the policy key where the schema declares them. Required fields and type constraints are validated before the project mutation is committed. Undo then operates on the recorded project before/after state.

Browsing, searching, changing pages, or exporting documentation cannot alter an image.

## Bilingual documentation

Studio export produces Markdown for every loaded policy. With `en-US` and `zh-HK`, it places both available languages beside each other for name, category, supported-on text, explanation, presentation labels, and enum options. Missing Cantonese resources remain visibly missing rather than being mislabeled as translated.

The export also includes schema identity, source ADMX, class, registry behavior, element constraints, dynamic control type, and full presentation structure.

## CLI examples

```powershell
.\WimForgeCli.exe gpo catalog --summary
.\WimForgeCli.exe gpo catalog --locale en-US --locale zh-HK --json
.\WimForgeCli.exe gpo search "Windows Update restart"
.\WimForgeCli.exe gpo search "restart.*deadline" --regex --json
.\WimForgeCli.exe gpo export policies.md --primary en-US --secondary zh-HK
.\WimForgeCli.exe gpo search demo --path D:\PolicyDefinitions --locale en-US
```

The number of policies is determined by the selected store. WimForge does not claim that one development machine's count is universal.

## Limitations

- ADMX definitions describe supported policy schema; they do not prove the setting is appropriate for the selected Windows edition/build.
- WimForge applies registry-backed project intent. It is not Active Directory, a domain GPO editor, Resultant Set of Policy, or a replacement for organizational change control.
- Third-party schemas are accepted when structurally valid, but the ISO author remains responsible for their source and target compatibility.
- Localization can only display ADML languages present in the selected store.

Implementation detail lives in [`docs/gpo-studio.md`](https://github.com/codingmachineedge/WimForge/blob/main/docs/gpo-studio.md).

---

[← Package Studio](Package-Studio) · [Unattended Studio →](Unattended-Studio)
