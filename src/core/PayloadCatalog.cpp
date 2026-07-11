#include "PayloadCatalog.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace wimforge {
namespace {

void setDetail(ServicingPayloadEntry *entry,
               const QString &english,
               const QString &cantonese)
{
    entry->detailEnglish = english;
    entry->detailCantonese = cantonese;
    entry->detail = QStringLiteral("%1 / %2").arg(english, cantonese);
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.front() == QLatin1Char('"')
        && value.back() == QLatin1Char('"')) {
        value = value.mid(1, value.size() - 2);
    }
    return value.trimmed();
}

QString decodeInf(const QByteArray &bytes)
{
    if (bytes.size() >= 2
        && static_cast<unsigned char>(bytes.at(0)) == 0xff
        && static_cast<unsigned char>(bytes.at(1)) == 0xfe) {
        const qsizetype units = (bytes.size() - 2) / 2;
        return QString::fromUtf16(
            reinterpret_cast<const char16_t *>(bytes.constData() + 2), units);
    }
    if (bytes.size() >= 3 && bytes.startsWith("\xef\xbb\xbf"))
        return QString::fromUtf8(bytes.sliced(3));
    return QString::fromLocal8Bit(bytes);
}

QString stripInfComment(const QString &line)
{
    bool quoted = false;
    for (qsizetype index = 0; index < line.size(); ++index) {
        if (line.at(index) == QLatin1Char('"'))
            quoted = !quoted;
        else if (!quoted && line.at(index) == QLatin1Char(';'))
            return line.left(index);
    }
    return line;
}

QString resolveInfToken(QString value, const QHash<QString, QString> &strings)
{
    value = unquote(value);
    static const QRegularExpression token(QStringLiteral("^%([^%]+)%$"));
    const QRegularExpressionMatch match = token.match(value);
    if (!match.hasMatch())
        return value;
    return strings.value(match.captured(1).trimmed().toLower(), value);
}

void inspectInf(const QString &path, ServicingPayloadEntry *entry)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    // Driver INFs are small text manifests. Bound the read so a mislabeled
    // file cannot stall the UI while the project catalog is refreshed.
    const QString text = decodeInf(file.read(8 * 1024 * 1024));
    QString section;
    QHash<QString, QString> version;
    QHash<QString, QString> strings;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")));
    for (const QString &rawLine : lines) {
        const QString line = stripInfComment(rawLine).trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed().toLower();
            continue;
        }
        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0)
            continue;
        const QString key = line.left(separator).trimmed().toLower();
        const QString value = line.mid(separator + 1).trimmed();
        if (section == QStringLiteral("version"))
            version.insert(key, value);
        else if (section == QStringLiteral("strings"))
            strings.insert(key, unquote(value));
    }

    entry->provider = resolveInfToken(version.value(QStringLiteral("provider")), strings);
    entry->driverClass = resolveInfToken(version.value(QStringLiteral("class")), strings);
    entry->driverVersion = resolveInfToken(version.value(QStringLiteral("driverver")), strings);
    entry->catalogFile = resolveInfToken(version.value(QStringLiteral("catalogfile")), strings);
    if (entry->catalogFile.isEmpty()) {
        for (auto it = version.cbegin(); it != version.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("catalogfile."))) {
                entry->catalogFile = resolveInfToken(it.value(), strings);
                break;
            }
        }
    }

    QStringList details;
    if (!entry->provider.isEmpty())
        details.append(entry->provider);
    if (!entry->driverClass.isEmpty())
        details.append(entry->driverClass);
    if (!entry->driverVersion.isEmpty())
        details.append(entry->driverVersion);
    if (details.isEmpty()) {
        setDetail(entry, QStringLiteral("INF driver package"),
                  QStringLiteral("INF 驅動套件"));
    } else {
        const QString metadata = details.join(QStringLiteral(" · "));
        setDetail(entry, QStringLiteral("Driver metadata: %1").arg(metadata),
                  QStringLiteral("驅動資料：%1").arg(metadata));
    }
}

QStringList nameFilters(ServicingPayloadKind kind)
{
    return kind == ServicingPayloadKind::Driver
        ? QStringList{QStringLiteral("*.inf")}
        : QStringList{QStringLiteral("*.msu"), QStringLiteral("*.cab")};
}

} // namespace

