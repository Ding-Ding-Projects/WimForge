#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QString>

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    if (application.arguments().size() != 3)
        return 2;
    const QIcon icon(application.arguments().at(1));
    if (icon.isNull())
        return 3;
    QImage image(256, 256, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    icon.paint(&painter, image.rect(), Qt::AlignCenter, QIcon::Normal, QIcon::On);
    painter.end();
    QImageWriter writer(application.arguments().at(2), QByteArrayLiteral("ico"));
    return writer.write(image) ? 0 : 4;
}
