using System.Security.Principal;
using System.Diagnostics;

namespace PulseIt;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        if (args.Contains("--configure-driver") || args.Contains("--restore-driver")) {
            var setup = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true };
            foreach (var arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(AppContext.BaseDirectory, "Setup-Devices.ps1"), "-Action", args.Contains("--restore-driver") ? "Restore" : "Configure", "-Root", AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar) }) setup.ArgumentList.Add(arg);
            using var process = Process.Start(setup)!; process.WaitForExit();
            MessageBox.Show(process.ExitCode == 0 ? "Adapter setup completed." : "Adapter setup failed. Close PulseIt and check C:\\ProgramData\\PulseIt\\setup.log.", "PulseIt setup");
            return process.ExitCode;
        }
        if (args.Contains("--self-test")) return Checks.Run();
        if (args.Contains("--recovery-test")) return Checks.Recovery().GetAwaiter().GetResult();
        if (args.Contains("--lifecycle-soak-test")) return Checks.Lifecycle(10).GetAwaiter().GetResult();
        if (args.Contains("--lifecycle-test")) return Checks.Lifecycle().GetAwaiter().GetResult();
        using var single = new Mutex(true, "Local\\PulseIt.Desktop", out bool first);
        if (!first && !args.Contains("--render")) { MessageBox.Show("PulseIt is already open.", "PulseIt"); return 0; }
        if (!args.Contains("--render") && !new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
        { MessageBox.Show("Open PulseIt.exe and accept the Windows administrator prompt to use awdl0.", "PulseIt"); return 1; }
        using var form = new MainForm(preview: args.Contains("--render"));
        if (args.Contains("--render"))
        {
            form.Show(); Application.DoEvents();
            bool HasHistory(Control control) => control is RecentRow || control.Controls.Cast<Control>().Any(HasHistory);
            if (HasHistory(form)) throw new IOException("Public previews must not contain transfer history.");
            void Save(string name)
            {
                Application.DoEvents();
                using var bitmap = new Bitmap(form.Width, form.Height);
                form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                bitmap.Save(Path.Combine(AppContext.BaseDirectory, name));
            }
            Save("preview.png");
            if (args.Contains("--demo"))
            {
                form.Demo(Device.Validate("iPhone 14 Pro Max", "fe80::1", 8770));
                Save("preview-devices.png");
                form.Demo(null);
                Save("preview-receive.png");
                form.DemoProgress(); Save("preview-progress.png");
                foreach (var (dialog, name) in new (Form, string)[] {
                    (new ConsentDialog(new Ask("demo", "Example iPhone", ["Photo 1.HEIC", "Photo 2.HEIC"], 3_400_000)), "preview-consent.png"),
                    (new ProfileDialog(new Session()), "preview-profile.png") })
                {
                    dialog.StartPosition = FormStartPosition.CenterScreen; dialog.Show(); Application.DoEvents();
                    using var shot = new Bitmap(dialog.Width, dialog.Height);
                    dialog.DrawToBitmap(shot, new Rectangle(Point.Empty, dialog.Size));
                    shot.Save(Path.Combine(AppContext.BaseDirectory, name)); dialog.Dispose();
                }
            }
            form.Close();
            for (int i = 0; i < 5; i++) Application.DoEvents();
            if (form.Visible) throw new IOException("Idle-window shutdown did not finish.");
            return 0;
        }
        Application.Run(form);
        return 0;
    }
}
