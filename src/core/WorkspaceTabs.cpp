#include "WorkspaceTabs.h"

#include "GitHistory.h"
#include "ProjectBundle.h"
#include "StructuredLogger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace wimforge {
namespace {

const QString StateFile = QStringLiteral("tabs.json");
const QString Format = QStringLiteral("org.wimforge.workspace-tabs");

QString bilingual(const QString &english, const QString &cantonese)
{
    return QStringLiteral("%1 / %2").arg(english, cantonese);
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool fail(QString *error, const QString &message)
{
    setError(error, message);
    return false;
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, bilingual(QStringLiteral("Could not create the tab repository folder."),
                                     QStringLiteral("建立唔到分頁資料庫資料夾。")));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, bilingual(QStringLiteral("Could not write tabs: %1").arg(file.errorString()),
                                     QStringLiteral("寫唔到分頁：%1").arg(file.errorString())));
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, bilingual(QStringLiteral("Could not save tabs atomically: %1").arg(file.errorString()),
                                     QStringLiteral("用原子方式儲存唔到分頁：%1").arg(file.errorString())));
    return true;
}

QString defaultTitleForPage(int page)
{
    static const QStringList titles{
        QStringLiteral("Overview"), QStringLiteral("Source & editions"),
        QStringLiteral("Customize"), QStringLiteral("Group Policy Studio"),
        QStringLiteral("Unattended Studio"), QStringLiteral("Package Studio"),
        QStringLiteral("WinForge Bridge"), QStringLiteral("Virtual Machine Lab"),
        QStringLiteral("Review & run"), QStringLiteral("History & recovery"),
        QStringLiteral("Settings"), QStringLiteral("Embedded terminal")};
    return page >= 0 && page < titles.size()
        ? titles.at(page) : QStringLiteral("Workspace");
}

QJsonObject makeTab(int page, const QString &title)
{
    return QJsonObject{
        {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("page"), page},
        {QStringLiteral("title"), title.trimmed().isEmpty() ? defaultTitleForPage(page) : title.trimmed()},
        {QStringLiteral("fontFamily"), QString()},
        {QStringLiteral("fontSize"), 13},
        {QStringLiteral("fontColor"), QString()},
        {QStringLiteral("bold"), false},
        {QStringLiteral("italic"), false},
        {QStringLiteral("strikeout"), false},
        // A tab keeps its own title only once the user renames it; otherwise
        // navigating the tab refreshes the title to the new page's name.
        {QStringLiteral("custom"), false},
    };
}

bool isLinkLike(const QFileInfo &info)
{
    if (info.isSymLink())
        return true;
#ifdef Q_OS_WIN
    if (info.isJunction())
        return true;
#endif
    return false;
}

bool isPathInside(const QString &childPath, const QString &parentPath)
{
    const QString child = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(childPath).absoluteFilePath()));
    QString parent = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(parentPath).absoluteFilePath()));
    if (child.compare(parent, Qt::CaseInsensitive) == 0)
        return true;
    if (!parent.endsWith(QLatin1Char('/')))
        parent.append(QLatin1Char('/'));
    return child.startsWith(parent, Qt::CaseInsensitive);
}

bool ensureSafeDirectory(const QString &path,
                         const QString &canonicalProject,
                         QString *error)
{
    QFileInfo info(path);
    if (isLinkLike(info))
        return fail(error, bilingual(
            QStringLiteral("Workspace tab paths cannot use links or junctions: %1")
                .arg(info.absoluteFilePath()),
            QStringLiteral("工作空間分頁路徑唔可以用連結或 junction：%1")
                .arg(info.absoluteFilePath())));
    if (info.exists() && !info.isDir())
        return fail(error, bilingual(
            QStringLiteral("Workspace tab path is not a directory: %1").arg(info.absoluteFilePath()),
            QStringLiteral("工作空間分頁路徑唔係資料夾：%1").arg(info.absoluteFilePath())));
    if (!info.exists() && !QDir().mkpath(info.absoluteFilePath()))
        return fail(error, bilingual(
            QStringLiteral("Could not create workspace tab directory: %1").arg(info.absoluteFilePath()),
            QStringLiteral("建立唔到工作空間分頁資料夾：%1").arg(info.absoluteFilePath())));
    info.refresh();
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !isPathInside(canonical, canonicalProject))
        return fail(error, bilingual(
            QStringLiteral("Workspace tab directory escapes the project: %1").arg(info.absoluteFilePath()),
            QStringLiteral("工作空間分頁資料夾走出工程範圍：%1").arg(info.absoluteFilePath())));
    return true;
}

