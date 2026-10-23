#pragma once
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// The Animator Studio MARKET's art (assets/animator_studio/market, made by contrib/animator-studio/market_art and built
// into ReSkate.dll): the wall behind the shop, sharp at 3840 x 1620 in a texture of its own; and one sheet of the rest -
// the tiling grip tape and papers the shop's torn paper is cut from, the skater's print and the blue splatters. Read
// off the render thread the first time the MARKET is opened, and kept on the GPU from then on.
namespace dingosdk::overlay {
enum class MarketArt { wallpaper, grip, paper_white, paper_blue, skater, splatter_a, splatter_b, count };
// Asked for each frame the MARKET is drawn: the first ask starts reading it.
void want_market_art() noexcept;
// Where a picture is on the GPU. For the tiling ones (grip, the papers) uv0..uv1 is one tile without the wrapped
// border round it (`border` texels of it each side, so a quad may run a little past the tile). False until it is there.
struct ArtPlace {
    ImTextureID texture{};
    ImVec2 uv0{}, uv1{};
    float width{}, height{}; // the picture's own size, in texels
    float border{};          // a tile's wrapped border, in texels
};
bool market_art(MarketArt art, ArtPlace& out) noexcept;
// THE CREW's own pictures (built in from assets/animator_studio/crew), on the same sheet: a member's round profile
// picture and the wide shot across the top of their cards, by their team (BUILT BY: by their pictures' name). False for
// one who has not sent theirs, and until it is there.
enum class CrewArt { profile, thumbnail };
bool crew_art(std::string_view team, CrewArt which, ArtPlace& out) noexcept;

// For the cover maker, off the render thread: a picture of the market's art decoded from the DLL (its own size, RGBA),
// by its resource name (STUDIO_ART_...); and a font file the studio carries (FONT_...), as it is in the DLL.
struct ArtPixels {
    std::vector<std::uint8_t> rgba;
    int width{}, height{};
};
std::optional<ArtPixels> decode_studio_resource(const wchar_t* resource, int most_wide = 0);
std::optional<ArtPixels> decode_picture_file(const wchar_t* path, int most_wide = 0);
std::pair<const unsigned char*, std::size_t> studio_resource_bytes(const wchar_t* resource);

// The cover the designer is showing, handed to the GPU (its own texture). `serial` goes up with each new one.
void show_cover_preview(std::vector<std::uint8_t> rgba, int side);
bool cover_preview(ImTextureID& texture) noexcept;
} // namespace dingosdk::overlay
