using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Text;

namespace PulseDrop;

static class Theme
{
    public static readonly Color Back = Color.FromArgb(18, 23, 33), Surface = Color.FromArgb(29, 35, 48), Raised = Color.FromArgb(42, 49, 64),
        Well = Color.FromArgb(22, 28, 40), Border = Color.FromArgb(56, 64, 82), Text = Color.FromArgb(242, 244, 248),
        Secondary = Color.FromArgb(150, 158, 175), Off = Color.FromArgb(99, 107, 124), Blue = Color.FromArgb(10, 132, 255),
        Green = Color.FromArgb(48, 209, 88), Orange = Color.FromArgb(255, 159, 10), Red = Color.FromArgb(255, 69, 58);
    static readonly string IconFamily = FontFamily.Families.Any(f => f.Name == "Segoe Fluent Icons") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets";
    public static Font UI(float size) => new("Segoe UI", size);
    public static Font Semibold(float size) => new("Segoe UI Semibold", size);
    public static Font Icons(float size) => new(IconFamily, size);
    public static float K(this Control c) => c.DeviceDpi / 96f;
    public static Color Shade(Color c, int d) => Color.FromArgb(c.A, Math.Clamp(c.R + d, 0, 255), Math.Clamp(c.G + d, 0, 255), Math.Clamp(c.B + d, 0, 255));
    public static GraphicsPath Round(RectangleF r, float radius)
    {
        var path = new GraphicsPath(); float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
        path.AddArc(r.X, r.Y, d, d, 180, 90); path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure(); return path;
    }
    public const TextFormatFlags Center = TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis;

    // Windows 11 rounded corners and a hairline border for borderless windows.
    public static void Chrome(Form form)
    {
        int round = 2, border = Border.R | Border.G << 8 | Border.B << 16;
        DwmSetWindowAttribute(form.Handle, 33, ref round, 4);
        DwmSetWindowAttribute(form.Handle, 34, ref border, 4);
    }
    public static void Drag(Form form, Control c) => c.MouseDown += (_, e) => { if (e.Button == MouseButtons.Left) { ReleaseCapture(); SendMessage(form.Handle, 0xA1, 2, 0); } };
    public static void DarkScroll(Control c) => c.HandleCreated += (_, _) => SetWindowTheme(c.Handle, "DarkMode_Explorer", null);
    [DllImport("user32.dll")] static extern bool ReleaseCapture();
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hwnd, int message, int wParam, int lParam);
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] static extern int SetWindowTheme(IntPtr hwnd, string app, string? id);
}

sealed class Bar : Control
{
    public double Value;
    public Bar() => SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; g.Clear(Parent?.BackColor ?? Theme.Back); g.SmoothingMode = SmoothingMode.AntiAlias;
        float h = Height - 1, w = (float)Math.Clamp(Value, 0, 1) * (Width - 1);
        using (var path = Theme.Round(new RectangleF(0, 0, Width - 1, h), h / 2)) using (var brush = new SolidBrush(Theme.Raised)) g.FillPath(brush, path);
        if (w < 1) return;
        using (var path = Theme.Round(new RectangleF(0, 0, Math.Max(w, h), h), h / 2)) using (var brush = new SolidBrush(Theme.Blue)) g.FillPath(brush, path);
    }
}

// iOS-style switch; the owner sets On from real state, a click only requests a change.
sealed class Switch : Button
{
    public bool On;
    public Switch()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0; Cursor = Cursors.Hand; AccessibleRole = AccessibleRole.CheckButton;
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; g.Clear(Parent?.BackColor ?? Theme.Back); g.SmoothingMode = SmoothingMode.AntiAlias;
        float h = Height - 1, pad = h * 0.08f, knob = h - 2 * pad;
        var track = On ? Theme.Green : Color.FromArgb(70, 76, 92);
        if (!Enabled) track = Color.FromArgb(120, track);
        using (var path = Theme.Round(new RectangleF(0, 0, Width - 1, h), h / 2)) using (var brush = new SolidBrush(track)) g.FillPath(brush, path);
        using var white = new SolidBrush(Enabled ? Color.White : Color.FromArgb(200, 200, 205));
        g.FillEllipse(white, On ? Width - 1 - pad - knob : pad, pad, knob, knob);
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(g, ClientRectangle);
    }
}

// Rounded panel; children inherit its fill as their BackColor.
class Surface : Panel
{
    public float Radius = 14;
    public Color Border = Color.Empty;
    public bool Dashed;
    public Surface() { DoubleBuffered = true; ResizeRedraw = true; BackColor = Theme.Surface; }
    protected override void OnPaintBackground(PaintEventArgs e)
    {
        var g = e.Graphics; float k = this.K();
        g.Clear(Parent?.BackColor ?? Theme.Back); g.SmoothingMode = SmoothingMode.AntiAlias;
        using var path = Theme.Round(new RectangleF(k / 2, k / 2, Width - 1 - k, Height - 1 - k), Radius * k);
        using (var brush = new SolidBrush(BackColor)) g.FillPath(brush, path);
        if (Border.IsEmpty) return;
        using var pen = new Pen(Border, k);
        if (Dashed) pen.DashPattern = [4f, 3f];
        g.DrawPath(pen, path);
    }
}

