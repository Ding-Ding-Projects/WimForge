#include "core/ServicingPlan.h"
#include "core/ProcessLaunch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include <functional>

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
            QTextStream(stdout) << "servicing_plan_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

QString makeFile(const QString &path, const QByteArray &contents = QByteArray("fixture"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        return {};
    file.close();
    return QFileInfo(file).absoluteFilePath();
}

ProjectConfig baseProject(const QString &root)
{
    ProjectConfig project;
    project.projectDirectory = QDir(root).filePath(QStringLiteral("project"));
    project.projectName = QStringLiteral("Servicing safety fixture");
    project.mountPath = QDir(project.projectDirectory).filePath(QStringLiteral("mount"));
    project.outputPath = QDir(project.projectDirectory).filePath(QStringLiteral("output/custom.wim"));
    project.outputFormat = QStringLiteral("wim");
    project.selectedImageIndex = 2;
    project.cloneSource = true;
    project.options.verifyPayloads = true;
    project.options.cleanupComponentStore = false;
    return project;
}

const ServicingOperation *findOperation(
    const ServicingPlanResult &plan,
    const std::function<bool(const ServicingOperation &)> &predicate)
{
    for (const ServicingOperation &operation : plan.operations) {
        if (predicate(operation))
            return &operation;
    }
    return nullptr;
}

QList<const ServicingOperation *> operationsOfKind(const ServicingPlanResult &plan, OperationKind kind)
{
    QList<const ServicingOperation *> result;
    for (const ServicingOperation &operation : plan.operations) {
        if (operation.kind == kind)
            result.append(&operation);
    }
    return result;
}

bool hasArgument(const ServicingOperation &operation, const QString &prefix, const QString &value)
{
    const QString expected = prefix + value;
    return std::any_of(operation.arguments.cbegin(), operation.arguments.cend(),
                       [&expected](const QString &argument) {
                           return argument.compare(expected, Qt::CaseInsensitive) == 0;
                       });
}

bool isAncestor(const ServicingPlanResult &plan, const QString &ancestor, const QString &descendant)
{
    QSet<QString> visited;
    QStringList pending{descendant};
    while (!pending.isEmpty()) {
        const QString current = pending.takeLast();
        if (current == ancestor)
            return true;
        if (visited.contains(current))
            continue;
        visited.insert(current);
        const ServicingOperation *operation = findOperation(
            plan, [&current](const ServicingOperation &candidate) { return candidate.id == current; });
        if (operation)
            pending.append(operation->dependsOn);
    }
    return false;
}

bool runOperation(const ServicingOperation &operation,
                  QString *detail = nullptr,
                  const QProcessEnvironment *environment = nullptr)
{
    QProcess process;
    process.setProgram(resolveExecutableForLaunch(operation.executable));
    process.setArguments(operation.arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    if (environment)
        process.setProcessEnvironment(*environment);
    process.start();
    if (!process.waitForStarted(10'000) || !process.waitForFinished(60'000)) {
        if (detail)
            *detail = process.errorString();
        process.kill();
        process.waitForFinished();
        return false;
    }
    if (detail) {
        const QByteArray output = process.readAllStandardOutput() + process.readAllStandardError();
        *detail = QStringLiteral("exit=%1; %2")
                      .arg(process.exitCode())
                      .arg(QString::fromLocal8Bit(output));
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void testIsoFolder(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    const QString media = QDir(root).filePath(QStringLiteral("input/media"));
    makeFile(QDir(media).filePath(QStringLiteral("boot/etfsboot.com")));
    makeFile(QDir(media).filePath(QStringLiteral("efi/microsoft/boot/efisys.bin")));
    project.imagePath = makeFile(QDir(media).filePath(QStringLiteral("sources/install.wim")));
    project.sourcePath = media;
    project.outputFormat = QStringLiteral("iso");
    project.outputPath = QDir(project.projectDirectory).filePath(QStringLiteral("output/custom.iso"));

    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("ISO-folder plan is valid: %1").arg(plan.errors.join(QStringLiteral(" | "))));
    const QString expectedWorking = QDir(plan.mediaWorkspace).filePath(QStringLiteral("sources/install.wim"));
    test.check(QDir::cleanPath(plan.workingImagePath) == QDir::cleanPath(expectedWorking),
               QStringLiteral("ISO-folder working image stays inside cloned media"));

    const ServicingOperation *mediaClone = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::PrepareWorkspace
            && operation.writesMediaWorkspace
            && operation.metadata.value(QStringLiteral("sourceImmutable")).toBool();
    });
    const QString cloneScript = mediaClone ? mediaClone->arguments.constLast() : QString();
    test.check(mediaClone && mediaClone->executable == QStringLiteral("powershell.exe")
                   && cloneScript.contains(QStringLiteral("$wimforgeRobocopy"))
                   && cloneScript.contains(QStringLiteral("[Environment]::SystemDirectory"))
                   && cloneScript.contains(QStringLiteral("$env:PSModulePath"))
                   && !cloneScript.contains(QStringLiteral("& robocopy.exe"))
                   && mediaClone->arguments.join(QLatin1Char(' ')).contains(QStringLiteral("/XJ")),
                QStringLiteral("media folder is recursively cloned without following junctions"));
    if (mediaClone) {
        const QString hostileBin = QDir(root).filePath(QStringLiteral("hostile-bin"));
        makeFile(QDir(hostileBin).filePath(QStringLiteral("robocopy.exe")),
                 QByteArray("not a trusted executable"));
        QProcessEnvironment hostile = QProcessEnvironment::systemEnvironment();
        hostile.insert(QStringLiteral("PATH"), hostileBin);
        hostile.insert(QStringLiteral("PSModulePath"),
                       QDir(root).filePath(QStringLiteral("hostile-modules")));
        QString detail;
        test.check(runOperation(*mediaClone, &detail, &hostile)
                       && QFileInfo::exists(QDir(plan.mediaWorkspace).filePath(
                              QStringLiteral("sources/install.wim"))),
                   QStringLiteral("media cloning ignores hostile PATH/PSModulePath: %1")
                       .arg(detail));
    }

    const ServicingOperation *inspect = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Inspect;
    });
    const ServicingOperation *mount = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Mount;
    });
    test.check(inspect && hasArgument(*inspect, QStringLiteral("/WimFile:"), expectedWorking),
               QStringLiteral("inspect uses working media image"));
    test.check(mount && hasArgument(*mount, QStringLiteral("/ImageFile:"), expectedWorking),
               QStringLiteral("mount uses working media image"));

    const ServicingOperation *iso = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::CreateIso
            && operation.executable.compare(QStringLiteral("oscdimg.exe"), Qt::CaseInsensitive) == 0;
    });
    test.check(iso != nullptr, QStringLiteral("ISO-folder plan includes direct oscdimg operation"));
    if (iso) {
        for (const ServicingOperation &operation : plan.operations) {
            if (!operation.writesMountedImage && !operation.writesMediaWorkspace)
                continue;
            test.check(iso->dependsOn.contains(operation.id),
                       QStringLiteral("ISO has direct dependency on write %1").arg(operation.id));
        }
        test.check(iso->arguments.constLast().endsWith(QStringLiteral(".iso"), Qt::CaseInsensitive)
                       && iso->arguments.constLast() != project.outputPath,
                   QStringLiteral("oscdimg writes a partial ISO instead of exposing a partial final file"));
    }
}

