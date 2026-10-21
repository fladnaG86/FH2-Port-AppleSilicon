// FH2 Port launcher: installs the layer into a CrossOver bottle and starts the
// wrapper there. First launch (or launch with the Option key held) asks for the
// bottle and the folder that holds azimuth.exe; later launches go straight to
// the game. The layer is reinstalled only when the bundled DLLs differ from the
// installed ones. The wrapper's own files are never modified.

import AppKit
import CryptoKit

let crossover = URL(fileURLWithPath: "/Applications/CrossOver.app")
let defaultBottlesDir = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Application Support/CrossOver/Bottles")
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

/// Folders CrossOver keeps bottles in: the one chosen in its preferences
/// ("Choose a Bottles Folder", e.g. on an external disk), BottlePath in
/// CrossOver.conf, CX_BOTTLE_PATH, and the default one.
func bottleDirs() -> [URL] {
    var paths: [String] = []
    let cx = UserDefaults(suiteName: "com.codeweavers.CrossOver")
    if let dir = cx?.string(forKey: "BottleDir") { paths.append(dir) }
    if let data = cx?.data(forKey: "BottleDir") {
        var stale = false
        if let url = try? URL(resolvingBookmarkData: data, options: [.withoutUI], bookmarkDataIsStale: &stale) {
            paths.append(url.path)
        }
    }
    let conf = defaultBottlesDir.deletingLastPathComponent().appendingPathComponent("CrossOver.conf")
    if let text = try? String(contentsOf: conf, encoding: .utf8) {
        for line in text.split(whereSeparator: \.isNewline) {
            let l = line.trimmingCharacters(in: .whitespaces)
            guard l.hasPrefix("\"BottlePath\""), let eq = l.firstIndex(of: "=") else { continue }
            let value = l[l.index(after: eq)...].trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\""))
            paths += value.split(separator: ":").map(String.init)
        }
    }
    if let env = ProcessInfo.processInfo.environment["CX_BOTTLE_PATH"] { paths += env.split(separator: ":").map(String.init) }
    paths.append(defaultBottlesDir.path)
    var seen = Set<String>()
    return paths.map { URL(fileURLWithPath: ($0 as NSString).expandingTildeInPath).standardizedFileURL }
        .filter { seen.insert($0.resolvingSymlinksInPath().path).inserted }
}

func isBottle(_ dir: URL) -> Bool {
    FileManager.default.fileExists(atPath: dir.appendingPathComponent("drive_c").path)
}

/// Full paths of every bottle found in the bottle folders.
func bottles() -> [URL] {
    bottleDirs().flatMap { dir -> [URL] in
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        return names.sorted().map { dir.appendingPathComponent($0) }.filter(isBottle)
    }
}

/// Older versions saved only the bottle name, which lives in the default folder.
func bottlePath(_ saved: String) -> URL {
    saved.hasPrefix("/") ? URL(fileURLWithPath: saved) : defaultBottlesDir.appendingPathComponent(saved)
}

struct Target {
    let bottle: String   // full path of the bottle folder
    let folder: String   // wrapper folder relative to drive_c, with "/" separators
    var name: String { (bottle as NSString).lastPathComponent }
    /// What gets saved: the name for bottles in the default folder (as older
    /// versions did), the full path otherwise.
    var key: String {
        URL(fileURLWithPath: bottle).deletingLastPathComponent().standardizedFileURL.path == defaultBottlesDir.standardizedFileURL.path ? name : bottle
    }
    var driveC: URL { URL(fileURLWithPath: bottle).appendingPathComponent("drive_c") }
    var wrapperDir: URL { driveC.appendingPathComponent(folder) }
    var env: [String: String] { ["BOTTLE": bottle, "AZDIR": folder, "FH2_BUILD": layerDir.path] }
}

func savedTarget() -> Target? {
    guard let b = defaults.string(forKey: "Bottle"), let f = defaults.string(forKey: "WrapperFolder") else { return nil }
    let t = Target(bottle: bottlePath(b).path, folder: f)
    return FileManager.default.fileExists(atPath: t.wrapperDir.appendingPathComponent("azimuth.exe").path) ? t : nil
}

enum SetupResult { case target(Target), uninstall(Target), quit }

