#include "NodeContentEditorPool.h"

#include "NodeContentEditor.h"

#include <QGraphicsProxyWidget>

#include <algorithm>

namespace VNX {

struct NodeContentEditorPool::Entry
{
    NodeContentFields availableFields;
    bool routeGain;
    bool active = false;
    std::unique_ptr<QGraphicsProxyWidget> proxy;
};

NodeContentEditorPool::NodeContentEditorPool() = default;
NodeContentEditorPool::~NodeContentEditorPool() = default;

NodeContentEditorPool::Entry& NodeContentEditorPool::create(const NodeContentInfo& info,
    NodeContentFields fields, int labelWidth, bool routeGain)
{
    auto entry = std::make_unique<Entry>();
    entry->availableFields = info.fields();
    entry->routeGain = routeGain;
    entry->proxy = std::make_unique<QGraphicsProxyWidget>();
    auto* editor = new NodeContentEditor(info, fields, labelWidth);
    editor->setRouteGainMode(routeGain);
    entry->proxy->setWidget(editor);
    entry->proxy->hide();
    entries_.push_back(std::move(entry));
    return *entries_.back();
}

void NodeContentEditorPool::prepare(const NodeContentInfo& info, NodeContentFields fields,
    int labelWidth, bool routeGain)
{
    const auto count = std::count_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return entry->availableFields == info.fields() && entry->routeGain == routeGain;
    });
    for (auto index = count; index < 2; ++index)
        create(info, fields, labelWidth, routeGain);
}

QGraphicsProxyWidget* NodeContentEditorPool::acquire(const NodeContentInfo& info,
    NodeContentFields fields, int labelWidth, bool routeGain, const QString& context)
{
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return !entry->active && entry->availableFields == info.fields() && entry->routeGain == routeGain;
    });
    auto& entry = found != entries_.end() ? **found : create(info, fields, labelWidth, routeGain);
    entry.active = true;
    auto* editor = static_cast<NodeContentEditor*>(entry.proxy->widget());
    editor->prepareForReuse(info, fields, labelWidth, routeGain, context);
    return entry.proxy.get();
}

void NodeContentEditorPool::release(QGraphicsProxyWidget* proxy)
{
    const auto found = std::find_if(entries_.begin(), entries_.end(), [proxy](const auto& entry) {
        return entry->proxy.get() == proxy;
    });
    Q_ASSERT(found != entries_.end());
    (*found)->active = false;
}

void NodeContentEditorPool::clear()
{
    Q_ASSERT(std::none_of(entries_.begin(), entries_.end(), [](const auto& entry) { return entry->active; }));
    entries_.clear();
}

}
