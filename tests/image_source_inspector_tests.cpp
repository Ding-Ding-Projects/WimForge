#include "core/ImageSourceInspector.h"

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
            QTextStream(stdout) << "image_source_inspector_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

QString makeFile(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7)
        return {};
    file.close();
    return QFileInfo(file).absoluteFilePath();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    const QString wim = makeFile(QDir(temporary.path()).filePath(QStringLiteral("direct/install.wim")));
    const ImageInspectionCommand direct = ImageSourceInspector::commandFor(wim);
    test.check(direct.error.isEmpty()
                   && QFileInfo(direct.program).fileName().compare(
                          QStringLiteral("dism.exe"), Qt::CaseInsensitive) == 0
                   && direct.arguments.contains(QStringLiteral("/WimFile:%1").arg(wim))
                   && !direct.isoSource,
               QStringLiteral("a direct WIM produces a typed DISM inventory command"));

    const QString media = QDir(temporary.path()).filePath(QStringLiteral("media"));
    const QString swm = makeFile(QDir(media).filePath(QStringLiteral("sources/install.swm")));
    const ImageInspectionCommand folder = ImageSourceInspector::commandFor(media);
    test.check(folder.error.isEmpty()
                   && folder.arguments.contains(QStringLiteral("/WimFile:%1").arg(swm)),
               QStringLiteral("installation media discovers split install.swm sources"));

    const QString hostileIso = makeFile(
        QDir(temporary.path()).filePath(QStringLiteral("iso/O'Brien & Windows.iso")));
    const QByteArray originalPath = qgetenv("PATH");
    const QByteArray originalModulePath = qgetenv("PSModulePath");
    qputenv("PATH", QDir::toNativeSeparators(temporary.path()).toLocal8Bit());
    qputenv("PSModulePath", QDir::toNativeSeparators(
        QDir(temporary.path()).filePath(QStringLiteral("attacker-modules"))).toLocal8Bit());
    qputenv("COR_ENABLE_PROFILING", QByteArray("1"));
    const ImageInspectionCommand iso = ImageSourceInspector::commandFor(hostileIso);
    qputenv("PATH", originalPath);
    qputenv("PSModulePath", originalModulePath);
    qunsetenv("COR_ENABLE_PROFILING");
    const QString isoScript = iso.arguments.isEmpty() ? QString() : iso.arguments.constLast();
    test.check(iso.error.isEmpty()
                   && QFileInfo(iso.program).fileName().compare(
                          QStringLiteral("powershell.exe"), Qt::CaseInsensitive) == 0
                   && iso.isoSource && iso.utf8Output
                   && iso.environment.value(QStringLiteral("WIMFORGE_ISO_PATH")) == hostileIso
                   && !iso.environment.contains(QStringLiteral("COR_ENABLE_PROFILING"))
                   && !QDir::fromNativeSeparators(
                          iso.environment.value(QStringLiteral("PSModulePath"))).contains(
                          QStringLiteral("attacker-modules"), Qt::CaseInsensitive),
                QStringLiteral("ISO inspection uses a sanitized process environment"));
    test.check(!isoScript.contains(hostileIso)
                   && isoScript.contains(QStringLiteral("Microsoft.PowerShell.Core\\Import-Module"))
                   && isoScript.contains(QStringLiteral("Storage\\Mount-DiskImage"))
                   && isoScript.contains(QStringLiteral("Storage\\Dismount-DiskImage"))
                   && isoScript.contains(QStringLiteral("Storage\\Get-DiskImage"))
                   && isoScript.contains(QStringLiteral("[Environment]::SystemDirectory"))
                   && isoScript.contains(QStringLiteral("$env:PSModulePath"))
                   && !isoScript.contains(QStringLiteral("$disk = Mount-DiskImage"))
                   && isoScript.contains(QStringLiteral("stillAttached"))
                   && isoScript.contains(QStringLiteral("finally")),
                QStringLiteral("ISO inspection pins Storage and confirms dismount"));

    const QByteArray output(
        "WIMFORGE_IMAGE_PATH::R:\\sources\\install.esd\r\n"
        "Deployment Image Servicing and Management tool\r\n"
        "Index : 1\r\nName : Windows 11 Pro\r\nDescription : Example\r\n"
        "Index : 2\r\nName : Windows 11 Enterprise\r\nDescription : Example\r\n");
    const ImageInspectionResult parsed = ImageSourceInspector::parseOutput(output, true, true);
    test.check(parsed.relativeImagePath == QStringLiteral("sources/install.esd")
                   && parsed.editions == QStringList{
                       QStringLiteral("Index 1 — Windows 11 Pro"),
                       QStringLiteral("Index 2 — Windows 11 Enterprise")},
               QStringLiteral("mounted ISO output yields a stable relative path and edition list"));

    const QString invalid = makeFile(
        QDir(temporary.path()).filePath(QStringLiteral("unsupported/source.txt")));
    test.check(!ImageSourceInspector::commandFor(invalid).error.isEmpty(),
               QStringLiteral("unsupported source files produce an actionable error"));

    return test.result();
}
