#include "GpoCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QXmlStreamReader>

#include <algorithm>
#include <limits>
#include <utility>

namespace wimforge {
namespace {

constexpr qsizetype MaximumRegexLength = 2048;
constexpr QChar KeySeparator(0x001f);
const QString RegexResourceLimits = QStringLiteral("(*LIMIT_MATCH=250000)(*LIMIT_DEPTH=500)");

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

QString attribute(const QXmlStreamReader &xml, const QString &name)
{
    return xml.attributes().value(name).toString();
}

bool booleanAttribute(const QXmlStreamReader &xml, const QString &name, bool fallback = false)
{
    if (!xml.attributes().hasAttribute(name))
        return fallback;
    const QString value = attribute(xml, name).trimmed().toCaseFolded();
    return value == QStringLiteral("true") || value == QStringLiteral("1")
        || value == QStringLiteral("yes");
}

std::optional<qint64> integerAttribute(const QXmlStreamReader &xml, const QString &name)
{
    if (!xml.attributes().hasAttribute(name))
        return std::nullopt;
    bool ok = false;
    const qint64 result = attribute(xml, name).trimmed().toLongLong(&ok, 10);
    return ok ? std::optional<qint64>(result) : std::nullopt;
}

std::optional<int> lengthAttribute(const QXmlStreamReader &xml, const QString &name)
{
    const std::optional<qint64> parsed = integerAttribute(xml, name);
    if (!parsed || *parsed < 0 || *parsed > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(*parsed);
}

QString canonicalKey(const QString &policyNamespace, const QString &id)
{
    return policyNamespace + KeySeparator + id;
}

QString publicId(const QString &canonical)
{
    const qsizetype separator = canonical.indexOf(KeySeparator);
    if (separator < 0)
        return canonical;
    const QString policyNamespace = canonical.left(separator);
    const QString id = canonical.mid(separator + 1);
    return policyNamespace.isEmpty() ? id : policyNamespace + QLatin1Char(':') + id;
}

QString resourceId(const QString &reference, const QString &table)
{
    const QString value = reference.trimmed();
    const QString prefix = QStringLiteral("$(%1.").arg(table);
    if (!value.startsWith(prefix) || !value.endsWith(QLatin1Char(')')))
        return {};
    return value.mid(prefix.size(), value.size() - prefix.size() - 1);
}

QString referencedKey(const QString &reference,
                      const QString &targetNamespace,
                      const QHash<QString, QString> &prefixes)
{
    const qsizetype separator = reference.indexOf(QLatin1Char(':'));
    if (separator < 0)
        return canonicalKey(targetNamespace, reference);

    const QString prefix = reference.left(separator);
    const QString id = reference.mid(separator + 1);
    return canonicalKey(prefixes.value(prefix, prefix), id);
}

QString resourceKey(const QString &reference,
                    const QString &table,
                    const QString &targetNamespace,
                    const QHash<QString, QString> &prefixes)
{
    const QString id = resourceId(reference, table);
    return id.isEmpty() ? QString() : referencedKey(id, targetNamespace, prefixes);
}

QString xmlError(const QString &path, const QXmlStreamReader &xml)
{
    return QStringLiteral("Invalid XML in %1 at line %2, column %3: %4")
        .arg(path)
        .arg(xml.lineNumber())
        .arg(xml.columnNumber())
        .arg(xml.errorString());
}

QString decodeUtf16Xml(const QByteArray &data, bool littleEndian)
{
    const qsizetype firstByte = 2; // Skip the byte-order mark.
    const qsizetype characterCount = (data.size() - firstByte) / 2;
    QString result;
    result.resize(characterCount);
    for (qsizetype index = 0; index < characterCount; ++index) {
        const auto low = static_cast<unsigned char>(data.at(firstByte + index * 2));
        const auto high = static_cast<unsigned char>(data.at(firstByte + index * 2 + 1));
        const ushort codeUnit = littleEndian
            ? static_cast<ushort>(low | (high << 8))
            : static_cast<ushort>((low << 8) | high);
        result[index] = QChar(codeUnit);
    }
    return result;
}

void addXmlData(QXmlStreamReader *xml, const QByteArray &data)
{
    if (data.size() >= 2
        && static_cast<unsigned char>(data.at(0)) == 0xff
        && static_cast<unsigned char>(data.at(1)) == 0xfe) {
        // Some third-party Administrative Templates declare the non-standard
        // encoding name "unicode". Supplying already-decoded characters lets
        // Qt honor the BOM without rejecting that legacy alias.
        xml->addData(decodeUtf16Xml(data, true));
    } else if (data.size() >= 2
               && static_cast<unsigned char>(data.at(0)) == 0xfe
               && static_cast<unsigned char>(data.at(1)) == 0xff) {
        xml->addData(decodeUtf16Xml(data, false));
    } else {
        xml->addData(data);
    }
}

struct ParsedElement
{
    GpoElement value;
    QStringList optionResourceKeys;
};

struct RawCategory
{
    GpoCategory value;
    QString key;
    QString parentKey;
    QString displayResourceKey;
};

struct RawSupportedDefinition
{
    GpoSupportedDefinition value;
    QString key;
    QString displayResourceKey;
};

struct RawPolicy
{
    GpoPolicy value;
    QHash<QString, QString> prefixes;
    QString displayResourceKey;
    QString explainResourceKey;
    QString categoryKey;
    QString supportedKey;
    QString presentationKey;
    QList<QStringList> enumOptionResourceKeys;
};

struct ParsedAdmx
{
    QString path;
    QString baseName;
    QString targetNamespace;
    QString targetPrefix;
    QHash<QString, QString> prefixes;
    QList<RawCategory> categories;
    QList<RawSupportedDefinition> supportedDefinitions;
    QList<RawPolicy> policies;
};

struct LocaleResources
{
    QHash<QString, QString> strings;
    QHash<QString, QList<GpoPresentationElement>> presentations;
};

GpoRegistryValue parseRegistryValue(QXmlStreamReader &xml)
{
    GpoRegistryValue result;
    while (xml.readNextStartElement()) {
        const QString name = xml.name().toString();
        if (name == QStringLiteral("decimal")) {
            if (result.kind == GpoValueKind::None) {
                result.kind = GpoValueKind::Decimal;
                result.value = attribute(xml, QStringLiteral("value"));
            }
            xml.skipCurrentElement();
        } else if (name == QStringLiteral("string")) {
            if (result.kind == GpoValueKind::None) {
                result.kind = GpoValueKind::String;
                if (xml.attributes().hasAttribute(QStringLiteral("value"))) {
                    result.value = attribute(xml, QStringLiteral("value"));
                    xml.skipCurrentElement();
                } else {
                    result.value = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                }
            } else {
                xml.skipCurrentElement();
            }
        } else if (name == QStringLiteral("delete")) {
            if (result.kind == GpoValueKind::None)
                result.kind = GpoValueKind::Delete;
            xml.skipCurrentElement();
        } else {
            xml.skipCurrentElement();
        }
    }
    return result;
}

QList<GpoRegistryAssignment> parseAssignmentList(QXmlStreamReader &xml,
                                                 const QString &defaultKey)
{
    QList<GpoRegistryAssignment> result;
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("item")) {
            xml.skipCurrentElement();
            continue;
        }

        GpoRegistryAssignment assignment;
        assignment.key = attribute(xml, QStringLiteral("key"));
        if (assignment.key.isEmpty())
            assignment.key = defaultKey;
        assignment.valueName = attribute(xml, QStringLiteral("valueName"));
        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("value"))
                assignment.value = parseRegistryValue(xml);
            else
                xml.skipCurrentElement();
        }
        result.append(std::move(assignment));
    }
    return result;
}

GpoEnumOption parseEnumOption(QXmlStreamReader &xml,
                              const QString &defaultKey,
                              QString *optionResourceKey,
                              const ParsedAdmx &document)
{
    GpoEnumOption option;
    option.displayNameReference = attribute(xml, QStringLiteral("displayName"));
    *optionResourceKey = resourceKey(option.displayNameReference,
                                     QStringLiteral("string"),
                                     document.targetNamespace,
                                     document.prefixes);
    while (xml.readNextStartElement()) {
        const QString name = xml.name().toString();
        if (name == QStringLiteral("value"))
            option.value = parseRegistryValue(xml);
        else if (name == QStringLiteral("valueList"))
            option.assignments = parseAssignmentList(xml, defaultKey);
        else
            xml.skipCurrentElement();
    }
    return option;
}