bool ensureSafeRepositoryLocation(const QString &projectDirectory,
                                  const QString &repositoryPath,
                                  QString *error)
{
    const QFileInfo project(projectDirectory);
    const QString canonicalProject = project.canonicalFilePath();
    if (canonicalProject.isEmpty() || !project.isDir())
        return fail(error, bilingual(QStringLiteral("The project directory cannot be resolved safely."),
                                     QStringLiteral("安全噉解析唔到工程資料夾。")));
    const QString metadata = QDir(project.absoluteFilePath()).filePath(QStringLiteral(".wimforge"));
    if (!ensureSafeDirectory(metadata, canonicalProject, error)
        || !ensureSafeDirectory(repositoryPath, canonicalProject, error)) {
        return false;
    }

    const QFileInfo state(QDir(repositoryPath).filePath(StateFile));
    if (isLinkLike(state) || (state.exists() && !state.isFile()))
        return fail(error, bilingual(QStringLiteral("Workspace tab state must be an ordinary project-local file."),
                                     QStringLiteral("工作空間分頁狀態一定要係工程內普通檔案。")));
    const QFileInfo git(QDir(repositoryPath).filePath(QStringLiteral(".git")));
    if (isLinkLike(git) || (git.exists() && !git.isDir()))
        return fail(error, bilingual(QStringLiteral("Workspace tab .git must be a real project-local directory."),
                                     QStringLiteral("工作空間分頁 .git 一定要係工程內真實資料夾。")));
    if (git.exists()) {
        const QString canonicalGit = git.canonicalFilePath();
        const QString canonicalRepository = QFileInfo(repositoryPath).canonicalFilePath();
        if (canonicalGit.isEmpty() || canonicalRepository.isEmpty()
            || !isPathInside(canonicalGit, canonicalRepository)) {
            return fail(error, bilingual(QStringLiteral("Workspace tab Git metadata escapes its repository."),
                                         QStringLiteral("工作空間分頁 Git 資料走出佢個資料庫範圍。")));
        }
    }
    return true;
}

bool rejectLinksRecursively(const QString &directoryPath, QString *error)
{
    const QDir directory(directoryPath);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isLinkLike(entry))
            return fail(error, bilingual(
                QStringLiteral("Tab repositories cannot contain links or junctions: %1").arg(entry.absoluteFilePath()),
                QStringLiteral("分頁資料庫唔可以包含連結或 junction：%1").arg(entry.absoluteFilePath())));
        if (entry.isDir() && !rejectLinksRecursively(entry.absoluteFilePath(), error))
            return false;
    }
    return true;
}

bool removeControlFile(const QString &path, QString *error)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return true;
    if (isLinkLike(info) || !info.isFile())
        return fail(error, bilingual(
            QStringLiteral("Unsafe Git control path in tab repository: %1").arg(info.absoluteFilePath()),
            QStringLiteral("分頁資料庫有唔安全嘅 Git 控制路徑：%1").arg(info.absoluteFilePath())));
    if (!QFile::remove(info.absoluteFilePath()))
        return fail(error, bilingual(
            QStringLiteral("Could not neutralize imported Git control file: %1").arg(info.absoluteFilePath()),
            QStringLiteral("匯入嘅 Git 控制檔案整唔到失效：%1").arg(info.absoluteFilePath())));
    return true;
}

