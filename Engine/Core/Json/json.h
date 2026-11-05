#pragma once
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dingosdk {
struct JsonLimits {
    std::size_t bytes = 4 * 1024 * 1024;
    std::size_t depth = 32;
    std::size_t events = 16384 * 16;
};

// Owning profile values with deep copies and stable object-member references.
// RapidJSON's SAX reader/writers handle JSON syntax in json.cpp; this deliberately
// small value model keeps allocator lifetimes out of snapshots and runtime code.
class Json {
public:
    using Object = std::map<std::string, Json, std::less<>>;
    using Array = std::vector<Json>;

    Json() = default;
    Json(const Json&) = default;
    Json(Json&&) noexcept = default;
    // Copy before replacing storage, including assignment from a child value.
    Json& operator=(Json value) noexcept { data_.swap(value.data_); return *this; }
    Json(std::nullptr_t) {}
    Json(bool value) : data_(value) {}
    template<std::integral T> requires (!std::same_as<T, bool>)
    Json(T value) {
        if constexpr (std::is_signed_v<T>) data_ = static_cast<std::int64_t>(value);
        else data_ = static_cast<std::uint64_t>(value);
    }
    template<std::floating_point T> Json(T value) : data_(static_cast<double>(value)) {}
    Json(const char* value) : data_(std::string(value)) {}
    Json(std::string value) : data_(std::move(value)) {}
    Json(std::string_view value) : data_(std::string(value)) {}
    Json(std::initializer_list<Json> values);
    template<std::ranges::input_range R>
        requires (!std::convertible_to<R, std::string_view> && !std::same_as<R, Json>)
    Json(const R& values) : data_(Array{}) {
        for (const auto& value : values) std::get<Array>(data_).emplace_back(value);
    }

    static Json object() { Json value; value.data_ = Object{}; return value; }
    static Json array(std::initializer_list<Json> values = {}) {
        Json value; value.data_ = Array(values); return value;
    }
    static Json parse(std::string_view text, JsonLimits limits = {});
    template<std::input_iterator It> static Json parse(It first, It last) {
        return parse(std::string(first, last));
    }
    std::string dump(int indent = -1) const;

    bool is_null() const { return std::holds_alternative<std::monostate>(data_); }
    bool is_boolean() const { return std::holds_alternative<bool>(data_); }
    bool is_number_unsigned() const { return std::holds_alternative<std::uint64_t>(data_); }
    bool is_number_integer() const { return is_number_unsigned() || std::holds_alternative<std::int64_t>(data_); }
    bool is_number_float() const { return std::holds_alternative<double>(data_); }
    bool is_number() const { return is_number_integer() || is_number_float(); }
    bool is_string() const { return std::holds_alternative<std::string>(data_); }
    bool is_array() const { return std::holds_alternative<Array>(data_); }
    bool is_object() const { return std::holds_alternative<Object>(data_); }
    std::size_t size() const {
        if (is_object()) return std::get<Object>(data_).size();
        if (is_array()) return std::get<Array>(data_).size();
        return is_null() ? 0 : 1;
    }
    bool empty() const { return size() == 0; }
    const std::string& string() const { return std::get<std::string>(data_); }
    template<class T> T get() const {
        if constexpr (std::same_as<T, Json>) return *this;
        else if constexpr (std::same_as<T, std::string>) return string();
        else if constexpr (std::same_as<T, bool>) return std::get<bool>(data_);
        else if constexpr (std::is_arithmetic_v<T>) {
            if (is_number_unsigned()) return static_cast<T>(std::get<std::uint64_t>(data_));
            if (std::holds_alternative<std::int64_t>(data_)) return static_cast<T>(std::get<std::int64_t>(data_));
            if (is_number_float()) return static_cast<T>(std::get<double>(data_));
            throw std::runtime_error("JSON value must be numeric");
        } else static_assert(sizeof(T) == 0, "Unsupported JSON conversion");
    }
    bool contains(std::string_view key) const {
        return is_object() && std::get<Object>(data_).contains(key);
    }
    const Json& at(std::string_view key) const {
        const auto& fields = std::get<Object>(data_);
        const auto found = fields.find(key);
        if (found == fields.end()) throw std::out_of_range("Missing JSON field: " + std::string(key));
        return found->second;
    }
    Json& at(std::string_view key) { return const_cast<Json&>(std::as_const(*this).at(key)); }
    const Json& at(std::size_t index) const { return std::get<Array>(data_).at(index); }
    Json& at(std::size_t index) { return std::get<Array>(data_).at(index); }
    Json& operator[](std::string_view key) {
        if (is_null()) data_ = Object{};
        return std::get<Object>(data_)[std::string(key)];
    }
    const Json& operator[](std::string_view key) const { return at(key); }
    Json& operator[](std::size_t index) { return at(index); }
    const Json& operator[](std::size_t index) const { return at(index); }
    template<class T> T value(std::string_view key, const T& fallback) const {
        if (!is_object()) throw std::runtime_error("JSON value must be an object");
        return contains(key) ? at(key).get<T>() : fallback;
    }
    std::string value(std::string_view key, const char* fallback) const {
        return value(key, std::string(fallback));
    }
    Object& items() { return std::get<Object>(data_); }
    const Object& items() const { return std::get<Object>(data_); }
    std::size_t erase(std::string_view key) { return items().erase(std::string(key)); }
    void push_back(Json value) {
        if (is_null()) data_ = Array{};
        std::get<Array>(data_).push_back(std::move(value));
    }

    template<bool Const> class Iterator {
        using Owner = std::conditional_t<Const, const Json, Json>;
        using Member = std::conditional_t<Const, Object::const_iterator, Object::iterator>;
        Owner* owner_{};
        Member member_{};
        std::size_t index_{};
        friend class Json;
        Iterator(Owner* owner, bool end) : owner_(owner), index_(end ? owner->size() : 0) {
            if (owner->is_object()) member_ = end ? owner->items().end() : owner->items().begin();
        }
    public:
        using value_type = Json;
        using difference_type = std::ptrdiff_t;
        using reference = Owner&;
        using pointer = Owner*;
        using iterator_category = std::forward_iterator_tag;
        Iterator() = default;
        reference operator*() const { return owner_->is_object() ? member_->second : owner_->at(index_); }
        pointer operator->() const { return &**this; }
        Iterator& operator++() { if (owner_->is_object()) ++member_; else ++index_; return *this; }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator& other) const {
            if (owner_ != other.owner_) return false;
            return owner_ && owner_->is_object() ? member_ == other.member_ : index_ == other.index_;
        }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    iterator begin() { return iterator(this, false); }
    iterator end() { return iterator(this, true); }
    const_iterator begin() const { return const_iterator(this, false); }
    const_iterator end() const { return const_iterator(this, true); }
    iterator find(std::string_view key) {
        auto result = end(); if (is_object()) result.member_ = items().find(key); return result;
    }
    const_iterator find(std::string_view key) const {
        auto result = end(); if (is_object()) result.member_ = items().find(key); return result;
    }
    bool operator==(const Json& other) const;
    bool operator<(const Json& other) const;
    bool operator>(const Json& other) const { return other < *this; }
    bool operator<=(const Json& other) const { return *this == other || *this < other; }
    bool operator>=(const Json& other) const { return *this == other || other < *this; }

private:
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object> data_;
};

// Small readers and writers shared by code that handles JSON by hand. In their own namespace so
// that a file's private helper of the same name is not made ambiguous by lookup through Json.
namespace json {
// An object's string field, or "" when the value is not an object or the field is missing or
// holds something other than a string.
inline std::string text_of(const Json& object, std::string_view key) {
    return object.is_object() && object.contains(key) && object.at(key).is_string() ? object.at(key).string() : std::string();
}

// Text as a JSON string, quotes included: the quote and the backslash escaped, control
// characters as a four-digit hex escape, every other byte (UTF-8 too) as it is.
inline std::string quoted(std::string_view text) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result{"\""};
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            result += "\\u00";
            result += digits[c >> 4];
            result += digits[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}
} // namespace json
}
