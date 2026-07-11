#pragma once

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace wimforge {

struct ImageInspectionCommand
{
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
    QString imagePath;
    QString error;
    bool isoSource = false;
    bool utf8Output = false;
};

struct ImageInspectionResult
{
    QStringList editions;
    QString mountedImagePath;
    QString relativeImagePath;
    QString output;
};

class ImageSourceInspector final
{
public:
    [[nodiscard]] static ImageInspectionCommand commandFor(
        const QString &sourcePath,
        const QString &configuredImagePath = {});
    [[nodiscard]] static ImageInspectionResult parseOutput(
        const QByteArray &output,
        bool isoSource,
        bool utf8Output);
    [[nodiscard]] static QString isoInspectionScript();
};

} // namespace wimforge
