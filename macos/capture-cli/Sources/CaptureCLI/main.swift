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
    var audio = false
    var audioOutput: String?
    /* The receiver applies its own gain and there is no volume control in the
     * AirPlay audio path, so trimming happens here, before the samples are cut
     * into frames. */
    var audioGain = 1.0

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
            case "--audio": o.audio = true
            case "--audio-output": o.audioOutput = try value()
            case "--audio-gain":
                guard let gain = Double(try value()), gain > 0, gain <= 4 else {
                    throw CLIError.invalid("--audio-gain 必须在 0 到 4 之间")
                }
                o.audioGain = gain
            case "--help", "-h": throw CLIError.usage
            default: throw CLIError.invalid("未知参数：\(arg)")
            }
            i += 1
        }
        guard o.fps > 0, o.fps <= 120 else { throw CLIError.invalid("fps 必须在 1 到 120 之间") }
        guard o.displayIndex == nil || o.windowID == nil else { throw CLIError.invalid("--display 和 --window-id 不能同时使用") }
        guard o.width == nil || (o.width! >= 2 && o.width! % 2 == 0) else { throw CLIError.invalid("width 必须是正偶数") }
        guard o.height == nil || (o.height! >= 2 && o.height! % 2 == 0) else { throw CLIError.invalid("height 必须是正偶数") }
        if o.audioOutput != nil && !o.audio { throw CLIError.invalid("--audio-output 需要配合 --audio") }
        if o.audioOutput != nil, let out = o.output, URL(fileURLWithPath: out).pathExtension.lowercased() == "mp4" {
            throw CLIError.invalid("MP4 会直接混入音频轨，--audio-output 只用于裸流、--stdout 或 --tcp")
        }
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
        case .usage: return "用法：\n  capture-cli list\n  capture-cli record (--display INDEX | --window-id ID) (--output PATH | --stdout | --tcp host:port) [选项]\n\n选项：\n  --fps N                 帧率，默认 30\n  --bitrate BPS          平均码率，默认按分辨率估算\n  --width N --height N   输出尺寸（必须为偶数）\n  --duration SECONDS     录制时长\n  --no-cursor            不捕获鼠标指针\n  --audio                 同时采集系统音频（默认关闭，需 macOS 13+）\n  --audio-gain FACTOR     音频增益，默认 1.0（接收端音量不可控时用它压一点）\n  --audio-output PATH     音频另外写成 WAV 文件（裸流/--stdout 需要；--tcp 时可选；MP4 直接混流）\n\n输出格式：\n  --output *.mp4         MP4 视频文件\n  --output *.h264/*.264  Annex-B H.264 裸流文件（包含 SPS/PPS）\n  --stdout               Annex-B H.264 裸流，二进制数据写 stdout，日志写 stderr\n  --tcp host:port        发给 ESP32 的 AirPlay 发送端（长度前缀的访问单元协议；--audio 时音频走 port+1）"
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
    /* Audio rides its own connection, on the device's port + 1: a keyframe
     * sharing the video socket stalls the sound for hundreds of milliseconds,
     * which is audible. -1 means the device did not take that connection and
     * the video path carries on alone. */
    private let audioFD: Int32
    private var configured = false
    private let width: Int
    private let height: Int
    private let fps: Int
    private var framesSent = 0
    private var bytesSent = 0
    private var lastReport = Date()
    private var lastProblem: String?
    /* An access unit only decodes once the receiver has seen an IDR, and
     * VideoToolbox only emits one when its keyframe interval expires on a
     * timeline that a still screen barely advances. Tracking the IDs that do
     * arrive lets the encoder step in when that gap grows too wide. */
    private(set) var lastIDR = Date.distantPast

    init(host: String, port: UInt16, width: Int, height: Int, fps: Int) throws {
        let sock = NetworkSink.connectSocket(host: host, port: port)
        guard sock >= 0 else { throw CLIError.message("无法连接 \(host):\(port)") }
        fd = sock
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        self.width = width
        self.height = height
        self.fps = fps
        fputs("streaming to \(host):\(port)\n", stderr)

        /* Audio is optional. A device running an older build, or one whose
         * audio task is busy, must not take the whole stream down with it. */
        let audioPort = Int(port) + 1
        let audio = audioPort <= Int(UInt16.max) ? NetworkSink.connectSocket(host: host, port: UInt16(audioPort)) : -1
        audioFD = audio
        if audio >= 0 {
            setsockopt(audio, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
            /* One ~1.4 KB frame every 8 ms: delaying a frame until the previous
             * one is acknowledged reaches the device as a gap in the sound. */
            setsockopt(audio, IPPROTO_TCP, TCP_NODELAY, &on, socklen_t(MemoryLayout<Int32>.size))
            /* Keep about a fifth of a second in the kernel. The socket buffer is
             * the one queue we cannot drop from, so audio that piles up there
             * plays late instead of being skipped; the slack belongs in the
             * sender's own queue, which is bounded and counted. */
            var sendBuffer: Int32 = 32 * 1024
            setsockopt(audio, SOL_SOCKET, SO_SNDBUF, &sendBuffer, socklen_t(MemoryLayout<Int32>.size))
            fputs("audio connected to \(host):\(audioPort)\n", stderr)
        } else {
            fputs("音频连接 \(host):\(audioPort) 失败，本次只发送视频\n", stderr)
        }
    }

    deinit {
        close(fd)
        if audioFD >= 0 { close(audioFD) }
    }

    var audioConnected: Bool { audioFD >= 0 }

    /* Resolves and connects, walking the whole address list so an IPv4-only or
     * IPv6-only answer still works. Returns -1 when nothing answered. */
    private static func connectSocket(host: String, port: UInt16) -> Int32 {
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        var found: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, String(port), &hints, &found) == 0, let first = found else {
            return -1
        }
        defer { freeaddrinfo(found) }

        var candidate: UnsafeMutablePointer<addrinfo>? = first
        while let node = candidate {
            let sock = socket(node.pointee.ai_family, node.pointee.ai_socktype, node.pointee.ai_protocol)
            if sock >= 0 {
                if connect(sock, node.pointee.ai_addr, node.pointee.ai_addrlen) == 0 { return sock }
                close(sock)
            }
            candidate = node.pointee.ai_next
        }
        return -1
    }

    func send(_ sampleBuffer: CMSampleBuffer, format: CMFormatDescription) -> Bool {
        if !configured {
            guard let sets = parameterSets(format) else {
                report("waiting for parameter sets from the encoder")
                return true
            }
            guard writeVideo(configurePacket(sps: sets.sps, pps: sets.pps)) else {
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
        let sent = writeVideo(message)
        if sent {
            if idr { lastIDR = Date() }
            framesSent += 1
            bytesSent += message.count
        }
        report(nil)
        return sent
    }

    /* True when the stream has gone long enough without an IDR that a receiver
     * joining now would sit on an undecodable frame. */
    var needsKeyFrame: Bool {
        Date().timeIntervalSince(lastIDR) > 2.5
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

    /* One ALAC frame as it goes on the wire: the shared header, type 3, then the
     * frame. The device reads only frame_len off an audio header and takes its
     * timing from the samples, so the timestamp is the audio timeline. */
    func makeAudioPacket(_ frame: [UInt8], ptsMs: Int) -> Data {
        var message = Data([0x41, 0x50, 0x56, 0x4e, 3, 1, 0, 0])
        appendBigEndian(&message, UInt32(frame.count))
        appendBigEndian(&message, UInt32(max(0, ptsMs)))
        message.append(contentsOf: frame)
        return message
    }

    /* Writes whatever the caller hands over. A device whose link is full blocks
     * here, which is why only the writer thread calls it. */
    func writeAudio(_ data: Data) -> Bool {
        audioFD >= 0 && writeAll(data, to: audioFD)
    }

    /* One byte comes back on the audio connection when the device starts its
     * audio channel: it empties the ring at that moment, so the lead this stream
     * had built up is gone and has to be built again. */
    enum AudioArm { case arm, none, closed }

    /* Non-blocking for `timeout` 0. Only the pacer thread calls this, and it is
     * the only reader on the connection. */
    func pollAudioArm(timeout: TimeInterval) -> AudioArm {
        guard audioFD >= 0 else { return .closed }
        var descriptor = pollfd(fd: audioFD, events: Int16(POLLIN), revents: 0)
        let ready = Darwin.poll(&descriptor, 1, Int32((timeout * 1000).rounded()))
        if ready < 0 { return errno == EINTR ? .none : .closed }
        if ready == 0 { return .none }
        var byte: UInt8 = 0
        let got = Darwin.recv(audioFD, &byte, 1, 0)
        if got == 1 { return .arm }
        return got == 0 ? .closed : .none
    }

    private func writeVideo(_ data: Data) -> Bool {
        guard writeAll(data, to: fd) else {
            fputs("到 ESP32 的连接已断开\n", stderr)
            return false
        }
        return true
    }

    private func writeAll(_ data: Data, to socket: Int32) -> Bool {
        var sent = 0
        return data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Bool in
            guard let base = raw.baseAddress else { return true }
            while sent < raw.count {
                let written = Darwin.send(socket, base.advanced(by: sent), raw.count - sent, 0)
                if written <= 0 { return false }
                sent += written
            }
            return true
        }
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

/* MP4 muxing shared by the video and audio paths. AVAssetWriter inputs must be
 * registered before writing starts, but each input's format is only known with
 * its first sample buffer. The first encoded video frame often beats the first
 * audio buffer out of the encoder pipeline, so when audio is expected the early
 * video frames are held until the audio format arrives, and the session starts
 * at whichever first timestamp is earlier. */
final class MP4Muxer {
    private let url: URL
    private let audioExpected: Bool
    private let lock = NSLock()
    private var writer: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var audioInput: AVAssetWriterInput?
    private var audioFormat: CMFormatDescription?
    private var pendingAudio: [CMSampleBuffer] = []
    private var pendingVideo: [CMSampleBuffer] = []
    private var finished = false
    private var failed: Error?

    init(url: URL, audioExpected: Bool = false) { self.url = url; self.audioExpected = audioExpected }

    var error: Error? { lock.lock(); defer { lock.unlock() }; return failed }

    func appendVideo(_ sampleBuffer: CMSampleBuffer, format: CMFormatDescription) {
        lock.lock(); defer { lock.unlock() }
        guard !finished else { return }
        if writer == nil {
            /* The audio input can only be registered before startWriting, so
             * hold frames until the audio format is known; give up waiting
             * after ~5s so a silent stream still produces a video-only file. */
            if audioExpected, audioFormat == nil, pendingVideo.count < 100 {
                pendingVideo.append(sampleBuffer)
                return
            }
            do { try start(sampleBuffer: sampleBuffer, format: format) }
            catch { failed = failed ?? error; finished = true; return }
        }
        appendVideoLocked(sampleBuffer)
    }

    private func appendVideoLocked(_ sampleBuffer: CMSampleBuffer) {
        guard let videoInput else { return }
        if videoInput.isReadyForMoreMediaData, !videoInput.append(sampleBuffer) {
            failed = failed ?? CLIError.message("MP4 视频帧写入失败：\(writer?.error?.localizedDescription ?? "未知错误")")
        }
    }

    func appendAudio(_ sampleBuffer: CMSampleBuffer) {
        lock.lock(); defer { lock.unlock() }
        guard !finished else { return }
        if audioFormat == nil { audioFormat = CMSampleBufferGetFormatDescription(sampleBuffer) }
        guard audioInput != nil else {
            /* Only worth holding before writing starts; once the writer is up
             * without an audio input, the file stays video-only. */
            if writer == nil, pendingAudio.count < 1000 { pendingAudio.append(sampleBuffer) }
            return
        }
        appendAudioLocked(sampleBuffer)
    }

    func finish() {
        lock.lock(); defer { lock.unlock() }
        guard !finished else { return }
        finished = true
        guard let writer else { return }
        videoInput?.markAsFinished()
        audioInput?.markAsFinished()
        let semaphore = DispatchSemaphore(value: 0)
        writer.finishWriting { semaphore.signal() }
        semaphore.wait()
        if writer.status != .completed {
            failed = failed ?? CLIError.message("MP4 写入失败：\(writer.error?.localizedDescription ?? "未知错误")")
        }
    }

    private func appendAudioLocked(_ sampleBuffer: CMSampleBuffer) {
        guard let audioInput, audioInput.isReadyForMoreMediaData, !audioInput.append(sampleBuffer) else { return }
        failed = failed ?? CLIError.message("MP4 音频帧写入失败：\(writer?.error?.localizedDescription ?? "未知错误")")
    }

    private func start(sampleBuffer: CMSampleBuffer, format: CMFormatDescription) throws {
        try? FileManager.default.removeItem(at: url)
        let assetWriter = try AVAssetWriter(outputURL: url, fileType: .mp4)
        let video = AVAssetWriterInput(mediaType: .video, outputSettings: nil, sourceFormatHint: format)
        video.expectsMediaDataInRealTime = true
        guard assetWriter.canAdd(video) else { throw CLIError.message("MP4 不支持 H.264 视频输入") }
        assetWriter.add(video)
        if let audioFormat {
            /* MP4 has no place for raw PCM, so the audio track is transcoded to
             * AAC; sample rate and channel count follow the stream. */
            let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(audioFormat)?.pointee
            var settings: [String: Any] = [AVFormatIDKey: kAudioFormatMPEG4AAC]
            if let asbd {
                settings[AVSampleRateKey] = asbd.mSampleRate
                settings[AVNumberOfChannelsKey] = Int(asbd.mChannelsPerFrame)
            }
            let audio = AVAssetWriterInput(mediaType: .audio, outputSettings: settings, sourceFormatHint: audioFormat)
            audio.expectsMediaDataInRealTime = true
            if assetWriter.canAdd(audio) {
                assetWriter.add(audio)
                audioInput = audio
            } else {
                fputs("MP4 不支持音频输入，本文件只包含视频\n", stderr)
            }
        }
        guard assetWriter.startWriting() else { throw CLIError.message("无法创建 MP4：\(assetWriter.error?.localizedDescription ?? "未知错误")") }
        /* Early frames and audio of both kinds may be held back; the session
         * must start no later than the earliest appended sample. */
        var sessionStart = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let candidates = pendingVideo.first.map { CMSampleBufferGetPresentationTimeStamp($0) } ?? .invalid
        let audioPTS = pendingAudio.first.map { CMSampleBufferGetPresentationTimeStamp($0) } ?? .invalid
        for candidate in [candidates, audioPTS] where candidate.isValid {
            if CMTimeCompare(candidate, sessionStart) < 0 { sessionStart = candidate }
        }
        assetWriter.startSession(atSourceTime: sessionStart)
        writer = assetWriter
        videoInput = video
        for sample in pendingVideo { appendVideoLocked(sample) }
        pendingVideo.removeAll()
        for sample in pendingAudio { appendAudioLocked(sample) }
        pendingAudio.removeAll()
    }
}

/* Minimal RIFF/WAVE writer for the 32-bit float PCM ScreenCaptureKit delivers,
 * accepting both interleaved and non-interleaved buffers. ffmpeg/ffplay read
 * IEEE-float WAV natively, which makes it the lowest-friction side format for
 * the raw-stream outputs that cannot carry audio in-band. */
final class WAVWriter {
    private let handle: FileHandle
    private let channels: Int
    private let nonInterleaved: Bool
    private var dataBytes = 0
    private var scratch = Data()
    private var closed = false

    init?(format: CMFormatDescription, path: String) {
        guard let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format)?.pointee,
              asbd.mFormatID == kAudioFormatLinearPCM,
              asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0,
              asbd.mBitsPerChannel == 32,
              asbd.mChannelsPerFrame > 0,
              asbd.mSampleRate > 0 else { return nil }
        channels = Int(asbd.mChannelsPerFrame)
        nonInterleaved = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        FileManager.default.createFile(atPath: path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: URL(fileURLWithPath: path)) else { return nil }
        self.handle = handle
        let sampleRate = UInt32(asbd.mSampleRate)
        let blockAlign = UInt32(channels) * 4
        var header = Data()
        func le16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { header.append(contentsOf: $0) } }
        func le32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { header.append(contentsOf: $0) } }
        header.append(contentsOf: "RIFF".utf8); le32(0); header.append(contentsOf: "WAVE".utf8)
        header.append(contentsOf: "fmt ".utf8); le32(16); le16(3) /* IEEE float */
        le16(UInt16(channels)); le32(sampleRate); le32(sampleRate * blockAlign); le16(UInt16(blockAlign)); le16(32)
        header.append(contentsOf: "data".utf8); le32(0)
        handle.write(header)
    }

    func append(_ sampleBuffer: CMSampleBuffer) {
        guard !closed, let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer),
              CMSampleBufferDataIsReady(sampleBuffer) else { return }
        var length = 0
        var pointer: UnsafeMutablePointer<Int8>?
        guard CMBlockBufferGetDataPointer(dataBuffer, atOffset: 0, lengthAtOffsetOut: nil,
                                          totalLengthOut: &length, dataPointerOut: &pointer) == noErr,
              let pointer, length > 0 else { return }
        let frames = CMSampleBufferGetNumSamples(sampleBuffer)
        guard length == frames * channels * 4 else { return }
        scratch.removeAll(keepingCapacity: true)
        if nonInterleaved {
            /* The block buffer stores channel planes back to back; WAV wants
             * interleaved samples. */
            for frame in 0..<frames {
                for channel in 0..<channels {
                    scratch.append(Data(bytes: pointer.advanced(by: (channel * frames + frame) * 4), count: 4))
                }
            }
        } else {
            scratch.append(Data(bytes: pointer, count: length))
        }
        dataBytes += scratch.count
        handle.write(scratch)
    }

    func finish() {
        guard !closed else { return }
        closed = true
        /* Patch the two little-endian size fields left as zero in the header. */
        let cap = UInt32(max(0, min(dataBytes, Int(UInt32.max))))
        for (offset, value) in [(4, cap &+ 36), (40, cap)] {
            var little = value.littleEndian
            try? handle.seek(toOffset: UInt64(offset))
            handle.write(Data(bytes: &little, count: 4))
        }
        try? handle.close()
    }
}

