#include "AppController.h"
#include "core/GpoPolicyCompiler.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <utility>

using namespace wimforge;

namespace {

QString cleanPath(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? QString() : QDir::cleanPath(trimmed);
}

QString openCodeExecutablePath()
{
#ifdef Q_OS_WIN
    // npm puts POSIX and cmd shims beside the real native binary. Passing either
    // shim to CreateProcess is unreliable, and routing prompts through cmd.exe
    // would unnecessarily expose them to shell expansion. Resolve the native
    // executable instead.
    const QStringList names{QStringLiteral("opencode.exe")};
#else
    const QStringList names{QStringLiteral("opencode")};
#endif
    for (const QString &name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }
    auto environmentFile = [](const char *name, const QString &relative) {
        const QString root = qEnvironmentVariable(name).trimmed();
        return root.isEmpty() ? QString() : QDir(root).filePath(relative);
    };
    const QStringList candidates{
        environmentFile("APPDATA", QStringLiteral("npm/node_modules/opencode-ai/bin/opencode.exe")),
        environmentFile("LOCALAPPDATA", QStringLiteral("npm/node_modules/opencode-ai/bin/opencode.exe")),
        environmentFile("USERPROFILE", QStringLiteral(".opencode/bin/opencode.exe")),
        environmentFile("LOCALAPPDATA", QStringLiteral("opencode/bin/opencode.exe")),
    };
    for (const QString &candidate : candidates)
        if (!candidate.trimmed().isEmpty() && QFileInfo(candidate).isFile())
            return QDir::cleanPath(candidate);
    return {};
}

struct ProcessCommand {
    QString program;
    QStringList prefixArguments;

    [[nodiscard]] bool isEmpty() const { return program.isEmpty(); }
};

ProcessCommand npmProcessCommand()
{
#ifdef Q_OS_WIN
    QStringList nodeCandidates;
    const QString pathNode = QStandardPaths::findExecutable(QStringLiteral("node.exe"));
    if (!pathNode.isEmpty())
        nodeCandidates.append(pathNode);
    auto environmentFile = [](const char *name, const QString &relative) {
        const QString root = qEnvironmentVariable(name).trimmed();
        return root.isEmpty() ? QString() : QDir(root).filePath(relative);
    };
    nodeCandidates.append(environmentFile("ProgramFiles", QStringLiteral("nodejs/node.exe")));
    nodeCandidates.append(environmentFile("ProgramFiles(x86)", QStringLiteral("nodejs/node.exe")));

    QStringList npmRoots;
    const QString npmShim = QStandardPaths::findExecutable(QStringLiteral("npm.cmd"));
    if (!npmShim.isEmpty())
        npmRoots.append(QFileInfo(npmShim).absolutePath());
    for (const QString &node : std::as_const(nodeCandidates)) {
        if (node.trimmed().isEmpty() || !QFileInfo(node).isFile())
            continue;
        QStringList roots = npmRoots;
        roots.prepend(QFileInfo(node).absolutePath());
        for (const QString &root : std::as_const(roots)) {
            const QString npmCli = QDir(root).filePath(
                QStringLiteral("node_modules/npm/bin/npm-cli.js"));
            if (QFileInfo(npmCli).isFile())
                return {QDir::cleanPath(node), {QDir::cleanPath(npmCli)}};
        }
    }
    return {};
#else
    const QString npm = QStandardPaths::findExecutable(QStringLiteral("npm"));
    return npm.isEmpty() ? ProcessCommand{} : ProcessCommand{npm, {}};
#endif
}

QString notificationTimestamp(const QDateTime &value)
{
    if (!value.isValid())
        return QString();
    const QDateTime local = value.toLocalTime();
    return local.date() == QDate::currentDate()
        ? local.time().toString(QStringLiteral("HH:mm"))
        : local.toString(QStringLiteral("yyyy-MM-dd"));
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("Could not create %1").arg(QFileInfo(path).absolutePath()));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    setError(error, {});
    return true;
}

QVariantMap gpoElementVariant(const GpoElement &element)
{
    QVariantList options;
    for (const GpoEnumOption &option : element.options) {
        options.append(QVariantMap{
            {QStringLiteral("label"), option.displayName},
            {QStringLiteral("value"), option.value.toDisplayString()},
        });
    }
    const qint64 minimum = element.minimumValue.value_or(0);
    const qint64 maximum = element.maximumValue.value_or(std::numeric_limits<int>::max());
    return QVariantMap{
        {QStringLiteral("id"), element.id},
        {QStringLiteral("label"), element.presentationLabel.isEmpty()
             ? element.id : element.presentationLabel},
        {QStringLiteral("control"), element.materialControl()},
        {QStringLiteral("required"), element.required},
        {QStringLiteral("defaultValue"), gpoPresentationDefaultValue(element)},
        {QStringLiteral("defaultChecked"), element.presentationDefaultChecked},
        {QStringLiteral("minimum"), QString::number(minimum)},
        {QStringLiteral("maximum"), QString::number(maximum)},
        {QStringLiteral("spinStep"), QString::number(element.presentationSpinStep.value_or(1))},
        {QStringLiteral("numericTextEditor"), gpoUsesNumericTextEditor(element)},
        {QStringLiteral("minimumLength"), element.minimumLength.value_or(-1)},
        {QStringLiteral("maximumLength"), element.maximumLength.value_or(-1)},
        {QStringLiteral("maximumStrings"), element.maximumStrings.value_or(-1)},
        {QStringLiteral("options"), options},
    };
}

QVariantMap gpoPolicyVariant(const GpoPolicy &policy)
{
    QVariantList elements;
    for (const GpoElement &element : policy.elements)
        elements.append(gpoElementVariant(element));
    return QVariantMap{
        {QStringLiteral("id"), policy.qualifiedId()},
        {QStringLiteral("name"), policy.displayName.isEmpty() ? policy.id : policy.displayName},
        {QStringLiteral("category"), policy.categoryHierarchy.join(QStringLiteral("  ›  "))},
        {QStringLiteral("policyClass"), gpoPolicyClassName(policy.policyClass)},
        {QStringLiteral("supportedOn"), policy.supportedOn},
        {QStringLiteral("documentation"), policy.explainText},
        {QStringLiteral("registryKey"), policy.registryKey},
        {QStringLiteral("registryValue"), policy.registryValueName},
        {QStringLiteral("elements"), elements},
    };
}

QString openCodeText(const QByteArray &raw)
{
    QStringList candidates;
    const QList<QByteArray> lines = raw.split('\n');
    for (const QByteArray &line : lines) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            continue;
        const QJsonObject object = document.object();
        for (const QString &key : {QStringLiteral("text"), QStringLiteral("content"),
                                   QStringLiteral("message"), QStringLiteral("output")}) {
            const QJsonValue value = object.value(key);
            if (value.isString() && !value.toString().trimmed().isEmpty())
                candidates.append(value.toString().trimmed());
            else if (value.isObject()) {
                const QString nested = value.toObject().value(QStringLiteral("text")).toString().trimmed();
                if (!nested.isEmpty())
                    candidates.append(nested);
            }
        }
    }
    if (!candidates.isEmpty())
        return candidates.constLast();
    return QString::fromUtf8(raw).trimmed();
}

