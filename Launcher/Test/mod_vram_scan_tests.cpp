#include "Launcher/gui_internal.h"

#include <iostream>
#include <latch>

using namespace dingosdk::launcher_gui::detail;

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (condition) return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    };

    std::latch finish_scan{1}, scan_finished{1};
    ModsPanel panel;
    auto& scan = panel.vram;
    scan.running = true;
    scan.worker = std::thread([&] {
        // Hold the worker in a read until the UI has requested and pumped a refresh.
        finish_scan.wait();
        {
            std::lock_guard lock(scan.mutex);
            scan.done.emplace_back("removed", dingosdk::mods::ModVram{});
        }
        scan.running = false;
        scan_finished.count_down();
    });

    request_vram_scan(scan);
    pump_vram_scan(panel);
    check(scan.stop && scan.refresh_requested && scan.running, "a live scan is cancelled without blocking the UI");

    panel.installing = true;
    request_vram_scan(scan);
    finish_scan.count_down();
    scan_finished.wait();
    pump_vram_scan(panel);
    check(scan.refresh_requested && scan.worker.joinable(), "a new scan waits until installs finish");
    check(scan.known.empty(), "results from the cancelled scan are ignored");

    panel.installing = false;
    panel.removal.busy = true;
    pump_vram_scan(panel);
    check(scan.refresh_requested, "a new scan also waits until removals finish");

    panel.removal.busy = false;
    pump_vram_scan(panel);
    check(!scan.refresh_requested && !scan.worker.joinable(), "the latest refresh is taken once the folders settle");
    check(scan.done.empty() && scan.known.empty(), "old results are cleared when the newest list is empty");
    return failures ? 1 : 0;
}
