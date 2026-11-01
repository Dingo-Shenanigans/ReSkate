// A FALSE from DingoSDKDebugInitialize: the reason the runtime gives, the one an
// older runtime only logged, and what the STATUS tile makes of each.
#include "Engine/Core/Platform/startup_failure.h"
#include "Launcher/problem.h"

#include <iostream>
#include <string>
#include <string_view>

using namespace dingosdk;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

void utf8_prefix_keeps_whole_characters() {
    using startup_failure::utf8_prefix;
    check(utf8_prefix("abc", 10) == 3, "a short text is kept whole");
    check(utf8_prefix("abcdef", 4) == 4, "ASCII is cut at the limit");
    const std::string e_acute = "ab\xC3\xA9";        // "abé"
    check(utf8_prefix(e_acute, 3) == 2, "a two-byte character is not split");
    check(utf8_prefix(e_acute, 4) == 4, "a two-byte character that fits is kept");
    const std::string euro = "a\xE2\x82\xAC" "b";    // "a€b"
    check(utf8_prefix(euro, 2) == 1, "a three-byte character cut after its first byte is dropped");
    check(utf8_prefix(euro, 3) == 1, "a three-byte character cut after its second byte is dropped");
    check(utf8_prefix(euro, 4) == 4, "a three-byte character that fits is kept");
}

void printable_makes_one_line() {
    using startup_failure::printable;
    check(printable("  Save is\r\nin use\t ") == "Save is  in use", "control characters become spaces, ends are trimmed");
    check(printable("\n\r\t").empty(), "only control characters leave nothing");
    check(printable("caf\xC3\xA9") == "caf\xC3\xA9", "UTF-8 is left alone");
}

void logged_errors_reads_one_process() {
    const std::string log =
        "[2026-10-07 17:29:57.548] [34456:44852] SDK: [Launcher] [INFO] Calling DingoSDKDebugInitialize at 0x7ffb2e823200\r\n"
        "[2026-10-07 17:30:03.100] [36556:44916] Native(C): [Profile] [ERROR] another game's error\r\n"
        "[2026-10-07 17:30:03.200] [48792:25272] Native(C): [Assets] [INFO] text with ] [ERROR] inside an info line\r\n"
        "[2026-10-07 17:30:03.436] [48792:25272] Native(C): [Profile] [ERROR] Profile initialization failed: Save is already in use or inaccessible; close the game before editing\r\n"
        "[2026-10-07 17:30:03.436] [48792:25272] SDK: [Runtime] [ERROR] Launcher readiness: ready=false, reason=local_profile\r\n"
        "[2026-10-07 17:30:03.454] [34456:44852] SDK: [Launcher] [ERROR] DingoSDKDebugInitialize returned FALSE\r\n"
        "[2026-10-07 17:30:03.460] [48792:25272] SDK: [Runtime] [CRITICAL] Runtime initialization failed: test\n"
        "[2026-10-07 17:30:03.470] [48792:25272] SDK: [Runtime] [WARNING] a warning is not a reason\n"
        "a line that is not a log line";
    const auto found = startup_failure::logged_errors(log, 48792, 3);
    check(found.size() == 3, "the child's ERROR and CRITICAL lines are found, up to the limit");
    if (found.size() == 3) {
        check(found[0] == "Profile initialization failed: Save is already in use or inaccessible; close the game before editing",
              "the message is taken without its prefix or line ending");
        check(found[1] == "Launcher readiness: ready=false, reason=local_profile", "lines stay in order");
        check(found[2] == "Runtime initialization failed: test", "CRITICAL counts");
    }
    const auto newest = startup_failure::logged_errors(log, 48792, 1);
    check(newest.size() == 1 && newest[0] == "Runtime initialization failed: test", "a limit keeps the newest");
    check(startup_failure::logged_errors(log, 4879, 3).empty(), "a process id is matched whole, not as a prefix");
    check(startup_failure::logged_errors(log, 48792, 0).empty(), "a zero limit finds nothing");
}

void the_reason_leads_the_status_tile() {
    using launcher_problem::explain;
    const auto in_use = explain("ReSkate stopped while starting: Profile initialization failed: Save is already in use "
                                "or inaccessible; close the game before editing");
    check(in_use.headline == "Skate is already running", "a save in use says Skate is already running");
    check(!contains(in_use.advice, "nti-virus"), "a save in use does not blame anti-virus");

    const auto logged = explain("ReSkate stopped while starting; ReSkate.log says: Profile initialization failed: Save is "
                                "already in use or inaccessible; close the game before editing | Launcher readiness: "
                                "ready=false, reason=local_profile");
    check(logged.headline == "Skate is already running", "an older runtime's logged reason is recognised too");

    const auto folder = explain("ReSkate stopped while starting: Another Skate is already running from this folder; "
                                "this one does not start.");
    check(folder.headline == "Skate is already running", "a folder another game holds says Skate is already running");
    check(!contains(folder.advice, "nti-virus"), "a folder in use does not blame anti-virus");

    const auto image = explain("ReSkate stopped while starting: Game image validation failed.");
    check(image.headline == "ReSkate stopped while starting", "an unrecognised reason keeps the plain headline");
    check(image.advice.starts_with("Game image validation failed."), "the reason leads the advice");
    check(!contains(image.advice, "nti-virus"), "an unrecognised reason does not blame anti-virus");

    const auto hook = explain("ReSkate stopped while starting: The engine update hook could not be installed: "
                              "Access is denied (5)");
    check(hook.headline == "ReSkate could not hook into Skate", "a hook failure says so");
    check(hook.advice.starts_with("The engine update hook could not be installed: Access is denied (5). "),
          "the reason ends as a sentence before the advice");

    const auto overlay = explain("ReSkate stopped while starting: Overlay startup failed.");
    check(overlay.headline == "ReSkate could not hook into Skate", "an overlay that failed to start is a hook failure");
    check(overlay.advice.starts_with("Overlay startup failed. "), "a reason that ends a sentence is not given a second stop");

    const auto silent = explain("ReSkate stopped while starting without saying why (DingoSDKDebugInitialize returned FALSE)");
    check(silent.headline == "ReSkate stopped while starting", "no reason keeps the plain headline");
    check(contains(silent.advice, "Open logs") && !contains(silent.advice, "nti-virus"),
          "no reason points at the log, not at anti-virus");

    const auto unsupported = explain("ReSkate stopped while starting: Mod layout layers: failed - Native layout-layer "
                                     "contract does not match the supported game");
    check(unsupported.headline == "ReSkate stopped while starting",
          "a reason is not mistaken for an unsupported game build by the rules after it");
}

void attach_failures_keep_their_advice() {
    using launcher_problem::explain;
    check(explain("CreateRemoteThread(DingoSDKDebugInitialize) failed (5): Access is denied.").headline ==
              "ReSkate could not attach to Skate", "a remote thread that cannot start is still an attach failure");
    check(explain("Remote DingoSDKDebugInitialize address is invalid").headline == "ReSkate could not attach to Skate",
          "a bad remote address is still an attach failure");
    check(explain("DingoSDKDebugInitialize timed out after 30 s").headline == "Skate took too long to start",
          "a timeout keeps its own advice");
}

} // namespace

int main() {
    utf8_prefix_keeps_whole_characters();
    printable_makes_one_line();
    logged_errors_reads_one_process();
    the_reason_leads_the_status_tile();
    attach_failures_keep_their_advice();
    if (failures) {
        std::cerr << failures << " startup failure check(s) failed\n";
        return 1;
    }
    std::cout << "startup failure checks passed\n";
    return 0;
}
