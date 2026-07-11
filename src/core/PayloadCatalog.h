#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace wimforge {

enum class ServicingPayloadKind
{
    Driver,
    Update,
};

struct ServicingPayloadEntry
{
    QString path;
    QString title;
    // detail remains the directly displayable bilingual value used by the
    // current QML model.  The language-specific values let future callers
    // select one side without parsing presentation text.
    QString detail;
    QString detailEnglish;
    QString detailCantonese;
    QString extension;
    QString knowledgeBaseId;
    QString provider;
    QString driverClass;
    QString driverVersion;
    QString catalogFile;
    qint64 sizeBytes = 0;
    int containedFileCount = 0;
    bool exists = false;
    bool directory = false;
    bool supported = false;
};

class PayloadCatalog
{
public:
    [[nodiscard]] static ServicingPayloadEntry inspect(
        const QString &path,
        ServicingPayloadKind kind);
    [[nodiscard]] static QList<ServicingPayloadEntry> inspectAll(
        const QStringList &paths,
        ServicingPayloadKind kind);
    [[nodiscard]] static QStringList discoverFiles(
        const QString &directory,
        ServicingPayloadKind kind);
    [[nodiscard]] static bool isSupportedFile(
        const QString &path,
        ServicingPayloadKind kind);
};

} // namespace wimforge