void testIsoFile(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/windows.iso")));
    project.imagePath = makeFile(QDir(root).filePath(QStringLiteral("input/extracted/install.wim")));
    project.outputFormat = QStringLiteral("iso");
    project.outputPath = QDir(project.projectDirectory).filePath(QStringLiteral("output/windows-custom.iso"));

    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("ISO-file plan is valid: %1").arg(plan.errors.join(QStringLiteral(" | "))));
    const ServicingOperation *extract = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::PrepareWorkspace
            && operation.writesMediaWorkspace
            && operation.arguments.join(QLatin1Char(' ')).contains(QStringLiteral("Mount-DiskImage"));
    });
    const QString extractScript = extract ? extract->arguments.constLast() : QString();
    test.check(extract && extractScript.contains(QStringLiteral("finally"))
                   && extractScript.contains(QStringLiteral("Microsoft.PowerShell.Core\\Import-Module"))
                   && extractScript.contains(QStringLiteral("Storage\\Mount-DiskImage"))
                   && extractScript.contains(QStringLiteral("Storage\\Dismount-DiskImage"))
                   && extractScript.contains(QStringLiteral("Storage\\Get-DiskImage"))
                   && extractScript.contains(QStringLiteral("$wimforgeRobocopy"))
                   && !extractScript.contains(QStringLiteral("& robocopy.exe")),
                QStringLiteral("ISO extraction pins Storage/native tools and confirms dismount"));
    const ServicingOperation *imageCopy = findOperation(plan, [&project](const ServicingOperation &operation) {
        return operation.kind == OperationKind::PrepareWorkspace
            && operation.metadata.value(QStringLiteral("source")).toString() == project.imagePath;
    });
    test.check(imageCopy && imageCopy->dependsOn.contains(extract ? extract->id : QString()),
               QStringLiteral("external image is copied into media after ISO extraction"));
}

void testIsoContainedEsd(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/windows.iso")));
    project.imagePath.clear();
    project.options.extra.insert(QStringLiteral("imageRelativePath"),
                                 QStringLiteral("sources/install.esd"));
    project.outputFormat = QStringLiteral("iso");
    project.outputPath = QDir(project.projectDirectory)
                             .filePath(QStringLiteral("output/windows-custom.iso"));

    const ProjectValidation executionValidation = project.validateForExecution();
    test.check(executionValidation.ok(),
               QStringLiteral("an inspected ISO-contained image validates without a stale mounted path: %1")
                   .arg(executionValidation.errors.join(QStringLiteral(" | "))));

    const ServicingPlanResult plan = ServicingPlan::build(project);
    const QString expectedMediaImage = QDir(plan.mediaWorkspace)
                                           .filePath(QStringLiteral("sources/install.esd"));
    const QString expectedWorking = QDir(project.projectDirectory)
                                        .filePath(QStringLiteral(".wimforge/work/images/install-working.wim"));
    const QString expectedPartialWorking = QDir(QFileInfo(expectedWorking).absolutePath())
                                               .filePath(QStringLiteral(".install-working.wimforge-partial.wim"));
    const QString expectedPartialEsd = QDir(QFileInfo(expectedMediaImage).absolutePath())
                                           .filePath(QStringLiteral(".install.wimforge-partial.esd"));
    test.check(plan.ok(),
               QStringLiteral("ISO-contained ESD plan is valid: %1")
                   .arg(plan.errors.join(QStringLiteral(" | "))));
    test.check(QDir::cleanPath(plan.workingImagePath) == QDir::cleanPath(expectedWorking),
               QStringLiteral("ISO-contained ESD is converted to a project-owned working WIM"));
    const ServicingOperation *extract = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::PrepareWorkspace
            && operation.writesMediaWorkspace
            && operation.arguments.join(QLatin1Char(' ')).contains(QStringLiteral("Mount-DiskImage"));
    });
    const ServicingOperation *convert = findOperation(
        plan, [&expectedMediaImage, &expectedPartialWorking](const ServicingOperation &operation) {
            return operation.kind == OperationKind::PrepareWorkspace
                && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
                && hasArgument(operation, QStringLiteral("/SourceImageFile:"), expectedMediaImage)
                && hasArgument(operation, QStringLiteral("/SourceIndex:"), QStringLiteral("2"))
                && hasArgument(operation, QStringLiteral("/DestinationImageFile:"), expectedPartialWorking)
                && operation.arguments.contains(QStringLiteral("/Compress:max"))
                && !std::any_of(operation.arguments.cbegin(), operation.arguments.cend(),
                                [](const QString &argument) {
                                    return argument.startsWith(QStringLiteral("/SWMFile:"),
                                                               Qt::CaseInsensitive);
                                });
        });
    test.check(extract && convert && isAncestor(plan, extract->id, convert->id),
               QStringLiteral("ESD conversion waits for ISO extraction and exports selected source index 2"));
    const ServicingOperation *inspect = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Inspect;
    });
    test.check(inspect && hasArgument(*inspect, QStringLiteral("/WimFile:"), expectedWorking),
               QStringLiteral("post-conversion inspection reads the serviceable working WIM"));
    const ServicingOperation *mount = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Mount;
    });
    test.check(mount && hasArgument(*mount, QStringLiteral("/ImageFile:"), expectedWorking)
                   && hasArgument(*mount, QStringLiteral("/Index:"), QStringLiteral("1"))
                   && !hasArgument(*mount, QStringLiteral("/ImageFile:"), expectedMediaImage),
               QStringLiteral("mount uses converted WIM index 1 and never attempts to mount install.esd"));

    const ServicingOperation *repack = findOperation(
        plan, [&expectedWorking, &expectedPartialEsd](const ServicingOperation &operation) {
            return operation.kind == OperationKind::Export
                && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
                && operation.writesMediaWorkspace
                && hasArgument(operation, QStringLiteral("/SourceImageFile:"), expectedWorking)
                && hasArgument(operation, QStringLiteral("/SourceIndex:"), QStringLiteral("1"))
                && hasArgument(operation, QStringLiteral("/DestinationImageFile:"), expectedPartialEsd)
                && operation.arguments.contains(QStringLiteral("/Compress:recovery"));
        });
    const ServicingOperation *publishMediaImage = findOperation(
        plan, [&expectedMediaImage](const ServicingOperation &operation) {
            return operation.kind == OperationKind::Export
                && operation.writesMediaWorkspace
                && operation.metadata.value(QStringLiteral("destination")).toString()
                       == expectedMediaImage;
        });
    const ServicingOperation *iso = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::CreateIso
            && operation.executable.compare(QStringLiteral("oscdimg.exe"), Qt::CaseInsensitive) == 0;
    });
    test.check(repack && publishMediaImage
                   && isAncestor(plan, repack->id, publishMediaImage->id)
                   && iso && isAncestor(plan, publishMediaImage->id, iso->id),
               QStringLiteral("serviced WIM index 1 is recovery-compressed back to install.esd before ISO creation"));

    ProjectConfig unsafe = project;
    unsafe.options.extra.insert(QStringLiteral("imageRelativePath"),
                                QStringLiteral("../outside/install.wim"));
    test.check(!unsafe.validate().ok() && !ServicingPlan::build(unsafe).ok(),
               QStringLiteral("unsafe ISO-relative image traversal is rejected"));
}