ParsedElement parseElement(QXmlStreamReader &xml,
                           const QString &policyKey,
                           const ParsedAdmx &document)
{
    ParsedElement parsed;
    const QString elementName = xml.name().toString();
    if (elementName == QStringLiteral("boolean"))
        parsed.value.kind = GpoElementKind::Boolean;
    else if (elementName == QStringLiteral("enum"))
        parsed.value.kind = GpoElementKind::Enum;
    else if (elementName == QStringLiteral("decimal"))
        parsed.value.kind = GpoElementKind::Decimal;
    else if (elementName == QStringLiteral("multiText"))
        parsed.value.kind = GpoElementKind::MultiText;
    else if (elementName == QStringLiteral("list"))
        parsed.value.kind = GpoElementKind::List;
    else
        parsed.value.kind = GpoElementKind::Text;

    parsed.value.id = attribute(xml, QStringLiteral("id"));
    for (const QXmlStreamAttribute &entry : xml.attributes())
        parsed.value.attributes.insert(entry.name().toString(), entry.value().toString());
    parsed.value.registryKey = attribute(xml, QStringLiteral("key"));
    if (parsed.value.registryKey.isEmpty())
        parsed.value.registryKey = policyKey;
    parsed.value.registryValueName = attribute(xml, QStringLiteral("valueName"));
    parsed.value.required = booleanAttribute(xml, QStringLiteral("required"));
    parsed.value.expandable = booleanAttribute(xml, QStringLiteral("expandable"));
    parsed.value.storeAsText = booleanAttribute(xml, QStringLiteral("storeAsText"));
    parsed.value.soft = booleanAttribute(xml, QStringLiteral("soft"));
    parsed.value.additive = booleanAttribute(xml, QStringLiteral("additive"));
    parsed.value.explicitValue = booleanAttribute(xml, QStringLiteral("explicitValue"));
    parsed.value.valuePrefix = attribute(xml, QStringLiteral("valuePrefix"));
    parsed.value.minimumValue = integerAttribute(xml, QStringLiteral("minValue"));
    parsed.value.maximumValue = integerAttribute(xml, QStringLiteral("maxValue"));
    parsed.value.minimumLength = lengthAttribute(xml, QStringLiteral("minLength"));
    parsed.value.maximumLength = lengthAttribute(xml, QStringLiteral("maxLength"));
    parsed.value.maximumStrings = lengthAttribute(xml, QStringLiteral("maxStrings"));

    while (xml.readNextStartElement()) {
        const QString childName = xml.name().toString();
        if (childName == QStringLiteral("trueValue")) {
            parsed.value.trueValue = parseRegistryValue(xml);
        } else if (childName == QStringLiteral("falseValue")) {
            parsed.value.falseValue = parseRegistryValue(xml);
        } else if (childName == QStringLiteral("trueList")) {
            parsed.value.trueAssignments = parseAssignmentList(xml, parsed.value.registryKey);
        } else if (childName == QStringLiteral("falseList")) {
            parsed.value.falseAssignments = parseAssignmentList(xml, parsed.value.registryKey);
        } else if (childName == QStringLiteral("item")
                   && parsed.value.kind == GpoElementKind::Enum) {
            QString optionKey;
            parsed.value.options.append(
                parseEnumOption(xml, parsed.value.registryKey, &optionKey, document));
            parsed.optionResourceKeys.append(optionKey);
        } else {
            xml.skipCurrentElement();
        }
    }
    return parsed;
}

void parsePolicyNamespaces(QXmlStreamReader &xml, ParsedAdmx *document)
{
    while (xml.readNextStartElement()) {
        const QString name = xml.name().toString();
        const QString prefix = attribute(xml, QStringLiteral("prefix"));
        const QString policyNamespace = attribute(xml, QStringLiteral("namespace"));
        if (name == QStringLiteral("target")) {
            document->targetPrefix = prefix;
            document->targetNamespace = policyNamespace;
            if (!prefix.isEmpty())
                document->prefixes.insert(prefix, policyNamespace);
        } else if (name == QStringLiteral("using") && !prefix.isEmpty()) {
            document->prefixes.insert(prefix, policyNamespace);
        }
        xml.skipCurrentElement();
    }
}

void parseSupportedDefinitions(QXmlStreamReader &xml, ParsedAdmx *document)
{
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("definitions")) {
            xml.skipCurrentElement();
            continue;
        }
        while (xml.readNextStartElement()) {
            if (xml.name() != QStringLiteral("definition")) {
                xml.skipCurrentElement();
                continue;
            }
            RawSupportedDefinition definition;
            definition.value.policyNamespace = document->targetNamespace;
            definition.value.id = attribute(xml, QStringLiteral("name"));
            definition.value.displayNameReference = attribute(xml, QStringLiteral("displayName"));
            definition.key = canonicalKey(document->targetNamespace, definition.value.id);
            definition.displayResourceKey = resourceKey(definition.value.displayNameReference,
                                                        QStringLiteral("string"),
                                                        document->targetNamespace,
                                                        document->prefixes);
            document->supportedDefinitions.append(std::move(definition));
            xml.skipCurrentElement();
        }
    }
}

void parseCategories(QXmlStreamReader &xml, ParsedAdmx *document)
{
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("category")) {
            xml.skipCurrentElement();
            continue;
        }

        RawCategory category;
        category.value.policyNamespace = document->targetNamespace;
        category.value.id = attribute(xml, QStringLiteral("name"));
        category.value.displayNameReference = attribute(xml, QStringLiteral("displayName"));
        category.key = canonicalKey(document->targetNamespace, category.value.id);
        category.displayResourceKey = resourceKey(category.value.displayNameReference,
                                                   QStringLiteral("string"),
                                                   document->targetNamespace,
                                                   document->prefixes);
        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("parentCategory")) {
                category.parentKey = referencedKey(attribute(xml, QStringLiteral("ref")),
                                                   document->targetNamespace,
                                                   document->prefixes);
                xml.skipCurrentElement();
            } else {
                xml.skipCurrentElement();
            }
        }
        category.value.parentId = publicId(category.parentKey);
        document->categories.append(std::move(category));
    }
}

GpoPolicyClass parsePolicyClass(const QString &value)
{
    if (value.compare(QStringLiteral("User"), Qt::CaseInsensitive) == 0)
        return GpoPolicyClass::User;
    if (value.compare(QStringLiteral("Machine"), Qt::CaseInsensitive) == 0)
        return GpoPolicyClass::Machine;
    return GpoPolicyClass::Both;
}

void parsePolicies(QXmlStreamReader &xml, ParsedAdmx *document)
{
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("policy")) {
            xml.skipCurrentElement();
            continue;
        }

        RawPolicy policy;
        policy.prefixes = document->prefixes;
        policy.value.sourceFile = document->path;
        policy.value.policyNamespace = document->targetNamespace;
        policy.value.id = attribute(xml, QStringLiteral("name"));
        policy.value.policyClass = parsePolicyClass(attribute(xml, QStringLiteral("class")));
        policy.value.displayNameReference = attribute(xml, QStringLiteral("displayName"));
        policy.value.explainTextReference = attribute(xml, QStringLiteral("explainText"));
        policy.value.registryKey = attribute(xml, QStringLiteral("key"));
        policy.value.registryValueName = attribute(xml, QStringLiteral("valueName"));
        policy.displayResourceKey = resourceKey(policy.value.displayNameReference,
                                                QStringLiteral("string"),
                                                document->targetNamespace,
                                                document->prefixes);
        policy.explainResourceKey = resourceKey(policy.value.explainTextReference,
                                                QStringLiteral("string"),
                                                document->targetNamespace,
                                                document->prefixes);
        const QString presentationReference = attribute(xml, QStringLiteral("presentation"));
        policy.presentationKey = resourceKey(presentationReference,
                                             QStringLiteral("presentation"),
                                             document->targetNamespace,
                                             document->prefixes);

        while (xml.readNextStartElement()) {
            const QString childName = xml.name().toString();
            if (childName == QStringLiteral("parentCategory")) {
                policy.categoryKey = referencedKey(attribute(xml, QStringLiteral("ref")),
                                                  document->targetNamespace,
                                                  document->prefixes);
                xml.skipCurrentElement();
            } else if (childName == QStringLiteral("supportedOn")) {
                policy.supportedKey = referencedKey(attribute(xml, QStringLiteral("ref")),
                                                   document->targetNamespace,
                                                   document->prefixes);
                xml.skipCurrentElement();
            } else if (childName == QStringLiteral("enabledValue")) {
                policy.value.enabledValue = parseRegistryValue(xml);
            } else if (childName == QStringLiteral("disabledValue")) {
                policy.value.disabledValue = parseRegistryValue(xml);
            } else if (childName == QStringLiteral("enabledList")) {
                policy.value.enabledAssignments = parseAssignmentList(xml, policy.value.registryKey);
            } else if (childName == QStringLiteral("disabledList")) {
                policy.value.disabledAssignments = parseAssignmentList(xml, policy.value.registryKey);
            } else if (childName == QStringLiteral("elements")) {
                while (xml.readNextStartElement()) {
                    const QString elementName = xml.name().toString();
                    if (elementName == QStringLiteral("boolean")
                        || elementName == QStringLiteral("enum")
                        || elementName == QStringLiteral("decimal")
                        || elementName == QStringLiteral("text")
                        || elementName == QStringLiteral("multiText")
                        || elementName == QStringLiteral("list")) {
                        ParsedElement element = parseElement(xml, policy.value.registryKey, *document);
                        policy.value.elements.append(std::move(element.value));
                        policy.enumOptionResourceKeys.append(std::move(element.optionResourceKeys));
                    } else {
                        xml.skipCurrentElement();
                    }
                }
            } else {
                xml.skipCurrentElement();
            }
        }

        policy.value.categoryId = publicId(policy.categoryKey);
        policy.value.supportedOnId = publicId(policy.supportedKey);
        document->policies.append(std::move(policy));
    }
}