bool PayloadCatalog::isSupportedFile(const QString &path, ServicingPayloadKind kind)
{
    const QString extension = QFileInfo(path).suffix().toLower();
    if (kind == ServicingPayloadKind::Driver)
        return extension == QStringLiteral("inf");
    return extension == QStringLiteral("msu") || extension == QStringLiteral("cab");
}

QStringList PayloadCatalog::discoverFiles(const QString &directory,
                                          ServicingPayloadKind kind)
{
    const QFileInfo root(directory);
    if (!root.isDir())
        return {};

    QStringList result;
    QDirIterator iterator(root.absoluteFilePath(), nameFilters(kind), QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext())
        result.append(QFileInfo(iterator.next()).absoluteFilePath());
    std::sort(result.begin(), result.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    result.removeDuplicates();
    return result;
}

ServicingPayloadEntry PayloadCatalog::inspect(const QString &path,
                                              ServicingPayloadKind kind)
{
    ServicingPayloadEntry entry;
    const QFileInfo info(path);
    entry.path = QDir::toNativeSeparators(info.absoluteFilePath());
    entry.title = info.fileName().isEmpty() ? path : info.fileName();
    entry.extension = info.suffix().toLower();
    entry.exists = info.exists();
    entry.directory = info.isDir();
    entry.sizeBytes = info.isFile() ? info.size() : 0;

    if (!entry.exists) {
        setDetail(&entry, QStringLiteral("File or folder is missing"),
                  QStringLiteral("檔案或資料夾唔見咗"));
        return entry;
    }

    if (entry.directory) {
        const QStringList files = discoverFiles(info.absoluteFilePath(), kind);
        entry.containedFileCount = files.size();
        entry.supported = kind == ServicingPayloadKind::Driver && !files.isEmpty();
        if (kind == ServicingPayloadKind::Driver) {
            setDetail(&entry,
                      QStringLiteral("%1 INF package(s); DISM will add the folder recursively")
                          .arg(files.size()),
                      QStringLiteral("%1 個 INF 驅動套件；DISM 會遞迴加入成個資料夾")
                          .arg(files.size()));
        } else {
            setDetail(&entry,
                      QStringLiteral("Update folders must be expanded into individual CAB/MSU files"),
                      QStringLiteral("更新資料夾要先展開做獨立 CAB/MSU 檔案"));
        }
        return entry;
    }

    entry.supported = isSupportedFile(info.absoluteFilePath(), kind);
    if (!entry.supported) {
        if (kind == ServicingPayloadKind::Driver) {
            setDetail(&entry,
                      QStringLiteral("Unsupported driver payload; choose an INF file or driver folder"),
                      QStringLiteral("唔支援呢個驅動 payload；請揀 INF 檔案或驅動資料夾"));
        } else {
            setDetail(&entry,
                      QStringLiteral("Unsupported update payload; choose a CAB or MSU file"),
                      QStringLiteral("唔支援呢個更新 payload；請揀 CAB 或 MSU 檔案"));
        }
        return entry;
    }

    if (kind == ServicingPayloadKind::Driver) {
        inspectInf(info.absoluteFilePath(), &entry);
    } else {
        static const QRegularExpression kbPattern(
            QStringLiteral("\\b(KB\\d{6,8})\\b"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = kbPattern.match(info.completeBaseName());
        if (match.hasMatch())
            entry.knowledgeBaseId = match.captured(1).toUpper();
        const double mebibytes = static_cast<double>(entry.sizeBytes) / (1024.0 * 1024.0);
        QString english = QStringLiteral("%1 package · %2 MiB")
                              .arg(entry.extension.toUpper())
                              .arg(mebibytes, 0, 'f', 1);
        QString cantonese = QStringLiteral("%1 更新套件 · %2 MiB")
                                .arg(entry.extension.toUpper())
                                .arg(mebibytes, 0, 'f', 1);
        if (!entry.knowledgeBaseId.isEmpty()) {
            const QString prefix = entry.knowledgeBaseId + QStringLiteral(" · ");
            english.prepend(prefix);
            cantonese.prepend(prefix);
        }
        setDetail(&entry, english, cantonese);
    }
    return entry;
}

QList<ServicingPayloadEntry> PayloadCatalog::inspectAll(
    const QStringList &paths,
    ServicingPayloadKind kind)
{
    QList<ServicingPayloadEntry> result;
    result.reserve(paths.size());
    for (const QString &path : paths)
        result.append(inspect(path, kind));
    return result;
}

} // namespace wimforge
