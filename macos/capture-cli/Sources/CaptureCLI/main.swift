import Foundation
import ScreenCaptureKit
import VideoToolbox
import CoreMedia
import CoreVideo
import CoreGraphics
import AVFoundation
import Darwin

struct Options {
    enum Command { case list, record }
    var command: Command
    var displayIndex: Int?
    var windowID: CGWindowID?
    var output: String?
    var stdout = false
    var tcpHost: String?
    var tcpPort: UInt16?
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
            case "--tcp":
                let target = try value()
                let parts = target.split(separator: ":")
                guard parts.count == 2, let port = UInt16(parts[1]), port > 0 else {
                    throw CLIError.invalid("--tcp 需要 host:port")
                }
                o.tcpHost = String(parts[0])
                o.tcpPort = port
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
        if command == .record && !o.stdout && o.output == nil && o.tcpHost == nil {
            throw CLIError.invalid("record 需要 --output PATH、--stdout 或 --tcp host:port")
        }
        let destinations = [o.stdout, o.output != nil, o.tcpHost != nil].filter { $0 }.count
        if destinations > 1 { throw CLIError.invalid("--output、--stdout 和 --tcp 只能选一个") }
        return o
    }
}

enum CLIError: Error, CustomStringConvertible {
    case usage, invalid(String), message(String)
    var description: String {
        switch self {
        case .usage: return "用法：\n  capture-cli list\n  capture-cli record (--display INDEX | --window-id ID) (--output PATH | --stdout | --tcp host:port) [选项]\n\n选项：\n  --fps N                 帧率，默认 30\n  --bitrate BPS          平均码率，默认按分辨率估算\n  --width N --height N   输出尺寸（必须为偶数）\n  --duration SECONDS     录制时长\n  --no-cursor            不捕获鼠标指针\n\n输出格式：\n  --output *.mp4         MP4 视频文件\n  --output *.h264/*.264  Annex-B H.264 裸流文件（包含 SPS/PPS）\n  --stdout               Annex-B H.264 裸流，二进制数据写 stdout，日志写 stderr\n  --tcp host:port        发给 ESP32 的 AirPlay 发送端（长度前缀的访问单元协议）"
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

/* Sends access units to the ESP32 AirPlay sender over TCP. The wire format is
 * the one firmware/components/media/src/media_net.c reads: a configure packet
 * carrying the geometry and both parameter sets, then one length-prefixed
 * access unit per frame, still in AVCC framing.
 *
 * Access units keep the boundaries VideoToolbox gives us, so the receiver never
 * has to split an H.264 byte stream. The parameter sets and other non-VCL NAL
 * units are dropped from the frames: they reach the receiver through the codec
 * record, and Apple's senders do the same. */
final class NetworkSink {
    /* Mirrors NET_MAX_AU in firmware/components/media/src/media_net.c: the device
     * drops anything larger, so skip it here rather than burn the link on it. */
    static let maxAccessUnit = 512 * 1024

    private let fd: Int32
    private var configured = false
    private let width: Int
    private let height: Int
    private let fps: Int
    private var framesSent = 0
    private var bytesSent = 0
    private var lastReport = Date()
    private var lastProblem: String?

    init(host: String, port: UInt16, width: Int, height: Int, fps: Int) throws {
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        var found: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, String(port), &hints, &found) == 0, let first = found else {
            throw CLIError.message("无法解析 \(host):\(port)")
        }
        defer { freeaddrinfo(found) }

        var sock: Int32 = -1
        var candidate: UnsafeMutablePointer<addrinfo>? = first
        while let node = candidate {
            let fd = socket(node.pointee.ai_family, node.pointee.ai_socktype, node.pointee.ai_protocol)
            if fd >= 0 && connect(fd, node.pointee.ai_addr, node.pointee.ai_addrlen) == 0 {
                sock = fd
                break
            }
            if fd >= 0 { close(fd) }
            candidate = node.pointee.ai_next
        }
        guard sock >= 0 else { throw CLIError.message("无法连接 \(host):\(port)") }
        fd = sock
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        self.width = width
        self.height = height
        self.fps = fps
        fputs("streaming to \(host):\(port)\n", stderr)
    }

    deinit { close(fd) }

    func send(_ sampleBuffer: CMSampleBuffer, format: CMFormatDescription) -> Bool {
        if !configured {
            guard let sets = parameterSets(format) else {
                report("waiting for parameter sets from the encoder")
                return true
            }
            guard writeAll(configurePacket(sps: sets.sps, pps: sets.pps)) else {
                return false
            }
            configured = true
            report("sent configure packet, sps=\(sets.sps.count) pps=\(sets.pps.count) bytes")
        }
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return true }
        var length = 0
        var pointer: UnsafeMutablePointer<Int8>?
        guard CMBlockBufferGetDataPointer(dataBuffer, atOffset: 0, lengthAtOffsetOut: nil,
                                          totalLengthOut: &length, dataPointerOut: &pointer) == noErr,
              let base = pointer else { return true }

        var accessUnit = Data()
        var idr = false
        var offset = 0
        while offset + 4 <= length {
            let size = Int(UInt32(bigEndian: base.advanced(by: offset).withMemoryRebound(to: UInt32.self, capacity: 1) { $0.pointee }))
            let start = offset + 4
            guard size > 0, start + size <= length else { break }
            let type = Int(UInt8(bitPattern: base[start])) & 0x1f
            if type >= 1 && type <= 5 {
                if type == 5 { idr = true }
                accessUnit.append(Data([UInt8(size >> 24), UInt8((size >> 16) & 0xff), UInt8((size >> 8) & 0xff), UInt8(size & 0xff)]))
                accessUnit.append(Data(bytes: base.advanced(by: start), count: size))
            }
            offset = start + size
        }
        guard !accessUnit.isEmpty else { return true }
        guard accessUnit.count <= NetworkSink.maxAccessUnit else {
            report("frame of \(accessUnit.count) bytes exceeds the \(NetworkSink.maxAccessUnit)-byte device limit")
            return true
        }

        let pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer))
        var message = Data([0x41, 0x50, 0x56, 0x4e, 2, 1, idr ? 1 : 0, 0])
        appendBigEndian(&message, UInt32(accessUnit.count))
        appendBigEndian(&message, UInt32(max(0, pts) * 1000))
        message.append(accessUnit)
        let sent = writeAll(message)
        if sent {
            framesSent += 1
            bytesSent += message.count
        }
        report(nil)
        return sent
    }

    /* One line a second: enough to tell an idle capture from a stalled socket
     * without flooding the terminal. */
    private func report(_ event: String?) {
        if let event, event != lastProblem {
            lastProblem = event
            fputs("[\(event)]\n", stderr)
        }
        let now = Date()
        guard now.timeIntervalSince(lastReport) >= 1 else { return }
        lastReport = now
        if framesSent > 0 || event == nil {
            fputs(String(format: "sent %d frame(s), %d byte(s)\n", framesSent, bytesSent), stderr)
            framesSent = 0
            bytesSent = 0
        }
    }

    private func configurePacket(sps: Data, pps: Data) -> Data {
        var message = Data([0x41, 0x50, 0x56, 0x4e, 1, 1, 0, 0])
        appendBigEndian(&message, UInt16(width))
        appendBigEndian(&message, UInt16(height))
        appendBigEndian(&message, UInt16(fps))
        appendBigEndian(&message, UInt16(sps.count))
        appendBigEndian(&message, UInt16(pps.count))
        message.append(sps)
        message.append(pps)
        return message
    }

    private func parameterSets(_ format: CMFormatDescription) -> (sps: Data, pps: Data)? {
        var sets: [Data] = []
        for index in 0..<2 {
            var parameter: UnsafePointer<UInt8>?
            var size = 0
            var count = 0
            guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, parameterSetIndex: index, parameterSetPointerOut: &parameter,
                parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil) == noErr,
                let parameter else { return nil }
            sets.append(Data(bytes: parameter, count: size))
        }
        guard sets.count == 2 else { return nil }
        return (sets[0], sets[1])
    }

    private func writeAll(_ data: Data) -> Bool {
        var sent = 0
        let ok = data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Bool in
            guard let base = raw.baseAddress else { return true }
            while sent < raw.count {
                let written = Darwin.send(fd, base.advanced(by: sent), raw.count - sent, 0)
                if written <= 0 { return false }
                sent += written
            }
            return true
        }
        if !ok { fputs("到 ESP32 的连接已断开\n", stderr) }
        return ok
    }

    private func appendBigEndian(_ data: inout Data, _ value: UInt16) {
        data.append(UInt8((value >> 8) & 0xff))
        data.append(UInt8(value & 0xff))
    }

    private func appendBigEndian(_ data: inout Data, _ value: UInt32) {
        data.append(UInt8((value >> 24) & 0xff))
        data.append(UInt8((value >> 16) & 0xff))
        data.append(UInt8((value >> 8) & 0xff))
        data.append(UInt8(value & 0xff))
    }
}