bool parseAdmxFile(const QString &path, ParsedAdmx *document, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open ADMX file %1: %2").arg(path, file.errorString()));
        return false;
    }

    document->path = QFileInfo(path).absoluteFilePath();
    document->baseName = QFileInfo(path).completeBaseName().toCaseFolded();
    QXmlStreamReader xml;
    addXmlData(&xml, file.readAll());
    if (!xml.readNextStartElement()) {
        setError(error, xml.hasError() ? xmlError(path, xml)
                                      : QStringLiteral("%1 is empty.").arg(path));
        return false;
    }
    if (xml.name() != QStringLiteral("policyDefinitions")) {
        setError(error, QStringLiteral("%1 is not an ADMX policyDefinitions document.").arg(path));
        return false;
    }

    while (xml.readNextStartElement()) {
        const QString name = xml.name().toString();
        if (name == QStringLiteral("policyNamespaces"))
            parsePolicyNamespaces(xml, document);
        else if (name == QStringLiteral("supportedOn"))
            parseSupportedDefinitions(xml, document);
        else if (name == QStringLiteral("categories"))
            parseCategories(xml, document);
        else if (name == QStringLiteral("policies"))
            parsePolicies(xml, document);
        else
            xml.skipCurrentElement();
    }

    if (xml.hasError()) {
        setError(error, xmlError(path, xml));
        return false;
    }
    if (document->targetNamespace.isEmpty()) {
        setError(error, QStringLiteral("ADMX file %1 has no target policy namespace.").arg(path));
        return false;
    }
    setError(error, {});
    return true;
}

GpoPresentationKind presentationKind(const QString &name)
{
    if (name == QStringLiteral("checkBox"))
        return GpoPresentationKind::CheckBox;
    if (name == QStringLiteral("dropdownList"))
        return GpoPresentationKind::DropDownList;
    if (name == QStringLiteral("decimalTextBox"))
        return GpoPresentationKind::DecimalTextBox;
    if (name == QStringLiteral("textBox"))
        return GpoPresentationKind::TextBox;
    if (name == QStringLiteral("multiTextBox"))
        return GpoPresentationKind::MultiTextBox;
    if (name == QStringLiteral("listBox"))
        return GpoPresentationKind::ListBox;
    return GpoPresentationKind::Text;
}

GpoPresentationElement parsePresentationElement(QXmlStreamReader &xml)
{
    GpoPresentationElement element;
    const QString name = xml.name().toString();
    element.kind = presentationKind(name);
    element.refId = attribute(xml, QStringLiteral("refId"));
    for (const QXmlStreamAttribute &entry : xml.attributes())
        element.attributes.insert(entry.name().toString(), entry.value().toString());

    if (xml.attributes().hasAttribute(QStringLiteral("defaultChecked"))) {
        element.hasDefaultChecked = true;
        element.defaultChecked = booleanAttribute(xml, QStringLiteral("defaultChecked"));
    }
    if (xml.attributes().hasAttribute(QStringLiteral("defaultValue")))
        element.defaultValue = attribute(xml, QStringLiteral("defaultValue"));
    else if (xml.attributes().hasAttribute(QStringLiteral("defaultItem")))
        element.defaultValue = attribute(xml, QStringLiteral("defaultItem"));
    element.spinStep = integerAttribute(xml, QStringLiteral("spinStep"));

    if (name == QStringLiteral("textBox")) {
        while (xml.readNextStartElement()) {
            const QString childName = xml.name().toString();
            if (childName == QStringLiteral("label"))
                element.label = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            else if (childName == QStringLiteral("defaultValue"))
                element.defaultValue = xml.readElementText(QXmlStreamReader::IncludeChildElements);
            else
                xml.skipCurrentElement();
        }
    } else {
        element.label = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
    }
    return element;
}

void parsePresentationTable(QXmlStreamReader &xml,
                            const QString &policyNamespace,
                            LocaleResources *resources)
{
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("presentation")) {
            xml.skipCurrentElement();
            continue;
        }
        const QString id = attribute(xml, QStringLiteral("id"));
        QList<GpoPresentationElement> elements;
        while (xml.readNextStartElement())
            elements.append(parsePresentationElement(xml));
        resources->presentations.insert(canonicalKey(policyNamespace, id), std::move(elements));
    }
}

void parseStringTable(QXmlStreamReader &xml,
                      const QString &policyNamespace,
                      LocaleResources *resources)
{
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("string")) {
            xml.skipCurrentElement();
            continue;
        }
        const QString id = attribute(xml, QStringLiteral("id"));
        const QString value = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
        resources->strings.insert(canonicalKey(policyNamespace, id), value);
    }
}

void parseAdmlResources(QXmlStreamReader &xml,
                        const QString &policyNamespace,
                        LocaleResources *resources)
{
    while (xml.readNextStartElement()) {
        const QString name = xml.name().toString();
        if (name == QStringLiteral("stringTable"))
            parseStringTable(xml, policyNamespace, resources);
        else if (name == QStringLiteral("presentationTable"))
            parsePresentationTable(xml, policyNamespace, resources);
        else
            xml.skipCurrentElement();
    }
}

bool parseAdmlFile(const QString &path,
                   const QString &policyNamespace,
                   LocaleResources *resources,
                   QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open ADML file %1: %2").arg(path, file.errorString()));
        return false;
    }

    QXmlStreamReader xml;
    addXmlData(&xml, file.readAll());
    if (!xml.readNextStartElement()) {
        setError(error, xml.hasError() ? xmlError(path, xml)
                                      : QStringLiteral("%1 is empty.").arg(path));
        return false;
    }
    if (xml.name() != QStringLiteral("policyDefinitionResources")) {
        setError(error, QStringLiteral("%1 is not an ADML policyDefinitionResources document.")
                            .arg(path));
        return false;
    }
    while (xml.readNextStartElement()) {
        if (xml.name() == QStringLiteral("resources"))
            parseAdmlResources(xml, policyNamespace, resources);
        else
            xml.skipCurrentElement();
    }
    if (xml.hasError()) {
        setError(error, xmlError(path, xml));
        return false;
    }
    setError(error, {});
    return true;
}

QFileInfoList filesWithSuffix(const QString &directory, const QString &suffix)
{
    QFileInfoList result;
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries) {
        if (entry.suffix().compare(suffix, Qt::CaseInsensitive) == 0)
            result.append(entry);
    }
    return result;
}

struct LocaleDirectory
{
    QString name;
    QString path;
};

