#include "core/FileDialogPath.h"

#include <QCoreApplication>
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
        ++m_checks;
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int result() const
    {
        QTextStream(stdout) << "file_dialog_path_tests: " << m_checks
                            << " checks, " << m_failures << " failures\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_checks = 0;
    int m_failures = 0;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary test directory is available"));
    if (!temporary.isValid())
        return test.result();

    const QString path = temporary.path()
        + QStringLiteral("/工程 #100%/New Folder");
    const QUrl folderUrl = QUrl::fromLocalFile(path);
    const QString selected = pathFromFileDialogUrl(folderUrl);
    test.check(selected == QDir::toNativeSeparators(QDir::cleanPath(path)),
               QStringLiteral("a selected folder displays native path separators"));
    test.check(QDir::fromNativeSeparators(selected) == QDir::cleanPath(path),
               QStringLiteral("spaces, Unicode, hash and percent characters survive URL decoding"));
    test.check(pathFromFileDialogUrl(QUrl::fromLocalFile(path + QStringLiteral("/")))
                   == selected,
               QStringLiteral("a selected folder's trailing separator is cleaned"));
    test.check(pathFromFileDialogUrl(QUrl::fromLocalFile(
                   path + QStringLiteral("/../Final Folder")))
                   == QDir::toNativeSeparators(QDir::cleanPath(
                       path + QStringLiteral("/../Final Folder"))),
               QStringLiteral("relative path components are cleaned before native formatting"));
    test.check(pathFromFileDialogUrl(folderUrl) == selected,
               QStringLiteral("repeated folder selections return the same path"));
    test.check(QDir().mkpath(selected),
               QStringLiteral("native folder-picker paths remain valid QDir creation inputs"));
    test.check(QDir(selected).exists(),
               QStringLiteral("native folder-picker paths locate the selected directory"));

    const QString filePath = temporary.filePath(QStringLiteral("工程 #100%.json"));
    QFile file(filePath);
    test.check(file.open(QIODevice::WriteOnly), QStringLiteral("selected file fixture can be created"));
    file.close();
    test.check(QFileInfo::exists(pathFromFileDialogUrl(QUrl::fromLocalFile(filePath))),
               QStringLiteral("native file-picker paths remain valid QFileInfo inputs"));

    const QUrl remoteUrl(QStringLiteral("https://example.com/a/b?next=/c/d"));
    test.check(pathFromFileDialogUrl(remoteUrl)
                   == QDir::cleanPath(remoteUrl.toString(QUrl::PreferLocalFile)),
               QStringLiteral("non-file URL fallback retains its existing behavior"));
    test.check(pathFromFileDialogUrl(QUrl())
                   == QDir::cleanPath(QString()),
               QStringLiteral("empty URL fallback retains its existing behavior"));

#ifdef Q_OS_WIN
    test.check(pathFromFileDialogUrl(QUrl(QStringLiteral("file:///C:/Projects/New%20Folder/")))
                   == QStringLiteral("C:\\Projects\\New Folder"),
               QStringLiteral("Windows drive folders display backslashes"));
    test.check(pathFromFileDialogUrl(QUrl(QStringLiteral("file:///C:/")))
                   == QStringLiteral("C:\\"),
               QStringLiteral("Windows drive roots retain their separator"));
    test.check(pathFromFileDialogUrl(QUrl(QStringLiteral("file://server/share/New%20Folder")))
                   == QStringLiteral("\\\\server\\share\\New Folder"),
               QStringLiteral("Windows UNC paths retain the network prefix"));
    test.check(pathFromFileDialogUrl(QUrl::fromLocalFile(QStringLiteral("C:\\Projects\\工程 #100%")))
                   == QStringLiteral("C:\\Projects\\工程 #100%"),
               QStringLiteral("existing native Windows paths keep their characters"));
#else
    test.check(selected.contains(QLatin1Char('/')) && !selected.contains(QLatin1Char('\\')),
               QStringLiteral("POSIX folder paths keep forward slashes"));
#endif

    return test.result();
}
