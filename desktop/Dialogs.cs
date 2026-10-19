using System.Diagnostics;
using System.Drawing.Drawing2D;
using System.Media;

namespace PulseDrop;

// Original two-note chime synthesized at startup (not Apple's AirDrop sound).
static class Chime
{
    static readonly byte[] Wave = Build();
    static SoundPlayer? player;
    public static void Play()
    {
        try { player?.Dispose(); player = new SoundPlayer(new MemoryStream(Wave)); player.Play(); }
        catch (Exception e) when (e is InvalidOperationException or IOException) { }
    }
    static byte[] Build()
    {
        const int rate = 44100; int count = (int)(rate * 0.8);
        (double Freq, double Start)[] notes = [(1318.5, 0), (1760.0, 0.11)];
        using var stream = new MemoryStream(); using var w = new BinaryWriter(stream);
        w.Write("RIFF"u8); w.Write(36 + count * 2); w.Write("WAVEfmt "u8); w.Write(16); w.Write((short)1); w.Write((short)1);
        w.Write(rate); w.Write(rate * 2); w.Write((short)2); w.Write((short)16); w.Write("data"u8); w.Write(count * 2);
        for (int i = 0; i < count; i++)
        {
            double t = (double)i / rate, v = 0;
            foreach (var (freq, start) in notes)
            {
                double u = t - start; if (u < 0) continue;
                double env = Math.Min(1, u / 0.005) * Math.Exp(-u * 6.5);
                v += env * (Math.Sin(2 * Math.PI * freq * u) + 0.22 * Math.Sin(4 * Math.PI * freq * u));
            }
            w.Write((short)(Math.Clamp(v * 0.28, -1, 1) * short.MaxValue));
        }
        w.Flush(); return stream.ToArray();
    }
}

class DarkDialog : Form
{
    public DarkDialog(int width, int height)
    {
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96F, 96F); AutoScaleMode = AutoScaleMode.Dpi;
        FormBorderStyle = FormBorderStyle.None; ClientSize = new Size(width, height); StartPosition = FormStartPosition.CenterParent;
        BackColor = Theme.Surface; ForeColor = Theme.Text; Font = Theme.UI(10); ShowInTaskbar = false; KeyPreview = true; DoubleBuffered = true;
        Theme.Drag(this, this);
    }
    protected void Done() => ResumeLayout();
    protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); Theme.Chrome(this); }
    protected override void OnKeyDown(KeyEventArgs e) { if (e.KeyCode == Keys.Escape) { DialogResult = DialogResult.Cancel; Close(); } base.OnKeyDown(e); }
    protected static Label Caption(string text, Font font, Color color, Rectangle bounds, ContentAlignment align = ContentAlignment.MiddleCenter) =>
        new() { Text = text, Font = font, ForeColor = color, Bounds = bounds, TextAlign = align, AutoEllipsis = true, UseMnemonic = false };
    protected static Pill MakeButton(string text, bool primary, Rectangle bounds) =>
        new() { Text = text, Bounds = bounds, Fill = primary ? Theme.Blue : Theme.Raised, Ink = primary ? Color.White : Theme.Text };
}

