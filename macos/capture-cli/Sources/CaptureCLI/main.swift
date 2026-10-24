import Foundation
import ScreenCaptureKit
import VideoToolbox
import CoreMedia
import CoreVideo
import CoreGraphics
import Darwin

struct Options {
    enum Command { case list, record }
    var command: Command
    var displayIndex: Int?
    var windowID: CGWindowID?
    var output: String?
    var stdout = false
    var fps = 30
    var bitrate: Int?
    var width: Int?
    var height: Int?
    var duration: Double?
    var showCursor = true

    static func parse(_ args: [String]) throws -> Options {
        guard let commandName = args.first else { throw CLIError.usage }
        let command: Command
        switch commandName { case "list": command = .list; case "record": command = .record; default: throw CLIError.usage }
        var o = Options(command: command)
        var i = 1
        while i < args.count {
            let arg = args[i]
            func value() throws -> String {
                i += 1
                guard i < args.count else { throw CLIError.invalid("缺少 \(arg) 的值") }
                return args[i]
            }
            switch arg {
            case "--display": o.displayIndex = try Int(value())
            case "--window-id":
                guard let id = UInt32(try value()) else { throw CLIError.invalid("window-id 必须是无符号整数") }
                o.windowID = CGWindowID(id)
            case "--output": o.output = try value()
            case "--stdout": o.stdout = true
            case "--fps":
                guard let fps = Int(try value()) else { throw CLIError.invalid("fps 必须是整数") }
                o.fps = fps
            case "--bitrate":
                guard let bitrate = Int(try value()) else { throw CLIError.invalid("bitrate 必须是整数") }
                o.bitrate = bitrate
            case "--width":
                guard let width = Int(try value()) else { throw CLIError.invalid("width 必须是整数") }
                o.width = width
            case "--height":
                guard let height = Int(try value()) else { throw CLIError.invalid("height 必须是整数") }
                o.height = height
            case "--duration":
                guard let duration = Double(try value()), duration >= 0 else { throw CLIError.invalid("duration 必须是非负数字") }
                o.duration = duration
            case "--no-cursor": o.showCursor = false
            case "--help", "-h": throw CLIError.usage
            default: throw CLIError.invalid("未知参数：\(arg)")
            }
            i += 1
        }
        guard o.fps > 0, o.fps <= 120 else { throw CLIError.invalid("fps 必须在 1 到 120 之间") }
        guard o.displayIndex == nil || o.windowID == nil else { throw CLIError.invalid("--display 和 --window-id 不能同时使用") }
        guard o.width == nil || (o.width! >= 2 && o.width! % 2 == 0) else { throw CLIError.invalid("width 必须是正偶数") }
        guard o.height == nil || (o.height! >= 2 && o.height! % 2 == 0) else { throw CLIError.invalid("height 必须是正偶数") }
        if command == .record && !o.stdout && o.output == nil { throw CLIError.invalid("record 需要 --output PATH 或 --stdout") }
        if o.stdout && o.output != nil { throw CLIError.invalid("--stdout 和 --output 只能选一个") }
        return o
    }
}

enum CLIError: Error, CustomStringConvertible {
    case usage, invalid(String), message(String)
    var description: String {
        switch self {
        case .usage: return "用法：\n  capture-cli list\n  capture-cli record (--display INDEX | --window-id ID) (--output PATH | --stdout) [选项]\n\n选项：\n  --fps N                 帧率，默认 30\n  --bitrate BPS          平均码率，默认按分辨率估算\n  --width N --height N   输出尺寸（必须为偶数）\n  --duration SECONDS     录制时长\n  --no-cursor             不捕获鼠标指针"
        case .invalid(let s), .message(let s): return s
        }
    }
}

struct Target {
    let filter: SCContentFilter
    let width: Int
    let height: Int
    let label: String
}

final class H264Encoder {
    private var session: VTCompressionSession?
    private let output: FileHandle
    private let lock = NSLock()
    private var failed: Error?
    private var frameCount = 0
    private let width: Int
    private let height: Int

