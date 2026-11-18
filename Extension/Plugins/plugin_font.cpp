#include "plugin_font.h"
#include "plugin_font_directwrite.h"
#include "plugin_manifest.h"
#include <dwrite_1.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

// Plugins' fonts as DirectWrite reads them: the loaders that hand it a file's bytes from memory
// (reskate.picture's texts and the overlay's share them), a font's metrics for the texts plugins
// measure, and its glyphs as grey coverage for the overlay's glyph pages.
namespace dingosdk::plugins::font {
std::string read_file(const std::filesystem::path& file, std::size_t most) {
    std::error_code error;
    if (file.empty() || !std::filesystem::is_regular_file(file, error)) return {};
    const auto size = std::filesystem::file_size(file, error);
    if (error || !size || size > most) return {};
    std::ifstream in(file, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in || !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return {};
    return bytes;
}

namespace {
using FontBytes = std::shared_ptr<const std::string>;
class FontStream final : public Unknown<IDWriteFontFileStream> {
public:
    explicit FontStream(FontBytes bytes) : bytes_(std::move(bytes)) {}
    HRESULT STDMETHODCALLTYPE ReadFileFragment(const void** start, UINT64 offset, UINT64 size, void** context) override {
        *context = nullptr;
        if (offset > bytes_->size() || size > bytes_->size() - offset) {
            *start = nullptr;
            return E_FAIL;
        }
        *start = bytes_->data() + offset;
        return S_OK;
    }
    void STDMETHODCALLTYPE ReleaseFileFragment(void*) override {}
    HRESULT STDMETHODCALLTYPE GetFileSize(UINT64* size) override {
        *size = bytes_->size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64* time) override {
        *time = 0; // (not known: nothing is cached by it)
        return S_OK;
    }

private:
    FontBytes bytes_;
};
class FontLoader final : public Unknown<IDWriteFontFileLoader> {
public:
    void hold(std::uint64_t id, FontBytes bytes) {
        std::lock_guard lock(mutex_);
        files_[id] = std::move(bytes);
    }
    void drop(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        files_.erase(id);
    }
    HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* key, UINT32 size, IDWriteFontFileStream** stream) override {
        *stream = nullptr;
        if (!key || size != sizeof(std::uint64_t)) return E_INVALIDARG;
        std::uint64_t id{};
        std::memcpy(&id, key, sizeof id);
        std::lock_guard lock(mutex_);
        const auto found = files_.find(id);
        if (found == files_.end()) return E_FAIL;
        *stream = new FontStream(found->second);
        return S_OK;
    }

private:
    std::mutex mutex_;
    std::map<std::uint64_t, FontBytes> files_;
};
class FontFiles final : public Unknown<IDWriteFontFileEnumerator> {
public:
    FontFiles(IDWriteFactory* factory, IDWriteFontFileLoader* loader, std::uint64_t id) : factory_(factory), loader_(loader), id_(id) {}
    HRESULT STDMETHODCALLTYPE MoveNext(BOOL* has) override {
        *has = FALSE;
        if (given_) {
            file_.Reset();
            return S_OK;
        }
        given_ = true;
        const HRESULT made = factory_->CreateCustomFontFileReference(&id_, sizeof id_, loader_.Get(), &file_);
        if (SUCCEEDED(made)) *has = TRUE;
        return made;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** file) override {
        *file = file_.Get();
        if (!file_) return E_FAIL;
        file_->AddRef();
        return S_OK;
    }

private:
    ComPtr<IDWriteFactory> factory_;
    ComPtr<IDWriteFontFileLoader> loader_;
    std::uint64_t id_;
    bool given_{};
    ComPtr<IDWriteFontFile> file_;
};
class FontCollections final : public Unknown<IDWriteFontCollectionLoader> {
public:
    explicit FontCollections(IDWriteFontFileLoader* loader) : loader_(loader) {}
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory* factory, const void* key, UINT32 size, IDWriteFontFileEnumerator** files) override {
        *files = nullptr;
        if (!key || size != sizeof(std::uint64_t)) return E_INVALIDARG;
        std::uint64_t id{};
        std::memcpy(&id, key, sizeof id);
        *files = new FontFiles(factory, loader_.Get(), id);
        return S_OK;
    }

private:
    ComPtr<IDWriteFontFileLoader> loader_;
};
}

