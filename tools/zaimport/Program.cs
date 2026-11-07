// zaimport [--title-keys <file>] <prod.keys> <out dir> <package>...: the game files ZA Recomp runs from, made from the player's own Switch
// packages (.xci / .nsp: base game, update, DLC) and keys, as the console mounts them: <out>/exefs/ (the update's when
// there is one), <out>/romfs.bin (the base RomFS with the update applied: BKTR) and <out>/aoc/<title id>.bin (each
// DLC's RomFS). Title keys come from the packages' tickets, else --title-keys or title.keys next to prod.keys (also
// title.keys.txt: a name Windows' hidden extensions make easy to end up with).
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
using Path = System.IO.Path;


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

static int Run(string[] args)
{
    string? titleKeys = null;  // (--title-keys: the launcher found it by its contents, whatever its name)
    if (args.Length >= 2 && args[0] == "--title-keys") (titleKeys, args) = (args[1], args[2..]);
    if (args.Length < 3) return Fail("usage: zaimport [--title-keys <title.keys>] <prod.keys> <out dir> <package.xci|nsp>...");
    string keyDir = Path.GetDirectoryName(Path.GetFullPath(args[0]))!;
    titleKeys ??= new[] { "title.keys", "title.keys.txt" }.Select(n => Path.Combine(keyDir, n)).FirstOrDefault(File.Exists);
    var ks = ExternalKeyReader.ReadKeyFile(args[0], titleKeys);
    if (ks.HeaderKey.IsZeros())
        return Fail($"{Path.GetFileName(args[0])} has no header_key: it isn't a complete prod.keys. Dump the keys from " +
                    "your Switch with Lockpick_RCM and use its prod.keys");

    // every NCA of every package (the files stay open: their storages read from them)
    var ncas = new List<(Nca nca, string name)>();
    foreach (var pkg in args.Skip(2))
    {
        var st = new LocalStorage(pkg, FileAccess.Read);
        IFileSystem fs;
        if (IsXci(st))
        {
            var xci = new Xci(ks, st);
            if (!xci.HasPartition(XciPartitionType.Secure)) return Fail($"{Path.GetFileName(pkg)}: no secure partition");
            fs = xci.OpenPartition(XciPartitionType.Secure);
        }
        else
        {
            var pfs = new PartitionFileSystem();
            if (pfs.Initialize(st).IsFailure()) return Fail($"{Path.GetFileName(pkg)}: not an NSP or XCI");
            fs = pfs;
        }
        foreach (var e in fs.EnumerateEntries("/", "*.tik"))
        {
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            var t = new Ticket(f.Get.AsStream());
            var key = t.GetTitleKey(ks);
            if (key != null) ks.ExternalKeySet.Add(new RightsId(t.RightsId), new AccessKey(key));
        }
        foreach (var e in fs.EnumerateEntries("/", "*.nca"))
        {
            var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            ncas.Add((new Nca(ks, f.Release().AsStorage()), $"{Path.GetFileName(pkg)}/{e.Name}"));
        }
    }

    Nca? base_ = null, upd = null;
    var dlcs = new List<Nca>();
    // The games set up from their packages (update: +0x800, DLC: +0x1000 + n); the first one found among them is taken
    var Games = new (ulong id, string name)[] { (0x0100F43008C44000, "Pokémon Legends Z-A"),
                                                (0x01001F5010DFA000, "Pokémon Legends Arceus") };
    ulong ZaBase = Games.Select(g => g.id).FirstOrDefault(id => ncas.Any(n => n.nca.Header.TitleId == id));
    foreach (var (nca, name) in ncas)
    {
        ulong tid = nca.Header.TitleId;
        if (ZaBase == 0 || (tid & ~0x1FFFUL) != ZaBase) continue;
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
    if (base_ == null)
        return Fail(ncas.Count == 0 ? "no game packages found" : $"the base game of {string.Join(" or ", Games.Select(g => g.name))} isn't among these files");
    foreach (var n in new[] { base_, upd }.Concat(dlcs))
        if (n != null && n.Header.HasRightsId && !ks.ExternalKeySet.Contains(new RightsId(n.Header.RightsId)))
            return Fail($"no title key for {n.Header.TitleId:x16}: put title.keys from your Switch in the folder with " +
                        "the game files (packages without their ticket need it)");

    string outDir = args[1];
    Directory.CreateDirectory(Path.Combine(outDir, "exefs"));
    var code = upd ?? base_;
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
        }
    }
    Console.WriteLine($"exefs from {(upd != null ? "the update" : "the base game")}");

    var images = new List<(IStorage st, string path)>
    {
        (upd != null ? base_.OpenStorageWithPatch(upd, NcaSectionType.Data, IntegrityCheckLevel.None)
                     : base_.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.None),
         Path.Combine(outDir, "romfs.bin"))
    };
    if (dlcs.Count > 0) Directory.CreateDirectory(Path.Combine(outDir, "aoc"));
    foreach (var d in dlcs)
        images.Add((d.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.None),
                    Path.Combine(outDir, "aoc", $"{d.Header.TitleId:x16}.bin")));
    long total = 0, done = 0;
    foreach (var (st, _) in images)
    {
        st.GetSize(out long s).ThrowIfFailure();
        total += s;
    }
    var buf = new byte[8 << 20];
    foreach (var (st, path) in images)
    {
        st.GetSize(out long size).ThrowIfFailure();
        using var f = File.Create(path + ".part");
        for (long off = 0; off < size; off += buf.Length)
        {
            int n = (int)Math.Min(buf.Length, size - off);
            st.Read(off, buf.AsSpan(0, n)).ThrowIfFailure();
            f.Write(buf, 0, n);
            done += n;
            Console.WriteLine($"progress {done} {total}");
        }
        f.Close();
        File.Move(path + ".part", path, true);
    }
    Console.WriteLine($"done: update {(upd != null ? "applied" : "none")}, {dlcs.Count} DLC");
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
