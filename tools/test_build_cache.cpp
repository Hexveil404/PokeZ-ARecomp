#include "build_cache.h"
#include "cache_sha256.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs = std::filesystem;
static void write(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char **argv) {
    assert(argc == 2);
    CacheSha256 empty;
    assert(empty.finish() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CacheSha256 abc;
    abc.add("a", 1);
    abc.add("bc", 2);
    assert(abc.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CacheSha256 million;
    std::string thousand(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        million.add(thousand.data(), thousand.size());
    assert(million.finish() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    fs::path root = fs::u8path(argv[1]);
    fs::create_directories(root);
    std::string data = (root / "input").u8string(), game = (root / "extracted").u8string();
    write(fs::u8path(data), "binary input");
    const char *inputs[] = {data.c_str()};
    char why[1024] = "", hash[65];
    auto open = [&](const char *spec = "compiler and flags") {
        return build_cache_open(game.c_str(), spec, inputs, 1, why, sizeof why);
    };
    fs::path work = fs::u8path(game) / ".gamebuild";
    write(work / "legacy.o", "old beta_5 object without a receipt");
    write(fs::u8path(game) / "lasgame.dll", "installed library");
    assert(!build_cache_resumable(game.c_str()));
    BuildCache *cache = open();
    assert(cache);
    assert(!fs::exists(work / "legacy.o") && read(fs::u8path(game) / "lasgame.dll") == "installed library");
    assert(build_cache_resumable(game.c_str()));
    assert(!open());
    assert(!build_cache_clear(game.c_str(), why, sizeof why)); // another process/window cannot erase an active build
    assert(build_cache_prepare_generation(cache, "generator 1", inputs, 1, why, sizeof why));
    write(work / "main/main_000.c", "int example;");
    write(work / "main/main_table.c", "int table;");
    auto source = (work / "main/main_000.c").u8string();
    int chunks = 0;
    assert(!build_cache_generation(cache, "main", &chunks));
    assert(build_cache_save_generation(cache, "main", 1, why, sizeof why));
    assert(build_cache_generation(cache, "main", &chunks) && chunks == 1);
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 0);
    write(work / "main_000.o.tmp", "partial or completed object");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 0); // no success receipt yet
    assert(build_cache_save_object(cache, "main_000", source.c_str(), hash, why, sizeof why));
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    build_cache_close(cache);
    assert(build_cache_resumable(game.c_str())); // closing an unfinished build leaves Resume available
    cache = open();
    assert(cache);
    assert(build_cache_generation(cache, "main", &chunks));
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    assert(build_cache_prepare_generation(cache, "generator 2", inputs, 1, why, sizeof why));
    assert(!build_cache_generation(cache, "main", &chunks));
    assert(fs::exists(work / "main_000.receipt"));
    write(work / "main/main_000.c", "int example;");
    write(work / "main/main_table.c", "int table;");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    assert(build_cache_save_generation(cache, "main", 1, why, sizeof why));
    write(work / "main_000.o", "damaged object");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 0);
    write(work / "main_000.o.tmp", "repaired object");
    assert(build_cache_save_object(cache, "main_000", source.c_str(), hash, why, sizeof why));
    write(fs::u8path(source), "changed source");
    assert(!build_cache_generation(cache, "main", &chunks));
    assert(!build_cache_save_object(cache, "main_000", source.c_str(), hash, why, sizeof why));
    write(fs::u8path(source), "int example;");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    write(fs::u8path(game) / "lasgame.dll", "old library");
    write(fs::u8path(game) / "lasgame.dll.id", "old id");
    assert(!build_cache_publish(cache, "lasgame.dll", "new-runtime", why, sizeof why));
    assert(read(fs::u8path(game) / "lasgame.dll") == "old library");
    write(work / "lasgame.dll.tmp", "new library");
    write(work / "gen_main.log", "generator log");
    write(work / "main_000.log", "compiler log");
    write(work / "link.rsp", "object list");
    write(work / "zig-cache" / "temporary", "compiler cache");
    write(fs::u8path(game) / ".zigcache" / "temporary", "legacy compiler cache");
    assert(build_cache_publish(cache, "lasgame.dll", "new-runtime", why, sizeof why));
    assert(read(fs::u8path(game) / "lasgame.dll.id") == "new-runtime\n");
    assert(fs::exists(work / "main_000.o") && fs::exists(work / "main_000.receipt"));
    for (const char *name : {"main", "gen_main.log", "main_000.log", "link.rsp", "zig-cache", "main.generation"})
        assert(!fs::exists(work / name));
    assert(!fs::exists(fs::u8path(game) / ".zigcache") && !build_cache_resumable(game.c_str()));
    // Simulate a crash between installing the library and its metadata, then restart.
    write(work / "previous-library", "old library");
    write(work / "previous-id", "old id");
    write(work / "install-journal", "LASINSTALL1\nlasgame.dll\n1 1\n");
    build_cache_close(cache);
    cache = open();
    assert(cache);
    assert(read(fs::u8path(game) / "lasgame.dll") == "old library");
    assert(read(fs::u8path(game) / "lasgame.dll.id") == "old id");
    build_cache_close(cache);
    write(fs::u8path(data), "different binary input");
    cache = open();
    assert(cache);
    assert(!fs::exists(work / "main_000.receipt"));
    assert(!build_cache_generation(cache, "main", &chunks));
    write(work / "marker", "stale object");
    build_cache_close(cache);
    cache = open("different compiler or flags");
    assert(cache && !fs::exists(work / "marker"));
    build_cache_failed(cache);
    assert(!build_cache_resumable(game.c_str()));
    build_cache_close(cache);
    // Clearing after interrupted publication must recover before discarding rollback files.
    write(work / "previous-library", "old library");
    write(work / "previous-id", "old id");
    write(work / "install-journal", "LASINSTALL1\nlasgame.dll\n1 1\n");
    write(fs::u8path(game) / "lasgame.dll", "incompletely published library");
    write(fs::u8path(game) / ".zigcache" / "temporary", "legacy compiler cache");
    assert(build_cache_clear(game.c_str(), why, sizeof why));
    assert(!fs::exists(fs::u8path(game) / ".zigcache"));
    assert(read(fs::u8path(game) / "lasgame.dll") == "old library");
    assert(read(fs::u8path(game) / "lasgame.dll.id") == "old id");
    assert(!build_cache_present(game.c_str())); // only the lock and owner remain: nothing left to clear
    std::string fresh = (root / "fresh").u8string();
    fs::create_directories(fs::u8path(fresh));
    assert(!build_cache_present(fresh.c_str()) && build_cache_clear(fresh.c_str(), why, sizeof why));
    assert(!fs::exists(fs::u8path(fresh) / ".gamebuild")); // clearing never creates the cache folder
    write(fs::u8path(fresh) / ".zigcache" / "temporary", "legacy compiler cache");
    assert(build_cache_present(fresh.c_str()) && build_cache_clear(fresh.c_str(), why, sizeof why));
    assert(!fs::exists(fs::u8path(fresh) / ".zigcache") && !fs::exists(fs::u8path(fresh) / ".gamebuild"));
#ifdef _WIN32
    // #141: read-only entries (OneDrive / copied folders) made every rebuild fail with "cannot remove: access denied".
    write(work / "sdk" / "sdk_000.c", "generated");
    SetFileAttributesW((work / "sdk" / "sdk_000.c").c_str(), FILE_ATTRIBUTE_READONLY);
    SetFileAttributesW((work / "sdk").c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY);
    assert(build_cache_clear(game.c_str(), why, sizeof why));
    assert(!fs::exists(work / "sdk"));
#endif
    std::error_code ec;
    fs::create_directory_symlink(root / "extracted", root / "linked", ec);
    if (!ec) {
        cache = build_cache_open((root / "linked").u8string().c_str(), "flags", inputs, 1, why, sizeof why);
        assert(cache && !open());
        build_cache_close(cache);
        fs::create_directory(root / "outside");
        fs::create_directory_symlink(root / "outside", work / "linked-inside");
        assert(!open());
        fs::remove(work / "linked-inside");
    }
    std::cout << "SHA-256, legacy upgrade, resume markers, success cleanup, locks, corruption, recovery and link boundaries "
                 "passed\n";
}
