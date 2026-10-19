using System.Diagnostics;

namespace PulseDrop;

sealed class MainForm : Form
{
    readonly Session session;
    readonly Label adapter;
    readonly Label stateText = new(), sendHint = new(), emptyText = new(), recvTitle = new(), recvDetail = new(), steps = new(), noRecent = new();
    readonly Dot dot = new();
    readonly AppIcon radar = new() { Corner = 32 };
    readonly Switch toggle = new();
    readonly Bar bar = new() { Visible = false };
    readonly System.Windows.Forms.Timer flashTimer = new() { Interval = 5000 };
    readonly System.Windows.Forms.Timer stallTimer = new() { Interval = 5000 };
    DateTime lastProgress;
    string? incoming, flash;
    long inReceived, inTotal;
    readonly Pill sendTab = new(), receiveTab = new(), turnOn = new(), showAll = new();
    readonly IconButton refresh = new(), edit = new(), gear = new(), minimize = new(), close = new(), help = new();
    readonly DropZone drop = new();
    readonly Panel sendPage = new(), receivePage = new(), list = new(), empty = new(), recent = new();
    readonly TextBox activity = new();
    readonly ActivityWindow activityWindow;
    readonly ContextMenuStrip menu = new() { Renderer = new ToolStripProfessionalRenderer(new DarkMenuColors()) };
    readonly ToolStripMenuItem recoverItem = new("Bluetooth recovery…"), stopItem = new("Stop receiving");
    readonly ToolStripMenuItem fasterItem = new("Faster sending (experimental)") { CheckOnClick = true };
    readonly ToolStripMenuItem matchItem = new("Match iPhone's Wi-Fi channel") { CheckOnClick = true };
    static readonly TimeSpan LostGrace = TimeSpan.FromSeconds(60);
    readonly List<DeviceRow> rows = [];
    readonly HashSet<string> known = [];
    ConsentDialog? consent;
    CancellationTokenSource? sendCancel;
    DeviceRow? sendingRow;
    string? selectedFile;
    DateTime endsAt;
    bool busy, closing, closed, wasRunning, demo;

    public MainForm(bool preview = false)
    {
        demo = preview;
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96F, 96F); AutoScaleMode = AutoScaleMode.Dpi;
        Text = "PulseDrop"; FormBorderStyle = FormBorderStyle.None; ClientSize = new Size(480, 720);
        using (var iconStream = typeof(MainForm).Assembly.GetManifestResourceStream("PulseDrop.Icon")!) Icon = new Icon(iconStream);
        StartPosition = FormStartPosition.CenterScreen; BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.UI(10); DoubleBuffered = true;

        // Header and window chrome.
        var icon = new AppIcon { Bounds = new Rectangle(24, 20, 56, 56) };
        var title = new Label { Text = "PulseDrop", Font = new Font("Segoe UI", 16, FontStyle.Bold), Bounds = new Rectangle(94, 18, 220, 34) };
        dot.Bounds = new Rectangle(96, 59, 8, 8);
        stateText.Bounds = new Rectangle(108, 52, 210, 22); stateText.Font = Theme.UI(9.5f); stateText.ForeColor = Theme.Secondary; stateText.AutoEllipsis = true;
        Glyph(edit, "", "Edit AirDrop name and icon", new Rectangle(320, 26, 34, 34));
        Glyph(gear, "", "Settings", new Rectangle(358, 26, 34, 34));
        Glyph(minimize, "", "Minimize", new Rectangle(396, 26, 34, 34));
        Glyph(close, "", "Close", new Rectangle(430, 26, 34, 34));
        minimize.Font = close.Font = Theme.Icons(10);
        Controls.AddRange([icon, title, dot, stateText, edit, gear, minimize, close]);
        foreach (var c in new Control[] { this, icon, title, stateText }) Theme.Drag(this, c);

        var tabs = new Surface { Bounds = new Rectangle(24, 96, 432, 54), Radius = 16 };
        Tab(sendTab, "Send", "", new Rectangle(4, 4, 212, 46)); Tab(receiveTab, "Receive", "", new Rectangle(216, 4, 212, 46));
        tabs.Controls.AddRange([sendTab, receiveTab]); Controls.Add(tabs);