sealed class ConsentDialog : DarkDialog
{
    public readonly string Id;
    public ConsentDialog(Ask ask) : base(380, 0)
    {
        Id = ask.Id; TopMost = true;
        var shown = ask.Files.Take(4).ToList();
        if (ask.Files.Length > 4) shown.Add($"and {ask.Files.Length - 4} more");
        int listHeight = shown.Count * 24 + 20, y = 168 + listHeight + 20;
        ClientSize = new Size(380, y + 44 + 24);
        var icon = new AppIcon { Bounds = new Rectangle(162, 24, 56, 56), Corner = 28 };
        string size = ask.Bytes < 1 << 20 ? $"{ask.Bytes / 1024.0:0.#} KB" : $"{ask.Bytes / 1048576.0:0.#} MB";
        var list = new Surface { Bounds = new Rectangle(24, 168, 332, listHeight), BackColor = Theme.Well, Radius = 12 };
        for (int i = 0; i < shown.Count; i++)
            list.Controls.Add(Caption(shown[i], Theme.UI(9.5f), i < 4 ? Theme.Text : Theme.Secondary, new Rectangle(14, 10 + i * 24, 304, 24), ContentAlignment.MiddleLeft));
        var decline = MakeButton("Decline", false, new Rectangle(24, y, 160, 44));
        var accept = MakeButton("Accept", true, new Rectangle(196, y, 160, 44));
        decline.Click += (_, _) => { DialogResult = DialogResult.Cancel; Close(); };
        accept.Click += (_, _) => { DialogResult = DialogResult.OK; Close(); };
        Controls.AddRange([icon,
            Caption(ask.Sender, Theme.Semibold(14), Theme.Text, new Rectangle(20, 92, 340, 32)),
            Caption($"wants to share {(ask.Files.Length == 1 ? "a file" : ask.Files.Length + " files")} · {size}", Theme.UI(9.5f), Theme.Secondary, new Rectangle(20, 124, 340, 24)),
            list, decline, accept]);
        AcceptButton = accept; CancelButton = decline;
        Done();
    }
    protected override void OnShown(EventArgs e) { base.OnShown(e); Activate(); }
}

sealed class ProfileDialog : DarkDialog
{
    public ProfileDialog(Session session) : base(380, 392)
    {
        string model = session.Model;
        var name = new TextBox { Text = session.Name, MaxLength = 40, BorderStyle = BorderStyle.None, BackColor = Theme.Well, ForeColor = Theme.Text, Font = Theme.UI(11), Bounds = new Rectangle(14, 11, 304, 26), AccessibleName = "AirDrop name" };
        var field = new Surface { Bounds = new Rectangle(24, 96, 332, 46), BackColor = Theme.Well, Radius = 12, Cursor = Cursors.IBeam };
        field.Controls.Add(name); field.Click += (_, _) => name.Focus();
        var choices = Session.Models.Select((m, i) => new Pill { Text = m.Label, Tag = m.Model, Radius = 12, Font = Theme.UI(10), Bounds = new Rectangle(24 + i % 2 * 172, 184 + i / 2 * 52, 160, 44) }).ToArray();
        void Pick(string value)
        {
            model = value;
            foreach (var c in choices) { bool on = (string)c.Tag! == value; c.Fill = on ? Theme.Blue : Theme.Raised; c.Ink = on ? Color.White : Theme.Text; c.Invalidate(); }
        }
        foreach (var c in choices) c.Click += (_, _) => Pick((string)c.Tag!);
        Pick(model);
        var error = Caption("", Theme.UI(9), Theme.Red, new Rectangle(24, 142, 332, 20), ContentAlignment.MiddleLeft);
        var cancel = MakeButton("Cancel", false, new Rectangle(24, 324, 160, 44));
        var save = MakeButton("Save", true, new Rectangle(196, 324, 160, 44));
        cancel.Click += (_, _) => Close();
        save.Click += (_, _) =>
        {
            try { session.SaveProfile(name.Text, model); DialogResult = DialogResult.OK; Close(); }
            catch (ArgumentException ex) { error.Text = ex.Message; }
        };
        Controls.AddRange([
            Caption("Name & icon", Theme.Semibold(14), Theme.Text, new Rectangle(24, 20, 332, 32), ContentAlignment.MiddleLeft),
            Caption("How this PC appears in AirDrop", Theme.UI(9.5f), Theme.Secondary, new Rectangle(24, 52, 332, 22), ContentAlignment.MiddleLeft),
            field, error,
            Caption("Device icon", Theme.UI(9.5f), Theme.Secondary, new Rectangle(24, 158, 332, 22), ContentAlignment.MiddleLeft),
            .. choices,
            Caption(session.Running ? "Applies the next time you start receiving." : "Custom photos need an Apple contact card.", Theme.UI(9), Theme.Secondary, new Rectangle(24, 290, 332, 22), ContentAlignment.MiddleLeft),
            cancel, save]);
        AcceptButton = save; CancelButton = cancel;
        Done();
    }
}

