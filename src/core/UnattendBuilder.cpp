#include "UnattendBuilder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace wimforge {
namespace {

constexpr auto UnattendNamespace = "urn:schemas-microsoft-com:unattend";
constexpr auto WcmNamespace = "http://schemas.microsoft.com/WMIConfig/2002/State";

void setError(QString *target, const QString &value)
{
    if (target)
        *target = value;
}

QJsonObject attributesToJson(const QMap<QString, QString> &attributes)
{
    QJsonObject object;
    for (auto it = attributes.cbegin(); it != attributes.cend(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

QMap<QString, QString> attributesFromJson(const QJsonObject &object)
{
    QMap<QString, QString> result;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (it.value().isString())
            result.insert(it.key(), it.value().toString());
    return result;
}

bool writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    if (path.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Choose an export path."));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("Could not create export folder."));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    setError(error, {});
    return true;
}

std::optional<QJsonObject> readJson(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("Invalid JSON at byte %1: %2")
                            .arg(parseError.offset).arg(parseError.errorString()));
        return std::nullopt;
    }
    return document.object();
}

QString pathKey(const QList<UnattendPathSegment> &path)
{
    QStringList pieces;
    for (const UnattendPathSegment &segment : path) {
        QString piece = segment.name;
        for (auto it = segment.attributes.cbegin(); it != segment.attributes.cend(); ++it)
            piece += QStringLiteral("[%1=%2]").arg(it.key(), it.value());
        pieces.append(piece);
    }
    return pieces.join(QLatin1Char('/'));
}

struct TreeNode
{
    QString name;
    QMap<QString, QString> attributes;
    QString value;
    bool hasValue = false;
    std::vector<std::unique_ptr<TreeNode>> children;
};

TreeNode *childFor(TreeNode *parent, const UnattendPathSegment &segment)
{
    for (const auto &child : parent->children) {
        if (child->name == segment.name && child->attributes == segment.attributes)
            return child.get();
    }
    auto node = std::make_unique<TreeNode>();
    node->name = segment.name;
    node->attributes = segment.attributes;
    parent->children.push_back(std::move(node));
    return parent->children.back().get();
}

void writeNode(QXmlStreamWriter &writer, const TreeNode &node)
{
    writer.writeStartElement(node.name);
    for (auto it = node.attributes.cbegin(); it != node.attributes.cend(); ++it) {
        if (it.key().startsWith(QStringLiteral("wcm:")))
            writer.writeAttribute(QString::fromLatin1(WcmNamespace), it.key().mid(4), it.value());
        else
            writer.writeAttribute(it.key(), it.value());
    }
    if (node.hasValue)
        writer.writeCharacters(node.value);
    for (const auto &child : node.children)
        writeNode(writer, *child);
    writer.writeEndElement();
}

QString sanitizePrefix(QString prefix)
{
    prefix = prefix.toUpper().trimmed();
    prefix.remove(QRegularExpression(QStringLiteral("[^A-Z0-9-]")));
    while (prefix.endsWith(QLatin1Char('-')))
        prefix.chop(1);
    return prefix.left(8);
}

UnattendPathSegment segment(const QString &name,
                            std::initializer_list<std::pair<QString, QString>> attributes = {})
{
    UnattendPathSegment result;
    result.name = name;
    for (const auto &attribute : attributes)
        result.attributes.insert(attribute.first, attribute.second);
    return result;
}

bool isFirstLogonCommandSetting(const UnattendSetting &setting)
{
    return setting.pass == SetupPass::OobeSystem
        && setting.component == QStringLiteral("Microsoft-Windows-Shell-Setup")
        && setting.path.size() == 3
        && setting.path.at(0).name == QStringLiteral("FirstLogonCommands")
        && setting.path.at(1).name == QStringLiteral("SynchronousCommand");
}

void removeGeneratedComputerNameCommand(UnattendProfile &profile)
{
    QList<QMap<QString, QString>> generatedCommandAttributes;
    for (const UnattendSetting &setting : std::as_const(profile.settings)) {
        if (isFirstLogonCommandSetting(setting)
            && setting.path.at(2).name == QStringLiteral("Description")
            && (setting.value == QStringLiteral("WimForge computer-name prompt")
                || setting.value == QStringLiteral("WimForge serial-number computer name"))) {
            generatedCommandAttributes.append(setting.path.at(1).attributes);
        }
    }

    for (qsizetype index = profile.settings.size(); index > 0; --index) {
        const UnattendSetting &setting = profile.settings.at(index - 1);
        if (isFirstLogonCommandSetting(setting)
            && generatedCommandAttributes.contains(setting.path.at(1).attributes)) {
            profile.settings.removeAt(index - 1);
        }
    }
}

void setGeneratedComputerNameCommandValue(UnattendProfile &profile,
                                          const QString &leaf,
                                          const QString &value)
{
    const QList<UnattendPathSegment> path{
        segment(QStringLiteral("FirstLogonCommands")),
        segment(QStringLiteral("SynchronousCommand"),
                {{QStringLiteral("wcm:action"), QStringLiteral("add")}}),
        segment(leaf),
    };
    const QString wanted = pathKey(path);
    for (UnattendSetting &setting : profile.settings) {
        if (setting.pass == SetupPass::OobeSystem
            && setting.component == QStringLiteral("Microsoft-Windows-Shell-Setup")
            && setting.architecture == QStringLiteral("amd64")
            && pathKey(setting.path) == wanted) {
            setting.value = value;
            return;
        }
    }
    profile.settings.append(UnattendSetting{
        SetupPass::OobeSystem,
        QStringLiteral("Microsoft-Windows-Shell-Setup"),
        QStringLiteral("amd64"),
        QStringLiteral("31bf3856ad364e35"),
        QStringLiteral("neutral"),
        QStringLiteral("nonSxS"),
        path,
        value,
    });
}

} // namespace

QString UnattendBuilder::passName(SetupPass pass)
{
    switch (pass) {
    case SetupPass::WindowsPE: return QStringLiteral("windowsPE");
    case SetupPass::OfflineServicing: return QStringLiteral("offlineServicing");
    case SetupPass::Generalize: return QStringLiteral("generalize");
    case SetupPass::Specialize: return QStringLiteral("specialize");
    case SetupPass::AuditSystem: return QStringLiteral("auditSystem");
    case SetupPass::AuditUser: return QStringLiteral("auditUser");
    case SetupPass::OobeSystem: return QStringLiteral("oobeSystem");
    }
    return QString();
}

std::optional<SetupPass> UnattendBuilder::parsePass(const QString &name)
{
    static const QList<SetupPass> passes{
        SetupPass::WindowsPE, SetupPass::OfflineServicing, SetupPass::Generalize,
        SetupPass::Specialize, SetupPass::AuditSystem, SetupPass::AuditUser, SetupPass::OobeSystem};
    for (const SetupPass pass : passes)
        if (passName(pass).compare(name, Qt::CaseInsensitive) == 0)
            return pass;
    return std::nullopt;
}

QJsonObject UnattendProfile::toJson() const
{
    QJsonArray settingArray;
    for (const UnattendSetting &setting : settings) {
        QJsonArray pathArray;
        for (const UnattendPathSegment &entry : setting.path) {
            pathArray.append(QJsonObject{{QStringLiteral("name"), entry.name},
                                         {QStringLiteral("attributes"), attributesToJson(entry.attributes)}});
        }
        settingArray.append(QJsonObject{
            {QStringLiteral("pass"), UnattendBuilder::passName(setting.pass)},
            {QStringLiteral("component"), setting.component},
            {QStringLiteral("architecture"), setting.architecture},
            {QStringLiteral("publicKeyToken"), setting.publicKeyToken},
            {QStringLiteral("language"), setting.language},
            {QStringLiteral("versionScope"), setting.versionScope},
            {QStringLiteral("path"), pathArray},
            {QStringLiteral("value"), setting.value},
        });
    }
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("wimforge.unattend")},
        {QStringLiteral("version"), CurrentSchemaVersion},
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), description},
        {QStringLiteral("settings"), settingArray},
        {QStringLiteral("placement"), QJsonObject{
             {QStringLiteral("mediaRoot"), copyToMediaRoot},
             {QStringLiteral("installImage"), copyToInstallImage},
             {QStringLiteral("bootImage"), copyToBootImage},
             {QStringLiteral("dualArchitecture"), dualArchitecture},
             {QStringLiteral("promptEditionSelection"), promptEditionSelection},
         }},
        {QStringLiteral("computerName"), QJsonObject{
             {QStringLiteral("mode"), static_cast<int>(computerNameMode)},
             {QStringLiteral("value"), computerName},
             {QStringLiteral("serialPrefix"), serialPrefix},
         }},
        {QStringLiteral("metadata"), metadata},
    };
}

