#include "core/PackageStudio.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

using namespace wimforge;

namespace {

class TestRun
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int result() const
    {
        if (m_failures == 0)
            QTextStream(stdout) << "package_studio_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

const PackageEntry *findPackage(const PackageProfile &profile, const QString &id)
{
    for (const PackageEntry &package : profile.packages) {
        if (package.id == id)
            return &package;
    }
    return nullptr;
}

QString templatePath()
{
    const QString sourceCandidate = QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
                                        .filePath(QStringLiteral("../templates/ai-development.json"));
    if (QFileInfo::exists(sourceCandidate))
        return QFileInfo(sourceCandidate).absoluteFilePath();

    QDir directory(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 6; ++depth) {
        const QString candidate = directory.filePath(QStringLiteral("templates/ai-development.json"));
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).absoluteFilePath();
        if (!directory.cdUp())
            break;
    }
    return {};
}

PackageEntry customPackage(const QString &id, const QStringList &dependencies = {})
{
    PackageEntry package;
    package.id = id;
    package.displayName = id;
    package.provider = PackageProvider::CustomCommand;
    package.requiresNetwork = false;
    package.dependencies = dependencies;
    package.installCommand = PackageCommand{
        QStringLiteral("safe-installer.exe"), {QStringLiteral("--silent")},
    };
    package.verifyCommand = PackageCommand{
        QStringLiteral("safe-installer.exe"), {QStringLiteral("--version")},
    };
    return package;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgePackageStudioTests"));
    TestRun test;

    PackageProfile builtIn = PackageStudio::fullAiDevelopmentTemplate();
    PackageStudioValidation validation = PackageStudio::validate(builtIn);
    test.check(validation.ok(),
               QStringLiteral("Full AI Development template validates: %1").arg(validation.message()));

    const QJsonObject json = PackageStudio::toJson(builtIn);
    test.check(json.value(QStringLiteral("schema")).toString()
                   == QStringLiteral("wimforge.package-studio"),
               QStringLiteral("exported profile has the package-studio schema"));
    test.check(json.value(QStringLiteral("version")).toInt()
                   == PackageStudio::CurrentSchemaVersion,
               QStringLiteral("exported profile has the current schema version"));
    test.check(json.value(QStringLiteral("packages")).isArray(),
               QStringLiteral("exported profile has a package array"));

    QString error;
    const std::optional<PackageProfile> roundTrip = PackageStudio::fromJson(json, &error);
    test.check(roundTrip.has_value(), QStringLiteral("schema round-trip succeeds: %1").arg(error));
    if (roundTrip)
        test.check(roundTrip->packages.size() == builtIn.packages.size(),
                   QStringLiteral("schema round-trip preserves every catalog entry"));

    const QSet<QString> requiredIdentifiers{
        QStringLiteral("Git.Git"),
        QStringLiteral("OpenJS.NodeJS.LTS"),
        QStringLiteral("Python.Python.3.13"),
        QStringLiteral("Kitware.CMake"),
        QStringLiteral("EclipseAdoptium.Temurin.21.JDK"),
        QStringLiteral("Microsoft.VisualStudio.2022.BuildTools"),
        QStringLiteral("Microsoft.VisualStudioCode"),
        QStringLiteral("Docker.DockerDesktop"),
        QStringLiteral("Anthropic.ClaudeCode"),
        QStringLiteral("Anthropic.Claude"),
    };
    QSet<QString> actualIdentifiers;
    for (const PackageEntry &package : builtIn.packages) {
        if (!package.packageIdentifier.isEmpty())
            actualIdentifiers.insert(package.packageIdentifier);
    }
    for (const QString &required : requiredIdentifiers)
        test.check(actualIdentifiers.contains(required),
                   QStringLiteral("template includes verified package ID %1").arg(required));

    const PackageEntry *opencode = findPackage(builtIn, QStringLiteral("opencode-cli"));
    test.check(opencode != nullptr, QStringLiteral("template includes OpenCode CLI"));
    if (opencode) {
        test.check(opencode->dependencies.contains(QStringLiteral("nodejs-lts")),
                   QStringLiteral("OpenCode depends on Node.js/npm"));
        test.check(opencode->installCommand.executable == QStringLiteral("npm.cmd")
                       && opencode->installCommand.arguments
                              == QStringList{QStringLiteral("install"), QStringLiteral("-g"),
                                             QStringLiteral("opencode-ai@latest")},
                   QStringLiteral("OpenCode uses the official npm install command"));
        test.check(opencode->verifyCommand.executable == QStringLiteral("opencode")
                       && opencode->verifyCommand.arguments == QStringList{QStringLiteral("--version")},
                   QStringLiteral("OpenCode verifies with opencode --version"));
    }

    const PackageEntry *codex = findPackage(builtIn, QStringLiteral("codex-cli"));
    test.check(codex && codex->installCommand.arguments.contains(QStringLiteral("@openai/codex"))
                   && codex->verifyCommand.executable == QStringLiteral("codex"),
               QStringLiteral("Codex uses @openai/codex and codex --version"));
    const PackageEntry *codexDesktop = findPackage(builtIn, QStringLiteral("codex-desktop"));
    test.check(codexDesktop && !codexDesktop->enabled && codexDesktop->optional
                   && codexDesktop->provider == PackageProvider::OfflinePayload
                   && codexDesktop->vendorReleasePage.contains(QStringLiteral("openai/codex")),
               QStringLiteral("Codex desktop is an explicit official-payload slot without an invented package ID"));
    const PackageEntry *claudeCode = findPackage(builtIn, QStringLiteral("claude-code"));
    test.check(claudeCode && claudeCode->packageIdentifier == QStringLiteral("Anthropic.ClaudeCode")
                   && PackageStudio::effectiveInstallCommand(*claudeCode).arguments.contains(
                       QStringLiteral("-e"))
                   && claudeCode->verifyCommand.executable == QStringLiteral("claude"),
               QStringLiteral("Claude Code uses the verified WinGet ID and live CLI verification"));
    const PackageEntry *chatgpt = findPackage(builtIn, QStringLiteral("chatgpt-desktop"));
    test.check(chatgpt && !chatgpt->enabled && chatgpt->optional
                   && chatgpt->provider == PackageProvider::OfflinePayload
                   && chatgpt->packageIdentifier.isEmpty(),
               QStringLiteral("ChatGPT Desktop is optional and does not invent a WinGet ID"));

    const QString diskTemplate = templatePath();
    test.check(!diskTemplate.isEmpty(), QStringLiteral("prebuilt template JSON is present"));
    if (!diskTemplate.isEmpty()) {
        const std::optional<PackageProfile> imported = PackageStudio::importJson(diskTemplate, &error);
        test.check(imported.has_value(),
                   QStringLiteral("prebuilt template JSON imports and validates: %1").arg(error));
        if (imported) {
            const PackageEntry *diskOpenCode = findPackage(*imported, QStringLiteral("opencode-cli"));
            test.check(diskOpenCode
                           && diskOpenCode->installCommand.arguments.contains(
                               QStringLiteral("opencode-ai@latest")),
                       QStringLiteral("disk template preserves automatic OpenCode installation"));
            test.check(PackageStudio::toJson(*imported) == json,
                       QStringLiteral("disk and compiled Full AI Development templates are identical"));
        }
    }

    PackageProfile ordering;
    ordering.name = QStringLiteral("Dependency order");
    ordering.networkMode = PackageNetworkMode::Offline;
    ordering.packages = {
        customPackage(QStringLiteral("application"), {QStringLiteral("runtime")}),
        customPackage(QStringLiteral("runtime"), {QStringLiteral("compiler")}),
        customPackage(QStringLiteral("compiler")),
    };
    const std::optional<QList<PackageEntry>> ordered = PackageStudio::dependencyOrder(ordering, &error);
    test.check(ordered.has_value(), QStringLiteral("acyclic dependencies sort: %1").arg(error));
    if (ordered) {
        test.check(ordered->size() == 3
                       && ordered->at(0).id == QStringLiteral("compiler")
                       && ordered->at(1).id == QStringLiteral("runtime")
                       && ordered->at(2).id == QStringLiteral("application"),
                   QStringLiteral("dependencies appear before their consumers"));
    }

    PackageProfile cycle = ordering;
    cycle.packages = {
        customPackage(QStringLiteral("alpha"), {QStringLiteral("beta")}),
        customPackage(QStringLiteral("beta"), {QStringLiteral("alpha")}),
    };
    test.check(!PackageStudio::dependencyOrder(cycle, &error)
                   && error.contains(QStringLiteral("cycle"), Qt::CaseInsensitive),
               QStringLiteral("dependency cycle is rejected with a readable error"));
    test.check(!PackageStudio::validate(cycle).ok(),
               QStringLiteral("normal profile validation also rejects cycles"));

    QString safetyReason;
    test.check(PackageStudio::isCommandSafe(
                   PackageCommand{QStringLiteral("installer.exe"), {QStringLiteral("--quiet")}},
                   &safetyReason),
               QStringLiteral("structured executable arguments are accepted: %1").arg(safetyReason));
    test.check(!PackageStudio::isCommandSafe(
                   PackageCommand{QStringLiteral("cmd.exe"),
                                  {QStringLiteral("/c"), QStringLiteral("payload.exe")}},
                   &safetyReason),
               QStringLiteral("shell command wrappers are rejected"));
    test.check(!PackageStudio::isCommandSafe(
                   PackageCommand{QStringLiteral("installer.exe"),
                                  {QStringLiteral("--quiet;Remove-Item")}},
                   &safetyReason),
               QStringLiteral("shell metacharacters are rejected"));
    test.check(!PackageStudio::isCommandSafe(
                   PackageCommand{QStringLiteral("installer.exe"),
                                  {QStringLiteral("--api-key=sk-this-is-a-secret-value")}},
                   &safetyReason),
               QStringLiteral("embedded credentials are rejected"));

    PackageProfile providerCoverage;
    providerCoverage.name = QStringLiteral("Provider coverage");
    providerCoverage.networkMode = PackageNetworkMode::Online;
    PackageEntry winget = customPackage(QStringLiteral("winget-entry"));
    winget.provider = PackageProvider::Winget;
    winget.packageIdentifier = QStringLiteral("Vendor.App");
    winget.requiresNetwork = true;
    winget.installCommand = {};
    PackageEntry npm = customPackage(QStringLiteral("npm-entry"));
    npm.provider = PackageProvider::Npm;
    npm.packageIdentifier = QStringLiteral("package-name");
    npm.installCommand = {};
    PackageEntry pip = customPackage(QStringLiteral("pip-entry"));
    pip.provider = PackageProvider::Pip;
    pip.packageIdentifier = QStringLiteral("package_name");
    pip.installCommand = {};
    PackageEntry direct = customPackage(QStringLiteral("direct-entry"));
    direct.provider = PackageProvider::DirectSignedInstaller;
    direct.downloadUrl = QStringLiteral("https://vendor.example/installer.exe");
    direct.expectedSha256 = QString(64, QLatin1Char('a'));
    direct.expectedPublisher = QStringLiteral("Example Publisher");
    direct.installCommand = {};
    PackageEntry offline = customPackage(QStringLiteral("offline-entry"));
    offline.provider = PackageProvider::OfflinePayload;
    offline.requiresNetwork = false;
    offline.offlinePayload = QStringLiteral("payloads/example/installer.exe");
    offline.expectedSha256 = QString(64, QLatin1Char('b'));
    offline.expectedPublisher = QStringLiteral("Example Publisher");
    offline.installCommand = {};
    PackageEntry custom = customPackage(QStringLiteral("custom-entry"));
    providerCoverage.packages = {winget, npm, pip, direct, offline, custom};
    test.check(PackageStudio::validate(providerCoverage).ok(),
               QStringLiteral("all six provider kinds have executable model support"));

    PackageProfile offlineInvalid = builtIn;
    offlineInvalid.networkMode = PackageNetworkMode::Offline;
    validation = PackageStudio::validate(offlineInvalid);
    test.check(!validation.ok()
                   && validation.message().contains(QStringLiteral("network-only")),
               QStringLiteral("offline mode cannot accidentally retain online-only selections"));

    const QString script = PackageStudio::generateFirstLogonPowerShell(builtIn, &error);
    test.check(!script.isEmpty(), QStringLiteral("first-logon script generates: %1").arg(error));
    test.check(script.contains(QStringLiteral("wimforge.package-state"))
                   && script.contains(QStringLiteral("ConvertFrom-Json"))
                   && script.contains(QStringLiteral("ConvertTo-Json"))
                   && script.contains(QStringLiteral(".json")),
               QStringLiteral("script uses per-package JSON resume markers"));
    test.check(script.contains(QStringLiteral("Test-WimForgeInstalled"))
                   && script.contains(QStringLiteral("Detected an existing installation"))
                   && script.contains(QStringLiteral("opencode-ai@latest")),
               QStringLiteral("script is install-if-missing, including OpenCode"));
    test.check(script.contains(QStringLiteral("Wait-WimForgeNetwork"))
                   && script.contains(QStringLiteral("$RetryCount"))
                   && script.contains(QStringLiteral("Status 'installing'"))
                   && script.contains(QStringLiteral("Status 'installed'"))
                   && script.contains(QStringLiteral("Status 'failed'")),
               QStringLiteral("script has network wait, retries, and crash-safe state transitions"));
    test.check(!script.contains(QStringLiteral("sk-"))
                   && !script.contains(QStringLiteral("OPENAI_API_KEY"))
                   && !script.contains(QStringLiteral("ANTHROPIC_API_KEY")),
               QStringLiteral("generated script contains no embedded secrets"));
    const qsizetype nodePosition = script.indexOf(QStringLiteral("Id = 'nodejs-lts'"));
    test.check(nodePosition >= 0
                   && nodePosition < script.indexOf(QStringLiteral("Id = 'opencode-cli'"))
                   && nodePosition < script.indexOf(QStringLiteral("Id = 'codex-cli'")),
               QStringLiteral("generated script preserves dependency order for npm AI tools"));

    QTemporaryDir scriptTemporary;
    test.check(scriptTemporary.isValid(), QStringLiteral("temporary script directory is available"));
    if (scriptTemporary.isValid()) {
        const QString generatedPath = QDir(scriptTemporary.path()).filePath(
            QStringLiteral("first-logon.ps1"));
        QFile generated(generatedPath);
        const QByteArray scriptBytes = script.toUtf8();
        test.check(generated.open(QIODevice::WriteOnly)
                       && generated.write(scriptBytes) == scriptBytes.size(),
                   QStringLiteral("generated script can be written for parser validation"));
        generated.close();

        QProcess parser;
        QString quotedGeneratedPath = generatedPath;
        quotedGeneratedPath.replace(QLatin1Char('\''), QStringLiteral("''"));
        const QString parserCommand = QStringLiteral(
            "$tokens=$null;$errors=$null;"
            "[System.Management.Automation.Language.Parser]::ParseFile('%1',"
            "[ref]$tokens,[ref]$errors)|Out-Null;"
            "if($errors.Count -gt 0){$errors|ForEach-Object{$_.ToString()}|"
            "Write-Error;exit 1}").arg(quotedGeneratedPath);
        parser.start(
            QStringLiteral("powershell.exe"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
             QStringLiteral("-Command"), parserCommand});
        const bool parserFinished = parser.waitForFinished(30000);
        test.check(parserFinished && parser.exitStatus() == QProcess::NormalExit
                       && parser.exitCode() == 0,
                   QStringLiteral("generated first-logon PowerShell parses cleanly: %1")
                       .arg(QString::fromUtf8(parser.readAllStandardError())));
    }

    const QJsonObject staging = PackageStudio::generateIsoStagingManifest(builtIn, &error);
    test.check(!staging.isEmpty()
                   && staging.value(QStringLiteral("schema")).toString()
                          == QStringLiteral("wimforge.package-staging")
                   && staging.value(QStringLiteral("firstLogonScript")).toString().endsWith(
                       QStringLiteral("first-logon.ps1"))
                   && staging.value(QStringLiteral("payloads")).isArray(),
               QStringLiteral("ISO staging manifest describes scripts, state, and optional payloads"));

    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary export directory is available"));
    if (temporary.isValid()) {
        const QString exportPath = QDir(temporary.path()).filePath(QStringLiteral("profiles/ai.json"));
        test.check(PackageStudio::exportJson(builtIn, exportPath, &error),
                   QStringLiteral("profile exports to JSON: %1").arg(error));
        const std::optional<PackageProfile> imported = PackageStudio::importJson(exportPath, &error);
        test.check(imported && imported->name == builtIn.name,
                   QStringLiteral("exported profile imports again: %1").arg(error));
    }

    QTemporaryDir stagingTemporary;
    test.check(stagingTemporary.isValid(), QStringLiteral("staging temporary directory is available"));
    if (stagingTemporary.isValid()) {
        const QString sourceRoot = QDir(stagingTemporary.path()).filePath(QStringLiteral("source"));
        const QString bundleRoot = QDir(stagingTemporary.path()).filePath(QStringLiteral("bundle"));
        const QString payloadRelative = QStringLiteral("payloads/example/setup.exe");
        const QString payloadPath = QDir(sourceRoot).filePath(payloadRelative);
        QDir().mkpath(QFileInfo(payloadPath).absolutePath());
        const QByteArray payloadBytes("signed fixture payload");
        QFile payload(payloadPath);
        test.check(payload.open(QIODevice::WriteOnly)
                       && payload.write(payloadBytes) == payloadBytes.size(),
                   QStringLiteral("offline staging fixture is written"));
        payload.close();

        PackageProfile stagedProfile;
        stagedProfile.name = QStringLiteral("Offline staging");
        stagedProfile.networkMode = PackageNetworkMode::Offline;
        PackageEntry stagedPackage = customPackage(QStringLiteral("offline-tool"));
        stagedPackage.provider = PackageProvider::OfflinePayload;
        stagedPackage.installCommand = {};
        stagedPackage.offlinePayload = payloadRelative;
        stagedPackage.expectedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256).toHex());
        stagedPackage.expectedPublisher = QStringLiteral("Fixture Publisher");
        stagedProfile.packages = {stagedPackage};

