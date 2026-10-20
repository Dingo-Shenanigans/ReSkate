#include "animation_studio_images.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/ui_textures.h"
#include "Engine/Vfs/game_textures.h"
#include "Engine/Resource/texture.h"
#include "Extension/HallOfMeat/hall_of_meat_skeleton.h"
#include <imgui.h>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace dingosdk::overlay {
namespace {
namespace ui = addr::ui_textures;

// skate.'s own loading screen (UI/Foundations/Loading/LoadScreen/DingoLoadScreen_Widget) shows this
// board doing kickflips beside its "Loading..." (found 2026-10-18 in the widget's imports): a
// RimeFlipbookTextureAsset, flip_Skateboard_Kickflip_256, of 27 frames at 30 a second, 9 across and 3
// down its atlas (FrameUvs): a RimeTextureAsset whose texture for a 2160 screen is this one (2304 x 768, read at half: it must fit the 2048-wide atlas).
// Where it is kept: tried in this order.
constexpr std::string_view kickflip_name = "ui/textures/common/flipbooks/loading/flip_skateboard_kickflip_256_win32_"
                                           "6143a1fc-cfd7-13ed-c764-164f1af9c663_0_2160_c2cf948c-5744-ac69-b92d-360134ba7b45_input";
constexpr int kickflip_frames = 27, kickflip_columns = 9, kickflip_rows = 3;
constexpr double kickflip_rate = 30;
struct Place {
    std::string_view toc, bundle;
};
constexpr std::array<Place, 3> kickflip_places{{
    {"Win32/globals.toc", "win32/ui/foundations/loading/loadscreen/dingoloadscreen_widget_bundle"},
    {ui::root_toc, ui::root_bundle},
    {ui::bam_toc, ui::bam_bundle},
}};

constexpr std::size_t picture_count = 6, backdrop_index = 4, backdrop_right_index = 5;
struct Source {
    const ui::Texture* texture; // the kickflip's is found in kickflip_places
    std::uint32_t side;
    bool brightness{}; // white with the texture's brightness as alpha, else its own alpha
};
constexpr std::array<Source, picture_count> sources{{
    {&ui::scratches, 1024, true}, {&ui::rough_tile, 256}, {&ui::brush_bar, 692}, {nullptr, 1152}, {nullptr, 0}, {nullptr, 0},
}};
using Images = std::array<std::optional<frostbite::Image>, picture_count>;

struct Reads {
    std::mutex mutex;
    bool started{};
    std::future<Images> pending;
    Images decoded;
};
Reads& reads() {
    static auto* state = new Reads;
    return *state;
}

// White, with its own alpha or its brightness as alpha.
frostbite::Image whiten(frostbite::Image image, bool brightness) {
    if (!image.width || !image.height || image.rgba.size() != std::size_t{image.width} * image.height * 4)
        throw std::runtime_error("the texture is empty");
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        auto* p = image.rgba.data() + i;
        if (brightness) p[3] = std::max({p[0], p[1], p[2]});
        p[0] = p[1] = p[2] = 255;
    }
    return image;
}