void testIsoContainedSwm(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/windows.iso")));
    project.imagePath.clear();
    project.options.extra.insert(QStringLiteral("imageRelativePath"),
                                 QStringLiteral("sources/install.swm"));
    project.outputFormat = QStringLiteral("iso");
    project.outputPath = QDir(project.projectDirectory)
                             .filePath(QStringLiteral("output/windows-custom.iso"));

    const ServicingPlanResult plan = ServicingPlan::build(project);
    const QString expectedMediaImage = QDir(plan.mediaWorkspace)
                                           .filePath(QStringLiteral("sources/install.swm"));
    const QString expectedWorking = QDir(project.projectDirectory)
                                        .filePath(QStringLiteral(".wimforge/work/images/install-working.wim"));
    const QString expectedPartialWorking = QDir(QFileInfo(expectedWorking).absolutePath())
                                               .filePath(QStringLiteral(".install-working.wimforge-partial.wim"));
    const QString expectedWildcard = QDir(QFileInfo(expectedMediaImage).absolutePath())
                                         .filePath(QStringLiteral("install*.swm"));
    test.check(plan.ok(), QStringLiteral("ISO-contained SWM plan is valid: %1")
                                  .arg(plan.errors.join(QStringLiteral(" | "))));
    test.check(QDir::cleanPath(plan.workingImagePath) == QDir::cleanPath(expectedWorking),
               QStringLiteral("imageRelativePath .swm selects the split-to-WIM preparation path"));
    const ServicingOperation *convert = findOperation(
        plan, [&expectedMediaImage, &expectedPartialWorking,
               &expectedWildcard](const ServicingOperation &operation) {
            return operation.kind == OperationKind::PrepareWorkspace
                && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
                && hasArgument(operation, QStringLiteral("/SourceImageFile:"), expectedMediaImage)
                && hasArgument(operation, QStringLiteral("/SWMFile:"), expectedWildcard)
                && hasArgument(operation, QStringLiteral("/SourceIndex:"), QStringLiteral("2"))
                && hasArgument(operation, QStringLiteral("/DestinationImageFile:"), expectedPartialWorking);
        });
    const ServicingOperation *mount = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Mount;
    });
    test.check(convert && mount
                   && hasArgument(*mount, QStringLiteral("/ImageFile:"), expectedWorking)
                   && hasArgument(*mount, QStringLiteral("/Index:"), QStringLiteral("1")),
               QStringLiteral("complete SWM set index 2 is converted and working WIM index 1 is mounted"));

    const ServicingOperation *split = findOperation(
        plan, [&expectedWorking](const ServicingOperation &operation) {
            return operation.kind == OperationKind::Split
                && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
                && hasArgument(operation, QStringLiteral("/ImageFile:"), expectedWorking);
        });
    const ServicingOperation *publishMediaImage = findOperation(
        plan, [&expectedMediaImage](const ServicingOperation &operation) {
            return operation.kind == OperationKind::Split
                && operation.writesMediaWorkspace
                && operation.metadata.value(QStringLiteral("destination")).toString()
                       == expectedMediaImage;
        });
    const ServicingOperation *iso = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::CreateIso
            && operation.executable.compare(QStringLiteral("oscdimg.exe"), Qt::CaseInsensitive) == 0;
    });
    test.check(split && publishMediaImage
                   && isAncestor(plan, split->id, publishMediaImage->id)
                   && iso && isAncestor(plan, publishMediaImage->id, iso->id),
               QStringLiteral("serviced WIM is split back into install*.swm before ISO creation"));
}

void testContainerSources(TestRun &test, const QString &root)
{
    for (const QString &suffix : {QStringLiteral("wim"), QStringLiteral("esd")}) {
        ProjectConfig project = baseProject(QDir(root).filePath(suffix));
        project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/install.%1").arg(suffix)));
        project.imagePath = project.sourcePath;
        project.outputFormat = suffix;
        project.outputPath = QDir(project.projectDirectory)
                                 .filePath(QStringLiteral("output/custom.%1").arg(suffix));
        const ServicingPlanResult plan = ServicingPlan::build(project);
        test.check(plan.ok(), QStringLiteral("%1 plan is valid: %2")
                                  .arg(suffix.toUpper(), plan.errors.join(QStringLiteral(" | "))));
        test.check(plan.workingImagePath != project.imagePath
                       && plan.workingImagePath.endsWith(QStringLiteral(".wim"), Qt::CaseInsensitive),
                   suffix == QStringLiteral("esd")
                       ? QStringLiteral("ESD source is converted to a serviceable working WIM")
                       : QStringLiteral("WIM source is cloned to a separate working WIM"));
        const ServicingOperation *conversion = findOperation(
            plan, [&project](const ServicingOperation &operation) {
                return operation.kind == OperationKind::PrepareWorkspace
                    && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
                    && hasArgument(operation, QStringLiteral("/SourceImageFile:"), project.imagePath);
            });
        test.check(suffix == QStringLiteral("esd")
                       ? conversion && hasArgument(*conversion, QStringLiteral("/SourceIndex:"),
                                                   QStringLiteral("2"))
                       : conversion == nullptr,
                   QStringLiteral("only ESD input has a selected-index conversion barrier"));
        const ServicingOperation *mount = findOperation(plan, [](const ServicingOperation &operation) {
            return operation.kind == OperationKind::Mount;
        });
        const ServicingOperation *exportImage = findOperation(plan, [](const ServicingOperation &operation) {
            return operation.kind == OperationKind::Export
                && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0;
        });
        const QString expectedWorkingIndex = suffix == QStringLiteral("esd")
            ? QStringLiteral("1") : QStringLiteral("2");
        test.check(mount && hasArgument(*mount, QStringLiteral("/ImageFile:"), plan.workingImagePath)
                       && hasArgument(*mount, QStringLiteral("/Index:"), expectedWorkingIndex),
                   QStringLiteral("%1 mount uses the correct working-image index").arg(suffix.toUpper()));
        test.check(exportImage
                       && hasArgument(*exportImage, QStringLiteral("/SourceImageFile:"), plan.workingImagePath)
                       && hasArgument(*exportImage, QStringLiteral("/SourceIndex:"), expectedWorkingIndex),
                   QStringLiteral("%1 output export reads the correct working-image index").arg(suffix.toUpper()));
    }
}

