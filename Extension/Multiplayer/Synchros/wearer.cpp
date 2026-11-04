#include "wearer.h"
#include "received.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dingosdk::multiplayer::synchros {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

std::string leaf(const std::string& name) { return name.substr(name.rfind('/') + 1); }

std::span<const std::uint8_t> as_bytes(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

FileKind kind_of(dingosdk::synchros::Piece::Kind kind) {
    switch (kind) {
    case dingosdk::synchros::Piece::ebx: return FileKind::ebx;
    case dingosdk::synchros::Piece::resource: return FileKind::resource;
    case dingosdk::synchros::Piece::chunk: return FileKind::chunk;
    case dingosdk::synchros::Piece::toc_chunk: return FileKind::toc_chunk;
    }
    return FileKind::ebx;
}

// A Contents message's size, without writing it out each time an item is added.
std::size_t encoded_size(const Contents& contents) {
    try {
        return encode(Message{contents}).size();
    } catch (const std::length_error&) {
        return max_message + 1;
    }
}

} // namespace

File describe_piece(const dingosdk::synchros::Piece& piece, std::span<const std::byte> stored) {
    File f;
    f.id = content_id(as_bytes(stored));
    f.size = static_cast<std::uint32_t>(stored.size());
    f.kind = kind_of(piece.kind);
    f.name = piece.name;
    f.bundle = piece.bundle;
    f.superbundle = piece.superbundle;
    f.original_size = piece.asset.originalSize;
    f.resource_type = piece.asset.resourceType;
    f.resource_id = piece.asset.resourceId;
    f.resource_meta.resize(piece.asset.resourceMeta.size());
    if (!f.resource_meta.empty()) std::memcpy(f.resource_meta.data(), piece.asset.resourceMeta.data(), f.resource_meta.size());
    std::memcpy(f.chunk_guid.data(), piece.asset.guid.bytes.data(), f.chunk_guid.size());
    f.logical_offset = piece.asset.logicalOffset;
    f.logical_size = piece.asset.logicalSize;
    std::memcpy(f.sha1.data(), piece.asset.sha1.bytes.data(), f.sha1.size());
    if (piece.kind == dingosdk::synchros::Piece::chunk && piece.chunk_record.size() <= max_chunk_record) {
        f.chunk_record.resize(piece.chunk_record.size());
        if (!f.chunk_record.empty()) std::memcpy(f.chunk_record.data(), piece.chunk_record.data(), f.chunk_record.size());
    }
    return f;
}

Outfit::Outfit(Closet& closet, std::set<std::string> worn, std::function<bool(std::string_view)> game_item)
    : closet_(closet), game_item_(std::move(game_item)) {
    for (const auto& key : worn) worn_.insert(lower(key));
}

ItemContents Outfit::answer_one(const std::string& item) {
    ItemContents answer;
    answer.item = item;
    // The outfit packet names items by key; a viewer may ask by key or by asset name.
    const auto key = lower(item);
    if (!worn_.contains(key) && !worn_.contains(leaf(key))) {
        answer.withheld = Withheld::not_worn;
        return answer;
    }
    if (game_item_ && game_item_(leaf(key))) {
        answer.withheld = Withheld::game_item;
        return answer;
    }
    std::optional<dingosdk::synchros::Cut> cut;
    try {
        cut = closet_.cut(item);
    } catch (const std::exception&) {
        cut.reset();
    }
    if (!cut || cut->pieces.size() > max_files_per_item) {
        answer.withheld = cut ? Withheld::too_large : Withheld::unreadable;
        return answer;
    }
    std::uint64_t total{};
    std::vector<std::pair<File, dingosdk::synchros::Piece>> files;
    for (const auto& piece : cut->pieces) {
        std::vector<std::byte> stored;
        try {
            stored = closet_.read(piece);
        } catch (const std::exception&) {
            answer.withheld = Withheld::unreadable;
            return answer;
        }
        if (stored.empty() || stored.size() > max_file) {
            answer.withheld = stored.empty() ? Withheld::unreadable : Withheld::too_large;
            return answer;
        }
        total += stored.size();
        if (total > max_outfit) {
            answer.withheld = Withheld::too_large;
            return answer;
        }
        files.emplace_back(describe_piece(piece, stored), piece);
    }
    for (auto& [file, piece] : files) {
        offered_.insert_or_assign(file.id, Offered{piece, file.size});
        answer.files.push_back(std::move(file));
    }
    for (const auto& row : cut->rows) answer.rows.push_back({row.table, row.path, row.bundle, row.table_bundle, row.table_superbundle});
    return answer;
}

std::vector<Contents> Outfit::answer(const Want& want) {
    std::vector<Contents> messages;
    Contents current;
    std::uint64_t outfit{};
    for (const auto& item : want.items) {
        auto one = answer_one(item);
        for (const auto& f : one.files) outfit += f.size;
        // Every item of one request together stays within what a viewer accepts.
        if (outfit > max_outfit && one.withheld == Withheld::none) {
            for (const auto& f : one.files) outfit -= f.size;
            one = ItemContents{item, Withheld::too_large, {}, {}};
        }
        current.items.push_back(std::move(one));
        if (encoded_size(current) <= max_message) continue;
        auto last = std::move(current.items.back());
        current.items.pop_back();
        if (!current.items.empty()) messages.push_back(std::move(current));
        current = Contents{{std::move(last)}};
        if (encoded_size(current) > max_message) {
            // One item's listing alone is over Steam's limit: it cannot be described, so it is not sent.
            for (const auto& f : current.items.front().files) outfit -= f.size;
            current.items.front() = ItemContents{item, Withheld::too_large, {}, {}};
        }
    }
    if (!current.items.empty()) messages.push_back(std::move(current));
    return messages;
}

bool Outfit::fetch(const Fetch& fetch) {
    for (const auto& id : fetch.files)
        if (!offered_.contains(id)) return false;
    for (const auto& id : fetch.files)
        if (std::find(queue_.begin(), queue_.end(), id) == queue_.end()) queue_.push_back(id);
    return true;
}

std::optional<Piece> Outfit::next() {
    while (!queue_.empty()) {
        const auto id = queue_.front();
        const auto& offered = offered_.at(id);
        if (current_.empty()) {
            if (sent_ + offered.size > max_outfit) {
                queue_.clear();  // a viewer asking for more than one outfit's worth gets no more
                return std::nullopt;
            }
            std::vector<std::byte> stored;
            try {
                stored = closet_.read(offered.piece);
            } catch (const std::exception&) {
                stored.clear();
            }
            const auto bytes = as_bytes(stored);
            // The mod changed under us (updated, removed): its file is not what was offered.
            if (bytes.size() != offered.size || content_id(bytes) != id) {
                queue_.pop_front();
                continue;
            }
            current_.assign(bytes.begin(), bytes.end());
            at_ = 0;
        }
        Piece piece;
        piece.id = id;
        piece.offset = at_;
        piece.total = static_cast<std::uint32_t>(current_.size());
        const auto size = std::min<std::size_t>(max_piece, current_.size() - at_);
        piece.bytes.assign(current_.begin() + static_cast<std::ptrdiff_t>(at_), current_.begin() + static_cast<std::ptrdiff_t>(at_ + size));
        at_ += static_cast<std::uint32_t>(size);
        sent_ += size;
        if (at_ >= current_.size()) {
            current_.clear();
            queue_.pop_front();
        }
        return piece;
    }
    return std::nullopt;
}

} // namespace dingosdk::multiplayer::synchros
