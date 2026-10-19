using System.Diagnostics;

namespace PulseDrop;

static class Checks
{
    static void Assert(bool condition, string name) { if (!condition) throw new Exception(name); }
    public static int Run()
    {
        try
        {
            string logs = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PulseDrop", "path-check-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(logs);
            try {
                string physical = Session.PhysicalDirectory(logs);
                Assert(Directory.Exists(physical), "physical log directory exists");
                Assert(Directory.GetFiles(physical).Length == 0, "path marker cleaned up");
                var check = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true };
                check.Environment["PULSEDROP_LOG_CHECK"] = physical;
                foreach (var arg in new[] { "-NoProfile", "-Command", "if (-not (Test-Path -LiteralPath $env:PULSEDROP_LOG_CHECK)) { exit 1 }" }) check.ArgumentList.Add(arg);
                using var child = Process.Start(check)!;
                Assert(child.WaitForExit(10000), "helper path check completed");
                Assert(child.ExitCode == 0, "PowerShell can access the resolved log directory");
                string blockedLog = Path.Combine(logs, "blocked.log");
                Directory.CreateDirectory(blockedLog);
                Assert(!Session.TryAppendLog(blockedLog, "test"), "unwritable diagnostic path is handled without crashing");
                Directory.Delete(blockedLog);
            } finally { Directory.Delete(logs); }
            Assert(Session.Identity(Convert.FromHexString("00FF8F35EA6E")).Address == "fe80::2ff:8fff:fe35:ea6e", "TAP identity");
            Assert(Session.PeerAddress("peer added da:2e:a6:70:d2:7 name=phone device_class=2 version=0xa0 valid=1") == "fe80::d82e:a6ff:fe70:d207", "single-digit MAC parsing");
            Assert(Session.PeerAddress("peer added broken name=x") == null, "malformed peer");
            Assert(Session.PeerAddress("peer removed da:2e:a6:70:d2:7 name=x") == null, "removed peer");
            Assert(Device.Validate("phone", "fe80::123%48", 8770).Address == "fe80::123", "scope handling");
            foreach (var ip in new[] { "::1", "192.168.1.2", "garbage" })
            {
                bool refused = false; try { Device.Validate("bad", ip, 8770); } catch (ArgumentException) { refused = true; }
                Assert(refused, "non-AWDL endpoint refused");
            }
            Assert(Session.FriendlyLine("receiver", "{\"received\":\"photo.jpg\",\"bytes\":123}") == "Received photo.jpg (123 bytes).", "receipt display");
            Assert(Session.FriendlyLine("link", "peer channels: 6 149") == null, "technical lines stay in logs");
            Assert(Session.FriendlyLine("link", "channel matched 149 -> 36 (peer slots: 149=2 36=6, peers=1)")!.Contains("channel 36"), "channel match shown");
            Assert(Session.FriendlyLine("link", "channel matched 36 -> 149 (peer slots: 149=0 36=0, peers=0)")!.Contains("149"), "return to 149 shown");
            Assert(Session.FriendlyLine("link", "channel match reverted 36 -> 149 (peer slots: 149=2 36=1, peers=1)")!.Contains("didn't help"), "revert shown");
            Assert(Session.FriendlyLine("recovery", "Bluetooth AirDrop advertising started without switching Bluetooth off.") != null, "soft recovery shown");
            var exportRoot = Directory.CreateTempSubdirectory("PulseDrop-export-check-");
            try {
                File.WriteAllText(Path.Combine(exportRoot.FullName, "sender.log"), "transfer evidence");
                string publisherLog = Path.Combine(exportRoot.FullName, "recovery-bluetooth-soft.stdout.log");
                File.WriteAllText(publisherLog, "airdrop state=Started error=Success\nairdrop state=Waiting error=ResourceInUse\n");
                Assert(!Session.PublisherLogStarted(publisherLog), "old Started does not hide a current blocked publisher");
                File.AppendAllText(publisherLog, "airdrop state=Started error=Success\n");
                Assert(Session.PublisherLogStarted(publisherLog), "latest Started is recognized");
                File.Delete(publisherLog);
                File.WriteAllText(Path.Combine(exportRoot.FullName, "key.pem"), "do not export");
                File.WriteAllText(Path.Combine(exportRoot.FullName, "photo.jpg"), "do not export");
                string archive = Path.Combine(exportRoot.FullName, "report.zip");
                DiagnosticExport.Write(exportRoot.FullName, archive);
                using (var zip = System.IO.Compression.ZipFile.OpenRead(archive)) {
                    Assert(zip.GetEntry("sender.log") != null && zip.GetEntry("manifest.json") != null, "export contains evidence and manifest");
                    Assert(zip.Entries.Count == 2, "export excludes keys and received files");
                }
                using (var huge = new FileStream(Path.Combine(exportRoot.FullName, "sender.log"), FileMode.Create)) huge.SetLength(65L * 1024 * 1024);
                string bounded = Path.Combine(exportRoot.FullName, "bounded.zip");
                DiagnosticExport.Write(exportRoot.FullName, bounded);
                using (var zip = System.IO.Compression.ZipFile.OpenRead(bounded)) Assert(zip.GetEntry("sender.log")!.Length == 64L * 1024 * 1024, "export is bounded while logs grow");
            } finally {
                string owned = Path.GetFullPath(exportRoot.FullName);
                Assert(owned.StartsWith(Path.GetFullPath(Path.GetTempPath()), StringComparison.OrdinalIgnoreCase) && exportRoot.Name.StartsWith("PulseDrop-export-check-"), "cleanup remains in the owned temporary directory");
                Directory.Delete(owned, true);
            }
            var session = new Session();
            int devices = 0;
            session.DeviceFound += _ => devices++;
            const string deviceEvent = "{\"event\":\"device\",\"name\":\"phone\",\"address\":\"fe80::123\",\"port\":8770}";
            session.ProcessHelperLine("discovery", deviceEvent, session.Generation - 1);
            session.ProcessHelperLine("receiver", deviceEvent, session.Generation);
            Assert(devices == 0, "old-session and wrong-source device events rejected");
            session.ProcessHelperLine("discovery", deviceEvent, session.Generation);
            Assert(devices == 1, "current discovery device accepted");
            session.ProcessHelperLine("discovery", "{\"event\":\"device\",\"name\":null,\"address\":\"fe80::123\",\"port\":8770}", session.Generation);
            Assert(devices == 1, "null device names rejected without crashing");
            session.ProcessHelperLine("discovery", "{\"event\":\"device\",\"name\":\"phone\",\"address\":\"fe80::123\",\"port\":4294967296}", session.Generation);
            Assert(devices == 1, "overflowing helper fields rejected without crashing");
            string? sendStatus = null;
            session.SendStatus += value => sendStatus = value;
            session.ProcessHelperLine("sender", "{\"event\":\"send-trace\",\"phase\":\"http-body-written\",\"endpoint\":\"/Upload\"}", session.Generation);
            Assert(sendStatus?.Contains("Finalizing") == true, "all bytes queued is not shown as completed before the phone responds");
            string? receiveFailure = null;
            session.ReceiveFailed += reason => receiveFailure = reason;
            session.ProcessHelperLine("receiver", "Could not save transfer: OSError", session.Generation);
            Assert(receiveFailure?.Contains("free space") == true, "save failures clear receive progress with an actionable reason");
            string? removedAddress = null;
            session.DeviceRemoved += address => removedAddress = address;
            session.ProcessHelperLine("link", "peer removed da:2e:a6:70:d2:7 name=phone", session.Generation);
            Assert(removedAddress == "fe80::d82e:a6ff:fe70:d207", "removed native peer reaches the device list");
            var earlyExit = new ProcessStartInfo(session.Python) { UseShellExecute = false, CreateNoWindow = true };
            foreach (var arg in new[] { "-E", "-s", "-c", "raise SystemExit(17)" }) earlyExit.ArgumentList.Add(arg);
            using (var failed = Process.Start(earlyExit)!) {
                bool exactFailure = false;
                try { Session.AwaitReadyAsync(new TaskCompletionSource().Task, failed, "test receiver", TimeSpan.FromSeconds(5), default).GetAwaiter().GetResult(); }
                catch (IOException e) { exactFailure = e.Message.Contains("test receiver") && e.Message.Contains("17"); }
                Assert(exactFailure, "early helper exit identifies the stage and exit code");
            }
            var idle = new ProcessStartInfo(session.Python) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
            foreach (var arg in new[] { "-E", "-s", "-u", "-X", "utf8", "-c", "import time; print('ready', flush=True); time.sleep(30)" }) idle.ArgumentList.Add(arg);
            using var sleeper = new Process { StartInfo = idle };
            var childReady = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            sleeper.OutputDataReceived += (_, e) => { if (e.Data == "ready") childReady.TrySetResult(); };
            sleeper.Start(); sleeper.BeginOutputReadLine();
            try {
                childReady.Task.WaitAsync(TimeSpan.FromSeconds(10)).GetAwaiter().GetResult();
                bool timeout = false;
                try { Session.AwaitReadyAsync(new TaskCompletionSource().Task, sleeper, "test readiness", TimeSpan.FromMilliseconds(30), default).GetAwaiter().GetResult(); }
                catch (TimeoutException e) { timeout = e.Message.Contains("test readiness"); }
                Assert(timeout, "a live unready helper reports its exact stage");
                using var cancellation = new CancellationTokenSource();
                var waiting = session.ExitAsync(sleeper, cancellation.Token);
                cancellation.Cancel();
                bool cancelled = false;
                try { waiting.GetAwaiter().GetResult(); } catch (OperationCanceledException) { cancelled = true; }
                Assert(cancelled && sleeper.HasExited, "cancellation reaps the owned child before returning");
            } finally { if (!sleeper.HasExited) { sleeper.Kill(true); sleeper.WaitForExit(); } }
            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "self-test.txt"), "PASS: stage-specific helper readiness/early exit, bounded diagnostic export, identity/endpoints, malformed/old-session events, save/device removal, owned-child cancellation/output drain.\n");
            return 0;
        }
        catch (Exception e) { File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "self-test.txt"), e.ToString()); return 1; }
    }
    public static async Task<int> Lifecycle(int cycles = 2)
    {
        var report = new List<string>(); var session = new Session();
        try
        {
            for (int i = 0; i < cycles; i++)
            {
                await session.StartAsync(); Assert(session.Running, "receiver readiness");
                report.Add("Ready " + session.LogDirectory); await Task.Delay(2000);
                await session.StopAsync(); Assert(!session.Running, "stopped");
                Assert(Process.GetProcessesByName("awdl_link").Length == 0 && Process.GetProcessesByName("devourer-bridge").Length == 0, "radio processes cleaned up");
                report.Add("Stop and cleanup passed.");
            }
            await session.StartAsync();
            var scanning = session.ScanAsync();
            await Task.Delay(500);
            await session.StopAsync();
            try { await scanning; } catch (OperationCanceledException) { }
            Assert(Process.GetProcessesByName("awdl_link").Length == 0 && Process.GetProcessesByName("devourer-bridge").Length == 0, "stop during discovery cleanup");
            report.Add("Stop during discovery passed.");
            return 0;
        }
        catch (Exception e) { report.Add(e.ToString()); return 1; }
        finally { report.Add("Session: " + session.LogDirectory); await session.StopAsync(); File.WriteAllLines(Path.Combine(AppContext.BaseDirectory, "lifecycle-test.txt"), report); }
    }
    public static async Task<int> Recovery()
    {
        var report = new List<string>(); var session = new Session();
        session.Log += line => { lock (report) report.Add(line); };
        int baseline = Process.GetProcessesByName("bleprobe").Length;
        try {
            await session.StartAsync();
            await session.RecoverAsync();
            Assert(File.ReadAllText(Path.Combine(session.LogDirectory, "recovery.log")).Contains("advertiser_pid="), "publisher ownership transferred");
            report.Add("Session: " + session.LogDirectory);
            await session.StopAsync();
            Assert(Process.GetProcessesByName("bleprobe").Length == baseline, "owned BLE publisher cleaned up");
            report.Add("PASS: Bluetooth publisher started, services restored, publisher adopted and cleaned up.");
            return 0;
        } catch (Exception e) { lock (report) report.Add(e.ToString()); return 1; }
        finally {
            await session.StopAsync();
            await session.WaitForRecoveryAsync().ContinueWith(_ => { });
            await session.StopAsync();
            lock (report) File.WriteAllLines(Path.Combine(AppContext.BaseDirectory, "recovery-test.txt"), report);
        }
    }

}
