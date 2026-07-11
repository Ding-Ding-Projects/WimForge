#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QMessageLogContext>
#include <QMutex>
#include <QString>
#include <QtLogging>

#include <source_location>

namespace wimforge {

enum class LogSeverity
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

struct StructuredLoggerOptions
{
    QString directory;
    QString fileName = QStringLiteral("wimforge.jsonl");
    qint64 maximumFileBytes = 5 * 1024 * 1024;
    int archiveCount = 5;
};

// Thread-safe JSONL application logging. Every record carries a stable session
// identifier and sequence number so reports from concurrent servicing workers
// can be reconstructed without relying on human-readable text ordering.
class StructuredLogger final
{
public:
    static StructuredLogger &instance();

    bool initialize(const StructuredLoggerOptions &options = {}, QString *error = nullptr);
    void installQtMessageHandler();
    void shutdown(const QJsonObject &data = {});

    void log(LogSeverity severity,
             const QString &category,
             const QString &event,
             const QString &message = {},
             const QJsonObject &data = {},
             const std::source_location &source = std::source_location::current());

    [[nodiscard]] QString logPath() const;
    [[nodiscard]] QString logDirectory() const;
    [[nodiscard]] QString sessionId() const;

    [[nodiscard]] static QString redactText(const QString &text);
    [[nodiscard]] static QJsonValue redactJson(const QJsonValue &value,
                                               const QString &key = {});
    [[nodiscard]] static QString severityName(LogSeverity severity);

private:
    StructuredLogger() = default;
    ~StructuredLogger();
    StructuredLogger(const StructuredLogger &) = delete;
    StructuredLogger &operator=(const StructuredLogger &) = delete;

    static void qtMessageHandler(QtMsgType type,
                                 const QMessageLogContext &context,
                                 const QString &message);
    void logQtMessage(QtMsgType type,
                      const QMessageLogContext &context,
                      const QString &message);
    [[nodiscard]] bool writeRecordLocked(LogSeverity severity,
                                         const QString &category,
                                         const QString &event,
                                         const QString &message,
                                         const QJsonObject &data,
                                         const QString &sourceFile,
                                         const QString &sourceFunction,
                                         int sourceLine,
                                         QString *error = nullptr);
    [[nodiscard]] bool rotateIfNeededLocked(qint64 incomingBytes, QString *error);
    [[nodiscard]] QString archivePathLocked(int index) const;
    [[nodiscard]] static bool sensitiveKey(const QString &key);

    mutable QMutex m_mutex;
    QString m_logPath;
    QString m_sessionId;
    qint64 m_maximumFileBytes = 5 * 1024 * 1024;
    int m_archiveCount = 5;
    quint64 m_sequence = 0;
    bool m_initialized = false;
    bool m_handlerInstalled = false;
    QtMessageHandler m_previousHandler = nullptr;
};

} // namespace wimforge
