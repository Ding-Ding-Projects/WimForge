#include "AppController.h"
#include "cli/CliRunner.h"
#include "core/EmbeddedTerminalSession.h"
#include "core/StructuredLogger.h"
#include "startup/Elevation.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#endif

namespace {

class ApplicationLogSession final
{
public:
    explicit ApplicationLogSession(QString mode) : m_mode(std::move(mode))
    {
        QString error;
        m_active = wimforge::StructuredLogger::instance().initialize({}, &error);
        if (!m_active) {
            QTextStream(stderr)
                << "WimForge logging could not start / WimForge 啟動唔到記錄功能: "
                << error << '\n';
            return;
        }
        wimforge::StructuredLogger::instance().installQtMessageHandler();
        wimforge::StructuredLogger::instance().log(
            wimforge::LogSeverity::Info, QStringLiteral("application"),
            QStringLiteral("application.started"),
            QStringLiteral("WimForge process initialized. / WimForge 程序已經啟動。"),
            QJsonObject{{QStringLiteral("mode"), m_mode}});
    }

    ~ApplicationLogSession()
    {
        if (!m_active)
            return;
        const QJsonObject data{{QStringLiteral("mode"), m_mode},
                               {QStringLiteral("exitCode"), m_exitCode}};
        wimforge::StructuredLogger::instance().log(
            wimforge::LogSeverity::Info, QStringLiteral("application"),
            QStringLiteral("application.stopping"),
            QStringLiteral("WimForge process is stopping. / WimForge 程序而家停止。"), data);
        wimforge::StructuredLogger::instance().shutdown(data);
    }

