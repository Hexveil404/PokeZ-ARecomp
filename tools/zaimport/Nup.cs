// NUP v1 reader: the clean-room container for unencrypted NX dumps (NUP\x01 superblock, TOC, zstd/raw extents).
// The format spec is https://github.com/a55uka/nup (SPEC.md). Only what ZA Recomp needs: validated package/dir/file
// tables, tree traversal, and a stream per file. The game's own files stay as they are in the user's .nup; nothing is
// decrypted here (a .nup is already plaintext).
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using ZstdSharp;

internal sealed class NupException : Exception
{
    public NupException(string message) : base(message) { }
}

internal sealed record NupPackage(
    int Index, ulong TitleId, uint Version, byte Kind, byte Flags,
    uint RootDir, uint ExefsDir, uint RomfsDir, ulong MetaOffset, uint MetaSize)
{
    public bool HasExefs => ExefsDir != uint.MaxValue;
    public bool HasRomfs => RomfsDir != uint.MaxValue;
    public string KindName => Kind switch
    {
        0 => "base",
        1 => "update",
        2 => "addon",
        3 => "addon_update",
        4 => "system",
        _ => $"kind{Kind}",
    };
}

internal sealed record NupFile(
    string Name, ulong Size, byte[] Sha256, ulong ExtentOffset, ulong ExtentSize, byte Codec);

internal readonly record struct NupNode(bool IsDir, int Index, string Name);

internal sealed class NupReader
{
    private const int HeaderSize = 0x80;
    private const int PackageSize = 0x40;
    private const int DirSize = 0x18;
    private const int FileSize = 0x50;
    private const int TocHeaderSize = 0x30;
    private const int ExtentAlignment = 16;
    private const int MaxTocSize = 256 << 20;
    private const uint TocMagic = 0x01434F54;  // 'T','O','C',1

    private static readonly UTF8Encoding Utf8 = new(false, true);

    private (string Name, uint FirstChild, uint ChildCount)[] _dirs = Array.Empty<(string, uint, uint)>();
    private NupFile[] _files = Array.Empty<NupFile>();
    private uint[] _children = Array.Empty<uint>();

    public string Path { get; }
    public IReadOnlyList<NupPackage> Packages { get; }
    public int DirCount => _dirs.Length;
    public int FileCount => _files.Length;

    public NupReader(string path)
    {
        Path = path;
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        long length = stream.Length;
        if (length < HeaderSize) throw new NupException($"{Path}: this file is too small to be a NUP");

        Span<byte> h = stackalloc byte[HeaderSize];
        stream.ReadExactly(h);
        if (!h[..4].SequenceEqual("NUP\x01"u8))
        {
            byte version = h[3];
            throw new NupException(version == (byte)'P' || h[..3].SequenceEqual("NUP"u8)
                ? $"{Path}: NUP version {version} is not supported (this build reads NUP v1)"
                : $"{Path}: not a NUP (it starts with {Convert.ToHexString(h[..4])})");
        }

        ushort headerSize = BinaryPrimitives.ReadUInt16LittleEndian(h[4..]);
        ushort headerVersion = BinaryPrimitives.ReadUInt16LittleEndian(h[6..]);
        ulong tocOffset = BinaryPrimitives.ReadUInt64LittleEndian(h[8..]);
        ulong tocSize = BinaryPrimitives.ReadUInt64LittleEndian(h[0x10..]);
        ulong blobOffset = BinaryPrimitives.ReadUInt64LittleEndian(h[0x38..]);
        ulong fileSize = BinaryPrimitives.ReadUInt64LittleEndian(h[0x40..]);
        uint packageCount = BinaryPrimitives.ReadUInt32LittleEndian(h[0x48..]);
        uint flags = BinaryPrimitives.ReadUInt32LittleEndian(h[0x4C..]);

        if (headerSize < HeaderSize || headerVersion != 1)
            throw new NupException($"{Path}: unsupported NUP header (size {headerSize}, version {headerVersion})");
        if (tocOffset != headerSize)
            throw new NupException($"{Path}: the NUP table of contents is not where the header says");
        if (tocSize == 0 || tocSize > MaxTocSize || (ulong)length < tocOffset + tocSize)
            throw new NupException($"{Path}: the NUP table of contents is missing or too large");
        if (blobOffset < tocOffset + tocSize || blobOffset % ExtentAlignment != 0)
            throw new NupException($"{Path}: the NUP data region is misplaced");
        if (fileSize != (ulong)length)
            throw new NupException($"{Path}: the .nup file is cut short ({length} bytes, header says {fileSize})");
        if (packageCount == 0)
            throw new NupException($"{Path}: the NUP contains no packages");
        if (flags != 0)
            throw new NupException($"{Path}: unsupported NUP flags {flags:#x}");

        var toc = new byte[(int)tocSize];
        stream.Position = (long)tocOffset;
        stream.ReadExactly(toc);
        if (!SHA256.HashData(toc).AsSpan().SequenceEqual(h.Slice(0x18, 32)))
            throw new NupException($"{Path}: the NUP table of contents is damaged (SHA-256 mismatch)");

        var packages = ParseToc(toc, packageCount);
        Validate(packages, blobOffset, fileSize);
        Packages = packages;
    }

