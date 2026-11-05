#pragma once
#include "synchros_wire.h"
#include "Engine/Vfs/synchros_closure.h"
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The wearer's side of an exchange (synchros_wire.h): what it answers a viewer's Want with, and
// the files it then sends. One Outfit per viewer per session.
//
// A viewer can only fetch files of items it was offered in this exchange: an id it was not told
// of is refused, so a code holder cannot pull arbitrary files out of the wearer's mods.
namespace dingosdk::multiplayer::synchros {

// What the wearer's mods hold, through the cutter (Engine/Vfs/synchros_closure.h). An interface
// so the exchange can be tested without a game.
class Closet {
public:
    virtual ~Closet() = default;
    [[nodiscard]] virtual std::optional<dingosdk::synchros::Cut> cut(std::string_view item) = 0;
    [[nodiscard]] virtual std::vector<std::byte> read(const dingosdk::synchros::Piece& piece) = 0;
};

// The cutter itself.
class IndexCloset final : public Closet {
public:
    explicit IndexCloset(dingosdk::synchros::Index& index) : index_(index) {}
    std::optional<dingosdk::synchros::Cut> cut(std::string_view item) override { return index_.cut(item); }
    std::vector<std::byte> read(const dingosdk::synchros::Piece& piece) override { return index_.read(piece); }

private:
    dingosdk::synchros::Index& index_;
};

class Outfit final {
public:
    // `worn`: the asset keys the wearer has on now (its outfit packet). `game_item`: whether a key
    // is one of the game's own items, which every viewer has.
    Outfit(Closet& closet, std::set<std::string> worn, std::function<bool(std::string_view)> game_item);

    // The answer to a Want, as one or more Contents messages each under Steam's limit.
    [[nodiscard]] std::vector<Contents> answer(const Want& want);

    // A Fetch: queues the files asked for. False when it names a file this exchange never offered
    // (the exchange ends).
    [[nodiscard]] bool fetch(const Fetch& fetch);
    // The next piece to send, or nothing when the queue is empty. Pieces of one file go in order,
    // each max_piece long but the last.
    [[nodiscard]] std::optional<Piece> next();
    [[nodiscard]] bool sending() const noexcept { return !queue_.empty(); }

    // When what the wearer has on changes: items no longer worn are not offered any more.
    void wearing(std::set<std::string> worn) { worn_ = std::move(worn); }

private:
    struct Offered {
        dingosdk::synchros::Piece piece;
        std::uint32_t size{};
    };
    [[nodiscard]] ItemContents answer_one(const std::string& item);

    Closet& closet_;
    std::set<std::string> worn_;
    std::function<bool(std::string_view)> game_item_;
    std::map<ContentId, Offered> offered_;
    std::deque<ContentId> queue_;
    std::vector<std::uint8_t> current_;   // the file being sent
    std::uint32_t at_{};
    std::uint64_t sent_{};                // bytes sent in this exchange, against max_outfit
};

// A received file's description, from a cutter piece and its bytes as stored.
[[nodiscard]] File describe_piece(const dingosdk::synchros::Piece& piece, std::span<const std::byte> stored);

} // namespace dingosdk::multiplayer::synchros