/* Converts the system audio ScreenCaptureKit hands over into ALAC frames and
 * releases them at the rate the device drains its ring.
 *
 * The device declares ALAC with spf=352 / sr=44100 / ss=16 / ch=2 in SETUP and
 * drops frames that do not match, so the format is fixed here and the captured
 * PCM is resampled into it.
 *
 * Four stages, so that nothing downstream can slow an upstream clock:
 *
 *   capture callback  copies the bytes into a queue and returns. Resampling
 *                     there holds ScreenCaptureKit's audio thread, and a held
 *                     audio thread does not deliver slower - it drops buffers.
 *   converter         resamples the queued buffers into 44100 Hz s16.
 *   pacer             cuts 352-sample ALAC frames out of that, one per 7.98 ms,
 *                     on an absolute schedule that runs a lead ahead of the
 *                     capture clock, and hands the packets over.
 *   writer            puts them on the socket. A link that cannot absorb one
 *                     packet every 8 ms blocks write(2) for hundreds of
 *                     milliseconds; doing that on the pacing thread would stop
 *                     the frames flowing on the audio timeline and turn a slow
 *                     link into a stream that lags and gaps.
 *
 * Every sample that leaves the converter either reaches the socket or is counted
 * as dropped, on one timeline, so the 10 s report adds up:
 * converted = sent + pcm queue + packet queue + dropped. */