bool Library::ready() {
    if (factory) return true;
    ComPtr<IUnknown> made;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), &made)) || FAILED(made.As(&factory))) return false;
    factory.As(&factory2);
    loader_.Attach(new FontLoader);
    collections_.Attach(new FontCollections(loader_.Get()));
    if (FAILED(factory->RegisterFontFileLoader(loader_.Get())) || FAILED(factory->RegisterFontCollectionLoader(collections_.Get()))) {
        factory.Reset();
        factory2.Reset();
        return false;
    }
    return true;
}
void Library::drop(std::uint64_t id) {
    if (loader_) static_cast<FontLoader*>(loader_.Get())->drop(id);
}
void Library::read(Font& font, const std::filesystem::path& file) {
    if (font.id) drop(font.id);
    font.collection.Reset();
    font.face.Reset();
    font.metrics = {};
    font.family.clear();
    font.id = next_id_++;
    auto bytes = read_file(file, maximum_font_bytes);
    if (bytes.empty()) return;
    static_cast<FontLoader*>(loader_.Get())->hold(font.id, std::make_shared<const std::string>(std::move(bytes)));
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> names;
    ComPtr<IDWriteFont> matched;
    UINT32 length{};
    if (FAILED(factory->CreateCustomFontCollection(collections_.Get(), &font.id, sizeof font.id, &collection)) || !collection->GetFontFamilyCount() ||
        FAILED(collection->GetFontFamily(0, &family)) || FAILED(family->GetFamilyNames(&names)) || !names->GetCount() ||
        FAILED(names->GetStringLength(0, &length)) ||
        FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &matched))) {
        drop(font.id);
        return;
    }
    font.family.resize(length + 1);
    if (FAILED(names->GetString(0, font.family.data(), length + 1))) {
        drop(font.id);
        return;
    }
    font.family.resize(length);
    // (The face's own weight, style and width are asked for, so DirectWrite makes none of them up.)
    font.weight = matched->GetWeight();
    font.style = matched->GetStyle();
    font.stretch = matched->GetStretch();
    if (SUCCEEDED(matched->CreateFontFace(&font.face)) && font.face) font.face->GetMetrics(&font.metrics);
    font.collection = std::move(collection);
}
Font* Library::font_of(const std::filesystem::path& file, bool look) {
    if (file.empty() || !ready()) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    auto found = fonts_.find(file.native());
    if (found == fonts_.end()) {
        if (fonts_.size() >= most_fonts_) {
            auto oldest = fonts_.begin();
            for (auto it = fonts_.begin(); it != fonts_.end(); ++it)
                if (it->second.used < oldest->second.used) oldest = it;
            drop(oldest->second.id);
            fonts_.erase(oldest);
        }
        found = fonts_.emplace(file.native(), Font{}).first;
    } else if (!look && now - found->second.looked < std::chrono::milliseconds(500)) {
        found->second.used = ++uses_;
        return found->second.collection ? &found->second : nullptr;
    }
    auto& font = found->second;
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    const auto size = error ? 0 : std::filesystem::file_size(file, error);
    if (!font.id || error || written != font.written || size != font.size) {
        font.written = error ? std::filesystem::file_time_type{} : written;
        font.size = error ? 0 : size;
        read(font, file);
    }
    font.looked = now;
    font.used = ++uses_;
    return font.collection ? &font : nullptr;
}
void Library::forget(const std::filesystem::path& file) {
    const auto found = fonts_.find(file.native());
    if (found == fonts_.end()) return;
    drop(found->second.id);
    fonts_.erase(found);
}
ComPtr<IDWriteGlyphRunAnalysis> Library::coverage(const DWRITE_GLYPH_RUN& run, DWRITE_MEASURING_MODE measuring, float x, float y, DWRITE_TEXTURE_TYPE& kind) {
    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    // (With a grey analysis the "aliased" texture is the one that holds the grey: a byte a pixel.)
    kind = factory2 ? DWRITE_TEXTURE_ALIASED_1x1 : DWRITE_TEXTURE_CLEARTYPE_3x1;
    const HRESULT made = factory2
        ? factory2->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, measuring, DWRITE_GRID_FIT_MODE_DISABLED,
                                           DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, x, y, &analysis)
        : factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, measuring, x, y, &analysis);
    return SUCCEEDED(made) ? analysis : nullptr;
}

