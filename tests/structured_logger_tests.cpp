#include "core/StructuredLogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
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
            QTextStream(stdout) << "structured_logger_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

QList<QJsonObject> readRecords(const QString &path, TestRun &test)
{
    QList<QJsonObject> records;
    QFile file(path);
    test.check(file.open(QIODevice::ReadOnly),
               QStringLiteral("log file opens: %1").arg(path));
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        test.check(parseError.error == QJsonParseError::NoError && document.isObject(),
                   QStringLiteral("every JSONL line is an object: %1")
                       .arg(parseError.errorString()));
        if (document.isObject())
            records.append(document.object());
    }
    return records;
}

void testSchemaAndRedaction(TestRun &test, const QString &root)
{
    StructuredLogger &logger = StructuredLogger::instance();
    StructuredLoggerOptions options;
    options.directory = QDir(root).filePath(QStringLiteral("schema"));
    options.maximumFileBytes = 1024 * 1024;
    options.archiveCount = 2;
    QString error;
    test.check(logger.initialize(options, &error),
               QStringLiteral("logger initializes: %1").arg(error));
    logger.installQtMessageHandler();
    logger.log(LogSeverity::Warning, QStringLiteral("test.controller"),
               QStringLiteral("test.redaction"),
               QStringLiteral("token=plain-secret Bearer bearer-secret"),
               QJsonObject{
                   {QStringLiteral("password"), QStringLiteral("hunter2")},
                   {QStringLiteral("nested"), QJsonObject{
                        {QStringLiteral("apiKey"), QStringLiteral("key-value")},
                        {QStringLiteral("safe"), QStringLiteral("keep-me")},
                    }},
                   {QStringLiteral("url"),
                    QStringLiteral("https://user:password@example.invalid/path")},
                   {QStringLiteral("arguments"), QJsonArray{
                        QStringLiteral("--token"), QStringLiteral("adjacent-secret"),
                        QStringLiteral("--safe"), QStringLiteral("keep-me-too")}},
               });
    qWarning().noquote() << QStringLiteral(
        R"msg(Qt warning before "C:\Users\SENTINEL_PRIVATE_USER\Secret Folder\opaque.bin" and \\server\share\SENTINEL_UNC_PATH\file.dat and \\?\C:\SENTINEL_DEVICE_PATH\disk.vhd and file:///C:/Users/SENTINEL_FILE_URL/item.iso; ordinary tail survives with https://example.invalid/docs and password=qt-secret.)msg");
    const QString path = logger.logPath();
    logger.shutdown(QJsonObject{{QStringLiteral("reason"), QStringLiteral("test")}});

    const QList<QJsonObject> records = readRecords(path, test);
    test.check(records.size() >= 4, QStringLiteral("startup, direct, Qt, and shutdown records exist"));
    bool foundDirect = false;
    bool foundQt = false;
    for (const QJsonObject &record : records) {
        test.check(record.value(QStringLiteral("schema")).toString()
                       == QStringLiteral("wimforge.log"),
                   QStringLiteral("record has schema"));
        test.check(!record.value(QStringLiteral("timestamp")).toString().isEmpty()
                       && !record.value(QStringLiteral("severity")).toString().isEmpty()
                       && !record.value(QStringLiteral("category")).toString().isEmpty()
                       && record.value(QStringLiteral("thread")).isObject()
                       && record.value(QStringLiteral("source")).isObject(),
                   QStringLiteral("record has timestamp/severity/category/thread/source"));
        const QString sourceFile = record.value(QStringLiteral("source")).toObject()
                                       .value(QStringLiteral("file")).toString();
        test.check(!sourceFile.contains(QLatin1Char('/'))
                       && !sourceFile.contains(QLatin1Char('\\')),
                   QStringLiteral("source metadata omits build/workspace paths"));
        const QByteArray encoded = QJsonDocument(record).toJson(QJsonDocument::Compact);
        test.check(!encoded.contains("plain-secret") && !encoded.contains("bearer-secret")
                       && !encoded.contains("hunter2") && !encoded.contains("key-value")
                       && !encoded.contains("qt-secret") && !encoded.contains(":password@")
                       && !encoded.contains("adjacent-secret")
                       && !encoded.contains("SENTINEL_PRIVATE_USER")
                       && !encoded.contains("SENTINEL_UNC_PATH")
                       && !encoded.contains("SENTINEL_DEVICE_PATH")
                       && !encoded.contains("SENTINEL_FILE_URL"),
                   QStringLiteral("secret-like values never reach disk"));
        if (record.value(QStringLiteral("event")).toString()
            == QStringLiteral("test.redaction")) {
            foundDirect = true;
            const QJsonObject data = record.value(QStringLiteral("data")).toObject();
            test.check(data.value(QStringLiteral("password")).toString()
                           == QStringLiteral("[REDACTED]")
                           && data.value(QStringLiteral("nested")).toObject()
                                  .value(QStringLiteral("safe")).toString()
                                  == QStringLiteral("keep-me"),
                       QStringLiteral("recursive key redaction preserves safe data"));
        }
        if (record.value(QStringLiteral("event")).toString()
            == QStringLiteral("qt.message")) {
            foundQt = true;
            const QString message = record.value(QStringLiteral("message")).toString();
            test.check(message.count(QStringLiteral("[PATH]")) >= 4
                           && message.contains(QStringLiteral("ordinary tail survives"))
                           && message.contains(QStringLiteral("https://example.invalid/docs")),
                       QStringLiteral("Qt path redaction preserves surrounding ordinary prose and HTTPS URLs"));
        }
    }
    test.check(foundDirect, QStringLiteral("direct structured event is present"));
    test.check(foundQt, QStringLiteral("Qt message handler event is present"));
}

