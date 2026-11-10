// zaimport [--no-keys] [--title-keys <file>] <prod.keys> <out dir> <package>...: the game files ZA Recomp runs from,
// made from the player's own Switch packages (.xci / .nsp: base game, update, DLC; .nup: an unencrypted NX package whose
// update tree is the same game data) and keys, as the console mounts them: <out>/exefs/ (the update's when there is one),
// <out>/romfs.bin (an .xci/.nsp base RomFS with the update applied: BKTR) or <out>/romfs/ (a .nup base+update tree),
// and <out>/aoc/<title id>.bin (each DLC's RomFS image). A .nup needs no keys; .nsp/.xci packages do.
// Title keys come from the packages' tickets, else --title-keys or title.keys next to prod.keys (also title.keys.txt:
// a name Windows' hidden extensions make easy to end up with).
// Progress on stdout: "progress <bytes done> <bytes total>"; a failure: exit 1 and "error: <reason>" as the last line.
// Published self-contained (runtime/ui/launcher.mm runs it from zaimport/ next to the exe).
using LibHac;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Spl;
using LibHac.Tools.Es;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using System.Security.Cryptography;
using System.Text;
using Path = System.IO.Path;

const ulong ZaBase = 0x0100F43008C44000;  // Pokémon Legends Z-A (update: +0x800, DLC: +0x1000 + n)

// (the error line is read by the start screen as UTF-8: without this "Pokémon ...nsp" came out as "Pok?mon ...nsp", #96)
// (a writer of its own: Console.OutputEncoding needs a console, and the start screen starts this without a usable one)
Console.SetOut(new StreamWriter(Console.OpenStandardOutput(), new System.Text.UTF8Encoding(false)) { AutoFlush = true });

try
{
    return Run(args);
}
catch (MissingKeyException e)  // (which key, and what to do about it: the players' most common problem)
{
    if (e.Type == LibHac.Common.Keys.KeyType.Title)
        return Fail("a title key is missing: put title.keys from your Switch in the folder with the game files " +
                    "(packages without their ticket need it)");
    return Fail($"your prod.keys has no {e.Name}: it comes from a Switch firmware older than the game or its " +
                "update needs. Dump the keys again (Lockpick_RCM) on a Switch updated to the latest firmware");
}
catch (NupException e)
{
    return Fail(e.Message);
}
catch (Exception e)
{
    Console.WriteLine($"error: {e.Message}");
    return 1;
}

static int Fail(string why)
{
    Console.WriteLine($"error: {why}");
    return 1;
}

// What a file that opened but has no NSP / XCI / NUP layout really is: the size and first bytes say a failed download
// or copy (empty, or zeros) from another format at once, and it's what a report needs (#96)
static string NotAPackage(string pkg)
{
    var name = Path.GetFileName(pkg);
    using var f = File.OpenRead(pkg);
    var head = new byte[4];
    int got = f.Read(head, 0, 4);
    string size = $"{f.Length / (1024.0 * 1024.0):0.0} MB", magic = Convert.ToHexString(head, 0, got);
    if (got < 4 || head.All(b => b == 0))
        return $"{name}: this file is empty or wasn't downloaded / copied completely ({size}). Get it again";
    return $"{name}: not an NSP, XCI or NUP ({size}, starts with {magic}). Dump this package again from your Switch";
}

// A .nup starts with "NUP" + its major version. The reader itself reports an unsupported version.
static bool IsNup(string path)
{
    using var f = File.OpenRead(path);
    var head = new byte[3];
    return f.Read(head, 0, 3) == 3 && head.AsSpan().SequenceEqual("NUP"u8);
}

