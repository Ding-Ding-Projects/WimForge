#include "StructuredLogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <algorithm>

namespace wimforge {
namespace {

constexpr auto redacted = "[REDACTED]";

QString defaultLogDirectory()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty())
        root = QDir::home().filePath(QStringLiteral(".wimforge"));
    return QDir(root).filePath(QStringLiteral("logs"));
}

QString currentThreadId()
{
    const auto id = reinterpret_cast<quintptr>(QThread::currentThreadId());
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(id), 0, 16);
}

QString sourceBaseName(const QString &path)
{
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return normalized.section(QLatin1Char('/'), -1);
}

LogSeverity severityForQtMessage(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return LogSeverity::Debug;
    case QtInfoMsg: return LogSeverity::Info;
    case QtWarningMsg: return LogSeverity::Warning;
    case QtCriticalMsg: return LogSeverity::Error;
    case QtFatalMsg: return LogSeverity::Critical;
    }
    return LogSeverity::Error;
}

bool isSensitiveOption(const QString &argument)
{
    static const QRegularExpression sensitiveOption(
        QStringLiteral(R"(^[-/]+(?:password|passwd|passphrase|secret|token|api[-_]?key|authorization|credential|cookie|product[-_]?key|license[-_]?key)$)"),
        QRegularExpression::CaseInsensitiveOption);
    return sensitiveOption.match(argument).hasMatch();
}

bool invocationOptionPresent(const QStringList &arguments,
                             const QString &option)
{
    const QString assignmentPrefix = option + QLatin1Char('=');
    return std::any_of(arguments.cbegin(), arguments.cend(),
                       [&option, &assignmentPrefix](const QString &argument) {
        return argument.compare(option, Qt::CaseInsensitive) == 0
            || argument.startsWith(assignmentPrefix, Qt::CaseInsensitive);
    });
}

QJsonObject safeInvocationMetadata()
{
    // Startup logging must never become an argv recorder.  Only counts and a
    // fixed allowlist of boolean mode indicators are retained; paths, command
    // names, option values, response-file values, and arbitrary tokens are
    // omitted in their entirety rather than relying on pattern redaction.
    const QStringList arguments = QCoreApplication::arguments().mid(1);
    const auto present = [&arguments](const QString &option) {
        return invocationOptionPresent(arguments, option);
    };
    const bool helpRequested = present(QStringLiteral("--help"))
        || present(QStringLiteral("-h")) || present(QStringLiteral("/?"));
    const bool versionRequested = present(QStringLiteral("--version"))
        || present(QStringLiteral("-v"));
    return QJsonObject{
        {QStringLiteral("argumentCount"), arguments.size()},
        {QStringLiteral("cliRequested"), present(QStringLiteral("--cli"))},
        {QStringLiteral("demoRequested"), present(QStringLiteral("--demo"))},
        {QStringLiteral("projectStartRequested"),
         present(QStringLiteral("--project-start"))},
        {QStringLiteral("screenshotRequested"),
         present(QStringLiteral("--screenshot"))},
        {QStringLiteral("projectOptionPresent"),
         present(QStringLiteral("--project"))},
        {QStringLiteral("configOptionPresent"),
         present(QStringLiteral("--config"))},
        {QStringLiteral("pageOptionPresent"), present(QStringLiteral("--page"))},
        {QStringLiteral("languageOptionPresent"),
         present(QStringLiteral("--language"))},
        {QStringLiteral("helpRequested"), helpRequested},
        {QStringLiteral("versionRequested"), versionRequested},
    };
}

} // namespace

StructuredLogger &StructuredLogger::instance()
{
    static StructuredLogger logger;
    return logger;
}

StructuredLogger::~StructuredLogger()
{
    // ApplicationLogSession performs the orderly shutdown while Qt and all
    // function-local helpers are still alive.  Do not attempt to serialize a
    // final record from static destruction: the redaction helpers may already
    // have been destroyed by then, and using them would be undefined behavior.
}