std::optional<UnattendProfile> UnattendProfile::fromJson(const QJsonObject &json, QString *error)
{
    QStringList errors;
    if (json.value(QStringLiteral("schema")).toString() != QStringLiteral("wimforge.unattend"))
        errors.append(QStringLiteral("Unsupported unattended profile schema."));
    if (json.value(QStringLiteral("version")).toInt(-1) != CurrentSchemaVersion)
        errors.append(QStringLiteral("Unsupported unattended profile version."));

    UnattendProfile profile;
    profile.name = json.value(QStringLiteral("name")).toString();
    profile.description = json.value(QStringLiteral("description")).toString();
    const QJsonArray settings = json.value(QStringLiteral("settings")).toArray();
    for (qsizetype index = 0; index < settings.size(); ++index) {
        if (!settings.at(index).isObject()) {
            errors.append(QStringLiteral("settings[%1] must be an object.").arg(index));
            continue;
        }
        const QJsonObject object = settings.at(index).toObject();
        const auto pass = UnattendBuilder::parsePass(object.value(QStringLiteral("pass")).toString());
        if (!pass) {
            errors.append(QStringLiteral("settings[%1] has an unknown setup pass.").arg(index));
            continue;
        }
        UnattendSetting setting;
        setting.pass = *pass;
        setting.component = object.value(QStringLiteral("component")).toString();
        setting.architecture = object.value(QStringLiteral("architecture")).toString(QStringLiteral("amd64"));
        setting.publicKeyToken = object.value(QStringLiteral("publicKeyToken")).toString(QStringLiteral("31bf3856ad364e35"));
        setting.language = object.value(QStringLiteral("language")).toString(QStringLiteral("neutral"));
        setting.versionScope = object.value(QStringLiteral("versionScope")).toString(QStringLiteral("nonSxS"));
        setting.value = object.value(QStringLiteral("value")).toString();
        for (const QJsonValue &pathValue : object.value(QStringLiteral("path")).toArray()) {
            const QJsonObject pathObject = pathValue.toObject();
            setting.path.append(UnattendPathSegment{
                pathObject.value(QStringLiteral("name")).toString(),
                attributesFromJson(pathObject.value(QStringLiteral("attributes")).toObject())});
        }
        if (setting.component.isEmpty() || setting.path.isEmpty())
            errors.append(QStringLiteral("settings[%1] needs a component and path.").arg(index));
        profile.settings.append(std::move(setting));
    }

    const QJsonObject placement = json.value(QStringLiteral("placement")).toObject();
    profile.copyToMediaRoot = placement.value(QStringLiteral("mediaRoot")).toBool(true);
    profile.copyToInstallImage = placement.value(QStringLiteral("installImage")).toBool(false);
    profile.copyToBootImage = placement.value(QStringLiteral("bootImage")).toBool(false);
    profile.dualArchitecture = placement.value(QStringLiteral("dualArchitecture")).toBool(false);
    profile.promptEditionSelection = placement.value(QStringLiteral("promptEditionSelection")).toBool(false);
    const QJsonObject computer = json.value(QStringLiteral("computerName")).toObject();
    const int mode = computer.value(QStringLiteral("mode")).toInt(0);
    if (mode < 0 || mode > static_cast<int>(ComputerNameMode::SerialNumber))
        errors.append(QStringLiteral("Unknown computer-name mode."));
    else
        profile.computerNameMode = static_cast<ComputerNameMode>(mode);
    profile.computerName = computer.value(QStringLiteral("value")).toString();
    profile.serialPrefix = computer.value(QStringLiteral("serialPrefix")).toString();
    profile.metadata = json.value(QStringLiteral("metadata")).toObject();

    if (!errors.isEmpty()) {
        setError(error, errors.join(QLatin1Char('\n')));
        return std::nullopt;
    }
    setError(error, {});
    return profile;
}

