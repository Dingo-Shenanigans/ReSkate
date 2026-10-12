#include "style.h"
#include "style_layer.h"

namespace dingosdk::style {
void on_client_tick(std::uintptr_t base, std::uintptr_t client, bool multiplayer_ready) noexcept {
    style_layer::tick(base, client, multiplayer_ready);
}

StyleModel model() {
    auto result = style_layer::model();
    return result;
}
} // namespace dingosdk::style