QByteArray unfenceJson(QString text)
{
    text = text.trimmed();
    const QRegularExpression fence(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)\\s*```"),
                                   QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = fence.match(text);
    if (match.hasMatch())
        text = match.captured(1).trimmed();
    const qsizetype begin = text.indexOf(QLatin1Char('{'));
    const qsizetype end = text.lastIndexOf(QLatin1Char('}'));
    if (begin >= 0 && end >= begin)
        text = text.mid(begin, end - begin + 1);
    return text.toUtf8();
}

WinForgeRecipe emptyWinForgeRecipe(const QString &name = QStringLiteral("WinForge install recipe"))
{
    WinForgeRecipe recipe;
    recipe.id = QStringLiteral("winforge-install-recipe");
    recipe.name = name;
    recipe.description = QStringLiteral(
        "Approved actions replayed by the versioned WimForge WinForge Bridge after Windows setup.");
    recipe.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return recipe;
}

QString discoverWinForgeRuntimePath()
{
    const QString environmentPath = cleanPath(qEnvironmentVariable("WINFORGE_RUNTIME"));
    if (QFileInfo(QDir(environmentPath).filePath(QStringLiteral("WinForge.exe"))).isFile())
        return environmentPath;

    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QStringList directCandidates{
        QDir(applicationDirectory).filePath(QStringLiteral("WinForge")),
        QDir(applicationDirectory).filePath(QStringLiteral("../WinForge")),
        QDir::current().filePath(QStringLiteral("WinForge")),
    };
    for (const QString &candidate : directCandidates) {
        if (QFileInfo(QDir(candidate).filePath(QStringLiteral("WinForge.exe"))).isFile())
            return QDir::cleanPath(candidate);
    }

    const QString developmentBin = QDir::cleanPath(
        QDir::current().filePath(QStringLiteral("../WinForge/bin")));
    if (QFileInfo(developmentBin).isDir()) {
        QDirIterator iterator(developmentBin, {QStringLiteral("WinForge.exe")}, QDir::Files,
                              QDirIterator::Subdirectories);
        if (iterator.hasNext())
            return QFileInfo(iterator.next()).absolutePath();
    }
    return {};
}

QString winForgeCapability(WinForgeActionKind kind)
{
    switch (kind) {
    case WinForgeActionKind::Module: return QStringLiteral("apply.module.v1");
    case WinForgeActionKind::Page: return QStringLiteral("launch.page.v1");
    case WinForgeActionKind::Tweak: return QStringLiteral("apply.tweak.v1");
    case WinForgeActionKind::Command:
    case WinForgeActionKind::Registry:
    case WinForgeActionKind::Copy:
        return {};
    }
    return {};
}

std::optional<WinForgeActionKind> winForgeActionKind(const QString &value)
{
    const QString kind = value.trimmed().toLower();
    if (kind == QStringLiteral("page")) return WinForgeActionKind::Page;
    if (kind == QStringLiteral("module")) return WinForgeActionKind::Module;
    if (kind == QStringLiteral("tweak")) return WinForgeActionKind::Tweak;
    if (kind == QStringLiteral("command")) return WinForgeActionKind::Command;
    if (kind == QStringLiteral("registry")) return WinForgeActionKind::Registry;
    if (kind == QStringLiteral("copy")) return WinForgeActionKind::Copy;
    return std::nullopt;
}

QString bridgeActionSummary(const WinForgeAction &action)
{
    switch (action.kind) {
    case WinForgeActionKind::Module:
    case WinForgeActionKind::Page:
    case WinForgeActionKind::Tweak:
        return action.target;
    case WinForgeActionKind::Command:
        return QStringLiteral("%1 %2").arg(action.executable, action.arguments.join(QLatin1Char(' '))).trimmed();
    case WinForgeActionKind::Registry:
        return QStringLiteral("%1\\%2  →  %3").arg(action.registryHive, action.registryPath,
                                                    action.registryValueName);
    case WinForgeActionKind::Copy:
        return QStringLiteral("%1  →  %2").arg(action.sourceRelative, action.destination);
    }
    return {};
}

} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent),
      m_notificationStore(NotificationStore::defaultStoreDirectory()),
      m_jobEngine(this),
      m_settings(QStringLiteral("WimForge"), QStringLiteral("WimForge"))
{
    m_languageMode = qBound(0, m_settings.value(QStringLiteral("ui/language"), 2).toInt(), 2);
    m_themeMode = qBound(0, m_settings.value(QStringLiteral("ui/theme"), 0).toInt(), 2);
    m_interfaceScale = qBound(0.8, m_settings.value(QStringLiteral("ui/scale"), 1.0).toDouble(), 1.25);
    m_motionEnabled = m_settings.value(QStringLiteral("ui/motion"), true).toBool();
    m_maxParallelJobs = qBound(1, m_settings.value(QStringLiteral("jobs/parallel"), 4).toInt(), 16);
    m_threadLimit = qBound(1, m_settings.value(QStringLiteral("jobs/threads"), logicalCpuCount()).toInt(), logicalCpuCount());
    m_scratchReserveGb = qBound(5, m_settings.value(QStringLiteral("jobs/reserveGb"), 20).toInt(), 500);
    m_crashJournalEnabled = m_settings.value(QStringLiteral("safety/journal"), true).toBool();
    m_verifySourceHash = m_settings.value(QStringLiteral("safety/hash"), true).toBool();
    m_checkpointBeforeDestructive = m_settings.value(QStringLiteral("safety/checkpoint"), true).toBool();
    m_winForgeIncludeRuntime = m_settings.value(QStringLiteral("bridge/includeRuntime"), true).toBool();
    m_winForgeRuntimePath = cleanPath(m_settings.value(QStringLiteral("bridge/runtimePath")).toString());
    if (m_winForgeRuntimePath.isEmpty())
        m_winForgeRuntimePath = discoverWinForgeRuntimePath();
    m_winForgeRecipe = emptyWinForgeRecipe();

    QString notificationError;
    if (m_notificationStore.initialize(&notificationError)) {
        if (m_notificationStore.list(true, true, nullptr).isEmpty()) {
            m_notificationStore.addNotification(
                QStringLiteral("Welcome to WimForge"),
                QStringLiteral("Projects and notifications both have Git history. Even this message can be read, dismissed, deleted and restored without losing its story."),
                QStringLiteral("success"), QStringLiteral("WimForge"), {}, nullptr);
        }
    }
    refreshNotifications();

    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &AppController::onWatchedProjectChanged);
    connect(&m_jobEngine, &JobEngine::stateChanged, this, [this] {
        m_plan = m_jobEngine.operations();
        m_statusText = m_jobEngine.statusText();
        emit stateChanged();
    });
    connect(&m_jobEngine, &JobEngine::operationChanged, this,
            [this](int, const QString &, const QString &) {
        m_plan = m_jobEngine.operations();
        emit stateChanged();
    });
    connect(&m_jobEngine, &JobEngine::finished, this,
            [this](bool success, const QString &message) {
        m_statusText = message;
        notify(success ? QStringLiteral("Servicing completed") : QStringLiteral("Servicing stopped"),
               message, success ? QStringLiteral("success") : QStringLiteral("error"));
        refreshRecoveryState();
        emit stateChanged();
    });

    restoreStudioState();
    if (!m_winForgeRuntimePath.isEmpty()) {
        QString bridgeError;
        m_winForgeRuntimeContract = WinForgeBridge::detectRuntimeContract(
            m_winForgeRuntimePath, &bridgeError);
        m_winForgeRuntimeStatus = m_winForgeRuntimeContract.runtimeFound
            ? QStringLiteral("Detected %1 runtime; capabilities: %2")
                  .arg(m_winForgeRuntimeContract.declaredContract ? QStringLiteral("declared-contract")
                                                                  : QStringLiteral("legacy"),
                       m_winForgeRuntimeContract.capabilities.join(QStringLiteral(", ")))
            : bridgeError;
    }
    m_openCodeStatus = openCodeInstalled()
        ? QStringLiteral("OpenCode was found; live verification is scheduled.")
        : QStringLiteral("OpenCode will be installed automatically when an AI helper is used.");

    const QString lastProject = m_settings.value(QStringLiteral("project/last")).toString();
    if (!lastProject.isEmpty() && QFileInfo::exists(QDir(lastProject).filePath(QStringLiteral("project.json"))))
        openProject(lastProject);

    // OpenCode is a first-class local helper, not a hidden prerequisite. The
    // user explicitly opted into automatic installation by installing
    // WimForge; the process stays asynchronous and reports through the in-app
    // notification center instead of interrupting image jobs with a dialog.
    QTimer::singleShot(2'500, this, [this] {
        installOpenCodeThen([] {});
    });
}

QString AppController::version() const { return QString::fromLatin1(WIMFORGE_VERSION); }
bool AppController::projectLoaded() const { return m_project.has_value(); }
QString AppController::projectName() const { return m_project ? m_project->projectName : QString(); }
QString AppController::projectRoot() const { return m_project ? m_project->projectDirectory : QString(); }
QString AppController::sourcePath() const { return m_project ? m_project->sourcePath : QString(); }
QString AppController::imagePath() const { return m_project ? m_project->imagePath : QString(); }
QString AppController::mountPath() const { return m_project ? m_project->mountPath : QString(); }
QString AppController::outputPath() const { return m_project ? m_project->outputPath : QString(); }
QString AppController::outputFormat() const { return m_project ? m_project->outputFormat : QStringLiteral("wim"); }
QString AppController::isoLabel() const { return m_project ? m_project->isoLabel : QStringLiteral("WIMFORGE"); }
int AppController::imageIndex() const { return m_project ? m_project->selectedImageIndex : 1; }
bool AppController::cloneSource() const { return !m_project || m_project->cloneSource; }
QStringList AppController::editionNames() const { return m_editionNames; }
QString AppController::imageSummary() const { return m_imageSummary; }
QStringList AppController::drivers() const { return m_project ? m_project->drivers : QStringList(); }
QStringList AppController::packages() const { return m_project ? m_project->packages : QStringList(); }
QStringList AppController::features() const { return m_project ? m_project->featuresToEnable : QStringList(); }
QStringList AppController::appRemovals() const { return m_project ? m_project->appxPackagesToRemove : QStringList(); }
QStringList AppController::componentRemovals() const { return m_project ? m_project->componentsToRemove : QStringList(); }
QStringList AppController::unattendedFiles() const { return m_project ? m_project->unattendedFiles : QStringList(); }
QStringList AppController::postSetupItems() const { return m_project ? m_project->postSetupItems : QStringList(); }

QVariantList AppController::operationPlan() const
{
    QVariantList result;
    for (const ServicingOperation &operation : m_plan) {
        result.append(QVariantMap{
            {QStringLiteral("id"), operation.id},
            {QStringLiteral("title"), localized(operation.titleEn, operation.titleZh)},
            {QStringLiteral("description"), localized(operation.descriptionEn, operation.descriptionZh)},
            {QStringLiteral("command"), operation.previewCommand()},
            {QStringLiteral("admin"), operation.requiresAdministrator},
            {QStringLiteral("destructive"), operation.destructive},
            {QStringLiteral("reboot"), operation.rebootRequired},
            {QStringLiteral("status"), ServicingPlan::operationStateName(operation.state)},
        });
    }
    return result;
}

int AppController::operationCount() const { return m_plan.size(); }

QVariantList AppController::projectHistory() const
{
    QVariantList result;
    for (const GitCommit &commit : m_history) {
        result.append(QVariantMap{
            {QStringLiteral("hash"), commit.hash},
            {QStringLiteral("shortHash"), commit.shortHash},
            {QStringLiteral("timestamp"), commit.authoredAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))},
            {QStringLiteral("subject"), commit.subject},
            {QStringLiteral("isRevert"), commit.isRevert()},
        });
    }
    return result;
}

int AppController::projectHistoryCount() const { return m_history.size(); }
QString AppController::gitStatusText() const
{
    return !m_project ? QString() : localized(QStringLiteral("✓ Auto-committed"), QStringLiteral("✓ 已自動 commit"));
}

QVariantList AppController::actionHistory() const
{
    return contextualHistory({}, {});
}

QString AppController::historyBranch() const
{
    if (!m_project)
        return QStringLiteral("main");
    return ActionHistory(m_project->projectDirectory).currentBranch(nullptr);
}

QStringList AppController::historyBranches() const
{
    if (!m_project)
        return {QStringLiteral("main")};
    return ActionHistory(m_project->projectDirectory).branchNames(nullptr);
}

QVariantList AppController::notifications() const
{
    QVariantList result;
    for (const Notification &item : m_notificationItems) {
        result.append(QVariantMap{
            {QStringLiteral("id"), item.id},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("message"), item.message},
            {QStringLiteral("kind"), item.severity},
            {QStringLiteral("timestamp"), notificationTimestamp(item.createdAt)},
            {QStringLiteral("read"), item.isRead},
            {QStringLiteral("dismissed"), item.isDismissed},
            {QStringLiteral("deleted"), item.isDeleted},
        });
    }
    return result;
}

int AppController::notificationUnreadCount() const
{
    return static_cast<int>(std::count_if(m_notificationItems.cbegin(), m_notificationItems.cend(),
        [](const Notification &item) { return !item.isRead && !item.isDismissed && !item.isDeleted; }));
}

QString AppController::notificationRepoPath() const { return m_notificationStore.storeDirectory(); }
bool AppController::busy() const { return m_jobEngine.isRunning() || m_inspecting; }
double AppController::progress() const { return m_jobEngine.progress(); }
QString AppController::statusText() const { return m_statusText; }
int AppController::runningJobCount() const { return m_jobEngine.runningCount(); }
bool AppController::pendingRecovery() const { return m_pendingRecovery; }

QString AppController::recoverySummary() const
{
    if (!m_pendingRecovery)
        return localized(QStringLiteral("No interrupted work is waiting."), QStringLiteral("冇中斷工序等緊。"));
    return localized(
        QStringLiteral("Run %1 was interrupted while using mount %2. The source remains untouched. Review the journal, rebuild the plan, or safely unmount.")
            .arg(m_recoveryJournal.value(QStringLiteral("runId")).toString(),
                 m_recoveryJournal.value(QStringLiteral("mountPath")).toString()),
        QStringLiteral("工序 %1 喺掛載 %2 時中斷。來源冇畀人掂過；繼續或者卸載之前先睇日誌。")
            .arg(m_recoveryJournal.value(QStringLiteral("runId")).toString(),
                 m_recoveryJournal.value(QStringLiteral("mountPath")).toString()));
}

QString AppController::recoveryPath() const { return JobEngine::defaultRecoveryRoot(); }
int AppController::languageMode() const { return m_languageMode; }
int AppController::themeMode() const { return m_themeMode; }
double AppController::interfaceScale() const { return m_interfaceScale; }
bool AppController::motionEnabled() const { return m_motionEnabled; }
int AppController::maxParallelJobs() const { return m_maxParallelJobs; }
int AppController::logicalCpuCount() const { return qMax(1, QThread::idealThreadCount()); }
int AppController::threadLimit() const { return m_threadLimit; }
int AppController::scratchReserveGb() const { return m_scratchReserveGb; }
bool AppController::crashJournalEnabled() const { return m_crashJournalEnabled; }
bool AppController::verifySourceHash() const { return m_verifySourceHash; }
bool AppController::checkpointBeforeDestructive() const { return m_checkpointBeforeDestructive; }
bool AppController::autoImport() const { return m_project && m_project->autoImport; }
bool AppController::autoExport() const { return m_project && m_project->autoExport; }
QString AppController::autoExportPath() const { return m_project ? m_project->autoExportPath : QString(); }

QString AppController::defaultProjectPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("WimForge Projects/Windows 11 Custom"));
}

bool AppController::gpoLoaded() const { return m_gpoLoaded; }
int AppController::gpoPolicyCount() const { return m_gpoCatalog.policies().size(); }
QString AppController::gpoStatus() const { return m_gpoStatus; }

QVariantList AppController::gpoResults() const
{
    QVariantList result;
    for (const GpoPolicy &policy : m_gpoSearchResults)
        result.append(gpoPolicyVariant(policy));
    return result;
}

QVariantList AppController::packageCatalog() const
{
    QVariantList result;
    for (const PackageEntry &package : m_packageProfile.packages) {
        result.append(QVariantMap{
            {QStringLiteral("id"), package.id},
            {QStringLiteral("name"), package.displayName},
            {QStringLiteral("description"), package.description},
            {QStringLiteral("provider"), PackageStudio::providerName(package.provider)},
            {QStringLiteral("identifier"), package.packageIdentifier},
            {QStringLiteral("version"), package.version},
            {QStringLiteral("license"), package.license},
            {QStringLiteral("homepage"), package.homepage},
            {QStringLiteral("enabled"), package.enabled},
            {QStringLiteral("optional"), package.optional},
            {QStringLiteral("offline"), !package.requiresNetwork},
        });
    }
    return result;
}

QString AppController::packageProfileName() const { return m_packageProfile.name; }

int AppController::selectedPackageCount() const
{
    return static_cast<int>(std::count_if(m_packageProfile.packages.cbegin(),
                                          m_packageProfile.packages.cend(),
        [](const PackageEntry &package) { return package.enabled; }));
}

QVariantList AppController::unattendedSettings() const
{
    QVariantList result;
    for (const UnattendSetting &setting : m_unattendProfile.settings) {
        QStringList path;
        for (const UnattendPathSegment &segment : setting.path)
            path.append(segment.name);
        result.append(QVariantMap{
            {QStringLiteral("pass"), UnattendBuilder::passName(setting.pass)},
            {QStringLiteral("component"), setting.component},
            {QStringLiteral("path"), path.join(QLatin1Char('/'))},
            {QStringLiteral("value"), setting.value},
            {QStringLiteral("architecture"), setting.architecture},
        });
    }
    return result;
}

QVariantList AppController::microsoftProductKeys() const
{
    QVariantList result;
    for (const ProductKeyEntry &entry : UnattendBuilder::microsoftPublishedGvlks()) {
        result.append(QVariantMap{
            {QStringLiteral("edition"), entry.edition},
            {QStringLiteral("key"), entry.key},
            {QStringLiteral("channel"), entry.channel},
            {QStringLiteral("documentationUrl"), entry.documentationUrl},
            {QStringLiteral("licensingNotice"), entry.licensingNotice},
        });
    }
    return result;
}

int AppController::computerNameMode() const
{
    return static_cast<int>(m_unattendProfile.computerNameMode);
}

QString AppController::computerNameValue() const
{
    return m_unattendProfile.computerNameMode == ComputerNameMode::SerialNumber
        ? m_unattendProfile.serialPrefix : m_unattendProfile.computerName;
}

bool AppController::openCodeInstalled() const
{
    return !openCodeExecutablePath().isEmpty();
}

bool AppController::openCodeBusy() const { return m_openCodeBusy; }
QString AppController::openCodeStatus() const { return m_openCodeStatus; }

QVariantList AppController::winForgeBridgeActions() const
{
    QVariantList result;
    for (const WinForgeAction &action : m_winForgeRecipe.actions) {
        const QString capability = winForgeCapability(action.kind);
        const bool supported = capability.isEmpty()
            || (m_winForgeRuntimeContract.runtimeFound
                && m_winForgeRuntimeContract.capabilities.contains(capability)
                && m_winForgeRuntimeContract.invocations.contains(capability));
        QString supportReason;
        if (!supported) {
            supportReason = !m_winForgeRuntimeContract.runtimeFound
                ? QStringLiteral("Select and detect a WinForge runtime before approving this action.")
                : QStringLiteral("Runtime does not declare %1.").arg(capability);
        }
        result.append(QVariantMap{
            {QStringLiteral("id"), action.id},
            {QStringLiteral("title"), action.id},
            {QStringLiteral("kind"), WinForgeBridge::actionKindName(action.kind)},
            {QStringLiteral("phase"), WinForgeBridge::actionPhaseName(action.phase)},
            {QStringLiteral("enabled"), action.enabled},
            {QStringLiteral("target"), action.target},
            {QStringLiteral("summary"), bridgeActionSummary(action)},
            {QStringLiteral("supported"), supported},
            {QStringLiteral("supportReason"), supportReason},
        });
    }
    return result;
}

bool AppController::winForgeBridgeIncludeRuntime() const { return m_winForgeIncludeRuntime; }
QString AppController::winForgeBridgeRuntimePath() const { return m_winForgeRuntimePath; }
QString AppController::winForgeBridgeRuntimeStatus() const { return m_winForgeRuntimeStatus; }
QString AppController::winForgeBridgeStatus() const { return m_winForgeBridgeStatus; }

void AppController::setLanguageMode(int value)
{
    value = qBound(0, value, 2);
    if (m_languageMode == value) return;
    m_languageMode = value; m_settings.setValue(QStringLiteral("ui/language"), value);
    emit preferencesChanged(); emit stateChanged(); emit notificationsChanged();
}
void AppController::setThemeMode(int value) { value = qBound(0, value, 2); if (m_themeMode == value) return; m_themeMode = value; m_settings.setValue(QStringLiteral("ui/theme"), value); emit preferencesChanged(); }
void AppController::setInterfaceScale(double value) { value = qBound(0.8, value, 1.25); if (qFuzzyCompare(m_interfaceScale, value)) return; m_interfaceScale = value; m_settings.setValue(QStringLiteral("ui/scale"), value); emit preferencesChanged(); }
void AppController::setMotionEnabled(bool value) { if (m_motionEnabled == value) return; m_motionEnabled = value; m_settings.setValue(QStringLiteral("ui/motion"), value); emit preferencesChanged(); }
void AppController::setMaxParallelJobs(int value) { value = qBound(1, value, 16); if (m_maxParallelJobs == value) return; m_maxParallelJobs = value; m_settings.setValue(QStringLiteral("jobs/parallel"), value); emit preferencesChanged(); }
void AppController::setThreadLimit(int value) { value = qBound(1, value, logicalCpuCount()); if (m_threadLimit == value) return; m_threadLimit = value; m_settings.setValue(QStringLiteral("jobs/threads"), value); emit preferencesChanged(); }
void AppController::setScratchReserveGb(int value) { value = qBound(5, value, 500); if (m_scratchReserveGb == value) return; m_scratchReserveGb = value; m_settings.setValue(QStringLiteral("jobs/reserveGb"), value); emit preferencesChanged(); }
void AppController::setCrashJournalEnabled(bool value) { if (m_crashJournalEnabled == value) return; m_crashJournalEnabled = value; m_settings.setValue(QStringLiteral("safety/journal"), value); emit preferencesChanged(); }
void AppController::setVerifySourceHash(bool value) { if (m_verifySourceHash == value) return; m_verifySourceHash = value; m_settings.setValue(QStringLiteral("safety/hash"), value); if (m_project) mutateProject(QStringLiteral("safety: change payload verification"), [value](ProjectConfig &p) { p.options.verifyPayloads = value; }); emit preferencesChanged(); }
void AppController::setCheckpointBeforeDestructive(bool value) { if (m_checkpointBeforeDestructive == value) return; m_checkpointBeforeDestructive = value; m_settings.setValue(QStringLiteral("safety/checkpoint"), value); emit preferencesChanged(); }
void AppController::setAutoImport(bool value) { if (m_project) mutateProject(QStringLiteral("automation: change auto import"), [value](ProjectConfig &p) { p.autoImport = value; }); }
void AppController::setAutoExport(bool value) { if (m_project) mutateProject(QStringLiteral("automation: change auto export"), [value](ProjectConfig &p) { p.autoExport = value; }); }
void AppController::setAutoExportPath(const QString &value) { if (m_project) mutateProject(QStringLiteral("automation: change export destination"), [value](ProjectConfig &p) { p.autoExportPath = cleanPath(value); }); }

void AppController::requestNewProject() { emit newProjectRequested(); }
void AppController::requestOpenProject() { emit openProjectRequested(); }

bool AppController::createProject(const QString &directory, const QString &name)
{
    ProjectConfig project;
    project.projectDirectory = cleanPath(directory);
    project.projectName = name.trimmed();
    project.mountPath = QDir(project.projectDirectory).filePath(QStringLiteral("mount"));
    project.outputPath = QDir(project.projectDirectory).filePath(QStringLiteral("output/custom-windows.wim"));
    project.options.scratchDirectory = QDir(project.projectDirectory).filePath(QStringLiteral("scratch"));
    project.options.verifyPayloads = m_verifySourceHash;
    project.options.maximumParallelOperations = m_maxParallelJobs;
    QString error;
    if (!project.save(&error, QStringLiteral("project: create %1").arg(project.projectName))) {
        showError(error); return false;
    }
    m_project = std::move(project);
    m_settings.setValue(QStringLiteral("project/last"), m_project->projectDirectory);
    loadProjectState();
    notify(QStringLiteral("Project created"), QStringLiteral("Every configuration action is now committed in this project's local Git repository."), QStringLiteral("success"));
    showSuccess(localized(QStringLiteral("Project created — Git history is active."), QStringLiteral("工程開好 — Git 歷史已經開工。")));
    return true;
}

bool AppController::openProject(const QString &directory)
{
    QString error;
    const auto project = ProjectConfig::load(cleanPath(directory), &error);
    if (!project) { showError(error); return false; }
    m_project = *project;
    m_settings.setValue(QStringLiteral("project/last"), m_project->projectDirectory);
    loadProjectState();
    showSuccess(localized(QStringLiteral("Project opened."), QStringLiteral("工程開咗。")));
    return true;
}

bool AppController::importProject(const QString &sourceFile, const QString &destinationDirectory)
{
    if (QFileInfo(sourceFile).suffix().compare(QStringLiteral("wimforge"), Qt::CaseInsensitive) == 0) {
        QString error;
        const auto imported = ProjectBundle::importFromFile(cleanPath(sourceFile),
                                                             cleanPath(destinationDirectory), {}, &error);
        if (!imported) {
            showError(error);
            return false;
        }
        const QString projectPath = imported->repositoryPaths.value(ProjectBundle::ProjectRepositoryRole);
        const QString notificationPath = imported->repositoryPaths.value(ProjectBundle::NotificationRepositoryRole);
        const auto project = ProjectConfig::load(projectPath, &error);
        if (!project) {
            showError(error);
            return false;
        }
        m_project = *project;
        if (!notificationPath.isEmpty()) {
            m_notificationStore = NotificationStore(notificationPath);
            if (!m_notificationStore.initialize(&error)) {
                showError(error);
                return false;
            }
            ProjectConfig candidate = *m_project;
            candidate.settings.insert(QStringLiteral("_notificationRepoPath"), notificationPath);
            if (!candidate.save(&error, QStringLiteral("bundle: reconnect notification history"))) {
                showError(error);
                return false;
            }
            m_project = candidate;
        }
        m_settings.setValue(QStringLiteral("project/last"), m_project->projectDirectory);
        loadProjectState();
        refreshNotifications();
        notify(QStringLiteral("Complete project bundle imported"),
               QStringLiteral("Project commits, action branches, notification events, tombstones and all local Git metadata were restored."),
               QStringLiteral("success"));
        return true;
    }
    QString error;
    const auto project = ProjectConfig::importJson(cleanPath(sourceFile), cleanPath(destinationDirectory), &error);
    if (!project) { showError(error); return false; }
    m_project = *project;
    m_settings.setValue(QStringLiteral("project/last"), m_project->projectDirectory);
    loadProjectState();
    notify(QStringLiteral("Project imported"), QStringLiteral("The imported configuration now has its own local Git history."), QStringLiteral("success"));
    return true;
}

bool AppController::exportProject(const QString &destinationFile)
{
    if (!m_project) { showError(QStringLiteral("Open a project first.")); return false; }
    QString error;
    const QString destination = cleanPath(destinationFile);
    if (QFileInfo(destination).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0) {
        if (!m_project->exportJson(destination, &error)) { showError(error); return false; }
    } else {
        const QList<ProjectBundleRepository> repositories{
            {ProjectBundle::ProjectRepositoryRole, m_project->projectDirectory,
             QStringLiteral("project")},
            {ProjectBundle::NotificationRepositoryRole, m_notificationStore.storeDirectory(),
             QStringLiteral("notifications")},
        };
        if (!ProjectBundle::exportToFile(destination, repositories, {}, &error)) {
            showError(error);
            return false;
        }
    }
    showSuccess(localized(QStringLiteral("Complete portable project bundle exported."),
                          QStringLiteral("完整可攜工程 bundle 已匯出。")));
    notify(QStringLiteral("Project exported"), destinationFile, QStringLiteral("success"));
    return true;
}

bool AppController::exportScript(const QString &destinationFile)
{
    if (!m_project) { showError(QStringLiteral("Open a project first.")); return false; }
    QString error;
    if (!ServicingPlan::exportPowerShell(*m_project, m_plan, cleanPath(destinationFile), &error)) { showError(error); return false; }
    showSuccess(localized(QStringLiteral("Reproducible PowerShell plan exported."), QStringLiteral("可重現 PowerShell 計劃已匯出。")));
    return true;
}

void AppController::requestExportProject() { emit exportProjectRequested(); }
void AppController::requestExportScript() { emit exportScriptRequested(); }

bool AppController::mutateProject(const QString &message, const ProjectMutation &mutation)
{
    if (!m_project) { showError(QStringLiteral("Open a project first.")); return false; }
    const QJsonObject before = m_project->toJson();
    ProjectConfig candidate = *m_project;
    mutation(candidate);
    QString error;
    if (!candidate.save(&error, message)) { showError(error); return false; }
    const QJsonObject after = candidate.toJson();
    m_project = std::move(candidate);

    ActionDraft action;
    const qsizetype separator = message.indexOf(QLatin1Char(':'));
    action.contextKey = separator > 0 ? message.left(separator).trimmed() : QStringLiteral("project");
    action.elementId = separator >= 0 ? message.mid(separator + 1).simplified() : message.simplified();
    action.title = message;
    action.description = QStringLiteral("Committed project configuration change.");
    action.icon = action.contextKey == QStringLiteral("gpo") ? QStringLiteral("policy")
        : action.contextKey == QStringLiteral("packages") ? QStringLiteral("inventory_2")
        : action.contextKey == QStringLiteral("unattended") ? QStringLiteral("auto_fix_high")
        : QStringLiteral("edit");
    action.forwardDiff = ActionHistory::createMergePatch(before, after);
    action.inverseDiff = ActionHistory::createMergePatch(after, before);
    action.metadata = QJsonObject{{QStringLiteral("diffSummary"), message},
                                  {QStringLiteral("stateFormat"), QStringLiteral("merge-patch")},
                                  {QStringLiteral("beforeState"), before},
                                  {QStringLiteral("afterState"), after}};
    QString historyError;
    if (!ActionHistory(m_project->projectDirectory).record(action, nullptr, &historyError)) {
        notify(QStringLiteral("Context history warning"),
               QStringLiteral("The project Git commit is safe, but the contextual event could not be recorded: %1")
                   .arg(historyError), QStringLiteral("warning"));
    }
    if (m_project->autoExport
        && QFileInfo(m_project->autoExportPath).suffix().compare(
               QStringLiteral("wimforge"), Qt::CaseInsensitive) == 0) {
        const QList<ProjectBundleRepository> repositories{
            {ProjectBundle::ProjectRepositoryRole, m_project->projectDirectory,
             QStringLiteral("project")},
            {ProjectBundle::NotificationRepositoryRole, m_notificationStore.storeDirectory(),
             QStringLiteral("notifications")},
        };
        QString bundleError;
        if (!ProjectBundle::exportToFile(m_project->autoExportPath, repositories, {}, &bundleError))
            showError(QStringLiteral("The action was committed, but complete auto-export failed: %1")
                          .arg(bundleError));
    }
    loadProjectState();
    return true;
}

bool AppController::saveProject(const QString &message)
{
    return mutateProject(message, [](ProjectConfig &) {});
}

void AppController::setProjectField(const QString &field, const QString &value)
{
    mutateProject(QStringLiteral("config: set %1").arg(field), [field, value](ProjectConfig &p) {
        const QString path = cleanPath(value);
        if (field == QStringLiteral("sourcePath")) p.sourcePath = path;
        else if (field == QStringLiteral("imagePath")) p.imagePath = path;
        else if (field == QStringLiteral("mountPath")) p.mountPath = path;
        else if (field == QStringLiteral("outputPath")) p.outputPath = path;
        else if (field == QStringLiteral("outputFormat")) p.outputFormat = value.trimmed().toLower();
        else if (field == QStringLiteral("isoLabel")) p.isoLabel = value.trimmed();
        else if (field == QStringLiteral("unattendedXmlPath")) p.unattendedXmlPath = path;
    });
}

void AppController::setProjectBool(const QString &field, bool value)
{
    mutateProject(QStringLiteral("config: set %1").arg(field), [field, value](ProjectConfig &p) {
        if (field == QStringLiteral("cloneSource")) p.cloneSource = value;
        else if (field == QStringLiteral("createIso")) p.options.createIso = value;
        else if (field == QStringLiteral("resetBase")) p.options.resetBase = value;
        else if (field == QStringLiteral("dryRun")) p.options.dryRun = value;
    });
}

void AppController::setProjectNumber(const QString &field, int value)
{
    mutateProject(QStringLiteral("config: set %1").arg(field), [field, value](ProjectConfig &p) {
        if (field == QStringLiteral("imageIndex")) p.selectedImageIndex = qMax(1, value);
        else if (field == QStringLiteral("parallel")) p.options.maximumParallelOperations = qBound(1, value, 16);
        else if (field == QStringLiteral("splitSizeMb")) p.options.extra.insert(QStringLiteral("splitSizeMb"), qBound(100, value, 4095));
    });
}

QStringList *AppController::listForCategory(ProjectConfig &project, const QString &category)
{
    if (category == QStringLiteral("drivers")) return &project.drivers;
    if (category == QStringLiteral("packages")) return &project.packages;
    if (category == QStringLiteral("updates")) return &project.updates;
    if (category == QStringLiteral("features")) return &project.featuresToEnable;
    if (category == QStringLiteral("appRemovals")) return &project.appxPackagesToRemove;
    if (category == QStringLiteral("appProvision")) return &project.appxPackagesToProvision;
    if (category == QStringLiteral("componentRemovals")) return &project.componentsToRemove;
    if (category == QStringLiteral("unattendFiles")) return &project.unattendedFiles;
    if (category == QStringLiteral("postSetupItems")) return &project.postSetupItems;
    return nullptr;
}

void AppController::addListItem(const QString &category, const QString &value)
{
    const QString item = value.trimmed();
    if (item.isEmpty()) return;
    mutateProject(QStringLiteral("config: add %1").arg(category), [this, category, item](ProjectConfig &p) {
        if (QStringList *list = listForCategory(p, category); list && !list->contains(item, Qt::CaseInsensitive))
            list->append(item);
    });
}

void AppController::removeListItem(const QString &category, int index)
{
    mutateProject(QStringLiteral("config: remove %1 item").arg(category), [this, category, index](ProjectConfig &p) {
        if (QStringList *list = listForCategory(p, category); list && index >= 0 && index < list->size())
            list->removeAt(index);
    });
}

void AppController::setFeature(const QString &name, bool enabled)
{
    mutateProject(QStringLiteral("feature: %1 %2").arg(enabled ? QStringLiteral("enable") : QStringLiteral("disable"), name),
                  [name, enabled](ProjectConfig &p) {
        p.featuresToEnable.removeAll(name); p.featuresToDisable.removeAll(name);
        (enabled ? p.featuresToEnable : p.featuresToDisable).append(name);
    });
}

void AppController::setSetting(const QString &name, bool enabled)
{
    mutateProject(QStringLiteral("setting: %1 %2").arg(enabled ? QStringLiteral("enable") : QStringLiteral("disable"), name),
                  [name, enabled](ProjectConfig &p) { p.settings.insert(name, enabled); });
}

bool AppController::settingEnabled(const QString &name) const
{
    return m_project && m_project->settings.value(name).toBool(false);
}

void AppController::inspectSource()
{
    if (!m_project || m_project->sourcePath.isEmpty()) { showError(QStringLiteral("Choose a source first.")); return; }
    QFileInfo source(m_project->sourcePath);
    if (!source.exists()) { showError(QStringLiteral("Source does not exist: %1").arg(source.filePath())); return; }

    QString image = m_project->imagePath;
    if (source.isFile() && QStringList{QStringLiteral("wim"), QStringLiteral("esd"), QStringLiteral("swm")}
            .contains(source.suffix().toLower()))
        image = source.absoluteFilePath();
    else if (source.isDir()) {
        const QString wim = QDir(source.filePath()).filePath(QStringLiteral("sources/install.wim"));
        const QString esd = QDir(source.filePath()).filePath(QStringLiteral("sources/install.esd"));
        if (QFileInfo::exists(wim)) image = wim;
        else if (QFileInfo::exists(esd)) image = esd;
    }
    if (image.isEmpty()) {
        showError(localized(QStringLiteral("For ISO sources, extract or mount the ISO and set its sources\\install.wim path. Automatic ISO workspace extraction is available from the plan after an image is selected."),
                            QStringLiteral("ISO 來源請先解壓或者掛載，再揀 sources\\install.wim。揀好映像之後，計劃可以自動整工作副本。")));
        return;
    }
    if (image != m_project->imagePath)
        mutateProject(QStringLiteral("source: detect image container"), [image](ProjectConfig &p) { p.imagePath = image; });

    m_inspecting = true;
    m_statusText = QStringLiteral("Inspecting image editions");
    emit stateChanged();
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("dism.exe"));
    process->setArguments({QStringLiteral("/English"), QStringLiteral("/Get-WimInfo"),
                           QStringLiteral("/WimFile:%1").arg(image)});
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, image](int exitCode, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(process->readAll());
        process->deleteLater(); m_inspecting = false;
        if (status != QProcess::NormalExit || exitCode != 0) {
            m_statusText = QStringLiteral("Image inspection failed"); showError(output.trimmed()); emit stateChanged(); return;
        }
        QStringList editions;
        QRegularExpression block(QStringLiteral("Index\\s*:\\s*(\\d+)[\\s\\S]*?Name\\s*:\\s*([^\\r\\n]+)"), QRegularExpression::CaseInsensitiveOption);
        auto match = block.globalMatch(output);
        while (match.hasNext()) {
            const auto item = match.next();
            editions.append(QStringLiteral("Index %1 — %2").arg(item.captured(1), item.captured(2).trimmed()));
        }
        if (!editions.isEmpty()) m_editionNames = editions;
        m_imageSummary = QStringLiteral("%1 edition(s) · %2").arg(m_editionNames.size()).arg(image);
        m_statusText = QStringLiteral("Image inventory ready");
        notify(QStringLiteral("Image inventory ready"), m_imageSummary, QStringLiteral("success"));
        emit stateChanged();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { m_inspecting = false; showError(process->errorString()); process->deleteLater(); emit stateChanged(); }
    });
    process->start();
}

void AppController::importHostDrivers()
{
    if (!m_project) { showError(QStringLiteral("Open a project first.")); return; }
    const QString destination = QDir(m_project->projectDirectory).filePath(QStringLiteral("payloads/host-drivers"));
    QDir().mkpath(destination);
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("dism.exe"));
    process->setArguments({QStringLiteral("/Online"), QStringLiteral("/Export-Driver"),
                           QStringLiteral("/Destination:%1").arg(destination)});
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_inspecting = true; m_statusText = QStringLiteral("Exporting host drivers"); emit stateChanged();
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, destination](int code, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(process->readAll()); process->deleteLater(); m_inspecting = false;
        if (status == QProcess::NormalExit && code == 0) {
            addListItem(QStringLiteral("drivers"), destination);
            notify(QStringLiteral("Host drivers exported"), destination, QStringLiteral("success"));
        } else showError(output.trimmed());
        emit stateChanged();
    });
    process->start();
}

void AppController::refreshPlan()
{
    m_plan.clear();
    if (!m_project) { emit stateChanged(); return; }
    ServicingPlanResult result = ServicingPlan::build(*m_project);
    m_plan = result.operations;
    const QJsonArray order = m_project->options.extra.value(QStringLiteral("planOrder")).toArray();
    if (!order.isEmpty()) {
        QList<ServicingOperation> reordered;
        QSet<QString> inserted;
        for (const QJsonValue &value : order) {
            const QString id = value.toString();
            const auto found = std::find_if(m_plan.cbegin(), m_plan.cend(),
                [&id](const ServicingOperation &operation) { return operation.id == id; });
            if (found != m_plan.cend()) {
                reordered.append(*found);
                inserted.insert(id);
            }
        }
        for (const ServicingOperation &operation : std::as_const(m_plan))
            if (!inserted.contains(operation.id)) reordered.append(operation);
        m_plan = reordered;
    }
    const QJsonArray skipped = m_project->options.extra.value(QStringLiteral("planSkipped")).toArray();
    QSet<QString> skippedIds;
    for (const QJsonValue &value : skipped)
        skippedIds.insert(value.toString());
    for (ServicingOperation &operation : m_plan)
        if (skippedIds.contains(operation.id)) operation.state = OperationState::Skipped;
    if (!result.errors.isEmpty()) m_statusText = result.errors.first();
    else m_statusText = QStringLiteral("Plan ready — %1 operations").arg(m_plan.size());
    emit stateChanged();
}

void AppController::requestRunPlan()
{
    if (!m_project || m_plan.isEmpty()) { showError(QStringLiteral("Build a valid plan first.")); return; }
    const int destructive = static_cast<int>(std::count_if(m_plan.cbegin(), m_plan.cend(), [](const ServicingOperation &item) { return item.destructive; }));
    emit runConfirmationRequested(localized(
        QStringLiteral("%1 operations will run with up to %2 concurrent jobs. Writes to the mounted image remain serialized.").arg(m_plan.size()).arg(m_maxParallelJobs),
        QStringLiteral("%1 項工序，最多 %2 個平行工作；寫入掛載映像仍然會逐個排隊。").arg(m_plan.size()).arg(m_maxParallelJobs)), destructive);
}

void AppController::runPlan()
{
    if (!m_project) return;
    QString error;
    if (!m_jobEngine.start(*m_project, m_plan, m_maxParallelJobs, &error)) { showError(error); return; }
    notify(QStringLiteral("Servicing started"), QStringLiteral("The crash journal is active. You can keep using WimForge while independent jobs run."), QStringLiteral("progress"));
}

void AppController::cancelJobs() { m_jobEngine.cancel(); }

void AppController::moveOperation(int index, int offset)
{
    const int destination = index + offset;
    if (index < 0 || index >= m_plan.size() || destination < 0 || destination >= m_plan.size() || busy()) return;
    m_plan.move(index, destination);
    QJsonArray order;
    for (const ServicingOperation &operation : std::as_const(m_plan))
        order.append(operation.id);
    mutateProject(QStringLiteral("plan: reorder operation"), [order](ProjectConfig &project) {
        project.options.extra.insert(QStringLiteral("planOrder"), order);
    });
}

void AppController::skipOperation(int index)
{
    if (index < 0 || index >= m_plan.size() || busy()) return;
    const OperationKind kind = m_plan.at(index).kind;
    const QSet<OperationKind> optionalKinds{
        OperationKind::Driver, OperationKind::Package, OperationKind::Feature,
        OperationKind::Capability, OperationKind::Appx, OperationKind::Component,
        OperationKind::Registry, OperationKind::Unattended, OperationKind::PostSetup,
        OperationKind::Cleanup,
    };
    if (!optionalKinds.contains(kind)) {
        showError(localized(QStringLiteral("That operation is a safety or image-structure dependency and cannot be skipped."),
                            QStringLiteral("嗰項係安全／映像結構依賴，唔可以略過。")));
        return;
    }
    const QString id = m_plan.at(index).id;
    const bool willSkip = m_plan.at(index).state != OperationState::Skipped;
    mutateProject(QStringLiteral("plan: %1 %2")
        .arg(willSkip ? QStringLiteral("skip") : QStringLiteral("restore"), id),
        [id, willSkip](ProjectConfig &project) {
            QJsonArray existing = project.options.extra.value(QStringLiteral("planSkipped")).toArray();
            QJsonArray updated;
            for (const QJsonValue &value : existing)
                if (value.toString() != id) updated.append(value);
            if (willSkip) updated.append(id);
            project.options.extra.insert(QStringLiteral("planSkipped"), updated);
        });
}

void AppController::refreshHistory()
{
    m_history.clear();
    if (m_project) {
        QString error; m_history = m_project->history(500, &error);
        if (!error.isEmpty()) showError(error);
    }
    emit stateChanged();
}

void AppController::undoLatestProjectChange()
{
    if (!m_project) return;
    ActionHistory actionHistory(m_project->projectDirectory);
    QString actionError;
    const QList<ActionEvent> actions = actionHistory.events(500, &actionError);
    if (actionError.isEmpty()) {
        for (const ActionEvent &event : actions) {
            if (!event.isAction() && !event.isCompensation())
                continue;
            QString effectiveError;
            const bool effective = actionHistory.isEffective(event.id, &effectiveError);
            if (!effectiveError.isEmpty())
                continue;
            if (effective)
                undoHistoryEvent(event.id);
            else
                redoHistoryEvent(event.id);
            return;
        }
    }

    QString error;
    if (!m_project->revertLatest(&error)) { showError(error); return; }
    const QString directory = m_project->projectDirectory;
    const auto reloaded = ProjectConfig::load(directory, &error);
    if (!reloaded) { showError(error); return; }
    m_project = *reloaded;
    loadProjectState();
    showSuccess(localized(QStringLiteral("Latest project action reversed. Undo again to reverse this undo."),
                          QStringLiteral("最新工程動作已逆轉。再 Undo 一次就會逆轉今次 Undo。")));
}

QVariantList AppController::contextualHistory(const QString &contextKey,
                                              const QString &elementId) const
{
    QVariantList result;
    if (!m_project)
        return result;
    ActionHistory history(m_project->projectDirectory);
    QString error;
    QList<ActionEvent> events;
    if (contextKey.trimmed().isEmpty()) {
        events = history.events(80, &error);
    } else if (elementId.trimmed().isEmpty()) {
        const QList<ActionEvent> all = history.events(200, &error);
        for (const ActionEvent &event : all) {
            if (event.contextKey == contextKey.trimmed() && events.size() < 40)
                events.append(event);
        }
    } else {
        events = history.recentForElement(contextKey.trimmed(), elementId.trimmed(), 40, &error);
    }
    if (!error.isEmpty())
        return result;
    for (const ActionEvent &event : events) {
        bool effective = true;
        if (event.isAction() || event.isCompensation()) {
            QString effectiveError;
            effective = history.isEffective(event.id, &effectiveError);
            if (!effectiveError.isEmpty())
                effective = true;
        }
        result.append(event.toVariantMap(effective));
    }
    return result;
}

void AppController::undoContext(const QString &contextKey, const QString &elementId)
{
    if (!m_project)
        return;
    ActionHistory history(m_project->projectDirectory);
    QString error;
    QList<ActionEvent> events;
    if (contextKey.trimmed().isEmpty()) {
        events = history.events(500, &error);
    } else if (elementId.trimmed().isEmpty()) {
        const QList<ActionEvent> all = history.events(500, &error);
        for (const ActionEvent &event : all)
            if (event.contextKey == contextKey.trimmed()) events.append(event);
    } else {
        events = history.recentForElement(contextKey.trimmed(), elementId.trimmed(), 200, &error);
    }
    if (!error.isEmpty()) {
        showError(error);
        return;
    }
    for (const ActionEvent &event : events) {
        if (!event.isAction() && !event.isCompensation())
            continue;
        QString effectiveError;
        const bool effective = history.isEffective(event.id, &effectiveError);
        if (!effectiveError.isEmpty())
            continue;
        if (effective)
            undoHistoryEvent(event.id);
        else
            redoHistoryEvent(event.id);
        return;
    }
    if (!contextKey.trimmed().isEmpty()) {
        const QList<ActionEvent> all = history.events(500, &error);
        if (!error.isEmpty()) {
            showError(error);
            return;
        }
        for (const ActionEvent &event : all) {
            if (!event.isAction() && !event.isCompensation())
                continue;
            QString effectiveError;
            const bool effective = history.isEffective(event.id, &effectiveError);
            if (!effectiveError.isEmpty())
                continue;
            if (effective)
                undoHistoryEvent(event.id);
            else
                redoHistoryEvent(event.id);
            return;
        }
    }
    emit snackbarRequested(localized(QStringLiteral("Nothing undoable in this context yet."),
                                     QStringLiteral("呢個位置暫時冇嘢可以 Undo。")), QStringLiteral("info"));
}

bool AppController::applyHistoryState(const ActionEvent &event, const QString &message)
{
    if (!m_project)
        return false;
    QString error;
    const QString directory = m_project->projectDirectory;
    const QJsonObject current = m_project->toJson();
    QJsonObject patch = event.forwardDiff;
    QJsonObject expectedPatch = event.inverseDiff;
    const bool fullState = event.metadata.value(QStringLiteral("fullProjectState")).toBool()
        || (event.metadata.value(QStringLiteral("stateFormat")).toString().isEmpty()
            && event.forwardDiff.contains(QStringLiteral("schemaVersion"))
            && event.forwardDiff.contains(QStringLiteral("projectName")));
    if (fullState) {
        patch = ActionHistory::createMergePatch(event.inverseDiff, event.forwardDiff);
        expectedPatch = ActionHistory::createMergePatch(event.forwardDiff, event.inverseDiff);
    }
    const MergePatchApplyResult merged = ActionHistory::applyMergePatchGuarded(
        current, patch, expectedPatch);
    if (!merged.applied) {
        showError(QStringLiteral(
            "This action cannot be changed selectively because later work modified: %1. "
            "Undo the conflicting newer action first, or use an explicit restore point.")
                      .arg(merged.conflicts.join(QStringLiteral(", "))));
        return false;
    }
    const auto restored = ProjectConfig::fromJson(merged.state, directory, &error);
    if (!restored) {
        showError(QStringLiteral("The selected history state could not be restored: %1").arg(error));
        return false;
    }
    if (!restored->save(&error, message)) {
        showError(error);
        return false;
    }
    m_project = *restored;
    loadProjectState();
    return true;
}

bool AppController::undoHistoryEvent(const QString &eventId)
{
    if (!m_project)
        return false;
    ActionHistory history(m_project->projectDirectory);
    ActionEvent compensation;
    QString error;
    if (!history.undoAction(eventId, &compensation, &error)) {
        showError(error);
        return false;
    }
    if (!applyHistoryState(compensation, compensation.title)) {
        ActionEvent repair;
        QString repairError;
        history.redoAction(eventId, &repair, &repairError);
        return false;
    }
    showSuccess(localized(QStringLiteral("Action undone. The undo is itself recorded and undoable."),
                          QStringLiteral("動作已 Undo；今次 Undo 自己都有記錄，亦可以 Undo。")));
    return true;
}

bool AppController::redoHistoryEvent(const QString &eventId)
{
    if (!m_project)
        return false;
    ActionHistory history(m_project->projectDirectory);
    ActionEvent compensation;
    QString error;
    if (!history.redoAction(eventId, &compensation, &error)) {
        showError(error);
        return false;
    }
    if (!applyHistoryState(compensation, compensation.title)) {
        ActionEvent repair;
        QString repairError;
        history.undoAction(eventId, &repair, &repairError);
        return false;
    }
    showSuccess(localized(QStringLiteral("Action redone as a new history event."),
                          QStringLiteral("動作已用新歷史事件 Redo。")));
    return true;
}

bool AppController::restoreHistoryEvent(const QString &eventId)
{
    if (!m_project)
        return false;
    ActionHistory history(m_project->projectDirectory);
    QString error;
    const QList<ActionEvent> events = history.events(2'000, &error);
    const auto selected = std::find_if(events.cbegin(), events.cend(),
        [&eventId](const ActionEvent &event) { return event.id == eventId; });
    if (selected == events.cend() || !selected->isAction()) {
        showError(error.isEmpty() ? QStringLiteral("The selected restore point no longer exists.") : error);
        return false;
    }
    const QJsonObject before = m_project->toJson();
    const QString directory = m_project->projectDirectory;
    QJsonObject restoredState = selected->metadata.value(QStringLiteral("afterState")).toObject();
    if (restoredState.isEmpty())
        restoredState = selected->forwardDiff;
    const auto restored = ProjectConfig::fromJson(restoredState, directory, &error);
    if (!restored || !restored->save(&error, QStringLiteral("history: restore event #%1")
                                                   .arg(selected->sequence))) {
        showError(error);
        return false;
    }
    m_project = *restored;
    ActionDraft draft;
    draft.title = QStringLiteral("Restore history event #%1").arg(selected->sequence);
    draft.description = selected->title;
    draft.icon = QStringLiteral("restore");
    draft.contextKey = selected->contextKey;
    draft.elementId = selected->elementId;
    const QJsonObject after = restored->toJson();
    draft.forwardDiff = ActionHistory::createMergePatch(before, after);
    draft.inverseDiff = ActionHistory::createMergePatch(after, before);
    draft.metadata = QJsonObject{{QStringLiteral("diffSummary"), draft.title},
                                 {QStringLiteral("restoredEventId"), selected->id},
                                 {QStringLiteral("stateFormat"), QStringLiteral("merge-patch")},
                                 {QStringLiteral("beforeState"), before},
                                 {QStringLiteral("afterState"), after}};
    if (!history.record(draft, nullptr, &error)) {
        showError(error);
        return false;
    }
    loadProjectState();
    showSuccess(localized(QStringLiteral("Restored that point as a new reversible action."),
                          QStringLiteral("已將嗰個時間點還原成新嘅可逆動作。")));
    return true;
}

bool AppController::bookmarkHistoryEvent(const QString &eventId, const QString &name)
{
    if (!m_project)
        return false;
    QString error;
    if (!ActionHistory(m_project->projectDirectory).createBookmark(name, eventId, nullptr, &error)) {
        showError(error);
        return false;
    }
    refreshHistory();
    return true;
}

bool AppController::branchHistoryEvent(const QString &eventId, const QString &name)
{
    if (!m_project)
        return false;
    QString error;
    if (!ActionHistory(m_project->projectDirectory).createBranch(name, eventId, nullptr, &error)) {
        showError(error);
        return false;
    }
    refreshHistory();
    return true;
}

bool AppController::switchHistoryBranch(const QString &name)
{
    if (!m_project)
        return false;
    QString error;
    if (!ActionHistory(m_project->projectDirectory).switchBranch(name, nullptr, &error)) {
        showError(error);
        return false;
    }
    refreshHistory();
    showSuccess(localized(QStringLiteral("History lane switched to %1.").arg(name),
                          QStringLiteral("歷史分支已轉去 %1。 ").arg(name)));
    return true;
}

void AppController::undoLatestNotificationChange()
{
    QString error;
    if (!m_notificationStore.revertLatest(&error)) { showError(error); return; }
    refreshNotifications();
    emit snackbarRequested(localized(QStringLiteral("Notification action reversed; repeat to redo it."),
                                     QStringLiteral("通知動作已逆轉；再做一次就 redo。")), QStringLiteral("success"));
}

void AppController::markNotificationRead(const QString &id) { QString e; if (!m_notificationStore.markRead(id, &e)) showError(e); refreshNotifications(); }
void AppController::markNotificationUnread(const QString &id) { QString e; if (!m_notificationStore.markUnread(id, &e)) showError(e); refreshNotifications(); }
void AppController::dismissNotification(const QString &id) { QString e; if (!m_notificationStore.dismiss(id, &e)) showError(e); refreshNotifications(); }
void AppController::deleteNotification(const QString &id) { QString e; if (!m_notificationStore.softDelete(id, &e)) showError(e); refreshNotifications(); }
void AppController::restoreNotification(const QString &id) { QString e; if (!m_notificationStore.restore(id, &e)) showError(e); refreshNotifications(); }

void AppController::sendTestNotification()
{
    notify(QStringLiteral("Test notification"), localized(
        QStringLiteral("This notification and every action you take on it are now committed to the notification repository."),
        QStringLiteral("呢個通知連你之後點樣搞佢，都已經 commit 去通知倉。走唔甩啦。")), QStringLiteral("info"));
}

void AppController::resumeRecovery()
{
    refreshPlan();
    emit recoveryReviewRequested();
    showSuccess(localized(
        QStringLiteral("The plan was rebuilt for review. Completed external steps are not guessed or skipped after a crash."),
        QStringLiteral("計劃已重新建立畀你檢查。死機之後唔會估外部工序做完未，更加唔會亂跳步。")));
}

void AppController::rollbackRecovery()
{
    undoLatestProjectChange();
    notify(QStringLiteral("Latest configuration change undone"), QStringLiteral("Only project configuration was reversed. External servicing side effects were not claimed as rolled back; the pristine source remains the clean baseline."), QStringLiteral("warning"));
}

void AppController::safeUnmountRecovery()
{
    if (!m_project || !m_pendingRecovery) {
        showError(QStringLiteral("There is no interrupted servicing run to unmount."));
        return;
    }
    if (m_recoveryUnmountBusy) {
        emit snackbarRequested(QStringLiteral("Recovery unmount is already running."),
                               QStringLiteral("info"));
        return;
    }
    if (m_jobEngine.isRunning()) {
        showError(QStringLiteral("Wait for the active servicing run before recovering an older mount."));
        return;
    }

    // Recovery must use the immutable path captured by the interrupted run. The
    // user may have edited the current project mount path since the crash.
    const QString mountPath = cleanPath(m_recoveryJournal.value(QStringLiteral("mountPath")).toString());
    const QString runId = m_recoveryJournal.value(QStringLiteral("runId")).toString().trimmed();
    if (mountPath.isEmpty() || !QDir::isAbsolutePath(mountPath) || runId.isEmpty()) {
        showError(QStringLiteral("The recovery journal has no valid absolute mount path or run ID."));
        return;
    }
    if (!JobEngine::isAdministrator()) {
        showError(QStringLiteral("Safe unmount needs an elevated WimForge session because DISM requires administrator rights."));
        return;
    }

    const QString journalPath = JobEngine::journalPathForProject(m_project->projectDirectory);
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("dism.exe"));
    process->setArguments({QStringLiteral("/English"), QStringLiteral("/Unmount-Image"),
                           QStringLiteral("/MountDir:%1").arg(mountPath), QStringLiteral("/Discard")});
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_recoveryUnmountBusy = true;
    notify(QStringLiteral("Recovery unmount started"),
           QStringLiteral("DISM is discarding the interrupted mount recorded by run %1. The original journal remains intact until DISM succeeds.").arg(runId),
           QStringLiteral("progress"));

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart || process->property("recoveryHandled").toBool())
            return;
        process->setProperty("recoveryHandled", true);
        m_recoveryUnmountBusy = false;
        showError(QStringLiteral("DISM could not start: %1. The recovery journal was not changed.")
                      .arg(process->errorString()));
        process->deleteLater();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, journalPath, runId, mountPath](int exitCode, QProcess::ExitStatus exitStatus) {
        if (process->property("recoveryHandled").toBool())
            return;
        process->setProperty("recoveryHandled", true);
        const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
        process->deleteLater();
        m_recoveryUnmountBusy = false;

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            showError(QStringLiteral("DISM did not discard the interrupted mount (exit %1). The recovery journal was preserved.%2")
                          .arg(exitCode)
                          .arg(output.isEmpty() ? QString() : QStringLiteral("\n\n%1").arg(output.right(3000))));
            return;
        }

        QFile journalFile(journalPath);
        if (!journalFile.open(QIODevice::ReadOnly)) {
            showError(QStringLiteral("The mount was discarded, but the original recovery journal could not be reopened: %1")
                          .arg(journalFile.errorString()));
            refreshRecoveryState();
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(journalFile.readAll(), &parseError);
        journalFile.close();
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            showError(QStringLiteral("The mount was discarded, but the recovery journal is no longer valid. It was left untouched."));
            refreshRecoveryState();
            return;
        }
        QJsonObject journal = document.object();
        if (journal.value(QStringLiteral("runId")).toString() != runId) {
            showError(QStringLiteral("The mount was discarded, but a newer recovery run replaced the journal. The newer journal was left untouched."));
            refreshRecoveryState();
            return;
        }
        const QString recoveredAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        journal.insert(QStringLiteral("status"), QStringLiteral("recovered-discarded"));
        journal.insert(QStringLiteral("recoveredAt"), recoveredAt);
        journal.insert(QStringLiteral("updatedAt"), recoveredAt);
        journal.insert(QStringLiteral("recoveryAction"), QStringLiteral("dism-unmount-discard"));
        journal.insert(QStringLiteral("recoveredMountPath"), mountPath);
        QString writeError;
        if (!writeBytes(journalPath, QJsonDocument(journal).toJson(QJsonDocument::Indented), &writeError)) {
            showError(QStringLiteral("The mount was discarded, but its journal status could not be saved: %1")
                          .arg(writeError));
            refreshRecoveryState();
            return;
        }
        refreshRecoveryState();
        emit stateChanged();
        notify(QStringLiteral("Interrupted mount discarded"),
               QStringLiteral("DISM succeeded and recovery run %1 is now closed without claiming its external steps were rolled back.").arg(runId),
               QStringLiteral("success"));
    });
    process->start();
}

void AppController::openUnattendGenerator()
{
    emit unattendedStudioRequested();
    emit snackbarRequested(localized(QStringLiteral("Unattended Studio is opening inside WimForge."),
                                     QStringLiteral("無人值守工房喺 WimForge 入面開緊。")), QStringLiteral("info"));
}

void AppController::search(const QString &query)
{
    if (query.trimmed().isEmpty()) return;
    emit searchRequested(query.trimmed());
    emit snackbarRequested(localized(QStringLiteral("Searching local features, GPO docs, packages and commands for: %1").arg(query.trimmed()),
                                     QStringLiteral("搜尋本機功能、GPO 文件、套件同指令：%1").arg(query.trimmed())), QStringLiteral("info"));
}

void AppController::copyText(const QString &text)
{
    if (QGuiApplication::clipboard()) QGuiApplication::clipboard()->setText(text);
    emit snackbarRequested(localized(QStringLiteral("Copied to clipboard."), QStringLiteral("已複製去剪貼簿。")), QStringLiteral("success"));
}

void AppController::restoreStudioState()
{
    bool packageLoaded = false;
    bool unattendLoaded = false;
    bool bridgeLoaded = false;
    if (m_project) {
        const QJsonValue packageValue = m_project->settings.value(QStringLiteral("_packageProfile"));
        if (packageValue.isObject()) {
            QString error;
            const auto profile = PackageStudio::fromJson(packageValue.toObject(), &error);
            if (profile) {
                m_packageProfile = *profile;
                packageLoaded = true;
            }
        }
        const QJsonValue unattendedValue = m_project->settings.value(QStringLiteral("_unattendProfile"));
        if (unattendedValue.isObject()) {
            QString error;
            const auto profile = UnattendProfile::fromJson(unattendedValue.toObject(), &error);
            if (profile) {
                m_unattendProfile = *profile;
                unattendLoaded = true;
            }
        }
        const QJsonValue bridgeValue = m_project->settings.value(QStringLiteral("_winForgeRecipe"));
        if (bridgeValue.isObject()) {
            QString error;
            const auto recipe = WinForgeBridge::fromJson(bridgeValue.toObject(), &error);
            if (recipe) {
                m_winForgeRecipe = *recipe;
                bridgeLoaded = true;
            } else {
                m_winForgeBridgeStatus = QStringLiteral("Saved WinForge recipe is invalid: %1").arg(error);
            }
        }
        const QJsonValue includeRuntime = m_project->settings.value(
            QStringLiteral("_winForgeIncludeRuntime"));
        if (includeRuntime.isBool())
            m_winForgeIncludeRuntime = includeRuntime.toBool();
        const QJsonValue runtimePath = m_project->settings.value(
            QStringLiteral("_winForgeRuntimePath"));
        if (runtimePath.isString())
            m_winForgeRuntimePath = cleanPath(runtimePath.toString());
    }
    if (!packageLoaded) {
        m_packageProfile = PackageStudio::fullAiDevelopmentTemplate();
        m_packageProfile.name = QStringLiteral("Custom software profile");
        m_packageProfile.description = QStringLiteral("Choose exactly what the finished Windows image installs.");
        for (PackageEntry &package : m_packageProfile.packages)
            package.enabled = false;
    }
    if (!unattendLoaded) {
        m_unattendProfile = UnattendProfile{};
        m_unattendProfile.name = QStringLiteral("Custom unattended setup");
        m_unattendProfile.description = QStringLiteral("All Windows setup passes remain available through the generic setting editor.");
    }
    if (!bridgeLoaded)
        m_winForgeRecipe = emptyWinForgeRecipe();
}

void AppController::loadGpoCatalog()
{
    if (m_gpoLoaded) {
        emit studioChanged();
        return;
    }
    m_gpoStatus = localized(QStringLiteral("Loading installed ADMX and ADML policy definitions…"),
                            QStringLiteral("載入已安裝 ADMX 同 ADML 政策定義…"));
    emit studioChanged();
    QString error;
    if (!m_gpoCatalog.loadInstalled({QStringLiteral("en-US"), QStringLiteral("zh-HK")}, &error)) {
        m_gpoStatus = error;
        showError(error);
        emit studioChanged();
        return;
    }
    m_gpoLoaded = true;
    const int initialCount = qMin(120, m_gpoCatalog.policies().size());
    m_gpoSearchResults = m_gpoCatalog.policies().mid(0, initialCount);
    m_gpoStatus = localized(
        QStringLiteral("%1 policies loaded from %2. Showing %3; search to narrow them.")
            .arg(m_gpoCatalog.policies().size()).arg(m_gpoCatalog.policyDefinitionsPath()).arg(initialCount),
        QStringLiteral("由 %2 載入 %1 項政策。暫時顯示 %3 項；搜尋就可以收窄。")
            .arg(m_gpoCatalog.policies().size()).arg(m_gpoCatalog.policyDefinitionsPath()).arg(initialCount));
    if (!m_gpoCatalog.warnings().isEmpty())
        notify(QStringLiteral("GPO locale notice"), m_gpoCatalog.warnings().join(QLatin1Char('\n')), QStringLiteral("warning"));
    emit studioChanged();
}

void AppController::searchGpo(const QString &query, bool regularExpression)
{
    if (!m_gpoLoaded)
        loadGpoCatalog();
    if (!m_gpoLoaded)
        return;
    QString error;
    QList<GpoPolicy> results = m_gpoCatalog.search(
        query.trimmed(), regularExpression ? GpoSearchMode::RegularExpression
                                           : GpoSearchMode::PlainText, &error);
    if (!error.isEmpty()) {
        m_gpoStatus = error;
        emit studioChanged();
        return;
    }
    constexpr int resultLimit = 300;
    m_gpoSearchResults = results.mid(0, resultLimit);
    m_gpoStatus = localized(
        QStringLiteral("%1 match(es)%2.").arg(results.size())
            .arg(results.size() > resultLimit ? QStringLiteral("; showing the first %1").arg(resultLimit) : QString()),
        QStringLiteral("搵到 %1 項%2。 ").arg(results.size())
            .arg(results.size() > resultLimit ? QStringLiteral("；顯示頭 %1 項").arg(resultLimit) : QString()));
    emit studioChanged();
}

bool AppController::applyGpoPolicy(const QString &qualifiedId,
                                   const QString &state,
                                   const QVariantMap &elementValues)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before applying policy configuration."));
        return false;
    }
    const auto found = std::find_if(m_gpoCatalog.policies().cbegin(), m_gpoCatalog.policies().cend(),
        [&qualifiedId](const GpoPolicy &policy) { return policy.qualifiedId() == qualifiedId; });
    if (found == m_gpoCatalog.policies().cend()) {
        showError(QStringLiteral("The selected GPO is no longer present in the loaded catalog."));
        return false;
    }
    const GpoPolicy policy = *found;
    const GpoPolicyCompilation compilation = compileGpoPolicy(
        policy, state, elementValues, m_project->registryTweaks);
    if (!compilation.ok()) {
        showError(compilation.error);
        return false;
    }
    const bool saved = mutateProject(
        QStringLiteral("gpo: %1 %2").arg(state, policy.displayName),
        [compilation](ProjectConfig &project) {
            mergeGpoPolicyCompilation(project, compilation);
        });
    if (saved)
        showSuccess(localized(QStringLiteral("Policy action committed to project history."),
                              QStringLiteral("政策動作已 commit 入工程歷史。")));
    return saved;
}

bool AppController::exportGpoDocumentation(const QString &destinationFile)
{
    if (!m_gpoLoaded)
        loadGpoCatalog();
    if (!m_gpoLoaded)
        return false;
    QString error;
    if (!m_gpoCatalog.exportMarkdown(cleanPath(destinationFile), QStringLiteral("en-US"),
                                     QStringLiteral("zh-HK"), &error)) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Complete GPO documentation exported."),
                          QStringLiteral("完整 GPO 文件已匯出。")));
    return true;
}

void AppController::askOpenCodeForGpo(const QString &intent)
{
    const QString request = intent.trimmed();
    if (request.isEmpty())
        return;
    runOpenCode(QStringLiteral(
        "You are helping search a local Windows ADMX policy catalog. Convert the user's intent "
        "into a short plain-text AND-token search query. Return only the search query, no prose. Intent: %1")
        .arg(request), [this](const QString &answer) {
            QString query = answer.section(QLatin1Char('\n'), -1).trimmed();
            query.remove(QLatin1Char('`'));
            searchGpo(query, false);
            emit snackbarRequested(localized(QStringLiteral("OpenCode suggested: %1").arg(query),
                                             QStringLiteral("OpenCode 建議搜尋：%1").arg(query)),
                                     QStringLiteral("success"));
        });
}