std::optional<UnattendProfile> UnattendProfile::importXml(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return std::nullopt;
    }
    QXmlStreamReader reader(&file);
    UnattendProfile profile;
    profile.name = QFileInfo(path).completeBaseName();
    std::optional<SetupPass> currentPass;
    QString component;
    QString architecture = QStringLiteral("amd64");
    QString token = QStringLiteral("31bf3856ad364e35");
    QString language = QStringLiteral("neutral");
    QString scope = QStringLiteral("nonSxS");
    QList<UnattendPathSegment> pathStack;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.name().toString();
            if (name == QStringLiteral("settings")) {
                currentPass = UnattendBuilder::parsePass(reader.attributes().value(QStringLiteral("pass")).toString());
            } else if (name == QStringLiteral("component") && currentPass) {
                component = reader.attributes().value(QStringLiteral("name")).toString();
                architecture = reader.attributes().value(QStringLiteral("processorArchitecture")).toString();
                token = reader.attributes().value(QStringLiteral("publicKeyToken")).toString();
                language = reader.attributes().value(QStringLiteral("language")).toString();
                scope = reader.attributes().value(QStringLiteral("versionScope")).toString();
                pathStack.clear();
            } else if (!component.isEmpty()) {
                QMap<QString, QString> attributes;
                for (const QXmlStreamAttribute &attribute : reader.attributes()) {
                    const QString prefix = attribute.prefix().toString();
                    const QString key = prefix.isEmpty()
                        ? attribute.name().toString()
                        : prefix + QLatin1Char(':') + attribute.name().toString();
                    attributes.insert(key, attribute.value().toString());
                }
                pathStack.append(UnattendPathSegment{name, attributes});
            }
        } else if (reader.isCharacters() && !reader.isWhitespace() && currentPass
                   && !component.isEmpty() && !pathStack.isEmpty()) {
            profile.settings.append(UnattendSetting{
                *currentPass, component, architecture, token, language, scope,
                pathStack, reader.text().toString()});
        } else if (reader.isEndElement()) {
            const QString name = reader.name().toString();
            if (name == QStringLiteral("component")) {
                component.clear();
                pathStack.clear();
            } else if (name == QStringLiteral("settings")) {
                currentPass.reset();
            } else if (!component.isEmpty() && !pathStack.isEmpty()
                       && pathStack.back().name == name) {
                pathStack.removeLast();
            }
        }
    }
    if (reader.hasError()) {
        setError(error, QStringLiteral("Unattend XML error at line %1: %2")
                            .arg(reader.lineNumber()).arg(reader.errorString()));
        return std::nullopt;
    }
    setError(error, {});
    return profile;
}

