#pragma once

#include "Widgets/Shared/RefDomain.h"

#include <QObject>

class QWidget;

namespace VNX {

class ObjectRefNavigation final : public QObject
{
    Q_OBJECT

public:
    explicit ObjectRefNavigation(QWidget* owner);

    static void locate(QWidget* source, RefDomain domain, uint32_t id);
    static void navigate(QWidget* source, RefDomain domain, uint32_t id);

signals:
    void locateRequested(VNX::RefDomain domain, uint32_t id);
    void navigateRequested(VNX::RefDomain domain, uint32_t id);

private:
    static ObjectRefNavigation* forWidget(QWidget* source);
};

}
