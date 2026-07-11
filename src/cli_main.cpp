#include "cli/CliRunner.h"
#include "core/StructuredLogger.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QTextStream>

namespace {

class CliLogSession final
{
public:
    CliLogSession()
    {
        QString error;
        m_active = wimforge::StructuredLogger::instance().initialize({}, &error);
        if (!m_active) {
            QTextStream(stderr)
                << "WimForge CLI logging could not start / WimForge CLI 啟動唔到記錄功能: "
                << error << '\n';
            return;
        }
        wimforge::StructuredLogger::instance().installQtMessageHandler();
        wimforge::StructuredLogger::instance().log(
            wimforge::LogSeverity::Info, QStringLiteral("application"),
            QStringLiteral("application.started"),
            QStringLiteral("WimForge companion CLI process initialized. / WimForge 配套 CLI 程序已經啟動。"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("companion-cli")}});
    }

    ~CliLogSession()
    {
        if (!m_active)
            return;
        const QJsonObject data{{QStringLiteral("mode"), QStringLiteral("companion-cli")},
                               {QStringLiteral("exitCode"), m_exitCode}};
        wimforge::StructuredLogger::instance().log(
            wimforge::LogSeverity::Info, QStringLiteral("application"),
            QStringLiteral("application.stopping"),
            QStringLiteral("WimForge companion CLI process is stopping. / WimForge 配套 CLI 程序而家停止。"), data);
        wimforge::StructuredLogger::instance().shutdown(data);
    }

    void setExitCode(int exitCode) { m_exitCode = exitCode; }

private:
    int m_exitCode = -1;
    bool m_active = false;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("WimForge"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("github.com/codingmachineedge"));
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeCli"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(WIMFORGE_VERSION));
    QCoreApplication application(argc, argv);
    CliLogSession logSession;

    const wimforge::CliResult result = wimforge::CliRunner().run(application.arguments().mid(1));
    logSession.setExitCode(result.exitCode());
    if (!result.standardOutput.isEmpty()) {
        QTextStream output(stdout);
        output << result.standardOutput;
        output.flush();
    }
    if (!result.standardError.isEmpty()) {
        QTextStream error(stderr);
        error << result.standardError;
        error.flush();
    }
    return result.exitCode();
}