QList<LocaleDirectory> availableLocaleDirectories(const QString &root)
{
    QList<LocaleDirectory> result;
    const QFileInfoList directories = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &directory : directories) {
        if (!filesWithSuffix(directory.absoluteFilePath(), QStringLiteral("adml")).isEmpty())
            result.append(LocaleDirectory{directory.fileName(), directory.absoluteFilePath()});
    }
    return result;
}

const LocaleDirectory *findLocale(const QList<LocaleDirectory> &available,
                                  const QString &requested)
{
    for (const LocaleDirectory &locale : available) {
        if (locale.name.compare(requested, Qt::CaseInsensitive) == 0)
            return &locale;
    }
    return nullptr;
}

void appendLocale(QList<LocaleDirectory> *selected, const LocaleDirectory &candidate)
{
    const bool alreadyPresent = std::any_of(selected->cbegin(), selected->cend(),
                                            [&](const LocaleDirectory &existing) {
        return existing.name.compare(candidate.name, Qt::CaseInsensitive) == 0;
    });
    if (!alreadyPresent)
        selected->append(candidate);
}

QList<LocaleDirectory> selectLocales(const QString &root,
                                     const QStringList &requested,
                                     QStringList *warnings)
{
    const QList<LocaleDirectory> available = availableLocaleDirectories(root);
    QList<LocaleDirectory> selected;
    if (!requested.isEmpty()) {
        for (const QString &name : requested) {
            if (const LocaleDirectory *locale = findLocale(available, name.trimmed()))
                appendLocale(&selected, *locale);
            else
                warnings->append(QStringLiteral("Locale resource folder '%1' was not found in %2.")
                                     .arg(name, root));
        }
        return selected;
    }

    QString systemLocale = QLocale::system().name();
    systemLocale.replace(QLatin1Char('_'), QLatin1Char('-'));
    if (systemLocale.compare(QStringLiteral("C"), Qt::CaseInsensitive) != 0) {
        if (const LocaleDirectory *exact = findLocale(available, systemLocale)) {
            appendLocale(&selected, *exact);
        } else {
            const QString language = systemLocale.section(QLatin1Char('-'), 0, 0);
            for (const LocaleDirectory &candidate : available) {
                if (candidate.name.startsWith(language + QLatin1Char('-'), Qt::CaseInsensitive)) {
                    appendLocale(&selected, candidate);
                    break;
                }
            }
        }
    }

    if (const LocaleDirectory *english = findLocale(available, QStringLiteral("en-US")))
        appendLocale(&selected, *english);
    if (selected.isEmpty() && !available.isEmpty())
        selected.append(available.first());
    if (selected.isEmpty())
        warnings->append(QStringLiteral("No ADML locale folders were found in %1.").arg(root));
    return selected;
}

std::optional<QString> localizedResource(const QString &literalOrReference,
                                         const QString &key,
                                         const QString &locale,
                                         const QHash<QString, LocaleResources> &resources)
{
    if (literalOrReference.isEmpty())
        return QString();
    if (key.isEmpty())
        return literalOrReference;
    const auto localeIt = resources.constFind(locale);
    if (localeIt == resources.cend())
        return std::nullopt;
    const auto stringIt = localeIt->strings.constFind(key);
    return stringIt == localeIt->strings.cend()
        ? std::optional<QString>()
        : std::optional<QString>(*stringIt);
}

QString firstLocalized(const QMap<QString, QString> &values,
                       const QStringList &locales,
                       const QString &fallback)
{
    for (const QString &locale : locales) {
        const auto it = values.constFind(locale);
        if (it != values.cend() && !it->isEmpty())
            return *it;
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!it->isEmpty())
            return *it;
    }
    return fallback;
}

QMap<QString, QString> resolveLocalizedMap(const QString &literalOrReference,
                                           const QString &key,
                                           const QStringList &locales,
                                           const QHash<QString, LocaleResources> &resources,
                                           int *unresolvedCount)
{
    QMap<QString, QString> result;
    for (const QString &locale : locales) {
        const std::optional<QString> value = localizedResource(literalOrReference,
                                                              key,
                                                              locale,
                                                              resources);
        if (value)
            result.insert(locale, *value);
        else if (!literalOrReference.isEmpty())
            ++*unresolvedCount;
    }
    return result;
}

int matchingPresentationIndex(const QList<GpoPresentationElement> &elements,
                              const GpoPresentationElement &candidate,
                              qsizetype sourceIndex)
{
    if (!candidate.refId.isEmpty()) {
        for (qsizetype index = 0; index < elements.size(); ++index) {
            if (elements.at(index).refId == candidate.refId
                && elements.at(index).kind == candidate.kind) {
                return static_cast<int>(index);
            }
        }
    } else if (sourceIndex < elements.size()
               && elements.at(sourceIndex).refId.isEmpty()
               && elements.at(sourceIndex).kind == candidate.kind) {
        return static_cast<int>(sourceIndex);
    }
    return -1;
}

QList<GpoPresentationElement> localizedPresentation(
    const QString &presentationKey,
    const QStringList &locales,
    const QHash<QString, LocaleResources> &resources)
{
    QList<GpoPresentationElement> result;
    if (presentationKey.isEmpty())
        return result;

    for (const QString &locale : locales) {
        const auto resourceIt = resources.constFind(locale);
        if (resourceIt == resources.cend())
            continue;
        const auto presentationIt = resourceIt->presentations.constFind(presentationKey);
        if (presentationIt == resourceIt->presentations.cend())
            continue;

        const QList<GpoPresentationElement> &localized = *presentationIt;
        for (qsizetype sourceIndex = 0; sourceIndex < localized.size(); ++sourceIndex) {
            const GpoPresentationElement &source = localized.at(sourceIndex);
            int destinationIndex = matchingPresentationIndex(result, source, sourceIndex);
            if (destinationIndex < 0) {
                GpoPresentationElement copy = source;
                copy.localizedLabels.clear();
                copy.label.clear();
                result.append(std::move(copy));
                destinationIndex = static_cast<int>(result.size() - 1);
            }
            GpoPresentationElement &destination = result[destinationIndex];
            destination.localizedLabels.insert(locale, source.label);
            if (destination.defaultValue.isEmpty())
                destination.defaultValue = source.defaultValue;
            if (!destination.hasDefaultChecked && source.hasDefaultChecked) {
                destination.hasDefaultChecked = true;
                destination.defaultChecked = source.defaultChecked;
            }
            if (!destination.spinStep && source.spinStep)
                destination.spinStep = source.spinStep;
            if (destination.attributes.isEmpty())
                destination.attributes = source.attributes;
        }
    }

    for (GpoPresentationElement &element : result)
        element.label = firstLocalized(element.localizedLabels, locales, {});
    return result;
}

struct BuildResult
{
    QString path;
    QStringList locales;
    QStringList warnings;
    QList<GpoCategory> categories;
    QList<GpoSupportedDefinition> supportedDefinitions;
    QList<GpoPolicy> policies;
};

