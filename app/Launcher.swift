// FH2 Port launcher: installs the layer into a CrossOver bottle and starts the
// wrapper there. First launch (or launch with the Option key held) asks for the
// bottle, the CrossOver installation and the folder that holds azimuth.exe;
// later launches go straight to the game. The bottle folder comes from
// CrossOver's own "BottleDir" preference. The layer is reinstalled only when the
// bundled DLLs differ from the installed ones. The wrapper's own files are never
// modified.

import AppKit
import CryptoKit

let defaultBottlesDir = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Application Support/CrossOver/Bottles")

/// Folder CrossOver itself uses for user bottles: its macOS preference
/// "BottleDir", else the standard location.
func bottleRoot() -> URL {
    if let p = UserDefaults(suiteName: "com.codeweavers.CrossOver")?.string(forKey: "BottleDir"), !p.isEmpty {
        return URL(fileURLWithPath: p)
    }
    return defaultBottlesDir
}

struct CrossOverInstall {
    let app: URL            // /Applications/CrossOver Preview.app
    let title: String       // "CrossOver Preview 20261015"
    let version: String     // CFBundleShortVersionString
    let d3dmetal: String    // "4.0b3", "" when the installation has none
}

/// The first four integers in `s`, missing ones as 0: "4.0b3" -> [4,0,3,0].
func versionParts(_ s: String) -> [Int] {
    var parts: [Int] = []
    var digits = ""
    for ch in s {
        if ch.isNumber {
            digits.append(ch)
        } else if !digits.isEmpty {
            parts.append(Int(digits) ?? 0)
            digits = ""
            if parts.count == 4 { break }
        }
    }
    if parts.count < 4, !digits.isEmpty { parts.append(Int(digits) ?? 0) }
    while parts.count < 4 { parts.append(0) }
    return parts
}

/// CrossOver apps in /Applications, ranked: D3DMetal version descending, then
/// CrossOver version descending, then path ascending.
func crossOverInstalls() -> [CrossOverInstall] {
    let fm = FileManager.default
    let appsDir = URL(fileURLWithPath: "/Applications")
    var found: [CrossOverInstall] = []
    for name in (try? fm.contentsOfDirectory(atPath: appsDir.path)) ?? [] where name.hasSuffix(".app") {
        let app = appsDir.appendingPathComponent(name)
        let support = app.appendingPathComponent("Contents/SharedSupport/CrossOver")
        guard fm.fileExists(atPath: support.appendingPathComponent("bin/wine").path),
              let info = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")),
              info["CFBundleIdentifier"] as? String == "com.codeweavers.CrossOver" else { continue }
        let version = info["CFBundleShortVersionString"] as? String ?? ""
        var d3dmetal = ""
        for rel in ["lib64/apple_gptk/external/D3DMetal.framework/Resources/Info.plist",
                    "lib/apple_gptk/external/D3DMetal.framework/Resources/Info.plist"] {
            if let p = NSDictionary(contentsOf: support.appendingPathComponent(rel)),
               let v = p["CFBundleShortVersionString"] as? String { d3dmetal = v; break }
        }
        found.append(CrossOverInstall(app: app,
                                      title: "\(name.dropLast(4)) \(version)",
                                      version: version,
                                      d3dmetal: d3dmetal))
    }
    return found.sorted { a, b in
        let da = versionParts(a.d3dmetal), db = versionParts(b.d3dmetal)
        if da != db { return db.lexicographicallyPrecedes(da) }
        let va = versionParts(a.version), vb = versionParts(b.version)
        if va != vb { return vb.lexicographicallyPrecedes(va) }
        return a.app.path < b.app.path
    }
}
let resources = Bundle.main.resourceURL!
let layerDir = resources.appendingPathComponent("layer")
let scriptsDir = resources.appendingPathComponent("scripts")
let logURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/FH2Port/launcher.log")
let defaults = UserDefaults.standard

func log(_ text: String) {
    try? FileManager.default.createDirectory(at: logURL.deletingLastPathComponent(), withIntermediateDirectories: true)
    let line = "\(ISO8601DateFormatter().string(from: Date())) \(text)\n"
    if let h = try? FileHandle(forWritingTo: logURL) {
        h.seekToEndOfFile()
        h.write(line.data(using: .utf8)!)
        try? h.close()
    } else {
        try? line.write(to: logURL, atomically: true, encoding: .utf8)
    }
}