final class ALACAudioSender {
    /* Must match APP_AUDIO_SPF / sr / ss / ch in the device's SETUP reply. */
    static let sampleRate = 44100
    static let channels = 2
    static let samplesPerFrame = 352
    /* 352 / 44100 s: one frame here, one packet on the wire. */
    static let frameInterval = Double(samplesPerFrame) / Double(sampleRate)

    private static let frameBytes = samplesPerFrame * channels * 2
    /* The 55-bit header plus the samples is 1415 bytes - not a multiple of the
     * 16-byte AES block. Every frame carries zero padding up to 1424 so the
     * encrypted payload covers every sample under either receiver decryption
     * habit. */
    private static let framePaddedBytes = (frameBytes + 15) / 16 * 16
    private static let bytesPerSecond = sampleRate * channels * 2
    /* How far ahead of the capture clock the schedule runs. The device's ring
     * holds 64 frames (511 ms): 200 ms rides out the drift between two clocks
     * without ever leaving it short of room. */
    private static let leadSeconds = 0.2
    /* In whole frames, so what leaves the queue lines up with the ring. */
    private static let leadFrames = Int((leadSeconds / frameInterval).rounded(.up))
    private static let leadBytes = leadFrames * frameBytes
    /* Raw capture PCM: 2 MiB is about 2.7 s of what ScreenCaptureKit delivers,
     * enough to ride out a stall in this process without unbounded growth. */
    private static let captureLimitBytes = 2 * 1024 * 1024
    /* Converted PCM waiting for the pacer, and packets waiting for the socket.
     * Half a second each: the pacer only has to cover capture chunkiness, and a
     * backlog past the device's 511 ms ring is stale audio nobody can play. */
    private static let pcmLimitBytes = bytesPerSecond / 2
    private static let outboundLimit = Int((0.5 / frameInterval).rounded())
    /* Packets per write(2). Coalescing cuts the syscall and segment count when
     * the link is the bottleneck; eight frames still leave the queue small
     * enough to drop from while a write is in flight. */
    private static let writeBatch = 8
    /* How long the pacer waits for the device's arm byte. A session normally
     * starts within seconds of the client; past this the device is assumed to be
     * an older build that never sends one, and pacing starts as it always did. */
    private static let armTimeout = 15.0
    /* The converter's output side, fixed by the device's SETUP. */
    private static let targetFormat = AVAudioFormat(commonFormat: .pcmFormatInt16,
                                                    sampleRate: Double(sampleRate),
                                                    channels: AVAudioChannelCount(channels),
                                                    interleaved: true)!