QByteArray UnattendProfile::toXml(QString *error) const
{
    const UnattendValidation validation = validate();
    if (!validation.ok()) {
        setError(error, validation.errors.join(QLatin1Char('\n')));
        return {};
    }

    QByteArray bytes;
    QXmlStreamWriter writer(&bytes);
    writer.setAutoFormatting(true);
    writer.writeStartDocument(QStringLiteral("1.0"));
    writer.writeStartElement(QStringLiteral("unattend"));
    writer.writeDefaultNamespace(QString::fromLatin1(UnattendNamespace));
    writer.writeNamespace(QString::fromLatin1(WcmNamespace), QStringLiteral("wcm"));

    static const QList<SetupPass> passOrder{
        SetupPass::WindowsPE, SetupPass::OfflineServicing, SetupPass::Generalize,
        SetupPass::Specialize, SetupPass::AuditSystem, SetupPass::AuditUser, SetupPass::OobeSystem};
    for (const SetupPass pass : passOrder) {
        QList<UnattendSetting> passSettings;
        for (const UnattendSetting &setting : settings)
            if (setting.pass == pass)
                passSettings.append(setting);
        if (passSettings.isEmpty())
            continue;

        writer.writeStartElement(QStringLiteral("settings"));
        writer.writeAttribute(QStringLiteral("pass"), UnattendBuilder::passName(pass));

        struct ComponentKey {
            QString name, arch, token, language, scope;
            bool operator<(const ComponentKey &other) const
            {
                return std::tie(name, arch, token, language, scope)
                    < std::tie(other.name, other.arch, other.token, other.language, other.scope);
            }
        };
        std::map<ComponentKey, TreeNode> trees;
        for (const UnattendSetting &setting : passSettings) {
            ComponentKey key{setting.component, setting.architecture, setting.publicKeyToken,
                             setting.language, setting.versionScope};
            TreeNode &root = trees[key];
            TreeNode *cursor = &root;
            for (const UnattendPathSegment &entry : setting.path)
                cursor = childFor(cursor, entry);
            cursor->value = setting.value;
            cursor->hasValue = true;
        }
        for (const auto &[key, tree] : trees) {
            writer.writeStartElement(QStringLiteral("component"));
            writer.writeAttribute(QStringLiteral("name"), key.name);
            writer.writeAttribute(QStringLiteral("processorArchitecture"), key.arch);
            writer.writeAttribute(QStringLiteral("publicKeyToken"), key.token);
            writer.writeAttribute(QStringLiteral("language"), key.language);
            writer.writeAttribute(QStringLiteral("versionScope"), key.scope);
            for (const auto &child : tree.children)
                writeNode(writer, *child);
            writer.writeEndElement();
        }
        writer.writeEndElement();
    }
    writer.writeEndElement();
    writer.writeEndDocument();
    setError(error, {});
    return bytes;
}

