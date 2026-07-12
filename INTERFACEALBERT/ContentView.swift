//
//  ContentView.swift  (backend)
//  INTERFACEALBERT
//
//  App entry point + Bluetooth stack + live telemetry model.
//  The visual interface (ContentView, RoboEyes, AlbertPalette) lives
//  in INTERFACEALBERTApp.swift.
//

import SwiftUI
import CoreBluetooth
import Combine

// =====================================================
// APP ENTRY POINT
// =====================================================
@main
struct INTERFACEALBERTApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

// Live telemetry parsed from the robot's STATUS reply, e.g.
// "H:60 E:70 B:0 Bond:0 pets:0 age:1 act:zoomies!"
struct PetStats: Equatable {
    var happiness: Int
    var energy: Int
    var boredom: Int
    var bonding: Int
    var pets: Int
    var age: Int
    var activity: String = ""   // current autopilot activity name

    /// Parse the firmware STATUS string. Returns nil if it isn't one.
    static func parse(_ s: String) -> PetStats? {
        guard s.contains("H:"), s.contains("Bond:") else { return nil }
        var map: [String: Int] = [:]
        var activity = ""
        for token in s.split(separator: " ") {
            let parts = token.split(separator: ":")
            guard parts.count == 2 else { continue }
            if let value = Int(parts[1]) {
                map[String(parts[0])] = value
            } else if parts[0] == "act" {
                activity = String(parts[1])
            }
        }
        guard let h = map["H"], let e = map["E"],
              let b = map["B"], let bond = map["Bond"] else { return nil }
        return PetStats(happiness: h, energy: e, boredom: b, bonding: bond,
                        pets: map["pets"] ?? 0, age: map["age"] ?? 0,
                        activity: activity)
    }
}

// =====================================================
// BLE MANAGER
// =====================================================
class BLEManager: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {

    // Nordic UART Service UUIDs — must match the ESP32 sketch
    static let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    static let rxUUID      = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E") // write (robot's RX)
    static let txUUID      = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E") // notify (robot's TX)

    private var centralManager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rxCharacteristic: CBCharacteristic?

    @Published var isBluetoothOn = false
    @Published var isScanning = false
    @Published var isConnected = false
    @Published var statusText = "Not connected"
    @Published var lastMessage = ""
    @Published var stats: PetStats?

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }

    func startScan() {
        guard centralManager.state == .poweredOn else {
            statusText = "Bluetooth is not powered on"
            return
        }
        statusText = "Scanning for AlbertMini..."
        isScanning = true
        centralManager.scanForPeripherals(withServices: [BLEManager.serviceUUID], options: nil)
    }

    func stopScan() {
        centralManager.stopScan()
        isScanning = false
    }

    /// True while the user explicitly asked to disconnect — suppresses
    /// the automatic reconnect.
    private var userDisconnected = false

    func disconnect() {
        userDisconnected = true
        if let p = peripheral {
            centralManager.cancelPeripheralConnection(p)
        }
    }

    /// Send a text command, e.g. "WALK", "STOP", "PUSHUPS"
    func send(_ command: String) {
        guard let rx = rxCharacteristic, let p = peripheral else {
            statusText = "Not connected — can't send \"\(command)\""
            return
        }
        guard let data = command.data(using: .utf8) else { return }
        p.writeValue(data, for: rx, type: .withoutResponse)
        statusText = "Sent: \(command)"
    }

    /// Ask the robot for its current mood/telemetry (updates `stats`).
    func requestStatus() {
        guard isConnected else { return }
        send("STATUS")
    }

    // ---------------- CBCentralManagerDelegate ----------------

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        isBluetoothOn = (central.state == .poweredOn)
        if central.state == .poweredOn {
            statusText = "Bluetooth ready"
        } else {
            statusText = "Bluetooth unavailable (check Settings)"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                         advertisementData: [String: Any], rssi RSSI: NSNumber) {
        stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        statusText = "Found \(peripheral.name ?? "device"), connecting..."
        centralManager.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        isConnected = true
        userDisconnected = false
        statusText = "Connected to \(peripheral.name ?? "AlbertMini")"
        peripheral.discoverServices([BLEManager.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        rxCharacteristic = nil
        stats = nil
        if userDisconnected {
            statusText = "Disconnected"
        } else {
            // dropped link (robot rebooted, out of range): quietly rescan
            statusText = "Connection lost — reconnecting…"
            startScan()
        }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        statusText = "Failed to connect: \(error?.localizedDescription ?? "unknown error")"
        if !userDisconnected {
            // keep the auto-reconnect promise: go back to scanning
            startScan()
        }
    }

    // ---------------- CBPeripheralDelegate ----------------

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for service in services where service.uuid == BLEManager.serviceUUID {
            peripheral.discoverCharacteristics([BLEManager.rxUUID, BLEManager.txUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            if characteristic.uuid == BLEManager.rxUUID {
                rxCharacteristic = characteristic
                statusText = "Ready to send commands"
            } else if characteristic.uuid == BLEManager.txUUID {
                peripheral.setNotifyValue(true, for: characteristic)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BLEManager.txUUID, let data = characteristic.value else { return }
        let message = String(data: data, encoding: .utf8) ?? ""
        lastMessage = message
        if let parsed = PetStats.parse(message) {
            stats = parsed          // STATUS telemetry
        }
    }
}