    /* One capture buffer exactly as it arrived. The capture callback copies the
     * bytes and stops there; this carries everything the converter thread needs
     * to rebuild the buffer they came out of. */
    private struct Chunk {
        let bytes: Data
        let frames: Int
        let format: AudioStreamBasicDescription
    }

    private let sink: NetworkSink
    private let condition = NSCondition()
    private let converterDone = DispatchSemaphore(value: 0)
    private let pacerDone = DispatchSemaphore(value: 0)
    private let writerDone = DispatchSemaphore(value: 0)
    private var threads: [Thread] = []
    private var reporter: DispatchSourceTimer?
    /* Raw capture PCM, oldest first. Guarded by `condition`, like everything
     * below it up to `sendBlocked`. */
    private var capture: [Chunk] = []
    private var capturedBytes = 0
    /* Converted 44100 Hz s16 samples the pacer cuts frames out of, and whole
     * packets the writer still has to put on the wire. */
    private var pcm = Data()
    private var outbound: [Data] = []
    private var converter: AVAudioConverter?
    private var converterFormat: AudioStreamBasicDescription?
    private var stopped = false
    private var live = true
    private var overflowing = false
    /* Whether the device has said its audio channel is live. Until it has, the
     * pacer holds back, so the queue only needs to keep the lead it will burst. */
    private var armed = false
    /* Counters, all on the 44100 Hz sample timeline: what capture delivered,
     * what the converter made of it, what reached the socket, and what was
     * thrown away where. */
    private var buffersTaken = 0
    private var samplesConverted = 0
    private var samplesDropped = 0
    private var framesSent = 0
    private var framesQueued = 0
    private var framesSentAtLastReport = 0
    private var sendBlocked = 0.0

    private let gain: Double

