using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace BakeCacheMaintenance;

internal sealed class Consolidation
{
    internal sealed record Entry(string Path, long Bytes, long WriteTicks, WindowsFiles.Identity Identity);
    internal sealed record Action(string Path, string Canonical, string Sha256, long Bytes, string State);
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private static readonly Regex CacheName = new("^[0-9a-f]{1,16}\\.bake$", RegexOptions.CultureInvariant);
    private readonly string[] roots;
    private readonly string report;
    private readonly List<Action> actions = [];
    internal long ReclaimedBytes { get; private set; }

    internal Consolidation(IEnumerable<string> roots, string report)
    {
        this.roots = roots.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        this.report = Path.GetFullPath(report);
        if (File.Exists(this.report)) throw new IOException("Choose a fresh report path.");
        foreach (var root in this.roots)
            if (!Directory.Exists(root) || File.GetAttributes(root).HasFlag(FileAttributes.ReparsePoint))
                throw new IOException($"Root must be an existing ordinary directory: {root}");
    }

    private void CheckScope(string path)
    {
        var absolute = Path.GetFullPath(path);
        if (!roots.Any(root =>
        {
            var relative = Path.GetRelativePath(root, absolute);
            return !Path.IsPathRooted(relative) && relative != ".." &&
                !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal);
        })) throw new IOException($"Path leaves the requested roots: {absolute}");
        if (File.Exists(absolute) && File.GetAttributes(absolute).HasFlag(FileAttributes.ReparsePoint))
            throw new IOException($"Refusing reparse point: {absolute}");
    }

    private static string Hash(string path)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete,
            1024 * 1024, FileOptions.SequentialScan);
        return Convert.ToHexString(SHA256.HashData(input));
    }

    private static bool Unchanged(Entry entry)
    {
        var file = new FileInfo(entry.Path);
        var identity = WindowsFiles.Identify(entry.Path);
        return file.Length == entry.Bytes && file.LastWriteTimeUtc.Ticks == entry.WriteTicks &&
            identity.Volume == entry.Identity.Volume && identity.Index == entry.Identity.Index;
    }

    private void WriteReport(string phase, int files)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(report)!);
        var temporary = report + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(new
        {
            phase, roots, files, reclaimedPayloadBytes = ReclaimedBytes,
            utc = DateTimeOffset.UtcNow, actions
        }, JsonOptions));
        File.Move(temporary, report, true);
    }

    internal void Run(bool apply)
    {
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint,
            IgnoreInaccessible = false, MaxRecursionDepth = 32
        };
        var entries = roots.SelectMany(root => Directory.EnumerateFiles(root, "*.bake", options))
            .Where(path => CacheName.IsMatch(Path.GetFileName(path)))
            .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase)
            .Select(path =>
            {
                CheckScope(path);
                var file = new FileInfo(path);
                return new Entry(Path.GetFullPath(path), file.Length, file.LastWriteTimeUtc.Ticks,
                    WindowsFiles.Identify(path));
            }).ToList();
        var candidates = entries.GroupBy(entry => (Path.GetFileName(entry.Path), entry.Bytes))
            .Where(group => group.Count() > 1).ToList();
        var hashed = 0;
        foreach (var group in candidates)
        {
            var canonical = new Dictionary<(uint, string), Entry>();
            foreach (var entry in group)
            {
                CheckScope(entry.Path);
                var sha = Hash(entry.Path);
                if (!Unchanged(entry)) throw new IOException($"File changed while hashing: {entry.Path}");
                if (canonical.TryGetValue((entry.Identity.Volume, sha), out var source))
                {
                    var state = entry.Identity.Index == source.Identity.Index ? "already-linked" : "planned";
                    actions.Add(new(entry.Path, source.Path, sha, entry.Bytes, state));
                }
                else canonical[(entry.Identity.Volume, sha)] = entry;
                if (++hashed % 10 == 0) Console.WriteLine($"Hashed {hashed} candidate files");
            }
        }
        // Persist the complete SHA-256 plan before replacing any directory entry.
        WriteReport("planned", entries.Count);
        if (!apply) return;
        var snapshots = entries.ToDictionary(entry => entry.Path, StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < actions.Count; i++)
        {
            var action = actions[i];
            if (action.State != "planned") continue;
            var destination = snapshots[action.Path];
            var source = snapshots[action.Canonical];
            CheckScope(destination.Path);
            CheckScope(source.Path);
            if (!Unchanged(destination) || !Unchanged(source))
                throw new IOException("A planned cache changed; nothing further will be replaced.");
            var priorLinks = WindowsFiles.Identify(destination.Path).Links;
            var temporary = destination.Path + ".dedupe-" + Guid.NewGuid().ToString("N") + ".tmp";
            CheckScope(temporary);
            var created = false;
            try
            {
                WindowsFiles.Link(temporary, source.Path);
                created = true;
                File.Move(temporary, destination.Path, true);
                var actual = WindowsFiles.Identify(destination.Path);
                if (actual.Volume != source.Identity.Volume || actual.Index != source.Identity.Index)
                    throw new IOException("Hard-link identity verification failed.");
                if (priorLinks == 1) ReclaimedBytes += destination.Bytes;
                actions[i] = action with { State = "linked" };
                WriteReport("applying", entries.Count);
            }
            finally
            {
                // Only our fresh, verified-scope temporary link may be removed.
                if (created && File.Exists(temporary)) File.Delete(temporary);
            }
        }
        WriteReport("complete", entries.Count);
        Console.WriteLine($"Consolidated {actions.Count(action => action.State == "linked")} copies; " +
            $"reclaimed {ReclaimedBytes:N0} duplicate payload bytes.");
    }
}