bool AppController::persistPackageProfile(const QString &message)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return false;
    }
    const QString profilePath = QDir(m_project->projectDirectory)
        .filePath(QStringLiteral(".wimforge/generated/packages/profile.json"));
    QString error;
    if (!writeBytes(profilePath,
                    QJsonDocument(PackageStudio::toJson(m_packageProfile)).toJson(QJsonDocument::Indented),
                    &error)) {
        showError(error);
        return false;
    }
    const QJsonObject profileJson = PackageStudio::toJson(m_packageProfile);
    return mutateProject(message, [profileJson](ProjectConfig &project) {
        project.settings.insert(QStringLiteral("_packageProfile"), profileJson);
    });
}

void AppController::loadAiDevelopmentPackageTemplate()
{
    m_packageProfile = PackageStudio::fullAiDevelopmentTemplate();
    if (persistPackageProfile(QStringLiteral("packages: select Full AI Development ISO template"))) {
        notify(QStringLiteral("AI development template selected"),
               QStringLiteral("Codex, Claude Code, Claude Desktop, OpenCode, Node/npm, Python, CMake, Java, Git, VS Build Tools, VS Code and Docker are in the reproducible profile. Desktop payloads without an official package ID stay visibly optional."),
               QStringLiteral("success"));
    }
    emit studioChanged();
}

