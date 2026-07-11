#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace wimforge {

enum class SetupPass
{
    WindowsPE,
    OfflineServicing,
    Generalize,
    Specialize,
    AuditSystem,
    AuditUser,
    OobeSystem
};

enum class ComputerNameMode { Random, Fixed, Prompt, SerialNumber };

struct UnattendPathSegment
{
    QString name;
    QMap<QString, QString> attributes;
};

struct UnattendSetting
{
    SetupPass pass = SetupPass::Specialize;
    QString component;
    QString architecture = QStringLiteral("amd64");
    QString publicKeyToken = QStringLiteral("31bf3856ad364e35");
    QString language = QStringLiteral("neutral");
    QString versionScope = QStringLiteral("nonSxS");
    QList<UnattendPathSegment> path;
    QString value;
};

struct UnattendValidation
{
    QStringList errors;
    QStringList warnings;
    [[nodiscard]] bool ok() const { return errors.isEmpty(); }
};

struct ProductKeyEntry
{
    QString edition;
    QString key;
    QString channel;
    QString documentationUrl;
    QString licensingNotice;
};

class UnattendProfile
{
public:
    static constexpr int CurrentSchemaVersion = 1;

    QString name;
    QString description;
    QList<UnattendSetting> settings;
    bool copyToMediaRoot = true;
    bool copyToInstallImage = false;
    bool copyToBootImage = false;
    bool dualArchitecture = false;
    bool promptEditionSelection = false;
    ComputerNameMode computerNameMode = ComputerNameMode::Random;
    QString computerName;
    QString serialPrefix;
    QJsonObject metadata;

    [[nodiscard]] QJsonObject toJson() const;
    static std::optional<UnattendProfile> fromJson(const QJsonObject &json, QString *error = nullptr);
    static std::optional<UnattendProfile> importXml(const QString &path, QString *error = nullptr);
    [[nodiscard]] QByteArray toXml(QString *error = nullptr) const;
    bool exportXml(const QString &path, QString *error = nullptr) const;
    bool exportJson(const QString &path, QString *error = nullptr) const;
    static std::optional<UnattendProfile> importJson(const QString &path, QString *error = nullptr);

    [[nodiscard]] UnattendValidation validate() const;
    void setValue(SetupPass pass,
                  const QString &component,
                  const QStringList &path,
                  const QString &value,
                  const QString &architecture = QStringLiteral("amd64"));
    [[nodiscard]] QString value(SetupPass pass,
                                const QString &component,
                                const QStringList &path,
                                const QString &architecture = QStringLiteral("amd64")) const;
    void applyComputerNameBehavior();
    // Start the Windows Narrator screen reader automatically at first sign-in
    // via a WimForge-owned oobeSystem first-logon command. The command writes
    // the documented accessibility auto-start registry value.
    void setNarratorAutostart(bool enabled);
    [[nodiscard]] bool narratorAutostartEnabled() const;
};

class UnattendBuilder
{
public:
    static QString passName(SetupPass pass);
    static std::optional<SetupPass> parsePass(const QString &name);
    static QList<ProductKeyEntry> microsoftPublishedGvlks();
    static UnattendProfile fullAutomationTemplate();
    static UnattendProfile aiDevelopmentTemplate();
    static QString computerNamePromptCommand();
    static QString computerNameSerialCommand(const QString &prefix);
    static QString narratorAutostartCommand();
};

} // namespace wimforge