static int Run(string[] args)
{
    string? titleKeys = null;  // (--title-keys: the launcher found it by its contents, whatever its name)
    bool noKeys = false;       // (--no-keys: a .nup-only import; the start screen passes this)
    while (args.Length > 0)
    {
        if (args[0] == "--title-keys" && args.Length >= 2) (titleKeys, args) = (args[1], args[2..]);
        else if (args[0] == "--no-keys") (noKeys, args) = (true, args[1..]);
        else break;
    }
    string keysPath = "", outDir;
    string[] packages;
    if (noKeys)
    {
        if (args.Length < 2) return Fail("usage: zaimport [--no-keys] [--title-keys <title.keys>] <prod.keys> <out dir> " +
                                         "<package.xci|nsp|nup>...");
        outDir = args[0];
        packages = args[1..];
    }
    else
    {
        if (args.Length < 3) return Fail("usage: zaimport [--title-keys <title.keys>] <prod.keys> <out dir> " +
                                         "<package.xci|nsp|nup>...");
        keysPath = args[0];
        outDir = args[1];
        packages = args[2..];
    }
    if (noKeys && titleKeys != null)
        return Fail("--title-keys is only used with a prod.keys; a .nup has no keys");

    KeySet? ks = null;
    if (!noKeys)
    {
        string keyDir = Path.GetDirectoryName(Path.GetFullPath(keysPath))!;
        titleKeys ??= new[] { "title.keys", "title.keys.txt" }
            .Select(n => Path.Combine(keyDir, n)).FirstOrDefault(File.Exists);
        ks = ExternalKeyReader.ReadKeyFile(keysPath, titleKeys);
        if (ks.HeaderKey.IsZeros())
            return Fail($"{Path.GetFileName(keysPath)} has no header_key: it isn't a complete prod.keys. Dump the keys from " +
                        "your Switch with Lockpick_RCM and use its prod.keys");
    }

    // every NCA of every .xci/.nsp and every package of every .nup (the files stay open for the NCA storages)
    var ncas = new List<(Nca nca, string name)>();
    var nups = new List<NupReader>();
    foreach (var pkg in packages)
    {
        if (IsNup(pkg))
        {
            nups.Add(new NupReader(pkg));
            continue;
        }
        if (noKeys)
            return Fail($"prod.keys is needed for {Path.GetFileName(pkg)} (.nsp / .xci); a .nup does not need one");
        var st = new LocalStorage(pkg, FileAccess.Read);
        IFileSystem fs;
        if (IsXci(st))
        {
            var xci = new Xci(ks!, st);
            if (!xci.HasPartition(XciPartitionType.Secure)) return Fail($"{Path.GetFileName(pkg)}: no secure partition");
            fs = xci.OpenPartition(XciPartitionType.Secure);
        }
        else
        {
            var pfs = new PartitionFileSystem();
            if (pfs.Initialize(st).IsFailure()) return Fail(NotAPackage(pkg));
            fs = pfs;
        }
        foreach (var e in fs.EnumerateEntries("/", "*.tik"))
        {
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            var t = new Ticket(f.Get.AsStream());
            var key = t.GetTitleKey(ks!);
            if (key != null) ks!.ExternalKeySet.Add(new RightsId(t.RightsId), new AccessKey(key));
        }
        foreach (var e in fs.EnumerateEntries("/", "*.nca"))
        {
            var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            ncas.Add((new Nca(ks!, f.Release().AsStorage()), $"{Path.GetFileName(pkg)}/{e.Name}"));
        }
    }

    // .nup packages: the base and the highest update, plus every add-on. An update's code and data are the full ones
    // (NUP v1 update trees are not deltas); the add-ons are the Switch 2 Edition upgrade and the DLC entitlements.
    (NupReader Reader, NupPackage Package)? nupBase = null, nupUpdate = null;
    var nupAddons = new List<(NupReader Reader, NupPackage Package)>();
    foreach (var r in nups)
    {
        foreach (var p in r.Packages)
        {
            Console.WriteLine($"{Path.GetFileName(r.Path)}: {p.KindName} {p.TitleId:x16} v{p.Version:#x}");
            if ((p.TitleId & ~0x1FFFUL) != ZaBase) continue;
            switch (p.Kind)
            {
                case 0:
                    if (nupBase == null || p.Version > nupBase.Value.Package.Version) nupBase = (r, p);
                    break;
                case 1:
                    if (nupUpdate == null || p.Version > nupUpdate.Value.Package.Version) nupUpdate = (r, p);
                    break;
                case 2:
                    nupAddons.Add((r, p));
                    break;
            }
        }
    }

    Nca? base_ = null, upd = null;
    var dlcs = new List<Nca>();
    foreach (var (nca, name) in ncas)
    {
        ulong tid = nca.Header.TitleId;
        if ((tid & ~0x1FFFUL) != ZaBase) continue;
        Console.WriteLine($"{name}: title {tid:x16} {nca.Header.ContentType}");
        if (nca.Header.ContentType == NcaContentType.Program)
        {
            // (the update's program NCA carries the base title id too: its data section is a patch)
            if (IsPatch(nca)) upd = nca;
            else if (tid == ZaBase) base_ = nca;
        }
        else if (nca.Header.ContentType == NcaContentType.PublicData)
        {
            dlcs.Add(nca);
        }
    }
    bool useNup = nupBase != null || nupUpdate != null;
    if (!useNup && base_ == null)
        return Fail(ncas.Count == 0 && nups.Count == 0 ? "no game packages found" : "the base game of Pokémon Legends Z-A isn't among these files");
    if (useNup && (base_ != null || upd != null))
        Console.WriteLine("note: using the .nup's base/update; the .nsp/.xci base/update in this folder is ignored");
    foreach (var n in new[] { base_, upd }.Concat(dlcs))
        if (n != null && n.Header.HasRightsId && !ks!.ExternalKeySet.Contains(new RightsId(n.Header.RightsId)))
            return Fail($"no title key for {n.Header.TitleId:x16}: put title.keys from your Switch in the folder with " +
                        "the game files (packages without their ticket need it)");

    Directory.CreateDirectory(outDir);

    // The add-ons: an .nsp's DLC is an NCA data section; a .nup's is a plain tree that has to be put back into a RomFS
    // image. The Switch 2 Edition's upgrade pack (0100f43008c45001) is a 0-byte dummy in the .nup, but the game's aoc
    // service still has to see it: its presence is the entitlement. If an .nsp DLC has the same title id it wins.
    var dlcIds = new HashSet<ulong>(dlcs.Select(d => d.Header.TitleId));
    var nupAddonImages = new List<(ulong TitleId, NupRomFs.Layout Layout)>();
    foreach (var (reader, pkg) in nupAddons)
    {
        if (dlcIds.Contains(pkg.TitleId))
        {
            Console.WriteLine($"add-on {pkg.TitleId:x16} from the .nup is superseded by the .nsp DLC");
            continue;
        }
        if (!pkg.HasRomfs)
        {
            Console.WriteLine($"add-on {pkg.TitleId:x16} has no RomFS; skipped");
            continue;
        }
        var entries = new List<NupRomFs.Entry>();
        foreach (var (path, file) in reader.EnumerateFiles(pkg.RomfsDir))
            entries.Add(new NupRomFs.Entry(path, file.Size, file.Sha256, () => reader.OpenFile(file)));
        nupAddonImages.Add((pkg.TitleId, NupRomFs.Build(entries)));
        Console.WriteLine($"add-on {pkg.TitleId:x16}: {entries.Count} RomFS file(s), image {nupAddonImages[^1].Layout.ImageSize} bytes");
    }

    ImportProgress progress;
    if (useNup)
    {
        PrepareNupOutput(outDir);
        var exefs = new Dictionary<string, (NupReader Reader, NupFile File)>(StringComparer.Ordinal);
        var romfs = new Dictionary<string, (NupReader Reader, NupFile File)>(StringComparer.Ordinal);
        if (nupBase != null)
        {
            AddNupTree(exefs, nupBase.Value.Reader, nupBase.Value.Package.ExefsDir);
            AddNupTree(romfs, nupBase.Value.Reader, nupBase.Value.Package.RomfsDir);
        }
        if (nupUpdate != null)
        {
            AddNupTree(exefs, nupUpdate.Value.Reader, nupUpdate.Value.Package.ExefsDir);
            AddNupTree(romfs, nupUpdate.Value.Reader, nupUpdate.Value.Package.RomfsDir);
        }
        if (exefs.Count == 0) return Fail("the .nup's game packages have no exefs (main)");
        if (romfs.Count == 0) return Fail("the .nup's game packages have no romfs");

        var dlcImages = OpenDlcImages(dlcs, outDir, out long dlcBytes);
        long total = 0;
        foreach (var (_, file) in exefs.Values) total += checked((long)file.Size);
        foreach (var (_, file) in romfs.Values) total += checked((long)file.Size);
        foreach (var (_, layout) in nupAddonImages) total += checked((long)layout.ImageSize);
        total += dlcBytes;
        progress = new ImportProgress(total);
        ExtractNupTree(exefs, Path.Combine(outDir, "exefs"), progress);
        ExtractNupTree(romfs, Path.Combine(outDir, "romfs"), progress);
        WriteAddonImages(nupAddonImages, dlcImages, outDir, progress);
        Console.WriteLine($"exefs from the .nup {(nupUpdate != null ? "update" : "base")}");
        LogMainBuildId(Path.Combine(outDir, "exefs", "main"));
    }
    else
    {
        PrepareNcaOutput(outDir);
        var code = upd ?? base_;
        long exefsBytes = 0;
        using (var exefs = code!.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.None))
        {
            foreach (var e in exefs.EnumerateEntries("/", "*")) exefsBytes += checked((long)e.Size);
        }
        var images = new List<(IStorage st, string path)>
        {
            (upd != null ? base_!.OpenStorageWithPatch(upd, NcaSectionType.Data, IntegrityCheckLevel.None)
                         : base_!.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.None),
             Path.Combine(outDir, "romfs.bin"))
        };
        foreach (var d in dlcs)
            images.Add((d.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.None),
                        Path.Combine(outDir, "aoc", $"{d.Header.TitleId:x16}.bin")));
        long total = exefsBytes;
        var imageSizes = new long[images.Count];
        for (int i = 0; i < images.Count; i++)
        {
            images[i].st.GetSize(out long size).ThrowIfFailure();
            imageSizes[i] = size;
            total += size;
        }
        foreach (var (_, layout) in nupAddonImages) total += checked((long)layout.ImageSize);
        progress = new ImportProgress(total);

        using (var exefs = code.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.None))
        {
            foreach (var e in exefs.EnumerateEntries("/", "*"))
            {
                using var f = new UniqueRef<IFile>();
                exefs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
                f.Get.GetSize(out long sz).ThrowIfFailure();
                var data = new byte[sz];
                f.Get.Read(out _, 0, data).ThrowIfFailure();
                File.WriteAllBytes(Path.Combine(outDir, "exefs", e.Name), data);
                progress.Add(sz);
            }
        }
        Console.WriteLine($"exefs from {(upd != null ? "the update" : "the base game")}");
        if (dlcs.Count > 0) Directory.CreateDirectory(Path.Combine(outDir, "aoc"));
        var buf = new byte[8 << 20];
        for (int i = 0; i < images.Count; i++)
        {
            var (st, path) = images[i];
            using var f = File.Create(path + ".part");
            for (long off = 0; off < imageSizes[i]; off += buf.Length)
            {
                int n = (int)Math.Min(buf.Length, imageSizes[i] - off);
                st.Read(off, buf.AsSpan(0, n)).ThrowIfFailure();
                f.Write(buf, 0, n);
                progress.Add(n);
            }
            f.Close();
            File.Move(path + ".part", path, true);
        }
        WriteAddonImages(nupAddonImages, Array.Empty<(IStorage St, string Path)>(), outDir, progress);
        LogMainBuildId(Path.Combine(outDir, "exefs", "main"));
    }

    int addons = nupAddonImages.Count + dlcs.Count;
    Console.WriteLine($"done: update {(upd != null || nupUpdate != null ? "applied" : "none")}, {addons} add-on(s)");
    return 0;
}

