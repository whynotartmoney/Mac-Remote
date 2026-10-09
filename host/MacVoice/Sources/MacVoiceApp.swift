import AppKit
import ApplicationServices
import AVFoundation
import CoreBluetooth
import CoreText
import Speech
import SwiftUI

private let serviceUUID = CBUUID(string: "F7A1C3E0-5B24-4D91-8C6E-1A2B3C4D5E6F")
private let eventUUID = CBUUID(string: "F7A1C3E0-5B24-4D91-8C6E-1A2B3C4D5E70")
private let textUUID = CBUUID(string: "F7A1C3E0-5B24-4D91-8C6E-1A2B3C4D5E71")

private enum Adpcm {
    static let steps: [Int] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    ]
    static let adjust: [Int] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

    static func encode(_ samples: [Int16]) -> [UInt8] {
        var predictor = 0
        var index = 0
        var out = [UInt8](repeating: 0, count: samples.count / 2)
        for (i, sample) in samples.enumerated() {
            let nibble = encodeNibble(&predictor, &index, Int(sample))
            if i % 2 == 0 { out[i / 2] = nibble } else { out[i / 2] |= nibble << 4 }
        }
        return out
    }

    static func decode(predictor: Int16, index: Int8, nibbles: Data, samples: Int) -> [Int16] {
        var pred = Int(predictor)
        var idx = Int(index)
        var pcm = [Int16]()
        pcm.reserveCapacity(samples)
        for i in 0..<samples {
            let byte = i / 2 < nibbles.count ? nibbles[nibbles.startIndex + i / 2] : 0
            let nibble = i % 2 == 0 ? byte & 0x0F : byte >> 4
            pcm.append(decodeNibble(&pred, &idx, Int(nibble)))
        }
        return pcm
    }

    private static func clamp(_ value: Int) -> Int {
        min(32767, max(-32768, value))
    }

    private static func encodeNibble(_ predictor: inout Int, _ index: inout Int, _ sample: Int) -> UInt8 {
        var diff = sample - predictor
        var nibble = 0
        if diff < 0 {
            nibble = 8
            diff = -diff
        }
        var stepIndex = min(88, max(0, index))
        var step = steps[stepIndex]
        var delta = step >> 3
        if diff >= step {
            nibble |= 4
            diff -= step
            delta += step
        }
        step >>= 1
        if diff >= step {
            nibble |= 2
            diff -= step
            delta += step
        }
        step >>= 1
        if diff >= step {
            nibble |= 1
            delta += step
        }
        predictor = clamp(predictor + ((nibble & 8) != 0 ? -delta : delta))
        stepIndex = min(88, max(0, stepIndex + adjust[nibble & 0x0F]))
        index = stepIndex
        return UInt8(nibble & 0x0F)
    }

    private static func decodeNibble(_ predictor: inout Int, _ index: inout Int, _ nibble: Int) -> Int16 {
        let code = nibble & 0x0F
        var stepIndex = min(88, max(0, index))
        let step = steps[stepIndex]
        var delta = step >> 3
        if code & 4 != 0 { delta += step }
        if code & 2 != 0 { delta += step >> 1 }
        if code & 1 != 0 { delta += step >> 2 }
        predictor = clamp(predictor + ((code & 8) != 0 ? -delta : delta))
        stepIndex = min(88, max(0, stepIndex + adjust[code]))
        index = stepIndex
        return Int16(predictor)
    }
}

private enum Keys {
    private static let queue = DispatchQueue(label: "macvoice.keys")

    static func trusted(prompt: Bool) -> Bool {
        let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        let options = [key: prompt] as CFDictionary
        return AXIsProcessTrustedWithOptions(options)
    }

    static func backspace(_ count: Int) {
        guard count > 0, trusted(prompt: false) else { return }
        queue.async {
            for _ in 0..<count {
                post(0x33, flags: [])
                usleep(12000)
            }
        }
    }