void AppController::setPackageEnabled(const QString &id, bool enabled)
{
    bool found = false;
    for (PackageEntry &package : m_packageProfile.packages) {
        if (package.id == id) {
            package.enabled = enabled;
            found = true;
            break;
        }
    }
    if (!found)
        return;
    if (enabled) {
        QSet<QString> wanted{id};
        bool changed = true;
        while (changed) {
            changed = false;
            for (PackageEntry &package : m_packageProfile.packages) {
                if (!wanted.contains(package.id))
                    continue;
                for (const QString &dependency : package.dependencies) {
                    if (!wanted.contains(dependency)) {
                        wanted.insert(dependency);
                        changed = true;
                    }
                }
            }
        }
        for (PackageEntry &package : m_packageProfile.packages)
            if (wanted.contains(package.id)) package.enabled = true;
    }
    persistPackageProfile(QStringLiteral("packages: %1 %2")
        .arg(enabled ? QStringLiteral("select") : QStringLiteral("remove"), id));
    emit studioChanged();
}

bool AppController::importPackageProfile(const QString &sourceFile)
{
    QString error;
    const auto profile = PackageStudio::importJson(cleanPath(sourceFile), &error);
    if (!profile) {
        showError(error);
        return false;
    }
    m_packageProfile = *profile;
    const bool saved = persistPackageProfile(QStringLiteral("packages: import profile"));
    emit studioChanged();
    return saved;
}

