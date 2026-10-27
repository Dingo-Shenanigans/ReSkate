// Checks how a mod's reskate-materials.json is read: a GUID's ParamDbKey hash and identity, what a
// declaration may hold (colours and numbers in bounds, never integers, switches or textures), the
// limits, and that a document of the wrong shape is refused whole.
#include "Engine/Vfs/mod_materials.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

using dingosdk::mods::parse_material_declarations;

bool refused(const std::string& json) {
    try { parse_material_declarations("mod", json); } catch (const std::exception&) { return true; }
    return false;
}
std::string with_parameter(const std::string& parameter, const std::string& part = "deck") {
    return R"({"schema":1,"materials":[{"part":")" + part + R"(","items":["Example_Deck"],"parameters":[)" + parameter + "]}]}";
}
constexpr const char* guid = "1af346775c032d5b59ff5e88010069b6";
}

int main() try {
    // The eight inputs of the Example declared-input deck: their GUIDs and the hash and identity the game
    // keys them by (parameter-contract.json).
    struct Known { const char* guid; std::uint64_t key; std::uint16_t identity; };
    const Known known[] = {
        {"1af346775c032d5b59ff5e88010069b6", 6569911127076434714ULL, 46697},
        {"d317d1d20f5c745159ff5e880100dcb0", 5869417437401782227ULL, 45276},
        {"ed58e12fb758105c59ff5e8801005aea", 6633899794922297581ULL, 59994},
        {"9f361c20ac084e5a59ff5e8801006ffe", 6507148046963062431ULL, 65135},
        {"c7c08365cf15fc5759ff5e8801006651", 6339966355936362695ULL, 20838},
        {"83f212bddd522f5a59ff5e880100e557", 6498503899632300675ULL, 22501},
        {"97901b52e1a1fd5559ff5e88010012b9", 6196286651448922263ULL, 47378},
        {"126f57aa15de0b5859ff5e8801003944", 6344408684715011858ULL, 17465},
    };
    for (const auto& k : known) {
        std::uint64_t key{};
        std::uint16_t identity{};
        check(dingosdk::mods::material_parameter_key(k.guid, key, identity) && key == k.key && identity == k.identity,
              std::string("GUID ") + k.guid + " keys as the game keys it");
    }
    std::uint64_t key{};
    std::uint16_t identity{};
    check(!dingosdk::mods::material_parameter_key("1af34677", key, identity), "A GUID is 32 hex digits");
    check(!dingosdk::mods::material_parameter_key("zzf346775c032d5b59ff5e88010069b6", key, identity), "of hex");

    {
        const auto read = parse_material_declarations("Example", R"({
            "schema": 1,
            "materials": [{
                "part": "deck",
                "items": ["Example_Deck", "Example_Deck_Wide"],
                "parameters": [
                    {"guid": "1af346775c032d5b59ff5e88010069b6", "type": "colour", "default": [0.5, 0.5, 0.5]},
                    {"guid": "126f57aa15de0b5859ff5e8801003944", "type": "color", "default": [0, 0, 0]},
                    {"guid": "d317d1d20f5c74512d5cb62b0100dcb0", "type": "number", "default": -2.5}
                ]
            }]
        })");
        check(read.size() == 1 && read[0].mod == "Example" && read[0].part == "deck" && read[0].items.size() == 2, "A material is read");
        const auto& p = read[0].parameters;
        check(p.size() == 3 && p[0].colour && p[0].key == 6569911127076434714ULL && p[0].identity == 46697 &&
                  p[0].fallback == std::array<float, 3>{.5f, .5f, .5f},
              "with its colours keyed from their GUIDs and their defaults");
        check(p[1].colour, "either spelling of colour");
        check(!p[2].colour && p[2].fallback[0] == -2.5f, "and a number");
    }

    {
        std::string collection = R"({"schema":1,"materials":[)";
        for (int deck = 0; deck < 14; ++deck) {
            if (deck) collection += ',';
            collection += R"({"part":"deck","items":["Own_Test_)" + std::to_string(deck) + R"("],"parameters":[)";
            for (int channel = 0; channel < 8; ++channel) {
                if (channel) collection += ',';
                collection += R"({"guid":")" + std::string(known[channel].guid) +
                    R"(","type":"colour","default":[0,0,)" + std::to_string(deck / 16.0) + "]}";
            }
            collection += "]}";
        }
        const auto declarations = parse_material_declarations("Collection", collection + "]}");
        check(declarations.size() == 14, "A fourteen-deck collection is accepted as a whole");
        for (const auto& material : declarations)
            check(material.items.size() == 1 && material.parameters.size() == 8,
                  "Each deck retains its eight defaults and item scope");
        check(declarations.front().parameters.back().fallback != declarations.back().parameters.back().fallback,
              "Different deck defaults survive parsing");
    }

    // Only what a plugin may set, in bounds.
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"integer","default":13})")),
          "An integer picks how the item is drawn: not declarable");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"switch","default":true})")), "nor a switch");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"texture","default":0})")), "nor a texture");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"colour","default":[1.5,0,0]})")), "A colour is 0 to 1");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"colour","default":[1,0]})")), "with three channels");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"number","default":1000})")), "A number has bounds");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"number","default":"1"})")), "and is a number");
    check(refused(with_parameter(R"({"guid":"1af34677","type":"colour","default":[0,0,0]})")), "A GUID is 32 hex digits");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"colour","default":[0,0,0]},{"guid":")" +
                                 std::string(guid) + R"(","type":"colour","default":[1,1,1]})")),
          "A parameter is declared once");
    check(refused(with_parameter(R"({"guid":")" + std::string(guid) + R"(","type":"colour","default":[0,0,0]})", "skater")),
          "A part is one of the board's");

    // The wrong shape is refused whole.
    check(refused(R"({"schema":1.5,"materials":[]})"),"Fractional schema rejected");
    check(refused(with_parameter(R"({"guid":"1af346775c032d5b2d5cb62b010069b6","type":"colour","default":[0,0,0]})")),"GUID type must match declared colour");
    check(refused(with_parameter(R"({"guid":"1af346775c032d5b59ff5e88020069b6","type":"colour","default":[0,0,0]})")),"GUID count must be one");
    check(refused("{}"), "A document without a schema");
    check(refused(R"({"schema":2,"materials":[]})"), "or of another schema");
    check(refused(R"({"schema":1,"materials":[{"part":"deck","items":[],"parameters":[]}]})"), "A material with no items");
    check(refused(R"({"schema":1,"materials":[{"part":"deck","items":["a"],"parameters":[]}]})"), "or no parameters");
    check(refused(R"({"schema":1,"materials":[{"part":"deck","items":["bad
name"],"parameters":[{"guid":"1af346775c032d5b59ff5e88010069b6","type":"colour","default":[0,0,0]}]}]})"),
          "An item's key is text without control characters");
    {
        std::string many = R"({"schema":1,"materials":[)";
        for (int i = 0; i < 33; ++i) many += std::string(i ? "," : "") + R"({"part":"deck","items":["a"],"parameters":[{"guid":")" + guid + R"(","type":"colour","default":[0,0,0]}]})";
        check(refused(many + "]}"), "At most 32 material groups");
        std::string parameters;
        for (int i = 0; i < 33; ++i) {
            char g[33];
            std::snprintf(g, sizeof(g), "%02x%030x", i + 1, i + 1);
            parameters += std::string(i ? "," : "") + R"({"guid":")" + g + R"(","type":"colour","default":[0,0,0]})";
        }
        check(refused(with_parameter(parameters)), "and 32 parameters to a material");
    }

    if (failures) return 1;
    std::cout << "mod materials tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