final class H264Encoder {
    private var session: VTCompressionSession?
    private let output: FileHandle?
    private let mp4URL: URL?
    private var writer: AVAssetWriter?
    private var writerInput: AVAssetWriterInput?
    private let lock = NSLock()
    private var failed: Error?
    private var frameCount = 0
    private let width: Int
    private let height: Int
    private let network: NetworkSink?

    init(width: Int, height: Int, fps: Int, bitrate: Int, output: FileHandle?, mp4URL: URL? = nil,
         network: NetworkSink? = nil) throws {
        let sinks = [output != nil, mp4URL != nil, network != nil].filter { $0 }.count
        guard sinks == 1 else { throw CLIError.message("必须选择一种输出格式") }
        self.output = output
        self.mp4URL = mp4URL
        self.network = network
        self.width = width; self.height = height
        var s: VTCompressionSession?
        let status = VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264, encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: H264Encoder.callback, refcon: Unmanaged.passUnretained(self).toOpaque(), compressionSessionOut: &s)
        guard status == noErr, let s else { throw CLIError.message("无法创建 H.264 编码器：\(status)") }
        session = s
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        /* Baseline keeps CABAC out of the stream. The cheap decoders behind TV
         * boxes and projectors usually implement Baseline only, and a Main
         * profile stream leaves them showing nothing at all. */
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel,
                             value: kVTProfileLevel_H264_Baseline_AutoLevel)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: bitrate as CFTypeRef)
        /* AverageBitRate alone lets a keyframe spike far above it. A screen full
         * of text (a terminal running top, say) then produces keyframes several
         * times the average, and a single keyframe takes longer to encrypt and
         * send than a whole frame interval. Capping bytes per second keeps
         * keyframes in the same league as the average. */
        let limits: [CFTypeRef] = [NSNumber(value: bitrate / 8), NSNumber(value: 1.0)]
        let limitStatus = VTSessionSetProperty(s, key: kVTCompressionPropertyKey_DataRateLimits,
                                               value: limits as CFTypeRef)
        if limitStatus != noErr {
            fputs("编码器不接受码率上限（\(limitStatus)），关键帧可能远超平均值\n", stderr)
        }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: fps * 5 as CFTypeRef)
        /* Keyframes are the expensive ones: 1224x846 of text costs a couple of
         * hundred kilobytes, which takes several frame intervals to send. A
         * one-second period turns that into visible stutter every second, so
         * only resynchronise every few seconds. */
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                             value: 5.0 as CFTypeRef)
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
        if let writer, let writerInput {
            writerInput.markAsFinished()
            let semaphore = DispatchSemaphore(value: 0)
            writer.finishWriting { semaphore.signal() }
            semaphore.wait()
            if writer.status != .completed { setFailure(CLIError.message("MP4 写入失败：\(writer.error?.localizedDescription ?? "未知错误")")) }
        }
        try? output?.close()
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
        guard let format = CMSampleBufferGetFormatDescription(sampleBuffer) else { return }
        if let network {
            if !network.send(sampleBuffer, format: format) {
                setFailure(CLIError.message("到 ESP32 的连接已断开"))
            }
            frameCount += 1
            return
        }
        if let mp4URL {
            writeMP4(sampleBuffer, format: format, url: mp4URL)
            return
        }
        guard let output, let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
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
            output?.write(Data([0, 0, 0, 1])); output?.write(Data(bytes: parameter, count: size))
        }
    }

    private func writeMP4(_ sampleBuffer: CMSampleBuffer, format: CMFormatDescription, url: URL) {
        if writer == nil {
            do {
                try? FileManager.default.removeItem(at: url)
                let assetWriter = try AVAssetWriter(outputURL: url, fileType: .mp4)
                let input = AVAssetWriterInput(mediaType: .video, outputSettings: nil, sourceFormatHint: format)
                input.expectsMediaDataInRealTime = true
                guard assetWriter.canAdd(input) else { throw CLIError.message("MP4 不支持 H.264 视频输入") }
                assetWriter.add(input)
                guard assetWriter.startWriting() else { throw CLIError.message("无法创建 MP4：\(assetWriter.error?.localizedDescription ?? "未知错误")") }
                assetWriter.startSession(atSourceTime: CMSampleBufferGetPresentationTimeStamp(sampleBuffer))
                writer = assetWriter
                writerInput = input
            } catch { setFailure(error); return }
        }
        guard let writerInput else { return }
        if writerInput.isReadyForMoreMediaData {
            guard writerInput.append(sampleBuffer) else { setFailure(CLIError.message("MP4 视频帧写入失败：\(writer?.error?.localizedDescription ?? "未知错误")")); return }
            frameCount += 1
        }
    }
}

