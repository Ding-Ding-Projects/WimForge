#pragma once

#include "ProjectConfig.h"
#include "ServicingPlan.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>

namespace wimforge {

class JobEngine final : public QObject
{
    Q_OBJECT

public:
    explicit JobEngine(QObject *parent = nullptr);
    ~JobEngine() override;

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] int runningCount() const;
    [[nodiscard]] double progress() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString journalPath() const;
    [[nodiscard]] QList<ServicingOperation> operations() const;

    bool start(const ProjectConfig &project,
               const QList<ServicingOperation> &operations,
               int maximumParallel,
               QString *error = nullptr);
    void cancel();

    static QString defaultRecoveryRoot();
    static QString journalPathForProject(const QString &projectDirectory);
    static bool hasInterruptedRun(const QString &projectDirectory, QJsonObject *journal = nullptr,
                                  QString *error = nullptr);
    static bool isAdministrator();

signals:
    void stateChanged();
    void operationChanged(int index, const QString &state, const QString &lastOutput);
    void outputReceived(int index, const QString &text);
    void finished(bool success, const QString &message);

private:
    struct ActiveProcess {
        int index = -1;
        QPointer<QProcess> process;
        QString logPath;
    };

    void schedule();
    void launch(int index);
    void finishOperation(int index, bool success, const QString &detail);
    void finishRun(bool success, const QString &message);
    [[nodiscard]] bool dependenciesDone(const ServicingOperation &operation) const;
    [[nodiscard]] bool dependencyFailed(const ServicingOperation &operation) const;
    [[nodiscard]] bool dependencyChainDone(const QString &operationId) const;
    [[nodiscard]] bool dependencyChainFailed(const QString &operationId) const;
    [[nodiscard]] int completedCount() const;
    [[nodiscard]] QString stateDirectory() const;
    bool writeJournal(QString *error = nullptr) const;
    void appendLog(const QString &path, const QByteArray &bytes) const;

    ProjectConfig m_project;
    QList<ServicingOperation> m_operations;
    QHash<QString, int> m_indexById;
    QList<ActiveProcess> m_active;
    QString m_runId;
    QString m_startedAt;
    QString m_statusText;
    int m_maximumParallel = 1;
    bool m_running = false;
    bool m_cancelling = false;
};

} // namespace wimforge
