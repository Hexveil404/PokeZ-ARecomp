// Included by launcher.mm so headless setup uses the same importer, version checks and compiler as the UI.
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <sstream>
namespace prepfs = std::filesystem;

static std::string prepare_path(const char *value) {
    if (!*value) throw std::runtime_error("An input path is empty.");
    return prepfs::canonical(prepfs::u8path(value)).u8string();
}

static void prepare_real_directory(const prepfs::path &path) {
    auto status = prepfs::symlink_status(path);
    if (prepfs::is_symlink(status) || (prepfs::exists(status) && !prepfs::is_directory(status)))
        throw std::runtime_error("A setup output path is not a regular directory.");
}

// A cache key, not dump verification. A changed path, size or timestamp causes a fresh import.
static std::string prepare_source_stamp(const std::vector<std::string> &inputs) {
    std::vector<std::string> entries;
    size_t slot = 0;
    for (const auto &input : inputs) {
        entries.push_back(std::to_string(slot++) + ":" + input);
        if (input.empty()) continue;
        auto path = prepfs::u8path(input);
        std::vector<prepfs::path> files = {path};
        if (prepfs::is_directory(path))
            for (const auto &entry : prepfs::recursive_directory_iterator(path)) files.push_back(entry.path());
        for (const auto &file : files) {
            if (prepfs::is_symlink(prepfs::symlink_status(file)))
                throw std::runtime_error("Extracted game folders must not contain symbolic links.");
            auto size = prepfs::is_regular_file(file) ? prepfs::file_size(file) : 0;
            entries.push_back(file.u8string() + ":" + std::to_string(size) + ":" +
                              std::to_string(prepfs::last_write_time(file).time_since_epoch().count()));
        }
    }
    std::sort(entries.begin(), entries.end());
    uint64_t hash = 14695981039346656037ull;
    for (const auto &entry : entries) {
        for (unsigned char c : entry) hash = (hash ^ c) * 1099511628211ull;
        hash = (hash ^ '\n') * 1099511628211ull;
    }
    return std::to_string(hash);
}

