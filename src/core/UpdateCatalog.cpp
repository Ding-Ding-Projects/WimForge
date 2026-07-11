#include "UpdateCatalog.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

namespace wimforge {
namespace {

const QString CatalogHost = QStringLiteral("www.catalog.update.microsoft.com");

QString stripTags(const QString &fragment)
{
    QString text = fragment;
    // Replace tags with a space so adjacent text (e.g. a display size and a
    // hidden byte-count span) never fuses; simplified() collapses the runs.
    text.replace(QRegularExpression(QStringLiteral("<[^>]*>")), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    return text.simplified();
}

// Text of the cell whose id is "<guid>_C<column>_R<row>", regardless of the
// row index the catalog assigned or the quote style it used.
QString cellText(const QString &row, const QString &guid, int column)
{
    const QRegularExpression expression(
        QStringLiteral("id=[\"']%1_C%2_R\\d+[\"'][^>]*>(.*?)</td>")
            .arg(QRegularExpression::escape(guid))
            .arg(column),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = expression.match(row);
    return match.hasMatch() ? stripTags(match.captured(1)) : QString();
}

} // namespace

QUrl UpdateCatalog::searchUrl(const QString &query)
{
    QUrl url(QStringLiteral("https://%1/Search.aspx").arg(CatalogHost));
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("q"), query.trimmed());
    url.setQuery(parameters);
    return url;
}

QUrl UpdateCatalog::downloadDialogUrl()
{
    return QUrl(QStringLiteral("https://%1/DownloadDialog.aspx").arg(CatalogHost));
}

bool UpdateCatalog::isValidUpdateId(const QString &updateId)
{
    static const QRegularExpression guid(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                       "[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return guid.match(updateId).hasMatch();
}

QByteArray UpdateCatalog::downloadDialogBody(const QString &updateId)
{
    if (!isValidUpdateId(updateId))
        return QByteArrayLiteral("updateIDs=%5B%5D");
    const QString json = QStringLiteral(
        "[{\"size\":0,\"languages\":\"\",\"uidInfo\":\"%1\",\"updateID\":\"%1\"}]")
        .arg(updateId);
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("updateIDs"), json);
    return form.toString(QUrl::FullyEncoded).toUtf8();
}

QList<UpdateCatalogEntry> UpdateCatalog::parseSearchResults(const QString &html)
{
    QList<UpdateCatalogEntry> results;
    QSet<QString> seen;
    // Each result is a <tr id="<guid>_R<n>"> row. Capture the guid, then read
    // the row's own slice of the document up to the next row or table end.
    const QRegularExpression rowStart(
        QStringLiteral("<tr[^>]*id=[\"']([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                       "[0-9a-fA-F]{4}-[0-9a-fA-F]{12})_R\\d+[\"']"));
    auto matches = rowStart.globalMatch(html);
    QList<QPair<QString, int>> starts;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        starts.append({match.captured(1), static_cast<int>(match.capturedStart())});
    }
    for (int index = 0; index < starts.size(); ++index) {
        const QString guid = starts.at(index).first;
        if (seen.contains(guid))
            continue;
        const int from = starts.at(index).second;
        const int to = index + 1 < starts.size() ? starts.at(index + 1).second : html.size();
        const QString row = html.mid(from, to - from);

        UpdateCatalogEntry entry;
        entry.updateId = guid;
        entry.title = cellText(row, guid, 1);
        entry.product = cellText(row, guid, 2);
        entry.classification = cellText(row, guid, 3);
        entry.lastUpdated = cellText(row, guid, 4);
        entry.version = cellText(row, guid, 5);
        const QString size = cellText(row, guid, 6);
        // The size cell renders "426.7 MB 447385650": a display size plus the
        // exact byte count. Keep both.
        const QRegularExpression sizePattern(QStringLiteral("^(.*?)\\s+(\\d+)\\s*$"));
        const QRegularExpressionMatch sizeMatch = sizePattern.match(size);
        if (sizeMatch.hasMatch()) {
            entry.sizeText = sizeMatch.captured(1).trimmed();
            entry.sizeBytes = sizeMatch.captured(2).toLongLong();
        } else {
            entry.sizeText = size;
        }
        if (entry.title.isEmpty())
            continue;
        seen.insert(guid);
        results.append(entry);
    }
    return results;
}

QStringList UpdateCatalog::parseDownloadUrls(const QString &downloadDialogResponse)
{
    QStringList urls;
    // The dialog response assigns each file URL like:
    //   downloadInformation[0].files[0].url = 'https://…/name.msu';
    const QRegularExpression pattern(
        QStringLiteral("downloadInformation\\[\\d+\\]\\.files\\[\\d+\\]\\.url\\s*=\\s*['\"]([^'\"]+)['\"]"));
    auto matches = pattern.globalMatch(downloadDialogResponse);
    while (matches.hasNext()) {
        const QUrl candidate(matches.next().captured(1));
        if (isTrustedDownloadUrl(candidate) && !urls.contains(candidate.toString()))
            urls.append(candidate.toString());
    }
    return urls;
}

bool UpdateCatalog::isTrustedDownloadUrl(const QUrl &url)
{
    if (url.scheme() != QStringLiteral("https"))
        return false;
    const QString host = url.host().toLower();
    static const QStringList trustedSuffixes{
        QStringLiteral(".download.windowsupdate.com"),
        QStringLiteral(".dl.delivery.mp.microsoft.com"),
        QStringLiteral(".delivery.mp.microsoft.com"),
        QStringLiteral(".download.microsoft.com"),
    };
    bool trustedHost = host == QStringLiteral("download.windowsupdate.com")
        || host == QStringLiteral("download.microsoft.com");
    for (const QString &suffix : trustedSuffixes)
        trustedHost = trustedHost || host.endsWith(suffix);
    if (!trustedHost)
        return false;
    const QString path = url.path().toLower();
    return path.endsWith(QStringLiteral(".msu")) || path.endsWith(QStringLiteral(".cab"));
}

} // namespace wimforge