namespace {
using Metrics = plugin_ui::detail::FontMetrics;
constexpr std::size_t most_wide = 65536; // the characters past 127 a font's metrics keep

// The libraries for plugins' drawn texts, each with a factory of its own and so sharing nothing: one
// reads a font's metrics (on the plugin's thread, once a file) and one its glyphs (on the overlay's
// worker), so neither waits for the other. Kept for the process: DirectWrite may still hold a stream.
Library& measuring() {
    static auto* value = new Library(4);
    return *value;
}
Library& drawing() {
    static auto* value = new Library;
    return *value;
}

// A font's metrics from its face: design units over the line's height, so they are at size 1.
std::shared_ptr<const Metrics> read_metrics(const Font& font) {
    auto* face = font.face.Get();
    const float line = static_cast<float>(font.metrics.ascent) + static_cast<float>(font.metrics.descent);
    if (!face || !(line > 0)) return nullptr;
    auto m = std::make_shared<Metrics>();
    m->own = true;
    m->size = 1;
    m->ascent = static_cast<float>(font.metrics.ascent) / line;
    m->lacks.fill(true);
    // The characters 32 to 127: each one's glyph, its advance and the box its ink is in.
    {
        std::array<UINT32, 96> codes{};
        std::array<UINT16, 96> glyphs{};
        std::array<DWRITE_GLYPH_METRICS, 96> design{};
        for (std::size_t i = 0; i < codes.size(); ++i) codes[i] = static_cast<UINT32>(i + 32);
        if (FAILED(face->GetGlyphIndices(codes.data(), static_cast<UINT32>(codes.size()), glyphs.data())) ||
            FAILED(face->GetDesignGlyphMetrics(glyphs.data(), static_cast<UINT32>(glyphs.size()), design.data(), FALSE)))
            return nullptr;
        for (std::size_t i = 0; i < codes.size(); ++i) {
            if (!glyphs[i]) continue;
            const auto& g = design[i];
            m->lacks[i] = false;
            m->advance[i] = static_cast<float>(g.advanceWidth) / line;
            // Its ink from the pen on the baseline, y upward: left to right, and bottom to top.
            const float left = static_cast<float>(g.leftSideBearing), right = static_cast<float>(g.advanceWidth) - static_cast<float>(g.rightSideBearing);
            const float top = static_cast<float>(g.verticalOriginY) - static_cast<float>(g.topSideBearing);
            const float bottom = static_cast<float>(g.verticalOriginY) - static_cast<float>(g.advanceHeight) + static_cast<float>(g.bottomSideBearing);
            if (right > left && top > bottom)
                m->box[i] = {left / line, (static_cast<float>(font.metrics.ascent) - top) / line, right / line,
                             (static_cast<float>(font.metrics.ascent) - bottom) / line};
        }
    }
    // The characters past 127 the font has: the ranges its character map names where DirectWrite says
    // them (Windows 8 on), else every one of the first plane tried.
    std::vector<UINT32> codes;
    ComPtr<IDWriteFontFace1> face1;
    UINT32 count = 0;
    if (SUCCEEDED(font.face.As(&face1)) && face1) {
        face1->GetUnicodeRanges(0, nullptr, &count); // (how many there are)
        std::vector<DWRITE_UNICODE_RANGE> ranges(count);
        if (count && SUCCEEDED(face1->GetUnicodeRanges(count, ranges.data(), &count))) {
            for (const auto& range : ranges)
                for (std::uint64_t code = std::max<UINT32>(range.first, 128); code <= range.last && code <= 0x10ffff && codes.size() < most_wide; ++code)
                    codes.push_back(static_cast<UINT32>(code));
        }
    } else {
        for (UINT32 code = 128; code < 0x10000; ++code) codes.push_back(code);
    }
    // Their advances alone, where DirectWrite gives those by themselves (Windows 8 on): a glyph's
    // whole metrics have it look at the glyph's outline too, and a font can have tens of thousands.
    constexpr std::size_t batch = 4096;
    std::vector<UINT16> glyphs(batch);
    std::vector<INT32> advances(face1 ? batch : 0);
    std::vector<DWRITE_GLYPH_METRICS> design(face1 ? 0 : batch);
    for (std::size_t at = 0; at < codes.size(); at += batch) {
        const auto some = static_cast<UINT32>(std::min(batch, codes.size() - at));
        if (FAILED(face->GetGlyphIndices(codes.data() + at, some, glyphs.data())) ||
            FAILED(face1 ? face1->GetDesignGlyphAdvances(some, glyphs.data(), advances.data(), FALSE) : face->GetDesignGlyphMetrics(glyphs.data(), some, design.data(), FALSE)))
            break;
        for (UINT32 i = 0; i < some && m->wide.size() < most_wide; ++i)
            if (glyphs[i]) m->wide.emplace_back(codes[at + i], static_cast<float>(face1 ? advances[i] : static_cast<INT32>(design[i].advanceWidth)) / line);
    }
    std::sort(m->wide.begin(), m->wide.end());
    return m;
}

// The metrics read so far by their file, each with the file's time and size when it was read. A file
// that is not a font is kept too, with none, so it is read once and not at every question.
struct Known {
    std::filesystem::file_time_type written{};
    std::uintmax_t size{};
    std::shared_ptr<const Metrics> metrics;
    std::uint64_t used{};
};
struct Cache {
    std::mutex mutex;
    std::map<std::wstring, Known> known;
    std::uint64_t uses{};
    static constexpr std::size_t most = 64;
};
Cache& cache() {
    static auto* value = new Cache;
    return *value;
}
}

