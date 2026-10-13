#pragma once

#include <QString>
#include <windows.h>

namespace VNH {

class UserSessionReady
{
public:
    UserSessionReady() = default;
    ~UserSessionReady();
    UserSessionReady(const UserSessionReady&) = delete;
    UserSessionReady& operator=(const UserSessionReady&) = delete;

    bool create(const QString& userId, QString* error);
    QString name() const { return name_; }
    bool wait(HANDLE process, int timeoutMs, QString* error) const;
    static bool signal(const QString& name, QString* error);

private:
    HANDLE event_ = nullptr;
    QString name_;
};

}