bool UnattendProfile::exportXml(const QString &path, QString *error) const
{
    const QByteArray xml = toXml(error);
    return !xml.isEmpty() && writeBytes(path, xml, error);
}

bool UnattendProfile::exportJson(const QString &path, QString *error) const
{
    return writeBytes(path, QJsonDocument(toJson()).toJson(QJsonDocument::Indented), error);
}

std::optional<UnattendProfile> UnattendProfile::importJson(const QString &path, QString *error)
{
    const auto object = readJson(path, error);
    return object ? fromJson(*object, error) : std::nullopt;
}

UnattendValidation UnattendProfile::validate() const
{
    UnattendValidation result;
    if (settings.isEmpty())
        result.warnings.append(QStringLiteral("The answer file contains no settings."));
    if (!copyToMediaRoot && !copyToInstallImage && !copyToBootImage)
        result.warnings.append(QStringLiteral("The answer file has no configured deployment destination."));
    if (computerNameMode == ComputerNameMode::Fixed) {
        const QByteArray bytes = computerName.toUtf8();
        const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9-]+$"));
        if (bytes.isEmpty() || bytes.size() > 15 || !allowed.match(computerName).hasMatch()
            || computerName.startsWith(QLatin1Char('-')) || computerName.endsWith(QLatin1Char('-'))
            || computerName.contains(QRegularExpression(QStringLiteral("^[0-9]+$")))) {
            result.errors.append(QStringLiteral("Fixed computer name must be 1-15 bytes, use only letters, numbers and hyphens, not be numeric-only, and not begin/end with a hyphen."));
        }
    }
    if (computerName == QStringLiteral("[Prompt]"))
        result.errors.append(QStringLiteral("[Prompt] is an editor convention, not a valid Microsoft ComputerName value. Use Prompt mode."));
    for (qsizetype index = 0; index < settings.size(); ++index) {
        const UnattendSetting &setting = settings.at(index);
        if (setting.component.trimmed().isEmpty() || setting.path.isEmpty())
            result.errors.append(QStringLiteral("Setting %1 needs a component and XML path.").arg(index));
        for (const UnattendPathSegment &entry : setting.path)
            if (entry.name.trimmed().isEmpty())
                result.errors.append(QStringLiteral("Setting %1 contains an empty path segment.").arg(index));
    }
    return result;
}

void UnattendProfile::setValue(SetupPass pass,
                               const QString &component,
                               const QStringList &path,
                               const QString &value,
                               const QString &architecture)
{
    QList<UnattendPathSegment> segments;
    for (const QString &entry : path)
        segments.append(UnattendPathSegment{entry, {}});
    const QString wanted = pathKey(segments);
    for (UnattendSetting &setting : settings) {
        if (setting.pass == pass && setting.component == component
            && setting.architecture == architecture && pathKey(setting.path) == wanted) {
            setting.value = value;
            return;
        }
    }
    settings.append(UnattendSetting{pass, component, architecture,
        QStringLiteral("31bf3856ad364e35"), QStringLiteral("neutral"), QStringLiteral("nonSxS"),
        segments, value});
}

QString UnattendProfile::value(SetupPass pass,
                               const QString &component,
                               const QStringList &path,
                               const QString &architecture) const
{
    QList<UnattendPathSegment> segments;
    for (const QString &entry : path)
        segments.append(UnattendPathSegment{entry, {}});
    const QString wanted = pathKey(segments);
    for (const UnattendSetting &setting : settings)
        if (setting.pass == pass && setting.component == component
            && setting.architecture == architecture && pathKey(setting.path) == wanted)
            return setting.value;
    return {};
}

