#include "GpoPolicyCompiler.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>

namespace wimforge {
namespace {

QString policyOwnerPrefix(const GpoPolicy &policy, bool userScope)
{
    return QStringLiteral("gpo:%1:%2:")
        .arg(policy.qualifiedId(), userScope ? QStringLiteral("user") : QStringLiteral("machine"));
}

bool sameText(const QString &left, const QString &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

bool sameKey(const RegistryTweak &left, const RegistryTweak &right)
{
    return sameText(left.hive, right.hive) && sameText(left.key, right.key);
}

bool sameLocation(const RegistryTweak &left, const RegistryTweak &right)
{
    if (!sameKey(left, right))
        return false;
    if (left.deleteAllValues || right.deleteAllValues)
        return left.deleteAllValues && right.deleteAllValues;
    return sameText(left.valueName, right.valueName);
}

void appendReplacing(QList<RegistryTweak> &target, RegistryTweak tweak)
{
    target.erase(std::remove_if(target.begin(), target.end(),
                                [&tweak](const RegistryTweak &existing) {
                                    return sameLocation(existing, tweak);
                                }),
                 target.end());
    target.append(std::move(tweak));
}

RegistryTweak registryTweak(const QString &hive,
                            const QString &key,
                            const QString &name,
                            const QString &type,
                            const QString &value,
                            const QString &owner,
                            bool deleteValue = false,
                            bool deleteAllValues = false)
{
    RegistryTweak result;
    result.hive = hive;
    result.key = key;
    result.valueName = name;
    result.type = type;
    result.value = value;
    result.deleteValue = deleteValue;
    result.deleteAllValues = deleteAllValues;
    result.ownerId = owner;
    return result;
}

QStringList listEntries(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList result;
    for (const QString &line : normalized.split(QLatin1Char('\n'))) {
        const QString entry = line.trimmed();
        if (!entry.isEmpty())
            result.append(entry);
    }
    return result;
}

QString elementLabel(const GpoElement &element)
{
    return element.presentationLabel.isEmpty() ? element.id : element.presentationLabel;
}

bool hasReplacementWrite(const QList<RegistryTweak> &tweaks,
                         const RegistryTweak &existing)
{
    return std::any_of(tweaks.cbegin(), tweaks.cend(),
                       [&existing](const RegistryTweak &candidate) {
                           return !candidate.deleteValue && !candidate.deleteAllValues
                               && sameLocation(candidate, existing);
                       });
}

bool hasDelete(const QList<RegistryTweak> &tweaks, const RegistryTweak &existing)
{
    return std::any_of(tweaks.cbegin(), tweaks.cend(),
                       [&existing](const RegistryTweak &candidate) {
                           return candidate.deleteValue && sameLocation(candidate, existing);
                       });
}

bool clearedByList(const QList<RegistryTweak> &tweaks, const RegistryTweak &existing)
{
    return std::any_of(tweaks.cbegin(), tweaks.cend(),
                       [&existing](const RegistryTweak &candidate) {
                           return candidate.deleteAllValues && sameKey(candidate, existing);
                       });
}

} // namespace

QString gpoPresentationDefaultValue(const GpoElement &element)
{
    if (element.kind == GpoElementKind::Enum) {
        bool validIndex = false;
        const qlonglong index = element.presentationDefaultValue.trimmed().toLongLong(&validIndex);
        if (validIndex && index >= 0 && index < element.options.size())
            return element.options.at(static_cast<qsizetype>(index)).value.toDisplayString();
        return element.options.isEmpty() ? QString() : element.options.first().value.toDisplayString();
    }

    if (element.kind != GpoElementKind::Decimal
        || !element.presentationDefaultValue.trimmed().isEmpty()) {
        return element.presentationDefaultValue;
    }

    qint64 value = 0;
    if (element.minimumValue && value < *element.minimumValue)
        value = *element.minimumValue;
    if (element.maximumValue && value > *element.maximumValue)
        value = *element.maximumValue;
    return QString::number(value);
}

bool gpoUsesNumericTextEditor(const GpoElement &element)
{
    if (element.kind != GpoElementKind::Decimal)
        return false;
    constexpr qint64 minimumSpinValue = std::numeric_limits<int>::min();
    constexpr qint64 maximumSpinValue = std::numeric_limits<int>::max();
    return (element.minimumValue && *element.minimumValue < minimumSpinValue)
        || (element.maximumValue && *element.maximumValue > maximumSpinValue)
        || (element.presentationSpinStep
            && (*element.presentationSpinStep < minimumSpinValue
                || *element.presentationSpinStep > maximumSpinValue));
}

GpoPolicyCompilation compileGpoPolicy(const GpoPolicy &policy,
                                      const QString &state,
                                      const QVariantMap &elementValues,
                                      const QList<RegistryTweak> &existingTweaks)
{
    const bool userScope = state.contains(QStringLiteral("user"), Qt::CaseInsensitive)
        || policy.policyClass == GpoPolicyClass::User;
    const bool disable = state.contains(QStringLiteral("disable"), Qt::CaseInsensitive);
    const bool notConfigured = state.contains(QStringLiteral("not"), Qt::CaseInsensitive)
        || state.contains(QStringLiteral("remove"), Qt::CaseInsensitive);
    const QString hive = userScope ? QStringLiteral("HKCU") : QStringLiteral("HKLM");
    GpoPolicyCompilation result;
    result.ownerPrefix = policyOwnerPrefix(policy, userScope);
    const QString policyOwner = result.ownerPrefix + QStringLiteral("policy");

    if (!disable && !notConfigured) {
        for (const GpoElement &element : policy.elements) {
            const QString label = elementLabel(element);
            if (!elementValues.contains(element.id)) {
                if (element.required) {
                    result.error = QStringLiteral("%1 is required by the selected policy.").arg(label);
                    return result;
                }
                continue;
            }

            const QString text = elementValues.value(element.id).toString();
            if (element.kind == GpoElementKind::Enum) {
                const auto option = std::find_if(element.options.cbegin(), element.options.cend(),
                    [&text](const GpoEnumOption &item) {
                        return item.displayName == text || item.value.value == text
                            || item.value.toDisplayString() == text;
                    });
                if (option == element.options.cend()) {
                    result.error = QStringLiteral("%1 is not one of the policy's declared choices.")
                                       .arg(label);
                    return result;
                }
            } else if (element.kind == GpoElementKind::Decimal) {
                bool validNumber = false;
                const qint64 number = text.trimmed().toLongLong(&validNumber, 10);
                if (!validNumber
                    || (element.minimumValue && number < *element.minimumValue)
                    || (element.maximumValue && number > *element.maximumValue)) {
                    result.error = QStringLiteral("%1 is outside the policy's numeric bounds.")
                                       .arg(label);
                    return result;
                }
            } else if (element.kind != GpoElementKind::Boolean) {
                const QStringList entries = (element.kind == GpoElementKind::MultiText
                                             || element.kind == GpoElementKind::List)
                    ? listEntries(text) : QStringList{};
                if (element.required
                    && ((element.kind == GpoElementKind::MultiText
                         || element.kind == GpoElementKind::List)
                            ? entries.isEmpty() : text.trimmed().isEmpty())) {
                    result.error = QStringLiteral("%1 cannot be empty.").arg(label);
                    return result;
                }
                if ((element.minimumLength && text.size() < *element.minimumLength)
                    || (element.maximumLength && text.size() > *element.maximumLength)) {
                    result.error = QStringLiteral("%1 does not meet the policy's length constraints.")
                                       .arg(label);
                    return result;
                }
                if (element.maximumStrings && entries.size() > *element.maximumStrings) {
                    result.error = QStringLiteral("%1 allows at most %2 entries.")
                                       .arg(label).arg(*element.maximumStrings);
                    return result;
                }
                if (element.kind == GpoElementKind::List && element.explicitValue) {
                    for (const QString &entry : entries) {
                        const qsizetype equals = entry.indexOf(QLatin1Char('='));
                        if (equals <= 0 || entry.left(equals).trimmed().isEmpty()) {
                            result.error = QStringLiteral("%1 requires every entry in name=value form.")
                                               .arg(label);
                            return result;
                        }
                    }
                }
            }
        }
    }

    QList<RegistryTweak> additions;
    auto appendRaw = [&additions](RegistryTweak tweak) {
        if (tweak.key.trimmed().isEmpty())
            return;
        appendReplacing(additions, std::move(tweak));
    };
    auto appendValue = [&appendRaw, &hive](const QString &key,
                                           const QString &name,
                                           const GpoRegistryValue &value,
                                           const QString &owner) {
        if (name.isNull() || !value.isSet())
            return;
        if (value.kind == GpoValueKind::Delete) {
            appendRaw(registryTweak(hive, key, name, QStringLiteral("REG_SZ"), {}, owner, true));
            return;
        }
        appendRaw(registryTweak(hive, key, name,
                                value.kind == GpoValueKind::Decimal
                                    ? QStringLiteral("REG_DWORD") : QStringLiteral("REG_SZ"),
                                value.value, owner));
    };
    auto appendAssignments = [&appendValue](const QList<GpoRegistryAssignment> &assignments,
                                             const QString &owner) {
        for (const GpoRegistryAssignment &assignment : assignments)
            appendValue(assignment.key, assignment.valueName, assignment.value, owner);
    };
    auto appendDelete = [&appendRaw, &hive](const QString &key,
                                            const QString &name,
                                            const QString &owner) {
        if (!name.isNull())
            appendRaw(registryTweak(hive, key, name, QStringLiteral("REG_SZ"), {}, owner, true));
    };
    auto appendAssignmentDeletes = [&appendDelete](
                                       const QList<GpoRegistryAssignment> &assignments,
                                       const QString &owner) {
        for (const GpoRegistryAssignment &assignment : assignments)
            appendDelete(assignment.key, assignment.valueName, owner);
    };
    auto appendClear = [&appendRaw, &hive](const QString &key, const QString &owner) {
        appendRaw(registryTweak(hive, key, {}, QStringLiteral("REG_SZ"), {}, owner,
                                false, true));
    };

    if (notConfigured) {
        appendDelete(policy.registryKey, policy.registryValueName, policyOwner);
        appendAssignmentDeletes(policy.enabledAssignments, policyOwner);
        appendAssignmentDeletes(policy.disabledAssignments, policyOwner);
        for (const GpoElement &element : policy.elements) {
            const QString owner = result.ownerPrefix + QStringLiteral("element:") + element.id;
            const QString key = element.registryKey.isEmpty() ? policy.registryKey : element.registryKey;
            if (element.kind == GpoElementKind::List && !element.additive)
                appendClear(key, owner);
            else
                appendDelete(key, element.registryValueName, owner);
            appendAssignmentDeletes(element.trueAssignments, owner);
            appendAssignmentDeletes(element.falseAssignments, owner);
            for (const GpoEnumOption &option : element.options)
                appendAssignmentDeletes(option.assignments, owner);
        }
    } else {
        appendValue(policy.registryKey, policy.registryValueName,
                    disable ? policy.disabledValue : policy.enabledValue, policyOwner);
        appendAssignments(disable ? policy.disabledAssignments : policy.enabledAssignments,
                          policyOwner);
        if (!disable) {
            for (const GpoElement &element : policy.elements) {
                if (!elementValues.contains(element.id))
                    continue;
                const QVariant selected = elementValues.value(element.id);
                const QString key = element.registryKey.isEmpty() ? policy.registryKey : element.registryKey;
                const QString owner = result.ownerPrefix + QStringLiteral("element:") + element.id;
                if (element.kind == GpoElementKind::Boolean) {
                    appendValue(key, element.registryValueName,
                                selected.toBool() ? element.trueValue : element.falseValue, owner);
                    appendAssignments(selected.toBool() ? element.trueAssignments
                                                        : element.falseAssignments,
                                      owner);
                } else if (element.kind == GpoElementKind::Enum) {
                    const QString selectedText = selected.toString();
                    const auto option = std::find_if(element.options.cbegin(), element.options.cend(),
                        [&selectedText](const GpoEnumOption &item) {
                            return item.displayName == selectedText || item.value.value == selectedText
                                || item.value.toDisplayString() == selectedText;
                        });
                    if (option != element.options.cend()) {
                        appendValue(key, element.registryValueName, option->value, owner);
                        appendAssignments(option->assignments, owner);
                    }
                } else if (element.kind == GpoElementKind::List) {
                    const QStringList entries = listEntries(selected.toString());
                    if (!element.additive)
                        appendClear(key, owner);
                    for (qsizetype index = 0; index < entries.size(); ++index) {
                        const QString entry = entries.at(index);
                        QString name;
                        QString value;
                        if (element.explicitValue) {
                            const qsizetype equals = entry.indexOf(QLatin1Char('='));
                            name = entry.left(equals).trimmed();
                            value = entry.mid(equals + 1);
                        } else if (!element.valuePrefix.isNull()) {
                            name = element.valuePrefix + QString::number(index + 1);
                            value = entry;
                        } else {
                            name = entry;
                            value = entry;
                        }
                        appendRaw(registryTweak(hive, key, name, QStringLiteral("REG_SZ"), value,
                                                owner));
                    }
                } else {
                    QString type = QStringLiteral("REG_SZ");
                    QString value = selected.toString();
                    if (element.kind == GpoElementKind::Decimal && !element.storeAsText) {
                        type = QStringLiteral("REG_DWORD");
                        bool ok = false;
                        const qint64 number = value.trimmed().toLongLong(&ok, 10);
                        if (ok)
                            value = QString::number(number);
                    } else if (element.kind == GpoElementKind::MultiText) {
                        type = QStringLiteral("REG_MULTI_SZ");
                        value = listEntries(value).join(QLatin1Char('\n'));
                    } else if (element.expandable) {
                        type = QStringLiteral("REG_EXPAND_SZ");
                    }
                    if (!element.registryValueName.isNull())
                        appendRaw(registryTweak(hive, key, element.registryValueName, type, value,
                                                owner));
                }
            }
        }
    }

    QList<RegistryTweak> staleDeletes;
    for (const RegistryTweak &existing : existingTweaks) {
        if (!existing.ownerId.startsWith(result.ownerPrefix)
            || existing.deleteValue || existing.deleteAllValues
            || hasReplacementWrite(additions, existing)
            || hasDelete(additions, existing)
            || clearedByList(additions, existing)) {
            continue;
        }
        RegistryTweak deletion = existing;
        deletion.type = QStringLiteral("REG_SZ");
        deletion.value.clear();
        deletion.deleteValue = true;
        deletion.deleteAllValues = false;
        appendReplacing(staleDeletes, std::move(deletion));
    }

    result.tweaks = std::move(staleDeletes);
    for (RegistryTweak &addition : additions)
        appendReplacing(result.tweaks, std::move(addition));
    return result;
}

void mergeGpoPolicyCompilation(ProjectConfig &project,
                               const GpoPolicyCompilation &compilation)
{
    if (!compilation.ok())
        return;
    project.registryTweaks.erase(
        std::remove_if(project.registryTweaks.begin(), project.registryTweaks.end(),
                       [&compilation](const RegistryTweak &existing) {
                           return existing.ownerId.startsWith(compilation.ownerPrefix);
                       }),
        project.registryTweaks.end());

    for (const RegistryTweak &addition : compilation.tweaks) {
        project.registryTweaks.erase(
            std::remove_if(project.registryTweaks.begin(), project.registryTweaks.end(),
                           [&addition](const RegistryTweak &existing) {
                               return sameLocation(existing, addition);
                           }),
            project.registryTweaks.end());
        project.registryTweaks.append(addition);
    }
}

} // namespace wimforge
