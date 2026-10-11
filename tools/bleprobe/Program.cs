// Can Windows do AirDrop's Bluetooth half? Built-in WinRT APIs only.
//
//   publish [seconds]  Broadcast Apple Continuity AirDrop (type 0x05) manufacturer
//                      data: the sender-side wake omdrop's ble-airdrop-adv.py sends
//                      (arm A, zero hashes = "no contact match", which Everyone mode
//                      accepts). A 0xFFFF (reserved test company) advert runs first
//                      as a control.
//   scan [seconds]     Listen for Apple manufacturer data and count Continuity TLV
//                      types. Open Share > AirDrop on a nearby iPhone to see 0x05.
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;

const ushort Apple = 0x004C;

var mode = args.Length > 0 ? args[0] : "";
var seconds = args.Length > 1 ? int.Parse(args[1]) : 10;
if (mode != "publish" && mode != "scan") {
    Console.WriteLine("usage: bleprobe publish|scan [seconds]");
    return 2;
}

var adapter = await BluetoothAdapter.GetDefaultAsync();
if (adapter is null) {
    Console.WriteLine("NO BLUETOOTH ADAPTER");
    return 4;
}
Console.WriteLine($"adapter  LE={adapter.IsLowEnergySupported} peripheral={adapter.IsPeripheralRoleSupported} " +
                  $"central={adapter.IsCentralRoleSupported} offload={adapter.IsAdvertisementOffloadSupported} " +
                  $"extendedAdv={adapter.IsExtendedAdvertisingSupported} maxAdvLen={adapter.MaxAdvertisementDataLength}");

if (mode == "publish") {
    // Continuity TLV: type 0x05, length 0x12, arm A body: 40 <tag 3> 00000000 03 <hashes 8> 00
    byte[] airdrop = [0x05, 0x12, 0x40, 0, 0, 0, 0, 0, 0, 0, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    var control = await Publish(0xFFFF, [0x01, 0x02, 0x03], "control");
    var apple = await Publish(Apple, airdrop, "airdrop");
    Console.WriteLine($"RESULT control={control} airdrop={apple}");
    Console.WriteLine("NOTE  'Started' means Windows accepted and is advertising it. It does not prove what " +
                      "went on air: confirm with a second scanner (e.g. nRF Connect, filter company 0x004C).");
    return 0;
}

// scan
var counts = new SortedDictionary<byte, int>();
var devices = new Dictionary<byte, HashSet<ulong>>();
var watcher = new BluetoothLEAdvertisementWatcher { ScanningMode = BluetoothLEScanningMode.Active };
watcher.Received += (_, e) => {
    foreach (var md in e.Advertisement.ManufacturerData) {
        if (md.CompanyId != Apple)
            continue;
        var b = md.Data.ToArray();
        for (int i = 0; i + 1 < b.Length; i += 2 + b[i + 1]) { // Continuity TLVs: type, length, value
            lock (counts) {
                counts[b[i]] = counts.GetValueOrDefault(b[i]) + 1;
                if (!devices.TryGetValue(b[i], out var set))
                    devices[b[i]] = set = [];
                set.Add(e.BluetoothAddress);
            }
        }
    }
};
watcher.Start();
await Task.Delay(TimeSpan.FromSeconds(seconds));
var status = watcher.Status;
watcher.Stop();
Console.WriteLine($"scan     {seconds} s, watcher status {status}");
var names = new Dictionary<byte, string> {
    [0x05] = "AirDrop", [0x07] = "Proximity pairing", [0x09] = "AirPlay target", [0x0c] = "Handoff",
    [0x0f] = "Nearby action", [0x10] = "Nearby info", [0x12] = "Find My",
};
if (counts.Count == 0)
    Console.WriteLine("scan     no Apple (0x004C) advertisements heard");
foreach (var (type, n) in counts)
    Console.WriteLine($"apple    type 0x{type:x2} {names.GetValueOrDefault(type, "?"),-18} adverts={n} devices={devices[type].Count}");
return 0;

async Task<bool> Publish(ushort company, byte[] data, string label) {
    var pub = new BluetoothLEAdvertisementPublisher();
    pub.Advertisement.ManufacturerData.Add(new BluetoothLEManufacturerData(company, data.AsBuffer()));
    var hex = Convert.ToHexString(data).ToLowerInvariant();
    var trail = new List<string>();
    BluetoothError error = BluetoothError.Success;
    pub.StatusChanged += (_, e) => {
        lock (trail) {
            trail.Add(e.Status.ToString());
            if (e.Error != BluetoothError.Success)
                error = e.Error;
        }
    };
    try {
        pub.Start();
    } catch (Exception ex) {
        Console.WriteLine($"{label,-8} company=0x{company:x4} data={hex}  START THREW: {ex.Message.Trim()}");
        return false;
    }
    await Task.Delay(TimeSpan.FromSeconds(seconds));
    var final = pub.Status;
    if (final == BluetoothLEAdvertisementPublisherStatus.Started)
        pub.Stop();
    lock (trail)
        Console.WriteLine($"{label,-8} company=0x{company:x4} data={hex}  status: {string.Join(" -> ", trail)}" +
                          (error != BluetoothError.Success ? $"  error={error}" : ""));
    return final == BluetoothLEAdvertisementPublisherStatus.Started;
}