bool buildCatalog(const QString &requestedPath,
                  const QStringList &requestedLocales,
                  BuildResult *result,
                  QString *error)
{
    const QString root = QDir(requestedPath).absolutePath();
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        setError(error, QStringLiteral("PolicyDefinitions folder does not exist: %1").arg(root));
        return false;
    }

    const QFileInfoList admxFiles = filesWithSuffix(root, QStringLiteral("admx"));
    if (admxFiles.isEmpty()) {
        setError(error, QStringLiteral("No ADMX files were found in %1.").arg(root));
        return false;
    }

    QList<ParsedAdmx> documents;
    QHash<QString, int> documentByBaseName;
    for (const QFileInfo &file : admxFiles) {
        ParsedAdmx document;
        if (!parseAdmxFile(file.absoluteFilePath(), &document, error))
            return false;
        documentByBaseName.insert(document.baseName, documents.size());
        documents.append(std::move(document));
    }

    result->path = root;
    const QList<LocaleDirectory> selectedLocales = selectLocales(root,
                                                                 requestedLocales,
                                                                 &result->warnings);
    QHash<QString, LocaleResources> resources;
    for (const LocaleDirectory &locale : selectedLocales) {
        result->locales.append(locale.name);
        LocaleResources localeResources;
        int matchedFiles = 0;
        for (const QFileInfo &file : filesWithSuffix(locale.path, QStringLiteral("adml"))) {
            const QString baseName = file.completeBaseName().toCaseFolded();
            const auto documentIt = documentByBaseName.constFind(baseName);
            if (documentIt == documentByBaseName.cend())
                continue;
            ++matchedFiles;
            const ParsedAdmx &document = documents.at(*documentIt);
            if (!parseAdmlFile(file.absoluteFilePath(),
                               document.targetNamespace,
                               &localeResources,
                               error)) {
                return false;
            }
        }
        if (matchedFiles < documents.size()) {
            result->warnings.append(
                QStringLiteral("Locale %1 supplied ADML resources for %2 of %3 ADMX files; "
                               "untranslated entries retain their schema identifiers.")
                    .arg(locale.name)
                    .arg(matchedFiles)
                    .arg(documents.size()));
        }
        resources.insert(locale.name, std::move(localeResources));
    }

    QList<RawCategory> rawCategories;
    QList<RawSupportedDefinition> rawSupportedDefinitions;
    QList<RawPolicy> rawPolicies;
    for (ParsedAdmx &document : documents) {
        rawCategories.append(std::move(document.categories));
        rawSupportedDefinitions.append(std::move(document.supportedDefinitions));
        rawPolicies.append(std::move(document.policies));
    }

    int unresolvedResources = 0;
    QHash<QString, int> categoryByKey;
    QHash<QString, QString> categoryParentByKey;
    for (const RawCategory &raw : rawCategories) {
        GpoCategory category = raw.value;
        category.localizedDisplayNames = resolveLocalizedMap(category.displayNameReference,
                                                             raw.displayResourceKey,
                                                             result->locales,
                                                             resources,
                                                             &unresolvedResources);
        category.displayName = firstLocalized(category.localizedDisplayNames,
                                              result->locales,
                                              category.id);
        category.parentId = publicId(raw.parentKey);
        categoryByKey.insert(raw.key, result->categories.size());
        categoryParentByKey.insert(raw.key, raw.parentKey);
        result->categories.append(std::move(category));
    }

    QHash<QString, int> supportedByKey;
    for (const RawSupportedDefinition &raw : rawSupportedDefinitions) {
        GpoSupportedDefinition definition = raw.value;
        definition.localizedDisplayNames = resolveLocalizedMap(definition.displayNameReference,
                                                               raw.displayResourceKey,
                                                               result->locales,
                                                               resources,
                                                               &unresolvedResources);
        definition.displayName = firstLocalized(definition.localizedDisplayNames,
                                                result->locales,
                                                definition.id);
        supportedByKey.insert(raw.key, result->supportedDefinitions.size());
        result->supportedDefinitions.append(std::move(definition));
    }

    QSet<QString> missingCategoryReferences;
    QSet<QString> cyclicCategories;
    for (RawPolicy &raw : rawPolicies) {
        GpoPolicy policy = std::move(raw.value);
        policy.localizedDisplayNames = resolveLocalizedMap(policy.displayNameReference,
                                                           raw.displayResourceKey,
                                                           result->locales,
                                                           resources,
                                                           &unresolvedResources);
        policy.displayName = firstLocalized(policy.localizedDisplayNames,
                                            result->locales,
                                            policy.id);
        policy.localizedExplainTexts = resolveLocalizedMap(policy.explainTextReference,
                                                           raw.explainResourceKey,
                                                           result->locales,
                                                           resources,
                                                           &unresolvedResources);
        policy.explainText = firstLocalized(policy.localizedExplainTexts,
                                            result->locales,
                                            policy.explainTextReference);

        for (const QString &locale : result->locales) {
            QStringList reversedPath;
            QString current = raw.categoryKey;
            QSet<QString> visited;
            while (!current.isEmpty()) {
                if (visited.contains(current)) {
                    cyclicCategories.insert(publicId(current));
                    break;
                }
                visited.insert(current);
                const auto categoryIt = categoryByKey.constFind(current);
                if (categoryIt == categoryByKey.cend()) {
                    missingCategoryReferences.insert(publicId(current));
                    reversedPath.append(publicId(current));
                    break;
                }
                const GpoCategory &category = result->categories.at(*categoryIt);
                const auto labelIt = category.localizedDisplayNames.constFind(locale);
                reversedPath.append(labelIt == category.localizedDisplayNames.cend()
                                        ? category.displayName
                                        : *labelIt);
                current = categoryParentByKey.value(current);
            }
            std::reverse(reversedPath.begin(), reversedPath.end());
            policy.localizedCategoryHierarchies.insert(locale, reversedPath);
        }
        if (!result->locales.isEmpty())
            policy.categoryHierarchy = policy.localizedCategoryHierarchies
                                           .value(result->locales.first());
        else {
            QStringList reversedPath;
            QString current = raw.categoryKey;
            QSet<QString> visited;
            while (!current.isEmpty()) {
                if (visited.contains(current)) {
                    cyclicCategories.insert(publicId(current));
                    break;
                }
                visited.insert(current);
                const auto categoryIt = categoryByKey.constFind(current);
                if (categoryIt == categoryByKey.cend()) {
                    missingCategoryReferences.insert(publicId(current));
                    reversedPath.append(publicId(current));
                    break;
                }
                reversedPath.append(result->categories.at(*categoryIt).displayName);
                current = categoryParentByKey.value(current);
            }
            std::reverse(reversedPath.begin(), reversedPath.end());
            policy.categoryHierarchy = reversedPath;
        }

        const auto supportedIt = supportedByKey.constFind(raw.supportedKey);
        if (supportedIt != supportedByKey.cend()) {
            const GpoSupportedDefinition &definition = result->supportedDefinitions.at(*supportedIt);
            policy.localizedSupportedOn = definition.localizedDisplayNames;
            policy.supportedOn = definition.displayName;
        } else if (!raw.supportedKey.isEmpty()) {
            policy.supportedOn = publicId(raw.supportedKey);
        }

        for (qsizetype elementIndex = 0; elementIndex < policy.elements.size(); ++elementIndex) {
            GpoElement &element = policy.elements[elementIndex];
            const QStringList optionKeys = elementIndex < raw.enumOptionResourceKeys.size()
                ? raw.enumOptionResourceKeys.at(elementIndex)
                : QStringList();
            for (qsizetype optionIndex = 0; optionIndex < element.options.size(); ++optionIndex) {
                GpoEnumOption &option = element.options[optionIndex];
                const QString key = optionIndex < optionKeys.size()
                    ? optionKeys.at(optionIndex)
                    : QString();
                option.localizedDisplayNames = resolveLocalizedMap(option.displayNameReference,
                                                                   key,
                                                                   result->locales,
                                                                   resources,
                                                                   &unresolvedResources);
                option.displayName = firstLocalized(option.localizedDisplayNames,
                                                    result->locales,
                                                    option.displayNameReference);
            }
        }

        policy.presentationElements = localizedPresentation(raw.presentationKey,
                                                            result->locales,
                                                            resources);
        for (GpoElement &element : policy.elements) {
            const auto presentationIt = std::find_if(
                policy.presentationElements.cbegin(),
                policy.presentationElements.cend(),
                [&](const GpoPresentationElement &presentation) {
                    return presentation.refId == element.id;
                });
            if (presentationIt == policy.presentationElements.cend())
                continue;
            element.presentationLabel = presentationIt->label;
            element.localizedPresentationLabels = presentationIt->localizedLabels;
            element.presentationDefaultValue = presentationIt->defaultValue;
            element.hasPresentationDefaultChecked = presentationIt->hasDefaultChecked;
            element.presentationDefaultChecked = presentationIt->defaultChecked;
            element.presentationSpinStep = presentationIt->spinStep;
        }

        result->policies.append(std::move(policy));
    }

    if (unresolvedResources > 0) {
        result->warnings.append(
            QStringLiteral("%1 localized string reference(s) were unresolved; schema identifiers "
                           "were retained as fallbacks.")
                .arg(unresolvedResources));
    }
    if (!missingCategoryReferences.isEmpty()) {
        QStringList missing;
        for (const QString &entry : missingCategoryReferences)
            missing.append(entry);
        missing.sort(Qt::CaseInsensitive);
        result->warnings.append(QStringLiteral("Unresolved category reference(s): %1")
                                    .arg(missing.join(QStringLiteral(", "))));
    }
    if (!cyclicCategories.isEmpty()) {
        QStringList cyclic;
        for (const QString &entry : cyclicCategories)
            cyclic.append(entry);
        cyclic.sort(Qt::CaseInsensitive);
        result->warnings.append(QStringLiteral("Cyclic category hierarchy detected at: %1")
                                    .arg(cyclic.join(QStringLiteral(", "))));
    }

    std::sort(result->categories.begin(), result->categories.end(),
              [](const GpoCategory &left, const GpoCategory &right) {
        return left.qualifiedId().compare(right.qualifiedId(), Qt::CaseInsensitive) < 0;
    });
    std::sort(result->supportedDefinitions.begin(), result->supportedDefinitions.end(),
              [](const GpoSupportedDefinition &left, const GpoSupportedDefinition &right) {
        return left.qualifiedId().compare(right.qualifiedId(), Qt::CaseInsensitive) < 0;
    });
    std::sort(result->policies.begin(), result->policies.end(),
              [](const GpoPolicy &left, const GpoPolicy &right) {
        const int displayOrder = left.displayName.compare(right.displayName, Qt::CaseInsensitive);
        return displayOrder == 0
            ? left.qualifiedId().compare(right.qualifiedId(), Qt::CaseInsensitive) < 0
            : displayOrder < 0;
    });

    setError(error, {});
    return true;
}

