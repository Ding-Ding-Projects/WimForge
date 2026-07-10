#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <optional>

namespace wimforge::vmlab {

enum class PowerState
{
    Unknown,
    Inaccessible,
    PoweredOff,
    Running,
    Paused,
    Suspended,
    Saved,
    Aborted
};

enum class Ownership { External, Managed };
enum class Risk { ReadOnly, Reversible, Disruptive, Destructive };
enum class Firmware { Bios, Efi };
enum class NetworkMode { Nat, Bridged, HostOnly, Internal, Disconnected };
enum class EvidenceFormat { RawSha256, VmwareRunningPathsSha256 };

[[nodiscard]] QString virtualBoxProviderId();
[[nodiscard]] QString vmwareWorkstationProviderId();
[[nodiscard]] QString vmwarePlayerProviderId();
[[nodiscard]] bool isKnownProviderId(const QString &id);
[[nodiscard]] QString powerStateName(PowerState state);
[[nodiscard]] QString ownershipName(Ownership ownership);
[[nodiscard]] QString riskName(Risk risk);
[[nodiscard]] QString firmwareName(Firmware firmware);
[[nodiscard]] QString networkModeName(NetworkMode mode);
[[nodiscard]] bool isSafeMachineFileStem(const QString &value);

struct ProviderInfo
{
    QString id;
    QString displayName;
    QString executable;
    QString consoleExecutable;
    QString diskManagerExecutable;
    QString version;
    QStringList evidence;
    QStringList warnings;
    QSet<QString> capabilities;
    bool available = false;

    [[nodiscard]] bool supports(const QString &capability) const;
};

struct VmRef
{
    QString providerId;
    QString id;
    QString name;

    [[nodiscard]] bool valid() const;
};

struct Machine
{
    VmRef ref;
    QString configPath;
    // Live provider inventory only. Catalog persistence intentionally omits
    // storage topology so stale paths can never authorize deletion.
    QStringList storagePaths;
    PowerState powerState = PowerState::Unknown;
    Ownership ownership = Ownership::External;
    QString inaccessibleReason;
    QStringList warnings;
    // Ephemeral hash of the provider response used to derive live state.
    // Catalog persistence intentionally clears this value.
    QString stateRevision;
    bool inventoryComplete = false;
};

struct Snapshot
{
    QString id;
    QString name;
    QString description;
    QDateTime createdAt;
    bool current = false;
};

struct CreateSpec
{
    QString providerId;
    QString id;
    QString name;
    QString directory;
    QString guestType;
    Firmware firmware = Firmware::Efi;
    bool secureBoot = false;
    bool tpm = false;
    // Provider planner resolves 0 to a version-compatible value.
    int virtualHardwareVersion = 0;
    int cpuCount = 2;
    int memoryMiB = 4096;
    int diskMiB = 65536;
    NetworkMode networkMode = NetworkMode::Nat;
    QString bridgedInterface;
    QString isoPath;
    bool unattendedBoot = false;
};

struct ConfigPatch
{
    std::optional<int> cpuCount;
    std::optional<int> memoryMiB;
    std::optional<Firmware> firmware;
    std::optional<bool> secureBoot;
    std::optional<bool> tpm;
    std::optional<NetworkMode> networkMode;
    std::optional<QString> bridgedInterface;
    // An empty value detaches the ISO. A non-empty value must be absolute.
    std::optional<QString> isoPath;

    [[nodiscard]] bool empty() const;
};

struct Command
{
    QString executable;
    QStringList arguments;
    QString workingDirectory;
    int timeoutMs = 30000;

    [[nodiscard]] bool valid(QString *error = nullptr) const;
};

struct OperationPreview
{
    QUuid id;
    QString action;
    VmRef target;
    Risk risk = Risk::ReadOnly;
    QStringList effects;
    QStringList warnings;
    QList<Command> commands;
    QString revision;
    QString confirmation;
    QDateTime expiry;

    [[nodiscard]] bool expired(const QDateTime &now = QDateTime::currentDateTimeUtc()) const;
};

struct AtomicWrite
{
    QString path;
    QByteArray contents;
    // Empty means the destination must not exist. Otherwise it is the
    // lowercase SHA-256 of the bytes observed while planning.
    QString expectedSha256;
};

struct CommandEvidence
{
    Command command;
    EvidenceFormat format = EvidenceFormat::RawSha256;
    QString expected;
    QString description;
};

struct ManagedDeletion
{
    Machine machine;
    QString managedRoot;
    QList<Machine> catalogMachines;
    // File identity captured by the reviewed preview. Execution refuses a
    // directory swapped into the same pathname after review.
    QString expectedIdentity;
};

struct Plan
{
    OperationPreview preview;
    QStringList errors;
    QList<AtomicWrite> atomicWritesAfterCommands;
    std::optional<ManagedDeletion> managedDeletionAfterCommands;
    QList<CommandEvidence> preflight;

    [[nodiscard]] bool ok() const { return errors.isEmpty(); }
};

struct ProcessResult
{
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
    QString error;

    [[nodiscard]] bool ok() const;
};

class CommandRunner
{
public:
    virtual ~CommandRunner() = default;
    virtual ProcessResult run(const Command &command) = 0;
};

class ProcessCommandRunner final : public CommandRunner
{
public:
    ProcessResult run(const Command &command) override;
};

struct Result
{
    bool success = false;
    QString error;
    QList<ProcessResult> processes;
};

struct DeletionGuard
{
    bool allowed = false;
    QString canonicalDirectory;
    QString identity;
    QString error;
};

class PathPolicy
{
public:
    static DeletionGuard managedDeletionGuard(const Machine &machine,
                                               const QString &managedRoot,
                                               const QList<Machine> &catalogMachines);
    static bool deleteManagedDirectory(const Machine &machine,
                                       const QString &managedRoot,
                                       const QList<Machine> &catalogMachines,
                                       const QString &expectedIdentity,
                                       QString *error = nullptr);
};

class Catalog
{
public:
    static constexpr int CurrentVersion = 1;

    explicit Catalog(QString path = {});

    [[nodiscard]] QString path() const;
    [[nodiscard]] QList<Machine> machines() const;
    [[nodiscard]] QString revision() const;

    void setMachines(const QList<Machine> &machines);
    bool upsert(const Machine &machine, QString *error = nullptr);
    bool remove(const VmRef &reference);
    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr);

private:
    QString m_path;
    QList<Machine> m_machines;
    QString m_expectedDiskSha256;
    bool m_diskStateKnown = false;
    bool m_expectedMissing = true;
};

class Executor
{
public:
    static bool validate(const Plan &plan,
                         const QString &currentRevision,
                         const QString &typedConfirmation,
                         const QDateTime &now,
                         QString *error = nullptr);
    static Result execute(const Plan &plan,
                          const QString &currentRevision,
                          const QString &typedConfirmation,
                          const QDateTime &now,
                          CommandRunner &runner);
};

[[nodiscard]] OperationPreview makePreview(const QString &action,
                                           const VmRef &target,
                                           Risk risk,
                                           const QStringList &effects,
                                           const QStringList &warnings,
                                           const QList<Command> &commands,
                                           const QString &revision,
                                           const QDateTime &now);
[[nodiscard]] QString fileSha256(const QString &path, QString *error = nullptr);
[[nodiscard]] QString commandEvidence(EvidenceFormat format,
                                      const QByteArray &standardOutput,
                                      QString *error = nullptr);

} // namespace wimforge::vmlab
