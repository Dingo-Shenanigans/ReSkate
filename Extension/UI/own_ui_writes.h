#pragma once
#include <utility>

namespace dingosdk::ui {
// While ReSkate's own native menus and HUD write their labels (on this thread), the trick feed
// ignores every UI write: those are not skate.'s trick list.
inline thread_local bool writing_own_ui = false;
struct OwnUiWrites {
    bool previous = std::exchange(writing_own_ui, true);
    ~OwnUiWrites() { writing_own_ui = previous; }
};
} // namespace dingosdk::ui