    init(sink: NetworkSink, gain: Double) {
        self.sink = sink
        self.gain = gain
        /* Every thread waits on its own queue, so they can all start at once:
         * the first capture buffer only wakes the converter. */
        for (name, loop) in [("capture-cli.alac.convert", convertLoop),
                             ("capture-cli.alac.pace", paceLoop),
                             ("capture-cli.alac.write", writeLoop)] {
            let thread = Thread { loop() }
            thread.name = name
            thread.start()
            threads.append(thread)
        }
        /* Reporting runs on a timer rather than off the audio threads: it has to
         * keep ticking when the pipeline is stalled, which is when it matters,
         * and it must never hold the lock while writing to stderr. */
        let reporter = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "capture-cli.alac.report"))
        reporter.schedule(deadline: .now() + 10, repeating: 10)
        reporter.setEventHandler { [weak self] in self?.report() }
        reporter.activate()
        self.reporter = reporter
    }

    /* Called on ScreenCaptureKit's audio queue, so it does the least it can:
     * copy the bytes out and get out of the way. Resampling here is what used to
     * make ScreenCaptureKit drop buffers and leave the receiver with silence.
     */
    func append(_ sampleBuffer: CMSampleBuffer) {
        guard let description = CMSampleBufferGetFormatDescription(sampleBuffer),
              let format = CMAudioFormatDescriptionGetStreamBasicDescription(description),
              let block = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
        let frames = CMSampleBufferGetNumSamples(sampleBuffer)
        let length = CMBlockBufferGetDataLength(block)
        guard frames > 0, length > 0 else { return }
        var bytes = Data(count: length)
        let status = bytes.withUnsafeMutableBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: base)
        }
        guard status == noErr else { return }

        condition.lock()
        var warning: String?
        if live, !stopped {
            buffersTaken += 1
            capture.append(Chunk(bytes: bytes, frames: frames, format: format.pointee))
            capturedBytes += length
            /* The newest audio is the one worth keeping, so the queue gives up
             * its oldest buffers. */
            while capturedBytes > Self.captureLimitBytes, capture.count > 1 {
                let dropped = capture.removeFirst()
                capturedBytes -= dropped.bytes.count
                samplesDropped += Self.samples(at: dropped.format, frames: dropped.frames)
            }
            if capturedBytes > Self.captureLimitBytes {
                warning = overflowWarning("音频队列已满，丢弃最旧的采集数据")
            }
        }
        /* Once the pipeline has shut down the buffer is simply not ours any
         * more; counting it would break the report, where every dropped sample
         * is one the converter produced. */
        condition.broadcast()
        condition.unlock()
        /* Never write to stderr while holding the lock: this runs on
         * ScreenCaptureKit's audio queue, and a blocked terminal would come
         * straight back in as dropped buffers. */
        if let warning { fputs(warning, stderr) }
    }

    /* Stops every thread and reports the totals. */
    func finish() {
        reporter?.cancel()
        condition.lock()
        stopped = true
        condition.broadcast()
        condition.unlock()
        /* Bounded: each thread only waits on its queue or sleeps for one frame
         * interval, and the stop flag releases both. The writer may still be
         * inside write(2) on a slow link, which is why the wait has a limit. */
        _ = converterDone.wait(timeout: .now() + 2)
        _ = pacerDone.wait(timeout: .now() + 2)
        _ = writerDone.wait(timeout: .now() + 2)
        threads = []
        condition.lock()
        let sent = framesSent
        let dropped = samplesDropped / Self.samplesPerFrame
        condition.unlock()
        fputs("audio: \(sent) frame(s) sent, \(dropped) frame(s) dropped\n", stderr)
    }

    /* Raw capture PCM into the device format, one buffer at a time. */
    private func convertLoop() {
        defer { converterDone.signal() }
        while true {
            condition.lock()
            while !stopped && live && capture.isEmpty { condition.wait() }
            let done = stopped || !live
            let chunk = capture.isEmpty ? nil : capture.removeFirst()
            if let chunk { capturedBytes -= chunk.bytes.count }
            condition.unlock()
            guard !done, let chunk else { break }

            let converted = convert(chunk)
            condition.lock()
            var warning: String?
            if let converted, !converted.isEmpty {
                pcm.append(converted)
                samplesConverted += converted.count / (Self.channels * 2)
                warning = trimLocked()
            } else {
                samplesDropped += Self.samples(at: chunk.format, frames: chunk.frames)
            }
            condition.broadcast()
            condition.unlock()
            if let warning { fputs(warning, stderr) }
        }
    }

    /* Puts queued packets on the socket, coalescing what has piled up into one
     * write. Blocking here - and only here - means a slow link delays packets
     * but never the schedule they were cut on. */
    private func writeLoop() {
        defer { writerDone.signal() }
        while true {
            condition.lock()
            while outbound.isEmpty && !stopped && live { condition.wait() }
            if outbound.isEmpty {
                condition.unlock()
                break
            }
            let batch = Array(outbound.prefix(Self.writeBatch))
            outbound.removeFirst(batch.count)
            condition.unlock()

            var message = Data()
            message.reserveCapacity(batch.reduce(0) { $0 + $1.count })
            for packet in batch { message.append(packet) }
            let started = Date()
            let written = sink.writeAudio(message)
            let spent = Date().timeIntervalSince(started)

            condition.lock()
            sendBlocked += spent
            if written {
                framesSent += batch.count
            } else {
                /* The socket is gone, so these packets will never play. */
                samplesDropped += batch.count * Self.samplesPerFrame
            }
            condition.unlock()
            guard written else {
                fputs("音频发送失败，已停止音频通道（视频继续）\n", stderr)
                shutDown()
                break
            }
        }
        /* Nothing left can be delivered; count it so the report still adds up. */
        condition.lock()
        if !outbound.isEmpty {
            samplesDropped += outbound.count * Self.samplesPerFrame
            outbound.removeAll()
        }
        condition.unlock()
    }

    /* Frames leave one per frame interval on a schedule anchored to an origin
     * rather than to the previous send, so a late wakeup is caught up instead
     * of pushing every frame behind it.
     *
     * Nothing leaves before the device says its audio channel is live. Capture
     * has been running since startup, and only the few hundred milliseconds
     * leading up to that moment are worth sending: the rest belongs to no
     * session, and the ring the device drains from is empty right then. */
    private func paceLoop() {
        defer { pacerDone.signal() }
        waitUntilArmed()
        var origin = Date().addingTimeInterval(-Self.leadSeconds)
        var index = 0
        while true {
            if takeArm() {
                /* Another session began and the device emptied its ring: the
                 * lead has to be rebuilt before the schedule means anything. */
                rebuildLead()
                origin = Date().addingTimeInterval(-Self.leadSeconds)
                index = 0
            }
            guard let step = nextFrame() else { break }
            if step.waited {
                /* Capture ran dry, so this frame is late by definition.
                 * Re-anchor the schedule, or the deadlines it already missed
                 * all fire at once and overflow the device's ring. */
                origin = Date().addingTimeInterval(-Self.leadSeconds)
                index = 0
            }
            let due = origin.addingTimeInterval(Double(index) * Self.frameInterval)
            index += 1
            let now = Date()
            if due > now {
                Thread.sleep(forTimeInterval: due.timeIntervalSince(now))
                if isStopped { break }
            }
            /* The device times its packets from the samples themselves, so the
             * timestamp only has to advance with the audio: frame n starts at
             * n * 352 samples. */
            let ptsMs = framesQueued * Self.samplesPerFrame * 1000 / Self.sampleRate
            let packet = sink.makeAudioPacket(step.frame, ptsMs: ptsMs)
            condition.lock()
            framesQueued += 1
            outbound.append(packet)
            var warning: String?
            /* A backlog the device could not have played yet is worth less than
             * the audio being captured now: drop the oldest. */
            while outbound.count > Self.outboundLimit {
                outbound.removeFirst()
                samplesDropped += Self.samplesPerFrame
                if warning == nil { warning = overflowWarning("音频链路跟不上，丢弃了最旧的音频包") }
            }
            condition.broadcast()
            condition.unlock()
            if let warning { fputs(warning, stderr) }
        }
    }

    /* The ring is empty when the stream starts, so the first frames have to
     * come from audio captured ahead of time. Waiting for them here is what
     * turns the lead into occupancy on the device instead of latency on this
     * side. */
    private func waitForPreRoll() {
        condition.lock()
        defer { condition.unlock() }
        while pcm.count < Self.leadBytes && !stopped && live { condition.wait() }
    }

    /* The device empties its ring when a session starts, which is also the
     * moment the sound first matters - and precisely then this stream has no
     * audio older than a frame or two to put into it. Pacing into an empty ring
     * starves the receiver on every scheduling slip, so wait for the arm byte
     * and start the schedule from the newest lead's worth of audio instead. */
    private func waitUntilArmed() {
        let deadline = Date().addingTimeInterval(Self.armTimeout)
        while !isStopped {
            /* Short polls: the device is already draining, so every millisecond
             * spent here is a slot it has to fill with silence. */
            switch sink.pollAudioArm(timeout: 0.05) {
            case .arm:
                rebuildLead()
                return
            case .closed:
                /* Connection gone; the writer thread reports that. */
                return
            case .none:
                if Date() > deadline {
                    fputs("audio: 设备没有通知音频通道就绪，直接开始发送\n", stderr)
                    rebuildLead()
                    return
                }
            }
        }
    }

    /* One non-blocking look for an arm byte. */
    private func takeArm() -> Bool {
        sink.pollAudioArm(timeout: 0) == .arm
    }

    /* Keeps the newest lead's worth of converted audio, and waits for it when
     * capture has not run that far ahead yet. */
    private func rebuildLead() {
        condition.lock()
        armed = true
        if pcm.count > Self.leadBytes {
            let excess = pcm.count - Self.leadBytes
            pcm.removeFirst(excess)
            samplesDropped += excess / (Self.channels * 2)
        }
        condition.unlock()
        waitForPreRoll()
    }

    /* One frame's worth of PCM, or nil once the thread should exit. `waited`
     * reports that the queue, not the schedule, set the pace. */
    private func nextFrame() -> (frame: [UInt8], waited: Bool)? {
        condition.lock()
        defer { condition.unlock() }
        let waited = pcm.count < Self.frameBytes
        while pcm.count < Self.frameBytes && !stopped && live { condition.wait() }
        guard !stopped, live else { return nil }
        let frame = alacFrame(pcm)
        pcm.removeFirst(Self.frameBytes)
        return (frame, waited)
    }

    /* Whole frames, oldest first: the samples after a drop stay where they were,
     * while dropping part of a frame would garble the bits. Caller holds the
     * lock. */
    private func trimLocked() -> String? {
        /* Before the device's channel is live there is nothing to send, so the
         * queue only keeps the lead the pacer will burst at the arm. Falling
         * behind a stream that has nowhere to go is not an overflow. */
        let limit = armed ? Self.pcmLimitBytes : Self.leadBytes
        let excess = pcm.count - limit
        guard excess > 0 else { return nil }
        let drop = (excess + Self.frameBytes - 1) / Self.frameBytes * Self.frameBytes
        let removed = min(drop, pcm.count)
        pcm.removeFirst(removed)
        samplesDropped += removed / (Self.channels * 2)
        return armed ? overflowWarning("音频发送跟不上采集，丢弃了最旧的音频") : nil
    }

    /* Overflow warnings are per episode: a link that cannot keep up would
     * otherwise print one line per dropped packet. Caller holds the lock. */
    private func overflowWarning(_ text: String) -> String? {
        guard !overflowing else { return nil }
        overflowing = true
        return text + "\n"
    }

    /* One line every 10 s. Every number is on the 44100 Hz timeline, so they add
     * up: converted = sent + pcm queue + packet queue + dropped. `blocked` is how
     * long write(2) held the writer up: when it grows, the link cannot absorb the
     * stream and the shortfall is the link's, not the capture's. */
    private func report() {
        condition.lock()
        let line = String(format: "sent %d audio frame(s) (%d total), %d buffer(s) captured, "
                                + "converted %d sample(s) = %d ms, dropped %d sample(s), "
                                + "queues %d + %d ms, blocked %d ms writing\n",
                          framesSent - framesSentAtLastReport, framesSent, buffersTaken,
                          samplesConverted, samplesConverted * 1000 / Self.sampleRate, samplesDropped,
                          pcm.count * 1000 / Self.bytesPerSecond,
                          outbound.count * Self.samplesPerFrame * 1000 / Self.sampleRate,
                          Int(sendBlocked * 1000))
        framesSentAtLastReport = framesSent
        if overflowing, capturedBytes <= Self.captureLimitBytes / 2,
           pcm.count <= Self.pcmLimitBytes / 2, outbound.count <= Self.outboundLimit / 2 {
            overflowing = false
        }
        condition.unlock()
        fputs(line, stderr)
    }

    /* Dropped audio is counted on the 44100 Hz timeline, so the report compares
     * like with like whatever the capture rate was. */
    private static func samples(at format: AudioStreamBasicDescription, frames: Int) -> Int {
        guard format.mSampleRate > 0 else { return frames }
        return Int((Double(frames) * Double(sampleRate) / format.mSampleRate).rounded())
    }

    /* AudioStreamBasicDescription is not Equatable; these are the fields that
     * decide whether a cached converter still fits. */
    private static func sameFormat(_ a: AudioStreamBasicDescription, _ b: AudioStreamBasicDescription) -> Bool {
        a.mSampleRate == b.mSampleRate && a.mFormatID == b.mFormatID
            && a.mFormatFlags == b.mFormatFlags && a.mBytesPerPacket == b.mBytesPerPacket
            && a.mFramesPerPacket == b.mFramesPerPacket && a.mChannelsPerFrame == b.mChannelsPerFrame
            && a.mBitsPerChannel == b.mBitsPerChannel
    }

    private var isStopped: Bool {
        condition.lock()
        defer { condition.unlock() }
        return stopped
    }

    /* One audio path only: the first failure ends it and the rest is dropped. */
    private func shutDown() {
        condition.lock()
        live = false
        condition.broadcast()
        condition.unlock()
    }

    /* One capture chunk resampled to the device format. */
    private func convert(_ chunk: Chunk) -> Data? {
        var asbd = chunk.format
        guard let source = AVAudioFormat(streamDescription: &asbd),
              let converter = converter(for: source),
              let input = AVAudioPCMBuffer(pcmFormat: source, frameCapacity: AVAudioFrameCount(chunk.frames)) else { return nil }
        input.frameLength = AVAudioFrameCount(chunk.frames)
        guard fill(input, from: chunk) else { return nil }
        let capacity = AVAudioFrameCount(Double(chunk.frames) * Double(Self.sampleRate) / source.sampleRate) + 64
        guard let output = AVAudioPCMBuffer(pcmFormat: Self.targetFormat, frameCapacity: capacity) else { return nil }
        /* Hand the buffer over once, then report that nothing is left: the
         * converter emits what it can and keeps its resampler state for the
         * buffers that follow. */
        var supplied = false
        var error: NSError?
        let status = converter.convert(to: output, error: &error) { _, inputStatus in
            if supplied {
                inputStatus.pointee = .noDataNow
                return nil
            }
            supplied = true
            inputStatus.pointee = .haveData
            return input
        }
        guard status != .error, output.frameLength > 0 else { return nil }
        let buffers = output.mutableAudioBufferList.pointee
        let bytes = Int(output.frameLength) * Self.channels * 2
        guard buffers.mNumberBuffers == 1, let data = buffers.mBuffers.mData,
              Int(buffers.mBuffers.mDataByteSize) >= bytes else { return nil }
        var pcm = Data(bytes: data, count: bytes)
        if gain != 1.0 {
            pcm.withUnsafeMutableBytes { raw in
                guard let base = raw.baseAddress?.assumingMemoryBound(to: Int16.self) else { return }
                for i in 0..<(bytes / 2) {
                    let scaled = Double(base[i]) * gain
                    base[i] = Int16(max(-32768, min(32767, scaled.rounded())))
                }
            }
        }
        return pcm
    }

    /* Puts the copied bytes back into the layout the format describes: a
     * non-interleaved buffer stores one plane per channel back to back, which is
     * the order AVAudioPCMBuffer hands its buffers out in. */
    private func fill(_ input: AVAudioPCMBuffer, from chunk: Chunk) -> Bool {
        let buffers = UnsafeMutableAudioBufferListPointer(input.mutableAudioBufferList)
        var offset = 0
        return chunk.bytes.withUnsafeBytes { raw -> Bool in
            guard let base = raw.baseAddress else { return false }
            for buffer in buffers {
                let size = Int(buffer.mDataByteSize)
                guard let destination = buffer.mData, offset + size <= raw.count else { return false }
                memcpy(destination, base.advanced(by: offset), size)
                offset += size
            }
            return true
        }
    }

    /* Built on the first chunk, and rebuilt if the capture format ever changes:
     * the device format is fixed, the capture side is whatever ScreenCaptureKit
     * hands over. */
    private func converter(for source: AVAudioFormat) -> AVAudioConverter? {
        if let converter, let current = converterFormat,
           Self.sameFormat(current, source.streamDescription.pointee) {
            return converter
        }
        guard let made = AVAudioConverter(from: source, to: Self.targetFormat) else {
            fputs(String(format: "无法把 %.0f Hz %d ch 的音频转成 44100 Hz 16 bit 立体声，关闭音频通道\n",
                         source.sampleRate, Int(source.channelCount)), stderr)
            shutDown()
            return nil
        }
        converter = made
        converterFormat = source.streamDescription.pointee
        fputs(String(format: "audio: %.0f Hz %d ch -> %d Hz 16-bit stereo, %d samples/frame\n",
                     source.sampleRate, Int(source.channelCount), Self.sampleRate,
                     Self.samplesPerFrame), stderr)
        return made
    }

    /* One ALAC frame in its uncompressed form, in the bit layout
     * libavcodec/alac.c decode_element() reads for a stereo element: the 55-bit
     * header below, then every sample as 16-bit two's complement, interleaved
     * L,R,L,R, MSB first. 55 bits is not a byte boundary, so the header and the
     * first sample share a byte.
     *
     * The frame is padded with zeros to a 16-byte boundary: the device
     * encrypts whole AES blocks, and a receiver that decrypts the entire UDP
     * payload (rather than stopping at the last complete block of the original
     * length) would otherwise garble the last few samples of every frame - a
     * periodic crackle at 125 frame/s. Decoders stop after the declared sample
     * count, so the padding is never heard. */
    private func alacFrame(_ pcm: Data) -> [UInt8] {
        var writer = BitWriter(capacity: Self.framePaddedBytes)
        writer.write(1, bits: 3)                        /* stereo element (CPE) */
        writer.write(0, bits: 4)                        /* element instance */
        writer.write(0, bits: 12)                       /* unused */
        writer.write(1, bits: 1)                        /* the sample count follows */
        writer.write(0, bits: 2)                        /* extra bits field */
        writer.write(1, bits: 1)                        /* not compressed */
        writer.write(UInt64(Self.samplesPerFrame), bits: 32)
        for offset in stride(from: 0, to: Self.frameBytes, by: 2) {
            /* The queued PCM is native little-endian; ALAC wants the value it
             * represents, not the bytes it was stored as. */
            let value = UInt64(pcm[pcm.startIndex + offset]) | UInt64(pcm[pcm.startIndex + offset + 1]) << 8
            writer.write(value, bits: 16)
        }
        return writer.finish(alignedTo: 16)
    }

    /* MSB-first bit writer. The 55-bit ALAC header leaves the sample data off a
     * byte boundary, so it cannot be appended to the output as bytes. */
    private struct BitWriter {
        private var bytes: [UInt8] = []
        private var accumulator: UInt64 = 0
        private var pending = 0

        init(capacity: Int) { bytes.reserveCapacity(capacity) }

        mutating func write(_ value: UInt64, bits: Int) {
            accumulator = (accumulator << UInt64(bits)) | (value & ((UInt64(1) << UInt64(bits)) - 1))
            pending += bits
            while pending >= 8 {
                pending -= 8
                bytes.append(UInt8(truncatingIfNeeded: accumulator >> UInt64(pending)))
            }
            accumulator &= (UInt64(1) << UInt64(pending)) - 1
        }

        /* The tail is padded with zeros - up to a `alignment`-byte boundary when
         * asked - so the frame on the wire is what the device encrypts as whole
         * blocks. */
        mutating func finish(alignedTo alignment: Int = 1) -> [UInt8] {
            if pending > 0 {
                bytes.append(UInt8(truncatingIfNeeded: accumulator << UInt64(8 - pending)))
                pending = 0
            }
            if alignment > 1 {
                while bytes.count % alignment != 0 {
                    bytes.append(0)
                }
            }
            return bytes
        }
    }
}