        const auto materialized = PackageStudio::materializeFirstLogonBundle(
            stagedProfile, sourceRoot, bundleRoot, &error);
        test.check(materialized.has_value(),
                   QStringLiteral("complete first-logon bundle materializes: %1").arg(error));
        if (materialized) {
            test.check(QFileInfo::exists(QDir(bundleRoot).filePath(QStringLiteral("first-logon.ps1")))
                           && QFileInfo::exists(QDir(bundleRoot).filePath(
                               QStringLiteral("register-first-logon.ps1")))
                           && QFileInfo::exists(QDir(bundleRoot).filePath(
                               QStringLiteral("package-profile.json")))
                           && QFileInfo::exists(QDir(bundleRoot).filePath(
                               QStringLiteral("staging-manifest.json")))
                           && QFileInfo::exists(QDir(bundleRoot).filePath(payloadRelative))
                           && materialized->files.size() == 5,
                       QStringLiteral("bundle contains scripts, portable metadata, and copied payload"));
            QFile registration(QDir(bundleRoot).filePath(
                QStringLiteral("register-first-logon.ps1")));
            const bool registrationOpen = registration.open(QIODevice::ReadOnly);
            const QByteArray registrationBytes = registrationOpen ? registration.readAll() : QByteArray();
            test.check(registrationOpen
                           && registrationBytes.contains("Register-ScheduledTask")
                           && registrationBytes.contains("S-1-5-32-544"),
                       QStringLiteral("registration script creates the elevated first-logon task"));
        }

        stagedProfile.packages[0].expectedSha256 = QString(64, QLatin1Char('0'));
        const auto rejected = PackageStudio::materializeFirstLogonBundle(
            stagedProfile, sourceRoot,
            QDir(stagingTemporary.path()).filePath(QStringLiteral("bad-bundle")), &error);
        test.check(!rejected && error.contains(QStringLiteral("SHA-256")),
                   QStringLiteral("payload hash mismatch prevents staging"));
    }

    return test.result();
}