        // Send page.
        sendPage.Bounds = receivePage.Bounds = new Rectangle(0, 166, 480, 490);
        drop.Bounds = new Rectangle(24, 0, 432, 172); drop.AccessibleName = "Choose a file to send";
        ResetDrop();
        var nearby = new Label { Text = "Nearby Apple Devices", Font = Theme.Semibold(11), Bounds = new Rectangle(28, 190, 300, 26), TextAlign = ContentAlignment.MiddleLeft };
        Glyph(refresh, "", "Find nearby devices", new Rectangle(422, 186, 34, 34)); refresh.Ink = Theme.Blue;
        list.Bounds = empty.Bounds = new Rectangle(24, 226, 432, 236); list.AutoScroll = true; Theme.DarkScroll(list);
        emptyText.Bounds = new Rectangle(16, 34, 400, 64); emptyText.TextAlign = ContentAlignment.MiddleCenter; emptyText.ForeColor = Theme.Secondary; emptyText.Font = Theme.UI(9.5f);
        turnOn.Text = "Turn on AirDrop"; turnOn.Bounds = new Rectangle(126, 112, 180, 44);
        empty.Controls.AddRange([emptyText, turnOn]);
        sendHint.Bounds = new Rectangle(28, 464, 424, 24); sendHint.ForeColor = Theme.Secondary; sendHint.Font = Theme.UI(9); sendHint.AutoEllipsis = true;
        sendPage.Controls.AddRange([drop, nearby, refresh, empty, list, sendHint]);

        // Receive page.
        var card = new Surface { Bounds = new Rectangle(24, 0, 432, 112), Radius = 18 };
        radar.Bounds = new Rectangle(18, 22, 64, 64);
        recvTitle.Bounds = new Rectangle(98, 22, 250, 30); recvTitle.Font = Theme.Semibold(14);
        recvDetail.Bounds = new Rectangle(98, 52, 250, 40); recvDetail.ForeColor = Theme.Secondary; recvDetail.Font = Theme.UI(9.5f);
        toggle.Bounds = new Rectangle(360, 40, 54, 32); toggle.AccessibleName = "Receiving";
        bar.Bounds = new Rectangle(98, 84, 316, 6);
        card.Controls.AddRange([radar, recvTitle, recvDetail, toggle, bar]);
        steps.Bounds = new Rectangle(28, 120, 424, 44); steps.ForeColor = Theme.Secondary; steps.Font = Theme.UI(9.5f);
        var recentTitle = new Label { Text = "Recent", Font = Theme.Semibold(11), Bounds = new Rectangle(28, 176, 200, 26), TextAlign = ContentAlignment.MiddleLeft };
        showAll.Text = "Show all"; showAll.Bounds = new Rectangle(366, 174, 90, 30); showAll.Fill = Theme.Raised; showAll.Ink = Theme.Text; showAll.Radius = 10; showAll.Font = Theme.UI(9);
        recent.Bounds = new Rectangle(24, 212, 432, 272);
        noRecent.Text = "Files you receive appear here."; noRecent.Bounds = new Rectangle(0, 40, 432, 40); noRecent.TextAlign = ContentAlignment.MiddleCenter; noRecent.ForeColor = Theme.Secondary; noRecent.Font = Theme.UI(9.5f);
        recent.Controls.Add(noRecent);
        receivePage.Controls.AddRange([card, steps, recentTitle, showAll, recent]);
        Controls.AddRange([sendPage, receivePage]);

        // Footer.
        var rule = new Panel { Bounds = new Rectangle(24, 664, 432, 1), BackColor = Color.FromArgb(38, 45, 60) };
        var antenna = new Label { Text = "", Font = Theme.Icons(12), ForeColor = Theme.Secondary, Bounds = new Rectangle(24, 676, 28, 32), TextAlign = ContentAlignment.MiddleCenter };
        adapter = new Label { Text = "Using TP-Link Archer T3U", Font = Theme.UI(9.5f), ForeColor = Theme.Secondary, Bounds = new Rectangle(56, 676, 300, 32), TextAlign = ContentAlignment.MiddleLeft };
        Glyph(help, "", "Help", new Rectangle(422, 675, 34, 34));
        Controls.AddRange([rule, antenna, adapter, help]);

