// Files: scenario files, autosave and recovery, the app's file dialog.
#include "app/examples.hpp"
#include "app/files.hpp"
#include "check.hpp"
#include "scenario/scenario_json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using resend::FileBrowser;
using resend::Recovery;

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

// A fresh folder for one test, removed at the end.
struct Scratch {
    fs::path folder;
    explicit Scratch(const std::string& name) {
#ifdef _WIN32
        const auto id = _getpid();
#else
        const auto id = getpid();
#endif
        folder = fs::temp_directory_path() / ("resend-" + name + "-" + std::to_string(id));
        fs::remove_all(folder);
        fs::create_directories(folder);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(folder, ignored);
    }
};

void saving_and_opening() {
    // The exit check of milestone M4: a network saved and opened again
    // gives the same file, for every example and for an edited network.
    Scratch scratch("files");
    int examples = 0;
    for (const auto& item : fs::directory_iterator(RESEND_EXAMPLES_DIR)) {
        if (item.path().extension() != ".json") continue;
        ++examples;
        std::string error;
        const auto scenario = resend::read_scenario_file(item.path(), error);
        CHECK(scenario.has_value() && error.empty());
        if (!scenario) continue;
        const auto copy = scratch.folder / item.path().filename();
        CHECK(resend::write_scenario_file(copy, *scenario, error));
        CHECK(read_file(copy) == read_file(item.path()));
        const auto again = resend::read_scenario_file(copy, error);
        CHECK(again && *again == *scenario);
    }
    CHECK(examples >= 6);

    // Overwriting leaves no temporary file behind.
    auto scenario = netsim::sample_lan();
    const auto file = scratch.folder / "lan.json";
    std::string error;
    CHECK(resend::write_scenario_file(file, scenario, error));
    scenario.devices[0].name = "Desk";
    CHECK(resend::write_scenario_file(file, scenario, error));
    CHECK(resend::read_scenario_file(file, error)->devices[0].name == "Desk");
    int files = 0;
    for ([[maybe_unused]] const auto& item : fs::directory_iterator(scratch.folder)) ++files;
    CHECK(files == examples + 1);

    // What can go wrong, said with the place it went wrong.
    CHECK(!resend::read_scenario_file(scratch.folder / "missing.json", error) && error == "cannot open the file");
    write_file(scratch.folder / "broken.json", "{\n  \"format\": \"resend-scenario\",\n  \"version\": 1,\n  oops\n}\n");
    CHECK(!resend::read_scenario_file(scratch.folder / "broken.json", error) && error.starts_with("line 4, column 3"));
    auto twins = netsim::sample_lan();
    twins.devices[1].id = twins.devices[0].id;
    write_file(scratch.folder / "twins.json", netsim::scenario_to_json(twins));
    CHECK(!resend::read_scenario_file(scratch.folder / "twins.json", error) && error.starts_with("PC-B: "));
    CHECK(!resend::write_scenario_file(scratch.folder / "no-such-folder" / "x.json", scenario, error) && !error.empty());

    // Paths are UTF-8 text in the interface.
    const auto accented = scratch.folder / resend::path_from_text("città.json");
    CHECK(resend::write_scenario_file(accented, scenario, error) && fs::exists(accented));
    CHECK(resend::path_text(accented.filename()) == "città.json");
}

void built_in_examples() {
    // The app's examples are the files of examples/, each a valid network.
    const auto list = resend::examples();
    CHECK(list.size() == 11 && list.front().file == "sample-lan.json" && list.back().file == "longest-prefix.json");
    int files = 0;
    for (const auto& item : fs::directory_iterator(RESEND_EXAMPLES_DIR)) files += item.path().extension() == ".json";
    CHECK(static_cast<int>(list.size()) == files);
    for (const auto& example : list) {
        CHECK(read_file(fs::path(RESEND_EXAMPLES_DIR) / std::string(example.file)) == example.text);
        std::string error;
        const auto scenario = netsim::scenario_from_json(example.text, error);
        CHECK(scenario && netsim::validate(*scenario).empty() && !scenario->title.empty() && !scenario->notes.empty());
    }
}

