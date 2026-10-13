
#pragma once

#include <QJsonObject>
#include <array>
#include <memory>
#include <functional>
#include <cstdint>

namespace VNX {

struct ProjectItem;

// Densely packed (one-time WP2 renumbering, zero persisted int faces).
// Retired slots must not be reused by future versions.
enum class ComponentType : uint8_t
{
    MixNode      = 0,
    Audio        = 1,
    Selection    = 2,
    Bus          = 3,
    Action       = 4,
    Variable     = 5,
    Attenuation  = 6,
    Spatial      = 7,
    Track        = 8,
    Piece        = 9,
    Sound        = 10,
    _Count       = 11
};

static constexpr size_t kMaxComponents = static_cast<size_t>(ComponentType::_Count);

class IComponent
{
public:
    virtual ~IComponent() = default;

    virtual ComponentType getType() const = 0;

    virtual QJsonObject toJson() const = 0;

    virtual bool fromJson(const QJsonObject& json) = 0;

    virtual QVariant getField(const QString& fieldName) const { (void)fieldName; return {}; }

    virtual bool setField(const QString& fieldName, const QVariant& value) { (void)fieldName; (void)value; return false; }

    virtual QString searchableText() const { return {}; }

    void setHostItem(ProjectItem* h) noexcept { host_ = h; }
    ProjectItem* hostItem() const noexcept { return host_; }

protected:
    ProjectItem* host_ = nullptr;
};

class ComponentStore
{
public:
    ComponentStore() = default;
    ~ComponentStore() = default;

    ComponentStore(ComponentStore&&) = default;
    ComponentStore& operator=(ComponentStore&&) = default;

    template<typename T>
    T* get() const
    {
        auto idx = static_cast<size_t>(T::kType);
        return (idx < kMaxComponents && slots_[idx])
            ? static_cast<T*>(slots_[idx].get())
            : nullptr;
    }

    template<typename T>
    T* getOrCreate()
    {
        auto idx = static_cast<size_t>(T::kType);
        if (idx >= kMaxComponents) return nullptr;
        if (!slots_[idx])
            slots_[idx] = std::make_unique<T>();
        return static_cast<T*>(slots_[idx].get());
    }

    template<typename T>
    void remove()
    {
        auto idx = static_cast<size_t>(T::kType);
        if (idx < kMaxComponents) slots_[idx].reset();
    }

    bool has(ComponentType type) const
    {
        auto idx = static_cast<size_t>(type);
        return idx < kMaxComponents && slots_[idx] != nullptr;
    }

    void forEach(const std::function<void(IComponent&)>& fn) const
    {
        for (auto& slot : slots_)
        {
            if (slot) fn(*slot);
        }
    }

    size_t count() const
    {
        size_t n = 0;
        for (auto& slot : slots_)
            if (slot) ++n;
        return n;
    }

private:
    std::array<std::unique_ptr<IComponent>, kMaxComponents> slots_{};
};

}