        var profileItem = new ToolStripMenuItem("Name && icon…"); var openFiles = new ToolStripMenuItem("Open received files");
        var exportLogs = new ToolStripMenuItem("Export diagnostics…"); var openLogs = new ToolStripMenuItem("Open logs"); var activityItem = new ToolStripMenuItem("Activity");
        menu.Items.AddRange([profileItem, new ToolStripSeparator(), openFiles, activityItem, openLogs, exportLogs, new ToolStripSeparator(), matchItem, fasterItem, recoverItem, stopItem]);
        foreach (ToolStripItem item in menu.Items) item.ForeColor = Theme.Text;

        activity.Multiline = true; activity.ReadOnly = true; activity.ScrollBars = ScrollBars.Vertical; activity.BorderStyle = BorderStyle.None;
        activity.ForeColor = Theme.Text; activity.Font = Theme.UI(9); activity.AccessibleName = "Transfer activity"; Theme.DarkScroll(activity);
        activityWindow = new ActivityWindow(activity);
        ResumeLayout();

        try { session = new Session(); }
        catch (Exception e) { throw new IOException("PulseDrop setup is incomplete. Run tools/package-desktop.ps1 first.", e); }
        fasterItem.Checked = session.FasterSending;
        fasterItem.Click += (_, _) => {
            try {
                session.SetFasterSending(fasterItem.Checked);
                sendHint.Text = fasterItem.Checked ? "Faster sending enabled. Switch off if transfers become unreliable." : "Compatible sending speed enabled.";
            } catch (Exception e) { fasterItem.Checked = session.FasterSending; MessageBox.Show(this, e.Message, "PulseDrop"); }
        };
        matchItem.Checked = session.MatchChannel;
        matchItem.Click += (_, _) => {
            try {
                session.SetMatchChannel(matchItem.Checked);
                sendHint.Text = matchItem.Checked ? "The radio will follow the iPhone between channels 149 and 36." : "The radio will stay on channel 149.";
            } catch (Exception e) { matchItem.Checked = session.MatchChannel; MessageBox.Show(this, e.Message, "PulseDrop"); }
        };
        session.Log += line => UI(() => Append(line)); session.Changed += () => UI(UpdateControls);
        session.DeviceFound += device => {
            int owner = session.Generation;
            UI(() => { if (session.Running && session.Generation == owner) AddDevice(device); });
        };
        session.DeviceRemoved += address => {
            int owner = session.Generation;
            UI(() => { if (session.Generation == owner) RemoveDevice(address); });
        };
        session.SendProgress += (sent, total) => UI(() => { if (sendingRow != null) sendHint.Text = $"Sending to {sendingRow.Device.Name} · {Bytes(sent)} of {Bytes(total)}"; });
        session.SendStatus += text => UI(() => { if (sendingRow != null) sendHint.Text = text; });
        session.AskReceived += ask => UI(() => ShowAsk(ask));
        session.AskExpired += id => UI(() => { if (consent?.Id == id) consent.Close(); });
        session.Saved += _ => UI(() => { Chime.Play(); Finish("Received " + (incoming ?? "a file")); RefreshRecent(); });
        session.ReceiveFailed += reason => UI(() => { if (incoming != null) Finish(reason); });
        session.Progress += (name, received, total) => UI(() => { lastProgress = DateTime.Now; incoming = name; inReceived = received; inTotal = total; flash = null; UpdateControls(); });
        flashTimer.Tick += (_, _) => { flashTimer.Stop(); flash = null; UpdateControls(); };
        // The sender may cancel after approval without telling us; the receiver gives up after 120 s.
        stallTimer.Tick += (_, _) => {
            if (incoming != null && !demo && DateTime.Now - lastProgress > TimeSpan.FromSeconds(130)) Finish("Transfer didn't finish");
            foreach (var row in rows.Where(r => r != sendingRow && DateTime.Now - r.LostAt > LostGrace).ToArray()) DropRow(row);
        };
        stallTimer.Start();