void appendSearchValue(QStringList *values, const GpoRegistryValue &value)
{
    if (value.isSet()) {
        values->append(value.value);
        values->append(value.toDisplayString());
    }
}

void appendSearchAssignments(QStringList *values,
                             const QList<GpoRegistryAssignment> &assignments)
{
    for (const GpoRegistryAssignment &assignment : assignments) {
        values->append(assignment.key);
        values->append(assignment.valueName);
        appendSearchValue(values, assignment.value);
    }
}

QString policySearchText(const GpoPolicy &policy)
{
    QStringList values{
        policy.sourceFile,
        policy.policyNamespace,
        policy.id,
        policy.qualifiedId(),
        gpoPolicyClassName(policy.policyClass),
        policy.displayNameReference,
        policy.displayName,
        policy.explainTextReference,
        policy.explainText,
        policy.categoryId,
        policy.categoryHierarchy.join(QStringLiteral(" / ")),
        policy.supportedOnId,
        policy.supportedOn,
        policy.registryKey,
        policy.registryValueName,
    };
    values.append(policy.localizedDisplayNames.values());
    values.append(policy.localizedExplainTexts.values());
    values.append(policy.localizedSupportedOn.values());
    for (const QStringList &path : policy.localizedCategoryHierarchies)
        values.append(path.join(QStringLiteral(" / ")));
    appendSearchValue(&values, policy.enabledValue);
    appendSearchValue(&values, policy.disabledValue);
    appendSearchAssignments(&values, policy.enabledAssignments);
    appendSearchAssignments(&values, policy.disabledAssignments);

    for (const GpoElement &element : policy.elements) {
        values.append(element.id);
        values.append(gpoElementKindName(element.kind));
        values.append(element.materialControl());
        values.append(element.registryKey);
        values.append(element.registryValueName);
        values.append(element.valuePrefix);
        values.append(element.presentationLabel);
        values.append(element.localizedPresentationLabels.values());
        values.append(element.attributes.values());
        appendSearchValue(&values, element.trueValue);
        appendSearchValue(&values, element.falseValue);
        appendSearchAssignments(&values, element.trueAssignments);
        appendSearchAssignments(&values, element.falseAssignments);
        for (const GpoEnumOption &option : element.options) {
            values.append(option.displayNameReference);
            values.append(option.displayName);
            values.append(option.localizedDisplayNames.values());
            appendSearchValue(&values, option.value);
            appendSearchAssignments(&values, option.assignments);
        }
    }
    for (const GpoPresentationElement &element : policy.presentationElements) {
        values.append(gpoPresentationKindName(element.kind));
        values.append(element.refId);
        values.append(element.label);
        values.append(element.localizedLabels.values());
        values.append(element.defaultValue);
        values.append(element.attributes.values());
    }
    values.removeAll(QString());
    return values.join(QLatin1Char('\n'));
}

QString htmlEscape(QString value)
{
    value.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    value.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    value.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return value;
}

QString tableCell(QString value)
{
    value = htmlEscape(std::move(value));
    value.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    value.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    value.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return value.isEmpty() ? QStringLiteral("—") : value;
}

QString codeValue(const QString &value)
{
    return value.isEmpty()
        ? QStringLiteral("—")
        : QStringLiteral("<code>%1</code>").arg(htmlEscape(value));
}

std::optional<QString> localeValue(const QMap<QString, QString> &values,
                                   const QString &locale)
{
    if (locale.isEmpty())
        return std::nullopt;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key().compare(locale, Qt::CaseInsensitive) == 0)
            return *it;
    }
    return std::nullopt;
}

std::optional<QStringList> localePath(const QMap<QString, QStringList> &values,
                                      const QString &locale)
{
    if (locale.isEmpty())
        return std::nullopt;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key().compare(locale, Qt::CaseInsensitive) == 0)
            return *it;
    }
    return std::nullopt;
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("Yes") : QStringLiteral("No");
}

QString assignmentSummary(const GpoRegistryAssignment &assignment)
{
    return QStringLiteral("%1 · %2 · %3")
        .arg(assignment.key, assignment.valueName, assignment.value.toDisplayString());
}

void writeAssignmentTable(QTextStream &out,
                          const QString &heading,
                          const QList<GpoRegistryAssignment> &assignments)
{
    if (assignments.isEmpty())
        return;
    out << "\n##### " << heading << "\n\n"
        << "| Registry key | Value name | Value/action |\n"
        << "|---|---|---|\n";
    for (const GpoRegistryAssignment &assignment : assignments) {
        out << "| " << codeValue(assignment.key)
            << " | " << codeValue(assignment.valueName)
            << " | " << tableCell(assignment.value.toDisplayString()) << " |\n";
    }
}

QString elementConstraints(const GpoElement &element)
{
    QStringList constraints;
    if (element.minimumValue)
        constraints.append(QStringLiteral("minimum %1").arg(*element.minimumValue));
    if (element.maximumValue)
        constraints.append(QStringLiteral("maximum %1").arg(*element.maximumValue));
    if (element.minimumLength)
        constraints.append(QStringLiteral("minimum length %1").arg(*element.minimumLength));
    if (element.maximumLength)
        constraints.append(QStringLiteral("maximum length %1").arg(*element.maximumLength));
    if (element.maximumStrings)
        constraints.append(QStringLiteral("maximum entries %1").arg(*element.maximumStrings));
    if (element.expandable)
        constraints.append(QStringLiteral("expandable string"));
    if (element.storeAsText)
        constraints.append(QStringLiteral("store as text"));
    if (element.soft)
        constraints.append(QStringLiteral("soft validation"));
    if (element.additive)
        constraints.append(QStringLiteral("additive list"));
    if (element.explicitValue)
        constraints.append(QStringLiteral("explicit list values"));
    if (!element.valuePrefix.isNull())
        constraints.append(QStringLiteral("value prefix '%1'").arg(element.valuePrefix));
    return constraints.isEmpty() ? QStringLiteral("None") : constraints.join(QStringLiteral("; "));
}

QString presentationDefault(const GpoPresentationElement &element)
{
    QStringList values;
    if (!element.defaultValue.isEmpty())
        values.append(element.defaultValue);
    if (element.hasDefaultChecked)
        values.append(QStringLiteral("checked: %1").arg(yesNo(element.defaultChecked)));
    if (element.spinStep)
        values.append(QStringLiteral("step: %1").arg(*element.spinStep));
    return values.isEmpty() ? QStringLiteral("—") : values.join(QStringLiteral("; "));
}

QString presentationAttributes(const GpoPresentationElement &element)
{
    QStringList values;
    for (auto it = element.attributes.cbegin(); it != element.attributes.cend(); ++it)
        values.append(QStringLiteral("%1=%2").arg(it.key(), it.value()));
    return values.isEmpty() ? QStringLiteral("—") : values.join(QStringLiteral("; "));
}

} // namespace

QString GpoRegistryValue::toDisplayString() const
{
    switch (kind) {
    case GpoValueKind::Decimal:
        return QStringLiteral("Decimal %1").arg(value);
    case GpoValueKind::String:
        return QStringLiteral("String %1").arg(value);
    case GpoValueKind::Delete:
        return QStringLiteral("Delete value");
    case GpoValueKind::None:
        return QStringLiteral("Not specified");
    }
    return QStringLiteral("Not specified");
}

