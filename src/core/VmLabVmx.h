#pragma once

#include "VmLab.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace wimforge::vmlab {

class VmxDocument
{
public:
    static std::optional<VmxDocument> parse(const QByteArray &bytes, QString *error = nullptr);
    static std::optional<VmxDocument> load(const QString &path, QString *error = nullptr);
    static std::optional<VmxDocument> fromCreateSpec(const CreateSpec &spec,
                                                     const QString &diskPath,
                                                     QString *error = nullptr);

    [[nodiscard]] QString value(const QString &key) const;
    [[nodiscard]] bool contains(const QString &key) const;
    [[nodiscard]] QStringList storagePaths(const QString &baseDirectory) const;
    bool setValue(const QString &key, const QString &value, QString *error = nullptr);
    bool remove(const QString &key);
    [[nodiscard]] QByteArray serialize() const;
    bool saveAtomic(const QString &path, QString *error = nullptr) const;

private:
    struct Line {
        QString raw;
        QString key;
        QString value;
        bool assignment = false;
    };

    QList<Line> m_lines;
    bool m_finalNewline = true;
    QByteArray m_lineEnding = QByteArray("\r\n");
};

bool applyConfigPatch(VmxDocument &document, const ConfigPatch &patch, QString *error = nullptr);

} // namespace wimforge::vmlab
