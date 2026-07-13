#include "core/WorkspaceTabs.h"
#include "core/GitHistory.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

class TestContext
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }
    int failures = 0;
};

QVariantMap tab(const wimforge::WorkspaceTabs &tabs, int index)
{
    return tabs.tabs().value(index).toMap();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    TestContext test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary directory is available"));
    const QString firstProject = QDir(temporary.path()).filePath(QStringLiteral("first"));
    const QString secondProject = QDir(temporary.path()).filePath(QStringLiteral("second"));
    QDir().mkpath(firstProject);
    QDir().mkpath(secondProject);

    QString error;
    wimforge::WorkspaceTabs first;
    test.check(first.openProject(firstProject, &error), QStringLiteral("open first project: %1").arg(error));
    test.check(first.tabs().size() == 1 && first.activeIndex() == 0,
               QStringLiteral("new project starts with one active overview tab"));
    test.check(QFileInfo::exists(QDir(first.repositoryPath()).filePath(QStringLiteral(".git"))),
               QStringLiteral("tabs have a dedicated local Git repository"));

    {
        const QString deferredProject = QDir(temporary.path()).filePath(QStringLiteral("deferred"));
        QDir().mkpath(deferredProject);
        wimforge::WorkspaceTabs deferred;
        QString deferredError;
        test.check(deferred.openProject(deferredProject, &deferredError), deferredError);
        deferred.setDeferredPersistence(true);
        test.check(deferred.navigateActiveTab(2, QStringLiteral("Customize"), &deferredError)
                       && deferred.tabs().constFirst().toMap().value(QStringLiteral("page")).toInt() == 2
                       && deferred.hasPendingPersistence()
                       && deferred.pendingCommitMessage().contains(QStringLiteral(" / ")),
                   QStringLiteral("deferred mode updates tab state immediately and queues a bilingual commit"));
        test.check(deferred.flushPendingPersistence(&deferredError)
                       && !deferred.hasPendingPersistence(),
                   QStringLiteral("deferred tab state can be committed later by a serialized worker"));
        wimforge::WorkspaceTabs deferredReopened;
        test.check(deferredReopened.openProject(deferredProject, &deferredError)
                       && deferredReopened.tabs().constFirst().toMap()
                              .value(QStringLiteral("page")).toInt() == 2,
                   QStringLiteral("flushed deferred tab state survives reopening"));
    }

    test.check(first.openPage(2, QStringLiteral("Customize"), &error), error);
    test.check(first.openPage(1, QStringLiteral("Source & editions"), &error), error);
    test.check(first.tabs().size() == 3 && first.activeIndex() == 2,
               QStringLiteral("pages open as browser-style tabs"));
    test.check(first.update(2, {{QStringLiteral("title"), QStringLiteral("My source")},
                                {QStringLiteral("fontFamily"), QStringLiteral("Consolas")},
                                {QStringLiteral("fontSize"), 18},
                                {QStringLiteral("fontColor"), QStringLiteral("#12ab34")},
                                {QStringLiteral("bold"), true},
                                {QStringLiteral("italic"), true},
                                {QStringLiteral("strikeout"), true}}, &error), error);
    const QVariantMap styled = tab(first, 2);
    test.check(styled.value(QStringLiteral("title")).toString() == QStringLiteral("My source")
                   && styled.value(QStringLiteral("fontFamily")).toString() == QStringLiteral("Consolas")
                   && styled.value(QStringLiteral("fontSize")).toInt() == 18
                   && styled.value(QStringLiteral("fontColor")).toString() == QStringLiteral("#12AB34")
                   && styled.value(QStringLiteral("bold")).toBool()
                   && styled.value(QStringLiteral("italic")).toBool()
                   && styled.value(QStringLiteral("strikeout")).toBool(),
               QStringLiteral("rename and all requested font styles persist"));

    // The navigate/new-tab/close-many behaviors run on an isolated workspace so
    // they do not perturb the shared linear fixture used by later assertions.
    {
        const QString navProject = QDir(temporary.path()).filePath(QStringLiteral("navigation"));
        QDir().mkpath(navProject);
        wimforge::WorkspaceTabs nav;
        QString navError;
        test.check(nav.openProject(navProject, &navError), navError);
        test.check(nav.openPage(2, QStringLiteral("Customize"), &navError), navError);

        // Pressing a navigation entry must retarget the active tab, not spawn one.
        const int navBefore = nav.tabs().size();
        test.check(nav.navigateActiveTab(4, QStringLiteral("Unattended Studio"), &navError), navError);
        test.check(nav.tabs().size() == navBefore,
                   QStringLiteral("navigating the active tab does not open a new tab"));
        test.check(tab(nav, nav.activeIndex()).value(QStringLiteral("page")).toInt() == 4,
                   QStringLiteral("the active tab now shows the navigated page"));

        // A tab the user renamed (custom) keeps its title across navigation.
        test.check(nav.update(nav.activeIndex(),
                              {{QStringLiteral("title"), QStringLiteral("Keeper")},
                               {QStringLiteral("custom"), true}}, &navError), navError);
        test.check(nav.navigateActiveTab(5, QStringLiteral("Package Studio"), &navError), navError);
        test.check(tab(nav, nav.activeIndex()).value(QStringLiteral("title")).toString()
                           == QStringLiteral("Keeper")
                       && tab(nav, nav.activeIndex()).value(QStringLiteral("page")).toInt() == 5,
                   QStringLiteral("a custom tab title survives navigation while its page changes"));

        // The explicit new-tab action always appends a fresh tab.
        const int navBeforeNew = nav.tabs().size();
        test.check(nav.openNewTab(0, QStringLiteral("Overview"), &navError), navError);
        test.check(nav.tabs().size() == navBeforeNew + 1 && nav.activeIndex() == navBeforeNew,
                   QStringLiteral("openNewTab appends and activates a fresh tab"));

        // closeMany removes several tabs atomically but never the entire workspace.
        QList<int> navEvery;
        for (int index = 0; index < nav.tabs().size(); ++index)
            navEvery.append(index);
        test.check(!nav.closeMany(navEvery, &navError),
                   QStringLiteral("closeMany refuses to close every workspace tab"));
        const int navBeforeClose = nav.tabs().size();
        test.check(nav.closeMany({0, 1}, &navError), navError);
        test.check(nav.tabs().size() == navBeforeClose - 2,
                   QStringLiteral("closeMany removes exactly the requested tabs"));
    }

    QString historyError;
    const QList<wimforge::GitCommit> tabHistory =
        wimforge::GitHistory(first.repositoryPath(), {QStringLiteral("tabs.json")})
            .history(100, &historyError);
    bool everySubjectIsBilingual = !tabHistory.isEmpty() && historyError.isEmpty();
    for (const wimforge::GitCommit &commit : tabHistory) {
        everySubjectIsBilingual = everySubjectIsBilingual
            && commit.subject.contains(QStringLiteral(" / "))
            && commit.subject.contains(QRegularExpression(QStringLiteral("[\\x{3400}-\\x{9fff}]")));
    }
    test.check(everySubjectIsBilingual,
               QStringLiteral("every product-generated workspace-tab commit subject is bilingual"));

    const QString statePath = QDir(first.repositoryPath()).filePath(QStringLiteral("tabs.json"));
    QFile beforeFailureFile(statePath);
    test.check(beforeFailureFile.open(QIODevice::ReadOnly),
               QStringLiteral("tab state opens before rollback test"));
    const QByteArray beforeFailure = beforeFailureFile.readAll();
    beforeFailureFile.close();
    const int countBeforeFailure = first.tabs().size();
    const int activeBeforeFailure = first.activeIndex();
    const QString objectsPath = QDir(first.repositoryPath())
                                    .filePath(QStringLiteral(".git/objects"));
    const QString objectsBackupPath = QDir(first.repositoryPath())
                                          .filePath(QStringLiteral(".git/objects-test-backup"));
    test.check(QDir().rename(objectsPath, objectsBackupPath)
                   && writeFile(objectsPath, QByteArray("not an object directory")),
               QStringLiteral("unwritable Git object-store fixture is created"));
    QString failureError;
    const bool uncommitted = first.openPage(3, QStringLiteral("Group Policy Studio"),
                                            &failureError);
    test.check(QFile::remove(objectsPath)
                   && QDir().rename(objectsBackupPath, objectsPath),
               QStringLiteral("Git object store is restored after rollback test"));
    QFile afterFailureFile(statePath);
    test.check(afterFailureFile.open(QIODevice::ReadOnly),
               QStringLiteral("tab state opens after rollback test"));
    test.check(!uncommitted && !failureError.isEmpty()
                   && first.tabs().size() == countBeforeFailure
                   && first.activeIndex() == activeBeforeFailure
                   && afterFailureFile.readAll() == beforeFailure,
               QStringLiteral("a Git failure rolls memory and tabs.json back together"));

    const QString portable = QDir(temporary.path()).filePath(QStringLiteral("tabs.wftabs"));
    test.check(first.exportTabs(portable, &error), error);

    const QString unsafeProject = QDir(temporary.path()).filePath(QStringLiteral("unsafe-working-tree"));
    const QString unsafeRepository = QDir(unsafeProject).filePath(QStringLiteral(".wimforge/tabs"));
    test.check(QDir().mkpath(unsafeRepository)
                   && writeFile(QDir(unsafeRepository).filePath(QStringLiteral(".gitattributes")),
                                QByteArray("tabs.json filter=attacker\n")),
               QStringLiteral("unsafe working-tree fixture is created"));
    wimforge::WorkspaceTabs unsafeWorkingTree;
    QString unsafeError;
    test.check(!unsafeWorkingTree.openProject(unsafeProject, &unsafeError)
                   && unsafeError.contains(QStringLiteral("only tabs.json")),
               QStringLiteral("pre-created tab repositories reject extra working-tree controls"));

    const QString pointerProject = QDir(temporary.path()).filePath(QStringLiteral("git-pointer"));
    const QString pointerRepository = QDir(pointerProject).filePath(QStringLiteral(".wimforge/tabs"));
    test.check(QDir().mkpath(pointerRepository)
                   && writeFile(QDir(pointerRepository).filePath(QStringLiteral(".git")),
                                QByteArray("gitdir: C:/outside\n")),
               QStringLiteral("unsafe git-pointer fixture is created"));
    wimforge::WorkspaceTabs unsafePointer;
    unsafeError.clear();
    test.check(!unsafePointer.openProject(pointerProject, &unsafeError)
                   && unsafeError.contains(QStringLiteral(".git")),
               QStringLiteral("a .git pointer file can never redirect elevated tab history"));

    wimforge::WorkspaceTabs second;
    test.check(second.openProject(secondProject, &error), error);
    test.check(second.importTabs(portable, &error), error);
    test.check(second.tabs().size() == 4,
               QStringLiteral("portable tab definitions merge into another project"));

    const QString extraProject = QDir(temporary.path()).filePath(QStringLiteral("extra-working-tree"));
    QDir().mkpath(extraProject);
    wimforge::WorkspaceTabs extraRepository;
    test.check(extraRepository.openProject(extraProject, &error), error);
    test.check(writeFile(QDir(extraRepository.repositoryPath()).filePath(QStringLiteral("unexpected.txt")),
                         QByteArray("not tab state")),
               QStringLiteral("extra repository fixture is created"));
    const QString extraBundle = QDir(temporary.path()).filePath(QStringLiteral("extra.wftabrepo"));
    test.check(extraRepository.exportRepository(extraBundle, &error), error);
    const int tabsBeforeUnsafeImport = second.tabs().size();
    test.check(!second.importRepository(extraBundle, &unsafeError)
                   && second.tabs().size() == tabsBeforeUnsafeImport,
               QStringLiteral("repository import rejects extra working-tree files without replacing state"));

    const QString hookProject = QDir(temporary.path()).filePath(QStringLiteral("hook-repository"));
    QDir().mkpath(hookProject);
    wimforge::WorkspaceTabs hookRepository;
    test.check(hookRepository.openProject(hookProject, &error), error);
    test.check(hookRepository.importTabs(portable, &error), error);
    const QString hookPath = QDir(hookRepository.repositoryPath())
                                 .filePath(QStringLiteral(".git/hooks/post-commit"));
    const QString configPath = QDir(hookRepository.repositoryPath())
                                   .filePath(QStringLiteral(".git/config"));
    const QString attributesPath = QDir(hookRepository.repositoryPath())
                                       .filePath(QStringLiteral(".git/info/attributes"));
    test.check(writeFile(hookPath, QByteArray("attacker.exe\n"))
                   && writeFile(configPath,
                                QByteArray("[core]\n\thooksPath = hooks\n\tfsmonitor = attacker.exe\n"))
                   && writeFile(attributesPath, QByteArray("tabs.json filter=attacker\n")),
               QStringLiteral("untrusted Git control fixture is created"));
    const QString hookBundle = QDir(temporary.path()).filePath(QStringLiteral("hooks.wftabrepo"));
    test.check(hookRepository.exportRepository(hookBundle, &error), error);
    test.check(second.importRepository(hookBundle, &error), error);
    QFile safeConfig(QDir(second.repositoryPath()).filePath(QStringLiteral(".git/config")));
    test.check(safeConfig.open(QIODevice::ReadOnly),
               QStringLiteral("sanitized imported repository has local config"));
    const QByteArray safeConfigBytes = safeConfig.readAll().toLower();
    safeConfig.close();
    test.check(!QFileInfo::exists(QDir(second.repositoryPath())
                                      .filePath(QStringLiteral(".git/hooks/post-commit")))
                   && !QFileInfo::exists(QDir(second.repositoryPath())
                                             .filePath(QStringLiteral(".git/info/attributes")))
                   && !safeConfigBytes.contains("hookspath")
                   && !safeConfigBytes.contains("fsmonitor"),
               QStringLiteral("repository import neutralizes hooks, attributes, and executable Git config"));

    const QString repositoryBundle = QDir(temporary.path()).filePath(QStringLiteral("tabs.wftabrepo"));
    test.check(first.exportRepository(repositoryBundle, &error), error);
    test.check(second.importRepository(repositoryBundle, &error), error);
    test.check(second.tabs().size() == 3
                   && tab(second, 2).value(QStringLiteral("title")).toString() == QStringLiteral("My source")
                   && QFileInfo::exists(QDir(second.repositoryPath()).filePath(QStringLiteral(".git"))),
               QStringLiteral("one-file repository import restores tab state and complete Git repository"));

    wimforge::WorkspaceTabs reopened;
    test.check(reopened.openProject(secondProject, &error), error);
    test.check(reopened.tabs().size() == 3
                   && tab(reopened, 2).value(QStringLiteral("fontFamily")).toString() == QStringLiteral("Consolas"),
               QStringLiteral("tab repository survives project close and reopen"));

    if (test.failures == 0)
        QTextStream(stdout) << "Workspace tabs are Git-backed, styled, portable, and repository-bundle safe.\n";
    return test.failures == 0 ? 0 : 1;
}
