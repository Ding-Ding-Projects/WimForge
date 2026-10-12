#pragma once

#include <QDir>
#include <QString>
#include <QUrl>

namespace wimforge {

// File dialogs return URLs; path fields show the platform's native separators.
// Keep the existing non-file fallback separate from local path conversion.
[[nodiscard]] inline QString pathFromFileDialogUrl(const QUrl &url)
{
    if (url.isLocalFile())
        return QDir::toNativeSeparators(QDir::cleanPath(url.toLocalFile()));
    return QDir::cleanPath(url.toString(QUrl::PreferLocalFile));
}

} // namespace wimforge