QString GpoElement::materialControl() const
{
    switch (kind) {
    case GpoElementKind::Boolean:
        return QStringLiteral("Switch");
    case GpoElementKind::Enum:
        return QStringLiteral("ComboBox");
    case GpoElementKind::Decimal:
        return QStringLiteral("SpinBox");
    case GpoElementKind::Text:
        return QStringLiteral("TextField");
    case GpoElementKind::MultiText:
        return QStringLiteral("TextArea");
    case GpoElementKind::List:
        return QStringLiteral("ListEditor");
    }
    return QStringLiteral("TextField");
}

QString GpoCategory::qualifiedId() const
{
    return policyNamespace.isEmpty() ? id : policyNamespace + QLatin1Char(':') + id;
}

QString GpoSupportedDefinition::qualifiedId() const
{
    return policyNamespace.isEmpty() ? id : policyNamespace + QLatin1Char(':') + id;
}

QString GpoPolicy::qualifiedId() const
{
    return policyNamespace.isEmpty() ? id : policyNamespace + QLatin1Char(':') + id;
}

QString gpoPolicyClassName(GpoPolicyClass value)
{
    switch (value) {
    case GpoPolicyClass::User:
        return QStringLiteral("User");
    case GpoPolicyClass::Machine:
        return QStringLiteral("Machine");
    case GpoPolicyClass::Both:
        return QStringLiteral("Both");
    }
    return QStringLiteral("Both");
}

QString gpoElementKindName(GpoElementKind value)
{
    switch (value) {
    case GpoElementKind::Boolean:
        return QStringLiteral("Boolean");
    case GpoElementKind::Enum:
        return QStringLiteral("Enum");
    case GpoElementKind::Decimal:
        return QStringLiteral("Decimal");
    case GpoElementKind::Text:
        return QStringLiteral("Text");
    case GpoElementKind::MultiText:
        return QStringLiteral("MultiText");
    case GpoElementKind::List:
        return QStringLiteral("List");
    }
    return QStringLiteral("Text");
}

QString gpoPresentationKindName(GpoPresentationKind value)
{
    switch (value) {
    case GpoPresentationKind::CheckBox:
        return QStringLiteral("CheckBox");
    case GpoPresentationKind::DropDownList:
        return QStringLiteral("DropDownList");
    case GpoPresentationKind::DecimalTextBox:
        return QStringLiteral("DecimalTextBox");
    case GpoPresentationKind::TextBox:
        return QStringLiteral("TextBox");
    case GpoPresentationKind::MultiTextBox:
        return QStringLiteral("MultiTextBox");
    case GpoPresentationKind::ListBox:
        return QStringLiteral("ListBox");
    case GpoPresentationKind::Text:
        return QStringLiteral("Text");
    }
    return QStringLiteral("Text");
}

QString GpoCatalog::defaultPolicyDefinitionsPath()
{
    QString windowsDirectory = qEnvironmentVariable("WINDIR");
    if (windowsDirectory.isEmpty())
        windowsDirectory = qEnvironmentVariable("SystemRoot");
    return windowsDirectory.isEmpty()
        ? QString()
        : QDir(windowsDirectory).filePath(QStringLiteral("PolicyDefinitions"));
}

bool GpoCatalog::loadInstalled(const QStringList &locales, QString *error)
{
    const QString path = defaultPolicyDefinitionsPath();
    if (path.isEmpty()) {
        setError(error,
                 QStringLiteral("Neither WINDIR nor SystemRoot is set; the installed "
                                "PolicyDefinitions folder cannot be located."));
        return false;
    }
    return loadFromDirectory(path, locales, error);
}

bool GpoCatalog::loadFromDirectory(const QString &policyDefinitionsPath,
                                   const QStringList &locales,
                                   QString *error)
{
    if (policyDefinitionsPath.trimmed().isEmpty()) {
        setError(error, QStringLiteral("PolicyDefinitions folder is empty."));
        return false;
    }

    BuildResult built;
    if (!buildCatalog(policyDefinitionsPath, locales, &built, error))
        return false;

    m_policyDefinitionsPath = std::move(built.path);
    m_locales = std::move(built.locales);
    m_warnings = std::move(built.warnings);
    m_categories = std::move(built.categories);
    m_supportedDefinitions = std::move(built.supportedDefinitions);
    m_policies = std::move(built.policies);
    setError(error, {});
    return true;
}

QList<GpoPolicy> GpoCatalog::search(const QString &query,
                                    GpoSearchMode mode,
                                    QString *error) const
{
    QList<GpoPolicy> result;
    setError(error, {});
    if (query.isEmpty())
        return m_policies;

    if (mode == GpoSearchMode::PlainText) {
        const QStringList tokens = query.simplified().toCaseFolded().split(
            QLatin1Char(' '), Qt::SkipEmptyParts);
        if (tokens.isEmpty())
            return m_policies;
        for (const GpoPolicy &policy : m_policies) {
            const QString haystack = policySearchText(policy).toCaseFolded();
            const bool matches = std::all_of(tokens.cbegin(), tokens.cend(),
                                             [&](const QString &token) {
                return haystack.contains(token);
            });
            if (matches)
                result.append(policy);
        }
        return result;
    }

    if (query.size() > MaximumRegexLength) {
        setError(error,
                 QStringLiteral("Regular expression is too long: maximum %1 UTF-16 code units.")
                     .arg(MaximumRegexLength));
        return {};
    }
    if (query.contains(QChar::Null)) {
        setError(error, QStringLiteral("Regular expression cannot contain a NUL character."));
        return {};
    }
    if (query.contains(QStringLiteral("(*LIMIT_"), Qt::CaseInsensitive)) {
        setError(error,
                 QStringLiteral("Regular-expression resource-limit controls are reserved by "
                                "WimForge."));
        return {};
    }

    const QRegularExpression expression(
        RegexResourceLimits + query,
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::UseUnicodePropertiesOption
            | QRegularExpression::DontCaptureOption);
    if (!expression.isValid()) {
        const qsizetype rawOffset = expression.patternErrorOffset();
        const qsizetype userOffset = rawOffset < 0
            ? rawOffset
            : std::max<qsizetype>(0, rawOffset - RegexResourceLimits.size());
        setError(error,
                 QStringLiteral("Invalid regular expression at offset %1: %2")
                     .arg(userOffset)
                     .arg(expression.errorString()));
        return {};
    }
    for (const GpoPolicy &policy : m_policies) {
        if (expression.match(policySearchText(policy)).hasMatch())
            result.append(policy);
    }
    return result;
}

