import Foundation
import Darwin

let files = FileManager.default
struct MacError: LocalizedError {
    let message: String
    var needsRosetta = false
    var errorDescription: String? { message }
}
func failure(_ message: String) -> MacError { MacError(message: message) }
func directory(_ url: URL) throws {
    try files.createDirectory(at: url, withIntermediateDirectories: true)
}

// A separate lock is also held in the game folder: two data roots must not launch
// or install into the same game simultaneously. Never unlink a flock lock file.
final class SessionLock {
    private let descriptor: Int32
    init(_ url: URL) throws {
        descriptor = open(url.path, O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
        guard descriptor >= 0 else { throw failure("Cannot open the launch lock: \(url.path)") }
        guard flock(descriptor, LOCK_EX | LOCK_NB) == 0 else {
            close(descriptor)
            throw failure("ReSkate is already running. Close it before starting another session.")
        }
    }
    deinit { close(descriptor) }
}

@discardableResult
func run(_ executable: String, _ arguments: [String], environment: [String: String]? = nil,
         cwd: URL? = nil, log: URL? = nil, timeout: TimeInterval = 0) throws -> Int32 {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = arguments
    process.environment = environment
    process.currentDirectoryURL = cwd
    var output: FileHandle?
    if let log = log {
        if !files.fileExists(atPath: log.path) { files.createFile(atPath: log.path, contents: nil) }
        output = try FileHandle(forWritingTo: log)
        try output?.seekToEnd()
        process.standardOutput = output
        process.standardError = output
    }
    defer { try? output?.close() }
    try process.run()
    let deadline = Date().addingTimeInterval(timeout)
    while process.isRunning {
        if timeout > 0 && Date() > deadline {
            process.terminate()
            Thread.sleep(forTimeInterval: 0.2)
            if process.isRunning { kill(process.processIdentifier, SIGKILL) }
            process.waitUntilExit()
            throw failure("\(URL(fileURLWithPath: executable).lastPathComponent) timed out. See the Mac launcher log.")
        }
        Thread.sleep(forTimeInterval: 0.1)
    }
    return process.terminationStatus
}

struct Installation: Codable { let game: String }

struct MacRuntime {
    let resources: URL
    let root: URL
    var configURL: URL { root.appendingPathComponent("installation.json") }
    var prefix: URL { root.appendingPathComponent("prefix") }
    var log: URL { root.appendingPathComponent("launcher.log") }
    var wine: String { resources.appendingPathComponent("runtime/wine/bin/wine").path }
    var server: String { resources.appendingPathComponent("runtime/wine/bin/wineserver").path }

    static var defaultRoot: URL {
        files.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/ReSkateMac")
    }

    func environment(inherited: [String: String] = ProcessInfo.processInfo.environment) -> [String: String] {
        var result = inherited
        for key in Array(result.keys) where ["WINE", "DYLD_", "GST_", "CX_"].contains(where: key.hasPrefix) {
            result.removeValue(forKey: key)
        }
        let runtime = resources.appendingPathComponent("runtime").path
        let graphics = runtime + "/graphics", libraries = runtime + "/libraries"
        result.merge([
            "WINEPREFIX": prefix.path, "WINELOADER": wine, "WINESERVER": server,
            "WINEDEBUG": "-all", "ROSETTA_ADVERTISE_AVX": "1", "WINEMSYNC": "1",
            "SikarugirAppWine11": "1", "MTL_DEBUG_LAYER": "0", "MVK_CONFIG_LOG_LEVEL": "0",
            "CX_APPLEGPTK_LIBD3DSHARED_PATH": graphics + "/external/libd3dshared.dylib",
            "WINEDLLPATH": graphics + "/wine", "WINEDLLPATH_D3DMETAL": graphics + "/wine",
            "DYLD_FALLBACK_LIBRARY_PATH": libraries + ":" + libraries + "/GStreamer.framework/Libraries:" + runtime + "/wine/lib/wine/x86_64-unix",
            "GST_PLUGIN_SYSTEM_PATH_1_0": libraries + "/GStreamer.framework/Versions/1.0/lib/gstreamer-1.0",
            "GST_PLUGIN_SCANNER": libraries + "/GStreamer.framework/Versions/1.0/libexec/gstreamer-1.0/gst-plugin-scanner"
        ], uniquingKeysWith: { _, new in new })
        return result
    }

    func preflight() throws {
        guard ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 else {
            throw failure("This build requires macOS 27 or later and an Apple Silicon Mac.")
        }
        guard (try? run("/usr/bin/arch", ["-x86_64", "/usr/bin/true"], timeout: 10)) == 0 else {
            throw MacError(message: "Install Rosetta, then reopen ReSkate. In Terminal, run: softwareupdate --install-rosetta", needsRosetta: true)
        }
        for path in [wine, server, resources.appendingPathComponent("runtime/graphics/external/libd3dshared.dylib").path] {
            guard files.fileExists(atPath: path) else { throw failure("The app is incomplete. Missing: \(path)") }
        }
    }

    func installation() throws -> Installation {
        try JSONDecoder().decode(Installation.self, from: Data(contentsOf: configURL))
    }

    func withSession<T>(_ game: URL, _ body: () throws -> T) throws -> T {
        try directory(root)
        let rootLock = try SessionLock(root.appendingPathComponent("session.lock"))
        let gameLock = try SessionLock(game.appendingPathComponent(".reskate-mac.lock"))
        return try withExtendedLifetime((rootLock, gameLock), body)
    }

