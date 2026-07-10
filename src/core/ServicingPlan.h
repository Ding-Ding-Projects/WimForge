#pragma once

#include "ProjectConfig.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace wimforge {

enum class OperationKind
{
    Inspect,
    PrepareWorkspace,
    VerifyPayload,
    StageFile,
    Mount,
    Driver,
    Update,
    Package,
    Feature,
    Capability,
    Appx,
    Component,
    ScheduledTask,
    Registry,
    Unattended,
    PostSetup,
    Cleanup,
    Validate,
    Commit,
    Export,
    Split,
    CreateIso,
    Recovery
};

// Skipped is reserved for an intentional review-time omission. Blocked means
// the operation could not run because a required dependency failed or was
// cancelled; unlike an intentional skip, it makes the overall run fail.
enum class OperationState { Queued, Running, Succeeded, Failed, Skipped, Blocked, Cancelled };

enum class OperationWriteScope
{
    None,
    ProjectWorkspace,
    WorkingImage,
    MountedImage,
    MediaWorkspace,
    Output,
    Host
};

enum class SkipConsequence
{
    None,
    OmitsOptionalChange,
    BlocksDependents,
    LeavesIncompleteOutput
};

struct ServicingOperation
{
    QString id;
    OperationKind kind = OperationKind::Inspect;
    QString titleEn;
    QString titleZh;
    QString descriptionEn;
    QString descriptionZh;
    QString executable;
    QStringList arguments;
    QString workingDirectory;
    QStringList dependsOn;
    bool requiresAdministrator = false;
    bool destructive = false;
    bool rebootRequired = false;
    bool mayRunInParallel = false;
    bool writesMountedImage = false;
    bool writesMediaWorkspace = false;
    bool checkpointBefore = false;
    bool reversible = false;
    OperationWriteScope writeScope = OperationWriteScope::None;
    SkipConsequence skipConsequence = SkipConsequence::None;
    QStringList compatibilityNotes;
    OperationState state = OperationState::Queued;
    QJsonObject metadata;

    [[nodiscard]] QString previewCommand() const;
    [[nodiscard]] QJsonObject toJson() const;
};

struct ServicingPlanResult
{
    QList<ServicingOperation> operations;
    QStringList errors;
    QStringList warnings;
    // Offline servicing always addresses this project-owned image unless the
    // caller explicitly opts in to in-place servicing.
    QString workingImagePath;
    // ISO/media writes are isolated here before oscdimg is allowed to run.
    QString mediaWorkspace;

    [[nodiscard]] bool ok() const { return errors.isEmpty(); }
    [[nodiscard]] int destructiveCount() const;
};

class ServicingPlan
{
public:
    static ServicingPlanResult build(const ProjectConfig &project);
    static QString quoteWindowsArgument(const QString &argument);
    static QString quotePowerShellLiteral(const QString &value);
    static QString operationKindName(OperationKind kind);
    static QString operationStateName(OperationState state);
    static QString writeScopeName(OperationWriteScope scope);
    static QString skipConsequenceName(SkipConsequence consequence);
    static bool exportPowerShell(const ProjectConfig &project,
                                 const QList<ServicingOperation> &operations,
                                 const QString &destination,
                                 QString *error = nullptr);
};

} // namespace wimforge