    static func send() {
        guard trusted(prompt: false) else { return }
        queue.async { post(0x24, flags: []) }
    }

    static func type(_ text: String) {
        guard trusted(prompt: false), !text.isEmpty else { return }
        queue.async {
            let units = Array(text.utf16)
            let source = CGEventSource(stateID: .hidSystemState)
            let down = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: true)
            units.withUnsafeBufferPointer { buffer in
                guard let base = buffer.baseAddress else { return }
                down?.keyboardSetUnicodeString(stringLength: buffer.count, unicodeString: base)
            }
            let up = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: false)
            down?.post(tap: .cghidEventTap)
            usleep(30000)
            up?.post(tap: .cghidEventTap)
        }
    }

    private static func post(_ key: CGKeyCode, flags: CGEventFlags) {
        let source = CGEventSource(stateID: .hidSystemState)
        let down = CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: true)
        let up = CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: false)
        down?.flags = flags
        up?.flags = flags
        down?.post(tap: .cghidEventTap)
        usleep(20000)
        up?.post(tap: .cghidEventTap)
    }
}

private enum SpeechPCM {
    static func floats(from samples: [Int16]) -> [Float] {
        samples.map { Float($0) / 32768 }
    }
}

private final class SpeechSession {
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var task: SFSpeechRecognitionTask?
    private var format: AVAudioFormat?
    private var latest = ""
    private var captured: [Int16] = []
    private var sampleRate = 16_000.0
    private var localeUsed = ""
    private var retried = false
    private let lock = NSLock()
    var authorized = false

    func languageName() -> String {
        let identifier = localeUsed.lowercased()
        if identifier.contains("hk") || identifier.contains("yue") { return "CANTONESE" }
        if identifier.hasPrefix("zh") { return "MANDARIN" }
        if identifier.hasPrefix("en") { return "ENGLISH" }
        return "LISTENING"
    }

    private func makeRecognizer(_ identifier: String) -> SFSpeechRecognizer? {
        guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: identifier)), recognizer.isAvailable else {
            return nil
        }
        return recognizer
    }

    private func recognizer(identifier: String?) -> SFSpeechRecognizer? {
        if let identifier { return makeRecognizer(identifier) }
        let identifiers = ["zh-HK", "yue-Hant-HK"] + Locale.preferredLanguages
            + [Locale.current.identifier, "zh-Hant", "zh-Hans", "en-US"]
        for item in identifiers {
            if let recognizer = makeRecognizer(item) { return recognizer }
        }
        return nil
    }

    func start(sampleRate: Double) -> Bool {
        cancel()
        self.sampleRate = sampleRate
        return begin(identifier: nil, replay: false)
    }

    private func begin(identifier: String?, replay: Bool) -> Bool {
        guard authorized, let recognizer = recognizer(identifier: identifier) else { return false }
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.shouldReportPartialResults = true
        self.request = request
        format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate, channels: 1, interleaved: false)
        latest = ""
        localeUsed = recognizer.locale.identifier
        task = recognizer.recognitionTask(with: request) { [weak self] result, _ in
            guard let self, let result else { return }
            self.lock.lock()
            self.latest = result.bestTranscription.formattedString
            self.lock.unlock()
        }
        if replay { feed(captured) }
        return format != nil && task != nil
    }

    func append(_ samples: [Int16]) {
        if samples.isEmpty { return }
        captured.append(contentsOf: samples)
        feed(samples)
    }

    private func feed(_ samples: [Int16]) {
        let floats = SpeechPCM.floats(from: samples)
        guard let request, let format, !floats.isEmpty,
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(floats.count)),
              let dest = buffer.floatChannelData?[0] else { return }
        buffer.frameLength = AVAudioFrameCount(floats.count)
        floats.withUnsafeBufferPointer { source in
            guard let base = source.baseAddress else { return }
            dest.update(from: base, count: floats.count)
        }
        request.append(buffer)
    }

    func finish(completion: @escaping (String) -> Void) {
        endAndWait { [weak self] text in
            guard let self else {
                completion(text)
                return
            }
            let used = self.localeUsed.lowercased()
            let cantonese = used.contains("hk") || used.contains("yue")
            let fallback = cantonese ? "en-US" : "zh-HK"
            let before = self.localeUsed
            if text.isEmpty, !self.retried, self.begin(identifier: fallback, replay: true), self.localeUsed != before {
                self.retried = true
                self.endAndWait(completion: completion)
            } else {
                completion(text)
            }
        }
    }

    private func endAndWait(completion: @escaping (String) -> Void) {
        request?.endAudio()
        let started = Date()
        func poll() {
            let state = task?.state
            let done = state == .completed || state == .canceling || Date().timeIntervalSince(started) > 8
            if done {
                lock.lock()
                let text = latest
                lock.unlock()
                request = nil
                task = nil
                completion(text)
            } else {
                DispatchQueue.global().asyncAfter(deadline: .now() + 0.05, execute: poll)
            }
        }
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.05, execute: poll)
    }

    func cancel() {
        task?.cancel()
        request = nil
        task = nil
        latest = ""
        captured = []
        retried = false
    }
}

