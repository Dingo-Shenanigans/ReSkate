// The cover maker on its own (studio_cover_maker.h): the designer's choices one by one,
// written as PNGs to look at - the same code, art and fonts the studio uses (the art and fonts are built into this tool as
// they are into ReSkate.dll). Usage: dingosdk_cover_maker_preview <folder>
#include "Extension/UI/Overlay/studio_cover_maker.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace dingosdk;

int main(int argc, char** argv) {
    const std::filesystem::path out = argc > 1 ? argv[1] : "covers";
    std::filesystem::create_directories(out);
    const auto write = [&](const std::string& name, const overlay::CoverDesign& design) {
        const auto png = overlay::cover_png(overlay::make_cover(design, 512), 512);
        if (png.empty()) {
            std::printf("%s: could not be made\n", name.c_str());
            return false;
        }
        std::ofstream(out / (name + ".png"), std::ios::binary).write(png.data(), static_cast<std::streamsize>(png.size()));
        std::printf("%s: %zu bytes\n", name.c_str(), png.size());
        return true;
    };
    int made = 0;
    // Each banner, in each place on the cover.
    std::uint32_t seed = 3;
    for (const char* banner : {"blue", "white", "grip", "tape", "none"})
        for (const char* place : {"top", "middle", "bottom"}) {
            overlay::CoverDesign c;
            c.base = "concrete";
            c.banner = banner;
            c.place = place;
            c.title = "Flip Theory";
            c.tag = "ALL STANCES";
            c.seed = seed += 7;
            made += write(std::string("banner_") + banner + "_" + place, c);
        }
    // Each base under the same banner, and each font on the same base.
    for (const char* base : {"skater", "print", "grip", "concrete", "paper", "blue"}) {
        overlay::CoverDesign c;
        c.base = base;
        c.title = "Street Essentials";
        c.tag = "GOOFY";
        made += write(std::string("base_") + base, c);
    }
    for (const char* font : {"anton", "bebas", "dirt", "sedgwick", "knewave"}) {
        overlay::CoverDesign c;
        c.base = "grip";
        c.banner = "white";
        c.place = "middle";
        c.font = font;
        c.title = "Heavy Hitters";
        made += write(std::string("font_") + font, c);
    }
    overlay::release_cover_art();
    std::printf("%d covers\n", made);
    return made > 0 ? 0 : 1;
}
