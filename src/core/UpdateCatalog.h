#pragma once

#include <QList>
#include <QString>
#include <QUrl>

namespace wimforge {

// One row from a Microsoft Update Catalog search. All fields come straight from
// the catalog result table; sizeBytes is the exact byte count the catalog
// publishes alongside the human-readable size.
struct UpdateCatalogEntry
{
    QString updateId;
    QString title;
    QString product;
    QString classification;
    QString lastUpdated;
    QString version;
    QString sizeText;
    qint64 sizeBytes = 0;
};

// Pure helpers for talking to the Microsoft Update Catalog entirely inside
// WimForge, without launching an external browser. The parsing functions are
// deterministic and unit-tested; the network calls live in AppController.
class UpdateCatalog
{
public:
    [[nodiscard]] static QUrl searchUrl(const QString &query);
    [[nodiscard]] static QUrl downloadDialogUrl();
    // application/x-www-form-urlencoded body for DownloadDialog.aspx. Returns an
    // empty array body when the update id is not a well-formed GUID.
    [[nodiscard]] static QByteArray downloadDialogBody(const QString &updateId);

    [[nodiscard]] static QList<UpdateCatalogEntry> parseSearchResults(const QString &html);
    [[nodiscard]] static QStringList parseDownloadUrls(const QString &downloadDialogResponse);

    // Only genuine Microsoft update-distribution hosts are ever fetched, so a
    // tampered catalog response can never redirect the download elsewhere.
    [[nodiscard]] static bool isTrustedDownloadUrl(const QUrl &url);
    [[nodiscard]] static bool isValidUpdateId(const QString &updateId);
};

} // namespace wimforge