/* Audio leaves the stream muxed into the MP4, written to a standalone WAV file,
 * or - with --tcp - handed to the device's audio channel as ALAC frames. */
final class AudioSink {
    private let muxer: MP4Muxer?
    private let wavPath: String?
    private let sender: ALACAudioSender?
    private var wav: WAVWriter?
    private var broken = false
    private(set) var buffers = 0

    init(muxer: MP4Muxer) { self.muxer = muxer; wavPath = nil; sender = nil }
    init(wavPath: String) { muxer = nil; self.wavPath = wavPath; sender = nil }
    init(sender: ALACAudioSender, wavPath: String?) { muxer = nil; self.wavPath = wavPath; self.sender = sender }

    func append(_ sampleBuffer: CMSampleBuffer) {
        buffers += 1
        sender?.append(sampleBuffer)
        if let muxer { muxer.appendAudio(sampleBuffer); return }
        guard wavPath != nil else { return }
        if wav == nil, !broken {
            guard let format = CMSampleBufferGetFormatDescription(sampleBuffer),
                  let writer = WAVWriter(format: format, path: wavPath ?? "") else {
                broken = true
                fputs("无法写入音频文件 \(wavPath ?? "?")：需要 32 位浮点 PCM 输入\n", stderr)
                return
            }
            wav = writer
        }
        wav?.append(sampleBuffer)
    }

