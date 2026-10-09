import AppKit

final class LauncherDelegate: NSObject, NSApplicationDelegate {
    let runtime: MacRuntime
    private var window: NSWindow!
    private var status: NSTextField!
    private var buttons: [NSButton] = []
    private var busy = false
    private let sessionState = NSLock()
    private var ownsSession = false

    init(runtime: MacRuntime) { self.runtime = runtime }
    func applicationDidFinishLaunching(_ notification: Notification) {
        let menu = NSMenu(), item = NSMenuItem(), submenu = NSMenu()
        submenu.addItem(withTitle: "Quit ReSkate", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = submenu; menu.addItem(item); NSApp.mainMenu = menu
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 510, height: 220),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "ReSkate"
        window.isReleasedWhenClosed = false
        let title = NSTextField(labelWithString: "ReSkate")
        title.font = .boldSystemFont(ofSize: 25)
        title.frame = NSRect(x: 24, y: 160, width: 460, height: 35)
        window.contentView?.addSubview(title)
        status = NSTextField(wrappingLabelWithString: "Ready")
        status.frame = NSRect(x: 24, y: 91, width: 460, height: 60)
        window.contentView?.addSubview(status)
        for (index, entry) in [("Open launcher", #selector(openLauncher)), ("Play offline", #selector(play)), ("Game folder…", #selector(chooseFolder))].enumerated() {
            let button = NSButton(title: entry.0, target: self, action: entry.1)
            button.frame = NSRect(x: 20 + index * 158, y: 28, width: 152, height: 32)
            window.contentView?.addSubview(button); buttons.append(button)
        }
        window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        if files.fileExists(atPath: runtime.configURL.path) { openLauncher() } else { chooseFolder() }
    }

    func showError(_ error: Error) {
        let alert = NSAlert()
        alert.messageText = "ReSkate could not start"
        alert.informativeText = error.localizedDescription + "\n\nLog: " + runtime.log.path
        let rosetta = URL(fileURLWithPath: "/System/Library/CoreServices/Rosetta 2 Updater.app")
        if (error as? MacError)?.needsRosetta == true && files.fileExists(atPath: rosetta.path) {
            alert.informativeText = "ReSkate needs Rosetta to run the Windows game. Open Apple's installer, then reopen ReSkate when installation finishes."
            alert.addButton(withTitle: "Install Rosetta")
            alert.addButton(withTitle: "Cancel")
            if alert.runModal() == .alertFirstButtonReturn { NSWorkspace.shared.open(rosetta) }
            return
        }
        alert.runModal()
    }

    func work(_ message: String, task: @escaping () throws -> Void, completed: (() -> Void)? = nil) {
        guard !busy else { return }
        busy = true; buttons.forEach { $0.isEnabled = false }; status.stringValue = message
        DispatchQueue.global(qos: .userInitiated).async {
            var caught: Error?
            do { try task() } catch { caught = error }
            let result = caught
            DispatchQueue.main.async {
                self.busy = false; self.buttons.forEach { $0.isEnabled = true }
                self.status.stringValue = "Ready"
                if let result = result { self.showError(result) } else { completed?() }
            }
        }
    }

    @objc func chooseFolder() {
        guard !busy else { return }
        let panel = NSOpenPanel()
        panel.title = "Choose your skate. folder"
        panel.message = "Choose an empty folder to download the game, or your existing skate. folder. You need your own copy on Steam."
        panel.canChooseDirectories = true; panel.canChooseFiles = false
        panel.canCreateDirectories = true; panel.allowsMultipleSelection = false
        panel.prompt = "Use folder"
        panel.directoryURL = files.homeDirectoryForCurrentUser.appendingPathComponent("Games")
        guard panel.runModal() == .OK, let game = panel.url else { return }
        work("Preparing ReSkate for the first launch…", task: { try self.runtime.setup(game: game) }, completed: { self.openLauncher() })
    }

    @objc func openLauncher() { start(play: false) }
    @objc func play() { start(play: true) }
    func start(play: Bool) {
        guard files.fileExists(atPath: runtime.configURL.path) else { chooseFolder(); return }
        work(play ? "Starting skate. First-time shader preparation can take several minutes." : "ReSkate is running. Use its window to download, play, or install mods.", task: {
            let status = try self.runtime.launch(play: play, acquired: {
                self.sessionState.lock(); self.ownsSession = true; self.sessionState.unlock()
            }, released: {
                self.sessionState.lock(); self.ownsSession = false; self.sessionState.unlock()
            })
            if status != 0 { throw failure("ReSkate exited with code \(status).") }
        })
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // A second instance which failed to acquire the lock must never kill the
        // first one's game. Hold the state lock through termination of our session.
        sessionState.lock()
        defer { sessionState.unlock() }
        if ownsSession { _ = try? runtime.stop() }
        else if busy { return .terminateCancel } // finish first-run setup atomically
        return .terminateNow
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        window.makeKeyAndOrderFront(nil); return true
    }
}

@main struct Launcher {
    static func main() {
        var args = Array(CommandLine.arguments.dropFirst())
        var root = MacRuntime.defaultRoot
        if let index = args.firstIndex(of: "--data-root") {
            guard index + 1 < args.count else { fputs("--data-root requires a folder\n", stderr); exit(2) }
            root = URL(fileURLWithPath: args[index + 1]).standardizedFileURL
            args.removeSubrange(index...index + 1)
        }
        let runtime = MacRuntime(resources: Bundle.main.resourceURL!, root: root)
        if !args.isEmpty {
            do { exit(try cli(args, runtime: runtime)) }
            catch { fputs(error.localizedDescription + "\n", stderr); exit(1) }
        }
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        let delegate = LauncherDelegate(runtime: runtime)
        app.delegate = delegate
        withExtendedLifetime(delegate) { app.run() }
    }

    static func cli(_ args: [String], runtime: MacRuntime) throws -> Int32 {
        switch args[0] {
        case "--help", "help":
            print("ReSkate [--data-root FOLDER] setup GAME_FOLDER | launcher | play | status | stop | wine ARGS…\nWith no command, open the Mac app. Updates and mods are managed in the ReSkate launcher.")
            return 0
        case "setup":
            guard args.count == 2 else { throw failure("Usage: ReSkate setup GAME_FOLDER") }
            try runtime.setup(game: URL(fileURLWithPath: args[1])); return 0
        case "launcher", "play": return try runtime.launch(play: args[0] == "play", arguments: Array(args.dropFirst()))
        case "stop": return try runtime.stop()
        case "status":
            print("Data: \(runtime.root.path)\nGame: \((try? runtime.installation().game) ?? "Not configured")")
            return try run(runtime.wine, ["--version"], environment: runtime.environment(), timeout: 15)
        case "wine":
            guard args.count > 1 else { throw failure("Usage: ReSkate wine ARGS…") }
            return try run(runtime.wine, Array(args.dropFirst()), environment: runtime.environment())
        default: throw failure("Unknown command: \(args[0]). Use --help.")
        }
    }
}
