#include "core/WinForgeBridge.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace wimforge;

namespace {

class TestRun
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition) {
            qInfo().noquote() << QStringLiteral("PASS: %1").arg(message);
        } else {
            qCritical().noquote() << QStringLiteral("FAIL: %1").arg(message);
            ++m_failures;
        }
    }
    [[nodiscard]] int result() const { return m_failures == 0 ? 0 : 1; }

private:
    int m_failures = 0;
};

bool writeFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

WinForgeRecipe directRecipe()
{
    WinForgeRecipe recipe;
    recipe.id = QStringLiteral("developer-workstation");
    recipe.name = QStringLiteral("Developer workstation");
    recipe.description = QStringLiteral("Typed replay test");
    recipe.createdUtc = QStringLiteral("2026-07-10T12:00:00.000Z");

    WinForgeAction command;
    command.id = QStringLiteral("install-git");
    command.idempotencyKey = command.id;
    command.kind = WinForgeActionKind::Command;
    command.phase = WinForgeActionPhase::Machine;
    command.executable = QStringLiteral("winget.exe");
    command.arguments = {QStringLiteral("install"), QStringLiteral("--id"),
                         QStringLiteral("Git.Git"), QStringLiteral("hello & goodbye")};
    command.successExitCodes = {0, 3010};
    recipe.actions.append(command);
    return recipe;
}

WinForgeRecipe pageRecipe()
{
    WinForgeRecipe recipe = directRecipe();
    WinForgeAction page;
    page.id = QStringLiteral("open-reactor");
    page.idempotencyKey = page.id;
    page.kind = WinForgeActionKind::Page;
    page.phase = WinForgeActionPhase::User;
    page.target = QStringLiteral("reactor");
    recipe.actions.append(page);
    return recipe;
}

bool makeDirectoryLink(const QString &linkPath, const QString &targetPath)
{
#ifdef Q_OS_WIN
    QProcess process;
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setArguments({QStringLiteral("/d"), QStringLiteral("/c"),
                          QStringLiteral("mklink"), QStringLiteral("/J"),
                          QDir::toNativeSeparators(linkPath),
                          QDir::toNativeSeparators(targetPath)});
    process.start();
    return process.waitForFinished(10000) && process.exitCode() == 0
        && QFileInfo(linkPath).isJunction();
#else
    return QFile::link(targetPath, linkPath) && QFileInfo(linkPath).isSymLink();
#endif
}

