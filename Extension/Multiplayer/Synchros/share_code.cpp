#include "share_code.h"
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif
#include <stdexcept>
#include <vector>

namespace dingosdk::multiplayer::synchros {
namespace {
constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford base32

int symbol_value(char c) noexcept {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const auto at = alphabet.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

// The check character: each of the first eleven times its place (1 to 11), summed modulo 31. 31 is
// prime and no weight is a multiple of it, so one changed character, or two swapped ones, always
// changes the sum, except a 0 written for a Z or the other way round (they differ by 31).
char check_symbol(std::string_view eleven) noexcept {
    unsigned sum = 0;
    for (std::size_t i = 0; i < eleven.size(); ++i) sum += static_cast<unsigned>(symbol_value(eleven[i])) * static_cast<unsigned>(i + 1);
    return alphabet[sum % 31];
}

void random_bytes(std::uint8_t* out, std::size_t size) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("No random numbers for a share code.");
#else
    if (RAND_bytes(out, static_cast<int>(size)) != 1) throw std::runtime_error("No random numbers for a share code.");
#endif
}

#ifdef _WIN32
void check(NTSTATUS result) {
    if (result < 0) throw std::runtime_error("Share code cryptography unavailable.");
}
std::array<std::uint8_t, 32> digest(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data, std::size_t size) {
    BCRYPT_ALG_HANDLE algorithm{};
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0));
    BCRYPT_HASH_HANDLE hash{};
    std::array<std::uint8_t, 32> out{};
    const auto result = BCryptCreateHash(algorithm, &hash, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(key_size), 0);
    if (result >= 0) {
        BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0);
        BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    check(result);
    return out;
}
#else
std::array<std::uint8_t, 32> digest(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data, std::size_t size) {
    std::array<std::uint8_t, 32> out{};
    if (key) {
        unsigned length = 0;
        if (!HMAC(EVP_sha256(), key, static_cast<int>(key_size), data, size, out.data(), &length) || length != out.size())
            throw std::runtime_error("Share code cryptography unavailable.");
    } else {
        SHA256(data, size, out.data());
    }
    return out;
}
#endif

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
} // namespace

std::string make_code(CodeKind kind) {
    std::array<std::uint8_t, 11> random{};
    random_bytes(random.data(), random.size());
    std::string symbols;
    // Each byte's low five bits: 256 is a multiple of 32, so every symbol is as likely.
    for (const auto byte : random) symbols += alphabet[byte & 31];
    symbols += check_symbol(symbols);
    return show_code(symbols, kind);
}

std::optional<CodeKind> typed_kind(std::string_view typed) noexcept {
    std::size_t at = 0;
    while (at < typed.size() && (typed[at] == ' ' || typed[at] == '\t')) ++at;
    if (typed.size() - at < 3) return std::nullopt;
    auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
    const std::string head{up(typed[at]), up(typed[at + 1]), up(typed[at + 2])};
    if (head == personal_prefix) return CodeKind::personal;
    if (head == group_prefix) return CodeKind::group;
    return std::nullopt;
}

std::optional<std::string> read_code(std::string_view typed, CodeKind kind) {
    // The prefix, if typed, has to be this kind's.
    if (const auto said = typed_kind(typed)) {
        if (*said != kind) return std::nullopt;
        typed.remove_prefix(typed.find_first_not_of(" \t") + 3);
    }
    std::string symbols;
    for (const char c : typed) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        const int value = symbol_value(c);
        if (value < 0) return std::nullopt;
        symbols += alphabet[static_cast<std::size_t>(value)];
    }
    if (symbols.size() != code_symbols) return std::nullopt;
    if (check_symbol(std::string_view(symbols).substr(0, code_symbols - 1)) != symbols.back()) return std::nullopt;
    return symbols;
}

std::string show_code(std::string_view symbols, CodeKind kind) {
    std::string out(prefix_of(kind));
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i % 4 == 0) out += '-';
        out += symbols[i];
    }
    return out;
}

CodeKey code_key(std::string_view symbols, CodeKind kind) {
    std::vector<std::uint8_t> data{'S', 'k', 'a', 't', 'e', 'S', 'y', 'n', 'c', 'h', 'r', 'o', 's'};
    const std::string_view label = kind == CodeKind::group ? "Group" : "Code";
    data.insert(data.end(), label.begin(), label.end());
    data.push_back(0);
    data.insert(data.end(), symbols.begin(), symbols.end());
    return digest(nullptr, 0, data.data(), data.size());
}

Proof code_proof(const CodeKey& key, std::uint64_t session, std::uint64_t wearer, std::uint64_t viewer) {
    std::vector<std::uint8_t> data{'S', 'k', 'a', 't', 'e', 'S', 'y', 'n', 'c', 'h', 'r', 'o', 's', 'P', 'r', 'o', 'o', 'f'};
    put_u64(data, session);
    put_u64(data, wearer);
    put_u64(data, viewer);
    return digest(key.data(), key.size(), data.data(), data.size());
}

bool proof_matches(const CodeKey& key, const Proof& proof, std::uint64_t session, std::uint64_t wearer,
                   std::uint64_t viewer) noexcept {
    try {
        const auto expected = code_proof(key, session, wearer, viewer);
        std::uint8_t difference = 0;
        for (std::size_t i = 0; i < expected.size(); ++i) difference |= static_cast<std::uint8_t>(expected[i] ^ proof[i]);
        return difference == 0;
    } catch (...) {
        return false;
    }
}
} // namespace dingosdk::multiplayer::synchros
