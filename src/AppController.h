#pragma once

#include "core/JobEngine.h"
#include "core/GpoCatalog.h"
#include "core/ActionHistory.h"
#include "core/NotificationStore.h"
#include "core/PackageStudio.h"
#include "core/ProjectBundle.h"
#include "core/ProjectConfig.h"
#include "core/ServicingPlan.h"
#include "core/UnattendBuilder.h"
#include "core/WinForgeBridge.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <optional>

class AppController final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool projectLoaded READ projectLoaded NOTIFY stateChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY stateChanged)
    Q_PROPERTY(QString projectRoot READ projectRoot NOTIFY stateChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY stateChanged)
    Q_PROPERTY(QString imagePath READ imagePath NOTIFY stateChanged)
    Q_PROPERTY(QString mountPath READ mountPath NOTIFY stateChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY stateChanged)
    Q_PROPERTY(QString outputFormat READ outputFormat NOTIFY stateChanged)
    Q_PROPERTY(QString isoLabel READ isoLabel NOTIFY stateChanged)
    Q_PROPERTY(int imageIndex READ imageIndex NOTIFY stateChanged)
    Q_PROPERTY(bool cloneSource READ cloneSource NOTIFY stateChanged)
    Q_PROPERTY(QStringList editionNames READ editionNames NOTIFY stateChanged)
    Q_PROPERTY(QString imageSummary READ imageSummary NOTIFY stateChanged)

    Q_PROPERTY(QStringList drivers READ drivers NOTIFY stateChanged)
    Q_PROPERTY(QStringList packages READ packages NOTIFY stateChanged)
    Q_PROPERTY(QStringList features READ features NOTIFY stateChanged)
    Q_PROPERTY(QStringList appRemovals READ appRemovals NOTIFY stateChanged)
    Q_PROPERTY(QStringList componentRemovals READ componentRemovals NOTIFY stateChanged)
    Q_PROPERTY(QStringList unattendedFiles READ unattendedFiles NOTIFY stateChanged)
    Q_PROPERTY(QStringList postSetupItems READ postSetupItems NOTIFY stateChanged)

    Q_PROPERTY(QVariantList operationPlan READ operationPlan NOTIFY stateChanged)
    Q_PROPERTY(int operationCount READ operationCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList projectHistory READ projectHistory NOTIFY stateChanged)
    Q_PROPERTY(int projectHistoryCount READ projectHistoryCount NOTIFY stateChanged)
    Q_PROPERTY(QString gitStatusText READ gitStatusText NOTIFY stateChanged)
    Q_PROPERTY(QVariantList actionHistory READ actionHistory NOTIFY stateChanged)
    Q_PROPERTY(QString historyBranch READ historyBranch NOTIFY stateChanged)
    Q_PROPERTY(QStringList historyBranches READ historyBranches NOTIFY stateChanged)

    Q_PROPERTY(QVariantList notifications READ notifications NOTIFY notificationsChanged)
    Q_PROPERTY(int notificationUnreadCount READ notificationUnreadCount NOTIFY notificationsChanged)
    Q_PROPERTY(QString notificationRepoPath READ notificationRepoPath CONSTANT)

    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(int runningJobCount READ runningJobCount NOTIFY stateChanged)
    Q_PROPERTY(bool pendingRecovery READ pendingRecovery NOTIFY stateChanged)
    Q_PROPERTY(QString recoverySummary READ recoverySummary NOTIFY stateChanged)
    Q_PROPERTY(QString recoveryPath READ recoveryPath CONSTANT)

    Q_PROPERTY(int languageMode READ languageMode WRITE setLanguageMode NOTIFY preferencesChanged)
    Q_PROPERTY(int themeMode READ themeMode WRITE setThemeMode NOTIFY preferencesChanged)
    Q_PROPERTY(double interfaceScale READ interfaceScale WRITE setInterfaceScale NOTIFY preferencesChanged)
    Q_PROPERTY(bool motionEnabled READ motionEnabled WRITE setMotionEnabled NOTIFY preferencesChanged)
    Q_PROPERTY(int maxParallelJobs READ maxParallelJobs WRITE setMaxParallelJobs NOTIFY preferencesChanged)
    Q_PROPERTY(int logicalCpuCount READ logicalCpuCount CONSTANT)
    Q_PROPERTY(int threadLimit READ threadLimit WRITE setThreadLimit NOTIFY preferencesChanged)
    Q_PROPERTY(int scratchReserveGb READ scratchReserveGb WRITE setScratchReserveGb NOTIFY preferencesChanged)
    Q_PROPERTY(bool crashJournalEnabled READ crashJournalEnabled WRITE setCrashJournalEnabled NOTIFY preferencesChanged)
    Q_PROPERTY(bool verifySourceHash READ verifySourceHash WRITE setVerifySourceHash NOTIFY preferencesChanged)
    Q_PROPERTY(bool checkpointBeforeDestructive READ checkpointBeforeDestructive WRITE setCheckpointBeforeDestructive NOTIFY preferencesChanged)
    Q_PROPERTY(bool autoImport READ autoImport WRITE setAutoImport NOTIFY stateChanged)
    Q_PROPERTY(bool autoExport READ autoExport WRITE setAutoExport NOTIFY stateChanged)
    Q_PROPERTY(QString autoExportPath READ autoExportPath WRITE setAutoExportPath NOTIFY stateChanged)
    Q_PROPERTY(QString defaultProjectPath READ defaultProjectPath CONSTANT)

    Q_PROPERTY(bool gpoLoaded READ gpoLoaded NOTIFY studioChanged)
    Q_PROPERTY(int gpoPolicyCount READ gpoPolicyCount NOTIFY studioChanged)
    Q_PROPERTY(QString gpoStatus READ gpoStatus NOTIFY studioChanged)
    Q_PROPERTY(QVariantList gpoResults READ gpoResults NOTIFY studioChanged)
    Q_PROPERTY(QVariantList packageCatalog READ packageCatalog NOTIFY studioChanged)
    Q_PROPERTY(QString packageProfileName READ packageProfileName NOTIFY studioChanged)
    Q_PROPERTY(int selectedPackageCount READ selectedPackageCount NOTIFY studioChanged)
    Q_PROPERTY(QVariantList unattendedSettings READ unattendedSettings NOTIFY studioChanged)
    Q_PROPERTY(QVariantList microsoftProductKeys READ microsoftProductKeys CONSTANT)
    Q_PROPERTY(int computerNameMode READ computerNameMode NOTIFY studioChanged)
    Q_PROPERTY(QString computerNameValue READ computerNameValue NOTIFY studioChanged)
    Q_PROPERTY(bool openCodeInstalled READ openCodeInstalled NOTIFY studioChanged)
    Q_PROPERTY(bool openCodeBusy READ openCodeBusy NOTIFY studioChanged)
    Q_PROPERTY(QString openCodeStatus READ openCodeStatus NOTIFY studioChanged)
    Q_PROPERTY(QVariantList winForgeBridgeActions READ winForgeBridgeActions NOTIFY studioChanged)
    Q_PROPERTY(bool winForgeBridgeIncludeRuntime READ winForgeBridgeIncludeRuntime NOTIFY studioChanged)
    Q_PROPERTY(QString winForgeBridgeRuntimePath READ winForgeBridgeRuntimePath NOTIFY studioChanged)
    Q_PROPERTY(QString winForgeBridgeRuntimeStatus READ winForgeBridgeRuntimeStatus NOTIFY studioChanged)
    Q_PROPERTY(QString winForgeBridgeStatus READ winForgeBridgeStatus NOTIFY studioChanged)