void UnattendProfile::applyComputerNameBehavior()
{
    const QString shell = QStringLiteral("Microsoft-Windows-Shell-Setup");
    removeGeneratedComputerNameCommand(*this);
    switch (computerNameMode) {
    case ComputerNameMode::Random:
        setValue(SetupPass::Specialize, shell, {QStringLiteral("ComputerName")}, QStringLiteral("*"));
        break;
    case ComputerNameMode::Fixed:
        setValue(SetupPass::Specialize, shell, {QStringLiteral("ComputerName")}, computerName);
        break;
    case ComputerNameMode::Prompt:
        setValue(SetupPass::Specialize, shell, {QStringLiteral("ComputerName")}, QStringLiteral("*"));
        setGeneratedComputerNameCommandValue(*this, QStringLiteral("Order"), QStringLiteral("1"));
        setGeneratedComputerNameCommandValue(
            *this, QStringLiteral("Description"), QStringLiteral("WimForge computer-name prompt"));
        setGeneratedComputerNameCommandValue(
            *this, QStringLiteral("CommandLine"), UnattendBuilder::computerNamePromptCommand());
        setGeneratedComputerNameCommandValue(
            *this, QStringLiteral("RequiresUserInput"), QStringLiteral("true"));
        break;
    case ComputerNameMode::SerialNumber:
        setValue(SetupPass::Specialize, shell, {QStringLiteral("ComputerName")}, QStringLiteral("*"));
        setGeneratedComputerNameCommandValue(*this, QStringLiteral("Order"), QStringLiteral("1"));
        setGeneratedComputerNameCommandValue(
            *this, QStringLiteral("Description"), QStringLiteral("WimForge serial-number computer name"));
        setGeneratedComputerNameCommandValue(
            *this, QStringLiteral("CommandLine"),
            UnattendBuilder::computerNameSerialCommand(serialPrefix));
        setGeneratedComputerNameCommandValue(
            *this, QStringLiteral("RequiresUserInput"), QStringLiteral("false"));
        break;
    }
}

QList<ProductKeyEntry> UnattendBuilder::microsoftPublishedGvlks()
{
    const QString docs = QStringLiteral("https://learn.microsoft.com/windows-server/get-started/kms-client-activation-keys");
    const QString notice = QStringLiteral("Microsoft-published Generic Volume License Key. It does not grant a license or activate Windows; use only on properly licensed volume editions with an authorized KMS/ADBA environment.");
    return {
        {QStringLiteral("Windows 11/10 Pro"), QStringLiteral("W269N-WFGWX-YVC9B-4J6C9-T83GX"), QStringLiteral("GVLK/KMS client"), docs, notice},
        {QStringLiteral("Windows 11/10 Pro N"), QStringLiteral("MH37W-N47XK-V7XM9-C7227-GCQG9"), QStringLiteral("GVLK/KMS client"), docs, notice},
        {QStringLiteral("Windows 11/10 Pro for Workstations"), QStringLiteral("NRG8B-VKK3Q-CXVCJ-9G2XF-6Q84J"), QStringLiteral("GVLK/KMS client"), docs, notice},
        {QStringLiteral("Windows 11/10 Pro Education"), QStringLiteral("6TP4R-GNPTD-KYYHQ-7B7DP-J447Y"), QStringLiteral("GVLK/KMS client"), docs, notice},
        {QStringLiteral("Windows 11/10 Education"), QStringLiteral("NW6C2-QMPVW-D7KKK-3GKT6-VCFB2"), QStringLiteral("GVLK/KMS client"), docs, notice},
        {QStringLiteral("Windows 11/10 Education N"), QStringLiteral("2WH4N-8QGBV-H22JP-CT43Q-MDWWJ"), QStringLiteral("GVLK/KMS client"), docs, notice},
        {QStringLiteral("Windows 11/10 Enterprise"), QStringLiteral("NPPR9-FWDCX-D2C8J-H872K-2YT43"), QStringLiteral("GVLK/KMS client"), docs, notice},
        {QStringLiteral("Windows 11/10 Enterprise N"), QStringLiteral("DPH2V-TTNVB-4X9Q3-TJR4H-KHJW4"), QStringLiteral("GVLK/KMS client"), docs, notice},
    };
}

