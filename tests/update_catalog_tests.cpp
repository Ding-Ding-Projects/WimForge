#include "core/UpdateCatalog.h"

#include <QCoreApplication>
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
            QTextStream(stdout) << "update_catalog_tests: all checks passed\n";
        else
            QTextStream(stderr) << "update_catalog_tests: " << m_failures << " failed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

// Two rows in the exact shape the Microsoft Update Catalog emits: a <tr
// id="<guid>_R0"> with single-quoted anchor ids and a size cell that carries
// both the display size and the exact byte count.
const QString kSearchFixture = QStringLiteral(R"HTML(
<table>
<tr id="9ed22950-33bd-4cbf-b123-8bceafe41a45_R0" style="border-width:0px;">
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C0_R0">&nbsp;</td>
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C1_R0">
    <a id='9ed22950-33bd-4cbf-b123-8bceafe41a45_link' href="javascript:void(0);" onclick='goToDetails("9ed22950-33bd-4cbf-b123-8bceafe41a45");'>2024-07 Cumulative Update for Windows 11 Version 24H2 for arm64-based Systems (KB5040529)</a>
  </td>
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C2_R0">Windows 11</td>
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C3_R0">Updates</td>
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C4_R0">7/30/2024</td>
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C5_R0">n/a</td>
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C6_R0">426.7 MB<span style="display:none">447385650</span></td>
  <td class="resultsbottomBorder resultspadding" id="9ed22950-33bd-4cbf-b123-8bceafe41a45_C7_R0"><input type="button"/></td>
</tr>
<tr id="07e328fd-a039-45f2-90ad-2b43002eab3b_R1" style="border-width:0px;">
  <td id="07e328fd-a039-45f2-90ad-2b43002eab3b_C1_R1"><a id='07e328fd-a039-45f2-90ad-2b43002eab3b_link' href="#">2024-07 Cumulative Update for Windows 11 (KB5040442)</a></td>
  <td id="07e328fd-a039-45f2-90ad-2b43002eab3b_C2_R1">Windows 11</td>
  <td id="07e328fd-a039-45f2-90ad-2b43002eab3b_C3_R1">Security Updates</td>
  <td id="07e328fd-a039-45f2-90ad-2b43002eab3b_C4_R1">7/9/2024</td>
  <td id="07e328fd-a039-45f2-90ad-2b43002eab3b_C5_R1">n/a</td>
  <td id="07e328fd-a039-45f2-90ad-2b43002eab3b_C6_R1">1.2 GB 1288490188</td>
</tr>
</table>
)HTML");

