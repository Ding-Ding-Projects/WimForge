#include "core/PayloadCatalog.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

using namespace wimforge;

namespace {

struct TestRun
{
    int failures = 0;

    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++failures;
        std::cerr << "FAIL: " << message.toStdString() << '\n';
    }
};

QString writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(contents);
    return path;
}

} // namespace

int main()
{
    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary directory is available"));
    if (!temporary.isValid())
        return 1;

    const QString driver = writeFile(
        QDir(temporary.path()).filePath(QStringLiteral("drivers/netadapter.inf")),
        QByteArrayLiteral(
            "; a representative third-party driver\n"
            "[Version]\n"
            "Signature=\"$Windows NT$\"\n"
            "Class=Net\n"
            "Provider=%Vendor%\n"
            "DriverVer=07/11/2026,2.4.6.8\n"
            "CatalogFile.NTamd64=netadapter.cat\n"
            "[Strings]\n"
            "Vendor=\"Example Devices Ltd.\"\n"));
    const QString secondDriver = writeFile(
        QDir(temporary.path()).filePath(QStringLiteral("drivers/sub/storage.inf")),
        QByteArrayLiteral("[Version]\nClass=SCSIAdapter\n"));
    const QString update = writeFile(
        QDir(temporary.path()).filePath(QStringLiteral("updates/windows11-kb5069999-x64.msu")),
        QByteArray(2048, 'u'));
    const QString cabinet = writeFile(
        QDir(temporary.path()).filePath(QStringLiteral("updates/language-pack.cab")),
        QByteArray(1024, 'c'));
    writeFile(QDir(temporary.path()).filePath(QStringLiteral("updates/readme.txt")),
              QByteArrayLiteral("not a servicing package"));

    const ServicingPayloadEntry driverEntry = PayloadCatalog::inspect(
        driver, ServicingPayloadKind::Driver);
    test.check(driverEntry.exists && driverEntry.supported,
               QStringLiteral("an existing INF is accepted as a driver payload"));
    test.check(driverEntry.provider == QStringLiteral("Example Devices Ltd.")
                   && driverEntry.driverClass == QStringLiteral("Net")
                   && driverEntry.driverVersion == QStringLiteral("07/11/2026,2.4.6.8")
                   && driverEntry.catalogFile == QStringLiteral("netadapter.cat"),
               QStringLiteral("INF Version and Strings metadata is resolved"));
    test.check(driverEntry.detailEnglish.contains(QStringLiteral("Driver metadata"))
                   && driverEntry.detailCantonese.contains(QStringLiteral("驅動資料"))
                   && driverEntry.detail.contains(QStringLiteral(" / ")),
               QStringLiteral("driver descriptions expose English and Hong Kong Cantonese"));

    const ServicingPayloadEntry driverFolder = PayloadCatalog::inspect(
        QFileInfo(driver).absolutePath(), ServicingPayloadKind::Driver);
    test.check(driverFolder.directory && driverFolder.supported
                   && driverFolder.containedFileCount == 2,
               QStringLiteral("a recursive driver folder reports its INF package count"));
    test.check(driverFolder.detailEnglish.contains(QStringLiteral("DISM"))
                   && driverFolder.detailCantonese.contains(QStringLiteral("遞迴加入")),
               QStringLiteral("driver-folder guidance is bilingual"));

    const ServicingPayloadEntry updateEntry = PayloadCatalog::inspect(
        update, ServicingPayloadKind::Update);
    test.check(updateEntry.exists && updateEntry.supported
                   && updateEntry.knowledgeBaseId == QStringLiteral("KB5069999")
                   && updateEntry.extension == QStringLiteral("msu"),
               QStringLiteral("CAB/MSU metadata includes a normalized KB identifier"));
    test.check(updateEntry.detailEnglish.contains(QStringLiteral("package"))
                   && updateEntry.detailCantonese.contains(QStringLiteral("更新套件"))
                   && updateEntry.detail.startsWith(QStringLiteral("KB5069999")),
               QStringLiteral("update descriptions are bilingual without changing the KB identifier"));

    const QStringList updates = PayloadCatalog::discoverFiles(
        QFileInfo(update).absolutePath(), ServicingPayloadKind::Update);
    test.check(updates.size() == 2 && updates.contains(update) && updates.contains(cabinet),
               QStringLiteral("update folder discovery accepts only CAB and MSU files"));

    const QStringList drivers = PayloadCatalog::discoverFiles(
        QFileInfo(driver).absolutePath(), ServicingPayloadKind::Driver);
    test.check(drivers.size() == 2 && drivers.contains(driver)
                   && drivers.contains(secondDriver),
               QStringLiteral("driver folder discovery is recursive and INF-only"));

    const ServicingPayloadEntry missing = PayloadCatalog::inspect(
        QDir(temporary.path()).filePath(QStringLiteral("missing.cab")),
        ServicingPayloadKind::Update);
    test.check(!missing.exists && !missing.supported,
               QStringLiteral("missing payloads remain visible but are not marked supported"));
    test.check(missing.detailCantonese == QStringLiteral("檔案或資料夾唔見咗"),
               QStringLiteral("missing-payload feedback has natural Hong Kong Cantonese"));
    test.check(!PayloadCatalog::isSupportedFile(
                   QDir(temporary.path()).filePath(QStringLiteral("setup.exe")),
                   ServicingPayloadKind::Driver),
               QStringLiteral("driver executables are not accepted as offline DISM driver packages"));

    if (test.failures == 0)
        std::cout << "All payload catalog tests passed.\n";
    return test.failures == 0 ? 0 : 1;
}
