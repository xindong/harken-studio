#pragma once

#include "NodeContentInfo.h"
#include "Core/PropertyDescriptor.h"

#include <QWidget>
#include <QCache>
#include <QPicture>
#include <map>
#include <tuple>
#include <memory>
#include <vector>

namespace VNX {

class ParameterBinding;
struct ContextTopic;

PropertyDescriptor nodeContentProperty(NodeContentField field);

struct NodeContentPresentation {
    QPicture drawing;
    QSize size;
    std::vector<std::pair<NodeContentField, QRect>> fields;
};

class NodeContentEditor : public QWidget
{
    Q_OBJECT

public:
    static constexpr int kSliderWidth = 100;
    static constexpr int kRouteGainMinimumWidth = 100;
    static constexpr int kValueWidth = 44;

    NodeContentEditor(const NodeContentInfo& info, NodeContentFields fields,
                      int labelWidth, QWidget* parent = nullptr);
    ~NodeContentEditor() override;

    static int labelWidth(NodeContentFields fields);
    void setValues(const NodeContentInfo& info);
    void setFields(NodeContentFields fields);
    void setContext(const QString& context);
    void setRouteGainMode(bool enabled);
    bool routeGainMode() const { return routeGainMode_; }
    bool isDragHandleAt(const QPoint& position) const;
    bool beginValueEditingAt(const QPoint& position);
    void finishEditing(bool committed, std::optional<NodeContentField> except = {});
    bool isEditing() const;
    ParameterBinding* binding(NodeContentField field) const;
    std::optional<NodeContentField> fieldAt(const QPoint& position) const;
    void setFieldTopic(NodeContentField field, const ContextTopic& topic);
    NodeContentPresentation presentation();
    void setLabelWidth(int width);

signals:
    void editBegan(NodeContentField field);
    void editPreviewed(NodeContentField field, const QVariant& value);
    void editEnded(NodeContentField field, bool committed);
    void valueCommitted(NodeContentField field, const QVariant& value);
    void editingClosed();
    void contentSizeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    struct Row;
    void finishValueEdit(Row& row, bool committed);
    void refreshValue(Row& row);
    void updateMetrics();
    void refreshFonts();
    std::vector<std::unique_ptr<Row>> rows_;
    NodeContentFields fields_;
    int labelColumnWidth_;
    int valueColumnWidth_ = kValueWidth;
    bool routeGainMode_ = false;
};

class NodeContentRenderer
{
public:
    NodeContentPresentation presentation(const NodeContentInfo& info, NodeContentFields fields,
                                         int labelWidth, bool routeGain);
    void clear();

private:
    QWidget host_;
    std::map<std::tuple<int, int, int, bool>, std::unique_ptr<NodeContentEditor>> editors_;
    QCache<QByteArray, NodeContentPresentation> cache_{256};
};

}
