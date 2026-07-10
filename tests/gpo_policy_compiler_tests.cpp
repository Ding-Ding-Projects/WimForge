#include "core/GpoPolicyCompiler.h"

#include <QCoreApplication>
#include <QTextStream>

#include <algorithm>
#include <limits>

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
            QTextStream(stdout) << "gpo_policy_compiler_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

GpoRegistryValue decimalValue(const QString &value)
{
    GpoRegistryValue result;
    result.kind = GpoValueKind::Decimal;
    result.value = value;
    return result;
}

GpoRegistryValue stringValue(const QString &value)
{
    GpoRegistryValue result;
    result.kind = GpoValueKind::String;
    result.value = value;
    return result;
}

const RegistryTweak *findTweak(const QList<RegistryTweak> &tweaks,
                               const QString &key,
                               const QString &name)
{
    const auto found = std::find_if(tweaks.cbegin(), tweaks.cend(),
        [&key, &name](const RegistryTweak &tweak) {
            return tweak.key.compare(key, Qt::CaseInsensitive) == 0
                && tweak.valueName.compare(name, Qt::CaseInsensitive) == 0
                && !tweak.deleteAllValues;
        });
    return found == tweaks.cend() ? nullptr : &*found;
}

GpoPolicy samplePolicy()
{
    GpoPolicy policy;
    policy.policyNamespace = QStringLiteral("WimForge.Tests");
    policy.id = QStringLiteral("CompletePolicy");
    policy.displayName = QStringLiteral("Complete policy");
    policy.policyClass = GpoPolicyClass::Both;
    policy.registryKey = QStringLiteral("Software\\Policies\\WimForge\\Complete");
    policy.registryValueName = QStringLiteral("Enabled");
    policy.enabledValue = decimalValue(QStringLiteral("1"));
    policy.disabledValue = decimalValue(QStringLiteral("0"));

    GpoElement number;
    number.kind = GpoElementKind::Decimal;
    number.id = QStringLiteral("Number");
    number.registryKey = policy.registryKey;
    number.registryValueName = QStringLiteral("Number");
    number.minimumValue = 0;
    number.maximumValue = 4'294'967'295LL;
    number.required = true;
    policy.elements.append(number);

    GpoElement multi;
    multi.kind = GpoElementKind::MultiText;
    multi.id = QStringLiteral("Multi");
    multi.registryKey = policy.registryKey;
    multi.registryValueName = QStringLiteral("Multi");
    policy.elements.append(multi);

    GpoElement expandable;
    expandable.kind = GpoElementKind::Text;
    expandable.id = QStringLiteral("Expandable");
    expandable.registryKey = policy.registryKey;
    expandable.registryValueName = QStringLiteral("Expandable");
    expandable.expandable = true;
    policy.elements.append(expandable);

    GpoElement sameNameList;
    sameNameList.kind = GpoElementKind::List;
    sameNameList.id = QStringLiteral("SameNameList");
    sameNameList.registryKey = QStringLiteral("Software\\Policies\\WimForge\\SameName");
    sameNameList.additive = true;
    policy.elements.append(sameNameList);

    GpoElement prefixedList;
    prefixedList.kind = GpoElementKind::List;
    prefixedList.id = QStringLiteral("PrefixedList");
    prefixedList.registryKey = QStringLiteral("Software\\Policies\\WimForge\\Prefixed");
    prefixedList.additive = true;
    prefixedList.valuePrefix = QStringLiteral("Server");
    policy.elements.append(prefixedList);

    GpoElement explicitList;
    explicitList.kind = GpoElementKind::List;
    explicitList.id = QStringLiteral("ExplicitList");
    explicitList.registryKey = QStringLiteral("Software\\Policies\\WimForge\\Explicit");
    explicitList.additive = true;
    explicitList.explicitValue = true;
    policy.elements.append(explicitList);

    GpoElement replacingList;
    replacingList.kind = GpoElementKind::List;
    replacingList.id = QStringLiteral("ReplacingList");
    replacingList.registryKey = QStringLiteral("Software\\Policies\\WimForge\\Replacing");
    replacingList.additive = false;
    replacingList.valuePrefix = QStringLiteral("");
    policy.elements.append(replacingList);
    return policy;
}

void testPresentationDefaults(TestRun &test)
{
    GpoElement enumeration;
    enumeration.kind = GpoElementKind::Enum;
    enumeration.presentationDefaultValue = QStringLiteral("1");
    GpoEnumOption first;
    first.displayName = QStringLiteral("First");
    first.value = stringValue(QStringLiteral("registry-first"));
    GpoEnumOption second;
    second.displayName = QStringLiteral("Second");
    second.value = stringValue(QStringLiteral("registry-second"));
    enumeration.options = {first, second};
    test.check(gpoPresentationDefaultValue(enumeration) == QStringLiteral("String registry-second"),
               QStringLiteral("ADML defaultItem is interpreted as an option index"));

    GpoElement decimal;
    decimal.kind = GpoElementKind::Decimal;
    decimal.minimumValue = 5;
    decimal.maximumValue = 99;
    test.check(gpoPresentationDefaultValue(decimal) == QStringLiteral("5"),
               QStringLiteral("empty decimal default is clamped exactly like the editor"));
    test.check(!gpoUsesNumericTextEditor(decimal),
               QStringLiteral("ordinary int-range decimal retains SpinBox"));
    decimal.maximumValue = 4'294'967'295LL;
    test.check(gpoUsesNumericTextEditor(decimal),
               QStringLiteral("unsigned DWORD range uses exact numeric text editing"));
    decimal.minimumValue = std::numeric_limits<qint64>::min();
    decimal.maximumValue = std::numeric_limits<qint64>::max();
    test.check(gpoUsesNumericTextEditor(decimal),
               QStringLiteral("full qint64 range uses exact numeric text editing"));
}

