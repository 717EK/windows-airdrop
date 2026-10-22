using System.IO.Compression;
using System.Text.Json;

namespace PulseIt;

static class DiagnosticExport
{
    internal static void Write(string directory, string output)
    {
        string[] names = ["diagnostics.jsonl", "link.log", "radio.log", "receiver.log", "sender.log", "discovery.log", "bluetooth.log", "candidates.json", "recovery.log", "recovery-result.json"];
        const long limit = 64L * 1024 * 1024;
        long remaining = limit;
        var included = new List<object>();
        using var file = new FileStream(output, FileMode.CreateNew, FileAccess.Write);
        using var zip = new ZipArchive(file, ZipArchiveMode.Create);
        foreach (var path in Directory.EnumerateFiles(directory).Order()) {
            var info = new FileInfo(path);
            bool recovery = info.Name.StartsWith("recovery-bluetooth") && info.Name.EndsWith(".log");
            if ((!names.Contains(info.Name) && !recovery) || (info.Attributes & FileAttributes.ReparsePoint) != 0) continue;
            using var source = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            long count = Math.Min(source.Length, remaining), left = count;
            using (var target = zip.CreateEntry(info.Name, CompressionLevel.Fastest).Open()) {
                byte[] buffer = new byte[64 * 1024];
                while (left > 0) {
                    int read = source.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
                    if (read == 0) break;
                    target.Write(buffer, 0, read); left -= read;
                }
            }
            included.Add(new { name = info.Name, bytes = count - left, truncated = source.Length > count });
            remaining -= count - left;
            if (remaining == 0) break;
        }
        using var writer = new StreamWriter(zip.CreateEntry("manifest.json").Open());
        writer.Write(JsonSerializer.Serialize(new { utc = DateTimeOffset.UtcNow,
            version = typeof(Session).Assembly.GetName().Version?.ToString(), limit, included }));
    }
}
