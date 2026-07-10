#include "core/GpoCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
            QTextStream(stdout) << "gpo_catalog_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

bool createFixtures(const QString &root)
{
    const QByteArray windowsAdmx(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitions xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                   revision="1.0" schemaVersion="1.0">
  <policyNamespaces>
    <target prefix="windows" namespace="Microsoft.Policies.Windows"/>
  </policyNamespaces>
  <resources minRequiredRevision="1.0"/>
  <supportedOn>
    <definitions>
      <definition name="SUPPORTED_Test" displayName="$(string.SUPPORTED_Test)"/>
    </definitions>
  </supportedOn>
  <categories>
    <category name="Root" displayName="$(string.Root)"/>
    <category name="WindowsComponents" displayName="$(string.WindowsComponents)">
      <parentCategory ref="Root"/>
    </category>
  </categories>
</policyDefinitions>)XML");

    const QByteArray demoAdmx(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitions xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                   revision="1.0" schemaVersion="1.0">
  <policyNamespaces>
    <target prefix="demo" namespace="Example.Policies.Demo"/>
    <using prefix="windows" namespace="Microsoft.Policies.Windows"/>
  </policyNamespaces>
  <resources minRequiredRevision="1.0"/>
  <categories>
    <category name="DemoCategory" displayName="$(string.DemoCategory)">
      <parentCategory ref="windows:WindowsComponents"/>
    </category>
  </categories>
  <policies>
    <policy name="SamplePolicy" class="Both"
            displayName="$(string.SamplePolicy)"
            explainText="$(string.SamplePolicy_Help)"
            presentation="$(presentation.SamplePresentation)"
            key="Software\Policies\WimForge\Demo" valueName="Enabled">
      <parentCategory ref="DemoCategory"/>
      <supportedOn ref="windows:SUPPORTED_Test"/>
      <enabledValue><decimal value="1"/></enabledValue>
      <disabledValue><decimal value="0"/></disabledValue>
      <elements>
        <boolean id="FeatureSwitch" valueName="FeatureSwitch" required="true">
          <trueValue><decimal value="1"/></trueValue>
          <falseValue><decimal value="0"/></falseValue>
        </boolean>
        <enum id="Mode" valueName="Mode" required="true">
          <item displayName="$(string.ModeSafe)">
            <value><decimal value="1"/></value>
          </item>
          <item displayName="$(string.ModeFast)">
            <value><string value="fast"/></value>
            <valueList>
              <item key="Software\Policies\WimForge\Demo\Extra" valueName="Legacy">
                <value><delete/></value>
              </item>
            </valueList>
          </item>
        </enum>
        <decimal id="RetryCount" valueName="Retries" minValue="1" maxValue="12"
                 required="true" storeAsText="true"/>
        <text id="ToolPath" valueName="ToolPath" minLength="3" maxLength="128"
              required="true" expandable="true"/>
        <multiText id="Packages" valueName="Packages" required="true" maxStrings="25"/>
        <list id="Servers" key="Software\Policies\WimForge\Demo\Servers"
              valuePrefix="" additive="true" explicitValue="true"/>
      </elements>
    </policy>
    <policy name="MachineLists" class="Machine" displayName="$(string.MachineLists)"
            explainText="$(string.MachineLists_Help)" key="Software\Policies\WimForge\Lists">
      <parentCategory ref="DemoCategory"/>
      <supportedOn ref="windows:SUPPORTED_Test"/>
      <enabledList>
        <item valueName="First"><value><decimal value="10"/></value></item>
        <item key="Software\Policies\WimForge\Other" valueName="Second">
          <value><string value="yes"/></value>
        </item>
      </enabledList>
      <disabledList>
        <item valueName="First"><value><delete/></value></item>
      </disabledList>
    </policy>
    <policy name="UserPolicy" class="User" displayName="$(string.UserPolicy)"
            explainText="$(string.UserPolicy_Help)" key="Software\Policies\WimForge\User"
            valueName="On">
      <parentCategory ref="DemoCategory"/>
      <supportedOn ref="windows:SUPPORTED_Test"/>
      <enabledValue><decimal value="1"/></enabledValue>
      <disabledValue><delete/></disabledValue>
    </policy>
  </policies>
</policyDefinitions>)XML");

    const QByteArray windowsEnglish(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitionResources xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                           revision="1.0" schemaVersion="1.0">
  <resources><stringTable>
    <string id="Root">Administrative Templates</string>
    <string id="WindowsComponents">Windows Components</string>
    <string id="SUPPORTED_Test">Windows Test Edition or later</string>
  </stringTable></resources>
</policyDefinitionResources>)XML");

    const QByteArray windowsCantonese(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitionResources xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                           revision="1.0" schemaVersion="1.0">
  <resources><stringTable>
    <string id="Root">系統管理範本</string>
    <string id="WindowsComponents">Windows 元件</string>
    <string id="SUPPORTED_Test">Windows 測試版或更新版本</string>
  </stringTable></resources>
</policyDefinitionResources>)XML");

    const QByteArray demoEnglish(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitionResources xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                           revision="1.0" schemaVersion="1.0">
  <resources>
    <stringTable>
      <string id="DemoCategory">WimForge Demo Studio</string>
      <string id="SamplePolicy">Demo policy</string>
      <string id="SamplePolicy_Help">Controls the demo registry settings and every editor type.</string>
      <string id="ModeSafe">Safe mode</string>
      <string id="ModeFast">Fast mode</string>
      <string id="MachineLists">Machine registry list</string>
      <string id="MachineLists_Help">Writes several machine values.</string>
      <string id="UserPolicy">User-only policy</string>
      <string id="UserPolicy_Help">Writes one user value.</string>
    </stringTable>
    <presentationTable>
      <presentation id="SamplePresentation">
        <checkBox refId="FeatureSwitch" defaultChecked="true">Enable the useful switch</checkBox>
        <dropdownList refId="Mode" defaultItem="1" noSort="true">Operating mode</dropdownList>
        <decimalTextBox refId="RetryCount" defaultValue="3" spinStep="2">Retry count</decimalTextBox>
        <textBox refId="ToolPath"><label>Tool path</label><defaultValue>%ProgramFiles%</defaultValue></textBox>
        <multiTextBox refId="Packages">Package IDs</multiTextBox>
        <listBox refId="Servers">Server names</listBox>
        <text>Changes are staged in the local project history.</text>
      </presentation>
    </presentationTable>
  </resources>
</policyDefinitionResources>)XML");

    const QByteArray demoCantonese(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitionResources xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                           revision="1.0" schemaVersion="1.0">
  <resources>
    <stringTable>
      <string id="DemoCategory">WimForge 示範工場</string>
      <string id="SamplePolicy">示範設定，撳掣唔使驚</string>
      <string id="SamplePolicy_Help">控制示範登錄設定，改錯都可以返轉頭。</string>
      <string id="ModeSafe">穩陣模式</string>
      <string id="ModeFast">快到飛起模式</string>
      <string id="MachineLists">電腦登錄清單</string>
      <string id="MachineLists_Help">一次過寫入幾個電腦值。</string>
      <string id="UserPolicy">使用者專用設定</string>
      <string id="UserPolicy_Help">寫入一個使用者值。</string>
    </stringTable>
    <presentationTable>
      <presentation id="SamplePresentation">
        <checkBox refId="FeatureSwitch" defaultChecked="true">開著實用掣</checkBox>
        <dropdownList refId="Mode" defaultItem="1" noSort="true">運作模式</dropdownList>
        <decimalTextBox refId="RetryCount" defaultValue="3" spinStep="2">重試次數</decimalTextBox>
        <textBox refId="ToolPath"><label>工具路徑</label><defaultValue>%ProgramFiles%</defaultValue></textBox>
        <multiTextBox refId="Packages">套件 ID</multiTextBox>
        <listBox refId="Servers">伺服器名稱</listBox>
        <text>所有改動都放入本機專案歷史，唔怕手滑。</text>
      </presentation>
    </presentationTable>
  </resources>
</policyDefinitionResources>)XML");

    return writeFile(QDir(root).filePath(QStringLiteral("Windows.admx")), windowsAdmx)
        && writeFile(QDir(root).filePath(QStringLiteral("Demo.admx")), demoAdmx)
        && writeFile(QDir(root).filePath(QStringLiteral("en-US/Windows.adml")), windowsEnglish)
        && writeFile(QDir(root).filePath(QStringLiteral("en-US/Demo.adml")), demoEnglish)
        && writeFile(QDir(root).filePath(QStringLiteral("zh-HK/Windows.adml")), windowsCantonese)
        && writeFile(QDir(root).filePath(QStringLiteral("zh-HK/Demo.adml")), demoCantonese);
}