void autosave_and_recovery() {
    Scratch scratch("recovery");
    Recovery recovery(scratch.folder);
    CHECK(!recovery.abandoned());
    auto scenario = netsim::sample_lan();
    scenario.devices[0].name = "Unsaved";
    const auto document = scratch.folder / resend::path_from_text("réseau.json");
    CHECK(recovery.save(scenario, document) && fs::exists(recovery.file()));
    // Its own file is not abandoned: the app is running.
    CHECK(!recovery.abandoned());
    Recovery other(scratch.folder);
    CHECK(!other.abandoned());

    // A file left by an app that is gone is found, with its network and
    // the file that network came from.
    fs::copy_file(recovery.file(), scratch.folder / "autosave-999999990.json");
    recovery.clear();
    CHECK(!fs::exists(recovery.file()));
    const auto found = other.abandoned();
    CHECK(found.has_value());
    if (found) {
        CHECK(found->scenario == scenario && found->document == document);
        Recovery::discard(*found);
        CHECK(!fs::exists(found->file) && !other.abandoned());
    }
    // An untitled network has no document; broken or foreign files are ignored.
    CHECK(recovery.save(netsim::sample_lan(), std::nullopt));
    fs::rename(recovery.file(), scratch.folder / "autosave-999999991.json");
    write_file(scratch.folder / "autosave-999999992.json", "{\"format\": \"something-else\", \"scenario\": {}}\n");
    write_file(scratch.folder / "autosave-999999993.json", "not json");
    write_file(scratch.folder / "autosave-x.json", "{}");
    const auto untitled = other.abandoned();
    CHECK(untitled && !untitled->document && untitled->scenario == netsim::sample_lan());
}

void state_folder() {
    Scratch scratch("state");
    const auto chosen = scratch.folder / "deeper" / "state";
#ifdef _WIN32
    _putenv_s("RESEND_STATE_DIR", resend::path_text(chosen).c_str());
#else
    setenv("RESEND_STATE_DIR", resend::path_text(chosen).c_str(), 1);
#endif
    CHECK(resend::state_directory() == chosen && fs::is_directory(chosen));
    CHECK(!resend::home_directory().empty());
}

void file_dialog() {
    Scratch scratch("browser");
    fs::create_directories(scratch.folder / "labs" / "week 1");
    fs::create_directories(scratch.folder / "Archive");
    fs::create_directories(scratch.folder / ".hidden");
    write_file(scratch.folder / "b-network.json", "{}");
    write_file(scratch.folder / "A-network.JSON", "{}");
    write_file(scratch.folder / "notes.txt", "");
    write_file(scratch.folder / ".secret.json", "{}");
    FileBrowser browser(scratch.folder);
    CHECK(browser.error().empty() && browser.folder() == fs::weakly_canonical(scratch.folder));
    const std::vector<FileBrowser::Entry> expected{{"Archive", true}, {"labs", true}, {"A-network.JSON", false}, {"b-network.json", false}};
    CHECK(browser.entries() == expected);
    CHECK(browser.enter("labs") && browser.entries().size() == 1 && browser.entries()[0].name == "week 1");
    CHECK(!browser.enter("nothing-here") && !browser.error().empty() && browser.entries().size() == 1);
    CHECK(!browser.enter("../labs"));
    CHECK(browser.up() && browser.entries() == expected);
    write_file(scratch.folder / "c.json", "{}");
    browser.refresh();
    CHECK(browser.entries().size() == 5);

    // Names typed to save.
    CHECK(browser.target("lab 2") == browser.folder() / "lab 2.json");
    CHECK(browser.target("lab.json") == browser.folder() / "lab.json");
    CHECK(!browser.target("") && !browser.target("a/b") && !browser.target("..") && !browser.target("a\\b"));

    // A folder that cannot be read leaves the browser where it was; a
    // missing start folder falls back to the home folder.
    CHECK(!browser.open(scratch.folder / "missing") && browser.folder() == fs::weakly_canonical(scratch.folder));
    FileBrowser elsewhere(scratch.folder / "missing");
    CHECK(elsewhere.folder() == fs::weakly_canonical(resend::home_directory()));
}

} // namespace

int main() {
    saving_and_opening();
    built_in_examples();
    autosave_and_recovery();
    state_folder();
    file_dialog();
    std::cout << "File tests passed: " << test::checks << " checks.\n";
}
