#pragma once

#include <QThread>

namespace VNX {

template<typename Operation>
bool retryFileOperation(Operation&& operation)
{
#ifdef Q_OS_WIN
    constexpr int attempts = 3;
#else
    constexpr int attempts = 1;
#endif
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (operation())
            return true;
        if (attempt + 1 < attempts)
            QThread::msleep(50);
    }
    return false;
}

}
