using BakeCacheMaintenance;
using System.Security.Cryptography;

if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("This utility uses NTFS hard links.");
var roots = new List<string>();
string? report = null;
var apply = false;
var selfTest = false;
for (var i = 0; i < args.Length; i++)
    switch (args[i])
    {
        case "--root": roots.Add(args[++i]); break;
        case "--report": report = args[++i]; break;
        case "--apply": apply = true; break;
        case "--self-test": selfTest = true; break;
        default: throw new ArgumentException($"Unknown argument: {args[i]}");
    }
if (report is null || roots.Count == 0)
    throw new ArgumentException("Supply --root DIRECTORY (repeatable), --report FRESH.json, and optionally --apply.");
if (selfTest)
{
    if (roots.Count != 1 || Directory.Exists(roots[0]))
        throw new ArgumentException("Self-test needs one fresh root directory.");
    var bytes = Enumerable.Range(0, 1024 * 1024).Select(i => (byte)(i % 251)).ToArray();
    foreach (var folder in new[] { "a", "b", "different", "existing" })
        Directory.CreateDirectory(Path.Combine(roots[0], folder));
    var first = Path.Combine(roots[0], "a", "123456789abcdef0.bake");
    var second = Path.Combine(roots[0], "b", "123456789abcdef0.bake");
    var different = Path.Combine(roots[0], "different", "123456789abcdef0.bake");
    File.WriteAllBytes(first, bytes);
    File.WriteAllBytes(second, bytes);
    File.WriteAllBytes(different, bytes.Select(value => (byte)(value ^ 255)).ToArray());
    var originalDifferent = SHA256.HashData(File.ReadAllBytes(different));
    WindowsFiles.Link(Path.Combine(roots[0], "existing", "123456789abcdef0.bake"), first);
    var test = new Consolidation(roots, report);
    test.Run(true);
    if (test.ReclaimedBytes != bytes.Length || WindowsFiles.Identify(first).Index != WindowsFiles.Identify(second).Index ||
        !SHA256.HashData(File.ReadAllBytes(different)).SequenceEqual(originalDifferent))
        throw new Exception("Duplicate cache consolidation self-test failed.");
    Console.WriteLine("Self-test passed: identical copies linked, differing bytes and existing links preserved.");
}
else new Consolidation(roots, report).Run(apply);
