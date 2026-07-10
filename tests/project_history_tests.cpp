#include "core/ProjectConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
            QTextStream(stdout) << "project_history_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

QString makeFile(const QString &path, const QByteArray &contents = QByteArray("test"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        return {};
    return QFileInfo(file).absoluteFilePath();
}

ProjectConfig sampleProject(const QString &root)
{
    ProjectConfig project;
    project.projectDirectory = QDir(root).filePath(QStringLiteral("projects/desktop-image"));
    project.projectName = QStringLiteral("Desktop image");
    project.description = QStringLiteral("Repeatable Windows image customization");
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("inputs/windows.iso")));
    project.imagePath = makeFile(QDir(root).filePath(QStringLiteral("inputs/install.wim")));
    project.mountPath = QDir(root).filePath(QStringLiteral("work/mount"));
    project.outputPath = QDir(root).filePath(QStringLiteral("output/install-custom.wim"));
    project.selectedImageIndex = 2;
    project.outputFormat = QStringLiteral("iso");
    project.isoLabel = QStringLiteral("WIMFORGE_TEST");
    project.cloneSource = true;

    const QString driverDirectory = QDir(root).filePath(QStringLiteral("inputs/drivers"));
    QDir().mkpath(driverDirectory);
    project.drivers = {driverDirectory};
    project.updates = {makeFile(QDir(root).filePath(QStringLiteral("inputs/updates/kb.msu")))};
    project.packages = {makeFile(QDir(root).filePath(QStringLiteral("inputs/packages/language.cab")))};
    project.featuresToEnable = {QStringLiteral("NetFx3")};
    project.featuresToDisable = {QStringLiteral("Printing-XPSServices-Features")};
    project.capabilitiesToAdd = {QStringLiteral("OpenSSH.Client~~~~0.0.1.0")};
    project.capabilitiesToRemove = {QStringLiteral("MathRecognizer~~~~0.0.1.0")};
    project.appxPackagesToRemove = {QStringLiteral("Microsoft.BingNews")};
    project.appxPackagesToProvision = {
        makeFile(QDir(root).filePath(QStringLiteral("inputs/appx/Terminal.msix")))};
    project.componentsToRemove = {QStringLiteral("OneDrive")};
    project.unattendedXmlPath = makeFile(
        QDir(root).filePath(QStringLiteral("inputs/autounattend.xml")), QByteArray("<unattend/>"));
    project.unattendedFiles = {
        makeFile(QDir(root).filePath(QStringLiteral("inputs/setup/first-logon.ps1")))};
    project.postSetupItems = {QStringLiteral("powershell.exe -File first-logon.ps1")};
    project.registryTweaks = {
        RegistryTweak{QStringLiteral("HKLM"),
                      QStringLiteral("SOFTWARE\\WimForge"),
                      QStringLiteral("Configured"),
                      QStringLiteral("REG_DWORD"),
                      QStringLiteral("1"),
                      false},
    };
    project.registryTweaks[0].ownerId = QStringLiteral("gpo:WimForge.Tests::Policy:element:Number");
    RegistryTweak clearListValues;
    clearListValues.hive = QStringLiteral("HKCU");
    clearListValues.key = QStringLiteral("Software\\Policies\\WimForge\\List");
    clearListValues.deleteAllValues = true;
    clearListValues.ownerId = QStringLiteral("gpo:WimForge.Tests::Policy:element:List");
    project.registryTweaks.append(clearListValues);
    project.options.compression = QStringLiteral("recovery");
    project.options.createIso = true;
    project.options.maximumParallelOperations = 4;
    project.options.extra.insert(QStringLiteral("futureOption"), QStringLiteral("preserved"));
    project.settings.insert(QStringLiteral("language"), QStringLiteral("en-CA"));
    return project;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeProjectHistoryTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    QString gitError;
    test.check(GitHistory::gitAvailable(&gitError),
               QStringLiteral("Git is available: %1").arg(gitError));
    QString error;

    ProjectConfig project = sampleProject(temporary.path());
    const ProjectValidation valid = project.validate();
    test.check(valid.ok(), QStringLiteral("sample project validates: %1").arg(valid.message()));
    const ProjectValidation executable = project.validateForExecution();
    test.check(executable.ok(),
               QStringLiteral("sample project is ready for execution: %1").arg(executable.message()));

    ProjectConfig draft;
    draft.projectDirectory = QDir(temporary.path()).filePath(QStringLiteral("projects/draft"));
    draft.projectName = QStringLiteral("Draft without source");
    test.check(draft.validate().ok(), QStringLiteral("draft validation permits empty payload paths"));
    test.check(!draft.validateForExecution().ok(),
               QStringLiteral("execution validation still requires payload paths"));
    test.check(draft.save(&error), QStringLiteral("draft project can be saved and committed: %1").arg(error));
    std::optional<ProjectConfig> loadedDraft = ProjectConfig::load(draft.projectDirectory, &error);
    test.check(loadedDraft && loadedDraft->sourcePath.isEmpty(),
               QStringLiteral("draft project with empty paths round-trips"));

    test.check(project.save(&error), QStringLiteral("first save succeeds: %1").arg(error));
    test.check(QFileInfo::exists(project.projectFilePath()), QStringLiteral("project.json was created"));
    test.check(QFileInfo::exists(QDir(project.projectDirectory).filePath(QStringLiteral(".git"))),
               QStringLiteral("project owns a local Git repository"));

    QList<GitCommit> commits = project.history(100, &error);
    test.check(error.isEmpty(), QStringLiteral("history is readable: %1").arg(error));
    test.check(commits.size() == 1, QStringLiteral("first save created exactly one commit"));

    std::optional<ProjectConfig> loaded = ProjectConfig::load(project.projectDirectory, &error);
    test.check(loaded.has_value(), QStringLiteral("saved project loads: %1").arg(error));
    if (loaded) {
        test.check(loaded->selectedImageIndex == 2, QStringLiteral("image index round-trips"));
        test.check(loaded->outputFormat == QStringLiteral("iso")
                       && loaded->isoLabel == QStringLiteral("WIMFORGE_TEST")
                       && loaded->cloneSource,
                   QStringLiteral("output and clone-source fields round-trip"));
        test.check(loaded->drivers == project.drivers, QStringLiteral("drivers round-trip"));
        test.check(loaded->updates == project.updates, QStringLiteral("updates round-trip"));
        test.check(loaded->packages == project.packages, QStringLiteral("packages round-trip"));
        test.check(loaded->featuresToEnable == project.featuresToEnable,
                   QStringLiteral("feature selections round-trip"));
        test.check(loaded->capabilitiesToRemove == project.capabilitiesToRemove,
                   QStringLiteral("capability selections round-trip"));
        test.check(loaded->appxPackagesToProvision == project.appxPackagesToProvision,
                   QStringLiteral("Appx provisioning list round-trips"));
        test.check(loaded->registryTweaks.size() == 2
                       && loaded->registryTweaks.at(0).ownerId == project.registryTweaks.at(0).ownerId
                       && loaded->registryTweaks.at(1).deleteAllValues
                       && loaded->registryTweaks.at(1).ownerId == project.registryTweaks.at(1).ownerId,
                   QStringLiteral("registry values, safe key clearing, and ownership round-trip"));
        test.check(loaded->unattendedFiles == project.unattendedFiles
                       && loaded->postSetupItems == project.postSetupItems,
                   QStringLiteral("unattended and post-setup lists round-trip"));
        test.check(loaded->settings.value(QStringLiteral("language")).toString()
                       == QStringLiteral("en-CA"),
                   QStringLiteral("settings map round-trips"));
        test.check(loaded->options.extra.value(QStringLiteral("futureOption")).toString()
                       == QStringLiteral("preserved"),
                   QStringLiteral("unknown operation options round-trip"));
    }

    project.featuresToEnable.append(QStringLiteral("Microsoft-Hyper-V-All"));
    project.description = QStringLiteral("Second saved version");
    project.autoImport = true;
    project.autoExport = true;
    project.autoExportPath = QDir(temporary.path()).filePath(QStringLiteral("auto-export/project.json"));
    test.check(project.save(&error), QStringLiteral("second save succeeds: %1").arg(error));
    test.check(QFileInfo::exists(project.autoExportPath),
               QStringLiteral("save automatically exports portable JSON when enabled"));
    commits = project.history(100, &error);
    test.check(commits.size() == 2, QStringLiteral("every save creates a Git commit"));

    test.check(project.revertLatest(&error), QStringLiteral("revert latest succeeds: %1").arg(error));
    loaded = ProjectConfig::load(project.projectDirectory, &error);
    test.check(loaded.has_value(), QStringLiteral("project loads after undo: %1").arg(error));
    if (loaded) {
        test.check(!loaded->featuresToEnable.contains(QStringLiteral("Microsoft-Hyper-V-All")),
                   QStringLiteral("undo restored the prior project state"));
        test.check(loaded->description != QStringLiteral("Second saved version"),
                   QStringLiteral("undo restored ordinary fields"));
    }

    test.check(project.revertLatest(&error),
               QStringLiteral("reverting the revert (redo) succeeds: %1").arg(error));
    loaded = ProjectConfig::load(project.projectDirectory, &error);
    test.check(loaded.has_value(), QStringLiteral("project loads after redo: %1").arg(error));
    if (loaded) {
        test.check(loaded->featuresToEnable.contains(QStringLiteral("Microsoft-Hyper-V-All")),
                   QStringLiteral("reverting an undo reapplies the change"));
        test.check(loaded->description == QStringLiteral("Second saved version"),
                   QStringLiteral("redo restored ordinary fields"));
    }
    commits = project.history(100, &error);
    test.check(commits.size() == 4, QStringLiteral("undo and redo are both durable commits"));
    test.check(commits.at(0).isRevert() && commits.at(1).isRevert(),
               QStringLiteral("undo and redo history entries are visibly marked as reverts"));

    const QString exportPath = QDir(temporary.path()).filePath(QStringLiteral("exports/project.json"));
    const ProjectConfig exportSource = loaded.value_or(project);
    test.check(exportSource.exportJson(exportPath, &error),
               QStringLiteral("portable JSON export succeeds: %1").arg(error));
    test.check(QFileInfo::exists(exportPath), QStringLiteral("export file exists"));

    const QString importedDirectory = QDir(temporary.path()).filePath(QStringLiteral("projects/imported"));
    std::optional<ProjectConfig> imported = ProjectConfig::importJson(exportPath, importedDirectory, &error);
    test.check(imported.has_value(), QStringLiteral("JSON import succeeds: %1").arg(error));
    if (imported) {
        test.check(imported->projectDirectory == QDir(importedDirectory).absolutePath(),
                   QStringLiteral("import is rebound to its own project folder"));
        test.check(QFileInfo::exists(QDir(importedDirectory).filePath(QStringLiteral(".git"))),
                   QStringLiteral("imported project owns a separate Git repository"));
        test.check(imported->history(100, &error).size() == 1,
                   QStringLiteral("import is automatically committed"));
        test.check(imported->save(&error, QStringLiteral("Audited no-op save")),
                   QStringLiteral("byte-identical save is still committed: %1").arg(error));
        test.check(imported->history(100, &error).size() == 2,
                   QStringLiteral("no-op save has an auditable history entry"));
        test.check(imported->revertLatest(&error),
                   QStringLiteral("no-op save can still be reverted: %1").arg(error));
        test.check(imported->revertLatest(&error),
                   QStringLiteral("no-op revert can itself be reverted: %1").arg(error));
    }

    ProjectConfig invalid = project;
    invalid.projectDirectory = QStringLiteral("relative/project");
    invalid.imagePath = QDir(temporary.path()).filePath(QStringLiteral("missing/install.wim"));
    invalid.selectedImageIndex = 0;
    const ProjectValidation invalidResult = invalid.validate();
    test.check(!invalidResult.ok(), QStringLiteral("invalid draft structure is rejected"));
    test.check(invalidResult.message().contains(QStringLiteral("must be absolute")),
               QStringLiteral("relative path error is readable"));
    const ProjectValidation invalidExecution = invalid.validateForExecution();
    test.check(invalidExecution.message().contains(QStringLiteral("does not exist")),
               QStringLiteral("missing path error is readable"));
    test.check(!invalid.save(&error) && error.contains(QStringLiteral("Project cannot be saved")),
               QStringLiteral("invalid project cannot reach disk or Git"));

    return test.result();
}