    func finish() {
        sender?.finish()
        wav?.finish()
    }
}

/* Only instantiated on macOS 13+, where SCStreamOutputType.audio exists. */
@available(macOS 13.0, *)
final class AudioCaptureDelegate: NSObject, SCStreamOutput {
    private let sink: AudioSink
    private var lastReport = Date()
    private var announced = false

    init(sink: AudioSink) { self.sink = sink }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, CMSampleBufferIsValid(sampleBuffer), CMSampleBufferDataIsReady(sampleBuffer) else { return }
        if !announced {
            announced = true
            if let asbd = CMSampleBufferGetFormatDescription(sampleBuffer).flatMap({ CMAudioFormatDescriptionGetStreamBasicDescription($0)?.pointee }) {
                fputs(String(format: "audio stream connected: %.0f Hz, %d ch, %@\n",
                             asbd.mSampleRate, asbd.mChannelsPerFrame,
                             asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0 ? "non-interleaved" : "interleaved"), stderr)
            }
        }
        sink.append(sampleBuffer)
        let now = Date()
        if now.timeIntervalSince(lastReport) >= 10 {
            lastReport = now
            fputs("captured \(sink.buffers) audio buffer(s)\n", stderr)
        }
    }
}

final class H264Encoder {
    private var session: VTCompressionSession?
    private let output: FileHandle?
    private let muxer: MP4Muxer?
    private let lock = NSLock()
    private var failed: Error?
    private var frameCount = 0
    private let width: Int
    private let height: Int
    private let network: NetworkSink?

