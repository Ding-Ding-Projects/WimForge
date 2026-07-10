#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextStream>

#ifndef WIMFORGE_SOURCE_DIR
#error WIMFORGE_SOURCE_DIR must point at the WimForge source tree
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

QString readText(const QString &path, TestContext *test)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        test->check(false, QStringLiteral("Could not read %1: %2").arg(path, file.errorString()));
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QSet<QString> captures(const QString &text, const QRegularExpression &expression)
{
    QSet<QString> values;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext())
        values.insert(matches.next().captured(1));
    return values;
}

QString lowerFirst(QString value)
{
    if (!value.isEmpty())
        value[0] = value.at(0).toLower();
    return value;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    TestContext test;
    const QString sourceRoot = QString::fromUtf8(WIMFORGE_SOURCE_DIR);
    const QString controllerHeader = readText(
        sourceRoot + QStringLiteral("/src/AppController.h"), &test);

    const QRegularExpression propertyExpression(
        QStringLiteral(R"(Q_PROPERTY\([^\n\)]*\b([A-Za-z_][A-Za-z0-9_]*)\s+READ\b)"));
    const QRegularExpression writablePropertyExpression(
        QStringLiteral(R"(Q_PROPERTY\([^\n\)]*\b([A-Za-z_][A-Za-z0-9_]*)\s+READ\b[^\n\)]*\bWRITE\b)"));
    const QRegularExpression invokableExpression(
        QStringLiteral(R"(Q_INVOKABLE\s+[^;\n\(]*\b([A-Za-z_][A-Za-z0-9_]*)\s*\()"));

    QSet<QString> readableApi = captures(controllerHeader, propertyExpression);
    const QSet<QString> writableApi = captures(controllerHeader, writablePropertyExpression);
    readableApi.unite(captures(controllerHeader, invokableExpression));

    const qsizetype signalsStart = controllerHeader.indexOf(QStringLiteral("signals:"));
    const qsizetype signalsEnd = controllerHeader.indexOf(QStringLiteral("private:"), signalsStart);
    test.check(signalsStart >= 0 && signalsEnd > signalsStart,
               QStringLiteral("AppController signals section could not be parsed"));
    const QString signalsSection = signalsStart >= 0 && signalsEnd > signalsStart
        ? controllerHeader.mid(signalsStart, signalsEnd - signalsStart)
        : QString();
    const QRegularExpression signalExpression(
        QStringLiteral(R"(^\s*void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"),
        QRegularExpression::MultilineOption);
    const QSet<QString> controllerSignals = captures(signalsSection, signalExpression);

    const QRegularExpression appReferenceExpression(
        QStringLiteral(R"(\bapp\.([A-Za-z_][A-Za-z0-9_]*))"));
    const QRegularExpression appAssignmentExpression(
        QStringLiteral(R"(\bapp\.([A-Za-z_][A-Za-z0-9_]*)\s*=(?!=))"));
    const QRegularExpression signalHandlerExpression(
        QStringLiteral(R"(\bfunction\s+on([A-Z][A-Za-z0-9_]*)\s*\()"));

    int qmlFileCount = 0;
    QDirIterator iterator(sourceRoot + QStringLiteral("/qml"),
                          {QStringLiteral("*.qml")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString text = readText(path, &test);
        ++qmlFileCount;

        for (const QString &name : captures(text, appReferenceExpression)) {
            test.check(readableApi.contains(name),
                       QStringLiteral("%1 references unknown AppController member app.%2")
                           .arg(QDir(sourceRoot).relativeFilePath(path), name));
        }
        for (const QString &name : captures(text, appAssignmentExpression)) {
            test.check(writableApi.contains(name),
                       QStringLiteral("%1 assigns read-only or unknown AppController property app.%2")
                           .arg(QDir(sourceRoot).relativeFilePath(path), name));
        }
        for (const QString &handler : captures(text, signalHandlerExpression)) {
            const QString signal = lowerFirst(handler);
            test.check(controllerSignals.contains(signal),
                       QStringLiteral("%1 handles unknown AppController signal %2")
                           .arg(QDir(sourceRoot).relativeFilePath(path), signal));
        }
    }

    test.check(qmlFileCount >= 15,
               QStringLiteral("Expected to scan the complete QML module, found only %1 files")
                   .arg(qmlFileCount));
    if (test.failures() == 0)
        QTextStream(stdout) << "QML/AppController contract is valid across " << qmlFileCount
                            << " files.\n";
    return test.failures() == 0 ? 0 : 1;
}