bool parsePowerShellAst(const QString &scriptPath, QString *details)
{
    const QString powerShell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (powerShell.isEmpty()) {
        if (details)
            *details = QStringLiteral("Windows PowerShell is not installed; AST check skipped.");
        return true;
    }
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WIMFORGE_AST_SCRIPT"), scriptPath);
    process.setProcessEnvironment(environment);
    process.setProgram(powerShell);
    process.setArguments({
        QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
        QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
        QStringLiteral("$t=$null;$e=$null;[System.Management.Automation.Language.Parser]::ParseFile($env:WIMFORGE_AST_SCRIPT,[ref]$t,[ref]$e)>$null;if($e.Count){$e|ForEach-Object{$_.Message};exit 7}")});
    process.start();
    if (!process.waitForFinished(30000)) {
        if (details)
            *details = QStringLiteral("PowerShell AST parser timed out.");
        return false;
    }
    if (details)
        *details = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary directory is available"));
    if (!temporary.isValid())
        return test.result();

    QString error;
    WinForgeRecipe direct = directRecipe();
    WinForgeBridgeValidation validation = WinForgeBridge::validateRecipe(direct);
    test.check(validation.ok(), QStringLiteral("typed direct recipe validates: %1").arg(validation.message()));

    const QString recipePath = QDir(temporary.path()).filePath(QStringLiteral("recipe/bridge.json"));
    test.check(WinForgeBridge::exportJson(direct, recipePath, &error),
               QStringLiteral("recipe exports atomically: %1").arg(error));
    const auto imported = WinForgeBridge::importJson(recipePath, &error);
    test.check(imported.has_value() && imported->actions.size() == direct.actions.size()
                   && imported->actions.first().arguments.last() == QStringLiteral("hello & goodbye"),
               QStringLiteral("round trip preserves exact argument tokens: %1").arg(error));

    QFile exported(recipePath);
    test.check(exported.open(QIODevice::ReadOnly), QStringLiteral("exported recipe can be inspected"));
    QJsonObject hostileJson = QJsonDocument::fromJson(exported.readAll()).object();
    QJsonArray hostileActions = hostileJson.value(QStringLiteral("actions")).toArray();
    QJsonObject tamperedAction = hostileActions.first().toObject();
    tamperedAction.insert(QStringLiteral("executable"), QStringLiteral("other.exe"));
    hostileActions.replace(0, tamperedAction);
    hostileJson.insert(QStringLiteral("actions"), hostileActions);
    const auto tampered = WinForgeBridge::fromJson(hostileJson, &error);
    test.check(!tampered && error.contains(QStringLiteral("digest"), Qt::CaseInsensitive),
               QStringLiteral("tampered action fails its canonical digest: %1").arg(error));

    hostileJson = WinForgeBridge::toJson(direct);
    hostileJson.insert(QStringLiteral("surprise"), true);
    test.check(!WinForgeBridge::fromJson(hostileJson, &error)
                   && error.contains(QStringLiteral("unknown"), Qt::CaseInsensitive),
               QStringLiteral("unknown recipe fields fail closed: %1").arg(error));

    WinForgeRecipe unsafeCommand = directRecipe();
    unsafeCommand.actions.first().executable = QStringLiteral("powershell.exe");
    test.check(!WinForgeBridge::validateRecipe(unsafeCommand).ok(),
               QStringLiteral("script interpreter executable is refused"));
    unsafeCommand.actions.first().executable = QStringLiteral("winget.exe install --id Git.Git");
    test.check(!WinForgeBridge::validateRecipe(unsafeCommand).ok(),
               QStringLiteral("combined relative command line is refused"));
    test.check(WinForgeBridge::validateRecipe(direct).ok(),
               QStringLiteral("metacharacters remain safe when contained in a distinct argument token"));

    WinForgeRecipe traversal = directRecipe();
    traversal.actions.clear();
    WinForgeAction traversalCopy;
    traversalCopy.id = QStringLiteral("escape");
    traversalCopy.kind = WinForgeActionKind::Copy;
    traversalCopy.phase = WinForgeActionPhase::Machine;
    traversalCopy.sourceRelative = QStringLiteral("../outside.bin");
    traversalCopy.destination = QStringLiteral("%ProgramData%\\outside.bin");
    traversalCopy.sha256 = QString(64, QLatin1Char('0'));
    traversal.actions.append(traversalCopy);
    test.check(!WinForgeBridge::validateRecipe(traversal).ok(),
               QStringLiteral("copy path traversal is rejected before staging"));

    const QString runtimeRoot = QDir(temporary.path()).filePath(QStringLiteral("runtime"));
    test.check(writeFile(QDir(runtimeRoot).filePath(QStringLiteral("WinForge.exe")),
                         QByteArrayLiteral("fake self-contained runtime")),
               QStringLiteral("legacy runtime fixture exists"));
    WinForgeRuntimeContract legacy = WinForgeBridge::detectRuntimeContract(runtimeRoot, &error);
    test.check(legacy.runtimeFound && !legacy.declaredContract
                   && legacy.capabilities == QStringList{QStringLiteral("launch.page.v1")}
                   && legacy.invocations.value(QStringLiteral("launch.page.v1"))
                       == QStringList{QStringLiteral("--page"), QStringLiteral("{target}")},
               QStringLiteral("legacy detection exposes only observed --page contract: %1").arg(error));

    const QString declaredRoot = QDir(temporary.path()).filePath(QStringLiteral("declared-runtime"));
    test.check(writeFile(QDir(declaredRoot).filePath(QStringLiteral("WinForge.exe")),
                         QByteArrayLiteral("declared runtime")),
               QStringLiteral("declared runtime executable exists"));
    QJsonObject invocations;
    invocations.insert(QStringLiteral("launch.page.v1"),
                       QJsonArray{QStringLiteral("--page"), QStringLiteral("{target}")});
    invocations.insert(QStringLiteral("apply.module.v1"),
                       QJsonArray{QStringLiteral("--bridge-module"), QStringLiteral("{target}"),
                                  QStringLiteral("{action-id}")});
    QJsonObject declaredJson;
    declaredJson.insert(QStringLiteral("format"), QStringLiteral("org.winforge.runtime-contract"));
    declaredJson.insert(QStringLiteral("formatVersion"), 1);
    declaredJson.insert(QStringLiteral("contractVersion"), 2);
    declaredJson.insert(QStringLiteral("runtimeVersion"), QStringLiteral("1.2.3"));
    declaredJson.insert(QStringLiteral("executable"), QStringLiteral("WinForge.exe"));
    declaredJson.insert(QStringLiteral("capabilities"),
                        QJsonArray{QStringLiteral("launch.page.v1"),
                                   QStringLiteral("apply.module.v1")});
    declaredJson.insert(QStringLiteral("invocations"), invocations);
    test.check(writeFile(QDir(declaredRoot).filePath(QStringLiteral("winforge-contract.json")),
                         QJsonDocument(declaredJson).toJson(QJsonDocument::Indented)),
               QStringLiteral("declared runtime contract fixture exists"));
    const WinForgeRuntimeContract declared =
        WinForgeBridge::detectRuntimeContract(declaredRoot, &error);
    test.check(declared.runtimeFound && declared.declaredContract
                   && declared.contractVersion == 2
                   && declared.runtimeVersion == QStringLiteral("1.2.3")
                   && declared.capabilities.contains(QStringLiteral("apply.module.v1")),
               QStringLiteral("declared contract capability and version detection is exact: %1").arg(error));

    WinForgeRecipe page = pageRecipe();
    validation = WinForgeBridge::validateAgainstRuntime(page, legacy);
    test.check(validation.ok(), QStringLiteral("source-verified page action matches legacy runtime"));
    WinForgeAction module;
    module.id = QStringLiteral("module-settings");
    module.kind = WinForgeActionKind::Module;
    module.phase = WinForgeActionPhase::User;
    module.target = QStringLiteral("module.settingshub");
    page.actions.append(module);
    validation = WinForgeBridge::validateAgainstRuntime(page, legacy);
    test.check(!validation.ok()
                   && validation.message().contains(QStringLiteral("apply.module.v1")),
               QStringLiteral("legacy runtime never claims a nonexistent headless module command"));
    page.actions.removeLast();

    const QString outsideRuntime = QDir(temporary.path()).filePath(QStringLiteral("outside-runtime"));
    test.check(writeFile(QDir(outsideRuntime).filePath(QStringLiteral("outside.dll")),
                         QByteArrayLiteral("outside")),
               QStringLiteral("outside link target exists"));
    const QString runtimeLink = QDir(runtimeRoot).filePath(QStringLiteral("linked"));
    const bool linkCreated = makeDirectoryLink(runtimeLink, outsideRuntime);
    test.check(linkCreated, QStringLiteral("runtime link/reparse fixture is created"));
    if (linkCreated) {
        WinForgeStageOptions linkedOptions;
        linkedOptions.includeRuntime = true;
        const auto linkedStage = WinForgeBridge::stageForIso(
            page, runtimeRoot,
            QDir(temporary.path()).filePath(QStringLiteral("linked-iso")),
            linkedOptions, &error);
        test.check(!linkedStage
                       && (error.contains(QStringLiteral("link"), Qt::CaseInsensitive)
                           || error.contains(QStringLiteral("reparse"), Qt::CaseInsensitive)),
                   QStringLiteral("full runtime staging refuses links rather than following them: %1").arg(error));
#ifdef Q_OS_WIN
        test.check(QDir(runtimeRoot).rmdir(QStringLiteral("linked")),
                   QStringLiteral("junction is unlinked without touching its target"));
#else
        test.check(QFile::remove(runtimeLink), QStringLiteral("symbolic link fixture is removed"));
#endif
        test.check(QFileInfo::exists(QDir(outsideRuntime).filePath(QStringLiteral("outside.dll"))),
                   QStringLiteral("outside link target remains untouched"));
    }

    WinForgeStageOptions runtimeOptions;
    runtimeOptions.includeRuntime = true;
    const auto runtimeStage = WinForgeBridge::stageForIso(
        page, runtimeRoot,
        QDir(temporary.path()).filePath(QStringLiteral("runtime-iso")),
        runtimeOptions, &error);
    test.check(runtimeStage.has_value()
                   && QFileInfo::exists(QDir(runtimeStage ? runtimeStage->bundleDirectory : QString())
                                            .filePath(QStringLiteral("Runtime/WinForge.exe")))
                   && runtimeStage->runtimeContract.capabilities
                          == QStringList{QStringLiteral("launch.page.v1")},
               QStringLiteral("optional full legacy runtime is copied with only its honest contract: %1")
                   .arg(error));

    const QByteArray payloadBytes = QByteArrayLiteral("verified payload\n");
    const QString payloadRoot = QDir(temporary.path()).filePath(QStringLiteral("payload"));
    test.check(writeFile(QDir(payloadRoot).filePath(QStringLiteral("tools/helper.exe")), payloadBytes),
               QStringLiteral("copy payload fixture exists"));
    WinForgeRecipe copyRecipe = directRecipe();
    copyRecipe.actions.clear();
    WinForgeAction copy;
    copy.id = QStringLiteral("copy-helper");
    copy.kind = WinForgeActionKind::Copy;
    copy.phase = WinForgeActionPhase::Machine;
    copy.sourceRelative = QStringLiteral("tools/helper.exe");
    copy.destination = QStringLiteral("%ProgramData%\\WimForge\\helper.exe");
    copy.sha256 = QString(64, QLatin1Char('0'));
    copy.overwrite = true;
    copyRecipe.actions.append(copy);
    WinForgeStageOptions directOptions;
    directOptions.includeRuntime = false;
    directOptions.payloadDirectory = payloadRoot;
    const QString checksumIso = QDir(temporary.path()).filePath(QStringLiteral("checksum-iso"));
    test.check(!WinForgeBridge::stageForIso(copyRecipe, {}, checksumIso, directOptions, &error)
                   && error.contains(QStringLiteral("checksum"), Qt::CaseInsensitive),
               QStringLiteral("staging fails closed on copy payload checksum mismatch: %1").arg(error));

    copyRecipe.actions.first().sha256 = digest(payloadBytes);
    const auto staged = WinForgeBridge::stageForIso(copyRecipe, {}, checksumIso, directOptions, &error);
    test.check(staged.has_value(), QStringLiteral("verified direct bundle stages without runtime: %1").arg(error));
    if (staged) {
        const QString stagedPayload = QDir(staged->bundleDirectory).filePath(
            QStringLiteral("Payload/tools/helper.exe"));
        QFile payload(stagedPayload);
        test.check(payload.open(QIODevice::ReadOnly) && payload.readAll() == payloadBytes,
                   QStringLiteral("staged payload bytes are exact"));
        test.check(QFileInfo::exists(staged->manifestPath)
                       && QFileInfo::exists(staged->bootstrapPath)
                       && QFileInfo::exists(staged->setupCompletePath),
                   QStringLiteral("offline manifest, bootstrap, and setup hook are staged"));
        const auto stagedAgain = WinForgeBridge::stageForIso(
            copyRecipe, {}, checksumIso, directOptions, &error);
        test.check(stagedAgain.has_value()
                       && stagedAgain->manifestSha256 == staged->manifestSha256,
                    QStringLiteral("re-staging an unchanged recipe is idempotent: %1").arg(error));
    }

    const QString mergeIso = QDir(temporary.path()).filePath(QStringLiteral("merge-iso"));
    const QString existingSetup = QDir(mergeIso).filePath(
        QStringLiteral("sources/$OEM$/$$/Setup/Scripts/SetupComplete.cmd"));
    const QByteArray existingSetupBytes = QByteArrayLiteral("@echo off\r\necho existing setup work\r\n");
    test.check(writeFile(existingSetup, existingSetupBytes),
               QStringLiteral("existing SetupComplete fixture is written"));
    const auto mergedStage = WinForgeBridge::stageForIso(
        copyRecipe, {}, mergeIso, directOptions, &error);
    QFile mergedSetup(existingSetup);
    const bool mergedOpen = mergedSetup.open(QIODevice::ReadOnly);
    const QByteArray mergedBytes = mergedOpen ? mergedSetup.readAll() : QByteArray();
    test.check(mergedStage && !mergedStage->setupCompleteNeedsMerge
                   && mergedBytes.contains("WimForgeBridge.")
                   && mergedBytes.contains(existingSetupBytes),
               QStringLiteral("existing SetupComplete is atomically preserved and hooked: %1")
                   .arg(error));

    const QString script = WinForgeBridge::generateBootstrapPowerShell(direct, {}, &error);
    test.check(!script.isEmpty()
                   && !script.contains(QStringLiteral("Invoke-Expression"), Qt::CaseInsensitive)
                   && !script.contains(QStringLiteral("iex "), Qt::CaseInsensitive)
                   && script.contains(QStringLiteral("ProcessStartInfo"))
                   && script.contains(QStringLiteral("idempotencyKey"))
                   && script.contains(QStringLiteral("Get-FileHash")),
               QStringLiteral("bootstrap is non-eval, tokenized, checksummed, and resumable: %1").arg(error));
    const QString scriptPath = QDir(temporary.path()).filePath(QStringLiteral("bootstrap-under-test.ps1"));
    test.check(writeFile(scriptPath, script.toUtf8()), QStringLiteral("generated PowerShell is written for AST parsing"));
    QString astDetails;
    test.check(parsePowerShellAst(scriptPath, &astDetails),
               QStringLiteral("generated PowerShell passes the installed parser AST check: %1").arg(astDetails));

    return test.result();
}