    init(width: Int, height: Int, fps: Int, bitrate: Int, output: FileHandle) throws {
        self.width = width; self.height = height; self.output = output
        var s: VTCompressionSession?
        let status = VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264, encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: H264Encoder.callback, refcon: Unmanaged.passUnretained(self).toOpaque(), compressionSessionOut: &s)
        guard status == noErr, let s else { throw CLIError.message("无法创建 H.264 编码器：\(status)") }
        session = s
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_Main_AutoLevel)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: bitrate as CFTypeRef)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: fps * 2 as CFTypeRef)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: fps as CFTypeRef)
        guard VTCompressionSessionPrepareToEncodeFrames(s) == noErr else { throw CLIError.message("无法准备 H.264 编码器") }
    }

    func encode(_ sampleBuffer: CMSampleBuffer) {
        guard let session, let image = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration = CMSampleBufferGetDuration(sampleBuffer)
        var flags = VTEncodeInfoFlags()
        let status = VTCompressionSessionEncodeFrame(session, imageBuffer: image, presentationTimeStamp: pts, duration: duration, frameProperties: nil, sourceFrameRefcon: nil, infoFlagsOut: &flags)
        if status != noErr { setFailure(CLIError.message("H.264 编码失败：\(status)")) }
    }

    func finish() {
        if let session { VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid); VTCompressionSessionInvalidate(session) }
        try? output.close()
    }

    var error: Error? { lock.lock(); defer { lock.unlock() }; return failed }
    private func setFailure(_ error: Error) { lock.lock(); if failed == nil { failed = error }; lock.unlock() }

    private static let callback: VTCompressionOutputCallback = { refcon, _, status, _, sampleBuffer in
        guard let refcon else { return }
        let encoder = Unmanaged<H264Encoder>.fromOpaque(refcon).takeUnretainedValue()
        guard status == noErr, let sampleBuffer, CMSampleBufferDataIsReady(sampleBuffer) else { encoder.setFailure(CLIError.message("编码器回调失败：\(status)")); return }
        encoder.write(sampleBuffer)
    }

    private func write(_ sampleBuffer: CMSampleBuffer) {
        guard let format = CMSampleBufferGetFormatDescription(sampleBuffer), let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
        if frameCount == 0 { writeParameterSets(format) }
        var length = 0; var pointer: UnsafeMutablePointer<Int8>?
        guard CMBlockBufferGetDataPointer(dataBuffer, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length, dataPointerOut: &pointer) == noErr, let pointer else { return }
        var offset = 0
        while offset + 4 <= length {
            let n = Int(UInt32(bigEndian: pointer.advanced(by: offset).withMemoryRebound(to: UInt32.self, capacity: 1) { $0.pointee }))
            guard n >= 0, offset + 4 + n <= length else { setFailure(CLIError.message("收到损坏的 AVCC 帧")); return }
            output.write(Data([0, 0, 0, 1])); output.write(Data(bytes: pointer.advanced(by: offset + 4), count: n))
            offset += 4 + n
        }
        frameCount += 1
    }

    private func writeParameterSets(_ format: CMFormatDescription) {
        for index in 0..<2 {
            var parameter: UnsafePointer<UInt8>?; var size = 0; var count = 0
            guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &parameter, parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil) == noErr, let parameter else { continue }
            output.write(Data([0, 0, 0, 1])); output.write(Data(bytes: parameter, count: size))
        }
    }
}

final class CaptureDelegate: NSObject, SCStreamOutput {
    let encoder: H264Encoder
    init(encoder: H264Encoder) { self.encoder = encoder }
    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, CMSampleBufferIsValid(sampleBuffer) else { return }
        encoder.encode(sampleBuffer)
    }
}

func shareableContent() async throws -> SCShareableContent {
    do { return try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true) }
    catch { throw CLIError.message("无法读取屏幕内容。请在“系统设置 → 隐私与安全性 → 屏幕与系统音频录制”中授权本工具后重试。\n\(error)") }
}