bool AppController::exportPackageProfile(const QString &destinationFile)
{
    QString error;
    if (!PackageStudio::exportJson(m_packageProfile, cleanPath(destinationFile), &error)) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Package profile exported."),
                          QStringLiteral("套件設定已匯出。")));
    return true;
}

bool AppController::stagePackageProfile()
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return false;
    }
    QString error;
    const QString bundleRoot = QDir(m_project->projectDirectory)
        .filePath(QStringLiteral(".wimforge/generated/packages/bundle"));
    const auto bundle = PackageStudio::materializeFirstLogonBundle(
        m_packageProfile, m_project->projectDirectory, bundleRoot, &error);
    if (!bundle) {
        showError(error);
        return false;
    }
    const QJsonObject profileJson = PackageStudio::toJson(m_packageProfile);
    const QJsonObject stagingManifest = PackageStudio::generateIsoStagingManifest(
        m_packageProfile, &error);
    const QList<PackageStagedFile> stagedFiles = bundle->files;
    const bool saved = mutateProject(QStringLiteral("packages: stage profile into image"),
        [profileJson, stagingManifest, stagedFiles](ProjectConfig &project) {
            project.settings.insert(QStringLiteral("_packageProfile"), profileJson);
            project.settings.insert(QStringLiteral("_packageStagingManifest"), stagingManifest);
            QJsonArray staged = project.options.extra.value(QStringLiteral("stagedFiles")).toArray();
            QJsonArray filtered;
            for (const QJsonValue &item : staged) {
                if (!item.toObject().value(QStringLiteral("role")).toString()
                         .startsWith(QStringLiteral("package-"))) {
                    filtered.append(item);
                }
            }
            for (const PackageStagedFile &file : stagedFiles) {
                filtered.append(QJsonObject{
                    {QStringLiteral("role"), file.role},
                    {QStringLiteral("source"), file.sourcePath},
                    {QStringLiteral("destination"),
                     QStringLiteral("ProgramData/WimForge/PackageStudio/%1")
                         .arg(file.relativePath)},
                    {QStringLiteral("scope"), QStringLiteral("image")},
                    {QStringLiteral("sha256"), file.sha256},
                });
            }
            project.options.extra.insert(QStringLiteral("stagedFiles"), filtered);
            project.postSetupItems.erase(
                std::remove_if(project.postSetupItems.begin(), project.postSetupItems.end(),
                    [](const QString &item) {
                        return item.contains(QStringLiteral("WimForge-Packages.ps1"),
                                             Qt::CaseInsensitive)
                            || item.contains(QStringLiteral("register-first-logon.ps1"),
                                             Qt::CaseInsensitive);
                    }), project.postSetupItems.end());
            const QString command = QStringLiteral(
                "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                "-File \"%ProgramData%\\WimForge\\PackageStudio\\register-first-logon.ps1\"");
            if (!project.postSetupItems.contains(command))
                project.postSetupItems.append(command);
        });
    if (saved)
        notify(QStringLiteral("Package profile staged"),
                QStringLiteral("%1 verified bundle files were staged. Setup registers an elevated interactive first-logon task; failed packages retry at a later administrator logon.")
                    .arg(stagedFiles.size()),
                QStringLiteral("success"));
    return saved;
}

