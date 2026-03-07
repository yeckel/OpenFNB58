#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>

#include "DeviceBackend.h"

int main(int argc, char* argv[])
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Material");

    QApplication app(argc, argv);
    app.setApplicationName("FNB58 Power Meter");
    app.setOrganizationName("OpenFNB58");
    app.setApplicationVersion("1.0");

    DeviceBackend backend;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);

    engine.loadFromModule("fnb58app", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
