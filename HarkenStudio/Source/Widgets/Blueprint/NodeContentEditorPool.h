#pragma once

#include "NodeContentInfo.h"

#include <QString>
#include <memory>
#include <vector>

class QGraphicsProxyWidget;

namespace VNX {

class NodeContentEditorPool
{
public:
    NodeContentEditorPool();
    ~NodeContentEditorPool();

    void prepare(const NodeContentInfo& info, NodeContentFields fields, int labelWidth, bool routeGain);
    QGraphicsProxyWidget* acquire(const NodeContentInfo& info, NodeContentFields fields,
                                  int labelWidth, bool routeGain, const QString& context);
    void release(QGraphicsProxyWidget* proxy);
    void clear();

private:
    struct Entry;
    Entry& create(const NodeContentInfo& info, NodeContentFields fields, int labelWidth, bool routeGain);
    std::vector<std::unique_ptr<Entry>> entries_;
};

}
