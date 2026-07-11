#include "ProcessLaunch.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shlobj.h>
#endif

namespace wimforge {

#ifdef Q_OS_WIN
namespace {

bool isPathInside(const QString &childPath, const QString &parentPath)
{
    const QString child = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(childPath).absoluteFilePath()));
    QString parent = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(parentPath).absoluteFilePath()));
    if (child.compare(parent, Qt::CaseInsensitive) == 0)
        return true;
    if (!parent.endsWith(QLatin1Char('/')))
        parent.append(QLatin1Char('/'));
    return child.startsWith(parent, Qt::CaseInsensitive);
}

bool isLinkLike(const QFileInfo &info)
{
    return info.isSymLink() || info.isJunction();
}

QString programFilesFolder(int folderId)
{
    wchar_t buffer[MAX_PATH + 1]{};
    if (FAILED(::SHGetFolderPathW(nullptr, folderId, nullptr, SHGFP_TYPE_CURRENT,
                                  buffer))) {
        return {};
    }
    return QDir::cleanPath(QString::fromWCharArray(buffer));
}

bool isOrdinaryFileWithoutReparseAncestors(const QString &filePath,
                                           const QString &trustedRoot)
{
    const QFileInfo file(filePath);
    const QFileInfo root(trustedRoot);
    if (!file.isFile() || isLinkLike(file) || !root.isDir() || isLinkLike(root))
        return false;

    const QString canonicalFile = file.canonicalFilePath();
    const QString canonicalRoot = root.canonicalFilePath();
    if (canonicalFile.isEmpty() || canonicalRoot.isEmpty()
        || !isPathInside(canonicalFile, canonicalRoot)) {
        return false;
    }

    QString current = file.absoluteFilePath();
    while (isPathInside(current, root.absoluteFilePath())) {
        const QFileInfo currentInfo(current);
        if (isLinkLike(currentInfo))
            return false;
        if (QDir::cleanPath(currentInfo.absoluteFilePath()).compare(
                QDir::cleanPath(root.absoluteFilePath()), Qt::CaseInsensitive) == 0) {
            break;
        }
        const QString parent = currentInfo.dir().absolutePath();
        if (parent.compare(current, Qt::CaseInsensitive) == 0)
            return false;
        current = parent;
    }
    return true;
}

QString trustedGitExecutable()
{
    QStringList roots{
        programFilesFolder(CSIDL_PROGRAM_FILES),
        programFilesFolder(CSIDL_PROGRAM_FILESX86),
    };
    roots.removeAll(QString());
    roots.removeDuplicates();

    const QStringList relativeCandidates{
        QStringLiteral("Git/cmd/git.exe"),
        QStringLiteral("Git/bin/git.exe"),
        QStringLiteral("Git/mingw64/bin/git.exe"),
    };
    for (const QString &root : std::as_const(roots)) {
        for (const QString &relative : relativeCandidates) {
            const QString candidate = QDir(root).filePath(relative);
            if (isOrdinaryFileWithoutReparseAncestors(candidate, root))
                return QFileInfo(candidate).canonicalFilePath();
        }
    }
    return {};
}

QString trustedOscdimgExecutable()
{
    QStringList roots{
        programFilesFolder(CSIDL_PROGRAM_FILESX86),
        programFilesFolder(CSIDL_PROGRAM_FILES),
    };
    roots.removeAll(QString());
    roots.removeDuplicates();

    const QString relative = QStringLiteral(
        "Windows Kits/10/Assessment and Deployment Kit/Deployment Tools/amd64/Oscdimg/oscdimg.exe");
    for (const QString &root : std::as_const(roots)) {
        const QString candidate = QDir(root).filePath(relative);
        if (isOrdinaryFileWithoutReparseAncestors(candidate, root))
            return QFileInfo(candidate).canonicalFilePath();
    }
    return {};
}

} // namespace
#endif