bool AppController::persistUnattendedProfile(const QString &message, bool writeXml)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return false;
    }
    QString xmlPath = m_project->unattendedXmlPath;
    QString error;
    if (writeXml) {
        xmlPath = QDir(m_project->projectDirectory)
            .filePath(QStringLiteral(".wimforge/generated/unattended/autounattend.xml"));
        if (!m_unattendProfile.exportXml(xmlPath, &error)) {
            showError(error);
            return false;
        }
    }
    const QJsonObject profileJson = m_unattendProfile.toJson();
    const UnattendProfile profile = m_unattendProfile;
    return mutateProject(message, [profileJson, profile, xmlPath](ProjectConfig &project) {
        project.settings.insert(QStringLiteral("_unattendProfile"), profileJson);
        if (!xmlPath.isEmpty())
            project.unattendedXmlPath = xmlPath;
        QJsonArray staged = project.options.extra.value(QStringLiteral("stagedFiles")).toArray();
        QJsonArray filtered;
        for (const QJsonValue &item : staged) {
            if (item.toObject().value(QStringLiteral("role")).toString() != QStringLiteral("unattended-answer"))
                filtered.append(item);
        }
        if (!xmlPath.isEmpty() && profile.copyToMediaRoot) {
            filtered.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("unattended-answer")},
                {QStringLiteral("source"), xmlPath},
                {QStringLiteral("destination"), QStringLiteral("autounattend.xml")},
                {QStringLiteral("scope"), QStringLiteral("media")},
            });
        }
        if (!xmlPath.isEmpty() && profile.copyToInstallImage) {
            filtered.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("unattended-answer")},
                {QStringLiteral("source"), xmlPath},
                {QStringLiteral("destination"), QStringLiteral("Windows/Panther/unattend.xml")},
                {QStringLiteral("scope"), QStringLiteral("image")},
            });
        }
        project.options.extra.insert(QStringLiteral("stagedFiles"), filtered);
    });
}

void AppController::loadUnattendedTemplate(const QString &templateId)
{
    m_unattendProfile = templateId.compare(QStringLiteral("ai-development"), Qt::CaseInsensitive) == 0
        ? UnattendBuilder::aiDevelopmentTemplate() : UnattendBuilder::fullAutomationTemplate();
    persistUnattendedProfile(QStringLiteral("unattended: select %1 template").arg(templateId));
    emit studioChanged();
}

void AppController::setComputerNameBehavior(int mode, const QString &value)
{
    const int bounded = qBound(0, mode, 3);
    m_unattendProfile.computerNameMode = static_cast<ComputerNameMode>(bounded);
    if (m_unattendProfile.computerNameMode == ComputerNameMode::SerialNumber)
        m_unattendProfile.serialPrefix = value.trimmed();
    else
        m_unattendProfile.computerName = value.trimmed();
    m_unattendProfile.applyComputerNameBehavior();
    persistUnattendedProfile(QStringLiteral("unattended: change computer-name behavior"));
    emit studioChanged();
}

void AppController::setUnattendedValue(const QString &pass,
                                       const QString &component,
                                       const QString &path,
                                       const QString &value)
{
    const auto parsedPass = UnattendBuilder::parsePass(pass);
    if (!parsedPass || component.trimmed().isEmpty() || path.trimmed().isEmpty()) {
        showError(QStringLiteral("Pass, component, and setting path are required."));
        return;
    }
    m_unattendProfile.setValue(*parsedPass, component.trimmed(),
                               path.split(QLatin1Char('/'), Qt::SkipEmptyParts), value);
    persistUnattendedProfile(QStringLiteral("unattended: set %1/%2/%3")
        .arg(pass, component.trimmed(), path.trimmed()));
    emit studioChanged();
}

bool AppController::importUnattended(const QString &sourceFile)
{
    QString error;
    const QString path = cleanPath(sourceFile);
    const auto profile = QFileInfo(path).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0
        ? UnattendProfile::importJson(path, &error) : UnattendProfile::importXml(path, &error);
    if (!profile) {
        showError(error);
        return false;
    }
    m_unattendProfile = *profile;
    const bool saved = persistUnattendedProfile(QStringLiteral("unattended: import answer file"));
    emit studioChanged();
    return saved;
}

bool AppController::exportUnattended(const QString &destinationFile)
{
    QString error;
    const QString path = cleanPath(destinationFile);
    const bool ok = QFileInfo(path).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0
        ? m_unattendProfile.exportJson(path, &error) : m_unattendProfile.exportXml(path, &error);
    if (!ok) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Unattended profile exported."),
                          QStringLiteral("無人值守設定已匯出。")));
    return true;
}

void AppController::verifyOpenCodeThen(const std::function<void()> &completed, bool installedNow)
{
    const QString executable = openCodeExecutablePath();
    if (executable.isEmpty()) {
        m_openCodeBusy = false;
        m_openCodeVerified = false;
        m_openCodeStatus = QStringLiteral(
            "OpenCode installation returned success, but no executable was found in PATH or the npm prefix.");
        showError(m_openCodeStatus);
        emit studioChanged();
        return;
    }
    auto *process = new QProcess(this);
    process->setProgram(executable);
    process->setArguments({QStringLiteral("--version")});
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart
            || process->property("wimforgeHandled").toBool()) {
            return;
        }
        process->setProperty("wimforgeHandled", true);
        m_openCodeBusy = false;
        m_openCodeVerified = false;
        m_openCodeStatus = QStringLiteral("OpenCode live verification could not start: %1")
                               .arg(process->errorString());
        showError(m_openCodeStatus);
        emit studioChanged();
        process->deleteLater();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, completed, installedNow](int code, QProcess::ExitStatus status) {
        if (process->property("wimforgeHandled").toBool())
            return;
        process->setProperty("wimforgeHandled", true);
        const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
        process->deleteLater();
        m_openCodeBusy = false;
        if (status != QProcess::NormalExit || code != 0 || output.isEmpty()) {
            m_openCodeVerified = false;
            m_openCodeStatus = QStringLiteral("OpenCode live verification failed (exit %1): %2")
                                   .arg(code).arg(output);
            showError(m_openCodeStatus);
            emit studioChanged();
            return;
        }
        m_openCodeStatus = QStringLiteral("OpenCode is installed and live-verified: %1")
                               .arg(output.split(QLatin1Char('\n')).first().trimmed());
        m_openCodeVerified = true;
        if (installedNow)
            notify(QStringLiteral("OpenCode installed and verified"), m_openCodeStatus,
                   QStringLiteral("success"));
        emit studioChanged();
        completed();
    });
    process->start();
}

void AppController::installOpenCodeThen(const std::function<void()> &completed)
{
    if (m_openCodeBusy) {
        QTimer::singleShot(500, this, [this, completed] { installOpenCodeThen(completed); });
        return;
    }
    if (m_openCodeVerified) {
        completed();
        return;
    }
    if (openCodeInstalled()) {
        m_openCodeBusy = true;
        m_openCodeStatus = QStringLiteral("Live-verifying OpenCode…");
        emit studioChanged();
        verifyOpenCodeThen(completed, false);
        return;
    }
    m_openCodeBusy = true;
    m_openCodeStatus = localized(QStringLiteral("Installing OpenCode automatically…"),
                                 QStringLiteral("自動安裝 OpenCode…"));
    emit studioChanged();

    auto installWithNpm = [this, completed]() {
        const ProcessCommand npm = npmProcessCommand();
        if (npm.isEmpty()) {
            m_openCodeBusy = false;
            m_openCodeStatus = QStringLiteral("Node/npm installation completed but npm was not found; restart WimForge and try again.");
            showError(m_openCodeStatus);
            emit studioChanged();
            return;
        }
        auto *npmProcess = new QProcess(this);
        npmProcess->setProgram(npm.program);
        QStringList npmArguments = npm.prefixArguments;
        npmArguments.append({QStringLiteral("install"), QStringLiteral("-g"),
                             QStringLiteral("opencode-ai@latest")});
        npmProcess->setArguments(npmArguments);
        npmProcess->setProcessChannelMode(QProcess::MergedChannels);
        connect(npmProcess, &QProcess::errorOccurred, this,
            [this, npmProcess](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart
                    || npmProcess->property("wimforgeHandled").toBool()) {
                    return;
                }
                npmProcess->setProperty("wimforgeHandled", true);
                m_openCodeBusy = false;
                m_openCodeStatus = QStringLiteral("npm could not start: %1")
                                       .arg(npmProcess->errorString());
                showError(m_openCodeStatus);
                emit studioChanged();
                npmProcess->deleteLater();
            });
        connect(npmProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, npmProcess, completed](int code, QProcess::ExitStatus status) {
                if (npmProcess->property("wimforgeHandled").toBool())
                    return;
                npmProcess->setProperty("wimforgeHandled", true);
                const QString output = QString::fromLocal8Bit(npmProcess->readAll()).trimmed();
                npmProcess->deleteLater();
                if (status != QProcess::NormalExit || code != 0) {
                    m_openCodeBusy = false;
                    m_openCodeStatus = QStringLiteral("OpenCode installation failed: %1").arg(output);
                    showError(m_openCodeStatus);
                    emit studioChanged();
                    return;
                }
                m_openCodeStatus = QStringLiteral("OpenCode installed; running live verification…");
                emit studioChanged();
                verifyOpenCodeThen(completed, true);
            });
        npmProcess->start();
    };

    if (!npmProcessCommand().isEmpty()) {
        installWithNpm();
        return;
    }
    const QString winget = QStandardPaths::findExecutable(QStringLiteral("winget.exe"));
    if (winget.isEmpty()) {
        m_openCodeBusy = false;
        m_openCodeStatus = QStringLiteral("OpenCode needs Node/npm, and neither npm nor WinGet is available.");
        showError(m_openCodeStatus);
        emit studioChanged();
        return;
    }
    m_openCodeStatus = localized(QStringLiteral("Installing Node.js LTS, then OpenCode…"),
                                 QStringLiteral("先裝 Node.js LTS，再裝 OpenCode…"));
    emit studioChanged();
    auto *nodeProcess = new QProcess(this);
    nodeProcess->setProgram(winget);
    nodeProcess->setArguments({QStringLiteral("install"), QStringLiteral("--id"),
        QStringLiteral("OpenJS.NodeJS.LTS"), QStringLiteral("--exact"),
        QStringLiteral("--silent"), QStringLiteral("--accept-package-agreements"),
        QStringLiteral("--accept-source-agreements"), QStringLiteral("--disable-interactivity")});
    nodeProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(nodeProcess, &QProcess::errorOccurred, this,
        [this, nodeProcess](QProcess::ProcessError processError) {
            if (processError != QProcess::FailedToStart
                || nodeProcess->property("wimforgeHandled").toBool()) {
                return;
            }
            nodeProcess->setProperty("wimforgeHandled", true);
            m_openCodeBusy = false;
            m_openCodeStatus = QStringLiteral("WinGet could not start Node.js installation: %1")
                                   .arg(nodeProcess->errorString());
            showError(m_openCodeStatus);
            emit studioChanged();
            nodeProcess->deleteLater();
        });
    connect(nodeProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, nodeProcess, installWithNpm](int code, QProcess::ExitStatus status) {
            if (nodeProcess->property("wimforgeHandled").toBool())
                return;
            nodeProcess->setProperty("wimforgeHandled", true);
            const QString output = QString::fromLocal8Bit(nodeProcess->readAll()).trimmed();
            nodeProcess->deleteLater();
            if (status != QProcess::NormalExit || code != 0) {
                m_openCodeBusy = false;
                m_openCodeStatus = QStringLiteral("Node.js installation failed: %1").arg(output);
                showError(m_openCodeStatus);
                emit studioChanged();
                return;
            }
            installWithNpm();
        });
    nodeProcess->start();
}