bool hardenTabRepository(const QString &repositoryPath, QString *error)
{
    const QFileInfo repository(repositoryPath);
    const QFileInfo git(QDir(repositoryPath).filePath(QStringLiteral(".git")));
    if (!repository.isDir() || isLinkLike(repository))
        return fail(error, bilingual(QStringLiteral("Workspace tab repository is not a safe directory."),
                                     QStringLiteral("工作空間分頁資料庫唔係安全資料夾。")));
    const QFileInfoList workingEntries = QDir(repositoryPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &entry : workingEntries) {
        if (isLinkLike(entry))
            return fail(error, bilingual(QStringLiteral("Workspace tab repository contains an unsafe link."),
                                         QStringLiteral("工作空間分頁資料庫有唔安全嘅連結。")));
        if (entry.fileName() == QStringLiteral(".git")) {
            if (!entry.isDir())
                return fail(error, bilingual(QStringLiteral("Workspace tab .git is not a directory."),
                                             QStringLiteral("工作空間分頁 .git 唔係資料夾。")));
        } else if (entry.fileName() != StateFile || !entry.isFile()) {
            return fail(error, bilingual(
                QStringLiteral("Workspace tab repositories may contain only tabs.json outside .git: %1")
                    .arg(entry.fileName()),
                QStringLiteral("工作空間分頁資料庫嘅 .git 外面只可以有 tabs.json：%1")
                    .arg(entry.fileName())));
        }
    }
    if (!git.exists())
        return true;
    if (!git.isDir() || isLinkLike(git))
        return fail(error, bilingual(QStringLiteral("Workspace tab .git must be a real directory."),
                                     QStringLiteral("工作空間分頁 .git 一定要係真實資料夾。")));
    if (!rejectLinksRecursively(git.absoluteFilePath(), error))
        return false;

    const QString gitPath = git.absoluteFilePath();
    const QStringList controlFiles{
        QStringLiteral("config"), QStringLiteral("config.lock"),
        QStringLiteral("config.worktree"), QStringLiteral("commondir"),
        QStringLiteral("gitdir"), QStringLiteral("index"),
        QStringLiteral("index.lock"), QStringLiteral("objects/info/alternates"),
        QStringLiteral("info/attributes"),
    };
    for (const QString &relative : controlFiles) {
        if (!removeControlFile(QDir(gitPath).filePath(relative), error))
            return false;
    }

    const QString hooksPath = QDir(gitPath).filePath(QStringLiteral("hooks"));
    const QFileInfo hooks(hooksPath);
    if (hooks.exists()) {
        if (!hooks.isDir() || isLinkLike(hooks)
            || !QDir(hooksPath).removeRecursively()) {
            return fail(error, bilingual(QStringLiteral("Could not neutralize imported Git hooks."),
                                         QStringLiteral("匯入嘅 Git hooks 整唔到失效。")));
        }
    }
    if (!QDir().mkpath(hooksPath))
        return fail(error, bilingual(QStringLiteral("Could not create the safe empty Git hooks directory."),
                                     QStringLiteral("建立唔到安全嘅空 Git hooks 資料夾。")));
    return true;
}

} // namespace

bool WorkspaceTabs::openProject(const QString &projectDirectory, QString *error)
{
    closeProject();
    const QFileInfo project(projectDirectory);
    if (!project.isAbsolute() || !project.isDir())
        return fail(error, bilingual(QStringLiteral("A valid project directory is required for workspace tabs."),
                                     QStringLiteral("工作空間分頁需要一個有效工程資料夾。")));
    m_projectDirectory = project.absoluteFilePath();
    m_repositoryPath = QDir(m_projectDirectory).filePath(QStringLiteral(".wimforge/tabs"));
    if (!ensureSafeRepositoryLocation(m_projectDirectory, m_repositoryPath, error)
        || !hardenTabRepository(m_repositoryPath, error)) {
        closeProject();
        return false;
    }
    const QString statePath = QDir(m_repositoryPath).filePath(StateFile);
    if (QFileInfo::exists(statePath)) {
        if (!loadState(statePath, error)) {
            closeProject();
            return false;
        }
        QString historyError;
        if (!GitHistory(m_repositoryPath, {StateFile}).initialize(&historyError)) {
            closeProject();
            return fail(error, historyError);
        }
        setError(error, {});
        return true;
    }
    m_tabs = {makeTab(0, QStringLiteral("Overview"))};
    m_activeIndex = 0;
    if (!save(bilingual(QStringLiteral("tabs: initialize project workspace"),
                        QStringLiteral("tabs：初始化工程工作空間")), error)) {
        closeProject();
        return false;
    }
    return true;
}

void WorkspaceTabs::closeProject()
{
    m_projectDirectory.clear();
    m_repositoryPath.clear();
    m_tabs.clear();
    m_activeIndex = -1;
    m_pendingCommitMessage.clear();
}

bool WorkspaceTabs::isOpen() const { return !m_repositoryPath.isEmpty(); }
QString WorkspaceTabs::repositoryPath() const { return m_repositoryPath; }
int WorkspaceTabs::activeIndex() const { return m_activeIndex; }

void WorkspaceTabs::setDeferredPersistence(bool deferred)
{
    m_deferredPersistence = deferred;
}

bool WorkspaceTabs::hasPendingPersistence() const
{
    return !m_pendingCommitMessage.isEmpty();
}

QString WorkspaceTabs::pendingCommitMessage() const
{
    return m_pendingCommitMessage;
}

QString WorkspaceTabs::takePendingCommitMessage()
{
    QString message = m_pendingCommitMessage;
    m_pendingCommitMessage.clear();
    return message;
}

bool WorkspaceTabs::flushPendingPersistence(QString *error)
{
    if (m_pendingCommitMessage.isEmpty()) {
        setError(error, {});
        return true;
    }
    const QString message = m_pendingCommitMessage;
    m_pendingCommitMessage.clear();
    const bool deferred = m_deferredPersistence;
    m_deferredPersistence = false;
    const bool saved = save(message, error);
    m_deferredPersistence = deferred;
    if (!saved)
        m_pendingCommitMessage = message;
    return saved;
}

QVariantList WorkspaceTabs::tabs() const
{
    QVariantList result;
    for (const QJsonObject &tab : m_tabs)
        result.append(tab.toVariantMap());
    return result;
}

