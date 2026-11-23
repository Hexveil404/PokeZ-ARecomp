"""Run with python3 tools/test_recomplauncher.py (g++ required).

Compile the real headless setup entry point against synthetic importer/compiler boundaries.
No Nintendo data, GUI, network calls or installed game folders are used.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace fs = std::filesystem;
static bool g_headless;
static std::string g_root, g_job, g_job_hint;
static void rt_paths_init() { fs::current_path(std::getenv("RECOMPLAUNCHER_DATA")); }
static void launcher_log_open() {}
static void settings_load() {}
static void set_job(const std::string &s) { g_job = s; }
static void setup_progress(uint64_t, uint64_t) {}
static bool read_build_id(const std::string &path, uint8_t *id) {
    std::ifstream f(path); char c = 0; f.get(c); *id = c; return bool(f);
}
static bool build_is_za(const uint8_t *id) { return *id == 'Z'; }
static std::string check_copied_files(const std::string &path) {
    return fs::exists(fs::path(path) / "romfs") ? "" : "Missing game data";
}
static bool romfs_copy_tree(const char *from, const char *to, void (*)(uint64_t,uint64_t)) {
    if (!fs::exists(from)) return false;
    fs::copy(from, to, fs::copy_options::recursive); return true;
}
static bool import_packages(const std::vector<std::string> &packages, const std::string &keys,
                            const std::string &titles, std::string &why) {
    if (std::getenv("FAIL_IMPORT")) { why = "Synthetic import failure"; return false; }
    fs::create_directories(fs::path(g_root) / "exefs");
    fs::create_directory(fs::path(g_root) / "romfs");
    std::ofstream(fs::path(g_root) / "exefs/main") << "Z";
    std::ofstream record(fs::path(g_root) / "imported");
    for (const auto &p : packages) record << p << '\n';
    record << keys << '\n' << titles << '\n';
    return true;
}
static bool build_game_code() {
    if (std::getenv("FAIL_BUILD")) { g_job = "Synthetic build failure"; return false; }
    std::ofstream(fs::path(g_root) / "game-library") << "built"; return true;
}
static std::string check_game_files(const std::string &path) { return check_copied_files(path); }
static bool prompts_wanted() { return false; }
static bool make_prompts(bool) { return true; }
#include "runtime/ui/launcher_prepare.h"
int main(int argc, char **argv) { return launcher_prepare(argc, argv); }
'''


class SetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="za-setup-build-")
        source = Path(cls.build.name) / "harness.cpp"
        source.write_text(HARNESS)
        cls.exe = source.with_suffix("")
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra",
                        "-Werror", "-I", str(ROOT), str(source), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="za setup spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "managed data"
        self.data.mkdir()
        self.game = self.root / "game.nup"
        self.game.write_text("synthetic package")

    def run_setup(self, *args, **env):
        return subprocess.run([str(self.exe), "--prepare", *args], cwd=self.root,
                              env={**os.environ, "RECOMPLAUNCHER_DATA": str(self.data), **env},
                              text=True, capture_output=True)

    def ready(self):
        return (self.data / "extracted/.recomplauncher-ready").exists()

    def test_package_and_paths_with_spaces(self):
        update, dlc, keys, titles = [self.root / x for x in ("update.nsp", "DLC.nsp", "prod.keys", "title.keys")]
        for path in (update, dlc, keys, titles):
            path.write_text("synthetic")
        result = self.run_setup(f"--game={self.game}", f"--update={update}", f"--dlc={dlc}",
                                f"--keys={keys}", f"--title-keys={titles}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.ready())
        self.assertEqual((self.data / "extracted/imported").read_text().splitlines(),
                         [str(p) for p in (self.game, update, dlc, keys, titles)])
        self.assertIn("progress 1 Game ready", result.stdout)

    def test_failure_keeps_previous_game_and_saves(self):
        self.assertEqual(self.run_setup(f"--game={self.game}").returncode, 0)
        save = self.data / "save/main"
        save.parent.mkdir()
        save.write_bytes(b"save data")
        other = self.root / "another.nup"
        other.write_text("replacement")
        for failure in ("FAIL_IMPORT", "FAIL_BUILD"):
            with self.subTest(failure=failure):
                result = self.run_setup(f"--game={other}", **{failure: "1"})
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(self.ready())
                self.assertIn(str(self.game), (self.data / "extracted/imported").read_text())
                self.assertEqual(save.read_bytes(), b"save data")

    def test_unchanged_copy_reuses_import(self):
        self.assertEqual(self.run_setup(f"--game={self.game}").returncode, 0)
        result = self.run_setup(f"--game={self.game}", FAIL_IMPORT="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.game.write_text("changed package")
        self.assertNotEqual(self.run_setup(f"--game={self.game}", FAIL_IMPORT="1").returncode, 0)

    def test_rebuild_without_original_copy(self):
        self.assertEqual(self.run_setup(f"--game={self.game}").returncode, 0)
        self.game.unlink()
        (self.data / "extracted/game-library").unlink()
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.ready())

    def test_failed_rebuild_removes_ready_marker(self):
        self.assertEqual(self.run_setup(f"--game={self.game}").returncode, 0)
        self.assertNotEqual(self.run_setup(FAIL_BUILD="1").returncode, 0)
        self.assertFalse(self.ready())
        self.assertEqual(self.run_setup().returncode, 0)

    def test_interrupted_swap_recovers_previous_game(self):
        self.assertEqual(self.run_setup(f"--game={self.game}").returncode, 0)
        (self.data / "extracted").rename(self.data / ".recomplauncher-previous")
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.ready())

    def test_invalid_arguments_do_not_create_output(self):
        for args in [("--game=",), ("--unknown=x",), ("--game", str(self.game)),
                     (f"--game={self.game}", f"--game={self.game}"), (f"--dlc={self.game}",)]:
            with self.subTest(args=args):
                self.assertNotEqual(self.run_setup(*args).returncode, 0)
                self.assertFalse((self.data / "extracted").exists())

    def test_extracted_folder_and_wrong_game(self):
        folder = self.root / "extracted copy"
        (folder / "exefs").mkdir(parents=True)
        (folder / "romfs").mkdir()
        (folder / "exefs/main").write_text("Z")
        self.assertEqual(self.run_setup(f"--game={folder}").returncode, 0)
        (folder / "exefs/main").write_text("wrong game")
        self.assertNotEqual(self.run_setup(f"--game={folder}").returncode, 0)
        self.assertTrue(self.ready())
        self.assertNotEqual(self.run_setup(f"--game={folder}", f"--dlc={self.game}").returncode, 0)

    def test_source_inside_managed_output_is_refused(self):
        stage = self.data / ".recomplauncher-stage"
        stage.mkdir()
        source = stage / "game.nup"
        source.write_text("keep me")
        self.assertNotEqual(self.run_setup(f"--game={source}").returncode, 0)
        self.assertEqual(source.read_text(), "keep me")

    def test_symlink_output_is_refused(self):
        victim = self.root / "unrelated"
        victim.mkdir()
        sentinel = victim / "keep"
        sentinel.write_text("untouched")
        (self.data / ".recomplauncher-stage").symlink_to(victim, target_is_directory=True)
        self.assertNotEqual(self.run_setup(f"--game={self.game}").returncode, 0)
        self.assertEqual(sentinel.read_text(), "untouched")


if __name__ == "__main__":
    unittest.main()
