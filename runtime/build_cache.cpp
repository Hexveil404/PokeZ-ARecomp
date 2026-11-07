// The game-code builder's cache: receipts validate completed objects; an OS lock keeps builders apart.
// Generated source and logs survive interruption, but are removed after successful publication.
#include "build_cache.h"
#include "cache_sha256.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace fs = std::filesystem;
struct BuildCache {
    fs::path root;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~BuildCache() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    int handle = -1;
    ~BuildCache() {
        if (handle >= 0) {
            flock(handle, LOCK_UN);
            close(handle);
        }
    }
#endif
};
namespace {
void need(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void error(char *out, size_t cap, const std::exception &e) {
    if (cap)
        snprintf(out, cap, "%s", e.what());
}
bool is_link(const fs::path &p) {
#ifdef _WIN32
    // Cloud placeholders also have REPARSE_POINT: only symlinks and junctions redirect the path.
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(p.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
#else
    return fs::is_symlink(fs::symlink_status(p));
#endif
}
void plain(const fs::path &p) {
    fs::path part;
    bool inside = false;
    for (const auto &c : fs::absolute(p)) {
        part /= c;
        if (c == ".gamebuild") inside = true;
        if (inside) need(!is_link(part), "The build cache cannot contain symlinks or junctions.");
    }
    if (!inside) need(!is_link(p), "Build files cannot be symlinks or junctions.");
}
void plain_tree(const fs::path &p) {
    plain(p);
    for (auto it = fs::recursive_directory_iterator(p); it != fs::recursive_directory_iterator(); ++it)
        need(it.depth() < 32 && !is_link(it->path()), "Unsafe build cache entry.");
}
std::string digest(const fs::path &p) {
    plain(p);
    need(fs::is_regular_file(p), "A build input is missing.");
    std::ifstream f(p, std::ios::binary);
    need(bool(f), "Cannot read a build input.");
    CacheSha256 hash;
    char block[65536];
    while (f.read(block, sizeof block) || f.gcount())
        hash.add(block, (size_t)f.gcount());
    need(f.eof(), "Cannot finish reading a build input.");
    return hash.finish();
}
std::string read(const fs::path &p, uintmax_t limit = 1 << 20) {
    plain(p);
    need(fs::file_size(p) <= limit, "Invalid build cache record.");
    std::ifstream f(p, std::ios::binary);
    need(bool(f), "Cannot read the build cache.");
    return {std::istreambuf_iterator<char>(f), {}};
}
void replace(const fs::path &from, const fs::path &to) {
    plain(from);
    plain(to);
#ifdef _WIN32
    need(MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
         "Cannot replace a build file.");
#else
    fs::rename(from, to);
#endif
}
void write(const fs::path &p, const std::string &data) {
    plain(p);
    auto tmp = p;
    tmp += ".writing";
    plain(tmp);
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << data;
    f.close();
    need(bool(f), "Cannot write a build cache record.");
    replace(tmp, p);
}
bool valid_stem(const std::string &s) {
    return !s.empty() && s.size() < 64 &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}
std::string generation(BuildCache *c, const char *module, int chunks) {
    need((!strcmp(module, "main") || !strcmp(module, "sdk")) && chunks > 0 && chunks <= 10000,
         "Invalid generated module.");
    std::ostringstream out;
    out << "LASGEN1\n" << chunks << '\n';
    for (int i = 0; i <= chunks; ++i) {
        char name[80];
        if (i == chunks)
            snprintf(name, sizeof name, "%s_table.c", module);
        else
            snprintf(name, sizeof name, "%s_%03d.c", module, i);
        out << name << ' ' << digest(c->root / module / name) << '\n';
    }
    return out.str();
}
std::unique_ptr<BuildCache> acquire(const char *game_dir) {
    auto c = std::make_unique<BuildCache>();
    // The selected game folder may live below /home's symlink or a Windows junction.
    c->root = fs::weakly_canonical(fs::absolute(fs::u8path(game_dir))) / ".gamebuild";
    plain(c->root);
    fs::create_directories(c->root);
    // Keep the lock file in place: unlinking it would let a second builder lock a different file.
    // Closing the OS handle releases ownership even when the application exits during a build.
    auto lock = c->root / ".lock";
    plain(lock);
#ifdef _WIN32
    c->handle =
        CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    need(c->handle != INVALID_HANDLE_VALUE, "This game folder is already being compiled by another process.");
#else
    c->handle = open(lock.c_str(), O_CREAT | O_RDWR, 0600);
    need(c->handle >= 0 && flock(c->handle, LOCK_EX | LOCK_NB) == 0,
         "This game folder is already being compiled by another process.");
#endif
    plain_tree(c->root);
    return c;
}
bool library_name(const std::string &s) {
    return s == "lasgame.dll" || s == "lasgame.so" || s == "lasgame.dylib";
}
void recover(BuildCache *c) {
    // The library and its ID cannot be replaced together. A remaining journal means restore the old pair
    // before a retry or cleanup can discard the backups; repeatable recovery also survives another exit.
    auto journal = c->root / "install-journal";
    if (!fs::exists(journal))
        return;
    std::istringstream in(read(journal, 256));
    std::string magic, name;
    int had_lib, had_id;
    need(bool(in >> magic >> name >> had_lib >> had_id) && magic == "LASINSTALL1" && library_name(name) &&
             (had_lib == 0 || had_lib == 1) && (had_id == 0 || had_id == 1),
         "Invalid interrupted library installation.");
    const char *old[] = {"previous-library", "previous-id"};
    for (int i = 0; i < 2; ++i) {
        auto destination = c->root.parent_path() / (name + (i ? ".id" : ""));
        plain(destination);
        if (i ? had_id : had_lib) {
            auto tmp = c->root / (std::string(old[i]) + ".restore");
            plain(tmp);
            fs::copy_file(c->root / old[i], tmp, fs::copy_options::overwrite_existing);
            replace(tmp, destination);
        } else if (fs::exists(destination))
            fs::remove(destination);
    }
    fs::remove(journal);
}
void clear_old_zig_cache(BuildCache *c) {
    auto old = c->root.parent_path() / ".zigcache";
    if (fs::exists(old)) { plain_tree(old); fs::remove_all(old); }
}
void finish(BuildCache *c) {
    // Only objects and their input identities are useful to the next update. Regeneration is cheap compared
    // with compiling; its receipts cannot remain valid once the generated C has been removed.
    // Keep source/object receipts for content comparison after regeneration, but drop the unfinished-build marker.
    plain_tree(c->root);
    for (auto &e : fs::directory_iterator(c->root)) {
        auto name = e.path().filename().u8string();
        if (name == ".lock" || name == "owner" || name == "identity" || name == "generation-identity") continue;
        auto ext = e.path().extension();
        if ((ext == ".o" || ext == ".receipt") && valid_stem(e.path().stem().u8string())) continue;
        fs::remove_all(e.path());
    }
    clear_old_zig_cache(c);
}
} // namespace
extern "C" BuildCache *build_cache_open(const char *game_dir, const char *spec, const char *const *inputs, int n,
                                        char *why, size_t cap) {
    try {
        auto c = acquire(game_dir);
        auto owner = c->root / "owner";
        if (fs::exists(owner))
            need(read(owner, 64) == "LASBUILD3\n", "Unknown build cache; clear it from Game Files and retry.");
        else {
            // Pre-receipt beta_5 caches are ours too; upgrade them without asking every existing player to clear.
            recover(c.get());
            for (auto &e : fs::directory_iterator(c->root))
                if (e.path().filename() != ".lock") fs::remove_all(e.path());
            write(owner, "LASBUILD3\n");
        }
        recover(c.get());
        CacheSha256 key;
        key.add(spec, strlen(spec) + 1);
        for (int i = 0; i < n; ++i) {
            auto h = digest(fs::canonical(fs::u8path(inputs[i])));
            key.add(h.data(), h.size());
        }
        std::string identity = "LASBUILD3\n" + key.finish() + "\n";
        auto record = c->root / "identity";
        if (!fs::exists(record) || read(record, 128) != identity) {
            for (auto &e : fs::directory_iterator(c->root))
                if (e.path().filename() != ".lock" && e.path().filename() != "owner")
                    fs::remove_all(e.path());
            write(record, identity);
        }
        write(c->root / "in-progress", "LASBUILD3\n");
        return c.release();
    } catch (const std::exception &e) {
        error(why, cap, e);
        return NULL;
    }
}
// A changed generator invalidates source receipts, but retains object receipts to compare regenerated C.
extern "C" bool build_cache_prepare_generation(BuildCache *c, const char *spec, const char *const *inputs, int n,
                                                char *why, size_t cap) {
    try {
        CacheSha256 key;
        key.add(spec, strlen(spec) + 1);
        for (int i = 0; i < n; ++i) {
            auto h = digest(fs::canonical(fs::u8path(inputs[i])));
            key.add(h.data(), h.size());
        }
        std::string identity = "LASGENKEY1\n" + key.finish() + "\n";
        auto record = c->root / "generation-identity";
        if (!fs::exists(record) || read(record, 128) != identity) {
            for (const char *name : {"main.generation", "sdk.generation", "subsdk0.generation", "subsdk1.generation",
                                     "main", "sdk", "subsdk0", "subsdk1"})
                fs::remove_all(c->root / name);
            write(record, identity);
        }
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
extern "C" void build_cache_close(BuildCache *c) {
    delete c;
}
extern "C" void build_cache_failed(BuildCache *c) {
    std::error_code ec;
    fs::remove(c->root / "in-progress", ec); // A reported failure is a normal retry, not an interrupted build.
}
extern "C" bool build_cache_resumable(const char *game_dir) {
    try {
        auto root = fs::weakly_canonical(fs::absolute(fs::u8path(game_dir))) / ".gamebuild";
        return read(root / "owner", 64) == "LASBUILD3\n" && read(root / "in-progress", 64) == "LASBUILD3\n";
    } catch (...) { return false; }
}
extern "C" bool build_cache_present(const char *game_dir) {
    try {
        auto game = fs::weakly_canonical(fs::absolute(fs::u8path(game_dir)));
        if (fs::exists(game / ".zigcache")) return true;
        std::error_code ec;
        for (auto &e : fs::directory_iterator(game / ".gamebuild", ec)) {
            auto name = e.path().filename();
            if (name != ".lock" && name != "owner") return true;
        }
        return false;
    } catch (...) { return false; }
}
extern "C" bool build_cache_generation(BuildCache *c, const char *module, int *chunks) {
    try {
        need(!strcmp(module, "main") || !strcmp(module, "sdk"), "Invalid module name.");
        auto record = read(c->root / (std::string(module) + ".generation"));
        std::istringstream in(record);
        std::string magic;
        int n;
        need(bool(in >> magic >> n) && magic == "LASGEN1", "Invalid generation record.");
        if (record != generation(c, module, n))
            return false;
        *chunks = n;
        return true;
    } catch (...) {
        return false;
    }
}
extern "C" bool build_cache_save_generation(BuildCache *c, const char *module, int chunks, char *why, size_t cap) {
    try {
        auto data = generation(c, module, chunks);
        write(c->root / (std::string(module) + ".generation"), data);
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
extern "C" int build_cache_object(BuildCache *c, const char *stem, const char *source, char source_hash[65]) {
    try {
        auto h = digest(fs::u8path(source));
        memcpy(source_hash, h.c_str(), 65);
    } catch (...) {
        return -1;
    }
    try {
        need(valid_stem(stem), "Invalid object name.");
        std::string expected =
            "LASOBJ1\n" + std::string(source_hash) + "\n" + digest(c->root / (std::string(stem) + ".o")) + "\n";
        return read(c->root / (std::string(stem) + ".receipt"), 256) == expected ? 1 : 0;
    } catch (...) {
        return 0;
    }
}
extern "C" bool build_cache_save_object(BuildCache *c, const char *stem, const char *source, const char *source_hash,
                                        char *why, size_t cap) {
    try {
        need(valid_stem(stem), "Invalid object name.");
        need(digest(fs::u8path(source)) == source_hash, "Generated source changed while being compiled.");
        auto tmp = c->root / (std::string(stem) + ".o.tmp"), obj = c->root / (std::string(stem) + ".o");
        need(fs::file_size(tmp) > 0, "The compiler did not produce an object.");
        auto hash = digest(tmp);
        replace(tmp, obj);
        write(c->root / (std::string(stem) + ".receipt"), "LASOBJ1\n" + std::string(source_hash) + "\n" + hash + "\n");
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
extern "C" bool build_cache_publish(BuildCache *c, const char *name, const char *runtime_id,
                                    char *why, size_t cap) {
    try {
        need(library_name(name), "Invalid game library filename.");
        auto lib = c->root.parent_path() / name, id = lib;
        id += ".id";
        plain(lib);
        plain(id);
        auto tmp = c->root / (std::string(name) + ".tmp");
        need(fs::file_size(tmp) > 0, "The linker did not produce a game library.");
        plain(tmp);
        write(c->root / "next-id", std::string(runtime_id) + "\n");
        bool had_lib = fs::exists(lib), had_id = fs::exists(id);
        if (had_lib)
            fs::copy_file(lib, c->root / "previous-library", fs::copy_options::overwrite_existing);
        if (had_id)
            fs::copy_file(id, c->root / "previous-id", fs::copy_options::overwrite_existing);
        // Record recovery information before replacing either installed file. Removing it commits the pair.
        write(c->root / "install-journal",
              std::string("LASINSTALL1\n") + name + "\n" + (had_lib ? "1" : "0") + " " + (had_id ? "1" : "0") + "\n");
        replace(tmp, lib);
        replace(c->root / "next-id", id);
        fs::remove(c->root / "install-journal");
        std::error_code ec;
        fs::remove(c->root / "previous-library", ec);
        fs::remove(c->root / "previous-id", ec);
        try { finish(c); }
        catch (const std::exception &e) { // The installed library is already good; cleanup can be retried with Clear.
            fprintf(stderr, "[gamebuild] cache cleanup: %s\n", e.what());
            build_cache_failed(c);
        }
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        try {
            recover(c);
        } catch (const std::exception &rollback) {
            error(why, cap, rollback);
        }
        return false;
    }
}

extern "C" bool build_cache_clear(const char *game_dir, char *why, size_t cap) {
    try {
        auto game = fs::weakly_canonical(fs::absolute(fs::u8path(game_dir)));
        if (!fs::exists(game / ".gamebuild")) {  // (nothing to lock: don't create the cache just to clear it)
            auto old = game / ".zigcache";
            if (fs::exists(old)) { plain_tree(old); fs::remove_all(old); }
            return true;
        }
        auto c = acquire(game_dir);
        // Finish rollback before deleting the only copy of a previously working library.
        recover(c.get());
        clear_old_zig_cache(c.get());
        for (auto &e : fs::directory_iterator(c->root))
            if (e.path().filename() != ".lock")
                fs::remove_all(e.path());
        write(c->root / "owner", "LASBUILD3\n");
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