// The loading screen's picture. The one built into ReSkate.dll (assets/animator_studio/loading.jpg, baked
// in by cmake/Runtime.cmake) is what everyone who installs the mod sees; a RESKATE_STUDIO_BACKDROP file, or
// backdrop.png in the folder above the tricks (RESKATE_ANIMATION_STUDIO_DIR), takes its place when there is
// one. Kept at up to 3072 across, which is more than a 16:9 screen at 1080 lines shows of it.
std::filesystem::path backdrop_path() {
    std::array<wchar_t, 32768> buffer{};
    if (const auto length = GetEnvironmentVariableW(L"RESKATE_STUDIO_BACKDROP", buffer.data(), static_cast<DWORD>(buffer.size())); length && length < buffer.size())
        return std::wstring(buffer.data(), length);
    if (const auto length = GetEnvironmentVariableW(L"RESKATE_ANIMATION_STUDIO_DIR", buffer.data(), static_cast<DWORD>(buffer.size())); length && length < buffer.size())
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path() / L"backdrop.png";
    return {};
}
// The picture baked into the DLL.
std::vector<unsigned char> built_in_backdrop() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&built_in_backdrop), &module))
        return {};
    const auto found = FindResourceW(module, L"STUDIO_LOADING", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = found ? LoadResource(module, found) : nullptr;
    const auto* bytes = handle ? static_cast<const unsigned char*>(LockResource(handle)) : nullptr;
    const auto size = found ? SizeofResource(module, found) : 0;
    return bytes && size ? std::vector<unsigned char>(bytes, bytes + size) : std::vector<unsigned char>{};
}
std::optional<frostbite::Image> read_backdrop() {
    const auto path = backdrop_path();
    std::error_code error;
    const bool own = !path.empty() && std::filesystem::is_regular_file(path, error);
    std::vector<unsigned char> bytes;
    if (own) {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else bytes = built_in_backdrop();
    if (bytes.empty() || bytes.size() > (64u << 20)) return std::nullopt;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::optional<frostbite::Image> out;
    try {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        UINT width{}, height{};
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width && height && width <= 8192 && height <= 8192 &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            frostbite::Image image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
            if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()))) {
                const float scale = std::min({1.0f, 3072.0f / static_cast<float>(width), 1728.0f / static_cast<float>(height)});
                out = scale < 1.0f ? frostbite::resize(image, std::max(1u, static_cast<std::uint32_t>(width * scale)), std::max(1u, static_cast<std::uint32_t>(height * scale)))
                                   : std::move(image);
            }
        }
    } catch (...) {
        out.reset();
    }
    if (com) CoUninitialize();
    if (out) logging::log(logging::Level::info, logging::Channel::graphics, "Animator Studio: the loading picture is {} x {}, from {}.",
                         out->width, out->height, own ? path.string() : std::string("the one built in"));
    else logging::log(logging::Level::info, logging::Channel::graphics, "Animator Studio: no loading picture was read.");
    return out;
}
// One half of the picture. The overlay's atlas is 2048 across, so a wide picture goes in as two rects and
// is drawn back together; cut on a whole pixel, and the atlas keeps a gap round each, so there is no seam.
std::optional<frostbite::Image> half_of(const frostbite::Image& image, bool right) {
    const std::uint32_t cut = image.width / 2;
    const std::uint32_t from = right ? cut : 0, width = right ? image.width - cut : cut;
    if (!width || !image.height) return std::nullopt;
    frostbite::Image out{width, image.height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * image.height * 4)};
    for (std::uint32_t y = 0; y < image.height; ++y)
        std::memcpy(out.rgba.data() + static_cast<std::size_t>(y) * width * 4,
                    image.rgba.data() + (static_cast<std::size_t>(y) * image.width + from) * 4, static_cast<std::size_t>(width) * 4);
    return out;
}

