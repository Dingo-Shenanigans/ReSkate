#include "Extension/UI/Overlay/material_method_hooks.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace dingosdk::overlay::detail;
struct Object
{
    void **table;
};
using Method = int (*)(Object *, int);
material_method_hooks<Method, 0, 2> hooks;
std::map<void *, Method> intercepted;
Object *nested{};
bool throw_inside{};
int callbacks{}, first_calls{}, second_calls{}, installs{};
void check(bool condition)
{
    if (!condition)
        std::abort();
}
int second(Object *, int value)
{
    ++second_calls;
    return value + 20;
}
int first(Object *, int value)
{
    ++first_calls;
    if (nested)
        return intercepted.at(nested->table[0])(nested, value) + 10;
    return value + 10;
}
int third(Object *, int value)
{
    return value + 30;
}
int callback(Object *object, int value)
{
    ++callbacks;
    const auto own = hooks.load();
    if (throw_inside)
        throw std::runtime_error("test unwind");
    const auto result = own(object, value);
    check(hooks.load() == own); // Nested native call must restore its wrapper relay.
    return result;
}
int main()
{
    std::array<void *, 1> a{reinterpret_cast<void *>(&first)}, b{reinterpret_cast<void *>(&second)},
        c{reinterpret_cast<void *>(&third)};
    Object wrapper{a.data()}, native{b.data()}, extra{c.data()};
    const auto install = [](void *target, void *replacement, auto publish) {
        ++installs;
        publish(target);
        intercepted.emplace(target, reinterpret_cast<Method>(replacement));
        return true;
    };
    check(!hooks.installed());
    check(hooks.install(&wrapper, &callback, install));
    check(hooks.installed() && !hooks.load());
    check(hooks.install(&native, &callback, install));
    check(hooks.install(&native, &callback, install) && installs == 2);
    check(intercepted.at(a[0])(&wrapper, 1) == 11);
    check(intercepted.at(b[0])(&native, 1) == 21);
    nested = &native;
    check(intercepted.at(a[0])(&wrapper, 1) == 31);
    check(callbacks == 4 && first_calls == 2 && second_calls == 2);
    check(!hooks.load() && hooks.installed());
    check(hooks.load(&wrapper) == &first && hooks.load(&native) == &second);
    check(!hooks.install(&extra, &callback, install) && installs == 2);
    throw_inside = true;
    try
    {
        intercepted.at(a[0])(&wrapper, 1);
        check(false);
    }
    catch (const std::runtime_error &)
    {
    }
    check(!hooks.load() && hooks.installed());
    std::cout << "PASS: distinct implementations, shared targets, nested forwarding, relay restoration, exception "
                 "unwind and bounded capacity\n";
}