QString GpoCatalog::toMarkdown(const QString &primaryLocale,
                               const QString &secondaryLocale) const
{
    const QString primary = primaryLocale.trimmed().isEmpty()
        ? (m_locales.isEmpty() ? QString() : m_locales.first())
        : primaryLocale.trimmed();
    const QString secondary = secondaryLocale.trimmed();
    const bool bilingual = !secondary.isEmpty()
        && secondary.compare(primary, Qt::CaseInsensitive) != 0;
    const QString primaryLabel = primary.isEmpty() ? QStringLiteral("default") : primary;

    QString markdown;
    QTextStream out(&markdown);
    out << "# WimForge Group Policy catalog\n\n"
        << "- PolicyDefinitions source: " << codeValue(m_policyDefinitionsPath) << "\n"
        << "- Policies: " << m_policies.size() << "\n"
        << "- Categories: " << m_categories.size() << "\n"
        << "- Loaded locales: "
        << tableCell(m_locales.isEmpty() ? QStringLiteral("none")
                                         : m_locales.join(QStringLiteral(", ")))
        << "\n"
        << "- Documentation locale: " << tableCell(primaryLabel);
    if (bilingual)
        out << " + " << tableCell(secondary);
    out << "\n\n"
        << "This file is generated from the installed ADMX schemas and their ADML resources. "
           "Registry actions are descriptive; generating this catalog does not apply a policy.\n";

    if (!m_warnings.isEmpty()) {
        out << "\n## Catalog warnings\n\n";
        for (const QString &warning : m_warnings)
            out << "- " << tableCell(warning) << "\n";
    }

    out << "\n## Policies\n";
    for (const GpoPolicy &policy : m_policies) {
        const QString primaryName = localeValue(policy.localizedDisplayNames, primary)
                                        .value_or(policy.displayName);
        const std::optional<QString> secondaryName = bilingual
            ? localeValue(policy.localizedDisplayNames, secondary)
            : std::optional<QString>();
        out << "\n### " << tableCell(primaryName);
        if (secondaryName && *secondaryName != primaryName)
            out << " / " << tableCell(*secondaryName);
        out << "\n\n"
            << "| Field | Value |\n"
            << "|---|---|\n"
            << "| Qualified ID | " << codeValue(policy.qualifiedId()) << " |\n"
            << "| Namespace | " << codeValue(policy.policyNamespace) << " |\n"
            << "| Schema ID | " << codeValue(policy.id) << " |\n"
            << "| Class | " << tableCell(gpoPolicyClassName(policy.policyClass)) << " |\n"
            << "| Source ADMX | " << codeValue(QFileInfo(policy.sourceFile).fileName()) << " |\n";

        const QStringList primaryPath = localePath(policy.localizedCategoryHierarchies, primary)
                                                .value_or(policy.categoryHierarchy);
        out << "| Category (" << tableCell(primaryLabel) << ") | "
            << tableCell(primaryPath.join(QStringLiteral(" / "))) << " |\n";
        if (bilingual) {
            const QStringList secondaryPath = localePath(policy.localizedCategoryHierarchies,
                                                         secondary).value_or(QStringList());
            out << "| Category (" << tableCell(secondary) << ") | "
                << tableCell(secondaryPath.join(QStringLiteral(" / "))) << " |\n";
        }
        out << "| Category ID | " << codeValue(policy.categoryId) << " |\n";

        const QString primarySupported = localeValue(policy.localizedSupportedOn, primary)
                                             .value_or(policy.supportedOn);
        out << "| Supported on (" << tableCell(primaryLabel) << ") | "
            << tableCell(primarySupported) << " |\n";
        if (bilingual) {
            out << "| Supported on (" << tableCell(secondary) << ") | "
                << tableCell(localeValue(policy.localizedSupportedOn, secondary).value_or(QString()))
                << " |\n";
        }
        out << "| Supported-on ID | " << codeValue(policy.supportedOnId) << " |\n"
            << "| Registry key | " << codeValue(policy.registryKey) << " |\n"
            << "| Registry value | " << codeValue(policy.registryValueName) << " |\n"
            << "| Enabled value | " << tableCell(policy.enabledValue.toDisplayString()) << " |\n"
            << "| Disabled value | " << tableCell(policy.disabledValue.toDisplayString()) << " |\n";

        out << "\n#### Explanation (" << primaryLabel << ")\n\n"
            << tableCell(localeValue(policy.localizedExplainTexts, primary)
                             .value_or(policy.explainText)) << "\n";
        if (bilingual) {
            out << "\n#### Explanation (" << secondary << ")\n\n"
                << tableCell(localeValue(policy.localizedExplainTexts, secondary)
                                 .value_or(QString())) << "\n";
        }

        writeAssignmentTable(out, QStringLiteral("Enabled registry list"),
                             policy.enabledAssignments);
        writeAssignmentTable(out, QStringLiteral("Disabled registry list"),
                             policy.disabledAssignments);

        out << "\n#### Configuration elements\n";
        if (policy.elements.isEmpty())
            out << "\nThis policy has no configurable ADMX elements.\n";
        for (const GpoElement &element : policy.elements) {
            const QString primaryElementLabel = localeValue(
                element.localizedPresentationLabels, primary).value_or(element.presentationLabel);
            const QString heading = primaryElementLabel.isEmpty() ? element.id : primaryElementLabel;
            out << "\n##### " << tableCell(heading) << "\n\n"
                << "| Field | Value |\n"
                << "|---|---|\n"
                << "| Element ID | " << codeValue(element.id) << " |\n"
                << "| ADMX kind | " << tableCell(gpoElementKindName(element.kind)) << " |\n"
                << "| Material control | " << tableCell(element.materialControl()) << " |\n"
                << "| Required | " << yesNo(element.required) << " |\n"
                << "| Registry key | " << codeValue(element.registryKey) << " |\n"
                << "| Registry value | " << codeValue(element.registryValueName) << " |\n"
                << "| Constraints | " << tableCell(elementConstraints(element)) << " |\n"
                << "| ADMX attributes | ";
            QStringList elementAttributes;
            for (auto it = element.attributes.cbegin(); it != element.attributes.cend(); ++it)
                elementAttributes.append(QStringLiteral("%1=%2").arg(it.key(), it.value()));
            out << tableCell(elementAttributes.join(QStringLiteral("; "))) << " |\n"
                << "| Presentation label (" << tableCell(primaryLabel) << ") | "
                << tableCell(primaryElementLabel) << " |\n";
            if (bilingual) {
                out << "| Presentation label (" << tableCell(secondary) << ") | "
                    << tableCell(localeValue(element.localizedPresentationLabels, secondary)
                                     .value_or(QString())) << " |\n";
            }
            out << "| Presentation default | "
                << tableCell(element.presentationDefaultValue) << " |\n"
                << "| Boolean true value | " << tableCell(element.trueValue.toDisplayString())
                << " |\n"
                << "| Boolean false value | " << tableCell(element.falseValue.toDisplayString())
                << " |\n";

            if (!element.options.isEmpty()) {
                out << "\n###### Enum options\n\n"
                    << "| Label (" << tableCell(primaryLabel) << ") |";
                if (bilingual)
                    out << " Label (" << tableCell(secondary) << ") |";
                out << " Value | Additional registry actions |\n|---|";
                if (bilingual)
                    out << "---|";
                out << "---|---|\n";
                for (const GpoEnumOption &option : element.options) {
                    out << "| " << tableCell(localeValue(option.localizedDisplayNames, primary)
                                                 .value_or(option.displayName)) << " |";
                    if (bilingual) {
                        out << " " << tableCell(localeValue(option.localizedDisplayNames, secondary)
                                                     .value_or(QString())) << " |";
                    }
                    QStringList assignments;
                    for (const GpoRegistryAssignment &assignment : option.assignments)
                        assignments.append(assignmentSummary(assignment));
                    out << " " << tableCell(option.value.toDisplayString())
                        << " | " << tableCell(assignments.join(QLatin1Char('\n'))) << " |\n";
                }
            }
            writeAssignmentTable(out, QStringLiteral("True registry list"),
                                 element.trueAssignments);
            writeAssignmentTable(out, QStringLiteral("False registry list"),
                                 element.falseAssignments);
        }

        out << "\n#### ADML presentation table\n";
        if (policy.presentationElements.isEmpty()) {
            out << "\nThis policy has no presentation table.\n";
        } else {
            out << "\n| Kind | Ref ID | Label (" << tableCell(primaryLabel) << ") |";
            if (bilingual)
                out << " Label (" << tableCell(secondary) << ") |";
            out << " Default / step | Attributes |\n|---|---|---|";
            if (bilingual)
                out << "---|";
            out << "---|---|\n";
            for (const GpoPresentationElement &element : policy.presentationElements) {
                out << "| " << tableCell(gpoPresentationKindName(element.kind))
                    << " | " << codeValue(element.refId)
                    << " | " << tableCell(localeValue(element.localizedLabels, primary)
                                                .value_or(element.label)) << " |";
                if (bilingual) {
                    out << " " << tableCell(localeValue(element.localizedLabels, secondary)
                                                 .value_or(QString())) << " |";
                }
                out << " " << tableCell(presentationDefault(element))
                    << " | " << tableCell(presentationAttributes(element)) << " |\n";
            }
        }
    }
    return markdown;
}

bool GpoCatalog::exportMarkdown(const QString &filePath,
                                const QString &primaryLocale,
                                const QString &secondaryLocale,
                                QString *error) const
{
    if (filePath.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Markdown destination is empty."));
        return false;
    }
    const QFileInfo destination(filePath);
    if (!QDir().mkpath(destination.absolutePath())) {
        setError(error,
                 QStringLiteral("Could not create Markdown folder: %1")
                     .arg(destination.absolutePath()));
        return false;
    }
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not open %1: %2").arg(filePath, file.errorString()));
        return false;
    }
    const QByteArray data = toMarkdown(primaryLocale, secondaryLocale).toUtf8();
    if (file.write(data) != data.size()) {
        setError(error, QStringLiteral("Could not write %1: %2").arg(filePath, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QStringLiteral("Could not finish %1: %2").arg(filePath, file.errorString()));
        return false;
    }
    setError(error, {});
    return true;
}

} // namespace wimforge