void testRotation(TestRun &test, const QString &root)
{
    StructuredLogger &logger = StructuredLogger::instance();
    StructuredLoggerOptions options;
    options.directory = QDir(root).filePath(QStringLiteral("rotation"));
    options.maximumFileBytes = 1024;
    options.archiveCount = 2;
    QString error;
    test.check(logger.initialize(options, &error),
               QStringLiteral("rotation logger initializes: %1").arg(error));
    const QString payload(500, QLatin1Char('x'));
    for (int index = 0; index < 20; ++index) {
        logger.log(LogSeverity::Debug, QStringLiteral("rotation"),
                   QStringLiteral("rotation.record"), payload,
                   QJsonObject{{QStringLiteral("index"), index}});
    }
    const QString current = logger.logPath();
    logger.shutdown();

    const QFileInfo info(current);
    const QString first = QDir(info.absolutePath()).filePath(QStringLiteral("wimforge.1.jsonl"));
    const QString second = QDir(info.absolutePath()).filePath(QStringLiteral("wimforge.2.jsonl"));
    const QString overflow = QDir(info.absolutePath()).filePath(QStringLiteral("wimforge.3.jsonl"));
    test.check(QFileInfo::exists(current) && QFileInfo::exists(first)
                   && QFileInfo::exists(second),
               QStringLiteral("current log and bounded archives exist"));
    test.check(!QFileInfo::exists(overflow),
               QStringLiteral("rotation never exceeds configured archive count"));
    static_cast<void>(readRecords(current, test));
    static_cast<void>(readRecords(first, test));
    static_cast<void>(readRecords(second, test));
}

int runInvocationPrivacyProbe()
{
    const QString directory =
        qEnvironmentVariable("WIMFORGE_STRUCTURED_LOGGER_PROBE_DIR");
    if (directory.isEmpty())
        return 2;
    StructuredLoggerOptions options;
    options.directory = directory;
    QString error;
    StructuredLogger &logger = StructuredLogger::instance();
    if (!logger.initialize(options, &error))
        return 3;
    logger.log(LogSeverity::Info, QStringLiteral("privacy-probe"),
               QStringLiteral("privacy-probe.completed"),
               QStringLiteral("Invocation privacy probe completed."));
    logger.shutdown();
    return 0;
}