func alert(_ title: String, _ text: String, buttons: [String] = ["OK"]) -> Int {
    let a = NSAlert()
    a.messageText = title
    a.informativeText = text
    buttons.forEach { a.addButton(withTitle: $0) }
    NSApp.activate(ignoringOtherApps: true)
    return a.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
}

/// Runs a bundled script with zsh; returns its exit status and output.
@discardableResult
func runScript(_ name: String, _ args: [String], env extra: [String: String]) -> (Int32, String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/bin/zsh")
    p.arguments = [scriptsDir.appendingPathComponent(name).path] + args
    p.environment = ProcessInfo.processInfo.environment.merging(extra) { $1 }
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe
    do { try p.run() } catch { return (-1, "\(error)") }
    let out = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
    p.waitUntilExit()
    log("\(name) \(args.joined(separator: " ")) -> \(p.terminationStatus)\n\(out)")
    return (p.terminationStatus, out)
}

func sha256(_ url: URL) -> String? {
    guard let data = try? Data(contentsOf: url) else { return nil }
    return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

func bottles(in root: URL) -> [String] {
    let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
    return names.filter { FileManager.default.fileExists(atPath: root.appendingPathComponent("\($0)/drive_c").path) }.sorted()
}

struct Target {
    let bottle: String
    let root: URL        // folder containing the bottle
    let folder: String   // wrapper folder relative to drive_c, with "/" separators
    let crossOver: URL   // the CrossOver .app that owns the bottle
    var driveC: URL { root.appendingPathComponent("\(bottle)/drive_c") }
    var wrapperDir: URL { driveC.appendingPathComponent(folder) }
    var env: [String: String] {
        ["BOTTLE": bottle, "AZDIR": folder, "FH2_BUILD": layerDir.path,
         "BOTTLE_ROOT": root.path,
         "CX": crossOver.appendingPathComponent("Contents/SharedSupport/CrossOver").path]
    }
}

func savedTarget() -> Target? {
    guard let b = defaults.string(forKey: "Bottle"), let f = defaults.string(forKey: "WrapperFolder") else { return nil }
    let root = URL(fileURLWithPath: defaults.string(forKey: "BottleRoot") ?? bottleRoot().path)
    guard let app = defaults.string(forKey: "CrossOverApp").map(URL.init(fileURLWithPath:))
            ?? crossOverInstalls().first?.app else { return nil }
    let t = Target(bottle: b, root: root, folder: f, crossOver: app)
    let fm = FileManager.default
    return fm.fileExists(atPath: t.wrapperDir.appendingPathComponent("azimuth.exe").path)
        && fm.fileExists(atPath: app.appendingPathComponent("Contents/SharedSupport/CrossOver/bin/wine").path) ? t : nil
}

enum SetupResult { case target(Target), uninstall(Target), quit }

/// Asks for the bottle and the CrossOver installation, then for the folder that
/// contains azimuth.exe inside the bottle.
func setup(current: Target?, root: URL, installs: [CrossOverInstall]) -> SetupResult {
    let names = bottles(in: root)
    if names.isEmpty {
        _ = alert("No CrossOver bottle found",
                  "Create a Windows 10 64-bit bottle in CrossOver, put the Forza Horizon 2 wrapper in it, then open this app again.\n\nLooking in \(root.path), the folder CrossOver uses for its bottles.")
        return .quit
    }
    let a = NSAlert()
    a.messageText = "Choose the CrossOver bottle"
    a.informativeText = "Pick the bottle that holds the wrapper (azimuth.exe). Next you choose its folder."
    let popup = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 260, height: 26))
    popup.addItems(withTitles: names)
    if let c = current, names.contains(c.bottle) { popup.selectItem(withTitle: c.bottle) }
    a.accessoryView = popup
    a.addButton(withTitle: "Continue")
    a.addButton(withTitle: "Quit")
    if current != nil { a.addButton(withTitle: "Uninstall the layer") }
    NSApp.activate(ignoringOtherApps: true)
    switch a.runModal() {
    case .alertFirstButtonReturn: break
    case .alertThirdButtonReturn: return current.map { .uninstall($0) } ?? .quit
    default: return .quit
    }
    let bottle = popup.titleOfSelectedItem!
    var crossOver = installs[0].app
    if installs.count > 1 {
        let c = NSAlert()
        c.messageText = "Choose the CrossOver installation"
        c.informativeText = "The layer needs the D3DMetal that comes with CrossOver. The list is ordered by D3DMetal version, newest first."
        let pop = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 340, height: 26))
        pop.addItems(withTitles: installs.map { "\($0.title) — D3DMetal \($0.d3dmetal.isEmpty ? "none" : $0.d3dmetal)" })
        if let i = current.flatMap({ c0 in installs.firstIndex(where: { $0.app.path == c0.crossOver.path }) }) {
            pop.selectItem(at: i)
        }
        c.accessoryView = pop
        c.addButton(withTitle: "Continue")
        c.addButton(withTitle: "Quit")
        NSApp.activate(ignoringOtherApps: true)
        guard c.runModal() == .alertFirstButtonReturn else { return .quit }
        crossOver = installs[max(0, min(pop.indexOfSelectedItem, installs.count - 1))].app
    }
    let driveC = root.appendingPathComponent("\(bottle)/drive_c").resolvingSymlinksInPath()
    while true {
        let panel = NSOpenPanel()
        panel.message = "Choose the folder that contains azimuth.exe (inside the \(bottle) bottle)"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = driveC
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let dir = panel.url?.resolvingSymlinksInPath() else { return .quit }
        let inside = dir.path.hasPrefix(driveC.path + "/")
        if inside && FileManager.default.fileExists(atPath: dir.appendingPathComponent("azimuth.exe").path) {
            return .target(Target(bottle: bottle, root: root, folder: String(dir.path.dropFirst(driveC.path.count + 1)), crossOver: crossOver))
        }
        let msg = inside ? "There is no azimuth.exe in \(dir.lastPathComponent)."
                         : "The folder must be inside the bottle's drive_c."
        if alert("Not the wrapper folder", msg, buttons: ["Choose again", "Quit"]) != 0 { return .quit }
    }
}

