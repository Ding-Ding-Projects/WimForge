#pragma once

#include "core/JobEngine.h"
#include "core/GpoCatalog.h"
#include "core/ActionHistory.h"
#include "core/NotificationStore.h"
#include "core/OpenCodeSetup.h"
#include "core/PackageStudio.h"
#include "core/ProjectBundle.h"
#include "core/ProjectConfig.h"
#include "core/ServicingPlan.h"
#include "core/SearchIndex.h"
#include "core/UnattendBuilder.h"
#include "core/VmLabManager.h"
#include "core/VmValidationStore.h"
#include "core/WinForgeBridge.h"
#include "core/WorkspaceTabs.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>
#include <QQueue>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <thread>

class QProcess;
class QNetworkAccessManager;
class QNetworkReply;

class AppController final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool projectLoaded READ projectLoaded NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY stateChanged)
    Q_PROPERTY(QString projectRoot READ projectRoot NOTIFY stateChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY stateChanged)
    Q_PROPERTY(QString imagePath READ imagePath NOTIFY stateChanged)
    Q_PROPERTY(QString imageRelativePath READ imageRelativePath NOTIFY stateChanged)
    Q_PROPERTY(QString mountPath READ mountPath NOTIFY stateChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY stateChanged)
    Q_PROPERTY(QString outputFormat READ outputFormat NOTIFY stateChanged)
    Q_PROPERTY(QString isoLabel READ isoLabel NOTIFY stateChanged)
    Q_PROPERTY(int imageIndex READ imageIndex NOTIFY stateChanged)
    Q_PROPERTY(bool cloneSource READ cloneSource NOTIFY stateChanged)
    Q_PROPERTY(QStringList editionNames READ editionNames NOTIFY stateChanged)
    Q_PROPERTY(QString imageSummary READ imageSummary NOTIFY stateChanged)

    Q_PROPERTY(QStringList drivers READ drivers NOTIFY stateChanged)
    Q_PROPERTY(QStringList updates READ updates NOTIFY stateChanged)
    Q_PROPERTY(QStringList packages READ packages NOTIFY stateChanged)
    Q_PROPERTY(QVariantList driverCatalog READ driverCatalog NOTIFY payloadCatalogChanged)
    Q_PROPERTY(QVariantList updateCatalog READ updateCatalog NOTIFY payloadCatalogChanged)
    Q_PROPERTY(QStringList features READ features NOTIFY stateChanged)
    Q_PROPERTY(QStringList featureDisables READ featureDisables NOTIFY stateChanged)
    Q_PROPERTY(QVariantList capabilityChanges READ capabilityChanges NOTIFY stateChanged)
    Q_PROPERTY(QStringList appRemovals READ appRemovals NOTIFY stateChanged)
    Q_PROPERTY(QStringList appProvisions READ appProvisions NOTIFY stateChanged)
    Q_PROPERTY(QStringList componentRemovals READ componentRemovals NOTIFY stateChanged)
    Q_PROPERTY(QVariantList scheduledTaskChanges READ scheduledTaskChanges NOTIFY stateChanged)
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
    Q_PROPERTY(bool backgroundBusy READ backgroundBusy NOTIFY stateChanged)
    Q_PROPERTY(QString backgroundStatus READ backgroundStatus NOTIFY stateChanged)
    Q_PROPERTY(bool sourceInspectionBusy READ sourceInspectionBusy NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(int runningJobCount READ runningJobCount NOTIFY stateChanged)
    Q_PROPERTY(bool pendingRecovery READ pendingRecovery NOTIFY stateChanged)
    Q_PROPERTY(QString recoverySummary READ recoverySummary NOTIFY stateChanged)
    Q_PROPERTY(QString recoveryPath READ recoveryPath CONSTANT)

    Q_PROPERTY(int languageMode READ languageMode WRITE setLanguageMode NOTIFY preferencesChanged)
    Q_PROPERTY(int themeMode READ themeMode WRITE setThemeMode NOTIFY preferencesChanged)
    Q_PROPERTY(int colorScheme READ colorScheme WRITE setColorScheme NOTIFY preferencesChanged)
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
    Q_PROPERTY(bool unattendedNarratorAutostart READ unattendedNarratorAutostart NOTIFY studioChanged)
    Q_PROPERTY(QVariantList updateCatalogResults READ updateCatalogResults NOTIFY updateCatalogChanged)
    Q_PROPERTY(QString updateCatalogStatus READ updateCatalogStatus NOTIFY updateCatalogChanged)
    Q_PROPERTY(bool updateCatalogBusy READ updateCatalogBusy NOTIFY updateCatalogChanged)
    Q_PROPERTY(double updateCatalogDownloadProgress READ updateCatalogDownloadProgress NOTIFY updateCatalogChanged)
    Q_PROPERTY(QString sourceCatalogQuery READ sourceCatalogQuery NOTIFY updateCatalogChanged)
    Q_PROPERTY(bool payloadCatalogBusy READ payloadCatalogBusy NOTIFY payloadCatalogChanged)
    Q_PROPERTY(QVariantList microsoftProductKeys READ microsoftProductKeys CONSTANT)
    Q_PROPERTY(int computerNameMode READ computerNameMode NOTIFY studioChanged)
    Q_PROPERTY(QString computerNameValue READ computerNameValue NOTIFY studioChanged)
    Q_PROPERTY(bool openCodeInstalled READ openCodeInstalled NOTIFY studioChanged)
    Q_PROPERTY(bool openCodeBusy READ openCodeBusy NOTIFY studioChanged)
    Q_PROPERTY(bool openCodeReady READ openCodeReady NOTIFY studioChanged)
    Q_PROPERTY(bool openCodeCanRetry READ openCodeCanRetry NOTIFY studioChanged)
    Q_PROPERTY(QString openCodeState READ openCodeState NOTIFY studioChanged)
    Q_PROPERTY(QString openCodeStatus READ openCodeStatus NOTIFY studioChanged)
    Q_PROPERTY(QString openCodeError READ openCodeError NOTIFY studioChanged)
    Q_PROPERTY(QVariantList winForgeBridgeActions READ winForgeBridgeActions NOTIFY studioChanged)
    Q_PROPERTY(bool winForgeBridgeIncludeRuntime READ winForgeBridgeIncludeRuntime NOTIFY studioChanged)
    Q_PROPERTY(QString winForgeBridgeRuntimePath READ winForgeBridgeRuntimePath NOTIFY studioChanged)
    Q_PROPERTY(QString winForgeBridgeRuntimeStatus READ winForgeBridgeRuntimeStatus NOTIFY studioChanged)
    Q_PROPERTY(QString winForgeBridgeStatus READ winForgeBridgeStatus NOTIFY studioChanged)
    Q_PROPERTY(QVariantList vmProviders READ vmProviders NOTIFY vmLabChanged)
    Q_PROPERTY(QVariantList vmInventory READ vmInventory NOTIFY vmLabChanged)
    Q_PROPERTY(QString vmSelectedId READ vmSelectedId NOTIFY vmLabChanged)
    Q_PROPERTY(QVariant vmSelected READ vmSelected NOTIFY vmLabChanged)
    Q_PROPERTY(QVariantList vmSnapshots READ vmSnapshots NOTIFY vmLabChanged)
    Q_PROPERTY(QVariantList vmValidationRuns READ vmValidationRuns NOTIFY vmLabChanged)
    Q_PROPERTY(bool vmBusy READ vmBusy NOTIFY vmLabChanged)
    Q_PROPERTY(QVariantMap vmStatus READ vmStatus NOTIFY vmLabChanged)
    Q_PROPERTY(QVariantMap vmPendingPreview READ vmPendingPreview NOTIFY vmLabChanged)
    Q_PROPERTY(QString currentOutput READ currentOutput NOTIFY stateChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY searchChanged)
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchChanged)
    Q_PROPERTY(QVariantList workspaceTabs READ workspaceTabs NOTIFY workspaceTabsChanged)
    Q_PROPERTY(int activeWorkspaceTab READ activeWorkspaceTab NOTIFY workspaceTabsChanged)
    Q_PROPERTY(QString workspaceTabRepository READ workspaceTabRepository NOTIFY workspaceTabsChanged)
    Q_PROPERTY(QString applicationLogPath READ applicationLogPath CONSTANT)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    [[nodiscard]] QString version() const;
    [[nodiscard]] bool projectLoaded() const;
    [[nodiscard]] QVariantList recentProjects() const;
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectRoot() const;
    [[nodiscard]] QString sourcePath() const;
    [[nodiscard]] QString imagePath() const;
    [[nodiscard]] QString imageRelativePath() const;
    [[nodiscard]] QString mountPath() const;
    [[nodiscard]] QString outputPath() const;
    [[nodiscard]] QString outputFormat() const;
    [[nodiscard]] QString isoLabel() const;
    [[nodiscard]] int imageIndex() const;
    [[nodiscard]] bool cloneSource() const;
    [[nodiscard]] QStringList editionNames() const;
    [[nodiscard]] QString imageSummary() const;
    [[nodiscard]] QStringList drivers() const;
    [[nodiscard]] QStringList updates() const;
    [[nodiscard]] QStringList packages() const;
    [[nodiscard]] QVariantList driverCatalog() const;
    [[nodiscard]] QVariantList updateCatalog() const;
    [[nodiscard]] QStringList features() const;
    [[nodiscard]] QStringList featureDisables() const;
    [[nodiscard]] QVariantList capabilityChanges() const;
    [[nodiscard]] QStringList appRemovals() const;
    [[nodiscard]] QStringList appProvisions() const;
    [[nodiscard]] QStringList componentRemovals() const;
    [[nodiscard]] QVariantList scheduledTaskChanges() const;
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
    [[nodiscard]] bool backgroundBusy() const;
    [[nodiscard]] QString backgroundStatus() const;
    [[nodiscard]] bool sourceInspectionBusy() const;
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
    [[nodiscard]] int colorScheme() const;
    void setColorScheme(int value);
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
    [[nodiscard]] bool unattendedNarratorAutostart() const;
    [[nodiscard]] QVariantList updateCatalogResults() const;
    [[nodiscard]] QString updateCatalogStatus() const;
    [[nodiscard]] bool updateCatalogBusy() const;
    [[nodiscard]] double updateCatalogDownloadProgress() const;
    [[nodiscard]] QString sourceCatalogQuery() const;
    [[nodiscard]] bool payloadCatalogBusy() const;
    [[nodiscard]] QVariantList microsoftProductKeys() const;
    [[nodiscard]] int computerNameMode() const;
    [[nodiscard]] QString computerNameValue() const;
    [[nodiscard]] bool openCodeInstalled() const;
    [[nodiscard]] bool openCodeBusy() const;
    [[nodiscard]] bool openCodeReady() const;
    [[nodiscard]] bool openCodeCanRetry() const;
    [[nodiscard]] QString openCodeState() const;
    [[nodiscard]] QString openCodeStatus() const;
    [[nodiscard]] QString openCodeError() const;
    [[nodiscard]] QVariantList winForgeBridgeActions() const;
    [[nodiscard]] bool winForgeBridgeIncludeRuntime() const;
    [[nodiscard]] QString winForgeBridgeRuntimePath() const;
    [[nodiscard]] QString winForgeBridgeRuntimeStatus() const;
    [[nodiscard]] QString winForgeBridgeStatus() const;
    [[nodiscard]] QVariantList vmProviders() const;
    [[nodiscard]] QVariantList vmInventory() const;
    [[nodiscard]] QString vmSelectedId() const;
    [[nodiscard]] QVariant vmSelected() const;
    [[nodiscard]] QVariantList vmSnapshots() const;
    [[nodiscard]] QVariantList vmValidationRuns() const;
    [[nodiscard]] bool vmBusy() const;
    [[nodiscard]] QVariantMap vmStatus() const;
    [[nodiscard]] QVariantMap vmPendingPreview() const;
    [[nodiscard]] QString currentOutput() const;
    [[nodiscard]] QString searchQuery() const;
    [[nodiscard]] QVariantList searchResults() const;
    [[nodiscard]] QVariantList workspaceTabs() const;
    [[nodiscard]] int activeWorkspaceTab() const;
    [[nodiscard]] QString workspaceTabRepository() const;
    [[nodiscard]] QString applicationLogPath() const;

    Q_INVOKABLE void requestNewProject();
    Q_INVOKABLE void requestOpenProject();
    Q_INVOKABLE bool createProject(const QString &directory, const QString &name);
    Q_INVOKABLE bool openProject(const QString &directory);
    Q_INVOKABLE bool importProject(const QString &sourceFile, const QString &destinationDirectory);
    Q_INVOKABLE void removeRecentProject(const QString &directory);
    Q_INVOKABLE void clearRecentProjects();
    Q_INVOKABLE bool exportProject(const QString &destinationFile);
    Q_INVOKABLE bool exportScript(const QString &destinationFile);
    Q_INVOKABLE void requestExportProject();
    Q_INVOKABLE void requestExportScript();

    Q_INVOKABLE void setProjectField(const QString &field, const QString &value);
    Q_INVOKABLE void setProjectBool(const QString &field, bool value);
    Q_INVOKABLE void setProjectNumber(const QString &field, int value);
    Q_INVOKABLE void addListItem(const QString &category, const QString &value);
    Q_INVOKABLE bool tryAddListItem(const QString &category, const QString &value);
    Q_INVOKABLE void removeListItem(const QString &category, int index);
    Q_INVOKABLE bool addPayloadFiles(const QString &category, const QVariantList &files);
    Q_INVOKABLE bool addPayloadDirectory(const QString &category, const QUrl &directory);
    Q_INVOKABLE void refreshPayloadCatalog();
    Q_INVOKABLE void openMicrosoftUpdateCatalog(const QString &query = {});
    Q_INVOKABLE QString catalogQueryForCategory(const QString &category) const;
    Q_INVOKABLE void searchUpdateCatalog(const QString &query);
    Q_INVOKABLE void downloadUpdateCatalogItem(const QString &updateId, const QString &title,
                                               const QString &category = QStringLiteral("updates"),
                                               double sizeBytes = 0.0);
    Q_INVOKABLE void cancelUpdateCatalog();
    Q_INVOKABLE void setFeature(const QString &name, bool enabled);
    Q_INVOKABLE int featureState(const QString &name) const;
    Q_INVOKABLE bool setFeatureState(const QString &name, int state);
    Q_INVOKABLE int capabilityState(const QString &name) const;
    Q_INVOKABLE bool setCapabilityState(const QString &name, int state);
    Q_INVOKABLE bool addAppxProvisionFiles(const QVariantList &files);
    Q_INVOKABLE bool setScheduledTaskChange(const QString &taskPath,
                                            const QString &disposition,
                                            bool compatibilityOverride = false);
    Q_INVOKABLE bool removeScheduledTaskChange(int index);
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
    Q_INVOKABLE void clearSearch();
    Q_INVOKABLE void activateSearchResult(const QVariantMap &result);
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
    Q_INVOKABLE void clearUnattendedValue(const QString &pass,
                                          const QString &component,
                                          const QString &path);
    Q_INVOKABLE void setUnattendedNarratorAutostart(bool enabled);
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

    Q_INVOKABLE void refreshVmLab();
    Q_INVOKABLE bool selectVm(const QString &providerId, const QString &id);
    Q_INVOKABLE bool createVm(const QVariantMap &spec);
    Q_INVOKABLE bool runVmAction(const QString &action, const QVariantMap &options = {});
    Q_INVOKABLE bool updateVmConfiguration(const QVariantMap &spec);
    Q_INVOKABLE bool vmDeviceAction(const QString &action, const QVariantMap &spec);
    Q_INVOKABLE bool vmSnapshotAction(const QString &action, const QVariantMap &spec);
    Q_INVOKABLE bool startVmValidation(const QVariantMap &spec);
    Q_INVOKABLE bool recordVmValidationMilestone(const QString &runId,
                                                 const QVariantMap &spec);
    Q_INVOKABLE bool finishVmValidation(const QString &runId,
                                        const QVariantMap &result);
    Q_INVOKABLE bool executePendingVmPreview(const QString &previewId,
                                             const QString &typedConfirmation = {});
    Q_INVOKABLE void discardPendingVmPreview();
    Q_INVOKABLE bool cancelVmAction();
    Q_INVOKABLE QString pathFromUrl(const QUrl &url) const;

    Q_INVOKABLE bool openWorkspacePage(int page, const QString &defaultTitle = {});
    Q_INVOKABLE bool navigateActiveWorkspaceTab(int page, const QString &defaultTitle = {});
    Q_INVOKABLE bool openWorkspaceTabForPage(int page, const QString &defaultTitle = {});
    Q_INVOKABLE bool activateWorkspaceTab(int index);
    Q_INVOKABLE bool closeWorkspaceTab(int index);
    Q_INVOKABLE bool closeWorkspaceTabsByIndices(const QVariantList &indices);
    Q_INVOKABLE bool moveWorkspaceTab(int from, int to);
    Q_INVOKABLE bool updateWorkspaceTab(int index, const QVariantMap &changes);
    Q_INVOKABLE bool exportWorkspaceTabs(const QString &destinationFile);
    Q_INVOKABLE bool importWorkspaceTabs(const QString &sourceFile);
    Q_INVOKABLE bool exportWorkspaceTabRepository(const QString &destinationFile);
    Q_INVOKABLE bool importWorkspaceTabRepository(const QString &sourceFile);
    Q_INVOKABLE bool openApplicationLog();
    Q_INVOKABLE bool openApplicationLogFolder();
    Q_INVOKABLE void retryBackgroundPersistence();

    bool loadDemoProject(QString *error = nullptr);