func printList(_ content: SCShareableContent) {
    print("Displays:")
    for (index, display) in content.displays.enumerated() { print("  [\(index)] id=\(display.displayID) pixels=\(Int(display.width))x\(Int(display.height))") }
    print("Windows:")
    for window in content.windows where window.isOnScreen && window.frame.width > 1 && window.frame.height > 1 {
        let app = window.owningApplication?.applicationName ?? "?"
        let title = window.title ?? ""
        print("  id=\(window.windowID) app=\"\(app)\" title=\"\(title)\" size=\(Int(window.frame.width))x\(Int(window.frame.height))")
    }
}

func makeTarget(_ options: Options, content: SCShareableContent) throws -> Target {
    if let index = options.displayIndex {
        guard content.displays.indices.contains(index) else { throw CLIError.invalid("不存在显示器索引：\(index)") }
        let display = content.displays[index]
        let width = options.width ?? Int(display.width), height = options.height ?? Int(display.height)
        return Target(filter: SCContentFilter(display: display, excludingWindows: []), width: width & ~1, height: height & ~1, label: "display \(index)")
    }
    if let id = options.windowID, let window = content.windows.first(where: { $0.windowID == id }) {
        let width = options.width ?? max(2, Int(window.frame.width)), height = options.height ?? max(2, Int(window.frame.height))
        return Target(filter: SCContentFilter(desktopIndependentWindow: window), width: width & ~1, height: height & ~1, label: "window \(id)")
    }
    throw CLIError.invalid("找不到目标窗口；请先运行 list，确认窗口 ID 仍有效")
}

func record(_ options: Options, content: SCShareableContent) async throws {
    let target = try makeTarget(options, content: content)
    let output: FileHandle
    if options.stdout {
        output = FileHandle.standardOutput
    } else {
        let url = URL(fileURLWithPath: options.output!)
        FileManager.default.createFile(atPath: url.path, contents: nil)
        output = try FileHandle(forWritingTo: url)
    }
    let bitrate = options.bitrate ?? max(1_000_000, target.width * target.height * options.fps / 8)
    let encoder = try H264Encoder(width: target.width, height: target.height, fps: options.fps, bitrate: bitrate, output: output)
    let delegate = CaptureDelegate(encoder: encoder)
    let config = SCStreamConfiguration()
    config.width = target.width; config.height = target.height
    config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(options.fps))
    config.queueDepth = 3; config.pixelFormat = kCVPixelFormatType_32BGRA; config.showsCursor = options.showCursor
    if #available(macOS 13.0, *) { config.capturesAudio = false }
    let stream = SCStream(filter: target.filter, configuration: config, delegate: nil)
    try stream.addStreamOutput(delegate, type: .screen, sampleHandlerQueue: DispatchQueue(label: "capture-cli.encode"))
    fputs("recording \(target.label) \(target.width)x\(target.height) @ \(options.fps) fps; Ctrl-C to stop\n", stderr)
    try await stream.startCapture()
    if let duration = options.duration {
        try await Task.sleep(nanoseconds: UInt64(max(0, duration) * 1_000_000_000))
    } else {
        let signalSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        signal(SIGINT, SIG_IGN)
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            signalSource.setEventHandler { continuation.resume() }
            signalSource.resume()
        }
        signalSource.cancel()
    }
    try? await stream.stopCapture(); encoder.finish()
    if let error = encoder.error { throw error }
}

@main struct CaptureCLI {
    static func main() async {
        do {
            let options = try Options.parse(Array(CommandLine.arguments.dropFirst()))
            if #available(macOS 12.3, *) {
                let content = try await shareableContent()
                switch options.command { case .list: printList(content); case .record: try await record(options, content: content) }
            } else { throw CLIError.message("需要 macOS 12.3 或更高版本") }
        } catch { fputs("错误：\(error)\n", stderr); exit(2) }
    }
}
