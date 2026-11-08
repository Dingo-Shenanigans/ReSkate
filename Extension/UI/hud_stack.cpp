#include "hud_stack.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include "Engine/Game/Build/20260929/reference_lists.h"
#include <cstring>
#include <vector>

namespace dingosdk::hud_stack {
namespace {
namespace corner = addr::hud_corner;
namespace menu_data = multiplayer::menu_data;
using Address = std::uintptr_t;

// The stack's items as bytes, each a stack item, reading at most `most` of them.
std::vector<std::byte> item_bytes(const Context& context, const Value& field, unsigned most, unsigned& count) {
    unsigned stride{};
    auto items = context.array(field, most, count, stride);
    menu_data::require(!count || stride == menu_data::stack_item.size, "the stack's items differ");
    items.resize(std::size_t{count} * menu_data::stack_item.size);
    return items;
}
std::optional<unsigned> key_index(const Context& context, const std::vector<std::byte>& items, unsigned count, std::uint32_t key) {
    const auto key_at = context.member(context.type(menu_data::stack_item), corner::item_key).offset;
    for (unsigned index = 0; index < count; ++index) {
        std::uint32_t found{};
        std::memcpy(&found, items.data() + std::size_t{index} * menu_data::stack_item.size + key_at, sizeof found);
        if (found == key) return index;
    }
    return std::nullopt;
}
} // namespace

std::string asset_name(Address asset) {
    return asset ? menu_data::string(menu_data::read<Address>(asset + addr::reference_lists::asset_name), 256) : std::string{};
}
std::string widget_of(const Context& context, const Value& item) {
    return asset_name(menu_data::read<Address>(context.address(context.path(item, {corner::item_content, corner::content_blueprint}))));
}
Value items_of(const Context& context, const Value& stack) { return context.field(stack, corner::stack_items); }

Value find_dpad_stack(const Context& context) {
    for (const auto& root : context.roots({corner::hud_view_schema})) {
        const auto stack = context.field(root.model, corner::dpad_scoring_stack);
        const auto items = items_of(context, stack);
        unsigned count{}, stride{};
        (void)context.array(items, most_items, count, stride);
        for (unsigned index = 0; index < count; ++index)
            if (widget_of(context, context.element(items, index)) == corner::dpad_widget) return stack;
    }
    return {};
}

std::optional<unsigned> find_key(const Context& context, const Value& stack, std::uint32_t key) {
    unsigned count{};
    const auto items = item_bytes(context, items_of(context, stack), most_items_out, count);
    return key_index(context, items, count, key);
}

void insert_item(const Context& context, const Value& stack, const Value& item) {
    const auto field = items_of(context, stack);
    unsigned count{};
    auto items = item_bytes(context, field, most_items, count);
    std::vector<std::byte> bytes(menu_data::stack_item.size);
    menu_data::require(memory::read_bytes(context.address(item), bytes.data(), bytes.size()), "our stack item is unavailable");
    items.insert(items.end(), bytes.begin(), bytes.end());
    context.array(field, items, count + 1);
}

bool remove_key(const Context& context, const Value& stack, std::uint32_t key) {
    const auto field = items_of(context, stack);
    unsigned count{};
    auto items = item_bytes(context, field, most_items_out, count);
    const auto ours = key_index(context, items, count, key);
    if (!ours) return false;
    const auto begin = items.begin() + static_cast<std::ptrdiff_t>(std::size_t{*ours} * menu_data::stack_item.size);
    items.erase(begin, begin + menu_data::stack_item.size);
    context.array(field, items, count - 1);
    return true;
}
} // namespace dingosdk::hud_stack
