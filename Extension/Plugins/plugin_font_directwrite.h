#pragma once
#include <Windows.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

// DirectWrite with plugins' fonts in it (Windows only; plugin_font.cpp). A plugin's font is a file of
// its package handed to DirectWrite from memory: a font file loader that gives its bytes by a number,
// and a collection of that one file. The factory is a library's alone (isolated), so nothing a
// plugin's font does reaches the fonts the rest of the process uses. reskate.picture's texts
// (plugin_picture.cpp) and the texts plugins draw on the overlay (plugin_font.h) both read fonts
// through this, and nothing else of ours does.
namespace dingosdk::plugins::font {
using Microsoft::WRL::ComPtr;

// A COM object of ours: one interface, counted, deleted with its last reference.
template <class Interface>
class Unknown : public Interface {
public:
    virtual ~Unknown() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(Interface)) {
            *object = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (!left) delete this;
        return left;
    }

private:
    std::atomic<ULONG> references_{1};
};

// A font file as DirectWrite has it: its one family, and the face in it to ask for.
struct Font {
    std::uint64_t id{};
    ComPtr<IDWriteFontCollection> collection; // none: the file is not a font DirectWrite reads
    std::wstring family;
    DWRITE_FONT_WEIGHT weight{};
    DWRITE_FONT_STYLE style{};
    DWRITE_FONT_STRETCH stretch{};
    // That face itself, for a glyph's metrics and coverage, and its design metrics (none when
    // DirectWrite could not make it: the family still lays text out).
    ComPtr<IDWriteFontFace> face;
    DWRITE_FONT_METRICS metrics{};
    std::filesystem::file_time_type written{};
    std::uintmax_t size{};
    std::chrono::steady_clock::time_point looked{};
    std::uint64_t used{};
};

// DirectWrite as one user has it, made at the first use and kept: its factory, our loaders, and the
// fonts read so far by their file (which names the plugin, being in its package), read again when the
// file's time or size changes. A file is looked at again at most twice a second, so asking for a font
// is a lookup. One call at a time, under `mutex`, which the caller takes: DirectWrite does not promise
// more for fonts given from memory. Users that must not wait for each other have a library each.
class Library {
public:
    explicit Library(std::size_t most_fonts = 16) : most_fonts_(most_fonts) {}
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    std::mutex mutex;
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteFactory2> factory2; // grey coverage straight from DirectWrite, where there is one: Windows 8.1 on

    bool ready();
    // The font a file holds, or nothing for one DirectWrite does not read. `look`: the file is looked
    // at now, however lately it was.
    Font* font_of(const std::filesystem::path& file, bool look = false);
    // The file is let go, and its bytes with it once DirectWrite has let go of them.
    void forget(const std::filesystem::path& file);
    // What a run of glyphs covers with its pen at (x, y), antialiased in grey and laid on no pixel
    // grid, and in `kind` the texture to ask the analysis for: a byte a pixel where DirectWrite gives
    // grey itself, else ClearType's three, which are averaged. Nothing when it cannot be made.
    ComPtr<IDWriteGlyphRunAnalysis> coverage(const DWRITE_GLYPH_RUN& run, DWRITE_MEASURING_MODE measuring, float x, float y, DWRITE_TEXTURE_TYPE& kind);

private:
    void read(Font& font, const std::filesystem::path& file);
    void drop(std::uint64_t id);

    ComPtr<IDWriteFontFileLoader> loader_;
    ComPtr<IDWriteFontCollectionLoader> collections_;
    std::map<std::wstring, Font> fonts_;
    std::uint64_t next_id_{1}, uses_{};
    std::size_t most_fonts_;
};

// A file's bytes, or nothing for one that is not there, empty, or larger than `most`.
std::string read_file(const std::filesystem::path& file, std::size_t most);
}
