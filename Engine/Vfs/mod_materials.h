#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dingosdk::mods {

// A shader input a mod's own board item declares for plugins (reskate.board.set_parameter): a
// colour or a number its compiled shader reads, whose default lives in that shader and so is in
// no material's live parameters until something sets it. Integers, switches and textures pick how
// an item is drawn and cannot be declared.
struct MaterialParameter {
    std::uint64_t key{};        // the ParamDbKey hash: the GUID's first eight bytes
    std::uint16_t identity{};   // the ParamDbKey identity: the GUID's last two bytes
    bool colour{};              // a ColorRgb, else a Float32
    std::array<float, 3> fallback{}; // the shader's own default (a number in [0])
};
struct MaterialDeclaration {
    std::string mod;
    std::string part;                 // "deck", "grip", "trucks" or "wheels"
    std::vector<std::string> items;   // the item keys whose material these belong to
    std::vector<MaterialParameter> parameters;
};

// Reads a mod's reskate-materials.json:
//   {"schema": 1, "materials": [{"part": "deck", "items": ["<item key>", ...],
//     "parameters": [{"guid": "<32 hex digits>", "type": "colour", "default": [r, g, b]},
//                    {"guid": "...", "type": "number", "default": 0.5}]}]}
// A GUID is the parameter's 16 serialized bytes as hex, as ReSkate Studio shows them. A colour's
// channels are 0 to 1; a number is -100 to 100 (board_material::most_number). Text is non-empty, at
// most 255 bytes and free of control characters. At most 32 materials, 64 items and 32 parameters
// each. Throws std::exception on a document that is not this shape (the whole file is skipped:
// half a declaration could hand a shader a value it does not expect).
std::vector<MaterialDeclaration> parse_material_declarations(const std::string& mod, const std::string& json);

// The ParamDbKey hash and identity of a GUID given as 32 hex digits; false if it is not one.
bool material_parameter_key(const std::string& guid, std::uint64_t& key, std::uint16_t& identity);

} // namespace dingosdk::mods
