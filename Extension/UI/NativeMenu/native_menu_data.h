#pragma once
#include "Engine/Game/Abi/native_data.h"
#include <cstring>
#include "Engine/Game/Build/fingerprint.h"
#include <map>
#include <span>
#include <string>
#include <vector>
#include <cstring>

namespace dingosdk::multiplayer::menu_data {
using Address = std::uintptr_t;
using Handle = std::uint64_t;
struct Ref { Address record{}; Handle handle{}; };
struct Widget { Address blueprint{}; Ref data; };
struct Value { Handle handle{}; Address type{}; };
struct Field { unsigned index{}; std::uint16_t offset{}; Address type{}; };
struct Root { Value model; Address value{}; };
struct Schema { std::uint32_t hash{}; std::uint16_t size{}; };
// The native loader assumes DataModelObject at +18/+20. Passing a widget
// blueprint reads its name as TypeInfo and faults inside the model factory.
inline constexpr game::build::Fingerprint model_record_load_contract{0x1918520,
    {0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
     0x48,0x83,0xec,0x50,0x41,0xbd,0x00,0x01,0x00,0x00,0x48,0x8d,0x6a,0xf0,0x66,0x44}};
using ModelRecordLoad=Handle (*)(Address,Address,bool);
extern ModelRecordLoad model_record_load_original;
Handle model_record_load_hook(Address manager,Address record,bool flag);
unsigned size(Address type);
inline constexpr Schema core{0xde615b97, 1392}, page{0x5844be65, 1968},
    stack_item{0xbd3e10f0, 1424}, button{0x608f1aab, 1232}, input{0x9832b0a8, 384},
    linear_list{0x48455d84, 464}, presenter{0x395d20a4, 24};

// Used only on the client thread while holding the native model write lock.
class Context {
    mutable std::map<std::uint32_t, Address> types_;
public:
    Address base{}, manager{};
    explicit Context(Address image, Address models) : base(image), manager(models) {}
    Address type(Schema schema) const;
    Address address(Value value) const;
    Address type_of(Handle handle) const;
    std::vector<Root> roots(std::initializer_list<std::uint32_t> hashes = {}) const;
    Field member(Address type, std::uint32_t hash) const;
    Value field(Value value, std::uint32_t hash) const;
    Value path(Value value, std::initializer_list<std::uint32_t> hashes) const;
    Value element(Value array, unsigned index) const;
    Value create(Schema schema, std::uint64_t id) const;
    // Gives every null CString in an owned copy an engine-owned empty string:
    // the VM's typed read calls strlen even for empty optional text.
    void initialize_text(Value value) const;
    void destroy(Value owned_root) const;
    void publish(Value target, const void* bytes) const;
    void copy(Value target, Address source) const;
    template<class T> void set(Value target, const T& value) const {
        static_assert(std::is_trivially_copyable_v<T>);
        if (size(target.type) != sizeof(T)) throw std::runtime_error("Native menu value size differs.");
        publish(target, &value);
    }
    template<class T> void set_if_changed(Value target, const T& value) const {
        static_assert(std::is_trivially_copyable_v<T>);
        if (size(target.type) != sizeof(T)) throw std::runtime_error("Native menu value size differs.");
        T current{};
        if (!memory::peek_bytes(address(target), &current, sizeof(T))) throw std::runtime_error("Native menu value is unavailable.");
        if (std::memcmp(&current, &value, sizeof(T))) publish(target, &value);
    }
    void text(Value target, const std::string& text) const;
    std::string text(Value value, std::size_t maximum = 128) const;
    // Snapshots copy row bytes but still borrow nested native values. Keep
    // their owners live and publish synchronously under the model write lock.
    std::vector<std::byte> array(Value value, unsigned maximum, unsigned& count, unsigned& stride) const;
    // Retains the snapshot in a transient native array before resizing target.
    // The temporary is destroyed before this call returns (including errors).
    void array(Value target, std::span<const std::byte> bytes, unsigned count) const;
    template<class T> void array(Value target, const std::vector<T>& values) const {
        array(target, std::as_bytes(std::span(values)), static_cast<unsigned>(values.size()));
    }
};

// Runs on the client thread for every menu page tick: peeked (a plain copy) rather than one
// ReadProcessMemory call per field, which profiled at ~16% of that thread on 2026-10-01.
template<class T> T read(Address at) {
    T value{};
    if (!memory::peek(at, value)) throw std::runtime_error("Native menu memory is unavailable.");
    return value;
}
void require(bool condition, const char* message);
std::string string(Address at, std::size_t maximum = 256);
unsigned kind(Address type);

// Blueprint references are borrowed only until publication. Native model
// publication performs the engine's typed copy/retain operation.
std::map<std::string, Address, std::less<>> blueprints(const Context& context, Value core);
// A loaded asset by name, searched from `anchor`'s asset domain up its parents.
Address named_asset(const Context& context, Address anchor, const char* name);
// Keep the registry lock until native publication has retained the icon. Missing
// optional art leaves the caller's already-published fallback untouched.
bool publish_tab_icons(const Context& context, Value tab, std::string_view idle, std::string_view focused);
bool publish_texture(const Context& context, std::span<const Value> fields, std::string_view name);
// Publishes one loaded texture or image asset, by name, into an asset reference field.
bool publish_texture(const Context& context, Value target, std::string_view name);
// The same texture into several fields, each written only when it differs.
bool publish_texture(const Context& context, std::span<const Value> targets, std::string_view name);
} // namespace dingosdk::multiplayer::menu_data