bool StructuredLogger::initialize(const StructuredLoggerOptions &options, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (m_initialized) {
        if (error)
            error->clear();
        return true;
    }

    const QString directory = options.directory.trimmed().isEmpty()
        ? defaultLogDirectory() : QDir::cleanPath(options.directory);
    const QString fileName = options.fileName.trimmed().isEmpty()
        ? QStringLiteral("wimforge.jsonl") : QFileInfo(options.fileName).fileName();
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("Could not create the application log folder: %1")
                         .arg(directory);
        return false;
    }

    m_logPath = QDir(directory).filePath(fileName);
    m_maximumFileBytes = std::max<qint64>(1024, options.maximumFileBytes);
    m_archiveCount = std::clamp(options.archiveCount, 1, 100);
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sequence = 0;
    m_initialized = true;

    const QJsonObject data{
        {QStringLiteral("application"), QCoreApplication::applicationName()},
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("invocation"), safeInvocationMetadata()},
    };
    QString writeError;
    if (!writeRecordLocked(LogSeverity::Info, QStringLiteral("application"),
                           QStringLiteral("session.started"),
                           QStringLiteral("WimForge logging session started. / WimForge 記錄工作階段已經開始。"), data,
                           QStringLiteral("StructuredLogger.cpp"),
                           QStringLiteral("StructuredLogger::initialize"), 0,
                           &writeError)) {
        m_initialized = false;
        if (error)
            *error = writeError;
        return false;
    }
    if (error)
        error->clear();
    return true;
}

void StructuredLogger::installQtMessageHandler()
{
    QMutexLocker lock(&m_mutex);
    if (m_handlerInstalled)
        return;
    m_previousHandler = qInstallMessageHandler(&StructuredLogger::qtMessageHandler);
    m_handlerInstalled = true;
}

void StructuredLogger::shutdown(const QJsonObject &data)
{
    QtMessageHandler previous = nullptr;
    bool handlerWasInstalled = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_handlerInstalled) {
            previous = m_previousHandler;
            m_previousHandler = nullptr;
            m_handlerInstalled = false;
            handlerWasInstalled = true;
        }
    }
    if (handlerWasInstalled)
        qInstallMessageHandler(previous);

    QMutexLocker lock(&m_mutex);
    if (!m_initialized)
        return;
    static_cast<void>(writeRecordLocked(
        LogSeverity::Info, QStringLiteral("application"),
        QStringLiteral("session.stopped"),
        QStringLiteral("WimForge logging session stopped. / WimForge 記錄工作階段已經停止。"), data,
        QStringLiteral("StructuredLogger.cpp"),
        QStringLiteral("StructuredLogger::shutdown"), 0));
    m_initialized = false;
}

void StructuredLogger::log(LogSeverity severity,
                           const QString &category,
                           const QString &event,
                           const QString &message,
                           const QJsonObject &data,
                           const std::source_location &source)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_initialized) {
            static_cast<void>(writeRecordLocked(
                severity, category, event, message, data,
                QString::fromUtf8(source.file_name()),
                QString::fromUtf8(source.function_name()),
                static_cast<int>(source.line())));
            return;
        }
    }
    if (!initialize())
        return;
    QMutexLocker lock(&m_mutex);
    static_cast<void>(writeRecordLocked(
        severity, category, event, message, data,
        QString::fromUtf8(source.file_name()),
        QString::fromUtf8(source.function_name()),
        static_cast<int>(source.line())));
}

QString StructuredLogger::logPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_logPath;
}

QString StructuredLogger::logDirectory() const
{
    QMutexLocker lock(&m_mutex);
    return QFileInfo(m_logPath).absolutePath();
}

QString StructuredLogger::sessionId() const
{
    QMutexLocker lock(&m_mutex);
    return m_sessionId;
}

QString StructuredLogger::severityName(LogSeverity severity)
{
    switch (severity) {
    case LogSeverity::Trace: return QStringLiteral("trace");
    case LogSeverity::Debug: return QStringLiteral("debug");
    case LogSeverity::Info: return QStringLiteral("info");
    case LogSeverity::Warning: return QStringLiteral("warning");
    case LogSeverity::Error: return QStringLiteral("error");
    case LogSeverity::Critical: return QStringLiteral("critical");
    }
    return QStringLiteral("error");
}

bool StructuredLogger::sensitiveKey(const QString &key)
{
    QString normalized = key.toLower();
    normalized.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (normalized == QStringLiteral("auth")
        || normalized == QStringLiteral("authorization")
        || normalized == QStringLiteral("authentication")) {
        return true;
    }
    const QStringList suffixes{
        QStringLiteral("password"), QStringLiteral("passwd"),
        QStringLiteral("passphrase"), QStringLiteral("secret"),
        QStringLiteral("token"), QStringLiteral("credential"),
        QStringLiteral("cookie"), QStringLiteral("apikey"),
        QStringLiteral("accesskey"), QStringLiteral("privatekey"),
        QStringLiteral("productkey"), QStringLiteral("licensekey"),
        QStringLiteral("connectionstring"),
    };
    return std::any_of(suffixes.cbegin(), suffixes.cend(),
                       [&normalized](const QString &suffix) {
        return normalized.endsWith(suffix);
    });
}