/// Installs the layer when it is missing or differs from the bundled one, and
/// applies the recommended settings once the wrapper has created its ini.
func ensureInstalled(_ t: Target) -> Bool {
    let stale = ["d3d12.dll", "fh2pad.dll"].contains {
        sha256(layerDir.appendingPathComponent($0)) != sha256(t.wrapperDir.appendingPathComponent($0))
    }
    let ini = t.wrapperDir.appendingPathComponent("data/azimuth.ini")
    let settingsKey = "SettingsApplied:\(t.bottle)/\(t.folder)"
    let wantSettings = !defaults.bool(forKey: settingsKey) && FileManager.default.fileExists(atPath: ini.path)
    if !stale && !wantSettings && defaults.string(forKey: "InstalledFor") == "\(t.bottle)/\(t.folder)" { return true }
    let (status, out) = runScript("install.sh", wantSettings ? ["--settings"] : [], env: t.env)
    if status != 0 {
        _ = alert("Installation failed", String(out.suffix(800)))
        return false
    }
    if wantSettings { defaults.set(true, forKey: settingsKey) }
    defaults.set("\(t.bottle)/\(t.folder)", forKey: "InstalledFor")
    if !FileManager.default.fileExists(atPath: t.wrapperDir.appendingPathComponent("d3dcompiler_47.dll").path) {
        _ = alert("d3dcompiler_47.dll is missing",
                  "The wrapper needs Microsoft's d3dcompiler_47.dll in \(t.folder). See the README for where to get it.")
    }
    return true
}

func main() {
    NSApp.setActivationPolicy(.regular)
    let installs = crossOverInstalls()
    guard !installs.isEmpty else {
        _ = alert("CrossOver not found", "This app runs the game through CrossOver (with D3DMetal). No CrossOver installation with bin/wine was found in /Applications. Install CrossOver there first.")
        return
    }
    let root = bottleRoot()
    var target = savedTarget()
    if target == nil || NSEvent.modifierFlags.contains(.option) {
        switch setup(current: target, root: root, installs: installs) {
        case .quit: return
        case .uninstall(let t):
            let (status, out) = runScript("install.sh", ["--uninstall"], env: t.env)
            defaults.removeObject(forKey: "InstalledFor")
            _ = alert(status == 0 ? "Layer removed" : "Uninstall failed", String(out.suffix(800)))
            return
        case .target(let t):
            defaults.set(t.bottle, forKey: "Bottle")
            defaults.set(t.folder, forKey: "WrapperFolder")
            defaults.set(t.root.path, forKey: "BottleRoot")
            defaults.set(t.crossOver.path, forKey: "CrossOverApp")
            target = t
        }
    }
    guard let t = target, ensureInstalled(t) else { return }
    let (status, out) = runScript("run.sh", [], env: t.env)
    if status != 0 { _ = alert("Could not start the game", String(out.suffix(800))) }
}

let app = NSApplication.shared
main()