extern "C" int launcher_prepare(int argc, char **argv) {
    g_headless = true;
    setenv("LARECOMP_HEADLESS", "1", 1);
    std::string game, update, dlc, keys, title_keys;
    bool staged = false;
    try {
        // Resolve before rt_paths_init changes the working directory. Never scan a package's neighbours.
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--prepare") continue;
            size_t equal = arg.find('=');
            if (equal == std::string::npos) throw std::runtime_error("Use --game=PATH, --update=PATH, --dlc=PATH, --keys=PATH or --title-keys=PATH.");
            std::string flag = arg.substr(0, equal), *slot = nullptr;
            if (flag == "--game") slot = &game;
            else if (flag == "--update") slot = &update;
            else if (flag == "--dlc") slot = &dlc;
            else if (flag == "--keys") slot = &keys;
            else if (flag == "--title-keys") slot = &title_keys;
            else throw std::runtime_error("Unknown setup argument.");
            if (!slot->empty()) throw std::runtime_error("A setup input was supplied twice.");
            *slot = prepare_path(arg.c_str() + equal + 1);
        }
        if (game.empty() && (!update.empty() || !dlc.empty()))
            throw std::runtime_error("Select the base game when changing an update or DLC.");
        if (!game.empty() && !prepfs::is_directory(prepfs::u8path(game)) && !prepfs::is_regular_file(prepfs::u8path(game)))
            throw std::runtime_error("The base game must be a regular package file or an extracted folder.");
        if (!game.empty() && prepfs::is_directory(prepfs::u8path(game)) && (!update.empty() || !dlc.empty()))
            throw std::runtime_error("Updates and DLC require a packaged base game (.nsp, .xci or .nup).");
        for (const auto *file : {&update, &dlc, &keys, &title_keys})
            if (!file->empty() && !prepfs::is_regular_file(prepfs::u8path(*file)))
                throw std::runtime_error("An update, DLC or key input is not a regular file.");
        rt_paths_init();
        launcher_log_open();
        settings_load();
        const prepfs::path target = "extracted", stage = ".recomplauncher-stage", previous = ".recomplauncher-previous";
        for (const auto &path : {target, stage, previous}) prepare_real_directory(path);
        for (const auto &input : {game, update, dlc, keys, title_keys}) {
            if (input.empty()) continue;
            for (const auto &path : {target, stage, previous}) {
                auto relative = prepfs::u8path(input).lexically_relative(prepfs::absolute(path));
                if (!relative.empty() && *relative.begin() != "..")
                    throw std::runtime_error("Setup inputs must be outside the managed game folders. Omit --game to rebuild retained files.");
            }
        }
        // Recover an interrupted swap before doing any new work. The previous game survives failed imports/builds.
        if (!prepfs::exists(target) && prepfs::exists(previous)) prepfs::rename(previous, target);
        if (prepfs::exists(target) && prepfs::exists(previous)) prepfs::remove_all(previous);
        g_root = "extracted";
        std::string stamp, saved_stamp;
        if (!game.empty()) stamp = prepare_source_stamp({game, update, dlc, keys, title_keys});
        std::ifstream("extracted/.recomplauncher-source") >> saved_stamp;
        if (!game.empty() && (stamp != saved_stamp || !check_copied_files(g_root).empty())) {
            prepfs::remove_all(stage);
            prepfs::create_directory(stage);
            staged = true;
            g_root = stage.u8string();
            set_job("Reading your game files...");
            if (prepfs::is_directory(prepfs::u8path(game))) {
                uint8_t id[8];
                if (!read_build_id(game + "/exefs/main", id) || !build_is_za(id))
                    throw std::runtime_error("Select an extracted Pokemon Legends: Z-A 1.0.0, 2.0.0 or 2.0.2 game.");
                if (!romfs_copy_tree((game + "/exefs").c_str(), (g_root + "/exefs").c_str(), NULL) ||
                    !romfs_copy_tree((game + "/romfs").c_str(), (g_root + "/romfs").c_str(), setup_progress))
                    throw std::runtime_error("Could not copy the extracted game files.");
            } else {
                std::vector<std::string> packages = {game};
                if (!update.empty()) packages.push_back(update);
                if (!dlc.empty()) packages.push_back(dlc);
                std::string why;
                if (!import_packages(packages, keys, title_keys, why)) throw std::runtime_error(why);
            }
        }
        uint8_t id[8];
        if (!read_build_id(g_root + "/exefs/main", id) || !build_is_za(id))
            throw std::runtime_error("No supported Pokemon Legends: Z-A game found. Select your copy again.");
        std::string why = check_copied_files(g_root);
        if (!why.empty()) throw std::runtime_error(why);
        prepfs::remove(prepfs::u8path(g_root + "/.recomplauncher-ready"));
        if (!build_game_code()) throw std::runtime_error(g_job + ". " + g_job_hint);
        why = check_game_files(g_root);
        if (!why.empty()) throw std::runtime_error(why);
        if (prompts_wanted()) make_prompts(true);
        if (!stamp.empty()) {
            std::ofstream source(prepfs::u8path(g_root + "/.recomplauncher-source"));
            source << stamp << '\n';
            source.close();
            if (!source) throw std::runtime_error("Could not record the source of this build.");
        }
        FILE *ready = fopen((g_root + "/.recomplauncher-ready").c_str(), "wb");
        if (!ready) throw std::runtime_error("Could not record completed setup.");
        if (fclose(ready)) throw std::runtime_error("Could not finish the setup marker.");
        if (staged) {
            if (prepfs::exists(target)) prepfs::rename(target, previous);
            try { prepfs::rename(stage, target); }
            catch (...) {
                if (prepfs::exists(previous)) prepfs::rename(previous, target);
                throw;
            }
            prepfs::remove_all(previous);
        }
        fprintf(stdout, "progress 1 Game ready\n");
        fflush(stdout);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "Setup failed: %s\n", error.what());
        return 1;
    }
}