void testRegistrySemantics(TestRun &test)
{
    const GpoPolicy policy = samplePolicy();
    const QVariantMap values{
        {QStringLiteral("Number"), QStringLiteral("4294967295")},
        {QStringLiteral("Multi"), QStringLiteral("alpha\r\nbeta")},
        {QStringLiteral("Expandable"), QStringLiteral("%SystemRoot%\\WimForge")},
        {QStringLiteral("SameNameList"), QStringLiteral("alpha\nbeta")},
        {QStringLiteral("PrefixedList"), QStringLiteral("east\nwest")},
        {QStringLiteral("ExplicitList"), QStringLiteral("Primary=one\nSecondary=two=2")},
        {QStringLiteral("ReplacingList"), QStringLiteral("red\nblue")},
    };
    const GpoPolicyCompilation compiled = compileGpoPolicy(
        policy, QStringLiteral("machineEnabled"), values);
    test.check(compiled.ok(), QStringLiteral("complete policy compiles: %1").arg(compiled.error));

    const RegistryTweak *number = findTweak(compiled.tweaks, policy.registryKey,
                                             QStringLiteral("Number"));
    test.check(number && number->type == QStringLiteral("REG_DWORD")
                   && number->value == QStringLiteral("4294967295"),
               QStringLiteral("unsigned DWORD remains exact text through compilation"));
    const RegistryTweak *multi = findTweak(compiled.tweaks, policy.registryKey,
                                            QStringLiteral("Multi"));
    test.check(multi && multi->type == QStringLiteral("REG_MULTI_SZ")
                   && multi->value == QStringLiteral("alpha\nbeta"),
               QStringLiteral("multiText compiles to REG_MULTI_SZ with distinct entries"));
    const RegistryTweak *expandable = findTweak(compiled.tweaks, policy.registryKey,
                                                 QStringLiteral("Expandable"));
    test.check(expandable && expandable->type == QStringLiteral("REG_EXPAND_SZ"),
               QStringLiteral("expandable text compiles to REG_EXPAND_SZ"));

    const RegistryTweak *sameAlpha = findTweak(
        compiled.tweaks, QStringLiteral("Software\\Policies\\WimForge\\SameName"),
        QStringLiteral("alpha"));
    test.check(sameAlpha && sameAlpha->value == QStringLiteral("alpha"),
               QStringLiteral("default list semantics use the entry as both name and value"));
    const RegistryTweak *prefixOne = findTweak(
        compiled.tweaks, QStringLiteral("Software\\Policies\\WimForge\\Prefixed"),
        QStringLiteral("Server1"));
    const RegistryTweak *prefixTwo = findTweak(
        compiled.tweaks, QStringLiteral("Software\\Policies\\WimForge\\Prefixed"),
        QStringLiteral("Server2"));
    test.check(prefixOne && prefixOne->value == QStringLiteral("east")
                   && prefixTwo && prefixTwo->value == QStringLiteral("west"),
               QStringLiteral("valuePrefix list names are one-based"));
    const RegistryTweak *explicitValue = findTweak(
        compiled.tweaks, QStringLiteral("Software\\Policies\\WimForge\\Explicit"),
        QStringLiteral("Secondary"));
    test.check(explicitValue && explicitValue->value == QStringLiteral("two=2"),
               QStringLiteral("explicitValue splits only the first name=value delimiter"));

    const auto clear = std::find_if(compiled.tweaks.cbegin(), compiled.tweaks.cend(),
        [](const RegistryTweak &tweak) {
            return tweak.key == QStringLiteral("Software\\Policies\\WimForge\\Replacing")
                && tweak.deleteAllValues;
        });
    const RegistryTweak *firstNumbered = findTweak(
        compiled.tweaks, QStringLiteral("Software\\Policies\\WimForge\\Replacing"),
        QStringLiteral("1"));
    test.check(clear != compiled.tweaks.cend() && firstNumbered
                   && std::distance(compiled.tweaks.cbegin(), clear)
                        < std::distance(compiled.tweaks.cbegin(),
                                        std::find_if(compiled.tweaks.cbegin(), compiled.tweaks.cend(),
                                            [firstNumbered](const RegistryTweak &candidate) {
                                                return &candidate == firstNumbered;
                                            })),
               QStringLiteral("non-additive list clears values before writing one-based entries"));

    QVariantMap invalid = values;
    invalid.insert(QStringLiteral("ExplicitList"), QStringLiteral("missing delimiter"));
    const GpoPolicyCompilation rejected = compileGpoPolicy(
        policy, QStringLiteral("machineEnabled"), invalid);
    test.check(!rejected.ok() && rejected.error.contains(QStringLiteral("name=value")),
               QStringLiteral("explicitValue rejects entries without name=value"));
}

