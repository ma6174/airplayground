// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "capture-cli",
    platforms: [.macOS(.v12)],
    products: [
        .executable(name: "capture-cli", targets: ["CaptureCLI"])
    ],
    targets: [
        .executableTarget(name: "CaptureCLI")
    ]
)
