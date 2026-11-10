// Builds a RomFS image from a NUP add-on tree. The layout is the one the game's own RomFS code walks (the same one
// runtime/romfs_build.c makes for the player's romfs/ folder): a 0x50-byte header, file data from 0x200 (16-byte
// aligned), then the directory hash table, directory entries, file hash table and file entries. A .nup stores an add-on
// as a plain tree rather than as an NCA data section, so the add-on has to be put back into this image before the
// runtime's aoc service can mount it (runtime/services/fs.c OpenDataStorageByDataId reads aoc/<title id>.bin).
using System.Security.Cryptography;
using System.Text;

internal static class NupRomFs
{
    private const ulong DataStart = 0x200;
    private const uint Empty = 0xFFFFFFFF;
    private static readonly byte[] Zeros = new byte[1 << 20];

    public sealed record Entry(string Path, ulong Size, byte[] Sha256, Func<Stream> Open);

    internal sealed class Layout
    {
        internal readonly List<FileNode> Files;
        internal readonly byte[] Header;
        internal readonly byte[] Tables;
        internal readonly ulong TablesOffset;
        internal ulong ImageSize { get; }

        internal Layout(List<FileNode> files, byte[] header, byte[] tables, ulong tablesOffset, ulong imageSize)
        {
            Files = files;
            Header = header;
            Tables = tables;
            TablesOffset = tablesOffset;
            ImageSize = imageSize;
        }
    }

    internal sealed class Dir
    {
        public string Name = "";
        public byte[] NameBytes = Array.Empty<byte>();
        public Dir? Parent;
        public readonly List<Dir> Dirs = new();
        public readonly List<FileNode> Files = new();
        public uint EntryOff;
        public uint NameLen;
    }

    internal sealed class FileNode
    {
        public string Name = "";
        public string Path = "";
        public byte[] NameBytes = Array.Empty<byte>();
        public ulong Size;
        public byte[] Sha256 = new byte[32];
        public Func<Stream> Open = null!;
        public Dir Parent = null!;
        public uint EntryOff;
        public uint NameLen;
        public ulong DataOff;
    }

    public static Layout Build(IEnumerable<Entry> entries)
    {
        var root = new Dir { Name = "" };
        foreach (var entry in entries)
        {
            if (entry.Sha256.Length != 32) throw new NupException($"{entry.Path}: NUP SHA-256 is not 32 bytes");
            string[] parts = entry.Path.Split('/');
            if (parts.Length == 0 || parts.Any(p => p.Length == 0 || p is "." or ".." ||
                                                      p.Contains('\\') || p.Contains('\0')))
                throw new NupException($"the add-on has an unsafe path: {entry.Path}");
            var dir = root;
            for (int i = 0; i + 1 < parts.Length; i++)
            {
                var child = dir.Dirs.FirstOrDefault(d => d.Name == parts[i]);
                if (child == null)
                {
                    child = new Dir { Name = parts[i], Parent = dir };
                    dir.Dirs.Add(child);
                }
                dir = child;
            }
            string name = parts[^1];
            if (dir.Files.Any(f => f.Name == name))
                throw new NupException($"the add-on has two files named {entry.Path}");
            dir.Files.Add(new FileNode
            {
                Name = name, Path = entry.Path, Size = entry.Size, Sha256 = entry.Sha256,
                Open = entry.Open, Parent = dir,
            });
        }
        SortTree(root);

        var dirs = new List<Dir> { root };
        var files = new List<FileNode>();
        Flatten(root, dirs, files);

        uint dsize = 0, fsize = 0;
        foreach (var d in dirs)
        {
            d.NameBytes = Encoding.UTF8.GetBytes(d.Name);
            d.NameLen = (uint)d.NameBytes.Length;
            d.EntryOff = dsize;
            dsize += 0x18 + Align4(d.NameLen);
        }
        foreach (var f in files)
        {
            f.NameBytes = Encoding.UTF8.GetBytes(f.Name);
            f.NameLen = (uint)f.NameBytes.Length;
            f.EntryOff = fsize;
            fsize += 0x20 + Align4(f.NameLen);
        }

        ulong data = 0;
        foreach (var f in files)
        {
            data = Align16(data);
            f.DataOff = data;
            data += f.Size;
        }

        uint nbd = BucketCount((uint)dirs.Count), nbf = BucketCount((uint)files.Count);
        var dbuckets = new uint[nbd];
        var fbuckets = new uint[nbf];
        Array.Fill(dbuckets, Empty);
        Array.Fill(fbuckets, Empty);
        var dtab = new byte[dsize];
        var ftab = new byte[fsize];

        foreach (var d in dirs)
        {
            uint parent = d.Parent?.EntryOff ?? 0;
            uint sibling = Empty;
            if (d.Parent != null)
            {
                int at = d.Parent.Dirs.IndexOf(d);
                if (at >= 0 && at + 1 < d.Parent.Dirs.Count) sibling = d.Parent.Dirs[at + 1].EntryOff;
            }
            uint firstDir = d.Dirs.Count > 0 ? d.Dirs[0].EntryOff : Empty;
            uint firstFile = d.Files.Count > 0 ? d.Files[0].EntryOff : Empty;
            uint hash = NameHash(parent, d.Name) % nbd;
            int e = (int)d.EntryOff;
            Put32(dtab, e, parent);
            Put32(dtab, e + 4, sibling);
            Put32(dtab, e + 8, firstDir);
            Put32(dtab, e + 12, firstFile);
            Put32(dtab, e + 16, dbuckets[hash]);
            Put32(dtab, e + 20, d.NameLen);
            d.NameBytes.CopyTo(dtab.AsSpan(e + 0x18));
            dbuckets[hash] = d.EntryOff;
        }

        foreach (var f in files)
        {
            uint parent = f.Parent.EntryOff;
            uint sibling = Empty;
            int at = f.Parent.Files.IndexOf(f);
            if (at >= 0 && at + 1 < f.Parent.Files.Count) sibling = f.Parent.Files[at + 1].EntryOff;
            uint hash = NameHash(parent, f.Name) % nbf;
            int e = (int)f.EntryOff;
            Put32(ftab, e, parent);
            Put32(ftab, e + 4, sibling);
            Put64(ftab, e + 8, f.DataOff);
            Put64(ftab, e + 16, f.Size);
            Put32(ftab, e + 24, fbuckets[hash]);
            Put32(ftab, e + 28, f.NameLen);
            f.NameBytes.CopyTo(ftab.AsSpan(e + 0x20));
            fbuckets[hash] = f.EntryOff;
        }

        ulong dhashOff = Align4(DataStart + data);
        ulong dmetaOff = dhashOff + 4UL * nbd;
        ulong fhashOff = dmetaOff + dsize;
        ulong fmetaOff = fhashOff + 4UL * nbf;
        ulong imageSize = fmetaOff + fsize;
        if (imageSize > int.MaxValue)
            throw new NupException("the add-on RomFS image is too large");

        var header = new byte[0x50];
        Put64(header, 0, 0x50);
        Put64(header, 8, dhashOff);
        Put64(header, 16, 4UL * nbd);
        Put64(header, 24, dmetaOff);
        Put64(header, 32, dsize);
        Put64(header, 40, fhashOff);
        Put64(header, 48, 4UL * nbf);
        Put64(header, 56, fmetaOff);
        Put64(header, 64, fsize);
        Put64(header, 72, DataStart);

        var tables = new byte[(int)(imageSize - dhashOff)];
        int t = 0;
        Buffer.BlockCopy(dbuckets, 0, tables, t, dbuckets.Length * 4);
        t += dbuckets.Length * 4;
        Buffer.BlockCopy(dtab, 0, tables, t, dtab.Length);
        t += dtab.Length;
        Buffer.BlockCopy(fbuckets, 0, tables, t, fbuckets.Length * 4);
        t += fbuckets.Length * 4;
        Buffer.BlockCopy(ftab, 0, tables, t, ftab.Length);

        return new Layout(files, header, tables, dhashOff, imageSize);
    }

