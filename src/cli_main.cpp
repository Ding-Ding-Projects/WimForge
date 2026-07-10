#include "cli/CliRunner.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("WimForge"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("github.com/codingmachineedge"));
    QCoreApplication::setApplicationName(QStringLiteral("WimForgeCli"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(WIMFORGE_VERSION));
    QCoreApplication application(argc, argv);

    const wimforge::CliResult result = wimforge::CliRunner().run(application.arguments().mid(1));
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
