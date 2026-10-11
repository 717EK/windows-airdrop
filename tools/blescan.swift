// Second-device check for tools/bleprobe, for a Mac: swiftc blescan.swift -o blescan && ./blescan 30
// Prints every advert carrying the control (company 0xFFFF) or an Apple Continuity
// AirDrop TLV (company 0x004C, first TLV type 0x05). CoreBluetooth's manufacturer
// data starts with the 2-byte little-endian company id.
import CoreBluetooth
import Foundation

final class Scanner: NSObject, CBCentralManagerDelegate {
    var seen = Set<String>()
    lazy var central = CBCentralManager(delegate: self, queue: nil)

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        print("STATE \(c.state.rawValue) (5 = poweredOn, 3 = unauthorized)")
        fflush(stdout)
        if c.state == .poweredOn {
            c.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData ad: [String: Any], rssi: NSNumber) {
        guard let md = ad[CBAdvertisementDataManufacturerDataKey] as? Data, md.count >= 2 else { return }
        let company = UInt16(md[0]) | UInt16(md[1]) << 8
        let value = md.dropFirst(2)
        guard company == 0xFFFF || (company == 0x004C && value.first == 0x05) else { return }
        let hex = value.map { String(format: "%02x", $0) }.joined()
        let key = "\(p.identifier) \(company) \(hex)"
        if seen.insert(key).inserted {
            print(String(format: "HEARD company=0x%04x data=%@ from=%@ rssi=%@", company, hex,
                         p.identifier.uuidString, rssi))
            fflush(stdout)
        }
    }
}

let seconds = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 20 : 20
let scanner = Scanner()
_ = scanner.central
RunLoop.main.run(until: Date().addingTimeInterval(seconds))
print("DONE \(Int(seconds))s, \(scanner.seen.count) distinct matching adverts")