sealed class ActivityWindow : DarkDialog
{
    public ActivityWindow(TextBox activity) : base(520, 440)
    {
        StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false;
        var close = new IconButton { Glyph = "", Font = Theme.Icons(10), Bounds = new Rectangle(474, 14, 34, 34), AccessibleName = "Close" };
        close.Click += (_, _) => Hide();
        var box = new Surface { Bounds = new Rectangle(16, 60, 488, 364), BackColor = Theme.Well, Radius = 12 };
        activity.Bounds = new Rectangle(14, 12, 464, 340); activity.BackColor = Theme.Well;
        box.Controls.Add(activity);
        Controls.AddRange([Caption("Activity", Theme.Semibold(14), Theme.Text, new Rectangle(24, 14, 300, 34), ContentAlignment.MiddleLeft), close, box]);
        Done();
    }
    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (e.CloseReason == CloseReason.UserClosing) { e.Cancel = true; Hide(); }
        base.OnFormClosing(e);
    }
    protected override void OnKeyDown(KeyEventArgs e) { if (e.KeyCode == Keys.Escape) Hide(); else base.OnKeyDown(e); }
}

// A received file. Clicking reveals it in Explorer rather than opening it: this app runs
// elevated, and opening an untrusted file from here would run its handler as administrator.
sealed class RecentRow : Surface
{
    readonly Image? thumb;
    public RecentRow(FileInfo file)
    {
        Size = new Size(432, 60); Cursor = Cursors.Hand; AccessibleName = "Show " + file.Name + " in folder"; AccessibleRole = AccessibleRole.PushButton;
        if (file.Extension.ToLowerInvariant() is ".jpg" or ".jpeg" or ".png" or ".gif" or ".bmp")
            try { using var source = Image.FromStream(new MemoryStream(File.ReadAllBytes(file.FullName))); thumb = new Bitmap(source, Fit(source.Size, 80)); }
            catch (Exception e) when (e is ArgumentException or IOException or OutOfMemoryException or UnauthorizedAccessException) { }
        string size = file.Length < 1 << 20 ? $"{file.Length / 1024.0:0.#} KB" : $"{file.Length / 1048576.0:0.#} MB";
        var name = new Label { Text = file.Name, Font = Theme.Semibold(10), ForeColor = Theme.Text, AutoEllipsis = true, Bounds = new Rectangle(64, 10, 310, 22) };
        var sub = new Label { Text = size + " · " + file.LastWriteTime.ToString("MMM d, h:mm tt"), Font = Theme.UI(8.5f), ForeColor = Theme.Secondary, Bounds = new Rectangle(64, 31, 310, 20) };
        var reveal = new IconButton { Glyph = "", Bounds = new Rectangle(386, 13, 34, 34), AccessibleName = "Show in folder" };
        Controls.AddRange([name, sub, reveal]);
        void Reveal(object? s, EventArgs e) => Process.Start(new ProcessStartInfo("explorer.exe") { ArgumentList = { "/select,", file.FullName } });
        foreach (var c in new Control[] { this, name, sub, reveal }) c.Click += Reveal;
    }
    static Size Fit(Size s, int max) => s.Width >= s.Height ? new Size(max, Math.Max(1, s.Height * max / s.Width)) : new Size(Math.Max(1, s.Width * max / s.Height), max);
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; float k = this.K(); g.SmoothingMode = SmoothingMode.AntiAlias;
        var box = new RectangleF(10 * k, 10 * k, 40 * k, 40 * k);
        using var path = Theme.Round(box, 9 * k);
        using (var brush = new SolidBrush(Theme.Raised)) g.FillPath(brush, path);
        if (thumb != null)
        {
            g.SetClip(path); g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            float scale = Math.Max(box.Width / thumb.Width, box.Height / thumb.Height);
            float w = thumb.Width * scale, h = thumb.Height * scale;
            g.DrawImage(thumb, box.X + (box.Width - w) / 2, box.Y + (box.Height - h) / 2, w, h); g.ResetClip();
        }
        else { using var icons = Theme.Icons(14); TextRenderer.DrawText(g, "", icons, Rectangle.Round(box), Theme.Secondary, Theme.Center); }
    }
    protected override void Dispose(bool disposing) { if (disposing) thumb?.Dispose(); base.Dispose(disposing); }
}