std::shared_ptr<const plugin_ui::detail::FontMetrics> metrics(const std::filesystem::path& file) {
    try {
        std::error_code error;
        const auto written = std::filesystem::last_write_time(file, error);
        const auto size = error ? 0 : std::filesystem::file_size(file, error);
        if (error) return nullptr;
        auto& c = cache();
        {
            std::lock_guard lock(c.mutex);
            if (const auto found = c.known.find(file.native()); found != c.known.end() && found->second.written == written && found->second.size == size) {
                found->second.used = ++c.uses;
                return found->second.metrics;
            }
        }
        std::shared_ptr<const Metrics> read;
        {
            auto& library = measuring();
            std::lock_guard lock(library.mutex);
            if (const auto* font = library.font_of(file, true)) read = read_metrics(*font);
            library.forget(file); // (its metrics are kept here: the file's bytes need not be)
        }
        std::lock_guard lock(c.mutex);
        if (c.known.size() >= Cache::most && !c.known.contains(file.native())) {
            auto oldest = c.known.begin();
            for (auto it = c.known.begin(); it != c.known.end(); ++it)
                if (it->second.used < oldest->second.used) oldest = it;
            c.known.erase(oldest);
        }
        c.known[file.native()] = Known{written, size, read, ++c.uses};
        return read;
    } catch (const std::exception&) {
        return nullptr;
    }
}
std::shared_ptr<const plugin_ui::detail::FontMetrics> known_metrics(const std::filesystem::path& file) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const auto found = c.known.find(file.native());
    return found == c.known.end() ? nullptr : found->second.metrics;
}