void testInvocationPrivacy(TestRun &test, const QString &root)
{
    const QString probeDirectory =
        QDir(root).filePath(QStringLiteral("invocation-privacy"));
    const QString sentinelPath = QStringLiteral(
        "C:\\Users\\SENTINEL_PRIVATE_PERSON\\ARG_PATH_SENTINEL");
    const QStringList probeArguments{
        QStringLiteral("--invocation-privacy-probe"),
        QStringLiteral("--project"), sentinelPath,
        QStringLiteral("--token=ARG_TOKEN_SENTINEL"),
        QStringLiteral("ARG_POSITIONAL_COMMAND_SENTINEL"),
        QStringLiteral("--language"), QStringLiteral("ARG_OPTION_VALUE_SENTINEL"),
        QStringLiteral("--config=C:\\ARG_CONFIG_PATH_SENTINEL\\project.json"),
        QStringLiteral("@C:\\ARG_RESPONSE_PATH_SENTINEL\\request.rsp"),
    };

    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(probeArguments);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WIMFORGE_STRUCTURED_LOGGER_PROBE_DIR"),
                       probeDirectory);
    process.setProcessEnvironment(environment);
    process.start();
    const bool finished = process.waitForFinished(30'000);
    test.check(finished && process.exitStatus() == QProcess::NormalExit
                   && process.exitCode() == 0,
               QStringLiteral("argv privacy probe completes: %1")
                   .arg(QString::fromLocal8Bit(process.readAllStandardError())));

    const QString path = QDir(probeDirectory).filePath(
        QStringLiteral("wimforge.jsonl"));
    QFile file(path);
    test.check(file.open(QIODevice::ReadOnly),
               QStringLiteral("argv privacy probe log opens"));
    const QByteArray encoded = file.readAll();
    file.close();
    const QList<QByteArray> forbidden{
        QByteArrayLiteral("--invocation-privacy-probe"),
        QByteArrayLiteral("--project"),
        QByteArrayLiteral("--language"),
        QByteArrayLiteral("--token="),
        QByteArrayLiteral("SENTINEL_PRIVATE_PERSON"),
        QByteArrayLiteral("ARG_PATH_SENTINEL"),
        QByteArrayLiteral("ARG_TOKEN_SENTINEL"),
        QByteArrayLiteral("ARG_POSITIONAL_COMMAND_SENTINEL"),
        QByteArrayLiteral("ARG_OPTION_VALUE_SENTINEL"),
        QByteArrayLiteral("ARG_CONFIG_PATH_SENTINEL"),
        QByteArrayLiteral("ARG_RESPONSE_PATH_SENTINEL"),
    };
    for (const QByteArray &sentinel : forbidden) {
        test.check(!encoded.contains(sentinel),
                   QStringLiteral("startup JSONL omits argv sentinel %1")
                       .arg(QString::fromLatin1(sentinel)));
    }

    const QList<QJsonObject> records = readRecords(path, test);
    bool foundSession = false;
    for (const QJsonObject &record : records) {
        if (record.value(QStringLiteral("event")).toString()
            != QStringLiteral("session.started")) {
            continue;
        }
        foundSession = true;
        const QJsonObject data = record.value(QStringLiteral("data")).toObject();
        const QJsonObject invocation =
            data.value(QStringLiteral("invocation")).toObject();
        test.check(!data.contains(QStringLiteral("arguments"))
                       && invocation.value(QStringLiteral("argumentCount")).toInt()
                              == probeArguments.size()
                       && invocation.value(QStringLiteral("projectOptionPresent")).toBool()
                       && invocation.value(QStringLiteral("configOptionPresent")).toBool()
                       && invocation.value(QStringLiteral("languageOptionPresent")).toBool(),
                   QStringLiteral("session stores only allowlisted invocation metadata"));
    }
    test.check(foundSession, QStringLiteral("argv privacy session record exists"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("StructuredLoggerTests"));
    if (application.arguments().contains(
            QStringLiteral("--invocation-privacy-probe"))) {
        return runInvocationPrivacyProbe();
    }
    QTemporaryDir temporary;
    TestRun test;
    test.check(temporary.isValid(), QStringLiteral("temporary directory is available"));
    if (temporary.isValid()) {
        testSchemaAndRedaction(test, temporary.path());
        testRotation(test, temporary.path());
        testInvocationPrivacy(test, temporary.path());
    }
    return test.result();
}
