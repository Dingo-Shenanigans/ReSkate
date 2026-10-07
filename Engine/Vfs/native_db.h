#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::native_db {

// Signed header in front of every native DB file (InitFS, layout.toc,
// superbundle TOCs). Studio writes it all zero apart from the magic, which the
// runtime verifier accepts without a signature check.
inline constexpr std::size_t envelope_size = 0x22c;
inline constexpr unsigned char magic[4]{0x00, 0xd1, 0xce, 0x01};

// Bounded reader for the native DB container, including unknown scalar fields.
// Byte arrays remain views while the containing buffer is alive.
struct Node {
    unsigned type{};
    bool named{};
    // Shipped containers close with an explicit zero tag inside their declared
    // length. Recording it keeps read followed by write byte for byte.
    bool terminated{};
    bool boolean{};
    std::string name, text;
    std::span<const unsigned char> bytes;
    // Payload for nodes this process built, which have no source buffer.
    std::vector<unsigned char> owned;
    std::vector<Node> children;

    const Node* field(std::string_view key) const;
    Node* field(std::string_view key);
    std::span<const unsigned char> payload() const {
        return owned.empty() ? bytes : std::span<const unsigned char>(owned);
    }
};

struct Options {
    // InitFS requires one value per key so a duplicate virtual path cannot hide
    // an entry. layout.toc's meta section repeats keys by design, so readers of
    // that file turn the rule off.
    bool unique_fields = true;
    // How many values a file may hold: more than any the game reads whole at
    // startup. A reader of something longer by nature (the chunk list of a
    // level's root bundle) raises it to what its input can hold.
    std::size_t max_entries = 32768;
};

// Throws std::runtime_error prefixed with `context`, for example "InitFS".
// `consumed` reports how much of `input` the tree used; trailing bytes are the
// caller's to reject.
Node read(std::span<const unsigned char> input, std::string_view context,
          std::size_t* consumed = nullptr, Options options = {});

// Re-emits `root`. A tree returned by read() writes back to the same bytes.
std::vector<unsigned char> write(const Node& root);

// The unnamed { "<field>": "<value>" } record shape layout.toc uses for its
// superBundles rows.
Node make_named_record(std::string_view field, std::string_view value);

} // namespace dingosdk::native_db
