#include "native_db.h"

#include <stdexcept>
#include <utility>

namespace dingosdk::native_db {
namespace {

class Reader {
public:
    Reader(std::span<const unsigned char> input, std::string_view context, Options options)
        : input_(input), context_(context), options_(options) {}

    Node read(unsigned depth = 0) {
        if (depth > 32 || ++nodes_ > options_.max_entries) fail("DB nesting/entry limit exceeded");
        const auto tag = take(1)[0];
        Node node;
        node.type = tag & 31;
        if (!node.type) return node;
        node.named = (tag & 128) == 0;
        if (node.named) {
            while (true) {
                const auto ch = take(1)[0];
                if (!ch) break;
                if (node.name.size() >= 1024) fail("DB field name too long");
                node.name.push_back(static_cast<char>(ch));
            }
        }
        if (node.type == 1 || node.type == 2) {
            const auto size = variable();
            if (size > input_.size() - position_) fail("Truncated DB container");
            const auto end = position_ + size;
            while (position_ < end) {
                auto child = read(depth + 1);
                if (!child.type) { node.terminated = true; break; }
                if (options_.unique_fields && node.type == 2 && child.named &&
                    node.field(child.name)) fail("Duplicate DB field");
                node.children.push_back(std::move(child));
            }
            if (position_ != end) fail("DB container length mismatch");
        } else if (node.type == 7 || node.type == 19) {
            node.bytes = take(variable());
            if (node.type == 7) {
                if (node.bytes.size() > 65536) fail("DB string too long");
                node.text.assign(reinterpret_cast<const char*>(node.bytes.data()), node.bytes.size());
                if (!node.text.empty() && node.text.back() == '\0') node.text.pop_back();
            }
        } else {
            std::size_t size{};
            switch (node.type) {
            case 6: size = 1; break;
            case 8: case 11: size = 4; break;
            case 9: case 12: size = 8; break;
            case 15: size = 16; break;
            case 16: size = 20; break;
            default: fail("Unsupported DB value type");
            }
            node.bytes = take(size);
            node.boolean = node.bytes[0] != 0;
        }
        return node;
    }

    std::size_t position() const { return position_; }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error(std::string(context_) + ": " + std::string(message));
    }
    std::span<const unsigned char> take(std::size_t size) {
        if (size > input_.size() - position_) fail("Truncated DB value");
        const auto value = input_.subspan(position_, size);
        position_ += size;
        return value;
    }
    std::size_t variable() {
        std::size_t value{};
        for (unsigned shift = 0; shift < 63; shift += 7) {
            const auto byte = take(1)[0];
            value |= static_cast<std::size_t>(byte & 127) << shift;
            if (!(byte & 128)) return value;
        }
        fail("Invalid DB variable length");
    }
    std::span<const unsigned char> input_;
    std::string_view context_;
    Options options_;
    std::size_t position_{}, nodes_{};
};

void write_variable(std::vector<unsigned char>& out, std::size_t value) {
    while (true) {
        auto byte = static_cast<unsigned char>(value & 127);
        value >>= 7;
        if (!value) { out.push_back(byte); return; }
        out.push_back(static_cast<unsigned char>(byte | 128));
    }
}

void emit(const Node& node, std::vector<unsigned char>& out) {
    if (!node.type) { out.push_back(0); return; }
    out.push_back(static_cast<unsigned char>(node.type | (node.named ? 0u : 128u)));
    if (node.named) {
        out.insert(out.end(), node.name.begin(), node.name.end());
        out.push_back(0);
    }
    if (node.type == 1 || node.type == 2) {
        std::vector<unsigned char> body;
        for (const auto& child : node.children) emit(child, body);
        if (node.terminated) body.push_back(0);
        write_variable(out, body.size());
        out.insert(out.end(), body.begin(), body.end());
        return;
    }
    const auto payload = node.payload();
    if (node.type == 7 || node.type == 19) write_variable(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

const Node* Node::field(std::string_view key) const {
    for (const auto& child : children) if (child.named && child.name == key) return &child;
    return nullptr;
}
Node* Node::field(std::string_view key) {
    return const_cast<Node*>(std::as_const(*this).field(key));
}

Node read(std::span<const unsigned char> input, std::string_view context, std::size_t* consumed,
          Options options) {
    Reader reader(input, context, options);
    auto root = reader.read();
    if (consumed) *consumed = reader.position();
    return root;
}

std::vector<unsigned char> write(const Node& root) {
    std::vector<unsigned char> out;
    emit(root, out);
    return out;
}

Node make_named_record(std::string_view field, std::string_view value) {
    Node text;
    text.type = 7;
    text.named = true;
    text.name = field;
    text.text = value;
    text.owned.assign(value.begin(), value.end());
    text.owned.push_back(0);

    Node record;
    record.type = 2;
    record.terminated = true;
    record.children.push_back(std::move(text));
    return record;
}

} // namespace dingosdk::native_db
