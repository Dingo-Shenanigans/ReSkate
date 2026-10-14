#include "overlay_internal.h"
#include "Extension/GraphicsCreator/graphic_raster.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace dingosdk::overlay::detail {
namespace {
std::mutex preview_mutex;
CreatePixels current;
std::atomic<ImTextureID> texture{};
std::atomic<bool> waiting{};
std::atomic<bool> busy{};
std::atomic<unsigned> serial{};
std::string requested;
}

void request_create_preview(const graphics_creator::GraphicDocument& document,
                            const std::filesystem::path& asset_directory) {
    std::string fingerprint;
    try { fingerprint = graphics_creator::encode(document); }
    catch (...) { return; }
    {
        std::lock_guard lock(preview_mutex);
        if (fingerprint == requested) return;
    }
    if (busy.exchange(true)) return;
    {
        std::lock_guard lock(preview_mutex);
        requested = fingerprint;
    }
    try {
        std::thread([document, asset_directory] {
            CreatePixels made;
            try {
                auto image = graphics_creator::rasterize(document, asset_directory, 1024);
                made.rgba = std::move(image.rgba);
                made.width = image.width;
                made.height = image.height;
            } catch (...) {}
            {
                std::lock_guard lock(preview_mutex);
                made.serial = ++serial;
                current = std::move(made);
                waiting.store(!current.rgba.empty());
                texture.store(0);
            }
            busy.store(false);
        }).detach();
    } catch (...) { busy.store(false); }
}

ImTextureID create_preview_texture() noexcept { return texture.load(); }

bool take_create_pixels(CreatePixels& out) {
    std::lock_guard lock(preview_mutex);
    if (!waiting.exchange(false)) return false;
    out = current;
    return true;
}

void create_preview_uploaded(ImTextureID value, unsigned uploaded_serial) {
    std::lock_guard lock(preview_mutex);
    if (uploaded_serial == serial.load()) texture.store(value);
}

void create_preview_lost() {
    std::lock_guard lock(preview_mutex);
    texture.store(0);
    waiting.store(!current.rgba.empty());
}
}
