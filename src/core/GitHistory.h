#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace wimforge {

struct GitCommit
{
    QString hash;
    QString shortHash;
    QDateTime authoredAt;
    QString subject;

    [[nodiscard]] bool isRevert() const
    {
        return subject.startsWith(QStringLiteral("Revert \""), Qt::CaseInsensitive);
    }
};

// Small, synchronous wrapper around the real Git CLI. Each instance is pinned
// to one repository and a narrow set of state files, which keeps project and
// notification histories independent.
class GitHistory
{
public:
    explicit GitHistory(QString repositoryPath,
                        QStringList trackedFiles = {QStringLiteral("project.json")});

    [[nodiscard]] QString repositoryPath() const;
    [[nodiscard]] QStringList trackedFiles() const;
    [[nodiscard]] bool isRepository() const;

    bool initialize(QString *error = nullptr) const;
    bool commit(const QString &message, QString *error = nullptr) const;
    [[nodiscard]] QList<GitCommit> history(int maximumCount = 100,
                                           QString *error = nullptr) const;

    // Creates an inverse commit. Calling it when HEAD is itself a revert commit
    // applies the original change again (redo without a separate mutable stack).
    bool revertLatest(QString *error = nullptr) const;

    static bool gitAvailable(QString *error = nullptr);

private:
    QString m_repositoryPath;
    QStringList m_trackedFiles;
};

} // namespace wimforge
