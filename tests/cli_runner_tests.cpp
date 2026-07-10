#include "cli/CliRunner.h"
#include "core/ProjectConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSharedPointer>
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
            QTextStream(stdout) << "cli_runner_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}

QString jsonArgument(const QJsonValue &value)
{
    const QByteArray encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}

QJsonObject jsonEnvelope(const CliResult &result)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(result.standardOutput.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject();
}

QJsonValue jsonResult(const CliResult &result)
{
    return jsonEnvelope(result).value(QStringLiteral("result"));
}

struct FakeProcesses
{
    int calls = 0;
    int installCalls = 0;
    bool nodeInstalled = false;
    bool openCodeInstalled = false;
};

CliProcessResult successProcess(const QByteArray &output = {})
{
    CliProcessResult result;
    result.started = true;
    result.finished = true;
    result.exitCode = 0;
    result.standardOutput = output;
    return result;
}

CliProcessResult missingProcess()
{
    CliProcessResult result;
    result.started = false;
    result.finished = false;
    result.exitCode = -1;
    result.standardError = QByteArrayLiteral("not found");
    return result;
}

bool createGpoFixture(const QString &root)
{
    const QByteArray admx(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitions xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                   revision="1.0" schemaVersion="1.0">
  <policyNamespaces><target prefix="demo" namespace="WimForge.Cli.Demo"/></policyNamespaces>
  <resources minRequiredRevision="1.0"/>
  <categories><category name="Root" displayName="$(string.Root)"/></categories>
  <policies>
    <policy name="DemoPolicy" class="Machine" displayName="$(string.DemoPolicy)"
            explainText="$(string.DemoHelp)" key="Software\Policies\WimForge\Cli"
            valueName="Enabled">
      <parentCategory ref="Root"/>
      <enabledValue><decimal value="1"/></enabledValue>
      <disabledValue><decimal value="0"/></disabledValue>
    </policy>
  </policies>
</policyDefinitions>)XML");
    const QByteArray adml(R"XML(<?xml version="1.0" encoding="utf-8"?>
<policyDefinitionResources xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                           revision="1.0" schemaVersion="1.0">
  <resources><stringTable>
    <string id="Root">WimForge CLI</string>
    <string id="DemoPolicy">CLI demo policy</string>
    <string id="DemoHelp">Searchable command-line policy documentation.</string>
  </stringTable></resources>
</policyDefinitionResources>)XML");
    return writeFile(QDir(root).filePath(QStringLiteral("Demo.admx")), admx)
        && writeFile(QDir(root).filePath(QStringLiteral("en-US/Demo.adml")), adml);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeCliRunnerTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    const auto fake = QSharedPointer<FakeProcesses>::create();
    CliDependencies dependencies;
    dependencies.processInvoker = [fake](const QString &executable,
                                         const QStringList &arguments,
                                         const QString &) {
        ++fake->calls;
        const QString lower = executable.toLower();
        if (lower.contains(QStringLiteral("node")) && arguments.contains(QStringLiteral("--version")))
            return fake->nodeInstalled ? successProcess(QByteArrayLiteral("v24")) : missingProcess();
        if (lower.contains(QStringLiteral("opencode")))
            return fake->openCodeInstalled ? successProcess(QByteArrayLiteral("1.0")) : missingProcess();
        if (lower.contains(QStringLiteral("winget"))) {
            if (!arguments.isEmpty() && arguments.first() == QStringLiteral("list"))
                return fake->nodeInstalled ? successProcess(QByteArrayLiteral("Node.js"))
                                           : missingProcess();
            ++fake->installCalls;
            fake->nodeInstalled = true;
            return successProcess();
        }
        if (lower.contains(QStringLiteral("npm"))) {
            ++fake->installCalls;
            fake->openCodeInstalled = true;
            return successProcess();
        }
        return successProcess(QByteArrayLiteral("simulated"));
    };
    CliRunner runner(dependencies);

    CliResult result = runner.run({QStringLiteral("--json"), QStringLiteral("help")});
    test.check(result.ok() && jsonEnvelope(result).value(QStringLiteral("ok")).toBool(),
               QStringLiteral("JSON help returns a successful envelope"));

    const QString projectDirectory = QDir(temporary.path()).filePath(QStringLiteral("project"));
    result = runner.run({
        QStringLiteral("--json"), QStringLiteral("project"), QStringLiteral("create"),
        projectDirectory, QStringLiteral("--name"), QStringLiteral("CLI Project"),
        QStringLiteral("--description"), QStringLiteral("Created by the command runner"),
    });
    test.check(result.ok() && QFileInfo::exists(
                   QDir(projectDirectory).filePath(QStringLiteral("project.json"))),
               QStringLiteral("project create writes a Git-backed project"));

    const QString imagePath = QDir(temporary.path()).filePath(QStringLiteral("install.wim"));
    test.check(writeFile(imagePath, QByteArrayLiteral("test image")),
               QStringLiteral("test image is created"));
    const QString mountPath = QDir(temporary.path()).filePath(QStringLiteral("mount"));
    const QString outputPath = QDir(temporary.path()).filePath(QStringLiteral("output.wim"));
    const QString registry = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("delete"), false},
        {QStringLiteral("hive"), QStringLiteral("HKLM")},
        {QStringLiteral("key"), QStringLiteral("Software\\WimForge")},
        {QStringLiteral("name"), QStringLiteral("CliFlag")},
        {QStringLiteral("type"), QStringLiteral("REG_DWORD")},
        {QStringLiteral("value"), QStringLiteral("1")},
    }).toJson(QJsonDocument::Compact));
    const QString settings = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("locale"), QStringLiteral("en-CA")},
        {QStringLiteral("telemetry"), false},
    }).toJson(QJsonDocument::Compact));
    const QString options = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("targetOnline"), true},
    }).toJson(QJsonDocument::Compact));

    result = runner.run({
        QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
        QStringLiteral("config"), QStringLiteral("edit"),
        QStringLiteral("--set"), QStringLiteral("paths.source"), jsonArgument(temporary.path()),
        QStringLiteral("--set"), QStringLiteral("/paths/image"), jsonArgument(imagePath),
        QStringLiteral("--set"), QStringLiteral("/paths/mount"), jsonArgument(mountPath),
        QStringLiteral("--set"), QStringLiteral("/paths/output"), jsonArgument(outputPath),
        QStringLiteral("--set"), QStringLiteral("/automation/autoImport"), QStringLiteral("true"),
        QStringLiteral("--add"), QStringLiteral("/features/enable"),
        jsonArgument(QStringLiteral("NetFx3")),
        QStringLiteral("--add"), QStringLiteral("/components/remove"),
        jsonArgument(QStringLiteral("WimForge-Test-Component")),
        QStringLiteral("--add"), QStringLiteral("/registry"), registry,
        QStringLiteral("--add"), QStringLiteral("/settings"), settings,
        QStringLiteral("--add"), QStringLiteral("/options"), options,
    });
    test.check(result.ok(), QStringLiteral("generic config edit covers scalars, lists, and maps: %1")
                                .arg(result.standardError));

    QString error;
    auto project = ProjectConfig::load(projectDirectory, &error);
    test.check(project && project->sourcePath == temporary.path()
                   && project->imagePath == imagePath
                   && project->featuresToEnable.contains(QStringLiteral("NetFx3"))
                   && project->componentsToRemove.contains(QStringLiteral("WimForge-Test-Component"))
                   && project->registryTweaks.size() == 1
                   && project->settings.value(QStringLiteral("locale")).toString()
                          == QStringLiteral("en-CA")
                   && project->options.extra.value(QStringLiteral("targetOnline")).toBool()
                   && project->autoImport,
               QStringLiteral("generic edits round-trip through every ProjectConfig shape: %1")
                   .arg(error));

    result = runner.run({
        QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
        QStringLiteral("config"), QStringLiteral("remove"), QStringLiteral("/features/enable"),
        jsonArgument(QStringLiteral("NetFx3")),
    });
    test.check(result.ok(), QStringLiteral("config remove deletes a matching list value"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("history"), QStringLiteral("undo")});
    project = ProjectConfig::load(projectDirectory, &error);
    test.check(result.ok() && project
                   && project->featuresToEnable.contains(QStringLiteral("NetFx3")),
               QStringLiteral("history undo creates an inverse commit and restores state"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("history"), QStringLiteral("redo")});
    project = ProjectConfig::load(projectDirectory, &error);
    test.check(result.ok() && project && project->featuresToEnable.isEmpty(),
               QStringLiteral("history redo reverts the undo commit"));

    const QString exportedProject = QDir(temporary.path()).filePath(QStringLiteral("export/project.json"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("project"), QStringLiteral("export"), exportedProject});
    test.check(result.ok() && QFileInfo::exists(exportedProject),
               QStringLiteral("project export writes portable JSON"));
    const QString importedDirectory = QDir(temporary.path()).filePath(QStringLiteral("imported"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("project"),
                         QStringLiteral("import"), exportedProject, importedDirectory});
    test.check(result.ok() && QFileInfo::exists(
                   QDir(importedDirectory).filePath(QStringLiteral(".git"))),
               QStringLiteral("project import creates its own local repository"));

    const QString responseFile = QDir(temporary.path()).filePath(QStringLiteral("invocation.json"));
    const QJsonObject responseObject{
        {QStringLiteral("arguments"), QJsonArray{
             QStringLiteral("config"), QStringLiteral("set"), QStringLiteral("/description"),
             jsonArgument(QStringLiteral("Edited through response JSON")),
         }},
        {QStringLiteral("output"), QStringLiteral("json")},
        {QStringLiteral("project"), projectDirectory},
    };
    test.check(writeFile(responseFile,
                         QJsonDocument(responseObject).toJson(QJsonDocument::Indented)),
               QStringLiteral("response JSON is created"));
    result = runner.run({QStringLiteral("@") + responseFile});
    project = ProjectConfig::load(projectDirectory, &error);
    test.check(result.ok() && project
                   && project->description == QStringLiteral("Edited through response JSON"),
               QStringLiteral("JSON response/config file expands a long invocation"));

    const int callsBeforeDryRun = fake->calls;
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("dry-run")});
    test.check(result.ok() && fake->calls == callsBeforeDryRun
                   && jsonResult(result).toObject().value(QStringLiteral("dryRun")).toBool(),
               QStringLiteral("dry-run produces a plan without executing a process: %1 %2")
                   .arg(result.standardOutput, result.standardError));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("apply")});
    test.check(result.code == CliExitCode::ConfirmationRequired,
               QStringLiteral("destructive apply is noninteractive and requires --yes: %1 %2")
                   .arg(result.standardOutput, result.standardError));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("apply"), QStringLiteral("--yes")});
    test.check(result.ok() && fake->calls > callsBeforeDryRun,
               QStringLiteral("confirmed apply uses the injected process runner: %1 %2")
                   .arg(result.standardOutput, result.standardError));

    const QString storeDirectory = QDir(temporary.path()).filePath(QStringLiteral("notifications"));
    result = runner.run({
        QStringLiteral("--json"), QStringLiteral("--store"), storeDirectory,
        QStringLiteral("notifications"), QStringLiteral("new"),
        QStringLiteral("--title"), QStringLiteral("Done"),
        QStringLiteral("--message"), QStringLiteral("CLI test finished"),
        QStringLiteral("--severity"), QStringLiteral("success"),
        QStringLiteral("--data"), QStringLiteral("{\"test\":true}"),
    });
    const QString notificationId = jsonResult(result).toObject().value(QStringLiteral("id")).toString();
    test.check(result.ok() && !notificationId.isEmpty(),
               QStringLiteral("notification new returns its stable ID"));
    for (const QString &action : {QStringLiteral("read"), QStringLiteral("dismiss"),
                                  QStringLiteral("restore"), QStringLiteral("delete")}) {
        result = runner.run({QStringLiteral("--json"), QStringLiteral("--store"), storeDirectory,
                             QStringLiteral("notifications"), action, notificationId});
        test.check(result.ok(), QStringLiteral("notification %1 is committed").arg(action));
    }
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--store"), storeDirectory,
                         QStringLiteral("notifications"), QStringLiteral("list"),
                         QStringLiteral("--all")});
    test.check(result.ok() && jsonResult(result).toArray().size() == 1
                   && jsonResult(result).toArray().first().toObject()
                          .value(QStringLiteral("softDeleted")).toBool(),
               QStringLiteral("--all includes the notification tombstone"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--store"), storeDirectory,
                         QStringLiteral("notifications"), QStringLiteral("undo")});
    test.check(result.ok(), QStringLiteral("notification history can undo the deletion"));

    const QString unattendedJson = QDir(temporary.path()).filePath(QStringLiteral("unattend/profile.json"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("unattend"),
                         QStringLiteral("template"), QStringLiteral("full"),
                         QStringLiteral("--output"), unattendedJson});
    test.check(result.ok() && QFileInfo::exists(unattendedJson),
               QStringLiteral("unattended template exports JSON"));
    const QString unattendedXml = QDir(temporary.path()).filePath(QStringLiteral("unattend/answer.xml"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("unattend"),
                         QStringLiteral("computer-name"), unattendedJson,
                         QStringLiteral("--mode"), QStringLiteral("prompt"),
                         QStringLiteral("--output"), unattendedXml});
    QFile unattendedFile(unattendedXml);
    const bool unattendedOpened = unattendedFile.open(QIODevice::ReadOnly);
    const QByteArray unattendedBytes = unattendedOpened ? unattendedFile.readAll() : QByteArray();
    test.check(result.ok() && unattendedBytes.contains("WimForge computer-name prompt"),
               QStringLiteral("prompt mode exports the WimForge prompt implementation"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("unattend"),
                         QStringLiteral("gvlk"), QStringLiteral("list"),
                         QStringLiteral("--edition"), QStringLiteral("Enterprise")});
    test.check(result.ok() && !jsonResult(result).toArray().isEmpty()
                   && jsonResult(result).toArray().first().toObject()
                          .value(QStringLiteral("licensingNotice")).toString()
                          .contains(QStringLiteral("does not grant"), Qt::CaseInsensitive),
               QStringLiteral("GVLK output includes the Microsoft licensing warning"));

    const QString packageProfile = QDir(temporary.path()).filePath(QStringLiteral("packages/ai.json"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("package"),
                         QStringLiteral("template"), QStringLiteral("ai-development"),
                         QStringLiteral("--output"), packageProfile});
    test.check(result.ok() && QFileInfo::exists(packageProfile),
               QStringLiteral("AI development package template exports"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("package"),
                         QStringLiteral("plan"), packageProfile});
    test.check(result.ok() && jsonResult(result).toArray().size() >= 10,
               QStringLiteral("package plan returns dependency-ordered common tools"));
    const QString stageDirectory = QDir(temporary.path()).filePath(QStringLiteral("iso-stage"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("package"),
                         QStringLiteral("stage"), packageProfile,
                         QStringLiteral("--directory"), stageDirectory});
    test.check(result.ok()
                   && QFileInfo::exists(QDir(stageDirectory).filePath(QStringLiteral("first-logon.ps1")))
                   && QFileInfo::exists(QDir(stageDirectory).filePath(QStringLiteral("staging-manifest.json"))),
               QStringLiteral("package stage emits ISO-ready profile, manifest, and script"));

    fake->nodeInstalled = false;
    fake->openCodeInstalled = false;
    const int installsBefore = fake->installCalls;
    result = runner.run({QStringLiteral("--json"), QStringLiteral("package"),
                         QStringLiteral("ensure-opencode"), QStringLiteral("--dry-run")});
    test.check(result.ok() && fake->installCalls == installsBefore,
               QStringLiteral("OpenCode dry-run verifies but does not install: %1 %2")
                   .arg(result.standardOutput, result.standardError));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("package"),
                         QStringLiteral("ensure-opencode")});
    test.check(result.code == CliExitCode::ConfirmationRequired,
               QStringLiteral("OpenCode install requires noninteractive confirmation"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("package"),
                         QStringLiteral("ensure-opencode"), QStringLiteral("--yes")});
    test.check(result.ok() && fake->nodeInstalled && fake->openCodeInstalled
                   && fake->installCalls == installsBefore + 2,
               QStringLiteral("OpenCode ensure installs Node dependency, installs OpenCode, and verifies both: %1 %2")
                   .arg(result.standardOutput, result.standardError));

    const QString policyDefinitions = QDir(temporary.path()).filePath(QStringLiteral("PolicyDefinitions"));
    test.check(createGpoFixture(policyDefinitions), QStringLiteral("GPO fixture is created"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("gpo"),
                         QStringLiteral("catalog"), QStringLiteral("--path"), policyDefinitions,
                         QStringLiteral("--locale"), QStringLiteral("en-US")});
    test.check(result.ok()
                   && jsonResult(result).toObject().value(QStringLiteral("policyCount")).toInt() == 1,
               QStringLiteral("GPO catalog command enumerates the supplied ADMX policy"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("gpo"),
                         QStringLiteral("search"), QStringLiteral("CLI\\s+demo"),
                         QStringLiteral("--regex"), QStringLiteral("--path"), policyDefinitions,
                         QStringLiteral("--locale"), QStringLiteral("en-US")});
    test.check(result.ok()
                   && jsonResult(result).toObject().value(QStringLiteral("policies")).toArray().size() == 1,
               QStringLiteral("GPO regex search returns structured policy data"));
    const QString gpoMarkdown = QDir(temporary.path()).filePath(QStringLiteral("gpo/catalog.md"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("gpo"),
                         QStringLiteral("export"), gpoMarkdown,
                         QStringLiteral("--path"), policyDefinitions,
                         QStringLiteral("--locale"), QStringLiteral("en-US"),
                         QStringLiteral("--primary"), QStringLiteral("en-US")});
    test.check(result.ok() && QFileInfo::exists(gpoMarkdown),
               QStringLiteral("GPO export writes Markdown documentation"));

    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("action-history"), QStringLiteral("record"),
                         QStringLiteral("--title"), QStringLiteral("CLI contextual action"),
                         QStringLiteral("--context"), QStringLiteral("tests"),
                         QStringLiteral("--element"), QStringLiteral("demo"),
                         QStringLiteral("--forward"), QStringLiteral("{\"value\":2}"),
                         QStringLiteral("--inverse"), QStringLiteral("{\"value\":1}")});
    const QString actionId = jsonResult(result).toObject().value(QStringLiteral("id")).toString();
    test.check(result.ok() && !actionId.isEmpty(),
               QStringLiteral("contextual action history records a Git-backed event"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("action-history"), QStringLiteral("undo"), actionId});
    test.check(result.ok(), QStringLiteral("CLI selectively undoes a contextual action"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("action-history"), QStringLiteral("redo"), actionId});
    test.check(result.ok(), QStringLiteral("CLI redoes an undone contextual action"));

    const QString saveBundle = QDir(temporary.path()).filePath(QStringLiteral("complete-save.wimforge"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("--project"), projectDirectory,
                         QStringLiteral("--store"), storeDirectory,
                         QStringLiteral("bundle"), QStringLiteral("export"), saveBundle});
    test.check(result.ok() && QFileInfo::exists(saveBundle),
               QStringLiteral("bundle export embeds the project and notification Git repositories"));
    const QString restoredBundle = QDir(temporary.path()).filePath(QStringLiteral("restored-save"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("bundle"),
                         QStringLiteral("import"), saveBundle, restoredBundle});
    const QJsonObject repositories = jsonResult(result).toObject()
                                         .value(QStringLiteral("repositories")).toObject();
    test.check(result.ok()
                   && QFileInfo::exists(QDir(repositories.value(QStringLiteral("project")).toString())
                                            .filePath(QStringLiteral(".git")))
                   && QFileInfo::exists(QDir(repositories.value(QStringLiteral("notifications")).toString())
                                            .filePath(QStringLiteral(".git"))),
               QStringLiteral("bundle import restores both complete local Git repositories"));

    const QString bridgeRecipe = QDir(temporary.path()).filePath(QStringLiteral("bridge/recipe.json"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("winforge"),
                         QStringLiteral("template"), QStringLiteral("page"), QStringLiteral("ai"),
                         QStringLiteral("--output"), bridgeRecipe});
    test.check(result.ok() && QFileInfo::exists(bridgeRecipe),
               QStringLiteral("WinForge CLI creates a strictly typed page recipe"));
    const QString runtimeDirectory = QDir(temporary.path()).filePath(QStringLiteral("bridge/runtime"));
    QDir().mkpath(runtimeDirectory);
    QFile runtimeExecutable(QDir(runtimeDirectory).filePath(QStringLiteral("WinForge.exe")));
    runtimeExecutable.open(QIODevice::WriteOnly);
    runtimeExecutable.write("test runtime");
    runtimeExecutable.close();
    result = runner.run({QStringLiteral("--json"), QStringLiteral("winforge"),
                         QStringLiteral("detect"), runtimeDirectory});
    test.check(result.ok()
                   && jsonResult(result).toObject().value(QStringLiteral("capabilities"))
                          .toArray().contains(QStringLiteral("launch.page.v1")),
               QStringLiteral("WinForge CLI detects the audited legacy page contract"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("winforge"),
                         QStringLiteral("validate"), bridgeRecipe,
                         QStringLiteral("--runtime"), runtimeDirectory});
    test.check(result.ok(), QStringLiteral("WinForge CLI validates a recipe against its runtime"));
    const QString bridgeStage = QDir(temporary.path()).filePath(QStringLiteral("bridge/iso"));
    result = runner.run({QStringLiteral("--json"), QStringLiteral("winforge"),
                         QStringLiteral("stage"), bridgeRecipe,
                         QStringLiteral("--iso"), bridgeStage,
                         QStringLiteral("--runtime"), runtimeDirectory});
    test.check(result.ok()
                   && QFileInfo::exists(jsonResult(result).toObject()
                                            .value(QStringLiteral("manifest")).toString()),
               QStringLiteral("WinForge CLI stages the runtime, manifest, recipe, and bootstrap"));

    return test.result();
}