QString resolveExecutableForLaunch(const QString &executable)
{
    const QString requested = executable.trimmed();
    if (requested.isEmpty())
        return {};
    const QFileInfo requestedInfo(requested);
    if (requestedInfo.isAbsolute())
        return QDir::cleanPath(requestedInfo.absoluteFilePath());
    if (requested.contains(QLatin1Char('/')) || requested.contains(QLatin1Char('\\')))
        return {};

#ifdef Q_OS_WIN
    const QString lower = requested.toLower();
    if (lower == QStringLiteral("git") || lower == QStringLiteral("git.exe"))
        return trustedGitExecutable();
    if (lower == QStringLiteral("oscdimg") || lower == QStringLiteral("oscdimg.exe"))
        return trustedOscdimgExecutable();

    wchar_t systemBuffer[MAX_PATH + 1]{};
    const UINT systemLength = ::GetSystemDirectoryW(systemBuffer, MAX_PATH);
    if (systemLength > 0 && systemLength < MAX_PATH) {
        static const QSet<QString> system32Executables{
            QStringLiteral("bcdboot.exe"), QStringLiteral("certutil.exe"),
            QStringLiteral("cmd.exe"), QStringLiteral("cscript.exe"),
            QStringLiteral("dism.exe"), QStringLiteral("expand.exe"),
            QStringLiteral("icacls.exe"), QStringLiteral("mshta.exe"),
            QStringLiteral("pnputil.exe"), QStringLiteral("reagentc.exe"),
            QStringLiteral("reg.exe"), QStringLiteral("regsvr32.exe"),
            QStringLiteral("robocopy.exe"), QStringLiteral("rundll32.exe"),
            QStringLiteral("schtasks.exe"), QStringLiteral("shutdown.exe"),
            QStringLiteral("takeown.exe"), QStringLiteral("where.exe"),
            QStringLiteral("wscript.exe"),
        };
        const QString systemDirectory = QString::fromWCharArray(
            systemBuffer, static_cast<qsizetype>(systemLength));
        if (lower == QStringLiteral("powershell.exe")) {
            return QDir(systemDirectory).filePath(
                QStringLiteral("WindowsPowerShell/v1.0/powershell.exe"));
        }
        if (system32Executables.contains(lower))
            return QDir(systemDirectory).filePath(lower);
    }
#endif

    // QStandardPaths searches PATH for bare names and returns an absolute path;
    // unlike CreateProcess' implicit lookup, it does not silently accept a
    // planted executable merely because it is in the current directory.
    const QString resolved = QStandardPaths::findExecutable(requested);
#ifdef Q_OS_WIN
    return resolved;
#else
    return resolved.isEmpty() ? requested : resolved;
#endif
}

QProcessEnvironment sanitizedPowerShellEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QStringList keys = environment.keys();
    for (const QString &key : keys) {
        const QString upper = key.toUpper();
        if (upper == QStringLiteral("PATH")
            || upper == QStringLiteral("PSMODULEPATH")
            || upper.startsWith(QStringLiteral("COR_"))
            || upper.startsWith(QStringLiteral("COMPLUS_"))
            || upper.startsWith(QStringLiteral("DOTNET_"))) {
            environment.remove(key);
        }
    }

#ifdef Q_OS_WIN
    wchar_t systemBuffer[MAX_PATH + 1]{};
    const UINT systemLength = ::GetSystemDirectoryW(systemBuffer, MAX_PATH);
    if (systemLength > 0 && systemLength < MAX_PATH) {
        const QString system32 = QString::fromWCharArray(
            systemBuffer, static_cast<qsizetype>(systemLength));
        const QString powerShellHome = QDir(system32).filePath(
            QStringLiteral("WindowsPowerShell/v1.0"));
        environment.insert(QStringLiteral("PATH"),
                           system32 + QDir::listSeparator() + powerShellHome);
        environment.insert(QStringLiteral("PSModulePath"),
                           QDir(powerShellHome).filePath(QStringLiteral("Modules")));
    }
#endif
    return environment;
}

void configureProcessWithoutConsole(QProcess &process)
{
#ifdef Q_OS_WIN
    const QProcess::CreateProcessArgumentModifier previous =
        process.createProcessArgumentsModifier();
    process.setCreateProcessArgumentsModifier(
        [previous](QProcess::CreateProcessArguments *arguments) {
            if (previous)
                previous(arguments);
            arguments->flags |= CREATE_NO_WINDOW;
        });
#else
    static_cast<void>(process);
#endif
}

} // namespace wimforge
