#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace wimforge {

enum class GpoPolicyClass
{
    User,
    Machine,
    Both,
};

enum class GpoValueKind
{
    None,
    Decimal,
    String,
    Delete,
};

struct GpoRegistryValue
{
    GpoValueKind kind = GpoValueKind::None;
    // ADMX values are deliberately kept as text. This preserves the complete
    // unsigned DWORD range and vendor-specific values without narrowing them.
    QString value;

    [[nodiscard]] bool isSet() const { return kind != GpoValueKind::None; }
    [[nodiscard]] QString toDisplayString() const;
};

struct GpoRegistryAssignment
{
    QString key;
    QString valueName;
    GpoRegistryValue value;
};

enum class GpoElementKind
{
    Boolean,
    Enum,
    Decimal,
    Text,
    MultiText,
    List,
};

enum class GpoPresentationKind
{
    CheckBox,
    DropDownList,
    DecimalTextBox,
    TextBox,
    MultiTextBox,
    ListBox,
    Text,
};

struct GpoEnumOption
{
    QString displayNameReference;
    QString displayName;
    QMap<QString, QString> localizedDisplayNames;
    GpoRegistryValue value;
    QList<GpoRegistryAssignment> assignments;
};

struct GpoPresentationElement
{
    GpoPresentationKind kind = GpoPresentationKind::Text;
    QString refId;
    QString label;
    QMap<QString, QString> localizedLabels;
    QString defaultValue;
    bool hasDefaultChecked = false;
    bool defaultChecked = false;
    std::optional<qint64> spinStep;
    // Retains extension attributes from third-party ADML files.
    QMap<QString, QString> attributes;
};

struct GpoElement
{
    GpoElementKind kind = GpoElementKind::Text;
    QString id;
    QString registryKey;
    QString registryValueName;

    bool required = false;
    bool expandable = false;
    bool storeAsText = false;
    bool soft = false;
    bool additive = false;
    bool explicitValue = false;
    QString valuePrefix;

    std::optional<qint64> minimumValue;
    std::optional<qint64> maximumValue;
    std::optional<int> minimumLength;
    std::optional<int> maximumLength;
    std::optional<int> maximumStrings;

    // Complete source attributes, including clientExtension and vendor data.
    QMap<QString, QString> attributes;

    GpoRegistryValue trueValue;
    GpoRegistryValue falseValue;
    QList<GpoRegistryAssignment> trueAssignments;
    QList<GpoRegistryAssignment> falseAssignments;
    QList<GpoEnumOption> options;

    QString presentationLabel;
    QMap<QString, QString> localizedPresentationLabels;
    QString presentationDefaultValue;
    bool hasPresentationDefaultChecked = false;
    bool presentationDefaultChecked = false;
    std::optional<qint64> presentationSpinStep;

    // Name of the Material/Qt Quick control selected by the schema-driven UI.
    [[nodiscard]] QString materialControl() const;
};

struct GpoCategory
{
    QString policyNamespace;
    QString id;
    QString parentId;
    QString displayNameReference;
    QString displayName;
    QMap<QString, QString> localizedDisplayNames;

    [[nodiscard]] QString qualifiedId() const;
};

struct GpoSupportedDefinition
{
    QString policyNamespace;
    QString id;
    QString displayNameReference;
    QString displayName;
    QMap<QString, QString> localizedDisplayNames;

    [[nodiscard]] QString qualifiedId() const;
};

struct GpoPolicy
{
    QString sourceFile;
    QString policyNamespace;
    QString id;
    GpoPolicyClass policyClass = GpoPolicyClass::Both;

    QString displayNameReference;
    QString displayName;
    QMap<QString, QString> localizedDisplayNames;
    QString explainTextReference;
    QString explainText;
    QMap<QString, QString> localizedExplainTexts;

    QString categoryId;
    QStringList categoryHierarchy;
    QMap<QString, QStringList> localizedCategoryHierarchies;
    QString supportedOnId;
    QString supportedOn;
    QMap<QString, QString> localizedSupportedOn;

    QString registryKey;
    QString registryValueName;
    GpoRegistryValue enabledValue;
    GpoRegistryValue disabledValue;
    QList<GpoRegistryAssignment> enabledAssignments;
    QList<GpoRegistryAssignment> disabledAssignments;

    QList<GpoElement> elements;
    QList<GpoPresentationElement> presentationElements;

    [[nodiscard]] QString qualifiedId() const;
};

enum class GpoSearchMode
{
    PlainText,
    RegularExpression,
};

class GpoCatalog
{
public:
    // Equivalent to %WINDIR%\PolicyDefinitions. An empty result means neither
    // WINDIR nor SystemRoot is available in the process environment.
    [[nodiscard]] static QString defaultPolicyDefinitionsPath();

    bool loadInstalled(const QStringList &locales = {}, QString *error = nullptr);
    bool loadFromDirectory(const QString &policyDefinitionsPath,
                           const QStringList &locales = {},
                           QString *error = nullptr);

    [[nodiscard]] const QList<GpoPolicy> &policies() const { return m_policies; }
    [[nodiscard]] const QList<GpoCategory> &categories() const { return m_categories; }
    [[nodiscard]] const QList<GpoSupportedDefinition> &supportedDefinitions() const
    {
        return m_supportedDefinitions;
    }
    [[nodiscard]] const QStringList &locales() const { return m_locales; }
    [[nodiscard]] const QStringList &warnings() const { return m_warnings; }
    [[nodiscard]] QString policyDefinitionsPath() const { return m_policyDefinitionsPath; }

    // Plain search is case-insensitive AND-token matching. Regex search uses a
    // bounded, validated QRegularExpression and reports syntax errors through
    // error instead of silently returning no matches.
    [[nodiscard]] QList<GpoPolicy> search(
        const QString &query,
        GpoSearchMode mode = GpoSearchMode::PlainText,
        QString *error = nullptr) const;

    [[nodiscard]] QString toMarkdown(const QString &primaryLocale = {},
                                     const QString &secondaryLocale = {}) const;
    bool exportMarkdown(const QString &filePath,
                        const QString &primaryLocale = {},
                        const QString &secondaryLocale = {},
                        QString *error = nullptr) const;

private:
    QString m_policyDefinitionsPath;
    QStringList m_locales;
    QStringList m_warnings;
    QList<GpoCategory> m_categories;
    QList<GpoSupportedDefinition> m_supportedDefinitions;
    QList<GpoPolicy> m_policies;
};

[[nodiscard]] QString gpoPolicyClassName(GpoPolicyClass value);
[[nodiscard]] QString gpoElementKindName(GpoElementKind value);
[[nodiscard]] QString gpoPresentationKindName(GpoPresentationKind value);

} // namespace wimforge