public:
    explicit AppController(QObject *parent = nullptr);

    [[nodiscard]] QString version() const;
    [[nodiscard]] bool projectLoaded() const;
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectRoot() const;
    [[nodiscard]] QString sourcePath() const;
    [[nodiscard]] QString imagePath() const;
    [[nodiscard]] QString mountPath() const;
    [[nodiscard]] QString outputPath() const;
    [[nodiscard]] QString outputFormat() const;
    [[nodiscard]] QString isoLabel() const;
    [[nodiscard]] int imageIndex() const;
    [[nodiscard]] bool cloneSource() const;
    [[nodiscard]] QStringList editionNames() const;
    [[nodiscard]] QString imageSummary() const;
    [[nodiscard]] QStringList drivers() const;
    [[nodiscard]] QStringList packages() const;
    [[nodiscard]] QStringList features() const;
    [[nodiscard]] QStringList appRemovals() const;
    [[nodiscard]] QStringList componentRemovals() const;
    [[nodiscard]] QStringList unattendedFiles() const;
    [[nodiscard]] QStringList postSetupItems() const;
    [[nodiscard]] QVariantList operationPlan() const;
    [[nodiscard]] int operationCount() const;
    [[nodiscard]] QVariantList projectHistory() const;
    [[nodiscard]] int projectHistoryCount() const;
    [[nodiscard]] QString gitStatusText() const;
    [[nodiscard]] QVariantList actionHistory() const;
    [[nodiscard]] QString historyBranch() const;
    [[nodiscard]] QStringList historyBranches() const;
    [[nodiscard]] QVariantList notifications() const;
    [[nodiscard]] int notificationUnreadCount() const;
    [[nodiscard]] QString notificationRepoPath() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] double progress() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] int runningJobCount() const;
    [[nodiscard]] bool pendingRecovery() const;
    [[nodiscard]] QString recoverySummary() const;
    [[nodiscard]] QString recoveryPath() const;

    [[nodiscard]] int languageMode() const;
    void setLanguageMode(int value);
    [[nodiscard]] int themeMode() const;
    void setThemeMode(int value);
    [[nodiscard]] double interfaceScale() const;
    void setInterfaceScale(double value);
    [[nodiscard]] bool motionEnabled() const;
    void setMotionEnabled(bool value);
    [[nodiscard]] int maxParallelJobs() const;
    void setMaxParallelJobs(int value);
    [[nodiscard]] int logicalCpuCount() const;
    [[nodiscard]] int threadLimit() const;
    void setThreadLimit(int value);
    [[nodiscard]] int scratchReserveGb() const;
    void setScratchReserveGb(int value);
    [[nodiscard]] bool crashJournalEnabled() const;
    void setCrashJournalEnabled(bool value);
    [[nodiscard]] bool verifySourceHash() const;
    void setVerifySourceHash(bool value);
    [[nodiscard]] bool checkpointBeforeDestructive() const;
    void setCheckpointBeforeDestructive(bool value);
    [[nodiscard]] bool autoImport() const;
    void setAutoImport(bool value);
    [[nodiscard]] bool autoExport() const;
    void setAutoExport(bool value);
    [[nodiscard]] QString autoExportPath() const;
    void setAutoExportPath(const QString &value);
    [[nodiscard]] QString defaultProjectPath() const;
    [[nodiscard]] bool gpoLoaded() const;
    [[nodiscard]] int gpoPolicyCount() const;
    [[nodiscard]] QString gpoStatus() const;
    [[nodiscard]] QVariantList gpoResults() const;
    [[nodiscard]] QVariantList packageCatalog() const;
    [[nodiscard]] QString packageProfileName() const;
    [[nodiscard]] int selectedPackageCount() const;
    [[nodiscard]] QVariantList unattendedSettings() const;
    [[nodiscard]] QVariantList microsoftProductKeys() const;
    [[nodiscard]] int computerNameMode() const;
    [[nodiscard]] QString computerNameValue() const;
    [[nodiscard]] bool openCodeInstalled() const;
    [[nodiscard]] bool openCodeBusy() const;
    [[nodiscard]] QString openCodeStatus() const;
    [[nodiscard]] QVariantList winForgeBridgeActions() const;
    [[nodiscard]] bool winForgeBridgeIncludeRuntime() const;
    [[nodiscard]] QString winForgeBridgeRuntimePath() const;
    [[nodiscard]] QString winForgeBridgeRuntimeStatus() const;
    [[nodiscard]] QString winForgeBridgeStatus() const;

    Q_INVOKABLE void requestNewProject();
    Q_INVOKABLE void requestOpenProject();
    Q_INVOKABLE bool createProject(const QString &directory, const QString &name);
    Q_INVOKABLE bool openProject(const QString &directory);
    Q_INVOKABLE bool importProject(const QString &sourceFile, const QString &destinationDirectory);
    Q_INVOKABLE bool exportProject(const QString &destinationFile);
    Q_INVOKABLE bool exportScript(const QString &destinationFile);
    Q_INVOKABLE void requestExportProject();
    Q_INVOKABLE void requestExportScript();

    Q_INVOKABLE void setProjectField(const QString &field, const QString &value);
    Q_INVOKABLE void setProjectBool(const QString &field, bool value);
    Q_INVOKABLE void setProjectNumber(const QString &field, int value);
    Q_INVOKABLE void addListItem(const QString &category, const QString &value);
    Q_INVOKABLE void removeListItem(const QString &category, int index);
    Q_INVOKABLE void setFeature(const QString &name, bool enabled);
    Q_INVOKABLE void setSetting(const QString &name, bool enabled);
    Q_INVOKABLE bool settingEnabled(const QString &name) const;

    Q_INVOKABLE void inspectSource();
    Q_INVOKABLE void importHostDrivers();
    Q_INVOKABLE void refreshPlan();
    Q_INVOKABLE void requestRunPlan();
    Q_INVOKABLE void runPlan();
    Q_INVOKABLE void cancelJobs();
    Q_INVOKABLE void moveOperation(int index, int offset);
    Q_INVOKABLE void skipOperation(int index);

    Q_INVOKABLE void refreshHistory();
    Q_INVOKABLE void undoLatestProjectChange();
    Q_INVOKABLE QVariantList contextualHistory(const QString &contextKey = {},
                                               const QString &elementId = {}) const;
    Q_INVOKABLE void undoContext(const QString &contextKey = {},
                                 const QString &elementId = {});
    Q_INVOKABLE bool undoHistoryEvent(const QString &eventId);
    Q_INVOKABLE bool redoHistoryEvent(const QString &eventId);
    Q_INVOKABLE bool restoreHistoryEvent(const QString &eventId);
    Q_INVOKABLE bool bookmarkHistoryEvent(const QString &eventId, const QString &name);
    Q_INVOKABLE bool branchHistoryEvent(const QString &eventId, const QString &name);
    Q_INVOKABLE bool switchHistoryBranch(const QString &name);
    Q_INVOKABLE void undoLatestNotificationChange();
    Q_INVOKABLE void markNotificationRead(const QString &id);
    Q_INVOKABLE void markNotificationUnread(const QString &id);
    Q_INVOKABLE void dismissNotification(const QString &id);
    Q_INVOKABLE void deleteNotification(const QString &id);
    Q_INVOKABLE void restoreNotification(const QString &id);
    Q_INVOKABLE void sendTestNotification();

    Q_INVOKABLE void resumeRecovery();
    Q_INVOKABLE void rollbackRecovery();
    Q_INVOKABLE void safeUnmountRecovery();
    Q_INVOKABLE void openUnattendGenerator();
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void copyText(const QString &text);

    Q_INVOKABLE void loadGpoCatalog();
    Q_INVOKABLE void searchGpo(const QString &query, bool regularExpression = false);
    Q_INVOKABLE bool applyGpoPolicy(const QString &qualifiedId,
                                    const QString &state,
                                    const QVariantMap &elementValues = {});
    Q_INVOKABLE bool exportGpoDocumentation(const QString &destinationFile);
    Q_INVOKABLE void askOpenCodeForGpo(const QString &intent);

    Q_INVOKABLE void loadAiDevelopmentPackageTemplate();
    Q_INVOKABLE void setPackageEnabled(const QString &id, bool enabled);
    Q_INVOKABLE bool importPackageProfile(const QString &sourceFile);
    Q_INVOKABLE bool exportPackageProfile(const QString &destinationFile);
    Q_INVOKABLE bool stagePackageProfile();

    Q_INVOKABLE void loadUnattendedTemplate(const QString &templateId);
    Q_INVOKABLE void setComputerNameBehavior(int mode, const QString &value);
    Q_INVOKABLE void setUnattendedValue(const QString &pass,
                                        const QString &component,
                                        const QString &path,
                                        const QString &value);
    Q_INVOKABLE bool importUnattended(const QString &sourceFile);
    Q_INVOKABLE bool exportUnattended(const QString &destinationFile);
    Q_INVOKABLE void askOpenCodeToFillUnattended(const QString &intent);
    Q_INVOKABLE void ensureOpenCode();

    Q_INVOKABLE void proposeWinForgeBridgeActions(const QString &intent);
    Q_INVOKABLE bool addWinForgeBridgeAction(const QString &kind,
                                             const QString &target,
                                             const QString &executable,
                                             const QString &argumentsJson,
                                             const QString &phase);
    Q_INVOKABLE bool removeWinForgeBridgeAction(const QString &id);
    Q_INVOKABLE bool setWinForgeBridgeActionEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void setWinForgeBridgeIncludeRuntime(bool enabled);
    Q_INVOKABLE void setWinForgeBridgeRuntimePath(const QString &path);
    Q_INVOKABLE void detectWinForgeBridgeRuntime();
    Q_INVOKABLE bool importWinForgeBridgeRecipe(const QString &sourceFile);
    Q_INVOKABLE bool exportWinForgeBridgeRecipe(const QString &destinationFile);
    Q_INVOKABLE bool stageWinForgeBridgeIntoIso(const QString &isoStagingPath);

    bool loadDemoProject(QString *error = nullptr);