class Pill : Button
{
    public Color Fill = Theme.Blue, Ink = Color.White;
    public string Glyph = "";
    public float Radius = 12;
    bool hover;
    public Pill()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0; Cursor = Cursors.Hand; Font = Theme.Semibold(10.5f);
    }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; float k = this.K();
        g.Clear(Parent?.BackColor ?? Theme.Back); g.SmoothingMode = SmoothingMode.AntiAlias;
        var fill = !Enabled ? Theme.Raised : hover ? Theme.Shade(Fill, 16) : Fill;
        using (var path = Theme.Round(new RectangleF(0, 0, Width - 1, Height - 1), Radius * k)) using (var brush = new SolidBrush(fill)) g.FillPath(brush, path);
        var ink = Enabled ? Ink : Theme.Off;
        if (Glyph.Length == 0) TextRenderer.DrawText(g, Text, Font, ClientRectangle, ink, Theme.Center);
        else
        {
            using var icons = Theme.Icons(Font.SizeInPoints + 2);
            var gs = TextRenderer.MeasureText(g, Glyph, icons, Size.Empty, TextFormatFlags.NoPadding);
            var ts = TextRenderer.MeasureText(g, Text, Font, Size.Empty, TextFormatFlags.NoPadding);
            int gap = (int)(14 * k), x = (Width - gs.Width - gap - ts.Width) / 2;
            TextRenderer.DrawText(g, Glyph, icons, new Rectangle(x, 0, gs.Width, Height), ink, TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
            TextRenderer.DrawText(g, Text, Font, new Rectangle(x + gs.Width + gap, 0, ts.Width + 2, Height), ink, TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
        }
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(g, Rectangle.Inflate(ClientRectangle, -3, -3));
    }
}

sealed class IconButton : Button
{
    public string Glyph = "";
    public Color Ink = Theme.Secondary;
    bool hover;
    public IconButton()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0; Cursor = Cursors.Hand; Font = Theme.Icons(12);
    }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; g.Clear(Parent?.BackColor ?? Theme.Back); g.SmoothingMode = SmoothingMode.AntiAlias;
        if (hover && Enabled) { using var brush = new SolidBrush(Theme.Raised); g.FillEllipse(brush, 0, 0, Width - 1, Height - 1); }
        TextRenderer.DrawText(g, Glyph, Font, ClientRectangle, Enabled ? Ink : Theme.Off, Theme.Center);
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(g, ClientRectangle);
    }
}

sealed class Dot : Control
{
    public Color Color = Theme.Off;
    public Dot() => SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Parent?.BackColor ?? Theme.Back); e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var brush = new SolidBrush(Color); e.Graphics.FillEllipse(brush, 0, 0, Width - 1, Height - 1);
    }
}

// PulseDrop mark; coordinates match assets/brand/pulsedrop.svg.
sealed class AppIcon : Control
{
    public bool On = true;
    public float Corner = 14;
    public AppIcon() => SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; float k = this.K(), w = Width;
        g.Clear(Parent?.BackColor ?? Theme.Back); g.SmoothingMode = SmoothingMode.AntiAlias;
        using (var path = Theme.Round(new RectangleF(0, 0, w - 1, Height - 1), Corner * k))
        using (var brush = On ? new LinearGradientBrush(ClientRectangle, Color.FromArgb(90, 180, 255), Color.FromArgb(10, 96, 235), 60f)
                              : new LinearGradientBrush(ClientRectangle, Color.FromArgb(70, 77, 94), Color.FromArgb(48, 54, 68), 60f)) g.FillPath(brush, path);
        using var pen = new Pen(Color.White, w / 16) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round };
        g.DrawLines(pen, new[] { new PointF(w * 52 / 256, Height * 136 / 256), new PointF(w * 88 / 256, Height * 136 / 256),
            new PointF(w * 108 / 256, Height * 88 / 256), new PointF(w * 140 / 256, Height * 172 / 256),
            new PointF(w * 164 / 256, Height * 120 / 256), new PointF(w * 204 / 256, Height * 120 / 256) });
    }
}