const GpoPolicy *findPolicy(const GpoCatalog &catalog, const QString &id)
{
    for (const GpoPolicy &policy : catalog.policies()) {
        if (policy.id == id)
            return &policy;
    }
    return nullptr;
}

const GpoElement *findElement(const GpoPolicy &policy, const QString &id)
{
    for (const GpoElement &element : policy.elements) {
        if (element.id == id)
            return &element;
    }
    return nullptr;
}

QString readUtf8(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeGpoCatalogTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    const QString definitions = QDir(temporary.path()).filePath(QStringLiteral("PolicyDefinitions"));
    test.check(createFixtures(definitions), QStringLiteral("ADMX and ADML fixtures are created"));

    GpoCatalog catalog;
    QString error;
    const bool loaded = catalog.loadFromDirectory(
        definitions, {QStringLiteral("en-US"), QStringLiteral("zh-HK")}, &error);
    test.check(loaded, QStringLiteral("catalog loads: %1").arg(error));
    if (!loaded)
        return test.result();

    test.check(catalog.policies().size() == 3, QStringLiteral("all fixture policies are parsed"));
    test.check(catalog.categories().size() == 3,
               QStringLiteral("categories from every ADMX are catalogued"));
    test.check(catalog.supportedDefinitions().size() == 1,
               QStringLiteral("supported-on definitions are catalogued"));
    test.check(catalog.locales() == QStringList{QStringLiteral("en-US"), QStringLiteral("zh-HK")},
               QStringLiteral("both requested locales are loaded"));

    GpoCatalog identifierOnlyCatalog;
    test.check(identifierOnlyCatalog.loadFromDirectory(
                   definitions, {QStringLiteral("fr-FR")}, &error),
               QStringLiteral("catalog remains usable when a requested ADML locale is absent: %1")
                   .arg(error));
    const GpoPolicy *identifierOnly = findPolicy(identifierOnlyCatalog,
                                                 QStringLiteral("SamplePolicy"));
    test.check(identifierOnly
                   && identifierOnly->categoryHierarchy
                          == QStringList{QStringLiteral("Root"),
                                         QStringLiteral("WindowsComponents"),
                                         QStringLiteral("DemoCategory")}
                   && !identifierOnlyCatalog.warnings().isEmpty(),
               QStringLiteral("identifier fallback retains the complete category hierarchy"));

    const GpoPolicy *sample = findPolicy(catalog, QStringLiteral("SamplePolicy"));
    test.check(sample != nullptr, QStringLiteral("sample policy exists"));
    if (sample) {
        test.check(sample->policyNamespace == QStringLiteral("Example.Policies.Demo"),
                   QStringLiteral("target namespace is retained"));
        test.check(sample->qualifiedId()
                       == QStringLiteral("Example.Policies.Demo:SamplePolicy"),
                   QStringLiteral("qualified policy ID combines namespace and ID"));
        test.check(sample->policyClass == GpoPolicyClass::Both,
                   QStringLiteral("Both policy class is parsed"));
        test.check(sample->displayName == QStringLiteral("Demo policy")
                       && sample->localizedDisplayNames.value(QStringLiteral("zh-HK"))
                              == QStringLiteral("示範設定，撳掣唔使驚"),
                   QStringLiteral("policy string resources resolve in both locales"));
        test.check(sample->explainText.contains(QStringLiteral("registry settings")),
                   QStringLiteral("explain text resource resolves"));
        test.check(sample->categoryHierarchy
                       == QStringList{QStringLiteral("Administrative Templates"),
                                      QStringLiteral("Windows Components"),
                                      QStringLiteral("WimForge Demo Studio")},
                   QStringLiteral("cross-namespace category hierarchy resolves root to leaf"));
        test.check(sample->localizedCategoryHierarchies.value(QStringLiteral("zh-HK")).last()
                       == QStringLiteral("WimForge 示範工場"),
                   QStringLiteral("category hierarchy is localized"));
        test.check(sample->supportedOn == QStringLiteral("Windows Test Edition or later")
                       && sample->localizedSupportedOn.value(QStringLiteral("zh-HK"))
                              == QStringLiteral("Windows 測試版或更新版本"),
                   QStringLiteral("external supported-on reference resolves in both locales"));
        test.check(sample->registryKey == QStringLiteral("Software\\Policies\\WimForge\\Demo")
                       && sample->registryValueName == QStringLiteral("Enabled"),
                   QStringLiteral("policy registry destination is retained"));
        test.check(sample->enabledValue.kind == GpoValueKind::Decimal
                       && sample->enabledValue.value == QStringLiteral("1")
                       && sample->disabledValue.value == QStringLiteral("0"),
                   QStringLiteral("enabled and disabled values are parsed"));
        test.check(sample->elements.size() == 6,
                   QStringLiteral("all six ADMX element kinds are parsed"));

        const GpoElement *toggle = findElement(*sample, QStringLiteral("FeatureSwitch"));
        test.check(toggle && toggle->kind == GpoElementKind::Boolean
                       && toggle->materialControl() == QStringLiteral("Switch")
                       && toggle->trueValue.value == QStringLiteral("1")
                       && toggle->presentationDefaultChecked,
                   QStringLiteral("boolean maps to Material Switch with values and default"));

        const GpoElement *mode = findElement(*sample, QStringLiteral("Mode"));
        test.check(mode && mode->kind == GpoElementKind::Enum
                       && mode->materialControl() == QStringLiteral("ComboBox")
                       && mode->options.size() == 2,
                   QStringLiteral("enum maps to Material ComboBox and keeps options"));
        if (mode && mode->options.size() == 2) {
            test.check(mode->options.at(1).localizedDisplayNames.value(QStringLiteral("zh-HK"))
                           == QStringLiteral("快到飛起模式")
                           && mode->options.at(1).value.kind == GpoValueKind::String
                           && mode->options.at(1).assignments.size() == 1
                           && mode->options.at(1).assignments.first().value.kind
                                  == GpoValueKind::Delete,
                       QStringLiteral("enum resources, string value, and valueList are retained"));
            test.check(mode->presentationDefaultValue == QStringLiteral("1")
                           && mode->localizedPresentationLabels.value(QStringLiteral("zh-HK"))
                                  == QStringLiteral("運作模式"),
                       QStringLiteral("dropdown presentation and default item resolve"));
        }

        const GpoElement *number = findElement(*sample, QStringLiteral("RetryCount"));
        test.check(number && number->materialControl() == QStringLiteral("SpinBox")
                       && number->minimumValue == 1 && number->maximumValue == 12
                       && number->required && number->storeAsText
                       && number->presentationSpinStep == 2,
                   QStringLiteral("decimal constraints map to a configured Material SpinBox"));

        const GpoElement *text = findElement(*sample, QStringLiteral("ToolPath"));
        test.check(text && text->materialControl() == QStringLiteral("TextField")
                       && text->minimumLength == 3 && text->maximumLength == 128
                       && text->expandable
                       && text->presentationDefaultValue == QStringLiteral("%ProgramFiles%"),
                   QStringLiteral("text constraints and nested textBox default are parsed"));

        const GpoElement *multi = findElement(*sample, QStringLiteral("Packages"));
        const GpoElement *list = findElement(*sample, QStringLiteral("Servers"));
        test.check(multi && multi->kind == GpoElementKind::MultiText
                       && multi->materialControl() == QStringLiteral("TextArea")
                       && multi->maximumStrings == 25,
                   QStringLiteral("multiText maps to Material TextArea with entry constraint"));
        test.check(list && list->kind == GpoElementKind::List
                       && list->materialControl() == QStringLiteral("ListEditor")
                       && list->additive && list->explicitValue
                       && !list->valuePrefix.isNull() && list->valuePrefix.isEmpty(),
                   QStringLiteral("list maps to editor and preserves empty valuePrefix flags"));
        test.check(sample->presentationElements.size() == 7
                       && sample->presentationElements.last().kind == GpoPresentationKind::Text
                       && sample->presentationElements.last().localizedLabels
                              .value(QStringLiteral("zh-HK")).contains(QStringLiteral("本機專案歷史")),
                   QStringLiteral("complete bilingual presentation table is retained"));
    }

    const GpoPolicy *machine = findPolicy(catalog, QStringLiteral("MachineLists"));
    test.check(machine && machine->policyClass == GpoPolicyClass::Machine
                   && machine->enabledAssignments.size() == 2
                   && machine->enabledAssignments.first().key
                          == QStringLiteral("Software\\Policies\\WimForge\\Lists")
                   && machine->disabledAssignments.first().value.kind == GpoValueKind::Delete,
               QStringLiteral("machine class and inherited enabled/disabled list keys are parsed"));
    const GpoPolicy *user = findPolicy(catalog, QStringLiteral("UserPolicy"));
    test.check(user && user->policyClass == GpoPolicyClass::User
                   && user->disabledValue.kind == GpoValueKind::Delete,
               QStringLiteral("user class and direct delete action are parsed"));

    error = QStringLiteral("stale");
    const QList<GpoPolicy> plain = catalog.search(
        QStringLiteral("demo editor"), GpoSearchMode::PlainText, &error);
    test.check(error.isEmpty() && plain.size() == 1
                   && plain.first().id == QStringLiteral("SamplePolicy"),
               QStringLiteral("plain search performs case-insensitive AND-token matching"));
    const QList<GpoPolicy> localized = catalog.search(
        QStringLiteral("撳掣"), GpoSearchMode::PlainText, &error);
    test.check(localized.size() == 1 && localized.first().id == QStringLiteral("SamplePolicy"),
               QStringLiteral("plain search includes secondary-locale resources"));

    const QList<GpoPolicy> regular = catalog.search(
        QStringLiteral("demo\\s+POLICY"), GpoSearchMode::RegularExpression, &error);
    test.check(error.isEmpty() && regular.size() == 1
                   && regular.first().id == QStringLiteral("SamplePolicy"),
               QStringLiteral("regex search is Unicode-aware and case-insensitive"));
    const QList<GpoPolicy> invalid = catalog.search(
        QStringLiteral("(["), GpoSearchMode::RegularExpression, &error);
    test.check(invalid.isEmpty()
                   && error.contains(QStringLiteral("Invalid regular expression"))
                   && error.contains(QStringLiteral("offset")),
               QStringLiteral("invalid regex returns no results and a readable error"));
    const QList<GpoPolicy> limitOverride = catalog.search(
        QStringLiteral("(*LIMIT_MATCH=999999999)demo"),
        GpoSearchMode::RegularExpression,
        &error);
    test.check(limitOverride.isEmpty() && error.contains(QStringLiteral("reserved")),
               QStringLiteral("regex queries cannot override resource-safety limits"));

    const QString markdownPath = QDir(temporary.path()).filePath(QStringLiteral("docs/gpo.md"));
    test.check(catalog.exportMarkdown(markdownPath,
                                      QStringLiteral("en-US"),
                                      QStringLiteral("zh-HK"),
                                      &error),
               QStringLiteral("bilingual Markdown export succeeds: %1").arg(error));
    const QString markdown = readUtf8(markdownPath);
    test.check(markdown.contains(QStringLiteral("Demo policy / 示範設定，撳掣唔使驚"))
                   && markdown.contains(QStringLiteral("Controls the demo registry settings"))
                   && markdown.contains(QStringLiteral("改錯都可以返轉頭"))
                   && markdown.contains(QStringLiteral("Software\\Policies\\WimForge\\Demo"))
                   && markdown.contains(QStringLiteral("Material control"))
                   && markdown.contains(QStringLiteral("Fast mode"))
                   && markdown.contains(QStringLiteral("快到飛起模式")),
               QStringLiteral("Markdown contains complete bilingual policy, registry, and UI docs"));

    test.check(GpoCatalog::defaultPolicyDefinitionsPath().isEmpty()
                   || GpoCatalog::defaultPolicyDefinitionsPath().endsWith(
                       QStringLiteral("PolicyDefinitions"), Qt::CaseInsensitive),
               QStringLiteral("installed catalog path follows WINDIR PolicyDefinitions"));

    // Opt-in integration check for developer/CI Windows machines. The normal
    // fixture suite remains portable and never depends on host policy files.
    if (application.arguments().contains(QStringLiteral("--installed"))) {
        GpoCatalog installed;
        error.clear();
        const bool installedLoaded = installed.loadInstalled(
            {QStringLiteral("en-US")}, &error);
        test.check(installedLoaded,
                   QStringLiteral("installed PolicyDefinitions catalog loads: %1").arg(error));
        if (installedLoaded) {
            test.check(installed.policies().size() > 100,
                       QStringLiteral("installed catalog contains a full policy set"));
            test.check(!installed.categories().isEmpty()
                           && !installed.supportedDefinitions().isEmpty(),
                       QStringLiteral("installed category and support tables are populated"));
            QTextStream(stdout) << "installed catalog: " << installed.policies().size()
                                << " policies, " << installed.categories().size()
                                << " categories, " << installed.warnings().size()
                                << " warnings\n";
        }
    }

    return test.result();
}