final class CaptureDelegate: NSObject, SCStreamOutput {
    let encoder: H264Encoder
    private var frames = 0
    private var lastReport = Date()
    init(encoder: H264Encoder) { self.encoder = encoder }
    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, CMSampleBufferIsValid(sampleBuffer) else { return }
        frames += 1
        let now = Date()
        if now.timeIntervalSince(lastReport) >= 2 {
            lastReport = now
            fputs("captured \(frames) sample(s)\n", stderr)
            frames = 0
        }
        encoder.encode(sampleBuffer)
    }
}

/* ScreenCaptureKit reports a failed stream here; without a delegate the failure
 * is silent and the capture just never delivers a frame. */
final class StreamDelegate: NSObject, SCStreamDelegate {
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        fputs("捕获停止：\(error)\n", stderr)
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
    /* Window filters go through the drawing-system initialisation that an
     * application normally performs at launch; without it ScreenCaptureKit
     * asserts (CGS_REQUIRE_INIT) as soon as the filter is built. */
    if options.windowID != nil {
        _ = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID)
    }
    let target = try makeTarget(options, content: content)
    let outputURL = options.output.map { URL(fileURLWithPath: $0) }
    let isMP4 = outputURL?.pathExtension.lowercased() == "mp4"
    var output: FileHandle?
    if options.stdout {
        output = FileHandle.standardOutput
    } else if !isMP4, let outputURL {
        FileManager.default.createFile(atPath: outputURL.path, contents: nil)
        output = try FileHandle(forWritingTo: outputURL)
    }
    let bitrate = options.bitrate ?? max(1_000_000, target.width * target.height * options.fps / 8)
    let network = try options.tcpHost.map {
        try NetworkSink(host: $0, port: options.tcpPort!, width: target.width, height: target.height,
                        fps: options.fps)
    }
    let encoder = try H264Encoder(width: target.width, height: target.height, fps: options.fps,
                                  bitrate: bitrate, output: output,
                                  mp4URL: isMP4 ? outputURL : nil, network: network)
    let delegate = CaptureDelegate(encoder: encoder)
    let config = SCStreamConfiguration()
    config.width = target.width; config.height = target.height
    config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(options.fps))
    config.queueDepth = 3; config.pixelFormat = kCVPixelFormatType_32BGRA; config.showsCursor = options.showCursor
    if #available(macOS 13.0, *) { config.capturesAudio = false }
    let stream = SCStream(filter: target.filter, configuration: config, delegate: StreamDelegate())
    try stream.addStreamOutput(delegate, type: .screen, sampleHandlerQueue: DispatchQueue(label: "capture-cli.encode"))
    fputs("recording \(target.label) \(target.width)x\(target.height) @ \(options.fps) fps; Ctrl-C to stop\n", stderr)
    try await stream.startCapture()
    fputs("capture started\n", stderr)
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
