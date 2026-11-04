using System.Security.Cryptography;
using System.Text.Json;

record Entry(string Path, long Size, string Sha256);
record Manifest(int Format, string Team, string Revision, Entry[] Files);
record EngineBuild(int MajorVersion, int MinorVersion, int PatchVersion, int Changelist);

static class Program
{
    const int MaxManifestBytes = 8 * 1024 * 1024;
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static string Hash(string path) { using var stream = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(stream)); }
    static string Root(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
    static bool Within(string path, string root) => path.Equals(root, StringComparison.OrdinalIgnoreCase) || path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    static void Separate(string a, string b) { if (Within(a, b) || Within(b, a)) throw new IOException("Source and destination must not overlap."); }
    static void NoLinks(string path)
    {
        for (var current = new DirectoryInfo(Path.GetFullPath(path)); current != null; current = current.Parent)
            if (current.Exists && (current.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("Linked paths are refused: " + current.FullName);
        if (File.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("Linked file refused.");
    }
    static string Resolve(string root, string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) || relative.Contains('\\') || relative.Contains(':') || Path.IsPathRooted(relative)) throw new IOException("Invalid manifest path.");
        var parts = relative.Split('/');
        if (parts.Any(p => p is "" or "." or ".." || p.EndsWith('.') || p.EndsWith(' ') || p.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || IsDevice(p))) throw new IOException("Invalid Windows path.");
        var result = Path.GetFullPath(Path.Combine(root, relative));
        if (!Within(result, root) || result.Equals(root, StringComparison.OrdinalIgnoreCase)) throw new IOException("Path escapes root.");
        NoLinks(result);
        return result;
    }
    static bool IsDevice(string part)
    {
        var stem = part.Split('.')[0].ToUpperInvariant();
        return stem is "CON" or "PRN" or "AUX" or "NUL" || (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && stem[3] >= '0' && stem[3] <= '9');
    }
    static void Identity(string value) { if (string.IsNullOrWhiteSpace(value) || value.Length > 128 || value.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not '-' and not '_')) throw new IOException("Team and revision must use 1–128 ASCII letters, numbers, hyphens or underscores."); }
    static IEnumerable<string> Walk(string root)
    {
        foreach (var file in Directory.EnumerateFiles(root)) { NoLinks(file); yield return file; }
        foreach (var dir in Directory.EnumerateDirectories(root))
        {
            NoLinks(dir);
            if (new[] { "Saved", "Intermediate", "DerivedDataCache", ".git", ".vs", "Binaries" }.Contains(Path.GetFileName(dir), StringComparer.OrdinalIgnoreCase)) continue;
            foreach (var file in Walk(dir)) yield return file;
        }
    }
    static Manifest Read(string root, string? pin = null)
    {
        var path = Resolve(root, "manifest.json");
        using var stream = File.OpenRead(path);
        if (stream.Length > MaxManifestBytes) throw new IOException("Manifest too large.");
        using var buffer = new MemoryStream();
        var chunk = new byte[65536];
        int count;
        while ((count = stream.Read(chunk)) != 0) { if (buffer.Length + count > MaxManifestBytes) throw new IOException("Manifest too large."); buffer.Write(chunk, 0, count); }
        var bytes = buffer.ToArray();
        if (pin != null && !Convert.ToHexString(SHA256.HashData(bytes)).Equals(pin, StringComparison.OrdinalIgnoreCase)) throw new IOException("Manifest identity mismatch.");
        var m = JsonSerializer.Deserialize<Manifest>(bytes) ?? throw new IOException("Missing manifest.");
        if (m.Format != 1 || m.Files == null || m.Files.Length == 0 || m.Files.Length > 100000) throw new IOException("Invalid manifest format.");
        Identity(m.Team); Identity(m.Revision);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in m.Files)
        {
            if (e == null || e.Size < 0 || e.Sha256 == null || e.Sha256.Length != 64 || !e.Sha256.All(Uri.IsHexDigit) || !seen.Add(e.Path)) throw new IOException("Invalid or duplicate file entry.");
            Resolve(root, e.Path);
            if (!e.Path.StartsWith("Project/", StringComparison.Ordinal)) throw new IOException("Files must be under Project/.");
        }
        if (!m.Files.Any(e => e.Path.Count(c => c == '/') == 1 && e.Path.EndsWith(".uproject", StringComparison.OrdinalIgnoreCase))) throw new IOException("No root Unreal project descriptor.");
        return m;
    }
    static void Verify(string path, Entry entry)
    {
        NoLinks(path);
        if (new FileInfo(path).Length != entry.Size || !Hash(path).Equals(entry.Sha256, StringComparison.OrdinalIgnoreCase)) throw new IOException("Hash or size mismatch: " + entry.Path);
    }
    static void CopyVerified(string source, string target, Entry? expected = null)
    {
        NoLinks(source); NoLinks(target);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        using (var input = File.OpenRead(source))
        using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        {
            if (expected == null) input.CopyTo(output);
            else
            {
                long remaining = expected.Size;
                var buffer = new byte[65536];
                while (remaining > 0)
                {
                    int read = input.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                    if (read == 0) throw new IOException("Source file truncated.");
                    output.Write(buffer, 0, read); remaining -= read;
                }
                if (input.ReadByte() != -1) throw new IOException("Source file exceeds declared size.");
            }
        }
        if (expected != null) Verify(target, expected);
    }
    static void Publish(string source, string destination, string team, string revision)
    {
        Identity(team); Identity(revision); Separate(source, destination); NoLinks(source); NoLinks(destination);
        if (!Directory.Exists(source) || !Directory.EnumerateFiles(source, "*.uproject").Any()) throw new IOException("Source must contain a .uproject file.");
        if (Directory.Exists(destination) || File.Exists(destination)) throw new IOException("Destination already exists.");
        var staging = destination + ".publishing-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(staging);
        var entries = new List<Entry>();
        foreach (var file in Walk(source).Order(StringComparer.OrdinalIgnoreCase))
        {
            var relative = "Project/" + Path.GetRelativePath(source, file).Replace('\\', '/');
            var target = Resolve(staging, relative);
            CopyVerified(file, target);
            entries.Add(new(relative, new FileInfo(target).Length, Hash(target)));
        }
        var manifest = new Manifest(1, team, revision, entries.ToArray());
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, Json);
        if (bytes.Length > MaxManifestBytes) throw new IOException("Manifest too large.");
        File.WriteAllBytes(Path.Combine(staging, "manifest.json"), bytes);
        Read(staging);
        Directory.Move(staging, destination);
        Console.WriteLine("Published. Pin this manifest SHA256: " + Hash(Path.Combine(destination, "manifest.json")));
    }
    static void Install(string source, string destination, string team, string revision, string pin)
    {
        Separate(source, destination); NoLinks(source); NoLinks(destination); Identity(team); Identity(revision);
        if (Directory.Exists(destination) || File.Exists(destination)) throw new IOException("Destination already exists; existing work is preserved.");
        var manifest = Read(source, pin);
        if (manifest.Team != team || manifest.Revision != revision) throw new IOException("Team or revision mismatch.");
        var staging = destination + ".installing-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(staging);
        foreach (var entry in manifest.Files)
        {
            var input = Resolve(source, entry.Path);
            var output = Resolve(staging, entry.Path[8..]);
            CopyVerified(input, output, entry);
        }
        Directory.Move(staging, destination);
        Console.WriteLine("Installed verified baseline: " + destination);
    }
    static string EditorPath(string engineRoot)
    {
        NoLinks(engineRoot);
        var versionFile = Path.Combine(engineRoot, "Engine", "Build", "Build.version");
        if (!File.Exists(versionFile)) throw new IOException("Not an Unreal Engine folder: Engine/Build/Build.version is missing.");
        var version = JsonSerializer.Deserialize<EngineBuild>(File.ReadAllBytes(versionFile));
        if (version is not { MajorVersion: 5, MinorVersion: 8, PatchVersion: 4 }) throw new IOException("Unreal Engine 5.8.4 is required.");
        var editor = Path.Combine(engineRoot, "Engine", "Binaries", "Win64", "UnrealEditor.exe");
        if (!File.Exists(editor)) throw new IOException("UnrealEditor.exe was not found under the engine folder.");
        NoLinks(editor);
        return editor;
    }
    // Opens a project and lets Unreal's own Multi-User auto-connect join the team session (ConcertSyncClientModule parses these flags).
    // Unreal's auto-connect creates the session if it is missing, so callers should launch only after the session is known to exist.
    static void Launch(string engineRoot, string project, string server, string session, bool dryRun)
    {
        Identity(server); Identity(session);
        if (!session.StartsWith("UCC-", StringComparison.Ordinal) || !server.StartsWith(session + "-", StringComparison.Ordinal)) throw new IOException("Server and session must belong to the same Classroom Connect team.");
        NoLinks(project);
        if (!project.EndsWith(".uproject", StringComparison.OrdinalIgnoreCase) || !File.Exists(project)) throw new IOException("Project must be an existing .uproject file.");
        var editor = EditorPath(engineRoot);
        string[] arguments = [project, "-CONCERTAUTOCONNECT", "-CONCERTSERVER=" + server, "-CONCERTSESSION=" + session];
        if (dryRun) { Console.WriteLine(JsonSerializer.Serialize(new { Editor = editor, Arguments = arguments }, Json)); return; }
        var start = new System.Diagnostics.ProcessStartInfo(editor) { UseShellExecute = false };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = System.Diagnostics.Process.Start(start) ?? throw new IOException("Unreal Editor did not start.");
        Console.WriteLine("Launched Unreal Editor (process " + process.Id + ").");
    }
    public static int Main(string[] args)
    {
        try
        {
            if (args.Length == 6 && args[0] == "publish" && args[5] == "--quiescent") Publish(Root(args[1]), Root(args[2]), args[3], args[4]);
            else if (args.Length == 6 && args[0] == "install") Install(Root(args[1]), Root(args[2]), args[3], args[4], args[5]);
            else if (args.Length is 5 or 6 && args[0] == "launch" && (args.Length == 5 || args[5] == "--dry-run")) Launch(Root(args[1]), Root(args[2]), args[3], args[4], args.Length == 6);
            else { Console.Error.WriteLine("publish <project> <new-snapshot> <team> <revision> --quiescent\ninstall <snapshot> <new-project> <team> <revision> <manifest-sha256>\nlaunch <engine-root> <project.uproject> <server-name> <session-name> [--dry-run]\nClose editors and stop all writers before publishing. Folder access uses existing permissions."); return 2; }
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e.Message); return 1; }
    }
}