void testSplitSource(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/install.swm")));
    project.imagePath = project.sourcePath;
    makeFile(QDir(root).filePath(QStringLiteral("input/install2.swm")));
    project.outputFormat = QStringLiteral("swm");
    project.outputPath = QDir(project.projectDirectory).filePath(QStringLiteral("output/install.swm"));
    project.options.splitSizeMb = 2048;

    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("SWM plan is valid: %1").arg(plan.errors.join(QStringLiteral(" | "))));
    test.check(plan.workingImagePath.endsWith(QStringLiteral(".wim"), Qt::CaseInsensitive)
                   && plan.workingImagePath != project.imagePath,
               QStringLiteral("SWM set is converted into a serviceable working WIM"));
    const ServicingOperation *convert = findOperation(plan, [&project](const ServicingOperation &operation) {
        return operation.kind == OperationKind::PrepareWorkspace
            && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
            && hasArgument(operation, QStringLiteral("/SourceImageFile:"), project.imagePath);
    });
    test.check(convert && hasArgument(*convert, QStringLiteral("/SourceIndex:"), QStringLiteral("2"))
                   && std::any_of(convert->arguments.cbegin(), convert->arguments.cend(),
                                  [](const QString &argument) {
                                      return argument.startsWith(QStringLiteral("/SWMFile:"))
                                          && argument.contains(QStringLiteral("*.swm"));
                                  }),
               QStringLiteral("SWM preparation reads the complete split set"));
    const ServicingOperation *mount = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Mount;
    });
    test.check(mount && hasArgument(*mount, QStringLiteral("/ImageFile:"), plan.workingImagePath)
                   && hasArgument(*mount, QStringLiteral("/Index:"), QStringLiteral("1")),
               QStringLiteral("converted SWM working WIM is mounted at its sole index 1"));
    const ServicingOperation *split = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Split
            && operation.executable.compare(QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0;
    });
    test.check(split && hasArgument(*split, QStringLiteral("/ImageFile:"), plan.workingImagePath),
               QStringLiteral("SWM output is split from working WIM, not source SWM"));
    test.check(split && split->arguments.contains(QStringLiteral("/FileSize:2048")),
               QStringLiteral("typed split-size setting controls DISM output"));
}

void testStagingAndHashGate(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/install.wim")));
    project.imagePath = project.sourcePath;
    const QString package = makeFile(QDir(root).filePath(QStringLiteral("payload/update.cab")));
    const QString answer = makeFile(QDir(root).filePath(QStringLiteral("payload/autounattend.xml")),
                                    QByteArray("<unattend/>"));
    const QString runtime = QDir(root).filePath(QStringLiteral("payload/runtime"));
    makeFile(QDir(runtime).filePath(QStringLiteral("bin/runtime.exe")));
    const QString readme = makeFile(QDir(root).filePath(QStringLiteral("payload/readme.txt")));
    project.packages = {package};
    project.unattendedXmlPath = answer;
    project.unattendedFiles = {readme};
    project.options.extra.insert(QStringLiteral("stagedFiles"), QJsonArray{
        QJsonObject{{QStringLiteral("source"), runtime},
                    {QStringLiteral("destination"), QStringLiteral("ProgramData/WimForge/runtime")},
                    {QStringLiteral("scope"), QStringLiteral("image")},
                    {QStringLiteral("role"), QStringLiteral("winforge-runtime")}},
        QJsonObject{{QStringLiteral("source"), answer},
                    {QStringLiteral("destination"), QStringLiteral("autounattend.xml")},
                    {QStringLiteral("scope"), QStringLiteral("media")},
                    {QStringLiteral("role"), QStringLiteral("unattended-answer")}},
    });

    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("staging plan is valid: %1").arg(plan.errors.join(QStringLiteral(" | "))));
    const QList<const ServicingOperation *> stages = operationsOfKind(plan, OperationKind::StageFile);
    test.check(stages.size() == 3, QStringLiteral("explicit image/media files and unattended file are staged"));
    const ServicingOperation *runtimeStage = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::StageFile
            && operation.metadata.value(QStringLiteral("role")).toString()
                   == QStringLiteral("winforge-runtime");
    });
    test.check(runtimeStage
                   && runtimeStage->metadata.value(QStringLiteral("recursive")).toBool()
                   && runtimeStage->metadata.value(QStringLiteral("destination")).toString()
                          .endsWith(QStringLiteral("ProgramData/WimForge/runtime"))
                   && runtimeStage->arguments.join(QLatin1Char(' ')).contains(QStringLiteral("Get-ChildItem")),
               QStringLiteral("recursive runtime directory is copied into exact safe image destination"));
    if (runtimeStage) {
        QString detail;
        test.check(runOperation(*runtimeStage, &detail),
                   QStringLiteral("recursive stage PowerShell executes: %1").arg(detail));
        test.check(QFileInfo::exists(QDir(project.mountPath).filePath(
                       QStringLiteral("ProgramData/WimForge/runtime/bin/runtime.exe"))),
                   QStringLiteral("recursive stage preserves runtime directory contents"));
    }
    const ServicingOperation *mediaStage = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::StageFile && operation.writesMediaWorkspace;
    });
    test.check(mediaStage
                   && mediaStage->metadata.value(QStringLiteral("destination")).toString()
                          == QDir(plan.mediaWorkspace).filePath(QStringLiteral("autounattend.xml")),
               QStringLiteral("media stage targets cloned media root"));

    const QList<const ServicingOperation *> hashes = operationsOfKind(plan, OperationKind::VerifyPayload);
    test.check(hashes.size() >= 5, QStringLiteral("source, package, unattended, and staged inputs are hashed"));
    const ServicingOperation *runtimeHash = findOperation(plan, [&runtime](const ServicingOperation &operation) {
        return operation.kind == OperationKind::VerifyPayload
            && operation.metadata.value(QStringLiteral("verifiedPath")).toString() == runtime;
    });
    if (runtimeHash) {
        QString detail;
        const bool succeeded = runOperation(*runtimeHash, &detail);
        test.check(succeeded && detail.contains(QStringLiteral("SHA256")),
                   QStringLiteral("recursive directory hash program executes: %1\n%2")
                       .arg(detail, runtimeHash->arguments.constLast()));
    } else {
        test.check(false, QStringLiteral("runtime directory receives a hash operation"));
    }
    for (const ServicingOperation &operation : plan.operations) {
        if (!operation.writesMountedImage && !operation.writesMediaWorkspace)
            continue;
        for (const ServicingOperation *hash : hashes) {
            test.check(isAncestor(plan, hash->id, operation.id),
                       QStringLiteral("hash %1 gates write %2").arg(hash->id, operation.id));
        }
    }
    const ServicingOperation *packageOperation = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Package;
    });
    const ServicingOperation *unattendOperation = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Unattended;
    });
    test.check(packageOperation != nullptr && unattendOperation != nullptr,
               QStringLiteral("package integration and DISM unattended application are preserved"));
}