bool WorkspaceTabs::validateAndNormalize(QJsonObject *tab, QString *error) const
{
    if (!tab)
        return fail(error, bilingual(QStringLiteral("Tab data is missing."),
                                     QStringLiteral("分頁資料唔見咗。")));
    const int page = tab->value(QStringLiteral("page")).toInt(-1);
    if (page < 0 || page > 11)
        return fail(error, bilingual(QStringLiteral("Tab page must be between 0 and 11."),
                                     QStringLiteral("分頁頁碼一定要介乎 0 至 11。")));
    QString title = tab->value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty())
        title = defaultTitleForPage(page);
    if (title.size() > 120 || title.contains(QChar::Null))
        return fail(error, bilingual(QStringLiteral("Tab titles must be at most 120 characters."),
                                     QStringLiteral("分頁標題最多 120 個字元。")));
    QString id = tab->value(QStringLiteral("id")).toString().trimmed();
    if (QUuid(id).isNull())
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString family = tab->value(QStringLiteral("fontFamily")).toString().trimmed();
    if (family.size() > 120 || family.contains(QChar::Null))
        return fail(error, bilingual(QStringLiteral("Tab font family is invalid."),
                                     QStringLiteral("分頁字體系列唔合規格。")));
    QString color = tab->value(QStringLiteral("fontColor")).toString().trimmed();
    if (!color.isEmpty()
        && !QRegularExpression(QStringLiteral("^#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$")).match(color).hasMatch()) {
        return fail(error, bilingual(QStringLiteral("Tab font color must be #RRGGBB or #AARRGGBB."),
                                     QStringLiteral("分頁字體顏色一定要用 #RRGGBB 或 #AARRGGBB。")));
    }
    tab->insert(QStringLiteral("id"), id);
    tab->insert(QStringLiteral("page"), page);
    tab->insert(QStringLiteral("title"), title);
    tab->insert(QStringLiteral("fontFamily"), family);
    tab->insert(QStringLiteral("fontSize"), qBound(8, tab->value(QStringLiteral("fontSize")).toInt(13), 48));
    tab->insert(QStringLiteral("fontColor"), color.toUpper());
    for (const QString &key : {QStringLiteral("bold"), QStringLiteral("italic"),
                               QStringLiteral("strikeout"), QStringLiteral("custom")})
        tab->insert(key, tab->value(key).toBool(false));
    return true;
}

QJsonObject WorkspaceTabs::state() const
{
    QJsonArray tabs;
    for (const QJsonObject &tab : m_tabs)
        tabs.append(tab);
    return QJsonObject{{QStringLiteral("format"), Format},
                       {QStringLiteral("formatVersion"), CurrentFormatVersion},
                       {QStringLiteral("activeIndex"), m_activeIndex},
                       {QStringLiteral("tabs"), tabs}};
}

bool WorkspaceTabs::loadState(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, bilingual(QStringLiteral("Could not read tabs: %1").arg(file.errorString()),
                                     QStringLiteral("讀唔到分頁：%1").arg(file.errorString())));
    if (file.size() > 4 * 1024 * 1024)
        return fail(error, bilingual(QStringLiteral("Tab state exceeds the 4 MiB safety limit."),
                                     QStringLiteral("分頁狀態超過 4 MiB 安全上限。")));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, bilingual(QStringLiteral("Tab state is invalid JSON: %1").arg(parseError.errorString()),
                                     QStringLiteral("分頁狀態唔係有效 JSON：%1").arg(parseError.errorString())));
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != Format
        || root.value(QStringLiteral("formatVersion")).toInt() != CurrentFormatVersion
        || !root.value(QStringLiteral("tabs")).isArray()) {
        return fail(error, bilingual(QStringLiteral("Unsupported workspace-tab format."),
                                     QStringLiteral("唔支援呢個工作空間分頁格式。")));
    }
    QList<QJsonObject> loaded;
    QSet<QString> ids;
    const QJsonArray array = root.value(QStringLiteral("tabs")).toArray();
    if (array.size() > 200)
        return fail(error, bilingual(QStringLiteral("A workspace can contain at most 200 tabs."),
                                     QStringLiteral("一個工作空間最多可以有 200 個分頁。")));
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            return fail(error, bilingual(QStringLiteral("Every tab entry must be an object."),
                                         QStringLiteral("每個分頁項目一定要係 object。")));
        QJsonObject tab = value.toObject();
        if (!validateAndNormalize(&tab, error))
            return false;
        QString id = tab.value(QStringLiteral("id")).toString();
        if (ids.contains(id)) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            tab.insert(QStringLiteral("id"), id);
        }
        ids.insert(id);
        loaded.append(tab);
    }
    if (loaded.isEmpty())
        loaded.append(makeTab(0, QStringLiteral("Overview")));
    m_tabs = std::move(loaded);
    m_activeIndex = root.value(QStringLiteral("activeIndex")).toInt(0);
    ensureActiveIndex();
    setError(error, {});
    return true;
}