    init(width: Int, height: Int, fps: Int, bitrate: Int, output: FileHandle?, muxer: MP4Muxer? = nil,
         network: NetworkSink? = nil) throws {
        let sinks = [output != nil, muxer != nil, network != nil].filter { $0 }.count
        guard sinks == 1 else { throw CLIError.message("必须选择一种输出格式") }
        self.output = output
        self.muxer = muxer
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
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: fps * 2 as CFTypeRef)
        /* Keyframes are the expensive ones: 1224x846 of text costs a couple of
         * hundred kilobytes, which takes several frame intervals to send. The
         * device queues that many frames, so a two-second period neither stalls
         * the pipeline nor leaves a broken reference chain waiting long. */
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                             value: 2.0 as CFTypeRef)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: fps as CFTypeRef)
        guard VTCompressionSessionPrepareToEncodeFrames(s) == noErr else { throw CLIError.message("无法准备 H.264 编码器") }
    }

    func encode(_ sampleBuffer: CMSampleBuffer) {
        guard let session, let image = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration = CMSampleBufferGetDuration(sampleBuffer)
        /* VTCompressionPropertyKey_ForceKeyFrame only exists as a per-frame
         * option on this SDK, so force it through frameProperties. */
        var properties: CFDictionary?
        if network?.needsKeyFrame == true {
            properties = [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary
        }
        var flags = VTEncodeInfoFlags()
        let status = VTCompressionSessionEncodeFrame(session, imageBuffer: image, presentationTimeStamp: pts, duration: duration, frameProperties: properties, sourceFrameRefcon: nil, infoFlagsOut: &flags)
        if status != noErr { setFailure(CLIError.message("H.264 编码失败：\(status)")) }
    }

    func finish() {
        if let session { VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid); VTCompressionSessionInvalidate(session) }
        muxer?.finish()
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
        if let muxer {
            muxer.appendVideo(sampleBuffer, format: format)
            frameCount += 1
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

/* MP4 takes the audio as a second track, --tcp hands it to the device's audio
 * channel, and every other output needs a standalone WAV file, whose path
 * either comes from --audio-output or is derived from the video file name. */
func makeAudioSink(_ options: Options, muxer: MP4Muxer?, outputURL: URL?,
                   sender: ALACAudioSender?) throws -> AudioSink? {
    guard options.audio else { return nil }
    if let muxer { return AudioSink(muxer: muxer) }
    if let sender { return AudioSink(sender: sender, wavPath: options.audioOutput) }
    /* --tcp with no audio connection: the device takes nothing, so a WAV file is
     * the only destination left, and there is one only when asked for. */
    if options.tcpHost != nil { return options.audioOutput.map { AudioSink(wavPath: $0) } }
    if let explicit = options.audioOutput { return AudioSink(wavPath: explicit) }
    guard let outputURL else { throw CLIError.invalid("--audio 配合 --stdout 时需要 --audio-output PATH 指定 WAV 文件") }
    return AudioSink(wavPath: outputURL.deletingPathExtension().appendingPathExtension("wav").path)
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
    let muxer = isMP4 ? MP4Muxer(url: outputURL!, audioExpected: options.audio) : nil
    /* The device's audio channel is the one destination that has to be set up
     * before capture starts, so a socket that never came up leaves --tcp with
     * video only rather than an error. */
    var audioSender: ALACAudioSender?
    if options.audio, let network, network.audioConnected {
        audioSender = ALACAudioSender(sink: network, gain: options.audioGain)
    }
    let audioSink = try makeAudioSink(options, muxer: muxer, outputURL: outputURL, sender: audioSender)
    let encoder = try H264Encoder(width: target.width, height: target.height, fps: options.fps,
                                  bitrate: bitrate, output: output,
                                  muxer: muxer, network: network)
    let delegate = CaptureDelegate(encoder: encoder)
    let config = SCStreamConfiguration()
    config.width = target.width; config.height = target.height
    config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(options.fps))
    config.queueDepth = 3; config.pixelFormat = kCVPixelFormatType_32BGRA; config.showsCursor = options.showCursor
    if #available(macOS 13.0, *) {
        config.capturesAudio = options.audio
        if options.audio { config.sampleRate = 48000; config.channelCount = 2 }
    }
    let stream = SCStream(filter: target.filter, configuration: config, delegate: StreamDelegate())
    try stream.addStreamOutput(delegate, type: .screen, sampleHandlerQueue: DispatchQueue(label: "capture-cli.encode"))
    let audioKeepAlive: [SCStreamOutput]
    if let sink = audioSink {
        guard #available(macOS 13.0, *) else { throw CLIError.message("--audio 需要 macOS 13 或更高版本") }
        let delegate = AudioCaptureDelegate(sink: sink)
        try stream.addStreamOutput(delegate, type: .audio,
                                   sampleHandlerQueue: DispatchQueue(label: "capture-cli.audio"))
        audioKeepAlive = [delegate]
    } else {
        audioKeepAlive = []
    }
    fputs("recording \(target.label) \(target.width)x\(target.height) @ \(options.fps) fps\(audioKeepAlive.isEmpty ? "" : " + audio"); Ctrl-C to stop\n", stderr)
    try await stream.startCapture()
    fputs("capture started\n", stderr)
    if let duration = options.duration {
        try await Task.sleep(nanoseconds: UInt64(max(0, duration) * 1_000_000_000))
    } else {
        /* 视频连接断了之后没人再消费帧：捕获还挂着只会每秒刷一行「连接已断开」，
         * 音频继续跑，电视就永远停在最后一帧（外面看不出进程有问题）。所以除了
         * SIGINT，还要盯编码器记录的失败：一旦写不出去就收工，让上层重起一路。 */
        final class StopFlag {
            var stopped = false
        }
        let flag = StopFlag()
        let signalSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        signal(SIGINT, SIG_IGN)
        signalSource.setEventHandler { flag.stopped = true }
        signalSource.resume()
        while !flag.stopped && encoder.error == nil && muxer?.error == nil {
            try? await Task.sleep(nanoseconds: 200_000_000)
        }
        signalSource.cancel()
    }
    try? await stream.stopCapture(); encoder.finish(); audioSink?.finish()
    if let sink = audioSink { fputs("audio: \(sink.buffers) buffer(s) captured\n", stderr) }
    if let error = encoder.error ?? muxer?.error { throw error }
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
