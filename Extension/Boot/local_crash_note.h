#pragma once

// Test builds: when the game dies with nothing shown (no error box, no report), a note of how it died is written
// to the file named by RESKATE_CRASH_NOTE_FILE: the exception code, the address, the module and the offset in it.
// Only an unhandled exception is noted, and nothing is sent anywhere. Without the variable, nothing happens.
namespace dingosdk {
void start_local_crash_note();
}
