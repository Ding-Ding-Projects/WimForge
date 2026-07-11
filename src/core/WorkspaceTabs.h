#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace wimforge {

// Browser-style page tabs are project data rather than global UI preferences.
// Their dedicated repository makes every rename, style change, reorder, import,
// and close operation independently auditable without polluting project.json.
class WorkspaceTabs
{
public:
    static constexpr int CurrentFormatVersion = 1;
    static inline const QString RepositoryRole = QStringLiteral("workspace-tabs");

    bool openProject(const QString &projectDirectory, QString *error = nullptr);
    void closeProject();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString repositoryPath() const;
    [[nodiscard]] QVariantList tabs() const;
    [[nodiscard]] int activeIndex() const;

    bool openPage(int page, const QString &defaultTitle, QString *error = nullptr);
    bool activate(int index, QString *error = nullptr);
    bool close(int index, QString *error = nullptr);
    bool move(int from, int to, QString *error = nullptr);
    bool update(int index, const QVariantMap &changes, QString *error = nullptr);

    // Portable tab definitions intentionally omit .git and can be merged into
    // another project's workspace. Repository bundles preserve all local Git
    // metadata and replace the current tab repository atomically on import.
    bool exportTabs(const QString &destinationFile, QString *error = nullptr) const;
    bool importTabs(const QString &sourceFile, QString *error = nullptr);
    bool exportRepository(const QString &destinationFile, QString *error = nullptr) const;
    bool importRepository(const QString &sourceFile, QString *error = nullptr);

private:
    [[nodiscard]] QJsonObject state() const;
    bool loadState(const QString &path, QString *error);
    bool save(const QString &message, QString *error);
    bool validateAndNormalize(QJsonObject *tab, QString *error) const;
    void ensureActiveIndex();

    QString m_projectDirectory;
    QString m_repositoryPath;
    QList<QJsonObject> m_tabs;
    int m_activeIndex = -1;
};

} // namespace wimforge