    private NupPackage[] ParseToc(byte[] toc, uint superPackageCount)
    {
        var span = toc.AsSpan();
        if (span.Length < TocHeaderSize)
            throw new NupException($"{Path}: the NUP table of contents is truncated");
        if (BinaryPrimitives.ReadUInt32LittleEndian(span) != TocMagic)
            throw new NupException($"{Path}: damaged NUP table of contents (bad magic)");

        uint packageCount = BinaryPrimitives.ReadUInt32LittleEndian(span[4..]);
        ulong dirCount64 = BinaryPrimitives.ReadUInt64LittleEndian(span[8..]);
        ulong fileCount64 = BinaryPrimitives.ReadUInt64LittleEndian(span[0x10..]);
        ulong poolOffset64 = BinaryPrimitives.ReadUInt64LittleEndian(span[0x18..]);
        ulong poolSize64 = BinaryPrimitives.ReadUInt64LittleEndian(span[0x20..]);
        if (packageCount != superPackageCount)
            throw new NupException($"{Path}: the NUP package count disagrees with its header");
        if (dirCount64 > int.MaxValue || fileCount64 > int.MaxValue)
            throw new NupException($"{Path}: the NUP has too many entries");
        if (poolOffset64 > (ulong)span.Length || poolSize64 > (ulong)span.Length - poolOffset64)
            throw new NupException($"{Path}: the NUP string table is outside the table of contents");

        int dirCount = (int)dirCount64, fileCount = (int)fileCount64;
        int poolOffset = (int)poolOffset64, poolSize = (int)poolSize64;
        long need = TocHeaderSize + (long)packageCount * PackageSize + (long)dirCount * DirSize + (long)fileCount * FileSize;
        if (need > span.Length || (ulong)need > poolOffset64)
            throw new NupException($"{Path}: the NUP table of contents is truncated");
        if ((poolOffset64 - (ulong)need) % 4 != 0)
            throw new NupException($"{Path}: the NUP child table is not aligned");
        int childSlots = (int)((poolOffset64 - (ulong)need) / 4);
        var pool = span.Slice(poolOffset, poolSize);

        var packages = new NupPackage[packageCount];
        int at = TocHeaderSize;
        for (int i = 0; i < packageCount; i++, at += PackageSize)
        {
            ulong titleId = BinaryPrimitives.ReadUInt64LittleEndian(span[at..]);
            uint root = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 8)..]);
            uint exefs = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 0x0C)..]);
            uint romfs = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 0x10)..]);
            uint version = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 0x14)..]);
            byte kind = span[at + 0x18], flags = span[at + 0x19];
            ulong metaOffset = BinaryPrimitives.ReadUInt64LittleEndian(span[(at + 0x20)..]);
            uint metaSize = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 0x28)..]);
            if (kind >= 5)
                throw new NupException($"{Path}: package {i} has unknown kind {kind} (this build knows NUP v1 kinds 0-4)");
            if ((flags & ~1) != 0)
                throw new NupException($"{Path}: package {i} has unknown flags {flags:#x}");
            packages[i] = new NupPackage(i, titleId, version, kind, flags, root, exefs, romfs, metaOffset, metaSize);
        }

        _dirs = new (string, uint, uint)[dirCount];
        for (int i = 0; i < dirCount; i++, at += DirSize)
        {
            uint nameOffset = BinaryPrimitives.ReadUInt32LittleEndian(span[at..]);
            uint nameLength = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 4)..]);
            uint firstChild = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 8)..]);
            uint childCount = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 0x0C)..]);
            _dirs[i] = (ReadString(pool, nameOffset, nameLength), firstChild, childCount);
        }

        _files = new NupFile[fileCount];
        for (int i = 0; i < fileCount; i++, at += FileSize)
        {
            uint nameOffset = BinaryPrimitives.ReadUInt32LittleEndian(span[at..]);
            uint nameLength = BinaryPrimitives.ReadUInt32LittleEndian(span[(at + 4)..]);
            ulong size = BinaryPrimitives.ReadUInt64LittleEndian(span[(at + 8)..]);
            ulong extentOffset = BinaryPrimitives.ReadUInt64LittleEndian(span[(at + 0x10)..]);
            ulong extentSize = BinaryPrimitives.ReadUInt64LittleEndian(span[(at + 0x18)..]);
            byte[] sha = span.Slice(at + 0x20, 32).ToArray();
            byte codec = span[at + 0x40], codecFlags = span[at + 0x41];
            if (codecFlags != 0)
                throw new NupException($"{Path}: file {i} uses unsupported codec flags {codecFlags}");
            if (codec > 1)
                throw new NupException($"{Path}: file {i} uses unsupported codec {codec} (this build reads raw and zstd)");
            _files[i] = new NupFile(ReadString(pool, nameOffset, nameLength), size, sha, extentOffset, extentSize, codec);
        }

        _children = new uint[childSlots];
        for (int i = 0; i < childSlots; i++, at += 4)
            _children[i] = BinaryPrimitives.ReadUInt32LittleEndian(span[at..]);
        if (at != poolOffset)
            throw new NupException($"{Path}: the NUP child table is larger than its directory entries say");

        ulong usedSlots = 0;
        for (int i = 0; i < dirCount; i++)
        {
            var (name, first, count) = _dirs[i];
            _ = name;
            if ((ulong)first + count > (ulong)childSlots)
                throw new NupException($"{Path}: directory {i}'s children are outside the NUP child table");
            usedSlots += count;
        }
        if (usedSlots != (ulong)childSlots)
            throw new NupException($"{Path}: the NUP child table has unowned entries");
        foreach (uint child in _children)
            if (child >= (uint)(dirCount + fileCount))
                throw new NupException($"{Path}: a NUP directory points at a missing entry");

        return packages;
    }

    private void Validate(NupPackage[] packages, ulong blobOffset, ulong fileSize)
    {
        int bases = 0;
        bool hasBase = packages.Any(p => p.Kind == 0);
        foreach (var p in packages)
        {
            if (p.RootDir >= (uint)_dirs.Length)
                throw new NupException($"{Path}: package {p.Index} points at a missing root directory");
            if (p.HasExefs && p.ExefsDir >= (uint)_dirs.Length)
                throw new NupException($"{Path}: package {p.Index} points at a missing exefs directory");
            if (p.HasRomfs && p.RomfsDir >= (uint)_dirs.Length)
                throw new NupException($"{Path}: package {p.Index} points at a missing romfs directory");
            if (p.MetaSize != 0 && (p.MetaOffset > fileSize || p.MetaSize > fileSize - p.MetaOffset))
                throw new NupException($"{Path}: package {p.Index}'s metadata is outside the file");
            if (p.Kind == 0) bases++;
            if (p.Kind == 1 && !hasBase && (p.Flags & 1) == 0)
                throw new NupException($"{Path}: package {p.Index} is an update without a base or the standalone flag");
        }
        if (bases > 1)
            throw new NupException($"{Path}: the NUP has more than one base package");

        foreach (var f in _files)
        {
            if (f.Size == 0)
            {
                if (f.ExtentSize != 0 || f.ExtentOffset != 0 || f.Codec != 0)
                    throw new NupException($"{Path}: empty file {f.Name} has data in the NUP");
                continue;
            }
            if (f.ExtentSize == 0 || f.ExtentOffset < blobOffset ||
                f.ExtentOffset > fileSize || f.ExtentSize > fileSize - f.ExtentOffset ||
                f.ExtentOffset % ExtentAlignment != 0)
                throw new NupException($"{Path}: file {f.Name} has an invalid data extent");
        }
    }

    private string ReadString(ReadOnlySpan<byte> pool, uint offset, uint length)
    {
        if (offset > (uint)pool.Length || length > (uint)pool.Length - offset)
            throw new NupException($"{Path}: a NUP name points outside the string table");
        try
        {
            return Utf8.GetString(pool.Slice((int)offset, (int)length));
        }
        catch (DecoderFallbackException)
        {
            throw new NupException($"{Path}: a NUP name is not valid UTF-8");
        }
    }

    public IEnumerable<NupNode> ChildrenOf(uint dirIndex)
    {
        if (dirIndex >= (uint)_dirs.Length)
            throw new NupException($"{Path}: directory {dirIndex} is missing");
        var dir = _dirs[(int)dirIndex];
        for (uint i = 0; i < dir.ChildCount; i++)
        {
            uint id = _children[dir.FirstChild + i];
            if (id < (uint)_dirs.Length)
                yield return new NupNode(true, (int)id, _dirs[(int)id].Name);
            else
            {
                int file = (int)(id - (uint)_dirs.Length);
                yield return new NupNode(false, file, _files[file].Name);
            }
        }
    }

    // The relative paths of every file under a package's exefs or romfs directory. Directory names are joined with '/'.
    public IEnumerable<(string Path, NupFile File)> EnumerateFiles(uint dirIndex)
    {
        var active = new HashSet<uint>();
        return Walk(dirIndex, "", active, 0);
    }

    private IEnumerable<(string Path, NupFile File)> Walk(uint dirIndex, string prefix, HashSet<uint> active, int depth)
    {
        if (depth > 64)
            throw new NupException($"{Path}: directories are nested too deeply");
        if (!active.Add(dirIndex))
            throw new NupException($"{Path}: a NUP directory contains itself");
        var dir = _dirs[(int)dirIndex];
        for (uint i = 0; i < dir.ChildCount; i++)
        {
            uint id = _children[dir.FirstChild + i];
            if (id < (uint)_dirs.Length)
            {
                string name = _dirs[(int)id].Name;
                if (name.Length == 0 || name is "." or ".." || name.Contains('/') || name.Contains('\\'))
                    throw new NupException($"{Path}: a NUP directory has an unsafe name");
                foreach (var entry in Walk(id, prefix + name + "/", active, depth + 1))
                    yield return entry;
            }
            else
            {
                int index = (int)(id - (uint)_dirs.Length);
                var file = _files[index];
                if (file.Name.Length == 0 || file.Name is "." or ".." || file.Name.Contains('/') || file.Name.Contains('\\'))
                    throw new NupException($"{Path}: a NUP file has an unsafe name");
                yield return (prefix + file.Name, file);
            }
        }
        active.Remove(dirIndex);
    }

    // A stream over the file's uncompressed content: the NUP extent (raw or one zstd frame), bounded to its size.
    public Stream OpenFile(NupFile file)
    {
        var stream = new FileStream(Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20,
                                    FileOptions.SequentialScan);
        stream.Position = checked((long)file.ExtentOffset);
        var bounded = new BoundedStream(stream, checked((long)file.ExtentSize));
        return file.Codec switch
        {
            0 => bounded,
            1 => new DecompressionStream(bounded),
            _ => throw new NupException($"{Path}: file {file.Name} uses unsupported codec {file.Codec}"),
        };
    }
}

internal sealed class BoundedStream : Stream
{
    private readonly Stream _inner;
    private readonly long _length;
    private long _remaining;
    private long _position;

    public BoundedStream(Stream inner, long length)
    {
        _inner = inner;
        _length = _remaining = length;
    }

    public override bool CanRead => _remaining > 0 || _inner.CanRead;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => _length;
    public override long Position
    {
        get => _position;
        set => throw new NotSupportedException();
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        int want = (int)Math.Min(count, _remaining);
        if (want <= 0) return 0;
        int got = _inner.Read(buffer, offset, want);
        if (got <= 0) return 0;
        _remaining -= got;
        _position += got;
        return got;
    }

    public override int Read(Span<byte> buffer)
    {
        int want = (int)Math.Min(buffer.Length, _remaining);
        if (want <= 0) return 0;
        int got = _inner.Read(buffer[..want]);
        if (got <= 0) return 0;
        _remaining -= got;
        _position += got;
        return got;
    }

    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

    protected override void Dispose(bool disposing)
    {
        if (disposing) _inner.Dispose();
        base.Dispose(disposing);
    }
}