private final class VoiceLink: NSObject, ObservableObject {
    @Published var status = "NO LINK"
    @Published var transcript = ""
    @Published var hint = "Cantonese. Hold OK on AI Passport. Release to finish."

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var eventChar: CBCharacteristic?
    private var textChar: CBCharacteristic?
    private let speech = SpeechSession()
    private let queue = DispatchQueue(label: "macvoice.ble")
    private var prompted = false
    private var helloSent = false
    private var heard = 0

    private var started = false

    func start() {
        guard !started else { return }
        started = true
        central = CBCentralManager(delegate: self, queue: queue)
        SFSpeechRecognizer.requestAuthorization { [weak self] state in
            self?.speech.authorized = state == .authorized
        }
    }

    private func scan(_ central: CBCentralManager) {
        central.scanForPeripherals(
            withServices: nil,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )
    }

    func promptAccess() {
        guard !prompted else { return }
        prompted = true
        if !Keys.trusted(prompt: false) {
            _ = Keys.trusted(prompt: true)
        }
    }

    private func setStatus(_ text: String) {
        DispatchQueue.main.async { self.status = text }
    }

    private func setTranscript(_ text: String) {
        DispatchQueue.main.async { self.transcript = text }
    }

    private func write(_ data: Data) {
        guard let peripheral, let textChar else { return }
        peripheral.writeValue(data, for: textChar, type: .withoutResponse)
    }

    private func writeFail() {
        write(Data([0x11]))
    }

    private func writeTranscript(_ text: String) {
        let bytes = Array(text.utf8.prefix(480))
        guard !bytes.isEmpty, let peripheral, let textChar else {
            writeFail()
            return
        }
        let budget = max(20, peripheral.maximumWriteValueLength(for: .withoutResponse) - 2)
        var offset = 0
        while offset < bytes.count {
            let count = min(budget, bytes.count - offset)
            var packet = [UInt8](repeating: 0, count: 2 + count)
            packet[0] = 0x10
            var flags: UInt8 = 0
            if offset == 0 { flags |= 0x01 }
            if offset + count == bytes.count { flags |= 0x02 }
            packet[1] = flags
            for index in 0..<count { packet[2 + index] = bytes[offset + index] }
            peripheral.writeValue(Data(packet), for: textChar, type: .withoutResponse)
            offset += count
        }
    }