static bool IsPatch(Nca nca)
{
    int i = Nca.GetSectionIndexFromType(NcaSectionType.Data, nca.Header.ContentType);
    return nca.Header.IsSectionEnabled(i) && nca.Header.GetFsHeader(i).IsPatchSection();
}

static bool IsXci(IStorage st)
{
    var magic = new byte[4];
    return st.Read(0x100, magic).IsSuccess() && magic.AsSpan().SequenceEqual("HEAD"u8);
}

// The .nup's trees: base first, then update, so the update's files replace the base's (and a base-only file, like
// arc/data.trfmd.bkup, stays).
static void AddNupTree(Dictionary<string, (NupReader Reader, NupFile File)> map, NupReader reader, uint dir)
{
    if (dir == uint.MaxValue) return;
    foreach (var (path, file) in reader.EnumerateFiles(dir)) map[path] = (reader, file);
}

static void PrepareNupOutput(string outDir)
{
    Directory.CreateDirectory(outDir);
    string exefs = Path.Combine(outDir, "exefs"), romfs = Path.Combine(outDir, "romfs");
    if (Directory.Exists(exefs)) Directory.Delete(exefs, true);
    Directory.CreateDirectory(exefs);
    if (File.Exists(Path.Combine(outDir, "romfs.bin"))) File.Delete(Path.Combine(outDir, "romfs.bin"));
    if (Directory.Exists(romfs)) Directory.Delete(romfs, true);
    Directory.CreateDirectory(romfs);
    if (Directory.Exists(Path.Combine(outDir, "aoc"))) Directory.Delete(Path.Combine(outDir, "aoc"), true);
}

