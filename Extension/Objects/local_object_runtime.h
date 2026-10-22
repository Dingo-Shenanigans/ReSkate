#pragma once
#include "Extension/Profile/runtime_internal.h"
#include "Engine/Game/Abi/native_callback_queue.h"

namespace dingosdk::profile_runtime {
struct ObjectCategoryFunctions {
    void* (*subscribe)(void*, const void*, const void*){};
    void* (*allocate)(std::size_t, std::size_t, std::uintptr_t, unsigned char, unsigned char){};
    void* (*construct)(void*){};
    void (*initialize)(void*, std::uintptr_t, unsigned char, unsigned char, unsigned char){};
    void (*parse)(void*, void*, const void*, const void*){};
    void (*release)(void*){};
};

struct ObjectRuntime {
    ObjectCategoryFunctions functions;
    struct Pending { std::uintptr_t callback{}; };
    game::NativeCallbackQueue<Pending> pending;
};

ObjectRuntime& object_runtime();

bool local_object_browser_ready() noexcept;
// Development-only bounded observation; never changes a graph's online result.
std::string bmx_flow_diagnostics();
void bmx_flow_expression_started(std::uintptr_t vm,std::uint32_t pc) noexcept;
void bmx_flow_expression_finished(std::uintptr_t vm,std::uint32_t pc=0xffffffffu) noexcept;

void initialize_object_functions(std::uintptr_t base);

struct ObjectReferenceGuard {
    std::uintptr_t value{};
    ~ObjectReferenceGuard() { if (value) object_runtime().functions.release(&value); }
};

struct ObjectServiceString {
    const char* data{};
    std::uint32_t size{}, capacity{};
    std::uintptr_t allocator{};
    void set(const std::string& value) {
        data = value.c_str(); size = static_cast<std::uint32_t>(value.size()); capacity = size | 0x80000000;
    }
};

struct ObjectServiceSubcategory {
    ObjectServiceString id;
    std::uint32_t hash{}, reserved{};
    ObjectServiceString title, unused, icon;
    std::uint32_t priority{}, padding{};
};

struct ObjectServiceCategory {
    ObjectServiceString id;
    std::uint32_t hash{}, reserved{};
    ObjectServiceString title, unused, icon;
    std::array<std::uintptr_t, 4> tags{};
    std::array<std::uintptr_t, 3> subcategories{};
    std::uint32_t flags{}, reserved_tail{}, priority{}, padding{};
};

static_assert(sizeof(ObjectServiceString) == 0x18 && sizeof(ObjectServiceSubcategory) == 0x70 &&
    sizeof(ObjectServiceCategory) == 0xb0 && offsetof(ObjectServiceCategory, subcategories) == 0x88 &&
    offsetof(ObjectServiceCategory, priority) == 0xa8);

bool accept_object_category(const void*, const void*);

void fill_object_categories(std::uintptr_t result, const profile::Snapshot& snapshot);

void* object_categories_hook(void* destination, const void* callback, const void* group);

void update_object_categories();
}