bool WorkspaceTabs::save(const QString &message, QString *error)
{
    if (!isOpen())
        return fail(error, bilingual(QStringLiteral("Open a project before changing tabs."),
                                     QStringLiteral("要改分頁，請先開啟工程。")));
    if (m_deferredPersistence) {
        m_pendingCommitMessage = message;
        setError(error, {});
        return true;
    }
    if (!ensureSafeRepositoryLocation(m_projectDirectory, m_repositoryPath, error)
        || !hardenTabRepository(m_repositoryPath, error)) {
        return false;
    }
    ensureActiveIndex();
    const QString statePath = QDir(m_repositoryPath).filePath(StateFile);
    QByteArray previousBytes;
    const bool previousFileExisted = QFileInfo::exists(statePath);
    if (previousFileExisted) {
        QFile previousFile(statePath);
        if (!previousFile.open(QIODevice::ReadOnly))
            return fail(error, bilingual(
                QStringLiteral("Could not preserve the previous tab state: %1").arg(previousFile.errorString()),
                QStringLiteral("保留唔到上一個分頁狀態：%1").arg(previousFile.errorString())));
        previousBytes = previousFile.readAll();
    }
    if (!writeJson(statePath, state(), error))
        return false;
    QString commitError;
    if (!GitHistory(m_repositoryPath, {StateFile}).commit(message, &commitError)) {
        QString rollbackError;
        if (previousFileExisted) {
            QSaveFile rollback(statePath);
            if (!rollback.open(QIODevice::WriteOnly)
                || rollback.write(previousBytes) != previousBytes.size()
                || !rollback.commit()) {
                rollbackError = rollback.errorString();
            }
        } else if (QFileInfo::exists(statePath) && !QFile::remove(statePath)) {
            rollbackError = bilingual(QStringLiteral("could not remove the uncommitted state file"),
                                      QStringLiteral("移除唔到未 commit 嘅狀態檔案"));
        }
        const QString detail = rollbackError.isEmpty()
            ? commitError
            : QStringLiteral("%1 Rollback also failed: %2").arg(commitError, rollbackError);
        setError(error, detail);
        return false;
    }
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("workspace.tabs"),
        QStringLiteral("tabs.committed"),
        bilingual(QStringLiteral("Workspace tab state was committed."),
                  QStringLiteral("工作空間分頁狀態已經 commit。")),
        QJsonObject{{QStringLiteral("tabCount"), m_tabs.size()},
                    {QStringLiteral("activeIndex"), m_activeIndex}});
    setError(error, {});
    return true;
}

void WorkspaceTabs::ensureActiveIndex()
{
    if (m_tabs.isEmpty())
        m_tabs.append(makeTab(0, QStringLiteral("Overview")));
    m_activeIndex = qBound(0, m_activeIndex, m_tabs.size() - 1);
}

