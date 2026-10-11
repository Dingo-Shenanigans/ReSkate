#pragma once

#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::launcher_gui {

// DepotDownloader prints its Steam sign-in QR code as rows of two-character
// modules ("â–ˆâ–ˆ" dark, "  " light) and its progress as " 12.34% path".
class DepotOutput {
public:
    std::optional<std::vector<std::string>> qr_update;
    std::optional<float> percent;
    std::string file;
    std::string message;

    void feed(std::string_view line) {
        qr_update.reset();
        percent.reset();
        if (line.find("sign in with this QR code") != std::string_view::npos) {
            collecting_ = true;
            rows_.clear();
            return;
        }
        if (line.find("QR code has changed") != std::string_view::npos) return;
        if (collecting_) {
            std::string modules;
            if (qr_row(line, modules)) {
                if (modules.find('#') != std::string::npos) rows_.push_back(std::move(modules));
                else if (rows_.size() >= 21) finish(); // the smallest QR code is 21 x 21
                return;
            }
            if (!rows_.empty()) finish();
            collecting_ = false;
        }
        const auto trimmed = trim(line);
        if (trimmed.empty()) return;
        if (showing_qr_) { showing_qr_ = false; qr_update = std::vector<std::string>{}; }
        const auto sign = trimmed.find('%');
        if (sign != std::string_view::npos && sign <= 6 && sign + 1 < trimmed.size() && trimmed[sign + 1] == ' ') {
            try {
                std::size_t consumed{};
                const float value = std::stof(std::string(trimmed.substr(0, sign)), &consumed);
                if (consumed == sign && std::isfinite(value) && value >= 0.0f && value <= 100.0f) {
                    percent = value;
                    file = filename(trimmed.substr(sign + 2));
                    return;
                }
            } catch (...) {}
        }
        message = std::string(trimmed.substr(0, 160));
    }

private:
    bool collecting_{};

    static std::string filename(std::string_view path) {
        const auto slash = path.find_last_of("\\/");
        return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
    }
    bool showing_qr_{};
    std::vector<std::string> rows_;

    static std::string_view trim(std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        return value;
    }
    // Without a console DepotDownloader writes in the ANSI code page, where the
    // full block becomes '?'; a console gives UTF-8 or OEM 0xDB instead.
    static bool qr_row(std::string_view line, std::string& modules) {
        std::string cells;
        for (std::size_t index = 0; index < line.size();) {
            if (line[index] == ' ') { cells.push_back(' '); ++index; }
            else if (line.substr(index, 3) == "\xE2\x96\x88") { cells.push_back('#'); index += 3; }
            else if (line[index] == '?' || line[index] == '\xDB') { cells.push_back('#'); ++index; }
            else return false;
        }
        if (cells.empty()) return true; // a blank line is part of the quiet zone
        for (std::size_t index = 0; index < cells.size(); index += 2) modules.push_back(cells[index]);
        return true;
    }
    void finish() {
        qr_update = rows_;
        showing_qr_ = true;
        collecting_ = false;
        rows_.clear();
    }
};

} // namespace dingosdk::launcher_gui