void testSourceImmutability(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/pristine.wim")));
    project.imagePath = project.sourcePath;
    project.featuresToEnable = {QStringLiteral("NetFx3")};
    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("immutability plan is valid"));
    test.check(plan.workingImagePath != project.sourcePath,
               QStringLiteral("default working image differs from source"));
    for (const ServicingOperation &operation : plan.operations) {
        if (operation.kind == OperationKind::VerifyPayload
            || operation.kind == OperationKind::PrepareWorkspace)
            continue;
        test.check(!hasArgument(operation, QStringLiteral("/ImageFile:"), project.sourcePath)
                       && !hasArgument(operation, QStringLiteral("/WimFile:"), project.sourcePath)
                       && !hasArgument(operation, QStringLiteral("/SourceImageFile:"), project.sourcePath),
                   QStringLiteral("operation %1 never services or exports from pristine source").arg(operation.id));
    }

    ProjectConfig unsafe = project;
    unsafe.cloneSource = false;
    const ServicingPlanResult refused = ServicingPlan::build(unsafe);
    test.check(!refused.ok()
                   && refused.errors.join(QLatin1Char(' ')).contains(
                       QStringLiteral("allowInPlaceSourceModification")),
               QStringLiteral("disabling clone requires explicit dangerous opt-in"));
}

void testUnsafeStagedPaths(TestRun &test, const QString &root)
{
    const QString source = makeFile(QDir(root).filePath(QStringLiteral("input/install.wim")));
    const QString payload = makeFile(QDir(root).filePath(QStringLiteral("payload/file.txt")));
    const QList<QJsonObject> unsafeObjects{
        QJsonObject{{QStringLiteral("source"), payload},
                    {QStringLiteral("destination"), QStringLiteral("../escape.txt")},
                    {QStringLiteral("scope"), QStringLiteral("image")},
                    {QStringLiteral("role"), QStringLiteral("test")}},
        QJsonObject{{QStringLiteral("source"), payload},
                    {QStringLiteral("destination"), QStringLiteral("C:/Windows/escape.txt")},
                    {QStringLiteral("scope"), QStringLiteral("image")},
                    {QStringLiteral("role"), QStringLiteral("test")}},
        QJsonObject{{QStringLiteral("source"), payload},
                    {QStringLiteral("destination"), QStringLiteral("CON/file.txt")},
                    {QStringLiteral("scope"), QStringLiteral("media")},
                    {QStringLiteral("role"), QStringLiteral("test")}},
        QJsonObject{{QStringLiteral("source"), payload},
                    {QStringLiteral("destination"), QStringLiteral("safe/file.txt")},
                    {QStringLiteral("scope"), QStringLiteral("host")},
                    {QStringLiteral("role"), QStringLiteral("test")}},
        QJsonObject{{QStringLiteral("source"), QDir(root).filePath(QStringLiteral("missing.bin"))},
                    {QStringLiteral("destination"), QStringLiteral("safe/file.txt")},
                    {QStringLiteral("scope"), QStringLiteral("image")},
                    {QStringLiteral("role"), QStringLiteral("test")}},
    };

    for (qsizetype index = 0; index < unsafeObjects.size(); ++index) {
        ProjectConfig project = baseProject(QDir(root).filePath(QStringLiteral("unsafe-%1").arg(index)));
        project.sourcePath = source;
        project.imagePath = source;
        project.options.extra.insert(QStringLiteral("stagedFiles"), QJsonArray{unsafeObjects.at(index)});
        const ServicingPlanResult plan = ServicingPlan::build(project);
        test.check(!plan.ok(), QStringLiteral("unsafe staged object %1 is rejected").arg(index));
    }

    ProjectConfig missing = baseProject(QDir(root).filePath(QStringLiteral("missing-source")));
    missing.sourcePath = QDir(root).filePath(QStringLiteral("does-not-exist.wim"));
    missing.imagePath = missing.sourcePath;
    const ServicingPlanResult missingPlan = ServicingPlan::build(missing);
    test.check(!missingPlan.ok()
                   && missingPlan.errors.join(QLatin1Char(' ')).contains(QStringLiteral("does not exist")),
               QStringLiteral("missing source and image are rejected before a plan is emitted"));
}

