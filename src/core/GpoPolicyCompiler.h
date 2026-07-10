#pragma once

#include "GpoCatalog.h"
#include "ProjectConfig.h"

#include <QList>
#include <QString>
#include <QVariantMap>

namespace wimforge {

struct GpoPolicyCompilation
{
    QList<RegistryTweak> tweaks;
    QString ownerPrefix;
    QString error;

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

// ADML dropdownList.defaultItem is a zero-based item index, not the registry
// value of the item. Decimal controls with no ADML default use the same
// clamped zero value that the editor displays.
[[nodiscard]] QString gpoPresentationDefaultValue(const GpoElement &element);

// Qt Quick SpinBox is int-based. Keep qint64 bounds and unsigned DWORD ranges
// exact by switching those schemas to a numeric text field.
[[nodiscard]] bool gpoUsesNumericTextEditor(const GpoElement &element);

[[nodiscard]] GpoPolicyCompilation compileGpoPolicy(
    const GpoPolicy &policy,
    const QString &state,
    const QVariantMap &elementValues,
    const QList<RegistryTweak> &existingTweaks = {});

// Replaces the prior state owned by this policy and resolves exact registry
// value collisions while retaining unrelated/manual project tweaks.
void mergeGpoPolicyCompilation(ProjectConfig &project,
                               const GpoPolicyCompilation &compilation);

} // namespace wimforge