const QString kDownloadFixture = QStringLiteral(
    "var downloadInformation = new Array();\n"
    "downloadInformation[0] = new Object();\n"
    "downloadInformation[0].files = new Array();\n"
    "downloadInformation[0].files[0] = new Object();\n"
    "downloadInformation[0].files[0].url = "
    "'https://catalog.sf.dl.delivery.mp.microsoft.com/filestreamingservice/files/"
    "db9d9456/public/windows11.0-kb5040529-arm64.msu';\n");

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    TestRun test;

    const QList<UpdateCatalogEntry> entries = UpdateCatalog::parseSearchResults(kSearchFixture);
    test.check(entries.size() == 2, QStringLiteral("both catalog rows parse"));
    if (entries.size() == 2) {
        const UpdateCatalogEntry &first = entries.at(0);
        test.check(first.updateId == QStringLiteral("9ed22950-33bd-4cbf-b123-8bceafe41a45"),
                   QStringLiteral("update id is read from the row"));
        test.check(first.title.contains(QStringLiteral("KB5040529"))
                       && first.title.contains(QStringLiteral("Cumulative Update")),
                   QStringLiteral("title is read from the single-quoted anchor cell"));
        test.check(first.product == QStringLiteral("Windows 11")
                       && first.classification == QStringLiteral("Updates")
                       && first.lastUpdated == QStringLiteral("7/30/2024"),
                   QStringLiteral("product, classification and date columns parse"));
        test.check(first.sizeText == QStringLiteral("426.7 MB") && first.sizeBytes == 447385650,
                   QStringLiteral("size splits into display text and exact bytes"));
        test.check(entries.at(1).sizeBytes == 1288490188,
                   QStringLiteral("a size with an inline byte count still parses"));
    }

    const QStringList urls = UpdateCatalog::parseDownloadUrls(kDownloadFixture);
    test.check(urls.size() == 1 && urls.first().endsWith(QStringLiteral(".msu")),
               QStringLiteral("download URL is extracted from the dialog response"));

    // A real cumulative update returns several files (e.g. a servicing-stack .cab
    // plus the .msu); all trusted ones must be kept in order, with duplicates removed.
    const QString multiFixture = QStringLiteral(
        "downloadInformation[0].files[0].url = 'https://catalog.s.download.windowsupdate.com/a/ssu.cab';\n"
        "downloadInformation[0].files[1].url = 'https://catalog.s.download.windowsupdate.com/a/lcu.msu';\n"
        "downloadInformation[0].files[2].url = 'https://catalog.s.download.windowsupdate.com/a/lcu.msu';\n");
    const QStringList multi = UpdateCatalog::parseDownloadUrls(multiFixture);
    test.check(multi.size() == 2
                   && multi.at(0).endsWith(QStringLiteral("ssu.cab"))
                   && multi.at(1).endsWith(QStringLiteral("lcu.msu")),
               QStringLiteral("every trusted file URL is kept in order and de-duplicated"));

    // A tampered response pointing at a foreign host must be rejected.
    const QString hostile = QStringLiteral(
        "downloadInformation[0].files[0].url = 'https://evil.example.com/payload.msu';");
    test.check(UpdateCatalog::parseDownloadUrls(hostile).isEmpty(),
               QStringLiteral("download URLs on untrusted hosts are rejected"));
    test.check(!UpdateCatalog::isTrustedDownloadUrl(
                   QUrl(QStringLiteral("http://download.windowsupdate.com/a.msu"))),
               QStringLiteral("plain-HTTP download URLs are rejected"));
    test.check(UpdateCatalog::isTrustedDownloadUrl(
                   QUrl(QStringLiteral("https://catalog.s.download.windowsupdate.com/x/a.cab"))),
               QStringLiteral("an https windowsupdate.com CAB URL is trusted"));
    // Trusted host + https, but a non-update file extension must still be rejected.
    test.check(!UpdateCatalog::isTrustedDownloadUrl(
                   QUrl(QStringLiteral("https://download.microsoft.com/x/setup.exe"))),
               QStringLiteral("a trusted host serving a non-msu/cab file is rejected"));
    // A look-alike host that merely contains a trusted domain must be rejected.
    test.check(!UpdateCatalog::isTrustedDownloadUrl(
                   QUrl(QStringLiteral("https://download.windowsupdate.com.evil.example/a.msu"))),
               QStringLiteral("a suffix look-alike host is rejected"));
    // Host matching is case-insensitive.
    test.check(UpdateCatalog::isTrustedDownloadUrl(
                   QUrl(QStringLiteral("https://DOWNLOAD.WINDOWSUPDATE.COM/a.msu"))),
               QStringLiteral("host matching ignores case"));

    test.check(UpdateCatalog::downloadDialogBody(
                   QStringLiteral("9ed22950-33bd-4cbf-b123-8bceafe41a45"))
                   .contains(QByteArrayLiteral("updateIDs=")),
               QStringLiteral("download-dialog body encodes the update id"));
    test.check(UpdateCatalog::downloadDialogBody(QStringLiteral("not-a-guid"))
                   == QByteArrayLiteral("updateIDs=%5B%5D"),
               QStringLiteral("a malformed update id yields an empty request body"));
    test.check(UpdateCatalog::searchUrl(QStringLiteral("KB5040529")).toString()
                   .contains(QStringLiteral("catalog.update.microsoft.com/Search.aspx")),
               QStringLiteral("search URL targets the Microsoft Update Catalog"));

    return test.result();
}