QString StructuredLogger::redactText(const QString &text)
{
    QString result = text;
    static const QRegularExpression assignment(
        QStringLiteral(R"((\b(?:password|passwd|passphrase|secret|token|api[_-]?key|authorization|credential|cookie|product[_-]?key)\b)(\s*(?:=|:)\s*|\s+)(?:"[^"]*"|'[^']*'|[^\s,;]+))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bearer(
        QStringLiteral(R"((\bBearer\s+)[A-Za-z0-9._~+/=-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression knownToken(
        QStringLiteral(R"(\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|sk-[A-Za-z0-9_-]{16,})\b)"));
    static const QRegularExpression urlPassword(
        QStringLiteral(R"((\b[a-z][a-z0-9+.-]*://[^:/\s@]+:)[^@\s/]+(@))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression quotedDoublePath(
        QStringLiteral(R"re("(?:file:(?://+)?|[a-z]:[\\/]|\\\\(?:[?.]\\)?|(?<!:)//)[^"\r\n]*")re"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression quotedSinglePath(
        QStringLiteral(R"re('(?:file:(?://+)?|[a-z]:[\\/]|\\\\(?:[?.]\\)?|(?<!:)//)[^'\r\n]*')re"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression fileUrl(
        QStringLiteral(R"(\bfile:(?://+)?[^\s<>"']+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression windowsDeviceOrUncPath(
        QStringLiteral(R"((?:\\\\(?:[?.]\\)?|\\\?\?\\)[^\s<>"']+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forwardSlashUncPath(
        QStringLiteral(R"((?<!:)//[^/\s<>"']+/[^\s<>"']+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression windowsDrivePath(
        QStringLiteral(R"(\b[a-z]:[\\/][^\s<>"']+)"),
        QRegularExpression::CaseInsensitiveOption);
    result.replace(urlPassword, QStringLiteral("\\1[REDACTED]\\2"));
    // Paths frequently reveal account names, project names, mounted media,
    // or secret-bearing response files. Redact path-shaped spans before the
    // generic credential rules while leaving surrounding prose and HTTP(S)
    // URLs intact. Quoted forms are handled first so paths containing spaces
    // cannot leak a suffix.
    result.replace(quotedDoublePath, QStringLiteral("[PATH]"));
    result.replace(quotedSinglePath, QStringLiteral("[PATH]"));
    result.replace(fileUrl, QStringLiteral("[PATH]"));
    result.replace(windowsDeviceOrUncPath, QStringLiteral("[PATH]"));
    result.replace(forwardSlashUncPath, QStringLiteral("[PATH]"));
    result.replace(windowsDrivePath, QStringLiteral("[PATH]"));
    result.replace(bearer, QStringLiteral("\\1[REDACTED]"));
    result.replace(assignment, QStringLiteral("\\1\\2[REDACTED]"));
    result.replace(knownToken, QString::fromLatin1(redacted));
    return result;
}

QJsonValue StructuredLogger::redactJson(const QJsonValue &value, const QString &key)
{
    if (sensitiveKey(key))
        return QString::fromLatin1(redacted);
    if (value.isString())
        return redactText(value.toString());
    if (value.isArray()) {
        QJsonArray output;
        bool redactNext = false;
        for (const QJsonValue &item : value.toArray()) {
            if (redactNext) {
                output.append(QString::fromLatin1(redacted));
                redactNext = false;
                continue;
            }
            output.append(redactJson(item));
            redactNext = item.isString() && isSensitiveOption(item.toString());
        }
        return output;
    }
    if (value.isObject()) {
        QJsonObject output;
        const QJsonObject input = value.toObject();
        for (auto iterator = input.constBegin(); iterator != input.constEnd(); ++iterator)
            output.insert(iterator.key(), redactJson(iterator.value(), iterator.key()));
        return output;
    }
    return value;
}

void StructuredLogger::qtMessageHandler(QtMsgType type,
                                        const QMessageLogContext &context,
                                        const QString &message)
{
    StructuredLogger &logger = instance();
    logger.logQtMessage(type, context, message);

    QtMessageHandler previous = nullptr;
    {
        QMutexLocker lock(&logger.m_mutex);
        previous = logger.m_previousHandler;
    }
    if (previous && previous != &StructuredLogger::qtMessageHandler)
        previous(type, context, message);
}

void StructuredLogger::logQtMessage(QtMsgType type,
                                    const QMessageLogContext &context,
                                    const QString &message)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_initialized) {
            const QString category = context.category && *context.category
                ? QStringLiteral("qt.%1").arg(QString::fromUtf8(context.category))
                : QStringLiteral("qt");
            static_cast<void>(writeRecordLocked(
                severityForQtMessage(type), category, QStringLiteral("qt.message"),
                message, {},
                context.file ? QString::fromUtf8(context.file) : QString(),
                context.function ? QString::fromUtf8(context.function) : QString(),
                context.line));
            return;
        }
    }
    if (initialize())
        logQtMessage(type, context, message);
}

bool StructuredLogger::writeRecordLocked(LogSeverity severity,
                                         const QString &category,
                                         const QString &event,
                                         const QString &message,
                                         const QJsonObject &data,
                                         const QString &sourceFile,
                                         const QString &sourceFunction,
                                         int sourceLine,
                                         QString *error)
{
    QJsonObject thread{
        {QStringLiteral("id"), currentThreadId()},
    };
    if (QThread::currentThread() && !QThread::currentThread()->objectName().isEmpty())
        thread.insert(QStringLiteral("name"), QThread::currentThread()->objectName());

    QJsonObject source{
        // Build roots frequently contain a developer account or workspace
        // path. Keep the diagnostic filename/line while dropping directories.
        {QStringLiteral("file"), redactText(sourceBaseName(sourceFile))},
        {QStringLiteral("function"), redactText(sourceFunction)},
        {QStringLiteral("line"), sourceLine},
    };
    QJsonObject record{
        {QStringLiteral("schema"), QStringLiteral("wimforge.log")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("sequence"), static_cast<qint64>(++m_sequence)},
        {QStringLiteral("severity"), severityName(severity)},
        {QStringLiteral("category"), redactText(category)},
        {QStringLiteral("event"), redactText(event)},
        {QStringLiteral("message"), redactText(message)},
        {QStringLiteral("sessionId"), m_sessionId},
        {QStringLiteral("processId"), QCoreApplication::applicationPid()},
        {QStringLiteral("thread"), thread},
        {QStringLiteral("source"), source},
        {QStringLiteral("data"), redactJson(data)},
    };
    QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (!rotateIfNeededLocked(bytes.size(), error))
        return false;

    QFile file(m_logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error)
            *error = QStringLiteral("Could not open %1: %2").arg(m_logPath, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        if (error)
            *error = QStringLiteral("Could not write %1: %2").arg(m_logPath, file.errorString());
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool StructuredLogger::rotateIfNeededLocked(qint64 incomingBytes, QString *error)
{
    const QFileInfo current(m_logPath);
    if (!current.exists() || current.size() == 0
        || current.size() + incomingBytes <= m_maximumFileBytes) {
        return true;
    }

    const QString oldest = archivePathLocked(m_archiveCount);
    if (QFileInfo::exists(oldest) && !QFile::remove(oldest)) {
        if (error)
            *error = QStringLiteral("Could not remove old log archive: %1").arg(oldest);
        return false;
    }
    for (int index = m_archiveCount - 1; index >= 1; --index) {
        const QString from = archivePathLocked(index);
        if (!QFileInfo::exists(from))
            continue;
        const QString to = archivePathLocked(index + 1);
        if (!QFile::rename(from, to)) {
            if (error)
                *error = QStringLiteral("Could not rotate log archive %1 to %2").arg(from, to);
            return false;
        }
    }
    const QString firstArchive = archivePathLocked(1);
    if (!QFile::rename(m_logPath, firstArchive)) {
        if (error)
            *error = QStringLiteral("Could not rotate application log to %1").arg(firstArchive);
        return false;
    }
    return true;
}

QString StructuredLogger::archivePathLocked(int index) const
{
    const QFileInfo info(m_logPath);
    const QString suffix = info.completeSuffix();
    const QString name = suffix.isEmpty()
        ? QStringLiteral("%1.%2").arg(info.completeBaseName()).arg(index)
        : QStringLiteral("%1.%2.%3").arg(info.completeBaseName()).arg(index).arg(suffix);
    return QDir(info.absolutePath()).filePath(name);
}

} // namespace wimforge