void AppController::runOpenCode(const QString &prompt,
                                const std::function<void(const QString &)> &completed)
{
    if (!m_openCodeVerified) {
        installOpenCodeThen([this, prompt, completed] { runOpenCode(prompt, completed); });
        return;
    }
    if (m_openCodeBusy) {
        emit snackbarRequested(localized(QStringLiteral("OpenCode is already helping with another request."),
                                         QStringLiteral("OpenCode 正幫緊另一個請求。")), QStringLiteral("info"));
        return;
    }
    const QString executable = openCodeExecutablePath();
    if (executable.isEmpty()) {
        m_openCodeVerified = false;
        installOpenCodeThen([this, prompt, completed] { runOpenCode(prompt, completed); });
        return;
    }
    auto *process = new QProcess(this);
    process->setProgram(executable);
    process->setArguments({QStringLiteral("run"), prompt, QStringLiteral("--format"), QStringLiteral("json")});
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_openCodeBusy = true;
    m_openCodeStatus = localized(QStringLiteral("OpenCode is reasoning locally…"),
                                 QStringLiteral("OpenCode 喺本機諗緊…"));
    emit studioChanged();
    QPointer<QProcess> guard(process);
    QTimer::singleShot(300'000, process, [guard] {
        if (guard && guard->state() != QProcess::NotRunning)
            guard->kill();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process, completed](int code, QProcess::ExitStatus status) {
            if (process->property("wimforgeHandled").toBool())
                return;
            process->setProperty("wimforgeHandled", true);
            const QByteArray output = process->readAll();
            process->deleteLater();
            m_openCodeBusy = false;
            if (status != QProcess::NormalExit || code != 0) {
                m_openCodeStatus = QStringLiteral("OpenCode request failed: %1")
                    .arg(QString::fromUtf8(output).trimmed());
                showError(m_openCodeStatus);
                emit studioChanged();
                return;
            }
            m_openCodeStatus = QStringLiteral("OpenCode completed the request.");
            emit studioChanged();
            completed(openCodeText(output));
        });
    connect(process, &QProcess::errorOccurred, this,
        [this, process](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart
                || process->property("wimforgeHandled").toBool())
                return;
            process->setProperty("wimforgeHandled", true);
            m_openCodeBusy = false;
            m_openCodeVerified = false;
            m_openCodeStatus = process->errorString();
            showError(m_openCodeStatus);
            process->deleteLater();
            emit studioChanged();
        });
    process->start();
}

void AppController::askOpenCodeToFillUnattended(const QString &intent)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project first."));
        return;
    }
    const QString current = QString::fromUtf8(
        QJsonDocument(m_unattendProfile.toJson()).toJson(QJsonDocument::Compact));
    runOpenCode(QStringLiteral(
        "Fill a WimForge unattended profile from the user's intent. Preserve its schema and all existing "
        "fields, never invent a product key, never put secrets in plaintext, and return only the complete "
        "JSON object. For a computer-name prompt use computerNameMode=prompt; do not write literal [Prompt] "
        "into Microsoft ComputerName. Current profile: %1 User intent: %2")
        .arg(current, intent.trimmed()), [this](const QString &answer) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(unfenceJson(answer), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                showError(QStringLiteral("OpenCode returned invalid unattended JSON: %1")
                    .arg(parseError.errorString()));
                return;
            }
            QString error;
            const auto profile = UnattendProfile::fromJson(document.object(), &error);
            if (!profile) {
                showError(QStringLiteral("OpenCode's unattended result did not validate: %1").arg(error));
                return;
            }
            m_unattendProfile = *profile;
            persistUnattendedProfile(QStringLiteral("unattended: apply OpenCode-assisted fill"));
            notify(QStringLiteral("Unattended profile filled"),
                   QStringLiteral("OpenCode's proposal was validated, written to XML and committed. Use Ctrl+Z to reverse it."),
                   QStringLiteral("success"));
            emit studioChanged();
        });
}

void AppController::ensureOpenCode()
{
    installOpenCodeThen([this] {
        showSuccess(localized(QStringLiteral("OpenCode is ready."),
                              QStringLiteral("OpenCode 準備好。")));
    });
}

bool AppController::persistWinForgeBridgeRecipe(const QString &message,
                                                const WinForgeRecipe &recipe)
{
    const WinForgeBridgeValidation validation = WinForgeBridge::validateRecipe(recipe);
    if (!validation.ok()) {
        showError(validation.message());
        return false;
    }
    const QJsonObject recipeJson = WinForgeBridge::toJson(recipe);
    const QString runtimePath = m_winForgeRuntimePath;
    const bool includeRuntime = m_winForgeIncludeRuntime;
    return mutateProject(message, [recipeJson, runtimePath, includeRuntime](ProjectConfig &project) {
        project.settings.insert(QStringLiteral("_winForgeRecipe"), recipeJson);
        project.settings.insert(QStringLiteral("_winForgeRuntimePath"), runtimePath);
        project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), includeRuntime);
    });
}

void AppController::proposeWinForgeBridgeActions(const QString &intent)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before creating a WinForge recipe."));
        return;
    }
    const QString request = intent.trimmed();
    if (request.isEmpty())
        return;
    runOpenCode(QStringLiteral(
        "Translate the user's desired post-install outcome into one safe WinForge page deep-link. "
        "The verified legacy interface is WinForge.exe --page <alias>; it does not expose a hidden "
        "headless tweak CLI. Use a known alias such as dashboard, ai, aichat, packages, git, vscode, "
        "terminal, settings, or use search:<short query> when uncertain. Return only a JSON object "
        "with string fields page and title. Do not return commands, scripts, registry edits, or prose. "
        "User intent: %1").arg(request), [this](const QString &answer) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(unfenceJson(answer), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                showError(QStringLiteral("OpenCode returned an invalid WinForge proposal: %1")
                              .arg(parseError.errorString()));
                return;
            }
            const QJsonObject proposal = document.object();
            const QString page = proposal.value(QStringLiteral("page")).toString().trimmed();
            if (page.isEmpty() || page.size() > 2048 || page.contains(QChar::Null)) {
                showError(QStringLiteral("OpenCode's proposed page target did not pass validation."));
                return;
            }
            WinForgeRecipe candidate = m_winForgeRecipe;
            WinForgeAction action;
            action.id = QStringLiteral("proposal-%1")
                            .arg(QDateTime::currentMSecsSinceEpoch());
            action.idempotencyKey = action.id;
            action.kind = WinForgeActionKind::Page;
            action.phase = WinForgeActionPhase::User;
            action.enabled = false;
            action.target = page;
            candidate.actions.append(action);
            if (persistWinForgeBridgeRecipe(
                    QStringLiteral("winforge: add OpenCode proposal %1").arg(page), candidate)) {
                m_winForgeBridgeStatus = localized(
                    QStringLiteral("OpenCode proposed '%1'. Review it, then switch it on to approve.")
                        .arg(proposal.value(QStringLiteral("title")).toString(page)),
                    QStringLiteral("OpenCode 提議咗「%1」；睇清楚先撳掣批准。")
                        .arg(proposal.value(QStringLiteral("title")).toString(page)));
                emit studioChanged();
            }
        });
}

bool AppController::addWinForgeBridgeAction(const QString &kind,
                                            const QString &target,
                                            const QString &executable,
                                            const QString &argumentsJson,
                                            const QString &phase)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before editing its WinForge recipe."));
        return false;
    }
    const auto parsedKind = winForgeActionKind(kind);
    if (!parsedKind) {
        showError(QStringLiteral("Unknown WinForge bridge action kind."));
        return false;
    }
    if (*parsedKind == WinForgeActionKind::Registry || *parsedKind == WinForgeActionKind::Copy) {
        showError(QStringLiteral(
            "Registry and verified-copy actions require their full typed schema. Import a validated "
            "WinForge recipe JSON; WimForge will not guess security-sensitive fields."));
        return false;
    }

    WinForgeAction action;
    action.id = QStringLiteral("%1-%2")
                    .arg(WinForgeBridge::actionKindName(*parsedKind))
                    .arg(QDateTime::currentMSecsSinceEpoch());
    action.idempotencyKey = action.id;
    action.kind = *parsedKind;
    action.phase = phase.trimmed().compare(QStringLiteral("machine"), Qt::CaseInsensitive) == 0
        ? WinForgeActionPhase::Machine : WinForgeActionPhase::User;
    action.enabled = false;
    if (*parsedKind == WinForgeActionKind::Command) {
        action.executable = executable.trimmed();
        if (!argumentsJson.trimmed().isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument arguments = QJsonDocument::fromJson(argumentsJson.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !arguments.isArray()) {
                showError(QStringLiteral("Command arguments must be one JSON string array: %1")
                              .arg(parseError.errorString()));
                return false;
            }
            for (const QJsonValue &value : arguments.array()) {
                if (!value.isString()) {
                    showError(QStringLiteral("Every command argument must be a JSON string."));
                    return false;
                }
                action.arguments.append(value.toString());
            }
        }
    } else {
        action.target = target.trimmed();
        if (*parsedKind == WinForgeActionKind::Tweak)
            action.value = true;
    }

    WinForgeRecipe candidate = m_winForgeRecipe;
    candidate.actions.append(action);
    const bool saved = persistWinForgeBridgeRecipe(
        QStringLiteral("winforge: add %1 draft").arg(WinForgeBridge::actionKindName(*parsedKind)),
        candidate);
    if (saved) {
        m_winForgeBridgeStatus = localized(
            QStringLiteral("Draft added. Switch it on only after reviewing its exact typed fields."),
            QStringLiteral("草稿已加入；睇清楚 typed 欄位先好開啟。"));
        emit studioChanged();
    }
    return saved;
}

bool AppController::removeWinForgeBridgeAction(const QString &id)
{
    WinForgeRecipe candidate = m_winForgeRecipe;
    const qsizetype originalCount = candidate.actions.size();
    candidate.actions.erase(std::remove_if(candidate.actions.begin(), candidate.actions.end(),
        [&id](const WinForgeAction &action) { return action.id == id; }), candidate.actions.end());
    if (candidate.actions.size() == originalCount) {
        showError(QStringLiteral("The selected WinForge action no longer exists."));
        return false;
    }
    return persistWinForgeBridgeRecipe(QStringLiteral("winforge: remove action %1").arg(id),
                                       candidate);
}

bool AppController::setWinForgeBridgeActionEnabled(const QString &id, bool enabled)
{
    WinForgeRecipe candidate = m_winForgeRecipe;
    auto found = std::find_if(candidate.actions.begin(), candidate.actions.end(),
        [&id](const WinForgeAction &action) { return action.id == id; });
    if (found == candidate.actions.end()) {
        showError(QStringLiteral("The selected WinForge action no longer exists."));
        return false;
    }
    if (enabled) {
        const QString capability = winForgeCapability(found->kind);
        if (!capability.isEmpty()
            && (!m_winForgeRuntimeContract.runtimeFound
                || !m_winForgeRuntimeContract.capabilities.contains(capability)
                || !m_winForgeRuntimeContract.invocations.contains(capability))) {
            showError(QStringLiteral(
                "This action cannot be approved until the selected runtime declares '%1'.")
                          .arg(capability));
            emit studioChanged();
            return false;
        }
    }
    found->enabled = enabled;
    return persistWinForgeBridgeRecipe(
        QStringLiteral("winforge: %1 action %2")
            .arg(enabled ? QStringLiteral("approve") : QStringLiteral("disable"), id), candidate);
}

void AppController::setWinForgeBridgeIncludeRuntime(bool enabled)
{
    if (m_winForgeIncludeRuntime == enabled)
        return;
    m_winForgeIncludeRuntime = enabled;
    m_settings.setValue(QStringLiteral("bridge/includeRuntime"), enabled);
    if (m_project) {
        mutateProject(QStringLiteral("winforge: change self-contained runtime bundling"),
            [enabled](ProjectConfig &project) {
                project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), enabled);
            });
    }
    emit studioChanged();
}

void AppController::setWinForgeBridgeRuntimePath(const QString &path)
{
    const QString cleaned = cleanPath(path);
    if (m_winForgeRuntimePath == cleaned)
        return;
    m_winForgeRuntimePath = cleaned;
    m_winForgeRuntimeContract = {};
    m_winForgeRuntimeStatus = QStringLiteral("Runtime path changed; detect its contract before approval.");
    m_settings.setValue(QStringLiteral("bridge/runtimePath"), cleaned);
    if (m_project) {
        mutateProject(QStringLiteral("winforge: select runtime folder"),
            [cleaned](ProjectConfig &project) {
                project.settings.insert(QStringLiteral("_winForgeRuntimePath"), cleaned);
            });
    }
    emit studioChanged();
}

void AppController::detectWinForgeBridgeRuntime()
{
    QString error;
    m_winForgeRuntimeContract = WinForgeBridge::detectRuntimeContract(m_winForgeRuntimePath, &error);
    if (!m_winForgeRuntimeContract.runtimeFound) {
        m_winForgeRuntimeStatus = error.isEmpty()
            ? QStringLiteral("No compatible WinForge runtime was found.") : error;
        showError(m_winForgeRuntimeStatus);
        emit studioChanged();
        return;
    }
    const WinForgeBridgeValidation validation = WinForgeBridge::validateAgainstRuntime(
        m_winForgeRecipe, m_winForgeRuntimeContract);
    m_winForgeRuntimeStatus = QStringLiteral("%1 runtime · contract %2 · %3")
        .arg(m_winForgeRuntimeContract.declaredContract ? QStringLiteral("Declared")
                                                        : QStringLiteral("Legacy"))
        .arg(m_winForgeRuntimeContract.contractVersion)
        .arg(m_winForgeRuntimeContract.capabilities.join(QStringLiteral(", ")));
    m_winForgeBridgeStatus = validation.ok()
        ? QStringLiteral("Recipe and runtime contract are compatible.")
        : validation.message();
    showSuccess(localized(QStringLiteral("WinForge runtime contract detected."),
                          QStringLiteral("WinForge runtime contract 偵測完成。")));
    emit studioChanged();
}

bool AppController::importWinForgeBridgeRecipe(const QString &sourceFile)
{
    QString error;
    const auto recipe = WinForgeBridge::importJson(cleanPath(sourceFile), &error);
    if (!recipe) {
        showError(error);
        return false;
    }
    const bool saved = persistWinForgeBridgeRecipe(
        QStringLiteral("winforge: import validated recipe %1").arg(recipe->id), *recipe);
    if (saved)
        showSuccess(localized(QStringLiteral("WinForge recipe imported and committed."),
                              QStringLiteral("WinForge recipe 已匯入同 commit。")));
    return saved;
}