signals:
    void stateChanged();
    void recentProjectsChanged();
    void preferencesChanged();
    void notificationsChanged();
    void studioChanged();
    void updateCatalogChanged();
    void snackbarRequested(const QString &message, const QString &tone);
    void newProjectRequested();
    void openProjectRequested();
    void runConfirmationRequested(const QString &summary, int destructiveCount);
    void exportProjectRequested();
    void exportScriptRequested();
    void unattendedStudioRequested();
    void recoveryReviewRequested();
    void searchRequested(const QString &query);
    void searchChanged();
    void payloadCatalogChanged();
    void searchNavigationRequested(int page, const QString &focusId, const QString &query);
    void vmLabChanged();
    void vmPreviewReady();
    void workspaceTabsChanged();

private:
    using ProjectMutation = std::function<void(wimforge::ProjectConfig &)>;

    struct PendingProjectMutation
    {
        wimforge::ProjectConfig project;
        QJsonObject before;
        QJsonObject after;
        QString message;
    };

    struct PendingWorkspacePersistence
    {
        wimforge::WorkspaceTabs snapshot;
        QString message;
    };

    bool mutateProject(const QString &message, const ProjectMutation &mutation);
    void beginNextProjectMutation();
    void finishProjectMutation(bool saved, const QString &error,
                               const QString &historyError,
                               const QString &bundleError);
    void refreshProjectDerivedState();
    void refreshImageInventoryState();
    void queueWorkspacePersistence();
    void beginNextWorkspacePersistence();
    bool saveProject(const QString &message);
    void loadRecentProjects();
    void rememberRecentProject(const QString &directory, const QString &name);
    void persistRecentProjects();
    void loadProjectState();
    void refreshNotifications();
    void refreshRecoveryState();
    void updateWatcher();
    void onWatchedProjectChanged(const QString &path);
    void notify(const QString &title, const QString &message, const QString &severity);
    void showError(const QString &message);
    void showSuccess(const QString &message);
    [[nodiscard]] QString localized(const QString &en, const QString &zh) const;
    // Download every resolved catalog file URL sequentially into the project's
    // own payload folder and queue each one; used by downloadUpdateCatalogItem.
    void beginCatalogFileDownloads(const QStringList &urls, const QString &category,
                                   const QString &destinationDir, qint64 perFileByteCap);
    [[nodiscard]] QStringList *listForCategory(wimforge::ProjectConfig &project, const QString &category);
    void reloadPayloadCatalog(bool force = false);
    void restoreStudioState();
    bool persistPackageProfile(const QString &message);
    bool persistUnattendedProfile(const QString &message, bool writeXml = true);
    bool persistWinForgeBridgeRecipe(const QString &message,
                                     const wimforge::WinForgeRecipe &recipe);
    bool applyHistoryState(const wimforge::ActionEvent &event, const QString &message);
    void runOpenCode(const QString &prompt, const std::function<void(const QString &)> &completed);
    void processNextOpenCodeRequest();
    void recreateVmLab();
    void refreshVmValidationRuns();
    void updateVmStatus(const QString &message, const QString &tone = QStringLiteral("info"),
                        const QString &detail = {});
    void appendVmLog(const QString &message);
    void tryPendingVmBoot();
    bool stageVmPreview(const std::optional<wimforge::vmlab::OperationPreview> &preview);

    struct OpenCodeRequest
    {
        QString prompt;
        std::function<void(const QString &)> completed;
    };

    std::optional<wimforge::ProjectConfig> m_project;
    wimforge::NotificationStore m_notificationStore;
    wimforge::JobEngine m_jobEngine;
    QList<wimforge::ServicingOperation> m_plan;
    QList<wimforge::GitCommit> m_history;
    QVariantList m_actionHistoryCache;
    QString m_historyBranch = QStringLiteral("main");
    QStringList m_historyBranchCache{QStringLiteral("main")};
    QList<wimforge::Notification> m_notificationItems;
    wimforge::GpoCatalog m_gpoCatalog;
    QList<wimforge::GpoPolicy> m_gpoSearchResults;
    wimforge::PackageProfile m_packageProfile;
    wimforge::UnattendProfile m_unattendProfile;
    wimforge::WinForgeRecipe m_winForgeRecipe;
    wimforge::WorkspaceTabs m_workspaceTabs;
    wimforge::WinForgeRuntimeContract m_winForgeRuntimeContract;
    std::unique_ptr<wimforge::OpenCodeSetup> m_openCodeSetup;
    std::unique_ptr<wimforge::vmlab::VmLabManager> m_vmManager;
    std::unique_ptr<wimforge::vmvalidation::VmValidationStore> m_vmValidationStore;
    std::jthread m_vmValidationWorker;
    QFileSystemWatcher m_watcher;
    QSettings m_settings;
    QVariantList m_recentProjects;
    QStringList m_editionNames;
    QString m_imageSummaryEn = QStringLiteral("Inspect a source to load edition metadata.");
    QString m_imageSummaryZh = QStringLiteral("檢查來源之後，就會載入映像版本資料。");
    QString m_statusText = QStringLiteral("Ready — no active servicing jobs");
    QJsonObject m_recoveryJournal;
    bool m_pendingRecovery = false;
    bool m_recoveryUnmountBusy = false;
    bool m_inspecting = false;
    bool m_gpoLoaded = false;
    bool m_openCodeRequestBusy = false;
    bool m_openCodeRequestTimedOut = false;
    QProcess *m_openCodeProcess = nullptr;
    QQueue<OpenCodeRequest> m_openCodeRequests;
    QQueue<PendingProjectMutation> m_projectMutationQueue;
    QQueue<PendingWorkspacePersistence> m_workspacePersistenceQueue;
    QString m_gpoStatus = QStringLiteral("Policy catalog has not been loaded yet.");
    QString m_pendingGpoQuery;
    bool m_pendingGpoRegularExpression = false;
    QString m_openCodeRequestStatus;
    QString m_winForgeRuntimePath;
    QString m_winForgeRuntimeStatus = QStringLiteral("No WinForge runtime has been detected yet.");
    QString m_winForgeBridgeStatus = QStringLiteral("Recipe is ready for review.");
    QString m_searchQuery;
    QVariantList m_searchResults;
    QVariantList m_driverCatalogItems;
    QVariantList m_updateCatalogItems;
    QStringList m_catalogDriverPaths;
    QStringList m_catalogUpdatePaths;
    QString m_sourceCatalogQuery;
    QString m_backgroundStatus;
    bool m_projectMutationBusy = false;
    bool m_workspacePersistenceBusy = false;
    bool m_workspacePersistencePaused = false;
    bool m_payloadCatalogBusy = false;
    bool m_payloadDiscoveryBusy = false;
    bool m_planRefreshBusy = false;
    bool m_historyRefreshBusy = false;
    bool m_gpoLoading = false;
    quint64 m_payloadCatalogGeneration = 0;
    quint64 m_planGeneration = 0;
    quint64 m_historyGeneration = 0;
    QNetworkAccessManager *m_catalogNetwork = nullptr;
    QNetworkReply *m_catalogReply = nullptr;
    QVariantList m_updateCatalogResults;
    QString m_updateCatalogStatus;
    bool m_updateCatalogBusy = false;
    double m_updateCatalogDownloadProgress = 0.0;
    QVariantList m_vmValidationItems;
    QVariantMap m_vmPendingPreview;
    QString m_vmStatusMessage;
    QString m_vmStatusTone = QStringLiteral("info");
    QString m_vmStatusDetail;
    QString m_vmLog;
    QString m_pendingVmBootProvider;
    QString m_pendingVmBootId;
    bool m_winForgeIncludeRuntime = true;
    bool m_vmValidationBusy = false;

    int m_languageMode = 2;
    int m_themeMode = 0;
    int m_colorScheme = 1;
    double m_interfaceScale = 1.0;
    bool m_motionEnabled = true;
    int m_maxParallelJobs = 4;
    int m_threadLimit = 4;
    int m_scratchReserveGb = 20;
    bool m_crashJournalEnabled = true;
    bool m_verifySourceHash = true;
    bool m_checkpointBeforeDestructive = true;
};
