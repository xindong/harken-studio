#pragma once

#include <QObject>
#include <atomic>
#include <memory>

namespace VNX {

class ProjectPlatformActivity : public QObject
{
    Q_OBJECT

public:
    class Lease
    {
    public:
        Lease();
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    };

    static ProjectPlatformActivity& instance();
    static std::shared_ptr<Lease> acquire();
    bool isBusy() const;
    void begin();
    void end();

signals:
    void busyChanged(bool busy);

private:
    ProjectPlatformActivity();
    std::atomic<int> active_{0};
};

}