Images decode() {
    Images result;
    try {
        vfs::GameTextures textures(hall_of_meat::game_folder());
        for (std::size_t index = 0; index < picture_count; ++index) {
            if (index == backdrop_index) {
                if (const auto whole = read_backdrop()) {
                    result[backdrop_index] = half_of(*whole, false);
                    result[backdrop_right_index] = half_of(*whole, true);
                }
                continue;
            }
            if (index == backdrop_right_index) continue; // filled with its left half
            const auto& source = sources[index];
            if (!source.texture) {
                for (const auto& place : kickflip_places) {
                    try {
                        // Its own colours: the board is drawn as it is.
                        auto image = textures.read(place.toc, place.bundle, kickflip_name, source.side);
                        logging::log(logging::Level::info, logging::Channel::graphics,
                                     "Animator Studio: the loading board is {} x {}, in {}.", image.width, image.height, place.bundle);
                        result[index] = std::move(image);
                        break;
                    } catch (const std::exception& error) {
                        logging::log(logging::Level::info, logging::Channel::graphics, "Animator Studio: the loading board is not in {}: {}.",
                                     place.bundle, error.what());
                    }
                }
                if (!result[index])
                    logging::log(logging::Level::warning, logging::Channel::graphics, "Animator Studio: the loading board was not found.");
                continue;
            }
            try {
                result[index] = whiten(textures.read(source.texture->toc, source.texture->bundle, source.texture->name, source.side),
                                       source.brightness);
            } catch (const std::exception& error) {
                logging::log(logging::Level::warning, logging::Channel::graphics, "Animator Studio picture {} is unavailable: {}.",
                             source.texture->name, error.what());
            }
        }
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Animator Studio's pictures are unavailable: {}.", error.what());
    }
    return result;
}

// Render thread only.
struct Loaded {
    ImFontAtlas* atlas{};
    bool filled{};
    std::array<int, picture_count> rects{};
    std::array<std::pair<std::uint32_t, std::uint32_t>, picture_count> sizes{};
};
Loaded& loaded() {
    static Loaded state;
    return state;
}
struct Found {
    const ImFontAtlasCustomRect* rect{};
    ImVec2 uv0, uv1;
};
bool find(StudioPicture picture, Found& found) {
    const auto& l = loaded();
    const auto index = static_cast<std::size_t>(picture);
    if (!l.filled || !l.atlas || ImGui::GetIO().Fonts != l.atlas || !l.atlas->TexID || l.rects[index] < 0) return false;
    found.rect = l.atlas->GetCustomRectByIndex(l.rects[index]);
    if (!found.rect || !found.rect->IsPacked() || !found.rect->Width || !found.rect->Height) return false;
    l.atlas->CalcCustomRectUV(found.rect, &found.uv0, &found.uv1);
    return true;
}
} // namespace

void prepare_animation_studio_images() noexcept {
    try {
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        if (std::exchange(state.started, true)) return;
        state.pending = std::async(std::launch::async, &decode);
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::graphics, "Animator Studio's pictures are unavailable: their reading could not start.");
    }
}

void reserve_animation_studio_images(ImFontAtlas& atlas, std::chrono::milliseconds wait) noexcept {
    auto& l = loaded();
    l = {};
    l.rects.fill(-1);
    try {
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        if (state.pending.valid() && state.pending.wait_for(wait) == std::future_status::ready) state.decoded = state.pending.get();
        for (std::size_t index = 0; index < picture_count; ++index) {
            const auto& image = state.decoded[index];
            if (!image) continue;
            if (!l.atlas) {
                atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048);
                atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
                l.atlas = &atlas;
            }
            l.rects[index] = atlas.AddCustomRectRegular(static_cast<int>(image->width), static_cast<int>(image->height));
            l.sizes[index] = {image->width, image->height};
        }
    } catch (...) {
        l = {};
    }
}

void fill_animation_studio_images(ImFontAtlas& atlas) noexcept {
    auto& l = loaded();
    if (l.atlas != &atlas) return;
    try {
        unsigned char* pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) {
            l = {};
            return;
        }
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        for (std::size_t index = 0; index < picture_count; ++index) {
            if (l.rects[index] < 0) continue;
            const auto& image = *state.decoded[index];
            const auto* rect = atlas.GetCustomRectByIndex(l.rects[index]);
            if (!rect || !rect->IsPacked() || rect->X + image.width > static_cast<unsigned>(width) ||
                rect->Y + image.height > static_cast<unsigned>(height)) {
                l = {};
                return;
            }
            for (std::uint32_t y = 0; y < image.height; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                            image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4, static_cast<std::size_t>(image.width) * 4);
        }
        atlas.TexPixelsUseColors = true;
        l.filled = true;
    } catch (...) {
        l = {};
    }
}

