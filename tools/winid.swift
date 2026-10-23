// Prints "<window id> <owner pid> <title>" for on-screen windows (for
// screencapture -l <id>: one window, never the whole desktop).
import CoreGraphics
import Foundation
let opts = CGWindowListOption(arrayLiteral: .optionOnScreenOnly, .excludeDesktopElements)
if let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] {
    for w in list {
        if let id = w[kCGWindowNumber as String] as? Int, let name = w[kCGWindowName as String] as? String, !name.isEmpty {
            let pid = w[kCGWindowOwnerPID as String] as? Int ?? 0
            print("\(id) \(pid) \(name)")
        }
    }
}
