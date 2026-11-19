#pragma once
#include "plugin_ui.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

// A plugin's own fonts (its manifest's "fonts") for the texts it draws on the overlay: a font file's
// metrics, which measure a text, and its glyphs as grey coverage, which draw it. A plugin's font is
// untrusted, so Windows alone reads it: DirectWrite, given the file's bytes from memory
// (plugin_font_directwrite.h, which reskate.picture's texts use too). Nothing of ours, and not Dear
// ImGui's stb_truetype, parses one. Windows only (plugin_font.cpp).
namespace dingosdk::plugins::font {
// A font file's metrics as plugin_ui.h's FontMetrics (`own`): at size 1, each character's advance
// with no kerning, the box of each of the characters 32 to 127, which of those the font lacks, and
// every character past 127 it has (at most 65536). The size is a line's height, the font's ascent
// and descent together, as the menus' fonts' sizes are. Nothing for a file DirectWrite does not read
// as a font. Read once a file and kept, and read again when the file's time or size has changed.
// Any thread.
std::shared_ptr<const plugin_ui::detail::FontMetrics> metrics(const std::filesystem::path& file);
// The same when they have been read already, without looking at the file: nothing otherwise.
std::shared_ptr<const plugin_ui::detail::FontMetrics> known_metrics(const std::filesystem::path& file);

// One glyph as DirectWrite draws it with its pen at (0, 0) on the baseline: grey coverage, a byte a
// pixel, `width` x `height` of them, the box's top left at (`left`, `top`) from the pen (y downward).
struct Coverage {
    // blank: nothing to draw (a space). missing: the font has no glyph for it, or one too large to keep.
    enum class Kind : std::uint8_t { drawn, blank, missing } kind{Kind::missing};
    std::uint32_t code{};
    int left{}, top{}, width{}, height{};
    std::vector<std::uint8_t> grey;
};
// The glyphs of `codes` in a font file at `size` pixels a line, one for each code and in their order;
// one wider or taller than `most_side` pixels is missing. False when the font cannot be read. Greys
// are antialiased coverage, never ClearType's. Any thread, one call at a time.
bool rasterise(const std::filesystem::path& file, float size, std::span<const std::uint32_t> codes, int most_side, std::vector<Coverage>& out);
}
