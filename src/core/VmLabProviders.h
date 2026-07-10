#pragma once

#include "VmLab.h"

#include <QHash>

#include <optional>

namespace wimforge::vmlab {

namespace capability {
QString inventory();
QString create();
QString registerMachine();
QString openConsole();
QString lifecycle();
QString configure();
QString media();
QString snapshots();
QString unregisterMachine();
QString deleteMachine();
QString secureBoot();
QString tpm();
} // namespace capability

struct ProviderProbePaths
{
    QString providerId;
    QString executable;
    QString consoleExecutable;
    QString diskManagerExecutable;
};

class ProviderDetector
{
public:
    static QList<ProviderProbePaths> defaultWindowsCandidates();
    static QList<ProviderInfo> detect(const QList<ProviderProbePaths> &candidates,
                                      CommandRunner &runner);
};

class VirtualBoxProvider
{
public:
    explicit VirtualBoxProvider(ProviderInfo info);

    static ProviderInfo detect(const QString &vboxManagePath,
                               const QString &consolePath,
                               CommandRunner &runner);
    static QList<VmRef> parseMachineList(const QByteArray &output, QString *error = nullptr);
    static std::optional<Machine> parseMachineInfo(const QByteArray &output,
                                                   QString *error = nullptr);
    static QList<Snapshot> parseSnapshotList(const QByteArray &output, QString *error = nullptr);

    [[nodiscard]] Command inventoryCommand() const;
    [[nodiscard]] Command machineInfoCommand(const VmRef &machine) const;
    [[nodiscard]] Plan create(const CreateSpec &spec, const QString &revision,
                              const QDateTime &now) const;
    [[nodiscard]] Plan registerMachine(const QString &vboxPath, const QString &name,
                                       const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan openConsole(const Machine &machine, const QString &revision,
                                   const QDateTime &now) const;
    [[nodiscard]] Plan start(const Machine &machine, bool headless, const QString &revision,
                             const QDateTime &now) const;
    [[nodiscard]] Plan gracefulShutdown(const Machine &machine, const QString &revision,
                                        const QDateTime &now) const;
    [[nodiscard]] Plan powerOff(const Machine &machine, const QString &revision,
                                const QDateTime &now) const;
    [[nodiscard]] Plan pause(const Machine &machine, const QString &revision,
                             const QDateTime &now) const;
    [[nodiscard]] Plan resume(const Machine &machine, const QString &revision,
                              const QDateTime &now) const;
    [[nodiscard]] Plan reset(const Machine &machine, const QString &revision,
                             const QDateTime &now) const;
    [[nodiscard]] Plan saveState(const Machine &machine, const QString &revision,
                                 const QDateTime &now) const;
    [[nodiscard]] Plan configure(const Machine &machine, const ConfigPatch &patch,
                                 const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan attachIso(const Machine &machine, const QString &isoPath,
                                 const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan detachIso(const Machine &machine, const QString &revision,
                                 const QDateTime &now) const;
    [[nodiscard]] Plan listSnapshots(const Machine &machine, const QString &revision,
                                     const QDateTime &now) const;
    [[nodiscard]] Plan takeSnapshot(const Machine &machine, const QString &name,
                                    const QString &description, const QString &revision,
                                    const QDateTime &now) const;
    [[nodiscard]] Plan restoreSnapshot(const Machine &machine, const Snapshot &snapshot,
                                       const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan deleteSnapshot(const Machine &machine, const Snapshot &snapshot,
                                      const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan unregisterMachine(const Machine &machine, const QString &revision,
                                         const QDateTime &now) const;
    [[nodiscard]] Plan deleteMachine(const Machine &machine, const QString &managedRoot,
                                     const QList<Machine> &catalogMachines,
                                     const QString &revision, const QDateTime &now) const;

private:
    ProviderInfo m_info;

    [[nodiscard]] Command command(const QStringList &arguments, int timeoutMs = 30000) const;
    void addPreflight(Plan &plan, const Machine &machine) const;
    [[nodiscard]] Plan control(const Machine &machine, const QString &verb, Risk risk,
                               const QString &effect, const QString &revision,
                               const QDateTime &now) const;
};

class VmwareProvider
{
public:
    explicit VmwareProvider(ProviderInfo info);

    static ProviderInfo detect(const QString &providerId,
                               const QString &vmrunPath,
                               const QString &consolePath,
                               const QString &diskManagerPath,
                               CommandRunner &runner);
    static QStringList parseRunningVmList(const QByteArray &output, QString *error = nullptr);
    static QList<Snapshot> parseSnapshotList(const QByteArray &output, QString *error = nullptr);
    [[nodiscard]] Machine inspectMachine(const QString &vmxPath,
                                         const QStringList &runningVmxPaths,
                                         Ownership ownership,
                                         QString *error = nullptr) const;

    [[nodiscard]] Command inventoryCommand() const;
    [[nodiscard]] Plan create(const CreateSpec &spec, const QString &revision,
                              const QDateTime &now) const;
    [[nodiscard]] Plan registerMachine(const QString &vmxPath, const QString &name,
                                       const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan openConsole(const Machine &machine, const QString &revision,
                                   const QDateTime &now) const;
    [[nodiscard]] Plan start(const Machine &machine, bool headless, const QString &revision,
                             const QDateTime &now) const;
    [[nodiscard]] Plan gracefulShutdown(const Machine &machine, const QString &revision,
                                        const QDateTime &now) const;
    [[nodiscard]] Plan powerOff(const Machine &machine, const QString &revision,
                                const QDateTime &now) const;
    [[nodiscard]] Plan pause(const Machine &machine, const QString &revision,
                             const QDateTime &now) const;
    [[nodiscard]] Plan resume(const Machine &machine, const QString &revision,
                              const QDateTime &now) const;
    [[nodiscard]] Plan reset(const Machine &machine, const QString &revision,
                             const QDateTime &now) const;
    [[nodiscard]] Plan saveState(const Machine &machine, const QString &revision,
                                 const QDateTime &now) const;
    [[nodiscard]] Plan configure(const Machine &machine, const ConfigPatch &patch,
                                 const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan attachIso(const Machine &machine, const QString &isoPath,
                                 const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan detachIso(const Machine &machine, const QString &revision,
                                 const QDateTime &now) const;
    [[nodiscard]] Plan listSnapshots(const Machine &machine, const QString &revision,
                                     const QDateTime &now) const;
    [[nodiscard]] Plan takeSnapshot(const Machine &machine, const QString &name,
                                    const QString &description, const QString &revision,
                                    const QDateTime &now) const;
    [[nodiscard]] Plan restoreSnapshot(const Machine &machine, const Snapshot &snapshot,
                                       const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan deleteSnapshot(const Machine &machine, const Snapshot &snapshot,
                                      const QString &revision, const QDateTime &now) const;
    [[nodiscard]] Plan unregisterMachine(const Machine &machine, const QString &revision,
                                         const QDateTime &now) const;
    [[nodiscard]] Plan deleteMachine(const Machine &machine, const QString &managedRoot,
                                     const QList<Machine> &catalogMachines,
                                     const QString &revision, const QDateTime &now) const;

private:
    ProviderInfo m_info;

    [[nodiscard]] QString targetToken() const;
    [[nodiscard]] Command vmrun(const QStringList &arguments, int timeoutMs = 30000) const;
    void addPreflight(Plan &plan, const Machine &machine) const;
    [[nodiscard]] Plan lifecycle(const Machine &machine, const QStringList &arguments,
                                 const QString &action, Risk risk, const QString &effect,
                                 const QString &revision, const QDateTime &now) const;
};

} // namespace wimforge::vmlab