bool rasterise(const std::filesystem::path& file, float size, std::span<const std::uint32_t> codes, int most_side, std::vector<Coverage>& out) {
    try {
        out.clear();
        if (!(size > 0) || codes.empty()) return size > 0;
        auto& library = drawing();
        std::lock_guard lock(library.mutex);
        // (Looked at now: a font file written again is drawn from as it is, not as it was.)
        const auto* font = library.font_of(file, true);
        const float line = font ? static_cast<float>(font->metrics.ascent) + static_cast<float>(font->metrics.descent) : 0.0f;
        if (!font || !font->face || !(line > 0)) return false;
        const float em = size * static_cast<float>(font->metrics.designUnitsPerEm) / line;
        std::vector<UINT32> wanted(codes.begin(), codes.end());
        std::vector<UINT16> glyphs(wanted.size());
        if (FAILED(font->face->GetGlyphIndices(wanted.data(), static_cast<UINT32>(wanted.size()), glyphs.data()))) return false;
        out.resize(codes.size());
        std::vector<std::uint8_t> three; // ClearType's bytes, where DirectWrite gives no grey
        for (std::size_t i = 0; i < codes.size(); ++i) {
            auto& glyph = out[i];
            glyph.code = codes[i];
            if (!glyphs[i]) continue;
            const FLOAT advance = 0;
            const DWRITE_GLYPH_OFFSET offset{};
            DWRITE_GLYPH_RUN run{};
            run.fontFace = font->face.Get();
            run.fontEmSize = em;
            run.glyphCount = 1;
            run.glyphIndices = &glyphs[i];
            run.glyphAdvances = &advance;
            run.glyphOffsets = &offset;
            DWRITE_TEXTURE_TYPE kind{};
            const auto analysis = library.coverage(run, DWRITE_MEASURING_MODE_NATURAL, 0, 0, kind);
            RECT bounds{};
            if (!analysis || FAILED(analysis->GetAlphaTextureBounds(kind, &bounds))) continue;
            const auto wide = bounds.right - bounds.left, tall = bounds.bottom - bounds.top;
            if (wide <= 0 || tall <= 0) {
                glyph.kind = Coverage::Kind::blank;
                continue;
            }
            if (wide > most_side || tall > most_side) continue;
            const auto pixels = static_cast<std::size_t>(wide) * static_cast<std::size_t>(tall);
            glyph.grey.resize(pixels);
            if (kind == DWRITE_TEXTURE_ALIASED_1x1) {
                if (FAILED(analysis->CreateAlphaTexture(kind, &bounds, glyph.grey.data(), static_cast<UINT32>(pixels)))) {
                    glyph.grey.clear();
                    continue;
                }
            } else {
                three.resize(pixels * 3);
                if (FAILED(analysis->CreateAlphaTexture(kind, &bounds, three.data(), static_cast<UINT32>(three.size())))) {
                    glyph.grey.clear();
                    continue;
                }
                for (std::size_t p = 0; p < pixels; ++p) glyph.grey[p] = static_cast<std::uint8_t>((three[p * 3] + three[p * 3 + 1] + three[p * 3 + 2] + 1) / 3);
            }
            glyph.kind = Coverage::Kind::drawn;
            glyph.left = static_cast<int>(bounds.left);
            glyph.top = static_cast<int>(bounds.top);
            glyph.width = static_cast<int>(wide);
            glyph.height = static_cast<int>(tall);
        }
        return true;
    } catch (const std::exception&) {
        out.clear();
        return false;
    }
}
}
