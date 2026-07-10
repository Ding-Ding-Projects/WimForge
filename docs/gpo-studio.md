# GPO Studio catalog

GPO Studio uses `GpoCatalog` as its read-only source of truth for Administrative Template policy definitions. The catalog reads the ADMX schemas installed on the computer and joins them with one or more ADML language packs. It does not hard-code a small policy list, silently apply registry changes, or mutate the Windows policy store while browsing.

The default source is `%WINDIR%\PolicyDefinitions`. A caller can point `loadFromDirectory()` at a copied policy store or a domain Central Store, which also makes fixture testing and offline-image workflows deterministic.

## Import pipeline

Loading is transactional: a malformed ADMX or ADML document returns `false` with the file, line, column, and XML error, while the previously loaded catalog remains available.

1. Enumerate every top-level `.admx` file, case-insensitively.
2. Parse each target namespace and its `using` prefix table.
3. Collect categories, supported-on definitions, policies, registry actions, and configurable elements.
4. Select the requested ADML locale folders. With no explicit list, the system locale is preferred and `en-US` is added as a fallback when installed.
5. Join each ADML file to the same-named ADMX file and load its string and presentation tables.
6. Resolve local and cross-file references by full policy namespace, including `$(string.*)`, `$(presentation.*)`, parent categories, and supported-on definitions.
7. Build root-to-leaf category paths, localized policy records, and schema-driven presentation metadata.

Missing language files or individual resource strings are non-fatal. They appear in `warnings()` and the stable schema identifier remains available as a fallback. Category cycles and missing external categories are also reported instead of causing an infinite traversal.

## Catalog coverage

Each `GpoPolicy` preserves:

- source ADMX filename, namespace, schema ID, and qualified ID;
- `User`, `Machine`, or `Both` class;
- display-name and explain-text references plus every loaded translation;
- category ID and the complete root-to-leaf category hierarchy in every loaded locale;
- supported-on ID and localized supported-on description;
- policy registry key and value name;
- direct enabled and disabled values;
- all entries in `enabledList` and `disabledList`, including inherited keys and delete actions;
- all configurable ADMX elements and the complete localized ADML presentation table.

Registry values retain their original textual representation and a kind of Decimal, String, Delete, or None. Keeping decimal data as text avoids narrowing unsigned DWORD values such as `4294967295`. Enum options and Boolean true/false states can also contain additional registry lists; those lists are retained in the model and Markdown export.

The catalog also exposes standalone category and supported-on collections. Their qualified IDs prevent collisions between Microsoft, third-party, and project-specific namespaces.

## Schema-driven Material controls

The UI calls `GpoElement::materialControl()` instead of guessing from a localized label.

| ADMX element | Material/Qt Quick control | Preserved behavior |
|---|---|---|
| `boolean` | `Switch` | required flag, true/false value, true/false registry lists, ADML checked default |
| `enum` | `ComboBox` | localized option labels, decimal/string/delete values, option registry lists, default item |
| `decimal` | `SpinBox` | minimum, maximum, required, store-as-text, default value, spin step |
| `text` | `TextField` | minimum/maximum length, required, expandable, nested label/default value |
| `multiText` | `TextArea` | required flag, maximum entries/length, and multiline presentation label |
| `list` | `ListEditor` | target key, value prefix (including an explicitly empty prefix), additive and explicit-value modes |

Every element receives its localized presentation label and presentation default. `GpoPolicy::presentationElements` additionally retains static explanatory text and the original presentation order. Standard attributes such as `defaultItem`, `defaultChecked`, `defaultValue`, and `spinStep` have typed fields; all attributes are also retained in a map so vendor extensions are not thrown away.

This model is intended to drive a non-blocking editor surface: changing the selected policy swaps controls inside the page. Validation can be shown inline or in the application snackbar/notification center rather than opening a modal system dialog.

## Search

`search()` searches one combined record containing identifiers, every loaded translation, explanation text, category paths, support text, registry destinations and actions, element constraints, enum choices, Material control names, and presentation labels.

Plain mode is case-insensitive AND-token search. For example, `windows telemetry machine` returns only policies whose combined record contains all three tokens, in any fields.

Regular-expression mode uses `QRegularExpression` with case-insensitive Unicode properties and non-capturing matches. Patterns are compiled before catalog traversal. Invalid syntax returns an empty result and an error such as `Invalid regular expression at offset 2: ...`; it is never treated as a valid query with zero matches. NUL characters are rejected, pattern input is bounded to 2,048 UTF-16 code units, and WimForge prepends PCRE2 match/depth limits to bound backtracking work. User-supplied `(*LIMIT_...)` controls are rejected so a query cannot raise those limits.

The regex-builder wizard and an optional OpenCode intent helper should produce a query and pass it through this same validated search API. AI output must not bypass parsing, regex validation, or the user's final selection.

## Bilingual documentation export

`toMarkdown(primaryLocale, secondaryLocale)` generates documentation for every loaded policy. `exportMarkdown()` writes the same content atomically as UTF-8. Supplying both `en-US` and `zh-HK`, for example, places both languages beside each other for names, category paths, supported-on text, explanations, presentation labels, and enum options.

The generated document includes the schema identity, class, source, registry behavior, direct and list-based state actions, every element constraint, dynamic Material control, enum values, Boolean values, and the full ADML presentation table. If a requested secondary translation is not installed, the field is visibly empty rather than being mislabeled as translated text.

Example:

```cpp
using namespace wimforge;

GpoCatalog catalog;
QString error;
if (!catalog.loadInstalled({QStringLiteral("en-US"), QStringLiteral("zh-HK")}, &error)) {
    // Route error to the in-app notification center.
}

const QList<GpoPolicy> matches = catalog.search(
    QStringLiteral("bitlocker.*startup"),
    GpoSearchMode::RegularExpression,
    &error);

catalog.exportMarkdown(QStringLiteral("gpo-reference.md"),
                       QStringLiteral("en-US"),
                       QStringLiteral("zh-HK"),
                       &error);
```

## Applying a policy to an image

The catalog describes intent and registry operations; it is not the transaction engine. A GPO Studio editor should translate the selected policy state and element values into a project operation, validate all required fields and constraints, preview the exact offline registry changes, and then hand that operation to the project's crash-safe history/servicing layer. This separation is important: browsing, searching, or exporting documentation can never alter the mounted image.

When policy state is enabled or disabled, the transaction layer must process both the direct value and every corresponding registry-list assignment. Delete is an explicit action, not an empty string. Element values use their element-level key when present and otherwise the policy key. `Not configured` should remove only the values owned by the selected policy transaction, using the project's recorded before-state for a reversible result.

## Verification

`tests/gpo_catalog_tests.cpp` creates two ADMX files and English/Cantonese ADML resources inside a temporary directory. It verifies cross-namespace category and support resolution, all policy classes, every element-to-control mapping, constraints, direct and list registry actions, enum option lists, presentation defaults/static text, bilingual resource resolution, plain and regex search, readable invalid-regex errors, and bilingual Markdown export. The fixtures never depend on or modify the host's real Group Policy store.