bool WorkspaceTabs::openPage(int page, const QString &defaultTitle, QString *error)
{
    if (page < 0 || page > 11)
        return fail(error, bilingual(QStringLiteral("Unknown workspace page."),
                                     QStringLiteral("唔識呢個工作空間頁面。")));
    for (qsizetype index = 0; index < m_tabs.size(); ++index) {
        if (m_tabs.at(index).value(QStringLiteral("page")).toInt() == page) {
            if (m_activeIndex == index) {
                setError(error, {});
                return true;
            }
            const int previousActive = m_activeIndex;
            m_activeIndex = static_cast<int>(index);
            if (!save(bilingual(
                          QStringLiteral("tabs: activate %1").arg(defaultTitleForPage(page)),
                          QStringLiteral("tabs：啟用 %1").arg(defaultTitleForPage(page))), error)) {
                m_activeIndex = previousActive;
                return false;
            }
            return true;
        }
    }
    if (m_tabs.size() >= 200)
        return fail(error, bilingual(
            QStringLiteral("Close a tab before opening another; the 200-tab limit was reached."),
            QStringLiteral("已經到 200 個分頁上限；請關一個先再開。")));
    const QList<QJsonObject> previousTabs = m_tabs;
    const int previousActive = m_activeIndex;
    m_tabs.append(makeTab(page, defaultTitle));
    m_activeIndex = m_tabs.size() - 1;
    if (!save(bilingual(
                  QStringLiteral("tabs: open %1").arg(defaultTitleForPage(page)),
                  QStringLiteral("tabs：開啟 %1").arg(defaultTitleForPage(page))), error)) {
        m_tabs = previousTabs;
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::navigateActiveTab(int page, const QString &defaultTitle, QString *error)
{
    if (page < 0 || page > 11)
        return fail(error, bilingual(QStringLiteral("Unknown workspace page."),
                                     QStringLiteral("唔識呢個工作空間頁面。")));
    ensureActiveIndex();
    QJsonObject tab = m_tabs.at(m_activeIndex);
    const bool custom = tab.value(QStringLiteral("custom")).toBool(false);
    const int oldPage = tab.value(QStringLiteral("page")).toInt();
    const QString newTitle = defaultTitle.trimmed().isEmpty()
        ? defaultTitleForPage(page) : defaultTitle.trimmed();
    if (oldPage == page && (custom || tab.value(QStringLiteral("title")).toString() == newTitle)) {
        setError(error, {});
        return true;
    }
    const QList<QJsonObject> previousTabs = m_tabs;
    const int previousActive = m_activeIndex;
    tab.insert(QStringLiteral("page"), page);
    if (!custom)
        tab.insert(QStringLiteral("title"), newTitle);
    m_tabs[m_activeIndex] = tab;
    if (!save(bilingual(QStringLiteral("tabs: navigate to %1").arg(defaultTitleForPage(page)),
                        QStringLiteral("tabs：切換到 %1").arg(defaultTitleForPage(page))), error)) {
        m_tabs = previousTabs;
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::openNewTab(int page, const QString &defaultTitle, QString *error)
{
    if (page < 0 || page > 11)
        return fail(error, bilingual(QStringLiteral("Unknown workspace page."),
                                     QStringLiteral("唔識呢個工作空間頁面。")));
    if (m_tabs.size() >= 200)
        return fail(error, bilingual(
            QStringLiteral("Close a tab before opening another; the 200-tab limit was reached."),
            QStringLiteral("已經到 200 個分頁上限；請關一個先再開。")));
    const QList<QJsonObject> previousTabs = m_tabs;
    const int previousActive = m_activeIndex;
    m_tabs.append(makeTab(page, defaultTitle));
    m_activeIndex = m_tabs.size() - 1;
    if (!save(bilingual(QStringLiteral("tabs: open %1 in a new tab").arg(defaultTitleForPage(page)),
                        QStringLiteral("tabs：喺新分頁開啟 %1").arg(defaultTitleForPage(page))), error)) {
        m_tabs = previousTabs;
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::activate(int index, QString *error)
{
    if (index < 0 || index >= m_tabs.size())
        return fail(error, bilingual(QStringLiteral("Tab index is out of range."),
                                     QStringLiteral("分頁索引超出範圍。")));
    if (m_activeIndex == index) {
        setError(error, {});
        return true;
    }
    const int previousActive = m_activeIndex;
    m_activeIndex = index;
    const QString title = m_tabs.at(index).value(QStringLiteral("title")).toString();
    if (!save(bilingual(QStringLiteral("tabs: activate %1").arg(title),
                        QStringLiteral("tabs：啟用 %1").arg(title)), error)) {
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::close(int index, QString *error)
{
    if (index < 0 || index >= m_tabs.size())
        return fail(error, bilingual(QStringLiteral("Tab index is out of range."),
                                     QStringLiteral("分頁索引超出範圍。")));
    const QList<QJsonObject> previousTabs = m_tabs;
    const int previousActive = m_activeIndex;
    const QString title = m_tabs.at(index).value(QStringLiteral("title")).toString();
    m_tabs.removeAt(index);
    if (index < m_activeIndex)
        --m_activeIndex;
    else if (index == m_activeIndex && m_activeIndex >= m_tabs.size())
        --m_activeIndex;
    ensureActiveIndex();
    if (!save(bilingual(QStringLiteral("tabs: close %1").arg(title),
                        QStringLiteral("tabs：關閉 %1").arg(title)), error)) {
        m_tabs = previousTabs;
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::closeMany(const QList<int> &indices, QString *error)
{
    QSet<int> toRemove;
    for (const int index : indices) {
        if (index >= 0 && index < m_tabs.size())
            toRemove.insert(index);
    }
    if (toRemove.isEmpty()) {
        setError(error, {});
        return true;
    }
    if (toRemove.size() >= m_tabs.size())
        return fail(error, bilingual(QStringLiteral("At least one workspace tab must stay open."),
                                     QStringLiteral("至少要保留一個工作分頁。")));
    const QList<QJsonObject> previousTabs = m_tabs;
    const int previousActive = m_activeIndex;
    const QString activeId = m_tabs.at(m_activeIndex).value(QStringLiteral("id")).toString();
    QList<QJsonObject> kept;
    for (qsizetype index = 0; index < m_tabs.size(); ++index) {
        if (!toRemove.contains(static_cast<int>(index)))
            kept.append(m_tabs.at(index));
    }
    m_tabs = std::move(kept);
    m_activeIndex = 0;
    for (qsizetype index = 0; index < m_tabs.size(); ++index) {
        if (m_tabs.at(index).value(QStringLiteral("id")).toString() == activeId) {
            m_activeIndex = static_cast<int>(index);
            break;
        }
    }
    ensureActiveIndex();
    if (!save(bilingual(QStringLiteral("tabs: close %1 tabs").arg(toRemove.size()),
                        QStringLiteral("tabs：關閉 %1 個分頁").arg(toRemove.size())), error)) {
        m_tabs = previousTabs;
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::move(int from, int to, QString *error)
{
    if (from < 0 || from >= m_tabs.size() || to < 0 || to >= m_tabs.size())
        return fail(error, bilingual(QStringLiteral("Tab move is out of range."),
                                     QStringLiteral("分頁移動位置超出範圍。")));
    if (from == to) {
        setError(error, {});
        return true;
    }
    const QList<QJsonObject> previousTabs = m_tabs;
    const int previousActive = m_activeIndex;
    const QJsonObject active = m_tabs.at(m_activeIndex);
    m_tabs.move(from, to);
    for (qsizetype index = 0; index < m_tabs.size(); ++index) {
        if (m_tabs.at(index).value(QStringLiteral("id")) == active.value(QStringLiteral("id"))) {
            m_activeIndex = static_cast<int>(index);
            break;
        }
    }
    if (!save(bilingual(QStringLiteral("tabs: reorder workspace"),
                        QStringLiteral("tabs：重新排列工作空間")), error)) {
        m_tabs = previousTabs;
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::update(int index, const QVariantMap &changes, QString *error)
{
    if (index < 0 || index >= m_tabs.size())
        return fail(error, bilingual(QStringLiteral("Tab index is out of range."),
                                     QStringLiteral("分頁索引超出範圍。")));
    const QList<QJsonObject> previousTabs = m_tabs;
    const int previousActive = m_activeIndex;
    QJsonObject tab = m_tabs.at(index);
    static const QSet<QString> allowed{QStringLiteral("title"), QStringLiteral("fontFamily"),
        QStringLiteral("fontSize"), QStringLiteral("fontColor"), QStringLiteral("bold"),
        QStringLiteral("italic"), QStringLiteral("strikeout"), QStringLiteral("custom")};
    for (auto iterator = changes.cbegin(); iterator != changes.cend(); ++iterator) {
        if (allowed.contains(iterator.key()))
            tab.insert(iterator.key(), QJsonValue::fromVariant(iterator.value()));
    }
    if (!validateAndNormalize(&tab, error))
        return false;
    m_tabs[index] = tab;
    const QString title = tab.value(QStringLiteral("title")).toString();
    if (!save(bilingual(QStringLiteral("tabs: style %1").arg(title),
                        QStringLiteral("tabs：更新 %1 樣式").arg(title)), error)) {
        m_tabs = previousTabs;
        m_activeIndex = previousActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::exportTabs(const QString &destinationFile, QString *error) const
{
    const QFileInfo destination(destinationFile);
    if (!isOpen() || !destination.isAbsolute())
        return fail(error, bilingual(QStringLiteral("Choose an absolute destination for the tab export."),
                                     QStringLiteral("匯出分頁請揀一個絕對目的地路徑。")));
    QJsonObject portable = state();
    portable.insert(QStringLiteral("kind"), QStringLiteral("portable-definitions"));
    const bool exported = writeJson(destination.absoluteFilePath(), portable, error);
    if (exported) {
        StructuredLogger::instance().log(
            LogSeverity::Info, QStringLiteral("workspace.tabs"),
            QStringLiteral("tabs.portable_exported"),
            bilingual(QStringLiteral("Portable workspace tabs were exported."),
                      QStringLiteral("已匯出可攜式工作空間分頁。")),
            QJsonObject{{QStringLiteral("tabCount"), m_tabs.size()}});
    }
    return exported;
}

bool WorkspaceTabs::importTabs(const QString &sourceFile, QString *error)
{
    const QFileInfo source(sourceFile);
    if (!isOpen() || !source.isAbsolute() || !source.isFile())
        return fail(error, bilingual(QStringLiteral("Choose an existing tab export file."),
                                     QStringLiteral("請揀一個已存在嘅分頁匯出檔案。")));
    const QList<QJsonObject> existing = m_tabs;
    const int oldActive = m_activeIndex;
    if (!loadState(source.absoluteFilePath(), error)) {
        m_tabs = existing;
        m_activeIndex = oldActive;
        return false;
    }
    QList<QJsonObject> imported = m_tabs;
    m_tabs = existing;
    QSet<QString> ids;
    for (const QJsonObject &tab : m_tabs)
        ids.insert(tab.value(QStringLiteral("id")).toString());
    for (QJsonObject tab : imported) {
        QString id = tab.value(QStringLiteral("id")).toString();
        if (ids.contains(id)) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            tab.insert(QStringLiteral("id"), id);
        }
        ids.insert(id);
        m_tabs.append(tab);
    }
    if (m_tabs.size() > 200) {
        m_tabs = existing;
        m_activeIndex = oldActive;
        return fail(error, bilingual(QStringLiteral("Import would exceed the 200-tab workspace limit."),
                                     QStringLiteral("匯入後會超過工作空間 200 個分頁上限。")));
    }
    m_activeIndex = oldActive;
    if (!save(bilingual(QStringLiteral("tabs: import portable definitions"),
                        QStringLiteral("tabs：匯入可攜式分頁定義")), error)) {
        m_tabs = existing;
        m_activeIndex = oldActive;
        return false;
    }
    return true;
}

bool WorkspaceTabs::exportRepository(const QString &destinationFile, QString *error) const
{
    if (!isOpen())
        return fail(error, bilingual(QStringLiteral("Open a project before exporting its tab repository."),
                                     QStringLiteral("要匯出分頁資料庫，請先開啟工程。")));
    const bool exported = ProjectBundle::exportToFile(
        QFileInfo(destinationFile).absoluteFilePath(),
        {{RepositoryRole, m_repositoryPath, QStringLiteral("tabs")}}, {}, error);
    if (exported) {
        StructuredLogger::instance().log(
            LogSeverity::Info, QStringLiteral("workspace.tabs"),
            QStringLiteral("tabs.repository_exported"),
            bilingual(QStringLiteral("Complete tab Git repository was exported."),
                      QStringLiteral("已匯出完整分頁 Git 資料庫。")),
            QJsonObject{{QStringLiteral("tabCount"), m_tabs.size()},
                        {QStringLiteral("historyPreserved"), true}});
    }
    return exported;
}

bool WorkspaceTabs::importRepository(const QString &sourceFile, QString *error)
{
    if (!isOpen())
        return fail(error, bilingual(QStringLiteral("Open a project before importing a tab repository."),
                                     QStringLiteral("要匯入分頁資料庫，請先開啟工程。")));
    const QString temporary = m_repositoryPath + QStringLiteral(".bundle-import-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto imported = ProjectBundle::importFromFile(
        QFileInfo(sourceFile).absoluteFilePath(), temporary, {}, error);
    if (!imported)
        return false;
    const QString importedRepository = imported->repositoryPaths.value(RepositoryRole);
    WorkspaceTabs validation;
    validation.m_projectDirectory = m_projectDirectory;
    validation.m_repositoryPath = importedRepository;
    if (importedRepository.isEmpty()
        || !hardenTabRepository(importedRepository, error)
        || !validation.loadState(QDir(importedRepository).filePath(StateFile), error)
        || !GitHistory(importedRepository, {StateFile}).initialize(error)) {
        QDir(temporary).removeRecursively();
        if (error && error->isEmpty())
            *error = bilingual(QStringLiteral("The bundle does not contain a valid Git-backed tab repository."),
                               QStringLiteral("呢個 bundle 冇有效嘅 Git-backed 分頁資料庫。"));
        return false;
    }
    const QString backup = m_repositoryPath + QStringLiteral(".backup-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().rename(m_repositoryPath, backup)) {
        QDir(temporary).removeRecursively();
        return fail(error, bilingual(QStringLiteral("Could not create a safety backup of the current tab repository."),
                                     QStringLiteral("現有分頁資料庫整唔到安全備份。")));
    }
    if (!QDir().rename(importedRepository, m_repositoryPath)) {
        QDir().rename(backup, m_repositoryPath);
        QDir(temporary).removeRecursively();
        return fail(error, bilingual(
            QStringLiteral("Could not promote the imported tab repository; the original was restored."),
            QStringLiteral("啟用唔到匯入嘅分頁資料庫；已還原原本資料庫。")));
    }
    QDir(temporary).removeRecursively();
    QDir(backup).removeRecursively();
    if (!loadState(QDir(m_repositoryPath).filePath(StateFile), error))
        return false;
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("workspace.tabs"),
        QStringLiteral("tabs.repository_imported"),
        bilingual(QStringLiteral("Complete tab Git repository was imported."),
                  QStringLiteral("已匯入完整分頁 Git 資料庫。")),
        QJsonObject{{QStringLiteral("tabCount"), m_tabs.size()},
                    {QStringLiteral("historyPreserved"), true}});
    return true;
}

} // namespace wimforge