bool draw_studio_picture_cover(ImDrawList* draw, StudioPicture picture, ImVec2 min, ImVec2 max, ImU32 tint) {
    Found found;
    if (!find(picture, found)) return false;
    const auto& size = loaded().sizes[static_cast<std::size_t>(picture)];
    const float box_w = max.x - min.x, box_h = max.y - min.y;
    if (!(size.first && size.second && box_w > 0 && box_h > 0)) return false;
    const float picture_shape = static_cast<float>(size.first) / static_cast<float>(size.second);
    const float box_shape = box_w / box_h;
    // The part of the picture that is shown: the rest is cropped, half off each side.
    float u = 1.0f, v = 1.0f;
    if (picture_shape > box_shape) u = box_shape / picture_shape;
    else v = picture_shape / box_shape;
    const ImVec2 a(found.uv0.x + (found.uv1.x - found.uv0.x) * (1 - u) * 0.5f, found.uv0.y + (found.uv1.y - found.uv0.y) * (1 - v) * 0.5f);
    const ImVec2 b(found.uv1.x - (found.uv1.x - found.uv0.x) * (1 - u) * 0.5f, found.uv1.y - (found.uv1.y - found.uv0.y) * (1 - v) * 0.5f);
    draw->AddImage(loaded().atlas->TexID, min, max, a, b, tint);
    return true;
}
bool draw_studio_backdrop_cover(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 tint) {
    Found left, right;
    if (!find(StudioPicture::backdrop, left) || !find(StudioPicture::backdrop_right, right)) return false;
    const auto& left_size = loaded().sizes[static_cast<std::size_t>(StudioPicture::backdrop)];
    const auto& right_size = loaded().sizes[static_cast<std::size_t>(StudioPicture::backdrop_right)];
    const float whole_w = static_cast<float>(left_size.first + right_size.first), whole_h = static_cast<float>(left_size.second);
    const float box_w = max.x - min.x, box_h = max.y - min.y;
    if (!(whole_w > 0 && whole_h > 0 && box_w > 0 && box_h > 0)) return false;
    // What is shown of it: the rest is cropped, half off each side (or off the top and bottom).
    const float picture_shape = whole_w / whole_h, box_shape = box_w / box_h;
    float u = 1.0f, v = 1.0f;
    if (picture_shape > box_shape) u = box_shape / picture_shape;
    else v = picture_shape / box_shape;
    const float x0 = whole_w * (1 - u) * 0.5f, x1 = whole_w - x0;
    const float shown = x1 - x0;
    // The join. Filtering mixes each half's edge texel with what lies beside it in the atlas, which drew a
    // faint line down the middle, so each half stops half a texel short of its own edge there and the two
    // are made to overlap by a pixel on the screen, leaving nothing between them.
    const float texel = loaded().atlas->TexWidth > 0 ? 0.5f / static_cast<float>(loaded().atlas->TexWidth) : 0.0f;
    const auto piece = [&](const Found& found, float from, float to, float inset_left, float inset_right) {
        // Where this half sits in the whole picture, against what is shown of it.
        const float a = std::max(from, x0), b = std::min(to, x1);
        if (b <= a) return;
        const float ua = (a - from) / (to - from), ub = (b - from) / (to - from);
        const float span = found.uv1.x - found.uv0.x;
        const ImVec2 uv0(found.uv0.x + span * ua + inset_left, found.uv0.y + (found.uv1.y - found.uv0.y) * (1 - v) * 0.5f);
        const ImVec2 uv1(found.uv0.x + span * ub - inset_right, found.uv1.y - (found.uv1.y - found.uv0.y) * (1 - v) * 0.5f);
        draw->AddImage(loaded().atlas->TexID, ImVec2(min.x + (a - x0) / shown * box_w - (inset_left > 0 ? 1.0f : 0.0f), min.y),
                       ImVec2(min.x + (b - x0) / shown * box_w + (inset_right > 0 ? 1.0f : 0.0f), max.y), uv0, uv1, tint);
    };
    piece(left, 0, static_cast<float>(left_size.first), 0, texel);
    piece(right, static_cast<float>(left_size.first), whole_w, texel, 0);
    return true;
}
bool draw_studio_picture(ImDrawList* draw, StudioPicture picture, ImVec2 min, ImVec2 max, ImU32 tint) {
    Found found;
    if (!find(picture, found)) return false;
    draw->AddImage(loaded().atlas->TexID, min, max, found.uv0, found.uv1, tint);
    return true;
}
bool draw_studio_brush(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 tint) {
    Found found;
    if (!find(StudioPicture::brush, found)) return false;
    const auto& body = ui::brush_bar.body;
    const float width = (max.x - min.x) / (body.right - body.left), height = (max.y - min.y) / (body.bottom - body.top);
    const ImVec2 at(min.x - body.left * width, min.y - body.top * height);
    draw->AddImage(loaded().atlas->TexID, at, ImVec2(at.x + width, at.y + height), found.uv0, found.uv1, tint);
    return true;
}
bool draw_studio_tile(ImDrawList* draw, ImVec2 min, ImVec2 max, float edge, ImU32 tint) {
    Found found;
    if (!find(StudioPicture::tile, found)) return false;
    const float slice = ui::rough_tile.slice;
    const float edge_x = std::clamp(edge, 0.0f, (max.x - min.x) * 0.5f), edge_y = std::clamp(edge, 0.0f, (max.y - min.y) * 0.5f);
    const float uv_x = slice * (found.uv1.x - found.uv0.x), uv_y = slice * (found.uv1.y - found.uv0.y);
    const std::array<float, 4> xs{min.x, min.x + edge_x, max.x - edge_x, max.x}, ys{min.y, min.y + edge_y, max.y - edge_y, max.y};
    const std::array<float, 4> us{found.uv0.x, found.uv0.x + uv_x, found.uv1.x - uv_x, found.uv1.x};
    const std::array<float, 4> vs{found.uv0.y, found.uv0.y + uv_y, found.uv1.y - uv_y, found.uv1.y};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            draw->AddImage(loaded().atlas->TexID, ImVec2(xs[column], ys[row]), ImVec2(xs[column + 1], ys[row + 1]),
                           ImVec2(us[column], vs[row]), ImVec2(us[column + 1], vs[row + 1]), tint);
    return true;
}
bool draw_studio_kickflip(ImDrawList* draw, ImVec2 min, ImVec2 max, double seconds, ImU32 tint) {
    Found found;
    if (!find(StudioPicture::kickflip, found)) return false;
    // The frame at `seconds`, from the grid.
    const int frame = static_cast<int>(std::max(seconds, 0.0) * kickflip_rate) % kickflip_frames;
    const float column = static_cast<float>(frame % kickflip_columns), row = static_cast<float>(frame / kickflip_columns);
    const float du = (found.uv1.x - found.uv0.x) / kickflip_columns, dv = (found.uv1.y - found.uv0.y) / kickflip_rows;
    const ImVec2 uv0(found.uv0.x + column * du, found.uv0.y + row * dv), uv1(uv0.x + du, uv0.y + dv);
    const auto [w, h] = loaded().sizes[static_cast<std::size_t>(StudioPicture::kickflip)];
    const float fw = static_cast<float>(w) / kickflip_columns, fh = static_cast<float>(h) / kickflip_rows;
    const float scale = std::min((max.x - min.x) / fw, (max.y - min.y) / fh);
    const ImVec2 size(fw * scale, fh * scale);
    const ImVec2 at(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f);
    draw->AddImage(loaded().atlas->TexID, at, ImVec2(at.x + size.x, at.y + size.y), uv0, uv1, tint);
    return true;
}
}