void testQuotingAndExport(TestRun &test, const QString &root)
{
    test.check(ServicingPlan::quotePowerShellLiteral(QStringLiteral("O'Brien"))
                   == QStringLiteral("'O''Brien'"),
               QStringLiteral("PowerShell single-quoted literals escape apostrophes"));
    test.check(ServicingPlan::quoteWindowsArgument(QStringLiteral("C:\\plain\\file.wim"))
                   == QStringLiteral("C:\\plain\\file.wim"),
               QStringLiteral("direct argument preview does not add shell quoting unnecessarily"));
    test.check(ServicingPlan::quoteWindowsArgument(QStringLiteral("C:\\space dir\\file.wim"))
                   == QStringLiteral("\"C:\\space dir\\file.wim\""),
               QStringLiteral("argument preview quotes whitespace"));

    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/O'Brien/install.wim")));
    project.imagePath = project.sourcePath;
    const ServicingPlanResult plan = ServicingPlan::build(project);
    const QString scriptPath = QDir(root).filePath(QStringLiteral("output/plan.ps1"));
    QString error;
    test.check(plan.ok() && ServicingPlan::exportPowerShell(project, plan.operations, scriptPath, &error),
               QStringLiteral("PowerShell plan exports: %1").arg(error));
    QFile script(scriptPath);
    const bool opened = script.open(QIODevice::ReadOnly);
    const QByteArray contents = opened ? script.readAll() : QByteArray();
    // The already-safe embedded PowerShell program is itself emitted as a
    // single-quoted argument, so its apostrophes are escaped a second time.
    test.check(opened && contents.contains("O''''Brien")
                   && contents.contains("Invoke-WimForgeStep")
                   && contents.contains("Resolve-WimForgeExecutable")
                   && contents.contains("[Environment]::SystemDirectory")
                   && contents.contains("ProgramFilesX86")
                   && contents.contains("Assessment and Deployment Kit")
                   && contents.contains("& $resolvedExe @Args")
                   && !contents.contains("& $Exe @Args"),
                QStringLiteral("export uses literal argument arrays rather than a concatenated command line"));
    if (opened) {
        QProcess exported;
        exported.setProgram(resolveExecutableForLaunch(QStringLiteral("powershell.exe")));
        exported.setArguments({QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
                               QStringLiteral("-NonInteractive"), QStringLiteral("-File"),
                               scriptPath, QStringLiteral("-WhatIfPlan")});
        exported.setProcessEnvironment(sanitizedPowerShellEnvironment());
        exported.setProcessChannelMode(QProcess::MergedChannels);
        exported.start();
        const bool started = exported.waitForStarted(10'000);
        const bool finished = started && exported.waitForFinished(60'000);
        const QString output = QString::fromLocal8Bit(exported.readAll());
        test.check(started && finished && exported.exitStatus() == QProcess::NormalExit
                       && exported.exitCode() == 0,
                   QStringLiteral("exported hardened plan parses and resolves trusted tools: %1")
                       .arg(output));
    }

    ServicingOperation publish;
    publish.id = QStringLiteral("publish");
    publish.titleEn = QStringLiteral("Publish output");
    publish.titleZh = QStringLiteral("Publish output");
    publish.executable = QStringLiteral("publish.exe");
    publish.arguments = {QStringLiteral("--publish")};
    publish.dependsOn = {QStringLiteral("optional")};
    publish.requiresAdministrator = true;
    publish.writesMountedImage = true;

    ServicingOperation optional;
    optional.id = QStringLiteral("optional");
    optional.titleEn = QStringLiteral("Optional sentinel");
    optional.titleZh = QStringLiteral("Optional sentinel");
    optional.executable = QStringLiteral("optional.exe");
    optional.dependsOn = {QStringLiteral("prepare")};
    optional.state = OperationState::Skipped;

    ServicingOperation prepare;
    prepare.id = QStringLiteral("prepare");
    prepare.titleEn = QStringLiteral("Prepare workspace");
    prepare.titleZh = QStringLiteral("Prepare workspace");
    prepare.executable = QStringLiteral("prepare.exe");
    prepare.arguments = {QStringLiteral("--prepare")};
    prepare.destructive = true;
    prepare.checkpointBefore = true;
    prepare.writesMediaWorkspace = true;
    prepare.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("media")},
                                   {QStringLiteral("crashSafe"), true}};

    const QString orderedPath = QDir(root).filePath(QStringLiteral("output/ordered.ps1"));
    error.clear();
    test.check(ServicingPlan::exportPowerShell(
                   project, {publish, optional, prepare}, orderedPath, &error),
               QStringLiteral("out-of-order acyclic graph exports: %1").arg(error));
    QFile orderedScript(orderedPath);
    const bool orderedOpened = orderedScript.open(QIODevice::ReadOnly);
    const QByteArray orderedContents = orderedOpened ? orderedScript.readAll() : QByteArray{};
    const qsizetype preparePosition = orderedContents.indexOf("Invoke-WimForgeStep 'prepare'");
    const qsizetype publishPosition = orderedContents.indexOf("Invoke-WimForgeStep 'publish'");
    test.check(orderedOpened && preparePosition >= 0 && publishPosition > preparePosition,
                QStringLiteral("export stable-topologically orders dependencies before consumers"));
    test.check(orderedContents.contains("throw \"Untrusted relative executable in servicing plan: $Exe\"")
                   && orderedContents.contains("Relative executable paths are forbidden"),
               QStringLiteral("exported runner rejects unknown or path-bearing relative executables"));
    test.check(!orderedContents.contains("Invoke-WimForgeStep 'optional'")
                   && !orderedContents.contains("Optional sentinel"),
               QStringLiteral("export omits an intentionally skipped operation"));
    test.check(orderedContents.contains("checkpointBefore=true")
                   && orderedContents.contains("writesMediaWorkspace=true")
                   && orderedContents.contains("\"writeScope\":\"media\"")
                   && orderedContents.contains("requiresAdministrator=true")
                   && orderedContents.contains("writesMountedImage=true"),
               QStringLiteral("export preserves safety flags and operation metadata as review comments"));

    ServicingOperation missingDependency = publish;
    missingDependency.dependsOn = {QStringLiteral("absent")};
    const QString missingPath = QDir(root).filePath(QStringLiteral("output/missing.ps1"));
    error.clear();
    test.check(!ServicingPlan::exportPowerShell(
                   project, {missingDependency}, missingPath, &error)
                   && error.contains(QStringLiteral("missing operation"))
                   && !QFileInfo::exists(missingPath),
               QStringLiteral("export rejects a missing dependency before writing a script: %1")
                   .arg(error));

    ServicingOperation cycleFirst = prepare;
    cycleFirst.id = QStringLiteral("cycle-first");
    cycleFirst.dependsOn = {QStringLiteral("cycle-second")};
    ServicingOperation cycleSecond = publish;
    cycleSecond.id = QStringLiteral("cycle-second");
    cycleSecond.dependsOn = {QStringLiteral("cycle-first")};
    const QString cyclePath = QDir(root).filePath(QStringLiteral("output/cycle.ps1"));
    error.clear();
    test.check(!ServicingPlan::exportPowerShell(
                   project, {cycleFirst, cycleSecond}, cyclePath, &error)
                   && error.contains(QStringLiteral("cycle"))
                   && !QFileInfo::exists(cyclePath),
               QStringLiteral("export rejects cyclic dependencies before writing a script: %1")
                   .arg(error));
}