// Click-to-choose file well. Uses WM_DROPFILES rather than OLE drag-drop so Explorer
// can still drop onto this elevated window (OLE drops are blocked by UIPI).
sealed class DropZone : Surface
{
    public string Glyph = "", Title = "", Sub = "";
    public Color GlyphColor = Theme.Secondary;
    public event Action<string[]>? FilesDropped;
    public DropZone()
    {
        Dashed = true; Border = Theme.Border; BackColor = Theme.Well; Radius = 16; Cursor = Cursors.Hand;
        SetStyle(ControlStyles.Selectable, true); TabStop = true; AccessibleRole = AccessibleRole.PushButton;
    }
    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        DragAcceptFiles(Handle, true);
        foreach (uint message in new uint[] { 0x0233, 0x004A, 0x0049 }) ChangeWindowMessageFilterEx(Handle, message, 1, IntPtr.Zero);
    }
    protected override void WndProc(ref Message m)
    {
        if (m.Msg == 0x0233)
        {
            var paths = new string[DragQueryFile(m.WParam, uint.MaxValue, null, 0)];
            var buffer = new StringBuilder(32768);
            for (uint i = 0; i < paths.Length; i++) { DragQueryFile(m.WParam, i, buffer, buffer.Capacity); paths[i] = buffer.ToString(); }
            DragFinish(m.WParam);
            FilesDropped?.Invoke(paths);
            return;
        }
        base.WndProc(ref m);
    }
    protected override void OnMouseEnter(EventArgs e) { Border = Theme.Blue; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { Border = Theme.Border; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnGotFocus(EventArgs e) { Border = Theme.Blue; Invalidate(); base.OnGotFocus(e); }
    protected override void OnLostFocus(EventArgs e) { Border = Theme.Border; Invalidate(); base.OnLostFocus(e); }
    protected override void OnKeyDown(KeyEventArgs e) { if (e.KeyCode is Keys.Enter or Keys.Space) OnClick(e); base.OnKeyDown(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; float k = this.K();
        using var icons = Theme.Icons(24); using var title = Theme.Semibold(13); using var sub = Theme.UI(9.5f);
        TextRenderer.DrawText(g, Glyph, icons, new Rectangle(0, (int)(30 * k), Width, (int)(46 * k)), GlyphColor, Theme.Center);
        TextRenderer.DrawText(g, Title, title, new Rectangle((int)(16 * k), (int)(84 * k), Width - (int)(32 * k), (int)(30 * k)), Theme.Text, Theme.Center);
        TextRenderer.DrawText(g, Sub, sub, new Rectangle((int)(16 * k), (int)(114 * k), Width - (int)(32 * k), (int)(24 * k)), Theme.Secondary, Theme.Center);
    }
    [DllImport("shell32.dll")] static extern void DragAcceptFiles(IntPtr hwnd, bool accept);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern uint DragQueryFile(IntPtr drop, uint index, StringBuilder? file, int size);
    [DllImport("shell32.dll")] static extern void DragFinish(IntPtr drop);
    [DllImport("user32.dll")] static extern bool ChangeWindowMessageFilterEx(IntPtr hwnd, uint message, uint action, IntPtr filter);
}

sealed class DeviceRow : Surface
{
    public readonly Device Device;
    public readonly Pill Send = new() { Text = "Send", Radius = 12 };
    readonly Label status = new() { Text = "AirDrop", Font = Theme.UI(9), ForeColor = Theme.Secondary, Bounds = new Rectangle(84, 39, 224, 20) };
    // When the radio stops hearing the phone; the row stays for a grace period instead of vanishing.
    public DateTime? LostAt { get; private set; }
    public DateTime VerifiedAt { get; private set; } = DateTime.UtcNow;
    public void Verified() { VerifiedAt = DateTime.UtcNow; SetLost(false); }
    public void SetLost(bool lost)
    {
        LostAt = lost ? LostAt ?? DateTime.Now : null;
        status.Text = lost ? "Reconnect to send" : "AirDrop";
        status.ForeColor = lost ? Theme.Orange : Theme.Secondary;
    }
    public DeviceRow(Device device)
    {
        Device = device; Size = new Size(432, 72);
        Controls.Add(new Label { Text = device.Name, Font = Theme.Semibold(11), ForeColor = Theme.Text, AutoEllipsis = true, Bounds = new Rectangle(84, 13, 224, 26) });
        Controls.Add(status);
        Send.Bounds = new Rectangle(322, 15, 96, 42); Send.AccessibleName = "Send to " + device.Name;
        Controls.Add(Send);
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics; float k = this.K(); g.SmoothingMode = SmoothingMode.AntiAlias;
        var circle = new RectangleF(10 * k, 10 * k, 52 * k, 52 * k);
        using (var brush = new SolidBrush(Theme.Raised)) g.FillEllipse(brush, circle);
        using var icons = Theme.Icons(17);
        TextRenderer.DrawText(g, "", icons, Rectangle.Round(circle), Theme.Text, Theme.Center);
    }
}

sealed class DarkMenuColors : ProfessionalColorTable
{
    public override Color ToolStripDropDownBackground => Theme.Raised;
    public override Color ImageMarginGradientBegin => Theme.Raised;
    public override Color ImageMarginGradientMiddle => Theme.Raised;
    public override Color ImageMarginGradientEnd => Theme.Raised;
    public override Color MenuBorder => Theme.Border;
    public override Color MenuItemBorder => Theme.Shade(Theme.Raised, 18);
    public override Color MenuItemSelected => Theme.Shade(Theme.Raised, 18);
    public override Color SeparatorDark => Theme.Border;
    public override Color SeparatorLight => Theme.Raised;
}