static void PrepareNcaOutput(string outDir)
{
    Directory.CreateDirectory(outDir);
    string exefs = Path.Combine(outDir, "exefs"), romfs = Path.Combine(outDir, "romfs");
    if (Directory.Exists(exefs)) Directory.Delete(exefs, true);
    Directory.CreateDirectory(exefs);
    if (Directory.Exists(romfs)) Directory.Delete(romfs, true);
    if (File.Exists(Path.Combine(outDir, "romfs.bin"))) File.Delete(Path.Combine(outDir, "romfs.bin"));
    if (Directory.Exists(Path.Combine(outDir, "aoc"))) Directory.Delete(Path.Combine(outDir, "aoc"), true);
}

static string SafeJoin(string root, string relative)
{
    if (OperatingSystem.IsWindows() && relative.Contains(':'))
        throw new NupException($"the NUP has an unsafe path: {relative}");
    string full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
    string prefix = Path.GetFullPath(root) + Path.DirectorySeparatorChar;
    StringComparison cmp = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
    if (!full.StartsWith(prefix, cmp))
        throw new NupException($"the NUP has a path outside the game folder: {relative}");
    return full;
}

static void ExtractNupTree(IReadOnlyDictionary<string, (NupReader Reader, NupFile File)> map, string dest,
                           ImportProgress progress)
{
    var buf = new byte[1 << 20];
    Directory.CreateDirectory(dest);
    foreach (var (path, (reader, file)) in map.OrderBy(kv => kv.Key, StringComparer.Ordinal))
    {
        string target = SafeJoin(dest, path);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        using var input = reader.OpenFile(file);
        using var output = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20,
                                          FileOptions.SequentialScan);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        ulong left = file.Size;
        while (left > 0)
        {
            int n = input.Read(buf, 0, (int)Math.Min((ulong)buf.Length, left));
            if (n <= 0) throw new NupException($"the .nup file {path} ended early");
            hash.AppendData(buf, 0, n);
            output.Write(buf, 0, n);
            left -= (ulong)n;
            progress.Add(n);
        }
        if (file.Size > 0 && !hash.GetHashAndReset().AsSpan().SequenceEqual(file.Sha256))
            throw new NupException($"the .nup file {path} is damaged (SHA-256 mismatch)");
    }
}

