#include "Extension/GraphicsCreator/graphic_document.h"
#include "Extension/GraphicsCreator/graphic_assets.h"
#include "Extension/GraphicsCreator/graphic_raster.h"
#include "Extension/GraphicsCreator/native_export.h"
#include "Extension/GraphicsCreator/graphic_store.h"
#include "Engine/Core/Json/json.h"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> void rejects(F&& fn) {
    bool rejected{};
    try { fn(); } catch (...) { rejected = true; }
    check(rejected, "Invalid graphic document was accepted");
}
} // namespace

int main(int argc, char** argv) {
    try {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        using namespace dingosdk;
        using namespace dingosdk::graphics_creator;
        GraphicDocument document{current_schema_version, "night-lines", "Night Lines", {1024, 1024}, {
            {"background", "Background", true, 1, {512, 512, 0, 1, 1},
             {ShapeKind::rectangle, 1024, 1024, {14, 18, 24, 255}}},
            {"sun", "Sun", true, .8f, {640, 310, 12, 1.25f, 1.25f},
             {ShapeKind::ellipse, 180, 180, {246, 198, 36, 255}}}}};

        const auto encoded = encode(document);
        check(encoded == encode(document), "Graphic serialization is not deterministic");
        check(decode(encoded) == document, "Graphic document round trip differs");
        check(Json::parse(encoded).at("layers").size() == 2, "Graphic JSON lost layers");
        check(valid_identifier("deck-logo_2") && !valid_identifier("deck logo"), "Identifier validation differs");
        check(valid_display_name("Deck Logo") && !valid_display_name(" Deck Logo"), "Display-name validation differs");
        check(valid_asset_name("asset-0123456789abcdef.png") && !valid_asset_name("../image.png"),
              "Asset-name validation differs");

        const auto legacy = decode(R"({
            "schema_version":1,"id":"legacy","name":"Legacy","canvas":{"width":512,"height":512},
            "layers":[{"id":"shape","name":"Shape","visible":true,"opacity":1,
            "transform":{"x":256,"y":256,"rotation_degrees":0,"scale_x":1,"scale_y":1},
            "shape":{"kind":"rectangle","width":100,"height":100,"fill_rgba":[255,255,255,255]}}]})");
        check(legacy.schema_version == current_schema_version && legacy.target == DesignTarget::sticker,
              "Version 1 graphic did not migrate to the sticker target");

        auto bad = document;
        bad.schema_version = 99;
        rejects([&] { encode(bad); });
        bad = document;
        bad.layers[1].id = bad.layers[0].id;
        rejects([&] { encode(bad); });
        bad = document;
        bad.layers[0].opacity = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { encode(bad); });
        bad = document;
        bad.layers[0].transform.scale_x = 0;
        rejects([&] { encode(bad); });
        rejects([&] { decode("{\"schema_version\":99}"); });
        rejects([&] { decode(std::string(1024 * 1024 + 1, ' ')); });

        for (const auto name : {"", "../graphic", "A/B", "graphic.json", "CON", "nul", "LPT2", " trailing ", "x:y"})
            check(!valid_file_name(name), "Unsafe graphic filename accepted");
        check(valid_file_name("Night Lines - v2"), "Normal graphic filename rejected");

        const auto directory = std::filesystem::absolute(argc > 1 ? argv[1] : "build/graphics-creator-fixtures") /
                               std::to_string(GetCurrentProcessId());
        std::filesystem::remove_all(directory);
        save(directory, "Night Lines", document);
        auto updated = document;
        updated.name = "Night Lines Revised";
        updated.layers[1].opacity = .5f;
        save(directory, "Night Lines", updated);
        const auto loaded = load(directory, "Night Lines");
        check(!loaded.recovered_from_backup && loaded.document == updated, "Current graphic save was not loaded");
        {
            std::ofstream corrupt(directory / "Night Lines.graphic.json", std::ios::binary | std::ios::trunc);
            corrupt << "not json";
        }
        const auto recovered = load(directory, "Night Lines");
        check(recovered.recovered_from_backup && recovered.document == document,
              "Corrupt graphic did not recover the previous valid save");
        bad = document;
        bad.layers[0].shape.width = 0;
        rejects([&] { save(directory, "Night Lines", bad); });
        check(load(directory, "Night Lines").recovered_from_backup, "Rejected save changed the stored document");
        const auto names = list(directory);
        check(std::find(names.begin(), names.end(), "Night Lines") != names.end(), "Saved graphic missing from browser");
        GraphicDocument flat{current_schema_version, "flat", "Flat", {64, 64}, {
            {"red", "Red", true, 1, {32, 32, 0, 1, 1},
             {ShapeKind::rectangle, 64, 64, {255, 0, 0, 255}}}}};
        const auto rendered = rasterize(flat, directory / "Assets", 64);
        check(rendered.width == 64 && rendered.height == 64 && rendered.rgba[0] == 255 && rendered.rgba[3] == 255,
              "A basic graphic did not rasterize at full opacity");
        export_png(directory / "Exports" / "flat.png", rendered);
        const auto exported = load_image(directory / "Exports", "flat.png");
        check(exported.width == 64 && exported.height == 64 && exported.rgba == rendered.rgba,
              "The exported target PNG did not round trip");
        const auto staged = stage_native_export(flat, directory / "Exports" / "flat.png");
        check(std::filesystem::is_regular_file(staged.manifest), "Native export manifest was not written");
        {
            std::ifstream native_manifest_input(staged.manifest, std::ios::binary);
            const auto native_manifest = Json::parse(std::string(std::istreambuf_iterator<char>(native_manifest_input),
                                                                 std::istreambuf_iterator<char>()));
            check(native_manifest.at("schema").string() == "ReSkate.Create.NativeExport.v1" &&
                  native_manifest.at("state").string() == "png-only",
                  "Unsupported-target native export manifest differs");
        }
        if (GetEnvironmentVariableW(L"RESKATE_NATIVE_EXPORT_INTEGRATION", nullptr, 0)) {
            flat.target = DesignTarget::board;
            const auto native = stage_native_export(flat, directory / "Exports" / "flat.png");
            check(native.attempted && native.validated && std::filesystem::is_regular_file(native.encoded_preview),
                  "Frostbite board texture integration validation failed");
            std::ifstream integration_manifest_input(native.manifest, std::ios::binary);
            const auto integration_manifest = Json::parse(std::string(
                std::istreambuf_iterator<char>(integration_manifest_input), std::istreambuf_iterator<char>()));
            check(integration_manifest.at("state").string() == "frostbite-texture-validated" &&
                  integration_manifest.at("packaging_state").string() == "studio-authoring-required",
                  "Frostbite board validation manifest differs");
        }
        bad = document;
        bad.layers[0].shape.kind = ShapeKind::image;
        bad.layers[0].shape.source = "../outside.png";
        rejects([&] { encode(bad); });
        rejects([&] { import_image(directory / "Assets", directory / "missing.png"); });
        if (argc > 2) {
            const auto imported = import_image(directory / "Assets", argv[2]);
            check(valid_asset_name(imported.asset_name) && imported.width && imported.height,
                  "Valid PNG was not imported");
            const auto preview = load_preview(directory / "Assets", imported.asset_name, 24);
            check(preview.width && preview.height && preview.width <= 24 && preview.height <= 24 &&
                  preview.rgba.size() == preview.width * preview.height * 4,
                  "Imported PNG preview was not bounded or decoded");
            check(import_image(directory / "Assets", argv[2]).asset_name == imported.asset_name,
                  "Importing the same image did not reuse its content-addressed asset");
            GraphicDocument image_document{current_schema_version, "image", "Image", {128, 128}, {
                {"image", "Image", true, 1, {64, 64, 0, 1, 1},
                 {ShapeKind::image, 128, 128, {255, 255, 255, 255}, imported.asset_name}}}};
            const auto image_render = rasterize(image_document, directory / "Assets", 128);
            check(image_render.width == 128 && image_render.height == 128 && image_render.rgba.size() == 128 * 128 * 4,
                  "An imported image did not rasterize into the target canvas");
        }
        std::filesystem::remove_all(directory);

        std::cout << "ReSkate.Create document, validation, atomic save and recovery tests passed.\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