signals:
    void stateChanged();
    void preferencesChanged();
    void notificationsChanged();
    void studioChanged();
    void snackbarRequested(const QString &message, const QString &tone);
    void newProjectRequested();
    void openProjectRequested();
    void runConfirmationRequested(const QString &summary, int destructiveCount);
    void exportProjectRequested();
    void exportScriptRequested();
    void unattendedStudioRequested();
    void recoveryReviewRequested();
    void searchRequested(const QString &query);

private:
    using ProjectMutation = std::function<void(wimforge::ProjectConfig &)>;

    bool mutateProject(const QString &message, const ProjectMutation &mutation);
    bool saveProject(const QString &message);
    void loadProjectState();
    void refreshNotifications();
    void refreshRecoveryState();
    void updateWatcher();
    void onWatchedProjectChanged(const QString &path);
    void notify(const QString &title, const QString &message, const QString &severity);
    void showError(const QString &message);
    void showSuccess(const QString &message);
    [[nodiscard]] QString localized(const QString &en, const QString &zh) const;
    [[nodiscard]] QStringList *listForCategory(wimforge::ProjectConfig &project, const QString &category);
    void restoreStudioState();
    bool persistPackageProfile(const QString &message);
    bool persistUnattendedProfile(const QString &message, bool writeXml = true);
    bool persistWinForgeBridgeRecipe(const QString &message,
                                     const wimforge::WinForgeRecipe &recipe);
    bool applyHistoryState(const wimforge::ActionEvent &event, const QString &message);
    void runOpenCode(const QString &prompt, const std::function<void(const QString &)> &completed);
    void installOpenCodeThen(const std::function<void()> &completed);
    void verifyOpenCodeThen(const std::function<void()> &completed, bool installedNow);

    std::optional<wimforge::ProjectConfig> m_project;
    wimforge::NotificationStore m_notificationStore;
    wimforge::JobEngine m_jobEngine;
    QList<wimforge::ServicingOperation> m_plan;
    QList<wimforge::GitCommit> m_history;
    QList<wimforge::Notification> m_notificationItems;
    wimforge::GpoCatalog m_gpoCatalog;
    QList<wimforge::GpoPolicy> m_gpoSearchResults;
    wimforge::PackageProfile m_packageProfile;
    wimforge::UnattendProfile m_unattendProfile;
    wimforge::WinForgeRecipe m_winForgeRecipe;
    wimforge::WinForgeRuntimeContract m_winForgeRuntimeContract;
    QFileSystemWatcher m_watcher;
    QSettings m_settings;
    QStringList m_editionNames{QStringLiteral("Index 1 — Windows edition")};
    QString m_imageSummary = QStringLiteral("Inspect a source to load edition metadata.");
    QString m_statusText = QStringLiteral("Ready — no active servicing jobs");
    QJsonObject m_recoveryJournal;
    bool m_pendingRecovery = false;
    bool m_recoveryUnmountBusy = false;
    bool m_inspecting = false;
    bool m_gpoLoaded = false;
    bool m_openCodeBusy = false;
    bool m_openCodeVerified = false;
    QString m_gpoStatus = QStringLiteral("Policy catalog has not been loaded yet.");
    QString m_openCodeStatus = QStringLiteral("OpenCode status has not been checked.");
    QString m_winForgeRuntimePath;
    QString m_winForgeRuntimeStatus = QStringLiteral("No WinForge runtime has been detected yet.");
    QString m_winForgeBridgeStatus = QStringLiteral("Recipe is ready for review.");
    bool m_winForgeIncludeRuntime = true;

    int m_languageMode = 2;
    int m_themeMode = 0;
    double m_interfaceScale = 1.0;
    bool m_motionEnabled = true;
    int m_maxParallelJobs = 4;
    int m_threadLimit = 4;
    int m_scratchReserveGb = 20;
    bool m_crashJournalEnabled = true;
    bool m_verifySourceHash = true;
    bool m_checkpointBeforeDestructive = true;
};
