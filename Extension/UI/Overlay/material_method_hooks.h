#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

namespace dingosdk::overlay::detail
{
// One relay per implementation address, including nested wrapper/native calls.
// Install and enable are supplied by the host so this can be tested without a GPU.
template <class Function, std::size_t Slot, std::size_t Capacity = 32> class material_method_hooks;

template <class R, class... Args, std::size_t Slot, std::size_t Capacity>
class material_method_hooks<R (*)(Args...), Slot, Capacity>
{
    using Function = R (*)(Args...);
    struct Entry
    {
        void *target{};
        std::atomic<Function> relay{};
        Function callback{};
    };
    inline static std::array<Entry, Capacity> entries_{};
    inline static std::mutex mutex_;
    inline static thread_local Function active_{};
    template <std::size_t Index> static R invoke(Args... args)
    {
        struct Restore
        {
            Function previous;
            ~Restore()
            {
                active_ = previous;
            }
        } restore{active_};
        active_ = entries_[Index].relay.load();
        return entries_[Index].callback(args...);
    }
    template <std::size_t... Index> static constexpr auto thunks(std::index_sequence<Index...>)
    {
        return std::array<Function, Capacity>{&invoke<Index>...};
    }

  public:
    template <class Install> bool install(void *object, Function callback, Install install_hook)
    {
        const auto target = (*static_cast<void ***>(object))[Slot];
        std::lock_guard lock(mutex_);
        for (const auto &entry : entries_)
            if (entry.target == target)
                return entry.callback == callback;
        constexpr auto entry_thunks = thunks(std::make_index_sequence<Capacity>{});
        for (std::size_t i = 0; i < Capacity; ++i)
            if (!entries_[i].target)
            {
                auto &entry = entries_[i];
                // Publish callback and relay before enabling the hook.
                entry.callback = callback;
                if (!install_hook(target, reinterpret_cast<void *>(entry_thunks[i]),
                                  [&](void *relay) { entry.relay.store(reinterpret_cast<Function>(relay)); }))
                    return false;
                entry.target = target;
                return true;
            }
        return false;
    }
    // load() is valid only inside this thread's detour. Presentation code
    // needs persistent registration state, not the callback-local relay.
    bool installed() const
    {
        std::lock_guard lock(mutex_);
        for (const auto &entry : entries_)
            if (entry.target && entry.relay.load())
                return true;
        return false;
    }
    Function load() const
    {
        return active_;
    }
    Function load(void *object) const
    {
        const auto target = (*static_cast<void ***>(object))[Slot];
        std::lock_guard lock(mutex_);
        for (const auto &entry : entries_)
            if (entry.target == target)
                return entry.relay.load();
        return nullptr;
    }
};
} // namespace dingosdk::overlay::detail
