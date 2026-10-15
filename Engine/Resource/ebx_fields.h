#pragma once
#include "ebx_document.h"
#include <stdexcept>
#include <string>
#include <string_view>

namespace dingosdk::frostbite::ebx {
// The value as a T. Throws std::runtime_error(what) when it holds another type.
template <class T> const T& as(const Value& value, std::string_view what) {
    const auto* result = std::get_if<T>(&value.data);
    if (!result) throw std::runtime_error(std::string(what));
    return *result;
}
// The field `name`. Throws std::runtime_error(what) when the object has none.
inline const Value& field(const Object& object, std::string_view name, std::string_view what) {
    const auto* found = object.find(name);
    if (!found) throw std::runtime_error(std::string(what));
    return found->value;
}
}