void testOfflineRegistrySemantics(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/install.wim")));
    project.imagePath = project.sourcePath;

    RegistryTweak user;
    user.hive = QStringLiteral("HKCU");
    user.key = QStringLiteral("Software\\Policies\\WimForge\\User");
    user.valueName = QStringLiteral("Path");
    user.type = QStringLiteral("REG_EXPAND_SZ");
    user.value = QStringLiteral("%SystemRoot%\\WimForge");
    user.ownerId = QStringLiteral("user");

    RegistryTweak multi;
    multi.hive = QStringLiteral("HKLM");
    multi.key = QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\WimForge\\Machine");
    multi.valueName = QStringLiteral("Items");
    multi.type = QStringLiteral("REG_MULTI_SZ");
    multi.value = QStringLiteral("one\r\ntwo");
    multi.ownerId = QStringLiteral("multi");

    RegistryTweak maximumDword;
    maximumDword.hive = QStringLiteral("HKLM");
    maximumDword.key = QStringLiteral("SOFTWARE\\Policies\\WimForge\\Machine");
    maximumDword.valueName = QStringLiteral("Maximum");
    maximumDword.type = QStringLiteral("REG_DWORD");
    maximumDword.value = QStringLiteral("4294967295");
    maximumDword.ownerId = QStringLiteral("dword");

    RegistryTweak clear;
    clear.hive = QStringLiteral("HKLM");
    clear.key = QStringLiteral("SOFTWARE\\Policies\\WimForge\\List");
    clear.deleteAllValues = true;
    clear.ownerId = QStringLiteral("clear");
    project.registryTweaks = {user, multi, maximumDword, clear};

    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("offline registry plan is valid: %1")
                              .arg(plan.errors.join(QStringLiteral(" | "))));
    auto editForOwner = [&plan](const QString &owner) {
        return findOperation(plan, [&owner](const ServicingOperation &operation) {
            return operation.kind == OperationKind::Registry
                && operation.metadata.value(QStringLiteral("owner")).toString() == owner;
        });
    };
    const ServicingOperation *userEdit = editForOwner(QStringLiteral("user"));
    test.check(userEdit && userEdit->arguments.value(1).endsWith(
                       QStringLiteral("\\Software\\Policies\\WimForge\\User"))
                   && userEdit->metadata.value(QStringLiteral("registryHiveFile")).toString()
                          .endsWith(QStringLiteral("Users/Default/NTUSER.DAT")),
               QStringLiteral("HKCU retains Software prefix beneath offline NTUSER.DAT"));

    const ServicingOperation *multiEdit = editForOwner(QStringLiteral("multi"));
    test.check(multiEdit && multiEdit->arguments.value(1).endsWith(
                       QStringLiteral("\\Policies\\WimForge\\Machine"))
                   && multiEdit->arguments.contains(QStringLiteral("REG_MULTI_SZ"))
                   && multiEdit->arguments.contains(QStringLiteral("one\\0two")),
               QStringLiteral("HKLM SOFTWARE root is stripped and REG_MULTI_SZ uses reg.exe separators"));

    const ServicingOperation *dwordEdit = editForOwner(QStringLiteral("dword"));
    test.check(dwordEdit && dwordEdit->arguments.contains(QStringLiteral("4294967295")),
               QStringLiteral("unsigned DWORD command data remains exact"));

    const ServicingOperation *clearEdit = editForOwner(QStringLiteral("clear"));
    test.check(clearEdit && clearEdit->arguments.value(0) == QStringLiteral("delete")
                   && clearEdit->arguments.contains(QStringLiteral("/va"))
                   && !clearEdit->arguments.contains(QStringLiteral("/ve"))
                   && clearEdit->destructive,
               QStringLiteral("non-additive clear deletes values only and retains the registry key"));
}