UnattendProfile UnattendBuilder::fullAutomationTemplate()
{
    UnattendProfile profile;
    profile.name = QStringLiteral("Full automation");
    profile.description = QStringLiteral("Complete Windows PE, specialize and OOBE baseline with safe defaults.");
    profile.setValue(SetupPass::WindowsPE, QStringLiteral("Microsoft-Windows-International-Core-WinPE"),
                     {QStringLiteral("SetupUILanguage"), QStringLiteral("UILanguage")}, QStringLiteral("en-US"));
    profile.setValue(SetupPass::WindowsPE, QStringLiteral("Microsoft-Windows-International-Core-WinPE"),
                     {QStringLiteral("InputLocale")}, QStringLiteral("0409:00000409"));
    profile.setValue(SetupPass::WindowsPE, QStringLiteral("Microsoft-Windows-International-Core-WinPE"),
                     {QStringLiteral("SystemLocale")}, QStringLiteral("en-US"));
    profile.setValue(SetupPass::WindowsPE, QStringLiteral("Microsoft-Windows-International-Core-WinPE"),
                     {QStringLiteral("UILanguage")}, QStringLiteral("en-US"));
    profile.setValue(SetupPass::WindowsPE, QStringLiteral("Microsoft-Windows-International-Core-WinPE"),
                     {QStringLiteral("UserLocale")}, QStringLiteral("en-HK"));
    profile.setValue(SetupPass::WindowsPE, QStringLiteral("Microsoft-Windows-Setup"),
                     {QStringLiteral("UserData"), QStringLiteral("AcceptEula")}, QStringLiteral("true"));
    profile.setValue(SetupPass::WindowsPE, QStringLiteral("Microsoft-Windows-Setup"),
                     {QStringLiteral("DynamicUpdate"), QStringLiteral("Enable")}, QStringLiteral("true"));
    profile.setValue(SetupPass::Specialize, QStringLiteral("Microsoft-Windows-Shell-Setup"),
                     {QStringLiteral("RegisteredOwner")}, QStringLiteral("WimForge User"));
    profile.setValue(SetupPass::Specialize, QStringLiteral("Microsoft-Windows-Shell-Setup"),
                     {QStringLiteral("TimeZone")}, QStringLiteral("China Standard Time"));
    profile.setValue(SetupPass::OobeSystem, QStringLiteral("Microsoft-Windows-Shell-Setup"),
                     {QStringLiteral("OOBE"), QStringLiteral("HideEULAPage")}, QStringLiteral("true"));
    profile.setValue(SetupPass::OobeSystem, QStringLiteral("Microsoft-Windows-Shell-Setup"),
                     {QStringLiteral("OOBE"), QStringLiteral("ProtectYourPC")}, QStringLiteral("3"));
    profile.applyComputerNameBehavior();
    return profile;
}

UnattendProfile UnattendBuilder::aiDevelopmentTemplate()
{
    UnattendProfile profile = fullAutomationTemplate();
    profile.name = QStringLiteral("AI development workstation");
    profile.description = QStringLiteral("Windows baseline prepared for the resumable WimForge AI development package template.");
    profile.computerNameMode = ComputerNameMode::SerialNumber;
    profile.serialPrefix = QStringLiteral("AI");
    profile.applyComputerNameBehavior();
    profile.metadata.insert(QStringLiteral("packageTemplate"), QStringLiteral("ai-development"));
    return profile;
}

QString UnattendBuilder::computerNamePromptCommand()
{
    // [Prompt] is an NTLite editor convention, not a valid Microsoft ComputerName value.
    // This explicit first-logon command provides the equivalent behavior without emitting
    // an invalid literal into the answer file.
    return QStringLiteral(
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Add-Type -AssemblyName Microsoft.VisualBasic; "
        "$n=[Microsoft.VisualBasic.Interaction]::InputBox('Computer name (1-15 letters, numbers or hyphens)','WimForge setup',$env:COMPUTERNAME); "
        "if($n -match '^(?![0-9]+$)[A-Za-z0-9](?:[A-Za-z0-9-]{0,13}[A-Za-z0-9])?$'){Rename-Computer -NewName $n -Force -Restart}\"");
}

QString UnattendBuilder::computerNameSerialCommand(const QString &prefix)
{
    const QString clean = sanitizePrefix(prefix);
    return QStringLiteral(
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"$s=(Get-CimInstance Win32_BIOS).SerialNumber; "
        "$s=($s -replace '[^A-Za-z0-9-]','').ToUpper(); $n=('%1-'+$s).Trim('-'); "
        "if($n.Length -gt 15){$n=$n.Substring(0,15).TrimEnd('-')}; "
        "if($n -and $n -notmatch '^[0-9]+$'){Rename-Computer -NewName $n -Force -Restart}\"")
        .arg(clean);
}

} // namespace wimforge