void testNotConfiguredOwnership(TestRun &test)
{
    const GpoPolicy policy = samplePolicy();
    const QVariantMap values{
        {QStringLiteral("Number"), QStringLiteral("8")},
        {QStringLiteral("Multi"), QStringLiteral("alpha\nbeta")},
        {QStringLiteral("Expandable"), QStringLiteral("%TEMP%")},
        {QStringLiteral("SameNameList"), QStringLiteral("owned")},
        {QStringLiteral("PrefixedList"), QStringLiteral("east\nwest")},
        {QStringLiteral("ExplicitList"), QStringLiteral("Primary=one")},
        {QStringLiteral("ReplacingList"), QStringLiteral("red\nblue")},
    };
    ProjectConfig project;
    project.registryTweaks.append(RegistryTweak{
        QStringLiteral("HKLM"), QStringLiteral("Software\\Unrelated"),
        QStringLiteral("Keep"), QStringLiteral("REG_SZ"), QStringLiteral("yes"), false});
    GpoPolicyCompilation enabled = compileGpoPolicy(
        policy, QStringLiteral("userEnabled"), values, project.registryTweaks);
    mergeGpoPolicyCompilation(project, enabled);
    test.check(std::all_of(project.registryTweaks.cbegin() + 1, project.registryTweaks.cend(),
                           [](const RegistryTweak &tweak) { return tweak.hive == QStringLiteral("HKCU"); }),
               QStringLiteral("Both policy honors explicit user scope"));

    const GpoPolicyCompilation machineEnabled = compileGpoPolicy(
        policy, QStringLiteral("machineEnabled"), values, project.registryTweaks);
    mergeGpoPolicyCompilation(project, machineEnabled);
    test.check(std::any_of(project.registryTweaks.cbegin(), project.registryTweaks.cend(),
                           [&enabled](const RegistryTweak &tweak) {
                               return tweak.hive == QStringLiteral("HKCU")
                                   && tweak.ownerId.startsWith(enabled.ownerPrefix)
                                   && !tweak.deleteValue && !tweak.deleteAllValues;
                           })
                   && std::any_of(project.registryTweaks.cbegin(), project.registryTweaks.cend(),
                                  [&machineEnabled](const RegistryTweak &tweak) {
                                      return tweak.hive == QStringLiteral("HKLM")
                                          && tweak.ownerId.startsWith(machineEnabled.ownerPrefix)
                                          && !tweak.deleteValue && !tweak.deleteAllValues;
                                  }),
               QStringLiteral("Both policy retains independent user and machine configurations"));

    const GpoPolicyCompilation removed = compileGpoPolicy(
        policy, QStringLiteral("userNotConfigured"), {}, project.registryTweaks);
    test.check(removed.ok(), QStringLiteral("NotConfigured compiles"));
    mergeGpoPolicyCompilation(project, removed);

    const RegistryTweak *unrelated = findTweak(
        project.registryTweaks, QStringLiteral("Software\\Unrelated"), QStringLiteral("Keep"));
    test.check(unrelated && !unrelated->deleteValue,
               QStringLiteral("NotConfigured retains unrelated manual registry changes"));
    const auto additiveDelete = std::find_if(
        project.registryTweaks.cbegin(), project.registryTweaks.cend(),
        [&removed](const RegistryTweak &tweak) {
            return tweak.key == QStringLiteral("Software\\Policies\\WimForge\\SameName")
                && tweak.valueName == QStringLiteral("owned") && tweak.deleteValue
                && tweak.ownerId.startsWith(removed.ownerPrefix);
        });
    test.check(additiveDelete != project.registryTweaks.cend(),
               QStringLiteral("NotConfigured deletes previously owned additive-list values"));
    const auto clear = std::find_if(project.registryTweaks.cbegin(), project.registryTweaks.cend(),
        [&removed](const RegistryTweak &tweak) {
            return tweak.key == QStringLiteral("Software\\Policies\\WimForge\\Replacing")
                && tweak.deleteAllValues && tweak.ownerId.startsWith(removed.ownerPrefix);
        });
    test.check(clear != project.registryTweaks.cend(),
               QStringLiteral("NotConfigured safely clears non-additive list values"));
    test.check(std::none_of(project.registryTweaks.cbegin(), project.registryTweaks.cend(),
                            [&removed](const RegistryTweak &tweak) {
                                return tweak.ownerId.startsWith(removed.ownerPrefix)
                                    && !tweak.deleteValue && !tweak.deleteAllValues;
                            }),
               QStringLiteral("NotConfigured leaves no policy-owned write silently active"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeGpoPolicyCompilerTests"));
    TestRun test;
    testPresentationDefaults(test);
    testRegistrySemantics(test);
    testNotConfiguredOwnership(test);
    return test.result();
}