void testTypedCustomizePlan(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = makeFile(QDir(root).filePath(QStringLiteral("input/install.wim")));
    project.imagePath = project.sourcePath;
    project.targetBuildNumber = 26000;
    project.updates = {makeFile(QDir(root).filePath(QStringLiteral("payload/kb-test.msu")))};
    project.packages = {makeFile(QDir(root).filePath(QStringLiteral("payload/language.cab")))};
    project.appxPackagesToRemove = {QStringLiteral("Microsoft.TestApp_1.0_neutral__test")};
    project.appxPackagesToProvision = {
        makeFile(QDir(root).filePath(QStringLiteral("payload/TestApp.msix")))};
    project.componentsToRemove = {QStringLiteral("Package_for_Test~31bf~amd64~~1.0.0.0")};
    project.scheduledTaskChanges = {
        ScheduledTaskChange{QStringLiteral("Microsoft/Windows/Maps/MapsUpdateTask"),
                            ScheduledTaskDisposition::Disable, false},
    };
    const QString answer = makeFile(QDir(root).filePath(QStringLiteral("payload/unattend.xml")),
                                    QByteArray("<unattend/>"));
    project.answerFileActions = {
        AnswerFileAction{AnswerFileMode::Apply, answer, {}, PayloadScope::Image},
        AnswerFileAction{AnswerFileMode::Place, answer,
                         QStringLiteral("Windows/Panther/WimForge.xml"), PayloadScope::Image},
        AnswerFileAction{AnswerFileMode::Remove, {}, QStringLiteral("autounattend.xml"),
                         PayloadScope::Media},
    };
    const QString payload = makeFile(QDir(root).filePath(QStringLiteral("payload/tool.exe")));
    project.stagedPayloads = {
        StagedPayload{payload, QStringLiteral("Windows/Setup/Scripts/WimForge/tool.exe"),
                      PayloadScope::Image, QStringLiteral("post-setup-tool"), QString()},
    };
    const QString literalCommand = QStringLiteral("cmd.exe /c echo A ^& B > C:\\WimForge.txt");
    project.postSetupCommands = {
        PostSetupCommand{literalCommand, QStringLiteral("Write WimForge marker")},
    };
    project.featuresToEnable = {QStringLiteral("NetFx3")};
    project.capabilitiesToRemove = {QStringLiteral("MathRecognizer~~~~0.0.1.0")};
    for (const QString &id : {
             QStringLiteral("disableTelemetry"), QStringLiteral("localAccountOobe"),
             QStringLiteral("showFileExtensions"), QStringLiteral("classicContextMenu"),
             QStringLiteral("disableConsumerFeatures"), QStringLiteral("enableLongPaths"),
             QStringLiteral("performanceVisuals"), QStringLiteral("disableRecall")}) {
        project.setCustomizeSetting(id, true);
    }
    project.options.dryRun = true;

    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("typed Customize plan is valid: %1")
                              .arg(plan.errors.join(QStringLiteral(" | "))));

    const ServicingOperation *update = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Update;
    });
    const ServicingOperation *package = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Package;
    });
    test.check(update && package
                   && update->metadata.value(QStringLiteral("packageClass")) == QStringLiteral("update")
                   && package->metadata.value(QStringLiteral("packageClass")) == QStringLiteral("package"),
               QStringLiteral("updates and standalone packages remain distinct typed operations"));

    const ServicingOperation *appxRemove = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Appx
            && operation.metadata.value(QStringLiteral("appxAction")) == QStringLiteral("remove");
    });
    const ServicingOperation *appxProvision = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Appx
            && operation.metadata.value(QStringLiteral("appxAction")) == QStringLiteral("provision");
    });
    test.check(appxRemove && appxProvision,
               QStringLiteral("Appx removal and provisioning have distinct typed actions"));

    const ServicingOperation *task = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::ScheduledTask;
    });
    test.check(task && task->metadata.value(QStringLiteral("taskAction")) == QStringLiteral("disable")
                   && task->reversible && !task->compatibilityNotes.isEmpty(),
               QStringLiteral("scheduled-task changes are reversible and compatibility annotated"));

    const ServicingOperation *answerApply = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Unattended
            && operation.metadata.value(QStringLiteral("answerFileAction")) == QStringLiteral("apply");
    });
    const ServicingOperation *answerPlace = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::StageFile
            && operation.metadata.value(QStringLiteral("role")) == QStringLiteral("answer-file");
    });
    const ServicingOperation *answerRemove = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Unattended
            && operation.metadata.value(QStringLiteral("answerFileAction")) == QStringLiteral("remove");
    });
    test.check(answerApply && answerPlace && answerRemove,
               QStringLiteral("answer-file apply, placement, and removal are distinct operations"));

    const ServicingOperation *staged = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::StageFile
            && operation.metadata.value(QStringLiteral("role")) == QStringLiteral("post-setup-tool");
    });
    const ServicingOperation *postSetup = findOperation(plan, [&literalCommand](const ServicingOperation &operation) {
        return operation.kind == OperationKind::PostSetup
            && operation.metadata.value(QStringLiteral("literalCommand")) == literalCommand;
    });
    test.check(staged && postSetup
                   && postSetup->metadata.value(QStringLiteral("idempotent")).toBool()
                   && postSetup->arguments.join(QLatin1Char(' ')).contains(QStringLiteral("REM WimForge:")),
               QStringLiteral("staged payloads and idempotent literal post-setup commands remain separate"));

    int settingOperations = 0;
    bool recallCompatibilityWarning = false;
    for (const ServicingOperation &operation : plan.operations) {
        if (operation.metadata.value(QStringLiteral("customizeSetting")).toString().isEmpty())
            continue;
        ++settingOperations;
        test.check(operation.kind == OperationKind::Registry && operation.reversible
                       && operation.metadata.value(QStringLiteral("reversal")).isObject()
                       && !operation.compatibilityNotes.isEmpty(),
                   QStringLiteral("Customize setting %1 is a reversible annotated registry operation")
                       .arg(operation.metadata.value(QStringLiteral("customizeSetting")).toString()));
        if (operation.metadata.value(QStringLiteral("customizeSetting")) == QStringLiteral("disableRecall"))
            recallCompatibilityWarning = operation.compatibilityNotes.join(QLatin1Char(' '))
                                             .contains(QStringLiteral("outside the verified range"));
    }
    test.check(settingOperations == 8 && recallCompatibilityWarning,
               QStringLiteral("all eight settings generate build-aware typed operations"));

    test.check(update && update->writeScope == OperationWriteScope::MountedImage
                   && update->skipConsequence == SkipConsequence::OmitsOptionalChange
                   && update->metadata.value(QStringLiteral("previewOnly")).toBool(),
               QStringLiteral("operation model exposes scope, skip consequence, and dry-run state"));
    if (update) {
        const QJsonObject json = update->toJson();
        test.check(json.value(QStringLiteral("dependsOn")).isArray()
                       && json.value(QStringLiteral("checkpointRequired")).isBool()
                       && json.value(QStringLiteral("parallelEligible")).isBool()
                       && json.value(QStringLiteral("writeScope")) == QStringLiteral("mounted-image")
                       && json.value(QStringLiteral("skipConsequence"))
                              == QStringLiteral("omits-optional-change"),
                   QStringLiteral("operation JSON serializes review and scheduling semantics"));
    }

    ProjectConfig unsafe = project;
    unsafe.scheduledTaskChanges = {
        ScheduledTaskChange{QStringLiteral("Microsoft/Windows/Test"),
                            ScheduledTaskDisposition::Remove, false},
    };
    const ServicingPlanResult rejected = ServicingPlan::build(unsafe);
    test.check(!rejected.ok()
                   && rejected.errors.join(QLatin1Char(' ')).contains(QStringLiteral("explicit compatibility override")),
               QStringLiteral("destructive scheduled-task removal requires an explicit override"));
}

void testOnlineTarget(TestRun &test, const QString &root)
{
    ProjectConfig project = baseProject(root);
    project.sourcePath = root;
    project.imagePath.clear();
    project.mountPath.clear();
    project.outputPath = QDir(root).filePath(QStringLiteral("output/live-plan.wim"));
    project.featuresToEnable = {QStringLiteral("NetFx3")};
    project.options.extra.insert(QStringLiteral("targetOnline"), true);

    const ServicingPlanResult plan = ServicingPlan::build(project);
    test.check(plan.ok(), QStringLiteral("online plan permits output metadata inside source/project root: %1")
                              .arg(plan.errors.join(QStringLiteral(" | "))));
    const ServicingOperation *feature = findOperation(plan, [](const ServicingOperation &operation) {
        return operation.kind == OperationKind::Feature;
    });
    test.check(feature && feature->arguments.contains(QStringLiteral("/Online")),
               QStringLiteral("online target uses DISM /Online"));
    test.check(operationsOfKind(plan, OperationKind::Mount).isEmpty()
                   && operationsOfKind(plan, OperationKind::Export).isEmpty(),
               QStringLiteral("online plan does not mount or export an absent working image"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeServicingPlanTests"));

    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    testIsoFolder(test, QDir(temporary.path()).filePath(QStringLiteral("iso-folder")));
    testIsoFile(test, QDir(temporary.path()).filePath(QStringLiteral("iso-file")));
    testIsoContainedEsd(test, QDir(temporary.path()).filePath(QStringLiteral("iso-contained-esd")));
    testIsoContainedSwm(test, QDir(temporary.path()).filePath(QStringLiteral("iso-contained-swm")));
    testContainerSources(test, QDir(temporary.path()).filePath(QStringLiteral("containers")));
    testSplitSource(test, QDir(temporary.path()).filePath(QStringLiteral("split")));
    testStagingAndHashGate(test, QDir(temporary.path()).filePath(QStringLiteral("staging")));
    testSourceImmutability(test, QDir(temporary.path()).filePath(QStringLiteral("immutability")));
    testUnsafeStagedPaths(test, QDir(temporary.path()).filePath(QStringLiteral("unsafe")));
    testQuotingAndExport(test, QDir(temporary.path()).filePath(QStringLiteral("quoting")));
    testOfflineRegistrySemantics(test, QDir(temporary.path()).filePath(QStringLiteral("registry")));
    testTypedCustomizePlan(test, QDir(temporary.path()).filePath(QStringLiteral("typed-customize")));
    testOnlineTarget(test, QDir(temporary.path()).filePath(QStringLiteral("online")));
    return test.result();
}