    public static void Write(Stream output, Layout layout, Action<ulong>? progress = null)
    {
        long written = 0;
        var buffer = new byte[1 << 20];

        void Write(ReadOnlySpan<byte> data)
        {
            output.Write(data);
            written += data.Length;
        }
        void PadTo(ulong target)
        {
            while ((ulong)written < target)
            {
                int n = (int)Math.Min((ulong)Zeros.Length, target - (ulong)written);
                output.Write(Zeros, 0, n);
                written += n;
            }
        }

        Write(layout.Header);
        PadTo(DataStart);
        foreach (var f in layout.Files)
        {
            PadTo(DataStart + f.DataOff);
            using var input = f.Open();
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            ulong left = f.Size;
            while (left > 0)
            {
                int n = input.Read(buffer, 0, (int)Math.Min((ulong)buffer.Length, left));
                if (n <= 0) throw new NupException($"the add-on file {f.Path} ended early");
                hash.AppendData(buffer, 0, n);
                output.Write(buffer, 0, n);
                written += n;
                left -= (ulong)n;
            }
            if (f.Size > 0 && !hash.GetHashAndReset().AsSpan().SequenceEqual(f.Sha256))
                throw new NupException($"the add-on file {f.Path} is damaged (SHA-256 mismatch)");
        }
        PadTo(layout.TablesOffset);
        Write(layout.Tables);
        if ((ulong)written != layout.ImageSize)
            throw new NupException("internal error: the add-on RomFS image size is wrong");
        progress?.Invoke(layout.ImageSize);
    }

    private static void SortTree(Dir dir)
    {
        dir.Dirs.Sort((a, b) => CompareNames(a.Name, b.Name));
        dir.Files.Sort((a, b) => CompareNames(a.Name, b.Name));
        foreach (var child in dir.Dirs) SortTree(child);
    }

    private static int CompareNames(string a, string b) =>
        Encoding.UTF8.GetBytes(a).AsSpan().SequenceCompareTo(Encoding.UTF8.GetBytes(b));

    private static void Flatten(Dir dir, List<Dir> dirs, List<FileNode> files)
    {
        files.AddRange(dir.Files);
        dirs.AddRange(dir.Dirs);
        foreach (var child in dir.Dirs) Flatten(child, dirs, files);
    }

    private static uint NameHash(uint parent, string name)
    {
        uint h = parent ^ 123456789u;
        foreach (byte b in Encoding.UTF8.GetBytes(name))
            h = ((h >> 5) | (h << 27)) ^ b;
        return h;
    }

    private static uint BucketCount(uint n)
    {
        if (n < 3) return 3;
        if (n < 19) return n | 1;
        uint c = n;
        while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
        return c;
    }

    private static uint Align4(uint x) => (x + 3) & ~3u;
    private static ulong Align4(ulong x) => (x + 3) & ~3ul;
    private static ulong Align16(ulong x) => (x + 15) & ~15ul;
    private static void Put32(byte[] p, int at, uint v) => System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(at), v);
    private static void Put64(byte[] p, int at, ulong v) => System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(p.AsSpan(at), v);
}