    private func deliver(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            writeFail()
            reveal("TRY AGAIN")
            return
        }
        setTranscript(trimmed)
        queue.asyncAfter(deadline: .now() + 0.12) { self.writeTranscript(trimmed) }
        guard Keys.trusted(prompt: false) else {
            reveal("ALLOW ACCESS")
            return
        }
        DispatchQueue.main.async {
            NSApp.hide(nil)
        }
        queue.asyncAfter(deadline: .now() + 0.35) { [weak self] in
            Keys.type(trimmed)
            self?.setStatus("UP DEL    DOWN SEND")
        }
    }

    private func reveal(_ text: String) {
        setStatus(text)
        DispatchQueue.main.async {
            NSApp.unhide(nil)
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    private func handle(_ data: Data) {
        guard let kind = data.first else { return }
        switch kind {
        case 0x02:
            guard data.count >= 3 else { return }
            heard = 0
            let rate = Double(UInt16(data[1]) | (UInt16(data[2]) << 8))
            if speech.authorized && speech.start(sampleRate: rate) {
                setStatus(speech.languageName())
            } else {
                reveal("ALLOW SPEECH")
            }
        case 0x03:
            guard data.count >= 8 else { return }
            heard += 1
            let predictor = Int16(bitPattern: UInt16(data[3]) | (UInt16(data[4]) << 8))
            let index = Int8(bitPattern: data[5])
            let samples = Int(UInt16(data[6]) | (UInt16(data[7]) << 8))
            let nibbles = data.subdata(in: 8..<data.count)
            speech.append(Adpcm.decode(predictor: predictor, index: index, nibbles: nibbles, samples: samples))
        case 0x04:
            let flags: UInt8 = data.count > 1 ? data[1] : 0
            if flags & 0x02 != 0 || !speech.authorized {
                speech.cancel()
                writeFail()
                reveal(speech.authorized ? "TRY AGAIN" : "ALLOW SPEECH")
                return
            }
            if heard == 0 {
                speech.cancel()
                writeFail()
                reveal("NO AUDIO")
                return
            }
            setStatus("WORKING")
            speech.finish { [weak self] text in self?.deliver(text) }
        case 0x05:
            Keys.backspace(1)
        case 0x06:
            Keys.send()
            setStatus("SENT")
        case 0x07:
            guard data.count >= 3 else { return }
            let count = Int(UInt16(data[1]) | (UInt16(data[2]) << 8))
            Keys.backspace(count)
        default:
            break
        }
    }
}

extension VoiceLink: CBCentralManagerDelegate, CBPeripheralDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            setStatus("SEARCHING")
            scan(central)
        case .poweredOff:
            setStatus("BLUETOOTH OFF")
        case .unauthorized:
            setStatus("ALLOW BLUETOOTH")
        default:
            setStatus("NO LINK")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        let name = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        let overflow = advertisementData[CBAdvertisementDataOverflowServiceUUIDsKey] as? [CBUUID] ?? []
        guard name == "MacVoice" || services.contains(serviceUUID) || overflow.contains(serviceUUID) else { return }
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        setStatus("WAIT")
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        self.peripheral = nil
        setStatus("SEARCHING")
        scan(central)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        self.peripheral = nil
        eventChar = nil
        textChar = nil
        helloSent = false
        speech.cancel()
        setStatus("SEARCHING")
        scan(central)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else { return }
        peripheral.discoverCharacteristics([eventUUID, textUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        eventChar = service.characteristics?.first { $0.uuid == eventUUID }
        textChar = service.characteristics?.first { $0.uuid == textUUID }
        if let eventChar { peripheral.setNotifyValue(true, for: eventChar) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == eventUUID, characteristic.isNotifying else { return }
        offerHello(peripheral, tries: 0)
    }

    private func offerHello(_ peripheral: CBPeripheral, tries: Int) {
        guard self.peripheral === peripheral else { return }
        let payload = maximumPayload(peripheral)
        if payload >= 168 || tries >= 15 {
            if !helloSent {
                helloSent = true
                write(Data([0x01, 0x4D, 0x56, 0x01]))
            }
            setStatus(payload >= 168 ? "HOLD OK" : "LINK SLOW")
            return
        }
        setStatus("SEARCHING")
        queue.asyncAfter(deadline: .now() + 0.2) { [weak self] in
            self?.offerHello(peripheral, tries: tries + 1)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == eventUUID, let value = characteristic.value else { return }
        handle(value)
    }

    private func maximumPayload(_ peripheral: CBPeripheral) -> Int {
        peripheral.maximumWriteValueLength(for: .withoutResponse)
    }
}

private enum AppFonts {
    static func register() {
        guard let url = Bundle.main.url(forResource: "Orbitron-Variable", withExtension: "ttf") else { return }
        CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
    }

    static func face(_ size: CGFloat) -> Font {
        for name in ["Orbitron", "Orbitron-Regular"] {
            if NSFont(name: name, size: size) != nil {
                return .custom(name, size: size)
            }
        }
        return .system(size: size, design: .monospaced)
    }
}

private struct RootView: View {
    @StateObject private var link = VoiceLink()

    var body: some View {
        ZStack {
            Color(red: 0.027, green: 0.067, blue: 0.059).ignoresSafeArea()
            Circle()
                .stroke(Color(red: 0.224, green: 0.949, blue: 0.776), lineWidth: 2)
                .frame(width: 180, height: 180)
            VStack(alignment: .leading, spacing: 18) {
                Text("MAC VOICE")
                    .font(AppFonts.face(22))
                    .foregroundStyle(Color(red: 0.224, green: 0.949, blue: 0.776))
                Text(link.status)
                    .font(AppFonts.face(28))
                    .foregroundStyle(Color(red: 0.843, green: 1.0, blue: 0.957))
                Text(link.transcript.isEmpty ? " " : link.transcript)
                    .font(AppFonts.face(16))
                    .foregroundStyle(Color(red: 0.843, green: 1.0, blue: 0.957))
                    .frame(maxWidth: .infinity, minHeight: 88, alignment: .topLeading)
                Text(link.hint)
                    .font(AppFonts.face(16))
                    .foregroundStyle(Color(red: 0.494, green: 0.620, blue: 0.588))
                Text("It connects by itself. Do not pair it in System Settings. Hold OK to speak. UP deletes one character. DOWN sends.")
                    .font(AppFonts.face(16))
                    .foregroundStyle(Color(red: 0.494, green: 0.620, blue: 0.588))
                    .fixedSize(horizontal: false, vertical: true)
                Spacer()
            }
            .padding(28)
        }
        .frame(minWidth: 420, minHeight: 520)
        .onAppear {
            AppFonts.register()
            link.start()
            link.promptAccess()
        }
    }
}

@main
struct MacVoiceApp: App {
    init() {
        if CommandLine.arguments.contains("--self-test") {
            let ok = MacVoiceSelfTest.run()
            fputs(ok ? "mac voice app self-test: PASS\n" : "mac voice app self-test: FAIL\n", stdout)
            fflush(stdout)
            exit(ok ? 0 : 1)
        }
        AppFonts.register()
    }

    var body: some Scene {
        Window("Mac Voice", id: "main") {
            RootView()
        }
        .defaultSize(width: 440, height: 560)
    }
}

private enum MacVoiceSelfTest {
    static func run() -> Bool {
        let silent = Adpcm.encode([0, 0, 0, 0])
        if silent != [0, 0] { return false }
        let levels = SpeechPCM.floats(from: [0, 32767, -32768])
        if levels.count != 3 || levels[0] != 0 || levels[1] < 0.99 || levels[2] > -1 { return false }
        var pcm = [Int16]()
        for i in 0..<64 { pcm.append(Int16(i * 300)) }
        let bytes = Adpcm.encode(pcm)
        let back = Adpcm.decode(predictor: 0, index: 0, nibbles: Data(bytes), samples: 64)
        var maxError = 0
        for i in 0..<64 {
            maxError = max(maxError, abs(Int(pcm[i]) - Int(back[i])))
        }
        if maxError >= 2500 { return false }
        let start = Data([0x02, 0x80, 0x3E])
        if start[0] != 0x02 || UInt16(start[1]) | (UInt16(start[2]) << 8) != 16000 { return false }
        return true
    }
}