/// Lets the user pick a bottle folder anywhere (bottles CrossOver keeps in a
/// folder this app does not know about).
func chooseBottleFolder() -> URL? {
    while true {
        let panel = NSOpenPanel()
        panel.message = "Choose the CrossOver bottle folder (the one that contains drive_c)"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = bottleDirs().first
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let dir = panel.url else { return nil }
        if isBottle(dir) { return dir }
        if alert("Not a bottle", "There is no drive_c in \(dir.lastPathComponent).", buttons: ["Choose again", "Quit"]) != 0 { return nil }
    }
}

/// Asks for the bottle, then for the folder that contains azimuth.exe inside it.
func setup(current: Target?) -> SetupResult {
    let found = bottles()
    let a = NSAlert()
    a.messageText = "Choose the CrossOver bottle"
    a.informativeText = found.isEmpty
        ? "No bottle found in \(bottleDirs().map { $0.path }.joined(separator: ", ")). Choose the bottle folder that holds the wrapper (azimuth.exe), or create a Windows 10 64-bit bottle in CrossOver first."
        : "Pick the bottle that holds the wrapper (azimuth.exe). Next you choose its folder."
    let otherTitle = "Other bottle folder…"
    let popup = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 360, height: 26))
    // Names alone when they are unique, otherwise with their folder.
    let names = found.map { $0.lastPathComponent }
    for url in found {
        let n = url.lastPathComponent
        popup.addItem(withTitle: names.filter { $0 == n }.count > 1 ? "\(n) — \(url.deletingLastPathComponent().path)" : n)
        popup.lastItem?.representedObject = url
    }
    if !found.isEmpty { popup.menu?.addItem(.separator()) }
    popup.addItem(withTitle: otherTitle)
    if let c = current, let i = found.firstIndex(where: { $0.resolvingSymlinksInPath().path == URL(fileURLWithPath: c.bottle).resolvingSymlinksInPath().path }) {
        popup.selectItem(at: i)
    }
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
    guard let bottleURL = (popup.selectedItem?.representedObject as? URL) ?? chooseBottleFolder() else { return .quit }
    let bottle = bottleURL.lastPathComponent
    let driveC = bottleURL.appendingPathComponent("drive_c").resolvingSymlinksInPath()
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
            return .target(Target(bottle: bottleURL.path, folder: String(dir.path.dropFirst(driveC.path.count + 1))))
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
    let settingsKey = "SettingsApplied:\(t.key)/\(t.folder)"
    let wantSettings = !defaults.bool(forKey: settingsKey) && FileManager.default.fileExists(atPath: ini.path)
    if !stale && !wantSettings && defaults.string(forKey: "InstalledFor") == "\(t.key)/\(t.folder)" { return true }
    let (status, out) = runScript("install.sh", wantSettings ? ["--settings"] : [], env: t.env)
    if status != 0 {
        _ = alert("Installation failed", String(out.suffix(800)))
        return false
    }
    if wantSettings { defaults.set(true, forKey: settingsKey) }
    defaults.set("\(t.key)/\(t.folder)", forKey: "InstalledFor")
    if !FileManager.default.fileExists(atPath: t.wrapperDir.appendingPathComponent("d3dcompiler_47.dll").path) {
        _ = alert("d3dcompiler_47.dll is missing",
                  "The wrapper needs Microsoft's d3dcompiler_47.dll in \(t.folder). See the README for where to get it.")
    }
    return true
}

func main() {
    NSApp.setActivationPolicy(.regular)
    guard FileManager.default.fileExists(atPath: crossover.path) else {
        _ = alert("CrossOver not found", "This app runs the game through CrossOver (with D3DMetal). Install CrossOver in /Applications first.")
        return
    }
    var target = savedTarget()
    if target == nil || NSEvent.modifierFlags.contains(.option) {
        switch setup(current: target) {
        case .quit: return
        case .uninstall(let t):
            let (status, out) = runScript("install.sh", ["--uninstall"], env: t.env)
            defaults.removeObject(forKey: "InstalledFor")
            _ = alert(status == 0 ? "Layer removed" : "Uninstall failed", String(out.suffix(800)))
            return
        case .target(let t):
            defaults.set(t.key, forKey: "Bottle")
            defaults.set(t.folder, forKey: "WrapperFolder")
            target = t
        }
    }
    guard let t = target, ensureInstalled(t) else { return }
    let (status, out) = runScript("run.sh", [], env: t.env)
    if status != 0 { _ = alert("Could not start the game", String(out.suffix(800))) }
}

let app = NSApplication.shared
main()
