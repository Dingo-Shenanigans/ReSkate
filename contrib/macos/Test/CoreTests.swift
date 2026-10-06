import Foundation
import Darwin

func check(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
    if try !condition() { throw failure("TEST FAILED: " + message) }
}
func rejected(_ message: String, _ operation: () throws -> Void) throws {
    do { try operation() } catch { return }
    throw failure("TEST FAILED: expected rejection: " + message)
}
func write(_ text: String, _ url: URL, executable: Bool = false) throws {
    try directory(url.deletingLastPathComponent())
    try Data(text.utf8).write(to: url)
    if executable { try files.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path) }
}

@main struct CoreTests {
    static func main() throws {
        if CommandLine.arguments.count == 4 && CommandLine.arguments[1] == "worker" {
            let runtime = MacRuntime(resources: URL(fileURLWithPath: CommandLine.arguments[2]), root: URL(fileURLWithPath: CommandLine.arguments[3]))
            exit(try runtime.launch(play: true))
        }
        let temp = files.temporaryDirectory.appendingPathComponent("ReSkate tests with spaces " + UUID().uuidString)
        try directory(temp)
        defer { try? files.removeItem(at: temp) }
        let runtime = MacRuntime(resources: temp.appendingPathComponent("App Resources"), root: temp.appendingPathComponent("Data Root"))
        try write("", URL(fileURLWithPath: runtime.wine), executable: true)
        // Fake Wine creates a minimal prefix; the server can be held alive after
        // the launcher exits to test the real handoff/lock lifetime.
        try write("""
        #!/bin/sh
        if [ "$1" = wineboot ]; then
            mkdir -p "$WINEPREFIX"
            touch "$WINEPREFIX/system.reg"
        fi
        exit 0
        """, URL(fileURLWithPath: runtime.wine), executable: true)
        try write("""
        #!/bin/sh
        if [ "$1" = -w ] && [ -f "$WINEPREFIX/block" ]; then
            touch "$WINEPREFIX/waiting"
            while [ ! -f "$WINEPREFIX/release" ]; do sleep 0.1; done
        fi
        exit 0
        """, URL(fileURLWithPath: runtime.server), executable: true)
        try write("graphics", runtime.resources.appendingPathComponent("runtime/graphics/external/libd3dshared.dylib"))
        for name in ["ReSkateLauncher.exe", "ReSkate.dll", "LICENSE.txt"] {
            try write("bundled", runtime.resources.appendingPathComponent("ReSkate/" + name))
        }

        let env = runtime.environment(inherited: ["WINEPREFIX": "/wrong", "WINEDLLOVERRIDES": "bad", "DYLD_INSERT_LIBRARIES": "/wrong", "GST_DEBUG": "9", "CX_BAD": "1", "RESKATE_CRASH_REPORTING": "0", "PATH": "/bin"])
        try check(env["WINEPREFIX"] == runtime.prefix.path, "dedicated prefix")
        try check(env["WINEDLLOVERRIDES"] == nil && env["DYLD_INSERT_LIBRARIES"] == nil && env["GST_DEBUG"] == nil && env["CX_BAD"] == nil, "clear inherited Wine overrides")
        try check(env["RESKATE_CRASH_REPORTING"] == "0" && env["PATH"] == "/bin", "preserve unrelated user preferences")

        let game = temp.appendingPathComponent("Game With Spaces")
        try runtime.setup(game: game)
        try check(try runtime.installation().game == game.path, "persist chosen folder")
        let dll = game.appendingPathComponent("ReSkate.dll")
        try write("newer user build", dll)
        let mods = game.appendingPathComponent("Mods/mods.json")
        try write("user mod state", mods)
        try runtime.setup(game: game)
        try check(try String(contentsOf: dll, encoding: .utf8) == "newer user build", "preserve newer release")
        try check(try String(contentsOf: mods, encoding: .utf8) == "user mod state", "preserve mods")

        try rejected("incomplete download") { try validateGame(game) }
        for name in ["Skate.exe", "steam.txt", "shader_cache/6803938.PcDx12"] {
            try write("nonempty", game.appendingPathComponent(name))
        }
        try validateGame(game)
        let half = temp.appendingPathComponent("Incomplete ReSkate")
        try write("only launcher", half.appendingPathComponent("ReSkateLauncher.exe"))
        try rejected("mismatched launcher/DLL") { try runtime.setup(game: half) }
        try check(try runtime.installation().game == game.path, "failed setup preserves config")

        try runtime.withSession(game) {
            try rejected("same root duplicate") { try runtime.withSession(game) {} }
            let other = MacRuntime(resources: runtime.resources, root: temp.appendingPathComponent("Other Root"))
            try rejected("same game across two roots") { try other.withSession(game) {} }
        }

        // Spawn the same test executable as a launch worker. The fake launcher
        // exits immediately; a second process must still be refused while -w waits.
        try write("", runtime.prefix.appendingPathComponent("block"))
        let worker = Process()
        worker.executableURL = URL(fileURLWithPath: CommandLine.arguments[0])
        worker.arguments = ["worker", runtime.resources.path, runtime.root.path]
        try worker.run()
        defer { if worker.isRunning { worker.terminate() } }
        let deadline = Date().addingTimeInterval(10)
        while !files.fileExists(atPath: runtime.prefix.appendingPathComponent("waiting").path) && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.05)
        }
        try check(worker.isRunning && files.fileExists(atPath: runtime.prefix.appendingPathComponent("waiting").path), "wait for game after launcher exits")
        try rejected("launch handoff retains lock") { _ = try runtime.launch(play: true) }
        try write("", runtime.prefix.appendingPathComponent("release"))
        worker.waitUntilExit()
        try check(worker.terminationStatus == 0, "session completes after server exit")
        try runtime.withSession(game) {} // locks released
        try rejected("timeout") { _ = try run("/bin/sleep", ["2"], timeout: 0.1) }
        print("PASS: preservation, game validation, environment, spaces, locks, handoff, timeout")
    }
}