static List<(IStorage St, string Path)> OpenDlcImages(List<Nca> dlcs, string outDir, out long bytes)
{
    var images = new List<(IStorage St, string Path)>();
    bytes = 0;
    if (dlcs.Count > 0) Directory.CreateDirectory(Path.Combine(outDir, "aoc"));
    foreach (var d in dlcs)
    {
        var st = d.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.None);
        st.GetSize(out long size).ThrowIfFailure();
        images.Add((st, Path.Combine(outDir, "aoc", $"{d.Header.TitleId:x16}.bin")));
        bytes += size;
    }
    return images;
}

static void WriteAddonImages(IReadOnlyList<(ulong TitleId, NupRomFs.Layout Layout)> nupAddons,
                             IReadOnlyList<(IStorage St, string Path)> dlcImages, string outDir, ImportProgress progress)
{
    if (nupAddons.Count + dlcImages.Count > 0) Directory.CreateDirectory(Path.Combine(outDir, "aoc"));
    foreach (var (titleId, layout) in nupAddons)
    {
        string path = Path.Combine(outDir, "aoc", $"{titleId:x16}.bin");
        using var output = File.Create(path + ".part");
        NupRomFs.Write(output, layout, n => progress.Add(checked((long)n)));
        output.Close();
        File.Move(path + ".part", path, true);
    }
    var buf = new byte[8 << 20];
    foreach (var (st, path) in dlcImages)
    {
        st.GetSize(out long size).ThrowIfFailure();
        using var f = File.Create(path + ".part");
        for (long off = 0; off < size; off += buf.Length)
        {
            int n = (int)Math.Min(buf.Length, size - off);
            st.Read(off, buf.AsSpan(0, n)).ThrowIfFailure();
            f.Write(buf, 0, n);
            progress.Add(n);
        }
        f.Close();
        File.Move(path + ".part", path, true);
    }
}

static void LogMainBuildId(string mainPath)
{
    if (!File.Exists(mainPath)) return;
    using var f = File.OpenRead(mainPath);
    var id = new byte[20];
    if (f.Length < 0x54 || f.Seek(0x40, SeekOrigin.Begin) != 0x40 || f.Read(id, 0, 20) != 20) return;
    Console.WriteLine($"main build ID {Convert.ToHexString(id).ToLowerInvariant()}");
}

internal sealed class ImportProgress
{
    private readonly long _total;
    private long _done, _reported;

    public ImportProgress(long total)
    {
        _total = total;
        Console.WriteLine($"progress 0 {total}");
    }

    public void Add(long n)
    {
        if (n <= 0) return;
        _done += n;
        if (_done - _reported >= (8 << 20) || _done >= _total)
        {
            _reported = _done;
            Console.WriteLine($"progress {_done} {_total}");
        }
    }
}
