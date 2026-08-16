/*
 * harbour-imira — device orientation watcher (QtSensors).
 */
#include "orientation.h"

#include <thread>

#include <QCoreApplication>
#include <QOrientationSensor>
#include <QOrientationReading>

namespace imira {

void startOrientationWatcher(std::atomic<int> *rotation)
{
    std::thread([rotation]() {
        int argc = 1;
        char name[] = "imira-castd";
        char *argv[] = { name, nullptr };
        QCoreApplication app(argc, argv);

        QOrientationSensor sensor;
        QObject::connect(&sensor, &QOrientationSensor::readingChanged,
                         [rotation, &sensor]() {
            QOrientationReading *r = sensor.reading();
            if (!r)
                return;
            // Mapping between the physical orientation and how lipstick
            // rotates the scene inside its portrait framebuffer. TopDown,
            // FaceUp and FaceDown keep the last known rotation.
            switch (r->orientation()) {
            case QOrientationReading::TopUp:
                rotation->store(0);
                break;
            case QOrientationReading::RightUp:
                rotation->store(270);
                break;
            case QOrientationReading::LeftUp:
                rotation->store(90);
                break;
            default:
                break;
            }
        });
        sensor.start();
        app.exec();
    }).detach();
}

} // namespace imira
