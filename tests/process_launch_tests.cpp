#include "core/ProcessLaunch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

class TestContext
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int failures() const { return m_failures; }

private:
    int m_failures = 0;
};

int runCapturedChild()
{
    QTextStream(stdout) << "captured stdout\n";
    QTextStream(stderr) << "captured stderr\n";
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--captured-child")))
        return runCapturedChild();

    TestContext test;
    QProcess process;

    const QString applicationPath = QDir::cleanPath(QCoreApplication::applicationFilePath());
    test.check(wimforge::resolveExecutableForLaunch(applicationPath) == applicationPath,
               QStringLiteral("absolute executable paths are preserved"));
    test.check(wimforge::resolveExecutableForLaunch(QStringLiteral("tools/helper.exe")).isEmpty()
                   && wimforge::resolveExecutableForLaunch(QStringLiteral("tools\\helper.exe")).isEmpty(),
               QStringLiteral("relative executable paths with separators are rejected"));

#ifdef Q_OS_WIN
    const QString dism = wimforge::resolveExecutableForLaunch(QStringLiteral("dism.exe"));
    const QString powershell = wimforge::resolveExecutableForLaunch(QStringLiteral("powershell.exe"));
    const QString trustedGit = wimforge::resolveExecutableForLaunch(QStringLiteral("git"));
    const QString trustedOscdimg =
        wimforge::resolveExecutableForLaunch(QStringLiteral("oscdimg.exe"));
    qputenv("COR_ENABLE_PROFILING", QByteArray("1"));
    qputenv("COR_PROFILER", QByteArray("{11111111-1111-1111-1111-111111111111}"));
    qputenv("COMPLUS_ReadyToRun", QByteArray("0"));
    qputenv("DOTNET_STARTUP_HOOKS", QByteArray("C:\\Temp\\attacker.dll"));
    qputenv("PSModulePath", QByteArray("C:\\Temp\\Modules"));
    const QProcessEnvironment sanitized = wimforge::sanitizedPowerShellEnvironment();
    qunsetenv("COR_ENABLE_PROFILING");
    qunsetenv("COR_PROFILER");
    qunsetenv("COMPLUS_ReadyToRun");
    qunsetenv("DOTNET_STARTUP_HOOKS");
    qunsetenv("PSModulePath");
    test.check(QFileInfo(dism).isAbsolute()
                   && QFileInfo(dism).fileName().compare(
                          QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
                   && QFileInfo(powershell).isAbsolute()
                   && QDir::fromNativeSeparators(powershell).contains(
                          QStringLiteral("System32/WindowsPowerShell"),
                          Qt::CaseInsensitive),
                QStringLiteral("Windows inbox servicing tools resolve through System32"));
    test.check(!sanitized.contains(QStringLiteral("COR_ENABLE_PROFILING"))
                   && !sanitized.contains(QStringLiteral("COR_PROFILER"))
                   && !sanitized.contains(QStringLiteral("COMPLUS_ReadyToRun"))
                   && !sanitized.contains(QStringLiteral("DOTNET_STARTUP_HOOKS"))
                   && !sanitized.value(QStringLiteral("PSModulePath")).contains(
                          QStringLiteral("Temp"), Qt::CaseInsensitive)
                   && QDir::fromNativeSeparators(
                          sanitized.value(QStringLiteral("PSModulePath"))).contains(
                          QStringLiteral("System32/WindowsPowerShell"),
                          Qt::CaseInsensitive),
               QStringLiteral("internal PowerShell children reject CLR and module-path injection"));

    test.check(QFileInfo(trustedGit).isAbsolute()
                   && QFileInfo(trustedGit).isFile()
                   && QDir::fromNativeSeparators(trustedGit).contains(
                          QStringLiteral("/Git/"), Qt::CaseInsensitive)
                   && !QFileInfo(trustedGit).isSymLink()
                   && !QFileInfo(trustedGit).isJunction(),
               QStringLiteral("Git resolves only from a non-reparse machine installation"));

    QTemporaryDir plantedDirectory;
    const QString previousDirectory = QDir::currentPath();
    QFile planted(QDir(plantedDirectory.path()).filePath(QStringLiteral("dism.exe")));
    test.check(plantedDirectory.isValid() && planted.open(QIODevice::WriteOnly)
                   && planted.write("not a program") > 0,
               QStringLiteral("planted current-directory fixture is available"));
    planted.close();
    QDir::setCurrent(plantedDirectory.path());
    const QString afterPlant = wimforge::resolveExecutableForLaunch(QStringLiteral("dism.exe"));
    QDir::setCurrent(previousDirectory);
    test.check(afterPlant == dism
                   && QFileInfo(afterPlant).absolutePath().compare(
                          plantedDirectory.path(), Qt::CaseInsensitive) != 0,
                QStringLiteral("current-directory executable planting is ignored"));

    QFile plantedGit(QDir(plantedDirectory.path()).filePath(QStringLiteral("git.exe")));
    test.check(plantedGit.open(QIODevice::WriteOnly)
                   && plantedGit.write("not trusted git") > 0,
               QStringLiteral("planted PATH Git fixture is available"));
    plantedGit.close();
    const QByteArray previousPath = qgetenv("PATH");
    qputenv("PATH", QDir::toNativeSeparators(plantedDirectory.path()).toLocal8Bit());
    const QString afterGitPlant =
        wimforge::resolveExecutableForLaunch(QStringLiteral("git.exe"));
    qputenv("PATH", previousPath);
    test.check(afterGitPlant == trustedGit
                   && QFileInfo(afterGitPlant).absoluteFilePath().compare(
                          plantedGit.fileName(), Qt::CaseInsensitive) != 0,
               QStringLiteral("user-writable PATH Git planting is ignored"));

    QFile plantedOscdimg(
        QDir(plantedDirectory.path()).filePath(QStringLiteral("oscdimg.exe")));
    test.check(plantedOscdimg.open(QIODevice::WriteOnly)
                   && plantedOscdimg.write("not trusted oscdimg") > 0,
               QStringLiteral("planted PATH Oscdimg fixture is available"));
    plantedOscdimg.close();
    qputenv("PATH", QDir::toNativeSeparators(plantedDirectory.path()).toLocal8Bit());
    const QString afterOscdimgPlant =
        wimforge::resolveExecutableForLaunch(QStringLiteral("oscdimg.exe"));
    qputenv("PATH", previousPath);
    test.check(afterOscdimgPlant == trustedOscdimg
                   && QFileInfo(afterOscdimgPlant).absoluteFilePath().compare(
                          plantedOscdimg.fileName(), Qt::CaseInsensitive) != 0,
               QStringLiteral("user-writable PATH Oscdimg planting is ignored"));
    if (!trustedOscdimg.isEmpty()) {
        test.check(QFileInfo(trustedOscdimg).isAbsolute()
                       && QFileInfo(trustedOscdimg).isFile()
                       && QDir::fromNativeSeparators(trustedOscdimg).contains(
                              QStringLiteral("/Windows Kits/10/Assessment and Deployment Kit/Deployment Tools/amd64/Oscdimg/"),
                              Qt::CaseInsensitive)
                       && !QFileInfo(trustedOscdimg).isSymLink()
                       && !QFileInfo(trustedOscdimg).isJunction(),
                   QStringLiteral("installed Oscdimg resolves only from protected ADK amd64 tools"));
    }

    int previousModifierCalls = 0;
    process.setCreateProcessArgumentsModifier(
        [&previousModifierCalls](QProcess::CreateProcessArguments *) {
            ++previousModifierCalls;
        });
#endif

    wimforge::configureProcessWithoutConsole(process);

#ifdef Q_OS_WIN
    const QProcess::CreateProcessArgumentModifier modifier =
        process.createProcessArgumentsModifier();
    test.check(static_cast<bool>(modifier),
               QStringLiteral("Windows child processes receive a CreateProcess modifier"));
    QProcess::CreateProcessArguments arguments{};
    arguments.flags = CREATE_UNICODE_ENVIRONMENT;
    modifier(&arguments);
    test.check((arguments.flags & CREATE_NO_WINDOW) != 0,
               QStringLiteral("Windows child processes use CREATE_NO_WINDOW"));
    test.check(previousModifierCalls == 1,
               QStringLiteral("The no-console helper preserves an existing modifier"));
#endif

    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({QStringLiteral("--captured-child")});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    test.check(process.waitForStarted(10'000),
               QStringLiteral("The configured child process starts"));
    test.check(process.waitForFinished(10'000),
               QStringLiteral("The configured child process finishes"));
    test.check(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
               QStringLiteral("The configured child process exits normally"));
    test.check(process.readAllStandardOutput().contains("captured stdout"),
               QStringLiteral("CREATE_NO_WINDOW preserves captured stdout"));
    test.check(process.readAllStandardError().contains("captured stderr"),
               QStringLiteral("CREATE_NO_WINDOW preserves captured stderr"));

    return test.failures() == 0 ? 0 : 1;
}