    void setExitCode(int exitCode) { m_exitCode = exitCode; }

private:
    QString m_mode;
    int m_exitCode = -1;
    bool m_active = false;
};

#ifdef Q_OS_WIN
QString elevationActionName(wimforge::startup::ElevationAction action)
{
    switch (action) {
    case wimforge::startup::ElevationAction::Continue:
        return QStringLiteral("continue");
    case wimforge::startup::ElevationAction::Relaunched:
        return QStringLiteral("relaunched");
    case wimforge::startup::ElevationAction::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

QString elevationFailureStageName(wimforge::startup::ElevationFailureStage stage)
{
    using Stage = wimforge::startup::ElevationFailureStage;
    switch (stage) {
    case Stage::None: return QStringLiteral("none");
    case Stage::OpenProcessToken: return QStringLiteral("open-process-token");
    case Stage::QueryTokenElevation: return QStringLiteral("query-token-elevation");
    case Stage::ResolveExecutable: return QStringLiteral("resolve-executable");
    case Stage::ParseArguments: return QStringLiteral("parse-arguments");
    case Stage::RequestRelaunch: return QStringLiteral("request-relaunch");
    }
    return QStringLiteral("unknown");
}

void logElevationResult(const wimforge::startup::ElevationResult &result)
{
    const bool failed = result.action == wimforge::startup::ElevationAction::Failed;
    const bool cancelled = failed
        && result.nativeError == static_cast<unsigned long>(ERROR_CANCELLED);
    wimforge::StructuredLogger::instance().log(
        failed ? wimforge::LogSeverity::Error : wimforge::LogSeverity::Info,
        QStringLiteral("startup"), QStringLiteral("startup.elevation.completed"),
        failed
            ? QStringLiteral("Administrator elevation failed. / 系統管理員權限提升失敗。")
            : QStringLiteral("Administrator elevation check completed. / 系統管理員權限檢查完成。"),
        QJsonObject{
            {QStringLiteral("action"), elevationActionName(result.action)},
            {QStringLiteral("cancelled"), cancelled},
            {QStringLiteral("failureStage"),
             elevationFailureStageName(result.failureStage)},
            {QStringLiteral("nativeError"),
             static_cast<qint64>(result.nativeError)},
        });
}
#endif

bool isCliInvocation(int argc, char *argv[])
{
    const QStringList commands{
        QStringLiteral("help"), QStringLiteral("project"), QStringLiteral("config"),
        QStringLiteral("plan"), QStringLiteral("dry-run"), QStringLiteral("apply"),
        QStringLiteral("history"), QStringLiteral("notification"),
        QStringLiteral("notifications"), QStringLiteral("unattend"),
        QStringLiteral("package"), QStringLiteral("packages"),
        QStringLiteral("package-manager"), QStringLiteral("gpo"),
        QStringLiteral("bundle"), QStringLiteral("action-history"),
        QStringLiteral("winforge"), QStringLiteral("vm"), QStringLiteral("vm-lab"),
    };
    bool explicitCliConfiguration = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QStringLiteral("--cli"))
            return true;
        if (argument == QStringLiteral("--demo"))
            return false;
        if (argument == QStringLiteral("--page") || argument == QStringLiteral("--language"))
            return false;
        if (argument == QStringLiteral("--project")) {
            if (index + 1 < argc)
                ++index;
            continue;
        }
        if (argument == QStringLiteral("--config")) {
            explicitCliConfiguration = true;
            if (index + 1 < argc)
                ++index;
            continue;
        }
        if (argument.startsWith(QStringLiteral("--config="))) {
            explicitCliConfiguration = true;
            continue;
        }
        if (argument.startsWith(QLatin1Char('-')))
            continue;
        if (argument.startsWith(QLatin1Char('@'))
            || commands.contains(argument.toLower()))
            return true;
        // Only the first positional token is a command. Option values such as
        // a project folder named "vm" are consumed above and never reclassified.
        return explicitCliConfiguration;
    }
    return explicitCliConfiguration;
}

void writeCliOutput(const QString &standardOutput, const QString &standardError)
{
#ifdef Q_OS_WIN
    if (::GetConsoleWindow() == nullptr)
        ::AttachConsole(ATTACH_PARENT_PROCESS);
    FILE *stream = nullptr;
    if (stdout && _fileno(stdout) < 0)
        freopen_s(&stream, "CONOUT$", "w", stdout);
    if (stderr && _fileno(stderr) < 0)
        freopen_s(&stream, "CONOUT$", "w", stderr);
#endif
    if (!standardOutput.isEmpty()) {
        QTextStream output(stdout);
        output << standardOutput;
        output.flush();
    }
    if (!standardError.isEmpty()) {
        QTextStream error(stderr);
        error << standardError;
        error.flush();
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("WimForge"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("github.com/codingmachineedge"));
    QCoreApplication::setApplicationName(QStringLiteral("WimForge"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(WIMFORGE_VERSION));

#if defined(Q_OS_WIN) && !defined(WIMFORGE_DOCUMENTATION_CAPTURE)
    const wimforge::startup::ElevationResult elevation =
        wimforge::startup::ensureElevated();
    if (elevation.action != wimforge::startup::ElevationAction::Continue) {
        // The unelevated process must not create the full GUI, but it still
        // creates a minimal Core application after the UAC result so that the
        // result is captured in the same rotating JSONL log.  Its argv is
        // intentionally synthetic: project paths and arbitrary command values
        // never enter this pre-controller logging session.
        int startupArgumentCount = 1;
        char startupName[] = "WimForge";
        char *startupArguments[]{startupName, nullptr};
        QCoreApplication startupApplication(startupArgumentCount,
                                            startupArguments);
        ApplicationLogSession startupLog(QStringLiteral("startup-elevation"));
        logElevationResult(elevation);
        const int startupExitCode =
            elevation.action == wimforge::startup::ElevationAction::Relaunched
            ? 0
            : (elevation.nativeError == 0
                   ? 1
                   : static_cast<int>(elevation.nativeError));
        startupLog.setExitCode(startupExitCode);
        if (elevation.action == wimforge::startup::ElevationAction::Relaunched)
            return startupExitCode;

        const QString detail = elevation.nativeError == ERROR_CANCELLED
            ? QStringLiteral(
                  "Administrator access was cancelled. WimForge cannot safely service Windows images without elevation.\n\n"
                  "你取消咗系統管理員權限要求。WimForge 未提升權限就唔可以安全噉維護 Windows 映像。")
            : QStringLiteral(
                  "WimForge could not request administrator access (Windows error %1).\n\n"
                  "WimForge 申請唔到系統管理員權限（Windows 錯誤 %1）。")
                  .arg(static_cast<qulonglong>(elevation.nativeError));
        ::MessageBoxW(nullptr, reinterpret_cast<const wchar_t *>(detail.utf16()),
                      L"WimForge requires administrator access / WimForge 需要系統管理員權限",
                      MB_OK | MB_ICONERROR);
        return startupExitCode;
    }
#endif

    if (isCliInvocation(argc, argv)) {
        QCoreApplication application(argc, argv);
        ApplicationLogSession logSession(QStringLiteral("cli"));
#if defined(Q_OS_WIN) && !defined(WIMFORGE_DOCUMENTATION_CAPTURE)
        logElevationResult(elevation);
#endif
        QStringList arguments = application.arguments().mid(1);
        arguments.removeAll(QStringLiteral("--cli"));
        const wimforge::CliResult result = wimforge::CliRunner().run(arguments);
        logSession.setExitCode(result.exitCode());
        writeCliOutput(result.standardOutput, result.standardError);
        return result.exitCode();
    }

    QQuickStyle::setStyle(QStringLiteral("Material"));

    QGuiApplication application(argc, argv);
    ApplicationLogSession logSession(QStringLiteral("gui"));
#if defined(Q_OS_WIN) && !defined(WIMFORGE_DOCUMENTATION_CAPTURE)
    logElevationResult(elevation);
#endif
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Open-source, Git-backed Windows image customization studio."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("project"), QStringLiteral("Open a project folder."), QStringLiteral("folder")});
    parser.addOption({QStringLiteral("demo"), QStringLiteral("Open a safe populated demo project for screenshots and evaluation.")});
    parser.addOption({QStringLiteral("language"), QStringLiteral("UI language: en, zh-HK, or bilingual."), QStringLiteral("mode")});
    parser.addOption({QStringLiteral("page"), QStringLiteral("Open a studio page: overview, source, customize, gpo, unattended, packages, winforge, vmlab, plan, history, settings, or terminal."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a PNG of the selected page after startup, then exit."), QStringLiteral("path")});
#ifdef WIMFORGE_DOCUMENTATION_CAPTURE
    parser.addOption({
        QStringLiteral("project-start"),
        QStringLiteral(
            "Capture the empty, privacy-safe project start page. / 擷取無私人路徑嘅空白工程起始頁。")});
    parser.addOption({
        QStringLiteral("customize-section"),
        QStringLiteral(
            "Open a Customize workbench for visual QA: updates, drivers, features, apps, components, settings, unattended, or post-setup. / 開指定調校工作台做畫面檢查。"),
        QStringLiteral("id")});
#endif
    parser.process(application);

    bool projectStartCapture = false;
    int startupCustomizeSection = 0;
#ifdef WIMFORGE_DOCUMENTATION_CAPTURE
    projectStartCapture = parser.isSet(QStringLiteral("project-start"));
    const bool demoCapture = parser.isSet(QStringLiteral("demo"));
    if (!parser.isSet(QStringLiteral("screenshot"))
        || demoCapture == projectStartCapture
        || parser.isSet(QStringLiteral("project"))) {
        qCritical().noquote()
            << QStringLiteral(
                   "The documentation-capture build accepts --screenshot with exactly one of --demo or --project-start, and never --project. / 文件擷取版本只接受 --screenshot 配搭 --demo 或 --project-start 其中一個，而且唔會接受 --project。");
        logSession.setExitCode(5);
        return 5;
    }
    if (parser.isSet(QStringLiteral("customize-section"))) {
        const QStringList customizeSections{
            QStringLiteral("updates"), QStringLiteral("drivers"),
            QStringLiteral("features"), QStringLiteral("apps"),
            QStringLiteral("components"), QStringLiteral("settings"),
            QStringLiteral("unattended"), QStringLiteral("post-setup"),
        };
        startupCustomizeSection = customizeSections.indexOf(
            parser.value(QStringLiteral("customize-section")).trimmed().toLower());
        if (startupCustomizeSection < 0) {
            qCritical().noquote() << QStringLiteral(
                "Unknown Customize section for documentation capture. / 文件擷取指定咗未知嘅調校工作台。");
            logSession.setExitCode(6);
            return 6;
        }
    }
#endif

    AppController controller;
    wimforge::EmbeddedTerminalSession terminalSession;
    if (parser.isSet(QStringLiteral("demo"))) {
        QString error;
        if (!controller.loadDemoProject(&error))
            qWarning().noquote() << error;
    } else if (parser.isSet(QStringLiteral("project"))) {
        controller.openProject(parser.value(QStringLiteral("project")));
    }
    const QString language = parser.value(QStringLiteral("language")).toLower();
    if (language == QStringLiteral("en")) controller.setLanguageMode(0);
    else if (language == QStringLiteral("zh-hk") || language == QStringLiteral("yue")) controller.setLanguageMode(1);
    else if (language == QStringLiteral("bilingual")) controller.setLanguageMode(2);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("terminalSession"),
                                             &terminalSession);
    // Main.qml uses this only in the documentation-capture build to replace
    // recent-project data with an empty model.  Normal launches always receive
    // false and keep their existing project startup behaviour unchanged.
    engine.rootContext()->setContextProperty(QStringLiteral("projectStartCapture"),
                                             projectStartCapture);
    engine.rootContext()->setContextProperty(QStringLiteral("startupCustomizeSection"),
                                             startupCustomizeSection);
    const QStringList pageIds{QStringLiteral("overview"), QStringLiteral("source"),
        QStringLiteral("customize"), QStringLiteral("gpo"), QStringLiteral("unattended"),
        QStringLiteral("packages"), QStringLiteral("winforge"), QStringLiteral("vmlab"),
        QStringLiteral("plan"), QStringLiteral("history"), QStringLiteral("settings"),
        QStringLiteral("terminal")};
    QString requestedPageId = parser.value(QStringLiteral("page")).toLower();
    if (requestedPageId == QStringLiteral("vm-lab"))
        requestedPageId = QStringLiteral("vmlab");
    const int requestedPage = qMax(0, pageIds.indexOf(requestedPageId));
    engine.rootContext()->setContextProperty(QStringLiteral("startupPage"), requestedPage);
    engine.rootContext()->setContextProperty(QStringLiteral("startupPageRequested"),
                                             parser.isSet(QStringLiteral("page")));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &application, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("WimForge"), QStringLiteral("Main"));

    if (parser.isSet(QStringLiteral("screenshot"))) {
        const QString screenshotPath = QFileInfo(parser.value(QStringLiteral("screenshot")))
                                           .absoluteFilePath();
        const auto rootObjects = engine.rootObjects();
        auto *window = rootObjects.isEmpty()
            ? nullptr
            : qobject_cast<QQuickWindow *>(rootObjects.constFirst());
        if (!window) {
            qCritical().noquote() << QStringLiteral("Unable to capture the documentation screenshot: the root window is unavailable.");
            return 2;
        }

        QTimer::singleShot(1500, &application,
                           [&application, window, screenshotPath] {
            if (!QDir().mkpath(QFileInfo(screenshotPath).absolutePath())) {
                qCritical().noquote() << QStringLiteral("Unable to create the screenshot output directory: %1")
                                             .arg(QFileInfo(screenshotPath).absolutePath());
                application.exit(3);
                return;
            }
            QImage image = window->grabWindow();
            const QSize viewportSize = window->size();
            // grabWindow() returns native pixels on a high-DPI display while
            // QQuickWindow::size() is expressed in device-independent pixels.
            // Cropping the native frame would keep only its top-left portion;
            // normalize the complete frame to the documented logical viewport.
            if (!image.isNull() && image.size() != viewportSize)
                image = image.scaled(viewportSize, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
            image.setDevicePixelRatio(1.0);
            if (image.isNull() || !image.save(screenshotPath, "PNG")) {
                qCritical().noquote() << QStringLiteral("Unable to save the documentation screenshot: %1")
                                             .arg(screenshotPath);
                application.exit(4);
                return;
            }
            application.quit();
        });
    }
    const int exitCode = application.exec();
    logSession.setExitCode(exitCode);
    return exitCode;
}