    func initializePrefix() throws {
        let ready = root.appendingPathComponent("prefix-ready-v1")
        if files.fileExists(atPath: ready.path) && files.fileExists(atPath: prefix.appendingPathComponent("system.reg").path) { return }
        let env = environment()
        do {
            guard try run(wine, ["wineboot", "-u"], environment: env, log: log, timeout: 180) == 0,
                  try run(server, ["-w"], environment: env, log: log, timeout: 90) == 0,
                  try run(wine, ["regedit", "/S", resources.appendingPathComponent("display.reg").path], environment: env, log: log, timeout: 60) == 0,
                  try run(server, ["-w"], environment: env, log: log, timeout: 60) == 0 else {
                throw failure("Could not prepare Wine. See \(log.path).")
            }
            try Data().write(to: ready, options: .atomic)
        } catch {
            // Only called while this root's exclusive lock is held.
            _ = try? stop()
            throw error
        }
    }

    func setup(game selected: URL) throws {
        try preflight()
        let game = selected.standardizedFileURL.resolvingSymlinksInPath()
        let home = files.homeDirectoryForCurrentUser.path
        guard !["/", home, home + "/Applications", home + "/Games"].contains(game.path),
              game.path != root.path, !game.path.hasPrefix(root.path + "/"),
              !game.path.hasPrefix(Bundle.main.bundleURL.path + "/") else {
            throw failure("Choose a dedicated game folder, such as ~/Games/ReSkate.")
        }
        try directory(game)
        try withSession(game) {
            let names = try files.contentsOfDirectory(atPath: game.path).filter { ![".DS_Store", ".reskate-mac.lock"].contains($0) }
            let existingLauncher = files.fileExists(atPath: game.appendingPathComponent("ReSkateLauncher.exe").path)
            let existingDLL = files.fileExists(atPath: game.appendingPathComponent("ReSkate.dll").path)
            guard existingLauncher == existingDLL else { throw failure("This folder has an incomplete ReSkate installation. Restore its matching launcher and DLL first.") }
            guard names.isEmpty || existingLauncher || files.fileExists(atPath: game.appendingPathComponent("Skate.exe").path) else {
                throw failure("Choose an empty folder or an existing skate. installation.")
            }
            guard files.isWritableFile(atPath: game.path) else { throw failure("The game folder is not writable.") }
            try initializePrefix()
            var installed: [URL] = []
            do {
                // Existing release binaries, settings, mods and saves belong to the
                // user. ReSkate's own updater continues to manage that installation.
                if !existingLauncher {
                    let payload = resources.appendingPathComponent("ReSkate")
                    for source in try files.contentsOfDirectory(at: payload, includingPropertiesForKeys: nil) {
                        let target = game.appendingPathComponent(source.lastPathComponent)
                        if !files.fileExists(atPath: target.path) {
                            installed.append(target)
                            try files.copyItem(at: source, to: target)
                        }
                    }
                }
                let config = try JSONEncoder().encode(Installation(game: game.path))
                try config.write(to: configURL, options: .atomic)
            } catch {
                for target in installed.reversed() { try? files.removeItem(at: target) }
                throw error
            }
        }
    }

    func launch(play: Bool, arguments: [String] = [], acquired: () -> Void = {}, released: () -> Void = {}) throws -> Int32 {
        try preflight()
        let game = URL(fileURLWithPath: try installation().game)
        return try withSession(game) {
            acquired()
            defer { released() }
            if play { try validateGame(game) }
            try initializePrefix()
            var args = ["ReSkateLauncher.exe", "-WorldRender.FrameSynthesisMode", "FrameSynthesisMode_None",
                        "-Render.ResolutionScale", "1", "-Render.ResolutionScaleMin", "1", "-Render.DynamicResolutionScaleEnable", "0"]
            if play { args += ["--no-gui", "--offline"] }
            args += arguments
            do {
                let result = try run(wine, args, environment: environment(), cwd: game, log: log)
                // The Windows launcher exits after spawning Skate.exe. Keep both locks
                // and the native app alive until its Wine session really ends.
                let waited = try run(server, ["-w"], environment: environment(), log: log)
                return result == 0 ? waited : result
            } catch {
                _ = try? stop()
                throw error
            }
        }
    }

    @discardableResult func stop() throws -> Int32 {
        let result = try run(server, ["-k"], environment: environment(), timeout: 10)
        _ = try run(server, ["-w"], environment: environment(), timeout: 20)
        return result
    }
}

func validateGame(_ game: URL) throws {
    // The launcher can repair/download these. A copied build containing only
    // Skate.exe and Data can otherwise stall indefinitely at the loading screen.
    let required = ["Skate.exe", "steam.txt", "shader_cache/6803938.PcDx12"]
    let missing = required.filter { name in
        guard let attributes = try? files.attributesOfItem(atPath: game.appendingPathComponent(name).path),
              attributes[.type] as? FileAttributeType == .typeRegular,
              let size = attributes[.size] as? NSNumber else { return true }
        return size.int64Value == 0
    }
    guard missing.isEmpty else {
        throw failure("The game download is incomplete (\(missing.joined(separator: ", "))). Open the launcher to finish or repair it. When moving an installation, copy the whole game folder.")
    }
}