bool AppController::exportWinForgeBridgeRecipe(const QString &destinationFile)
{
    QString error;
    if (!WinForgeBridge::exportJson(m_winForgeRecipe, cleanPath(destinationFile), &error)) {
        showError(error);
        return false;
    }
    showSuccess(localized(QStringLiteral("Portable WinForge recipe exported."),
                          QStringLiteral("可攜 WinForge recipe 已匯出。")));
    return true;
}

bool AppController::stageWinForgeBridgeIntoIso(const QString &isoStagingPath)
{
    if (!m_project) {
        showError(QStringLiteral("Open a project before staging its WinForge bridge."));
        return false;
    }
    const QString staging = cleanPath(isoStagingPath);
    if (staging.isEmpty() || !QFileInfo(staging).isAbsolute()) {
        showError(QStringLiteral("The ISO staging folder must be an absolute path."));
        return false;
    }
    WinForgeStageOptions options;
    options.includeRuntime = m_winForgeIncludeRuntime;
    options.overwriteExisting = true;
    options.payloadDirectory = QDir(m_project->projectDirectory)
        .filePath(QStringLiteral(".wimforge/winforge-payloads"));
    QString error;
    const auto staged = WinForgeBridge::stageForIso(
        m_winForgeRecipe, m_winForgeRuntimePath, staging, options, &error);
    if (!staged) {
        showError(error);
        return false;
    }

    const QString oemSource = QDir(staging).filePath(QStringLiteral("sources/$OEM$"));
    const QJsonObject recipeJson = WinForgeBridge::toJson(m_winForgeRecipe);
    const QString runtimePath = m_winForgeRuntimePath;
    const bool includeRuntime = m_winForgeIncludeRuntime;
    const QJsonObject stageInfo{
        {QStringLiteral("stagingRoot"), staging},
        {QStringLiteral("bundleDirectory"), staged->bundleDirectory},
        {QStringLiteral("manifest"), staged->manifestPath},
        {QStringLiteral("manifestSha256"), staged->manifestSha256},
    };
    const bool saved = mutateProject(QStringLiteral("winforge: stage approved bridge into ISO plan"),
        [recipeJson, runtimePath, includeRuntime, stageInfo, oemSource](ProjectConfig &project) {
            project.settings.insert(QStringLiteral("_winForgeRecipe"), recipeJson);
            project.settings.insert(QStringLiteral("_winForgeRuntimePath"), runtimePath);
            project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), includeRuntime);
            project.settings.insert(QStringLiteral("_winForgeLastStage"), stageInfo);
            QJsonArray existing = project.options.extra.value(QStringLiteral("stagedFiles")).toArray();
            QJsonArray updated;
            for (const QJsonValue &value : existing) {
                if (!value.isObject()
                    || value.toObject().value(QStringLiteral("role")).toString()
                        != QStringLiteral("winforge-bridge")) {
                    updated.append(value);
                }
            }
            updated.append(QJsonObject{
                {QStringLiteral("source"), oemSource},
                {QStringLiteral("destination"), QStringLiteral("sources/$OEM$")},
                {QStringLiteral("scope"), QStringLiteral("media")},
                {QStringLiteral("role"), QStringLiteral("winforge-bridge")},
            });
            project.options.extra.insert(QStringLiteral("stagedFiles"), updated);
        });
    if (!saved)
        return false;
    m_winForgeBridgeStatus = localized(
        QStringLiteral("Staged %1 file(s), %2 bytes. The verified OEM tree is now part of the reviewed ISO plan.")
            .arg(staged->fileCount).arg(staged->totalBytes),
        QStringLiteral("已放入 %1 個檔、%2 bytes；驗證過嘅 OEM tree 已加入 ISO 計劃。")
            .arg(staged->fileCount).arg(staged->totalBytes));
    notify(QStringLiteral("WinForge bridge staged"), m_winForgeBridgeStatus,
           QStringLiteral("success"));
    emit studioChanged();
    return true;
}

bool AppController::loadDemoProject(QString *error)
{
    const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("WimForge-Demo"));
    QDir().mkpath(QDir(directory).filePath(QStringLiteral("payloads/drivers")));
    QDir().mkpath(QDir(directory).filePath(QStringLiteral("payloads/updates")));
    auto touch = [](const QString &path) {
        QFile file(path); if (!file.exists() && file.open(QIODevice::WriteOnly)) file.write("WimForge demo payload\n");
    };
    const QString iso = QDir(directory).filePath(QStringLiteral("Windows11-25H2.iso"));
    const QString image = QDir(directory).filePath(QStringLiteral("install.wim"));
    const QString driver = QDir(directory).filePath(QStringLiteral("payloads/drivers/netadapter.inf"));
    const QString update = QDir(directory).filePath(QStringLiteral("payloads/updates/KB-demo.cab"));
    touch(iso); touch(image); touch(driver); touch(update);

    ProjectConfig project;
    project.projectDirectory = directory;
    project.projectName = QStringLiteral("AI Dev Workstation — 香港版");
    project.description = QStringLiteral("Material Design demo with a reversible Windows 11 image recipe.");
    project.sourcePath = iso; project.imagePath = image;
    project.mountPath = QDir(directory).filePath(QStringLiteral("mount"));
    project.outputPath = QDir(directory).filePath(QStringLiteral("output/AI-Dev-Windows11.iso"));
    project.outputFormat = QStringLiteral("iso"); project.isoLabel = QStringLiteral("WIMFORGE_AI");
    project.drivers = {driver}; project.packages = {update};
    project.featuresToEnable = {QStringLiteral("NetFx3"), QStringLiteral("Microsoft-Windows-Subsystem-Linux"), QStringLiteral("VirtualMachinePlatform")};
    project.appxPackagesToRemove = {QStringLiteral("Microsoft.BingNews_8wekyb3d8bbwe"), QStringLiteral("Microsoft.XboxGamingOverlay_8wekyb3d8bbwe")};
    project.postSetupItems = {QStringLiteral("winget install Git.Git -e --silent"), QStringLiteral("npm install -g opencode-ai@latest @openai/codex")};
    project.settings.insert(QStringLiteral("disableTelemetry"), true);
    project.settings.insert(QStringLiteral("enableLongPaths"), true);
    project.settings.insert(QStringLiteral("_packageProfile"),
                            PackageStudio::toJson(PackageStudio::fullAiDevelopmentTemplate()));
    project.settings.insert(QStringLiteral("_unattendProfile"),
                            UnattendBuilder::aiDevelopmentTemplate().toJson());
    WinForgeRecipe bridgeRecipe = emptyWinForgeRecipe(QStringLiteral("AI development WinForge recipe"));
    for (const QString &page : {QStringLiteral("ai"), QStringLiteral("packages"),
                                QStringLiteral("git")}) {
        WinForgeAction action;
        action.id = QStringLiteral("open-%1").arg(page);
        action.idempotencyKey = action.id;
        action.kind = WinForgeActionKind::Page;
        action.phase = WinForgeActionPhase::User;
        action.enabled = false;
        action.target = page;
        bridgeRecipe.actions.append(action);
    }
    project.settings.insert(QStringLiteral("_winForgeRecipe"),
                            WinForgeBridge::toJson(bridgeRecipe));
    project.settings.insert(QStringLiteral("_winForgeRuntimePath"), m_winForgeRuntimePath);
    project.settings.insert(QStringLiteral("_winForgeIncludeRuntime"), true);
    project.options.cleanupComponentStore = true;
    project.options.extra.insert(QStringLiteral("mediaWorkspace"), QDir(directory).filePath(QStringLiteral("media")));
    QString saveError;
    if (!project.save(&saveError, QStringLiteral("demo: create AI development image recipe"))) {
        setError(error, saveError); return false;
    }
    ActionHistory demoHistory(directory);
    QString actionError;
    if (demoHistory.events(1, &actionError).isEmpty() && actionError.isEmpty()) {
        const QJsonObject finalState = project.toJson();
        struct DemoAction {
            QString context;
            QString element;
            QString title;
            QString icon;
            QString summary;
            std::function<void(QJsonObject &)> makeInverse;
        };
        const QList<DemoAction> actions{
            {QStringLiteral("source"), QStringLiteral("windows-11-25h2"),
             QStringLiteral("source: select Windows 11 25H2 image"), QStringLiteral("image"),
             QStringLiteral("Source and edition selection"), [](QJsonObject &state) {
                 QJsonObject paths = state.value(QStringLiteral("paths")).toObject();
                 paths.insert(QStringLiteral("source"), QString());
                 paths.insert(QStringLiteral("image"), QString());
                 state.insert(QStringLiteral("paths"), paths);
             }},
            {QStringLiteral("gpo"), QStringLiteral("enable-long-paths"),
             QStringLiteral("gpo: enable Win32 long paths"), QStringLiteral("policy"),
             QStringLiteral("HKLM policy changed from Not Configured to Enabled"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.insert(QStringLiteral("enableLongPaths"), false);
                 state.insert(QStringLiteral("settings"), settings);
             }},
            {QStringLiteral("packages"), QStringLiteral("full-ai-development"),
             QStringLiteral("packages: select Full AI Development ISO"),
             QStringLiteral("inventory_2"), QStringLiteral("20 enabled tools plus optional desktop payloads"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.remove(QStringLiteral("_packageProfile"));
                 state.insert(QStringLiteral("settings"), settings);
             }},
            {QStringLiteral("unattended"), QStringLiteral("ai-development"),
             QStringLiteral("unattended: apply AI development template"),
             QStringLiteral("auto_fix_high"), QStringLiteral("Answer file and prompt-safe computer naming"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.remove(QStringLiteral("_unattendProfile"));
                 state.insert(QStringLiteral("settings"), settings);
             }},
            {QStringLiteral("winforge"), QStringLiteral("ai-tools"),
             QStringLiteral("winforge: draft post-install AI tool pages"),
             QStringLiteral("account_tree"), QStringLiteral("Three contract-checked page actions"),
             [](QJsonObject &state) {
                 QJsonObject settings = state.value(QStringLiteral("settings")).toObject();
                 settings.remove(QStringLiteral("_winForgeRecipe"));
                 state.insert(QStringLiteral("settings"), settings);
             }},
        };
        for (const DemoAction &demo : actions) {
            QJsonObject inverse = finalState;
            demo.makeInverse(inverse);
            ActionDraft draft;
            draft.contextKey = demo.context;
            draft.elementId = demo.element;
            draft.title = demo.title;
            draft.description = demo.summary;
            draft.icon = demo.icon;
            draft.forwardDiff = finalState;
            draft.inverseDiff = inverse;
            draft.metadata = QJsonObject{
                {QStringLiteral("diffSummary"), demo.summary},
                {QStringLiteral("fullProjectState"), true},
                {QStringLiteral("demo"), true},
            };
            if (!demoHistory.record(draft, nullptr, &actionError)) {
                setError(error, actionError);
                return false;
            }
        }
    }
    m_project = std::move(project);
    m_settings.setValue(QStringLiteral("project/last"), directory);
    m_editionNames = {QStringLiteral("Index 1 — Windows 11 Pro"), QStringLiteral("Index 2 — Windows 11 Enterprise")};
    m_imageSummary = QStringLiteral("2 editions · Windows 11 25H2 · amd64");
    loadProjectState();
    notify(QStringLiteral("Demo recipe ready"), QStringLiteral("No real image will be modified. Explore the Material UI, plan, Git history and notification actions."), QStringLiteral("success"));
    setError(error, {});
    return true;
}

void AppController::loadProjectState()
{
    if (m_project) {
        const QString notificationPath = m_project->settings
            .value(QStringLiteral("_notificationRepoPath")).toString();
        if (!notificationPath.isEmpty()
            && QDir::cleanPath(notificationPath) != QDir::cleanPath(m_notificationStore.storeDirectory())) {
            QString error;
            NotificationStore projectStore(notificationPath);
            if (projectStore.initialize(&error)) {
                m_notificationStore = projectStore;
                refreshNotifications();
            }
        }
    }
    restoreStudioState();
    if (m_winForgeRuntimePath.isEmpty()) {
        m_winForgeRuntimeContract = {};
        m_winForgeRuntimeStatus = QStringLiteral("Choose a WinForge runtime folder to detect its contract.");
    } else {
        QString bridgeError;
        m_winForgeRuntimeContract = WinForgeBridge::detectRuntimeContract(
            m_winForgeRuntimePath, &bridgeError);
        m_winForgeRuntimeStatus = m_winForgeRuntimeContract.runtimeFound
            ? QStringLiteral("%1 runtime · contract %2 · %3")
                  .arg(m_winForgeRuntimeContract.declaredContract ? QStringLiteral("Declared")
                                                                  : QStringLiteral("Legacy"))
                  .arg(m_winForgeRuntimeContract.contractVersion)
                  .arg(m_winForgeRuntimeContract.capabilities.join(QStringLiteral(", ")))
            : bridgeError;
    }
    refreshPlan(); refreshHistory(); refreshRecoveryState(); updateWatcher();
    emit stateChanged();
    emit studioChanged();
}

void AppController::refreshNotifications()
{
    QString error;
    m_notificationItems = m_notificationStore.list(true, true, &error);
    if (!error.isEmpty()) emit snackbarRequested(error, QStringLiteral("error"));
    emit notificationsChanged();
}

void AppController::refreshRecoveryState()
{
    m_pendingRecovery = false; m_recoveryJournal = {};
    if (m_project) {
        QString error;
        m_pendingRecovery = JobEngine::hasInterruptedRun(m_project->projectDirectory, &m_recoveryJournal, &error);
        if (!error.isEmpty()) emit snackbarRequested(error, QStringLiteral("warning"));
    }
}

void AppController::updateWatcher()
{
    if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
    if (m_project && m_project->autoImport && QFileInfo::exists(m_project->projectFilePath()))
        m_watcher.addPath(m_project->projectFilePath());
}

void AppController::onWatchedProjectChanged(const QString &path)
{
    if (!m_project || !m_project->autoImport) return;
    const QString directory = m_project->projectDirectory;
    QTimer::singleShot(250, this, [this, directory, path] {
        QString error;
        const auto updated = ProjectConfig::load(directory, &error);
        if (!updated) { showError(error); updateWatcher(); return; }
        m_project = *updated; loadProjectState();
        notify(QStringLiteral("External config imported"), path, QStringLiteral("info"));
    });
}

void AppController::notify(const QString &title, const QString &message, const QString &severity)
{
    QString error;
    m_notificationStore.addNotification(title, message, severity, QStringLiteral("WimForge"), {}, &error);
    if (!error.isEmpty()) emit snackbarRequested(error, QStringLiteral("error"));
    refreshNotifications();
}

void AppController::showError(const QString &message)
{
    if (message.trimmed().isEmpty()) return;
    m_statusText = message.trimmed();
    emit snackbarRequested(message.trimmed(), QStringLiteral("error"));
    QString notificationError;
    m_notificationStore.addNotification(
        QStringLiteral("Action needs attention"), message.trimmed(), QStringLiteral("error"),
        QStringLiteral("WimForge"), {}, &notificationError);
    if (notificationError.isEmpty())
        refreshNotifications();
    emit stateChanged();
}

void AppController::showSuccess(const QString &message)
{
    m_statusText = message;
    emit snackbarRequested(message, QStringLiteral("success"));
    emit stateChanged();
}

QString AppController::localized(const QString &en, const QString &zh) const
{
    if (m_languageMode == 1) return zh;
    if (m_languageMode == 2) return en + QStringLiteral("  ·  ") + zh;
    return en;
}
