#include "core/UnattendBuilder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

using namespace wimforge;

namespace {

class TestRun
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int result() const
    {
        if (m_failures == 0)
            QTextStream(stdout) << "unattend_builder_tests: all checks passed\n";
        else
            QTextStream(stderr) << "unattend_builder_tests: " << m_failures
                                << " check(s) failed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

UnattendSetting setting(SetupPass pass,
                        const QString &component,
                        const QList<UnattendPathSegment> &path,
                        const QString &value,
                        const QString &architecture = QStringLiteral("amd64"))
{
    return UnattendSetting{
        pass,
        component,
        architecture,
        QStringLiteral("31bf3856ad364e35"),
        QStringLiteral("neutral"),
        QStringLiteral("nonSxS"),
        path,
        value,
    };
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

bool hasError(const UnattendValidation &validation, const QString &needle)
{
    for (const QString &error : validation.errors)
        if (error.contains(needle, Qt::CaseInsensitive))
            return true;
    return false;
}

bool hasWarning(const UnattendValidation &validation, const QString &needle)
{
    for (const QString &warning : validation.warnings)
        if (warning.contains(needle, Qt::CaseInsensitive))
            return true;
    return false;
}

const UnattendSetting *findGeneratedCommandLeaf(const UnattendProfile &profile,
                                                const QString &leaf)
{
    for (const UnattendSetting &candidate : profile.settings) {
        if (candidate.pass == SetupPass::OobeSystem
            && candidate.component == QStringLiteral("Microsoft-Windows-Shell-Setup")
            && candidate.path.size() == 3
            && candidate.path.at(0).name == QStringLiteral("FirstLogonCommands")
            && candidate.path.at(1).name == QStringLiteral("SynchronousCommand")
            && candidate.path.at(2).name == leaf) {
            return &candidate;
        }
    }
    return nullptr;
}

const UnattendSetting *findByKeyValue(const UnattendProfile &profile, const QString &keyValue)
{
    for (const UnattendSetting &candidate : profile.settings) {
        for (const UnattendPathSegment &pathSegment : candidate.path) {
            if (pathSegment.attributes.value(QStringLiteral("wcm:keyValue")) == keyValue)
                return &candidate;
        }
    }
    return nullptr;
}

int firstLogonCommandSettingCount(const UnattendProfile &profile)
{
    int result = 0;
    for (const UnattendSetting &candidate : profile.settings) {
        if (candidate.pass == SetupPass::OobeSystem
            && candidate.component == QStringLiteral("Microsoft-Windows-Shell-Setup")
            && candidate.path.size() == 3
            && candidate.path.at(0).name == QStringLiteral("FirstLogonCommands")
            && candidate.path.at(1).name == QStringLiteral("SynchronousCommand")) {
            ++result;
        }
    }
    return result;
}

void appendFirstLogonCommand(UnattendProfile &profile,
                             const QString &order,
                             const QString &description,
                             const QString &commandLine,
                             const QString &requiresUserInput)
{
    const auto appendLeaf = [&](const QString &leaf, const QString &value) {
        profile.settings.append(setting(
            SetupPass::OobeSystem,
            QStringLiteral("Microsoft-Windows-Shell-Setup"),
            {
                {QStringLiteral("FirstLogonCommands"), {}},
                {QStringLiteral("SynchronousCommand"),
                 {{QStringLiteral("wcm:action"), QStringLiteral("add")}}},
                {leaf, {}},
            },
            value));
    };
    appendLeaf(QStringLiteral("Order"), order);
    appendLeaf(QStringLiteral("Description"), description);
    appendLeaf(QStringLiteral("CommandLine"), commandLine);
    appendLeaf(QStringLiteral("RequiresUserInput"), requiresUserInput);
}

bool hasFirstLogonLeafValue(const UnattendProfile &profile,
                            const QString &leaf,
                            const QString &value)
{
    for (const UnattendSetting &candidate : profile.settings) {
        if (candidate.pass == SetupPass::OobeSystem
            && candidate.component == QStringLiteral("Microsoft-Windows-Shell-Setup")
            && candidate.path.size() == 3
            && candidate.path.at(0).name == QStringLiteral("FirstLogonCommands")
            && candidate.path.at(1).name == QStringLiteral("SynchronousCommand")
            && candidate.path.at(2).name == leaf
            && candidate.value == value) {
            return true;
        }
    }
    return false;
}

void testPasses(TestRun &test, const QString &temporaryRoot)
{
    struct PassCase {
        SetupPass pass;
        QString name;
    };
    const QList<PassCase> passes{
        {SetupPass::WindowsPE, QStringLiteral("windowsPE")},
        {SetupPass::OfflineServicing, QStringLiteral("offlineServicing")},
        {SetupPass::Generalize, QStringLiteral("generalize")},
        {SetupPass::Specialize, QStringLiteral("specialize")},
        {SetupPass::AuditSystem, QStringLiteral("auditSystem")},
        {SetupPass::AuditUser, QStringLiteral("auditUser")},
        {SetupPass::OobeSystem, QStringLiteral("oobeSystem")},
    };

    UnattendProfile profile;
    qsizetype ordinal = 0;
    for (const PassCase &entry : passes) {
        test.check(UnattendBuilder::passName(entry.pass) == entry.name,
                   QStringLiteral("passName maps %1").arg(entry.name));
        const auto parsed = UnattendBuilder::parsePass(entry.name.toUpper());
        test.check(parsed && *parsed == entry.pass,
                   QStringLiteral("parsePass accepts %1 case-insensitively").arg(entry.name));
        profile.setValue(entry.pass,
                         QStringLiteral("WimForge-Test-Component-%1").arg(ordinal),
                         {QStringLiteral("Value")},
                         QStringLiteral("pass-value-%1").arg(ordinal));
        ++ordinal;
    }
    test.check(!UnattendBuilder::parsePass(QStringLiteral("notASetupPass")),
               QStringLiteral("parsePass rejects an unknown setup pass"));

    QString error;
    const QByteArray xml = profile.toXml(&error);
    test.check(!xml.isEmpty() && error.isEmpty(),
               QStringLiteral("all-pass profile exports: %1").arg(error));
    qsizetype priorPosition = -1;
    for (const PassCase &entry : passes) {
        const QByteArray marker = QByteArrayLiteral("pass=\"") + entry.name.toUtf8()
            + QByteArrayLiteral("\"");
        const qsizetype position = xml.indexOf(marker);
        test.check(position > priorPosition,
                   QStringLiteral("XML emits %1 in setup-pass order").arg(entry.name));
        priorPosition = position;
    }

    const QString path = QDir(temporaryRoot).filePath(QStringLiteral("passes/all-passes.xml"));
    test.check(writeFile(path, xml), QStringLiteral("all-pass XML fixture is writable"));
    const auto imported = UnattendProfile::importXml(path, &error);
    test.check(imported.has_value(), QStringLiteral("all-pass XML imports: %1").arg(error));
    if (imported) {
        test.check(imported->settings.size() == passes.size(),
                   QStringLiteral("all seven setup-pass settings survive XML import"));
        ordinal = 0;
        for (const PassCase &entry : passes) {
            test.check(imported->value(entry.pass,
                                       QStringLiteral("WimForge-Test-Component-%1").arg(ordinal),
                                       {QStringLiteral("Value")})
                           == QStringLiteral("pass-value-%1").arg(ordinal),
                       QStringLiteral("%1 setting round-trips").arg(entry.name));
            ++ordinal;
        }
    }
}

UnattendProfile preservationProfile()
{
    UnattendProfile profile;
    profile.name = QStringLiteral("Preservation profile");
    profile.description = QStringLiteral("Unknown fields and attributed repeated paths");
    profile.copyToMediaRoot = false;
    profile.copyToInstallImage = true;
    profile.copyToBootImage = true;
    profile.dualArchitecture = true;
    profile.promptEditionSelection = true;
    profile.computerNameMode = ComputerNameMode::SerialNumber;
    profile.computerName = QStringLiteral("editor-only-value");
    profile.serialPrefix = QStringLiteral("LAB");
    profile.metadata.insert(QStringLiteral("futureObject"),
                            QJsonObject{{QStringLiteral("enabled"), true},
                                        {QStringLiteral("revision"), 7}});

    const auto pathFor = [](const QString &keyValue, const QString &futureFlag) {
        return QList<UnattendPathSegment>{
            {QStringLiteral("Interfaces"), {}},
            {QStringLiteral("Interface"),
             {{QStringLiteral("wcm:action"), QStringLiteral("add")},
              {QStringLiteral("futureInterfaceAttribute"), QStringLiteral("preserve")}}},
            {QStringLiteral("NameServerList"), {}},
            {QStringLiteral("IpAddress"),
             {{QStringLiteral("wcm:action"), QStringLiteral("add")},
              {QStringLiteral("wcm:keyValue"), keyValue},
              {QStringLiteral("futureFlag"), futureFlag}}},
        };
    };
    profile.settings.append(setting(SetupPass::Specialize,
                                    QStringLiteral("Microsoft-Windows-NetBT"),
                                    pathFor(QStringLiteral("IpAddress1"), QStringLiteral("alpha")),
                                    QStringLiteral("alpha & <one>")));
    profile.settings.append(setting(SetupPass::Specialize,
                                    QStringLiteral("Microsoft-Windows-NetBT"),
                                    pathFor(QStringLiteral("IpAddress2"), QStringLiteral("beta")),
                                    QStringLiteral("beta > two")));
    return profile;
}

void checkPreservedSettings(TestRun &test,
                            const UnattendProfile &profile,
                            const QString &context)
{
    const UnattendSetting *first = findByKeyValue(profile, QStringLiteral("IpAddress1"));
    const UnattendSetting *second = findByKeyValue(profile, QStringLiteral("IpAddress2"));
    test.check(first && second, QStringLiteral("%1 keeps both repeated attributed paths").arg(context));
    if (first) {
        test.check(first->value == QStringLiteral("alpha & <one>"),
                   QStringLiteral("%1 decodes XML metacharacters losslessly").arg(context));
        test.check(first->path.at(1).attributes.value(
                       QStringLiteral("futureInterfaceAttribute")) == QStringLiteral("preserve")
                       && first->path.at(3).attributes.value(QStringLiteral("futureFlag"))
                           == QStringLiteral("alpha")
                       && first->path.at(3).attributes.value(QStringLiteral("wcm:action"))
                           == QStringLiteral("add"),
                   QStringLiteral("%1 preserves unknown and WCM path attributes").arg(context));
    }
    if (second) {
        test.check(second->value == QStringLiteral("beta > two")
                       && second->path.at(3).attributes.value(QStringLiteral("futureFlag"))
                           == QStringLiteral("beta"),
                   QStringLiteral("%1 preserves the second repeated item independently").arg(context));
    }
}

void testJsonAndXmlPreservation(TestRun &test, const QString &temporaryRoot)
{
    const UnattendProfile profile = preservationProfile();
    QString error;
    const QString jsonPath = QDir(temporaryRoot).filePath(
        QStringLiteral("preservation/nested/profile.json"));
    test.check(profile.exportJson(jsonPath, &error),
               QStringLiteral("portable JSON exports to a new folder: %1").arg(error));
    const auto jsonImported = UnattendProfile::importJson(jsonPath, &error);
    test.check(jsonImported.has_value(), QStringLiteral("portable JSON imports: %1").arg(error));
    if (jsonImported) {
        test.check(jsonImported->toJson() == profile.toJson(),
                   QStringLiteral("JSON round-trip preserves every represented field"));
        checkPreservedSettings(test, *jsonImported, QStringLiteral("JSON round-trip"));
    }

    const QString xmlPath = QDir(temporaryRoot).filePath(
        QStringLiteral("preservation/nested/autounattend.xml"));
    test.check(profile.exportXml(xmlPath, &error),
               QStringLiteral("unattend XML exports to a new folder: %1").arg(error));
    QFile xmlFile(xmlPath);
    test.check(xmlFile.open(QIODevice::ReadOnly), QStringLiteral("exported XML is readable"));
    const QByteArray xml = xmlFile.readAll();
    test.check(xml.contains(QByteArrayLiteral("xmlns=\"urn:schemas-microsoft-com:unattend\"")),
               QStringLiteral("XML declares the Microsoft unattend namespace; prefix was: %1")
                   .arg(QString::fromUtf8(xml.left(240))));
    test.check(xml.contains(QByteArrayLiteral("wcm:keyValue=\"IpAddress1\""))
                   && xml.contains(QByteArrayLiteral("wcm:keyValue=\"IpAddress2\""))
                   && xml.contains(QByteArrayLiteral("futureFlag=\"alpha\"")),
               QStringLiteral("XML writes repeated and unknown path attributes"));
    test.check(xml.contains(QByteArrayLiteral("alpha &amp; &lt;one&gt;")),
               QStringLiteral("XML escapes setting values"));

    const auto xmlImported = UnattendProfile::importXml(xmlPath, &error);
    test.check(xmlImported.has_value(), QStringLiteral("unattend XML imports: %1").arg(error));
    if (xmlImported) {
        test.check(xmlImported->settings.size() == 2,
                   QStringLiteral("XML import creates one setting per repeated leaf"));
        checkPreservedSettings(test, *xmlImported, QStringLiteral("XML round-trip"));

        const QString secondPath = QDir(temporaryRoot).filePath(
            QStringLiteral("preservation/second-generation.xml"));
        test.check(xmlImported->exportXml(secondPath, &error),
                   QStringLiteral("imported XML can be exported again: %1").arg(error));
        const auto secondImport = UnattendProfile::importXml(secondPath, &error);
        test.check(secondImport.has_value(),
                   QStringLiteral("second-generation XML imports: %1").arg(error));
        if (secondImport) {
            test.check(secondImport->settings.size() == 2,
                       QStringLiteral("second XML generation retains both repeated leaves"));
            checkPreservedSettings(test, *secondImport,
                                   QStringLiteral("second XML generation"));
        }
    }

    QJsonObject badSchema = profile.toJson();
    badSchema.insert(QStringLiteral("schema"), QStringLiteral("another.product"));
    badSchema.insert(QStringLiteral("version"), 999);
    const auto rejectedSchema = UnattendProfile::fromJson(badSchema, &error);
    test.check(!rejectedSchema
                   && error.contains(QStringLiteral("schema"), Qt::CaseInsensitive)
                   && error.contains(QStringLiteral("version"), Qt::CaseInsensitive),
               QStringLiteral("JSON import rejects unknown schema and version"));

    QJsonObject badPass = profile.toJson();
    QJsonArray settings = badPass.value(QStringLiteral("settings")).toArray();
    QJsonObject firstSetting = settings.at(0).toObject();
    firstSetting.insert(QStringLiteral("pass"), QStringLiteral("futurePass"));
    settings.replace(0, firstSetting);
    badPass.insert(QStringLiteral("settings"), settings);
    test.check(!UnattendProfile::fromJson(badPass, &error)
                   && error.contains(QStringLiteral("unknown setup pass"),
                                     Qt::CaseInsensitive),
               QStringLiteral("JSON import rejects unknown setup passes"));
}

void testComputerNameJsonAndXmlCoherence(TestRun &test, const QString &temporaryRoot)
{
    QString error;
    const QJsonObject validJson = UnattendBuilder::fullAutomationTemplate().toJson();
    const auto rejectMode = [&](const QJsonValue &mode,
                                const QString &expectedError,
                                const QString &context) {
        QJsonObject candidate = validJson;
        QJsonObject computer = candidate.value(QStringLiteral("computerName")).toObject();
        computer.insert(QStringLiteral("mode"), mode);
        candidate.insert(QStringLiteral("computerName"), computer);
        const auto imported = UnattendProfile::fromJson(candidate, &error);
        test.check(!imported && error.contains(expectedError, Qt::CaseInsensitive), context);
    };
    rejectMode(QStringLiteral("fixed"), QStringLiteral("integer"),
               QStringLiteral("JSON rejects a string computer-name mode"));
    rejectMode(QJsonValue::Null, QStringLiteral("integer"),
               QStringLiteral("JSON rejects an explicit null computer-name mode"));
    rejectMode(1.5, QStringLiteral("integer"),
               QStringLiteral("JSON rejects a fractional computer-name mode"));
    rejectMode(4, QStringLiteral("unknown computer-name mode"),
               QStringLiteral("JSON rejects an out-of-range computer-name mode"));

    QJsonObject legacyJson = validJson;
    QJsonObject legacyComputer = legacyJson.value(QStringLiteral("computerName")).toObject();
    legacyComputer.remove(QStringLiteral("mode"));
    legacyJson.insert(QStringLiteral("computerName"), legacyComputer);
    const auto legacyProfile = UnattendProfile::fromJson(legacyJson, &error);
    test.check(legacyProfile
                   && legacyProfile->computerNameMode == ComputerNameMode::Random,
               QStringLiteral("schema-v1 JSON without computerName.mode retains the Random default"));

    QJsonObject minimalLegacyJson = validJson;
    minimalLegacyJson.remove(QStringLiteral("computerName"));
    const auto minimalLegacyProfile = UnattendProfile::fromJson(minimalLegacyJson, &error);
    test.check(minimalLegacyProfile
                   && minimalLegacyProfile->computerNameMode == ComputerNameMode::Random,
               QStringLiteral("schema-v1 JSON without computerName retains the Random default"));

    QJsonObject mismatchedJson = validJson;
    QJsonObject computer = mismatchedJson.value(QStringLiteral("computerName")).toObject();
    computer.insert(QStringLiteral("mode"), static_cast<int>(ComputerNameMode::Fixed));
    computer.insert(QStringLiteral("value"), QStringLiteral("JSON-FIXED"));
    mismatchedJson.insert(QStringLiteral("computerName"), computer);
    const auto mismatched = UnattendProfile::fromJson(mismatchedJson, &error);
    test.check(mismatched.has_value(),
               QStringLiteral("structurally valid mismatched JSON still imports for diagnosis"));
    if (mismatched) {
        const UnattendValidation validation = mismatched->validate();
        test.check(!validation.ok() && hasError(validation, QStringLiteral("intent")),
                   QStringLiteral("fixed JSON cannot silently retain an effective random name"));
        test.check(mismatched->toXml(&error).isEmpty()
                       && error.contains(QStringLiteral("intent"), Qt::CaseInsensitive),
                   QStringLiteral("a fixed-name intent mismatch cannot reach XML"));
    }

    const auto answerXml = [](const QByteArray &computerName) {
        return QByteArrayLiteral(
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                   "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">"
                   "<settings pass=\"specialize\">"
                   "<component name=\"Microsoft-Windows-Shell-Setup\" "
                   "processorArchitecture=\"amd64\" publicKeyToken=\"31bf3856ad364e35\" "
                   "language=\"neutral\" versionScope=\"nonSxS\">"
                   "<ComputerName>")
            + computerName
            + QByteArrayLiteral("</ComputerName></component></settings></unattend>");
    };

    const QString fixedXmlPath = QDir(temporaryRoot).filePath(
        QStringLiteral("computer-name/imported-fixed.xml"));
    test.check(writeFile(fixedXmlPath, answerXml(QByteArrayLiteral("IMPORTED-PC"))),
               QStringLiteral("fixed-name XML fixture is writable"));
    const auto fixedXml = UnattendProfile::importXml(fixedXmlPath, &error);
    test.check(fixedXml && fixedXml->computerNameMode == ComputerNameMode::Fixed
                   && fixedXml->computerName == QStringLiteral("IMPORTED-PC")
                   && fixedXml->validate().ok(),
               QStringLiteral("XML import infers a coherent fixed computer-name intent: %1")
                   .arg(error));
    if (fixedXml) {
        const QByteArray exported = fixedXml->toXml(&error);
        test.check(exported.contains(QByteArrayLiteral("<ComputerName>IMPORTED-PC")),
                   QStringLiteral("an imported fixed name remains exportable"));
    }

    const QString randomXmlPath = QDir(temporaryRoot).filePath(
        QStringLiteral("computer-name/imported-random.xml"));
    test.check(writeFile(randomXmlPath, answerXml(QByteArrayLiteral("*"))),
               QStringLiteral("random-name XML fixture is writable"));
    const auto randomXml = UnattendProfile::importXml(randomXmlPath, &error);
    test.check(randomXml && randomXml->computerNameMode == ComputerNameMode::Random
                   && randomXml->validate().ok(),
               QStringLiteral("legitimate random-name XML retains normal import behavior: %1")
                   .arg(error));

    const QString invalidXmlPath = QDir(temporaryRoot).filePath(
        QStringLiteral("computer-name/imported-literal-prompt.xml"));
    test.check(writeFile(invalidXmlPath, answerXml(QByteArrayLiteral("[Prompt]"))),
               QStringLiteral("invalid computer-name XML fixture is writable"));
    const auto invalidXml = UnattendProfile::importXml(invalidXmlPath, &error);
    test.check(invalidXml && invalidXml->computerNameMode == ComputerNameMode::Fixed
                   && invalidXml->computerName == QStringLiteral("[Prompt]"),
               QStringLiteral("XML import exposes the invalid literal as fixed intent"));
    if (invalidXml) {
        const UnattendValidation validation = invalidXml->validate();
        test.check(!validation.ok() && hasError(validation, QStringLiteral("editor convention")),
                   QStringLiteral("an imported [Prompt] literal is caught by validation"));
        test.check(invalidXml->toXml(&error).isEmpty(),
                   QStringLiteral("an imported invalid ComputerName cannot be re-exported"));
    }
}

UnattendValidation validateFixedName(const QString &name)
{
    UnattendProfile profile;
    profile.computerNameMode = ComputerNameMode::Fixed;
    profile.computerName = name;
    profile.applyComputerNameBehavior();
    return profile.validate();
}

void testValidationAndNamingRules(TestRun &test, const QString &temporaryRoot)
{
    UnattendProfile empty;
    const UnattendValidation emptyValidation = empty.validate();
    test.check(emptyValidation.ok() && hasWarning(emptyValidation, QStringLiteral("no settings")),
               QStringLiteral("an empty answer file is warning-only"));
    empty.copyToMediaRoot = false;
    test.check(hasWarning(empty.validate(), QStringLiteral("no configured deployment destination")),
               QStringLiteral("missing deployment destinations produce a warning"));

    const QString fifteenBytes = QStringLiteral("PC-123456789012");
    const QString sixteenBytes = QStringLiteral("PC-1234567890123");
    test.check(fifteenBytes.toUtf8().size() == 15 && validateFixedName(fifteenBytes).ok(),
               QStringLiteral("a valid 15-byte fixed computer name is accepted"));
    test.check(sixteenBytes.toUtf8().size() == 16
                   && !validateFixedName(sixteenBytes).ok(),
               QStringLiteral("a 16-byte fixed computer name is rejected"));
    test.check(validateFixedName(QStringLiteral("A")).ok(),
               QStringLiteral("a one-byte fixed computer name is accepted"));

    const QList<QString> invalidNames{
        QString(),
        QStringLiteral("-LEADING"),
        QStringLiteral("TRAILING-"),
        QStringLiteral("123456789"),
        QStringLiteral("HAS SPACE"),
        QStringLiteral("HAS_UNDERSCORE"),
        QStringLiteral("電腦"),
    };
    for (const QString &name : invalidNames) {
        test.check(!validateFixedName(name).ok(),
                   QStringLiteral("invalid fixed computer name is rejected: '%1'").arg(name));
    }

    UnattendProfile literalPrompt;
    literalPrompt.computerNameMode = ComputerNameMode::Fixed;
    literalPrompt.computerName = QStringLiteral("[Prompt]");
    literalPrompt.applyComputerNameBehavior();
    const UnattendValidation promptValidation = literalPrompt.validate();
    test.check(!promptValidation.ok()
                   && hasError(promptValidation, QStringLiteral("editor convention")),
               QStringLiteral("literal [Prompt] is explicitly rejected"));
    QString error;
    test.check(literalPrompt.toXml(&error).isEmpty()
                   && error.contains(QStringLiteral("[Prompt]")),
               QStringLiteral("invalid literal [Prompt] cannot reach exported XML"));
    const QString invalidExport = QDir(temporaryRoot).filePath(
        QStringLiteral("invalid/literal-prompt.xml"));
    test.check(!literalPrompt.exportXml(invalidExport, &error)
                   && !QFileInfo::exists(invalidExport),
               QStringLiteral("invalid answer files are not written to disk"));

    UnattendProfile malformed;
    malformed.settings.append(setting(SetupPass::Specialize, QString(), {},
                                      QStringLiteral("value")));
    malformed.settings.append(setting(
        SetupPass::OobeSystem,
        QStringLiteral("Component"),
        {{QString(), {}}},
        QStringLiteral("value")));
    const UnattendValidation malformedValidation = malformed.validate();
    test.check(!malformedValidation.ok()
                   && hasError(malformedValidation, QStringLiteral("component and XML path"))
                   && hasError(malformedValidation, QStringLiteral("empty path segment")),
               QStringLiteral("validation rejects missing components, paths, and path names"));
}

void testComputerNameModes(TestRun &test)
{
    const QString shell = QStringLiteral("Microsoft-Windows-Shell-Setup");

    UnattendProfile random;
    random.computerNameMode = ComputerNameMode::Random;
    random.applyComputerNameBehavior();
    test.check(random.value(SetupPass::Specialize, shell,
                            {QStringLiteral("ComputerName")}) == QStringLiteral("*"),
               QStringLiteral("Random mode emits Microsoft's asterisk value"));
    test.check(firstLogonCommandSettingCount(random) == 0,
               QStringLiteral("Random mode does not add a rename command"));

    UnattendProfile fixed;
    fixed.computerNameMode = ComputerNameMode::Fixed;
    fixed.computerName = QStringLiteral("WIMFORGE-01");
    fixed.applyComputerNameBehavior();
    test.check(fixed.validate().ok()
                   && fixed.value(SetupPass::Specialize, shell,
                                  {QStringLiteral("ComputerName")})
                       == QStringLiteral("WIMFORGE-01"),
               QStringLiteral("Fixed mode emits the validated literal name"));
    test.check(firstLogonCommandSettingCount(fixed) == 0,
               QStringLiteral("Fixed mode does not add a rename command"));

    UnattendProfile multiPass;
    multiPass.setValue(SetupPass::OfflineServicing, shell,
                       {QStringLiteral("ComputerName")}, QStringLiteral("OLD-NAME"));
    multiPass.computerNameMode = ComputerNameMode::Fixed;
    multiPass.computerName = QStringLiteral("NEW-NAME");
    multiPass.applyComputerNameBehavior();
    test.check(multiPass.validate().ok()
                   && multiPass.value(SetupPass::OfflineServicing, shell,
                                      {QStringLiteral("ComputerName")})
                       == QStringLiteral("NEW-NAME")
                   && multiPass.value(SetupPass::Specialize, shell,
                                      {QStringLiteral("ComputerName")})
                       == QStringLiteral("NEW-NAME"),
               QStringLiteral("mode changes keep offlineServicing and specialize names coherent"));
    multiPass.computerNameMode = ComputerNameMode::Random;
    multiPass.applyComputerNameBehavior();
    test.check(multiPass.validate().ok()
                   && multiPass.value(SetupPass::OfflineServicing, shell,
                                      {QStringLiteral("ComputerName")})
                       == QStringLiteral("*"),
               QStringLiteral("Random mode also clears a stale offline fixed name"));

    UnattendProfile prompt;
    prompt.computerNameMode = ComputerNameMode::Prompt;
    prompt.applyComputerNameBehavior();
    const UnattendSetting *promptCommand = findGeneratedCommandLeaf(
        prompt, QStringLiteral("CommandLine"));
    const UnattendSetting *promptInput = findGeneratedCommandLeaf(
        prompt, QStringLiteral("RequiresUserInput"));
    test.check(prompt.value(SetupPass::Specialize, shell,
                            {QStringLiteral("ComputerName")}) == QStringLiteral("*"),
               QStringLiteral("Prompt mode keeps the Microsoft ComputerName value valid"));
    test.check(promptCommand
                   && promptCommand->value == UnattendBuilder::computerNamePromptCommand()
                   && promptCommand->path.at(1).attributes.value(QStringLiteral("wcm:action"))
                       == QStringLiteral("add"),
               QStringLiteral("Prompt mode generates a WCM-added custom first-logon command"));
    test.check(promptInput && promptInput->value == QStringLiteral("true"),
               QStringLiteral("Prompt mode declares that its command requires user input"));

    const QString promptScript = UnattendBuilder::computerNamePromptCommand();
    test.check(promptScript.contains(QStringLiteral("InputBox"))
                   && promptScript.contains(QStringLiteral("Rename-Computer"))
                   && promptScript.contains(QStringLiteral("1-15"))
                   && promptScript.contains(QStringLiteral("-Restart"))
                   && !promptScript.contains(QStringLiteral("[Prompt]")),
               QStringLiteral("custom prompt command asks, validates, renames, and never emits [Prompt]"));
    QString error;
    const QByteArray promptXml = prompt.toXml(&error);
    test.check(!promptXml.isEmpty() && error.isEmpty()
                   && promptXml.contains(QByteArrayLiteral("wcm:action=\"add\""))
                   && promptXml.contains(QByteArrayLiteral("<RequiresUserInput>true"))
                   && !promptXml.contains(QByteArrayLiteral("[Prompt]")),
               QStringLiteral("Prompt XML is schema-shaped and contains no editor token"));

    UnattendProfile serial;
    serial.computerNameMode = ComputerNameMode::SerialNumber;
    serial.serialPrefix = QStringLiteral("ai lab --");
    serial.applyComputerNameBehavior();
    const UnattendSetting *serialCommand = findGeneratedCommandLeaf(
        serial, QStringLiteral("CommandLine"));
    const UnattendSetting *serialInput = findGeneratedCommandLeaf(
        serial, QStringLiteral("RequiresUserInput"));
    test.check(serialCommand
                   && serialCommand->value.contains(QStringLiteral("Get-CimInstance Win32_BIOS"))
                   && serialCommand->value.contains(QStringLiteral("$n=('AILAB-'+$s).Trim('-')"))
                   && serialCommand->value.contains(QStringLiteral("$n.Length -gt 15"))
                   && serialCommand->value.contains(QStringLiteral("Rename-Computer"))
                   && !serialCommand->value.contains(QStringLiteral("%SERIAL%")),
               QStringLiteral("Serial mode sanitizes the prefix and creates a bounded runtime command"));
    test.check(serialInput && serialInput->value == QStringLiteral("false"),
               QStringLiteral("Serial mode declares that it needs no user input"));
    test.check(serial.value(SetupPass::Specialize, shell,
                            {QStringLiteral("ComputerName")}) == QStringLiteral("*"),
               QStringLiteral("Serial mode does not put %SERIAL% into Microsoft ComputerName"));
    const QByteArray serialXml = serial.toXml(&error);
    test.check(!serialXml.isEmpty() && !serialXml.contains(QByteArrayLiteral("%SERIAL%")),
               QStringLiteral("Serial XML contains no NTLite-only token"));

    prompt.computerNameMode = ComputerNameMode::Fixed;
    prompt.computerName = QStringLiteral("MODE-SWITCH");
    prompt.applyComputerNameBehavior();
    test.check(prompt.value(SetupPass::Specialize, shell,
                            {QStringLiteral("ComputerName")}) == QStringLiteral("MODE-SWITCH")
                   && firstLogonCommandSettingCount(prompt) == 0,
               QStringLiteral("switching Prompt to Fixed removes the generated prompt command"));

    serial.computerNameMode = ComputerNameMode::Random;
    serial.applyComputerNameBehavior();
    test.check(serial.value(SetupPass::Specialize, shell,
                            {QStringLiteral("ComputerName")}) == QStringLiteral("*")
                   && firstLogonCommandSettingCount(serial) == 0,
               QStringLiteral("switching Serial to Random removes the generated serial command"));
}

void testGeneratedCommandIsolation(TestRun &test, const QString &temporaryRoot)
{
    const QString unrelatedDescription = QStringLiteral("Keep this deployment command");
    const QString unrelatedCommand = QStringLiteral("cmd.exe /c echo unrelated-command");

    UnattendProfile profile;
    profile.applyComputerNameBehavior();
    appendFirstLogonCommand(profile, QStringLiteral("7"), unrelatedDescription,
                            unrelatedCommand, QStringLiteral("false"));

    profile.computerNameMode = ComputerNameMode::Prompt;
    profile.applyComputerNameBehavior();
    test.check(firstLogonCommandSettingCount(profile) == 8
                   && hasFirstLogonLeafValue(profile, QStringLiteral("Description"),
                                             unrelatedDescription)
                   && hasFirstLogonLeafValue(profile, QStringLiteral("CommandLine"),
                                             unrelatedCommand)
                   && hasFirstLogonLeafValue(
                       profile, QStringLiteral("Description"),
                       QStringLiteral("WimForge computer-name prompt")),
               QStringLiteral("Prompt mode remains isolated from an unrelated first-logon command"));

    QString error;
    const QByteArray promptXml = profile.toXml(&error);
    test.check(!promptXml.isEmpty() && error.isEmpty()
                   && promptXml.count(QByteArrayLiteral("<SynchronousCommand")) == 2
                   && promptXml.contains(unrelatedDescription.toUtf8())
                   && promptXml.contains(QByteArrayLiteral("WimForge computer-name prompt"))
                   && !promptXml.contains(QByteArrayLiteral("_wimforgeInternalIdentity")),
               QStringLiteral("XML renders generated and unrelated commands as distinct clean siblings"));

    const QString promptXmlPath = QDir(temporaryRoot).filePath(
        QStringLiteral("computer-name/two-first-logon-commands.xml"));
    test.check(writeFile(promptXmlPath, promptXml),
               QStringLiteral("multi-command XML fixture is writable"));
    const auto imported = UnattendProfile::importXml(promptXmlPath, &error);
    test.check(imported && firstLogonCommandSettingCount(*imported) == 8
                   && hasFirstLogonLeafValue(*imported, QStringLiteral("Description"),
                                             unrelatedDescription),
               QStringLiteral("XML import keeps indistinguishable-at-schema-level command siblings: %1")
                   .arg(error));
    if (imported) {
        UnattendProfile fixedAfterImport = *imported;
        fixedAfterImport.computerNameMode = ComputerNameMode::Fixed;
        fixedAfterImport.computerName = QStringLiteral("AFTER-IMPORT");
        fixedAfterImport.applyComputerNameBehavior();
        const QByteArray fixedXml = fixedAfterImport.toXml(&error);
        test.check(fixedAfterImport.validate().ok()
                       && firstLogonCommandSettingCount(fixedAfterImport) == 4
                       && hasFirstLogonLeafValue(
                           fixedAfterImport, QStringLiteral("Description"),
                           unrelatedDescription)
                       && !hasFirstLogonLeafValue(
                           fixedAfterImport, QStringLiteral("Description"),
                           QStringLiteral("WimForge computer-name prompt"))
                       && fixedXml.count(QByteArrayLiteral("<SynchronousCommand")) == 1
                       && fixedXml.contains(unrelatedDescription.toUtf8()),
                   QStringLiteral("switching imported Prompt XML to Fixed removes only WimForge's command"));
    }

    profile.computerNameMode = ComputerNameMode::SerialNumber;
    profile.serialPrefix = QStringLiteral("LAB");
    profile.applyComputerNameBehavior();
    test.check(firstLogonCommandSettingCount(profile) == 8
                   && hasFirstLogonLeafValue(profile, QStringLiteral("Description"),
                                             unrelatedDescription)
                   && hasFirstLogonLeafValue(
                       profile, QStringLiteral("Description"),
                       QStringLiteral("WimForge serial-number computer name"))
                   && !hasFirstLogonLeafValue(
                       profile, QStringLiteral("Description"),
                       QStringLiteral("WimForge computer-name prompt")),
               QStringLiteral("Serial mode replaces only WimForge's generated command"));

    profile.computerNameMode = ComputerNameMode::Fixed;
    profile.computerName = QStringLiteral("FINAL-PC");
    profile.applyComputerNameBehavior();
    const QByteArray fixedXml = profile.toXml(&error);
    test.check(profile.validate().ok()
                   && firstLogonCommandSettingCount(profile) == 4
                   && hasFirstLogonLeafValue(profile, QStringLiteral("Description"),
                                             unrelatedDescription)
                   && fixedXml.count(QByteArrayLiteral("<SynchronousCommand")) == 1
                   && fixedXml.contains(unrelatedCommand.toUtf8()),
               QStringLiteral("Fixed mode removes generated naming while preserving unrelated setup"));
}

void testTemplates(TestRun &test)
{
    const QString shell = QStringLiteral("Microsoft-Windows-Shell-Setup");
    const UnattendProfile full = UnattendBuilder::fullAutomationTemplate();
    test.check(full.name == QStringLiteral("Full automation")
                   && full.description.contains(QStringLiteral("Windows PE"))
                   && full.copyToMediaRoot,
               QStringLiteral("full automation template has identity and media placement"));
    test.check(full.value(SetupPass::WindowsPE,
                          QStringLiteral("Microsoft-Windows-International-Core-WinPE"),
                          {QStringLiteral("SetupUILanguage"), QStringLiteral("UILanguage")})
                       == QStringLiteral("en-US")
                   && full.value(SetupPass::WindowsPE,
                                 QStringLiteral("Microsoft-Windows-International-Core-WinPE"),
                                 {QStringLiteral("UserLocale")}) == QStringLiteral("en-HK")
                   && full.value(SetupPass::WindowsPE,
                                 QStringLiteral("Microsoft-Windows-Setup"),
                                 {QStringLiteral("UserData"), QStringLiteral("AcceptEula")})
                       == QStringLiteral("true")
                   && full.value(SetupPass::WindowsPE,
                                 QStringLiteral("Microsoft-Windows-Setup"),
                                 {QStringLiteral("DynamicUpdate"), QStringLiteral("Enable")})
                       == QStringLiteral("true"),
               QStringLiteral("full template contains locale, EULA, and dynamic-update settings"));
    test.check(full.value(SetupPass::Specialize, shell,
                          {QStringLiteral("TimeZone")}) == QStringLiteral("China Standard Time")
                   && full.value(SetupPass::Specialize, shell,
                                 {QStringLiteral("ComputerName")}) == QStringLiteral("*")
                   && full.value(SetupPass::OobeSystem, shell,
                                 {QStringLiteral("OOBE"), QStringLiteral("HideEULAPage")})
                       == QStringLiteral("true")
                   && full.value(SetupPass::OobeSystem, shell,
                                 {QStringLiteral("OOBE"), QStringLiteral("ProtectYourPC")})
                       == QStringLiteral("3"),
               QStringLiteral("full template contains specialize and OOBE baseline settings"));
    QString error;
    test.check(full.validate().ok() && !full.toXml(&error).isEmpty(),
               QStringLiteral("full template validates and exports: %1").arg(error));

    const UnattendProfile ai = UnattendBuilder::aiDevelopmentTemplate();
    const UnattendSetting *aiCommand = findGeneratedCommandLeaf(
        ai, QStringLiteral("CommandLine"));
    test.check(ai.name == QStringLiteral("AI development workstation")
                   && ai.computerNameMode == ComputerNameMode::SerialNumber
                   && ai.serialPrefix == QStringLiteral("AI")
                   && ai.metadata.value(QStringLiteral("packageTemplate")).toString()
                       == QStringLiteral("ai-development"),
               QStringLiteral("AI development template selects its package and serial-name modes"));
    test.check(aiCommand && aiCommand->value.contains(QStringLiteral("$n=('AI-'+$s)")),
               QStringLiteral("AI development template applies its AI serial prefix"));
    test.check(ai.validate().ok() && !ai.toXml(&error).isEmpty(),
               QStringLiteral("AI development template validates and exports: %1").arg(error));
}

void testGvlkCatalog(TestRun &test)
{
    const QList<ProductKeyEntry> catalog = UnattendBuilder::microsoftPublishedGvlks();
    test.check(catalog.size() >= 8,
               QStringLiteral("the curated catalog contains the baseline Microsoft client GVLKs"));
    const QRegularExpression keyPattern(
        QStringLiteral("^[A-Z0-9]{5}(?:-[A-Z0-9]{5}){4}$"));
    QSet<QString> editions;
    QSet<QString> keys;
    for (const ProductKeyEntry &entry : catalog) {
        test.check(!entry.edition.isEmpty() && !editions.contains(entry.edition),
                   QStringLiteral("GVLK edition labels are present and unique"));
        test.check(keyPattern.match(entry.key).hasMatch() && !keys.contains(entry.key),
                   QStringLiteral("GVLK values are formatted and unique for %1").arg(entry.edition));
        test.check(entry.channel == QStringLiteral("GVLK/KMS client"),
                   QStringLiteral("%1 is explicitly identified as a KMS client key").arg(entry.edition));
        test.check(entry.documentationUrl
                       == QStringLiteral("https://learn.microsoft.com/windows-server/get-started/kms-client-activation-keys"),
                   QStringLiteral("%1 links to Microsoft's GVLK publication").arg(entry.edition));
        test.check(entry.licensingNotice.contains(QStringLiteral("does not grant a license"),
                                                  Qt::CaseInsensitive)
                       && entry.licensingNotice.contains(QStringLiteral("activate Windows"),
                                                         Qt::CaseInsensitive)
                       && entry.licensingNotice.contains(QStringLiteral("properly licensed"),
                                                         Qt::CaseInsensitive)
                       && entry.licensingNotice.contains(QStringLiteral("authorized KMS/ADBA"),
                                                         Qt::CaseInsensitive),
                   QStringLiteral("%1 carries the full non-licensing warning").arg(entry.edition));
        editions.insert(entry.edition);
        keys.insert(entry.key);
    }

    QMap<QString, QString> expected{
        {QStringLiteral("Windows 11/10 Pro"),
         QStringLiteral("W269N-WFGWX-YVC9B-4J6C9-T83GX")},
        {QStringLiteral("Windows 11/10 Pro N"),
         QStringLiteral("MH37W-N47XK-V7XM9-C7227-GCQG9")},
        {QStringLiteral("Windows 11/10 Pro for Workstations"),
         QStringLiteral("NRG8B-VKK3Q-CXVCJ-9G2XF-6Q84J")},
        {QStringLiteral("Windows 11/10 Pro Education"),
         QStringLiteral("6TP4R-GNPTD-KYYHQ-7B7DP-J447Y")},
        {QStringLiteral("Windows 11/10 Education"),
         QStringLiteral("NW6C2-QMPVW-D7KKK-3GKT6-VCFB2")},
        {QStringLiteral("Windows 11/10 Education N"),
         QStringLiteral("2WH4N-8QGBV-H22JP-CT43Q-MDWWJ")},
        {QStringLiteral("Windows 11/10 Enterprise"),
         QStringLiteral("NPPR9-FWDCX-D2C8J-H872K-2YT43")},
        {QStringLiteral("Windows 11/10 Enterprise N"),
         QStringLiteral("DPH2V-TTNVB-4X9Q3-TJR4H-KHJW4")},
    };
    for (const ProductKeyEntry &entry : catalog)
        if (expected.value(entry.edition) == entry.key)
            expected.remove(entry.edition);
    test.check(expected.isEmpty(),
               QStringLiteral("catalog contains every expected baseline Pro, Education, and Enterprise GVLK"));
}

} // namespace

void testNarratorAutostart(TestRun &test)
{
    UnattendProfile profile;
    test.check(!profile.narratorAutostartEnabled(),
               QStringLiteral("Narrator autostart is off by default"));

    profile.setNarratorAutostart(true);
    test.check(profile.narratorAutostartEnabled()
                   && firstLogonCommandSettingCount(profile) == 4
                   && hasFirstLogonLeafValue(profile, QStringLiteral("Description"),
                                             QStringLiteral("WimForge Narrator autostart"))
                   && hasFirstLogonLeafValue(profile, QStringLiteral("CommandLine"),
                                             UnattendBuilder::narratorAutostartCommand()),
               QStringLiteral("enabling Narrator adds one complete first-logon command"));

    profile.setNarratorAutostart(true);
    test.check(firstLogonCommandSettingCount(profile) == 4,
               QStringLiteral("enabling Narrator again does not duplicate the command"));

    QString error;
    const auto restored = UnattendProfile::fromJson(profile.toJson(), &error);
    test.check(restored && restored->narratorAutostartEnabled(),
               QStringLiteral("Narrator autostart survives a JSON round-trip: %1").arg(error));

    const QByteArray xml = profile.toXml(&error);
    test.check(!xml.isEmpty() && error.isEmpty()
                   && xml.contains(QByteArrayLiteral("WimForge Narrator autostart"))
                   && xml.contains(QByteArrayLiteral("Accessibility"))
                   && !xml.contains(QByteArrayLiteral("_wimforgeInternalIdentity")),
               QStringLiteral("Narrator command exports to a clean answer-file XML"));

    UnattendProfile mixed;
    mixed.computerNameMode = ComputerNameMode::Prompt;
    mixed.applyComputerNameBehavior();
    mixed.setNarratorAutostart(true);
    test.check(mixed.narratorAutostartEnabled()
                   && firstLogonCommandSettingCount(mixed) == 8,
               QStringLiteral("Narrator and computer-name commands coexist"));

    mixed.setNarratorAutostart(false);
    test.check(!mixed.narratorAutostartEnabled()
                   && firstLogonCommandSettingCount(mixed) == 4
                   && hasFirstLogonLeafValue(mixed, QStringLiteral("Description"),
                                             QStringLiteral("WimForge computer-name prompt")),
               QStringLiteral("disabling Narrator leaves the computer-name command intact"));
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeUnattendBuilderTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    testPasses(test, temporary.path());
    testJsonAndXmlPreservation(test, temporary.path());
    testComputerNameJsonAndXmlCoherence(test, temporary.path());
    testValidationAndNamingRules(test, temporary.path());
    testComputerNameModes(test);
    testGeneratedCommandIsolation(test, temporary.path());
    testNarratorAutostart(test);
    testTemplates(test);
    testGvlkCatalog(test);
    return test.result();
}
