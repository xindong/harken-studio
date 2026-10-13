
#pragma once

#include "VQLTypes.h"
#include <QObject>
#include <QTimer>
#include <map>
#include <functional>
#include <cstdint>

namespace VNX {

class VQLEngine;

using ObserverId = uint32_t;
using ObserverCallback = std::function<void(const VQLResultSet&)>;

class VQLObserver : public QObject
{
    Q_OBJECT

public:
    static constexpr int DEBOUNCE_MS = 100;

    explicit VQLObserver(VQLEngine* engine, QObject* parent = nullptr);
    ~VQLObserver() override;

    ObserverId observe(const QString& vql, ObserverCallback callback);
    void unobserve(ObserverId id);
    void unobserveAll();

    int observerCount() const { return static_cast<int>(entries_.size()); }
    bool hasObservers() const { return !entries_.empty(); }

public slots:
    void onProjectChanged();

private slots:
    void evaluatePending();

private:
    uint32_t computeResultHash(const VQLResultSet& result) const;

    struct ObserverEntry
    {
        QString           vql;
        VQLCompiledQuery  compiled;
        ObserverCallback  callback;
        uint32_t          lastHash = 0;
    };

    VQLEngine*                     engine_ = nullptr;
    std::map<ObserverId, ObserverEntry> entries_;
    ObserverId                     nextId_ = 1;
    QTimer                         debounceTimer_;
    bool                           dirty_ = false;
};

}