        sendTab.Click += (_, _) => ShowTab(true); receiveTab.Click += (_, _) => ShowTab(false);
        gear.Click += (_, _) => menu.Show(gear, new Point(0, gear.Height));
        edit.Click += (_, _) => EditProfile(); profileItem.Click += (_, _) => EditProfile();
        minimize.Click += (_, _) => WindowState = FormWindowState.Minimized;
        close.Click += (_, _) => Close();
        help.Click += (_, _) => MessageBox.Show(this,
            "Receive: turn on receiving. On the iPhone choose Share → AirDrop → " + session.Name + ", then accept the request on this PC.\n\n" +
            "Send: unlock the iPhone, set AirDrop to Everyone for 10 Minutes, tap refresh, then press Send next to the device. One file up to 1 GiB.\n\n" +
            "If no device appears, try Bluetooth recovery from the settings menu.", "PulseDrop help");
        toggle.Click += async (_, _) => await Toggle();
        turnOn.Click += async (_, _) => await Toggle();
        refresh.Click += async (_, _) => await Work(async () => { ClearDevices(); sendHint.Text = "Looking for nearby devices…"; await session.WakeAsync(); if (sendCancel == null) sendHint.Text = rows.Count > 0 ? "Press Send next to a device." : "No device yet. Refresh Everyone mode on the iPhone, or try Bluetooth recovery."; });
        drop.Click += (_, _) => Choose();
        drop.FilesDropped += paths =>
        {
            var file = paths.FirstOrDefault(File.Exists);
            if (file == null) { sendHint.Text = "Drop a file, not a folder."; return; }
            SetFile(file);
            if (paths.Length > 1) sendHint.Text = "One file at a time. Using " + Path.GetFileName(file) + ".";
        };
        showAll.Click += (_, _) => OpenDownloads(); openFiles.Click += (_, _) => OpenDownloads();
        openLogs.Click += (_, _) => { if (session.LogDirectory.Length > 0) Open(session.LogDirectory); else Append("Start a session to create its logs."); };
        exportLogs.Click += (_, _) => ExportLogs();
        activityItem.Click += (_, _) => { if (!activityWindow.Visible) activityWindow.Show(this); activityWindow.Activate(); };
        stopItem.Click += async (_, _) => await Toggle();
        recoverItem.Click += async (_, _) => await Work(async () => { await RecoverPrompt(); });
        FormClosing += async (_, e) =>
        {
            if (closed) return;
            e.Cancel = true;
            if (closing) return;
            if (e.CloseReason == CloseReason.UserClosing && !ConfirmInterrupt("Closing PulseDrop")) return;
            closing = true; Enabled = false;
            await Task.Yield();
            try { sendCancel?.Cancel(); await session.StopAsync(); await session.WaitForRecoveryAsync(); await session.StopAsync(); }
            catch (Exception error) { MessageBox.Show(error.Message, "Shutdown status"); }
            closed = true; Close();
        };
        Append("Plug in the tested TP-Link adapter, then start receiving. Incoming transfers require your approval.");
        ShowTab(true);
        RefreshRecent();
        UpdateControls();
    }

    protected override CreateParams CreateParams
    {
        // WS_MINIMIZEBOX lets the taskbar button minimize/restore the borderless window.
        get { var p = base.CreateParams; p.Style |= 0x20000; return p; }
    }
    protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); Theme.Chrome(this); }

    static void Glyph(IconButton button, string glyph, string name, Rectangle bounds) { button.Glyph = glyph; button.AccessibleName = name; button.Bounds = bounds; }
    static void Tab(Pill tab, string text, string glyph, Rectangle bounds) { tab.Text = text; tab.Glyph = glyph; tab.Bounds = bounds; tab.Radius = 13; tab.Font = Theme.UI(11); }

    void ShowTab(bool send)
    {
        sendPage.Visible = send; receivePage.Visible = !send;
        foreach (var (tab, on) in new[] { (sendTab, send), (receiveTab, !send) })
        { tab.Fill = on ? Theme.Blue : Theme.Surface; tab.Ink = on ? Color.White : Theme.Secondary; tab.Invalidate(); }
    }
    void ShowAsk(Ask ask)
    {
        if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
        Activate(); Chime.Play();
        bool accept;
        using (var dialog = consent = new ConsentDialog(ask)) accept = dialog.ShowDialog(this) == DialogResult.OK;
        consent = null;
        session.Answer(ask.Id, accept);
        if (accept) { lastProgress = DateTime.Now; incoming = ask.Files.Length == 1 ? ask.Files[0] : ask.Files.Length + " files"; inReceived = 0; inTotal = ask.Bytes; flash = null; ShowTab(false); UpdateControls(); }
        Append((accept ? "Accepted " : "Declined ") + (ask.Files.Length == 1 ? ask.Files[0] : ask.Files.Length + " files") + " from " + ask.Sender + ".");
    }
    void Finish(string message) { incoming = null; flash = message; flashTimer.Stop(); flashTimer.Start(); UpdateControls(); }
    static string Bytes(long bytes) => bytes < 1 << 20 ? $"{bytes / 1024.0:0.#} KB" : $"{bytes / 1048576.0:0.#} MB";
    void EditProfile()
    {
        using var dialog = new ProfileDialog(session);
        if (dialog.ShowDialog(this) == DialogResult.OK) Append("AirDrop name is now " + session.Name + (session.Running ? ". Restart receiving to apply it." : "."));
    }
    void RefreshRecent()
    {
        foreach (var row in recent.Controls.OfType<RecentRow>().ToArray()) row.Dispose();
        if (demo) { noRecent.Visible = true; return; }
        var files = Directory.Exists(session.Downloads)
            ? new DirectoryInfo(session.Downloads).EnumerateDirectories().Where(d => !d.Name.StartsWith(".incoming-"))
                .SelectMany(d => d.EnumerateFiles()).OrderByDescending(f => f.LastWriteTimeUtc).Take(4).ToArray()
            : [];
        float k = this.K();
        for (int i = 0; i < files.Length; i++)
        {
            var row = new RecentRow(files[i]) { Location = new Point(0, i * 68) };
            row.Scale(new SizeF(k, k)); // created after the form's DPI scaling pass
            recent.Controls.Add(row);
        }
        noRecent.Visible = files.Length == 0;
    }
    void ResetDrop() { drop.Glyph = ""; drop.GlyphColor = Theme.Secondary; drop.Title = "Drop a file here"; drop.Sub = "or click to choose · up to 1 GiB"; drop.Invalidate(); }
    void Choose()
    {
        using var dialog = new OpenFileDialog { Title = "Send one file (up to 1 GiB)", CheckFileExists = true };
        if (dialog.ShowDialog(this) == DialogResult.OK) SetFile(dialog.FileName);
    }
    void SetFile(string path)
    {
        selectedFile = path;
        long bytes = new FileInfo(path).Length;
        drop.GlyphColor = Theme.Blue; drop.Title = Path.GetFileName(path);
        drop.Sub = (bytes < 1 << 20 ? $"{bytes / 1024.0:0.#} KB" : $"{bytes / 1048576.0:0.#} MB") + " · click to change";
        drop.Invalidate(); UpdateControls();
    }
    async Task Toggle()
    {
        if (session.Running && !ConfirmInterrupt("Stopping receiving")) return;
        await Work(ToggleCore);
    }
    async Task ToggleCore()
    {
        bool stopping = session.Running;
        sendCancel?.Cancel();
        ClearDevices();
        if (stopping) await session.StopAsync(); else await session.StartAsync();
    }
    // Sending runs outside Work so the rest of the window stays usable; the row's button becomes Cancel.
    async Task SendTo(DeviceRow row)
    {
        if (sendCancel != null) { if (row == sendingRow) sendCancel.Cancel(); return; }
        if (selectedFile == null) { Choose(); if (selectedFile == null) return; }
        string file = selectedFile, name = row.Device.Name;
        sendCancel = new CancellationTokenSource(); sendingRow = row; UpdateControls();
        try
        {
            if (row.LostAt != null || DateTime.UtcNow - row.VerifiedAt > TimeSpan.FromSeconds(10))
            {
                var fresh = await Reacquire(row, sendCancel.Token);
                if (fresh == null) { sendHint.Text = "Couldn't reach " + name + ". Unlock it, set AirDrop to Everyone for 10 Minutes, then try again."; return; }
                sendingRow = row = fresh; UpdateControls();
            }
            sendHint.Text = "Waiting for " + name + " to accept " + Path.GetFileName(file) + "…";
            await session.SendAsync(row.Device, file, sendCancel.Token);
            sendHint.Text = name + " accepted " + Path.GetFileName(file) + ".";
            Chime.Play();
        }
        catch (BluetoothBusyException) { await RecoverPrompt(); }
        catch (OperationCanceledException) { sendHint.Text = "Sending cancelled."; Append("Sending to " + name + " cancelled."); }
        catch (Exception e) { sendHint.Text = e.Message; Append(e.Message); }
        finally { sendCancel.Dispose(); sendCancel = null; sendingRow = null; if (!IsDisposed) UpdateControls(); }
    }
    void ExportLogs()
    {
        if (session.LogDirectory.Length == 0) { Append("Start a session to create its diagnostics."); return; }
        using var dialog = new SaveFileDialog { Title = "Export diagnostic logs", Filter = "Diagnostic ZIP (*.zip)|*.zip",
            FileName = "PulseDrop-diagnostics-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".zip", OverwritePrompt = false };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        try { DiagnosticExport.Write(session.LogDirectory, dialog.FileName); Append("Diagnostics saved to " + dialog.FileName); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { MessageBox.Show(this, "Could not export diagnostics. Choose a new filename.\n\n" + e.Message, "PulseDrop"); }
    }

    void OpenDownloads() { Directory.CreateDirectory(session.Downloads); Open(session.Downloads); }

    void UI(Action action) { if (IsDisposed || Disposing) return; if (InvokeRequired) { try { BeginInvoke(action); } catch (InvalidOperationException) { } } else action(); }
    void Append(string text) { if (activity.TextLength > 24000) activity.Text = activity.Text[^12000..]; activity.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + text + Environment.NewLine); }
    void UpdateControls()
    {
        bool running = session.Running, working = session.Working;
        adapter.Text = $"T3U · {(session.MatchChannel ? "Auto" : "149")} · {(session.FasterSending ? 24 : 12)} Mbit/s PHY";
        fasterItem.Enabled = matchItem.Enabled = !running && !working && !busy;
        if (running && !wasRunning) endsAt = DateTime.Now.AddHours(1);
        wasRunning = running;
        dot.Color = running ? Theme.Green : working ? Theme.Orange : Theme.Off; dot.Invalidate();
        string shown = running ? session.ActiveName : session.Name;
        stateText.Text = running ? "Visible as " + shown : session.Status;
        radar.On = running; radar.Invalidate();
        recvTitle.Text = running ? "Receiving is on" : working ? session.Status : "Receiving is off";
        recvDetail.Text = running ? $"Visible as {shown} until {endsAt:h:mm tt}" : "Turn on to appear in AirDrop. The TP-Link adapter must be plugged in.";
        if (!running && !demo) incoming = null;
        bar.Visible = incoming != null; recvDetail.Height = (int)((incoming != null ? 26 : 40) * this.K());
        if (incoming != null)
        {
            // Received bytes are the compressed archive, so hold at 99% until the file is saved.
            double done = inTotal > 0 ? Math.Min(0.99, (double)inReceived / inTotal) : 0;
            recvTitle.Text = "Receiving…";
            recvDetail.Text = $"{incoming} · {Bytes(Math.Min(inReceived, inTotal))} of {Bytes(inTotal)}";
            bar.Value = done; bar.Invalidate();
            stateText.Text = $"Receiving {incoming} · {done:P0}"; dot.Color = Theme.Blue;
        }
        else if (flash != null && running) recvTitle.Text = flash;
        steps.Text = "On iPhone: Share → AirDrop → " + shown + ", then accept the request here. Use Everyone for 10 Minutes on the iPhone if this PC isn't listed.";
        toggle.On = running; toggle.Enabled = stopItem.Enabled = running ? !working : !busy && !working; toggle.Invalidate();
        stopItem.Visible = running;
        turnOn.Enabled = !busy && !working; turnOn.Visible = !running;
        refresh.Enabled = recoverItem.Enabled = !busy && running && sendCancel == null;
        empty.Visible = rows.Count == 0; list.Visible = rows.Count > 0;
        emptyText.Text = running ? "Looking for devices…\nUnlock the iPhone and set AirDrop to Everyone for 10 Minutes, then tap refresh."
            : "AirDrop is off.\nTurn it on to find nearby Apple devices.";
        for (int i = 0; i < rows.Count; i++)
        {
            var send = rows[i].Send; bool mine = rows[i] == sendingRow;
            send.Text = mine ? "Cancel" : "Send";
            // Let a verified device accept immediately while the remaining scan continues.
            send.Enabled = running && (mine || ((!busy || session.Scanning) && sendingRow == null));
            send.Fill = mine ? Color.FromArgb(64, 32, 38) : i == 0 && selectedFile != null && sendingRow == null ? Theme.Blue : Theme.Raised;
            send.Ink = mine ? Theme.Red : Theme.Text; send.Invalidate();
        }
        foreach (Control c in new Control[] { turnOn, refresh }) c.Invalidate();
    }
    async Task<bool> RecoverPrompt()
    {
        if (MessageBox.Show(this, "Windows blocked the AirDrop Bluetooth signal. Recover now?\n\nThis first pauses Windows connected-device features. Only if that isn't enough does it switch Bluetooth off and on, which briefly disconnects accessories. Windows service states are restored afterward.", "Recover Bluetooth advertising", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return false;
        try {
            sendHint.Text = "Recovering Bluetooth and restoring services…";
            await session.RecoverAsync();
            if (sendCancel == null) sendHint.Text = rows.Count > 0 ? "Press Send next to a device." : "Recovery finished. Refresh Everyone mode on the iPhone.";
            return true;
        } catch (Exception e) { Append(e.Message); MessageBox.Show(this, e.Message, "PulseDrop"); return false; }
    }
    async Task Work(Func<Task> action)
    {
        busy = true; UpdateControls();
        try { await action(); }
        catch (BluetoothBusyException) { await RecoverPrompt(); }
        catch (OperationCanceledException) { Append("Operation cancelled."); }
        catch (Exception e) { Append(e.Message); MessageBox.Show(this, e.Message, "PulseDrop", MessageBoxButtons.OK, MessageBoxIcon.Information); }
        finally { busy = false; if (!IsDisposed) UpdateControls(); }
    }
    void AddDevice(Device device)
    {
        if (!known.Add(device.Address + ":" + device.Port))
        {
            foreach (var r in rows.Where(r => r.Device.Address == device.Address && r.Device.Port == device.Port)) r.Verified();
            return;
        }
        var row = new DeviceRow(device) { Location = new Point(0, rows.Count * 80) };
        row.Scale(new SizeF(this.K(), this.K())); // created after the form's DPI scaling pass
        row.Send.Click += async (_, _) => await SendTo(row);
        rows.Add(row); list.Controls.Add(row);
        Append("Found " + device.Name);
        UpdateControls();
    }
    // The radio forgets an idle phone within seconds; keep its row (marked) so Send can wake it.
    void RemoveDevice(string address)
    {
        foreach (var row in rows.Where(row => row.Device.Address == address)) row.SetLost(true);
    }
    void DropRow(DeviceRow row)
    {
        known.Remove(row.Device.Address + ":" + row.Device.Port);
        list.Controls.Remove(row); rows.Remove(row); row.Dispose();
        for (int i = 0; i < rows.Count; i++) rows[i].Top = (int)(i * 80 * this.K());
        UpdateControls();
    }
    // Wake an out-of-range phone and wait for it to answer again; returns the row to send to, or null.
    async Task<DeviceRow?> Reacquire(DeviceRow row, CancellationToken token)
    {
        string name = row.Device.Name;
        sendHint.Text = "Waking " + name + "… keep it unlocked with AirDrop set to Everyone.";
        if (session.Scanning) await session.ScanAsync().WaitAsync(token);
        var requested = DateTime.UtcNow;
        Task scan = session.WakeAsync();
        DeviceRow? Fresh() => rows.FirstOrDefault(r => r.VerifiedAt >= requested && r.Device.Address == row.Device.Address && r.Device.Port == row.Device.Port);
        while (!scan.IsCompleted && Fresh() == null) await Task.WhenAny(scan, Task.Delay(250, token));
        token.ThrowIfCancellationRequested();
        if (scan.IsFaulted) await scan; // surfaces BluetoothBusyException for the recovery prompt
        return Fresh();
    }
    bool Transferring => !demo && (incoming != null || sendCancel != null);
    bool ConfirmInterrupt(string action) => !Transferring || MessageBox.Show(this,
        $"A transfer is in progress. {action} now will cancel it.\n\nContinue?", "PulseDrop", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes;
    void ClearDevices()
    {
        known.Clear();
        foreach (var row in rows) row.Dispose();
        rows.Clear(); UpdateControls();
    }
    // Preview-only: populate a device row, or switch to the Receive tab.
    internal void Demo(Device? device) { if (device != null) AddDevice(device); else ShowTab(false); }
    internal void DemoProgress() { demo = true; incoming = "Example video.mp4"; inReceived = 3_100_000; inTotal = 6_846_469; UpdateControls(); }
    internal void DemoAsk() => BeginInvoke(() => ShowAsk(new Ask("demo", "Example iPhone", ["Photo 1.HEIC", "Photo 2.HEIC"], 3_400_000)));
    static void Open(string path) => Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
}
