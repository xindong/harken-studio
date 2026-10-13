
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace VNX {

namespace ObjectListName
{
    inline const QString ChildrenList  = QStringLiteral("ChildrenList");
    inline const QString PropertyList  = QStringLiteral("PropertyList");
    inline const QString ReferenceList = QStringLiteral("ReferenceList");
}

class IObjectList
{
public:
    virtual ~IObjectList() = default;

    virtual QString getName() const = 0;

    virtual size_t size() const = 0;

    virtual void clear() = 0;

    virtual QJsonObject toJson() const = 0;

    virtual void fromJson(const QJsonObject& json) = 0;
};

template<typename T>
class TypedObjectList : public IObjectList
{
public:
    explicit TypedObjectList(QString name) : name_(std::move(name)) {}
    ~TypedObjectList() override = default;

    TypedObjectList(TypedObjectList&&) noexcept = default;
    TypedObjectList& operator=(TypedObjectList&&) noexcept = default;

    TypedObjectList(const TypedObjectList&) = delete;
    TypedObjectList& operator=(const TypedObjectList&) = delete;

    QString getName() const override { return name_; }
    size_t  size() const override    { return items_.size(); }
    void    clear() override         { items_.clear(); }

    T* add(std::unique_ptr<T> item)
    {
        items_.push_back(std::move(item));
        return items_.back().get();
    }

    bool remove(size_t index)
    {
        if (index >= items_.size()) return false;
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    T* at(size_t index)
    {
        return index < items_.size() ? items_[index].get() : nullptr;
    }

    const T* at(size_t index) const
    {
        return index < items_.size() ? items_[index].get() : nullptr;
    }

    auto begin()       { return items_.begin(); }
    auto end()         { return items_.end(); }
    auto begin() const { return items_.begin(); }
    auto end()   const { return items_.end(); }

    QJsonObject toJson() const override
    {
        QJsonArray arr;
        for (const auto& item : items_)
        {
            if (item) arr.append(item->toJson());
        }
        QJsonObject obj;
        obj["name"]  = name_;
        obj["items"] = arr;
        return obj;
    }

    void fromJson(const QJsonObject& json) override
    {
        if (json.contains("name")) name_ = json.value("name").toString();
        items_.clear();
        const QJsonArray arr = json.value("items").toArray();
        items_.reserve(static_cast<size_t>(arr.size()));
        for (const auto& v : arr)
        {
            auto item = std::make_unique<T>();
            item->fromJson(v.toObject());
            items_.push_back(std::move(item));
        }
    }

private:
    QString                         name_;
    std::vector<std::unique_ptr<T>> items_;
};


}
