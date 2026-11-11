#pragma once

#include "lab/lab.hpp"
#include "scenario/scenario.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace resend {

// Scenario files on disk. Reading checks the file and the network it
// describes; the first problem is returned as text.
std::optional<netsim::Scenario> read_scenario_file(const std::filesystem::path& path, std::string& error);
// Writes to a temporary file next to `path`, then renames it over `path`,
// so a crash never leaves half a file.
bool write_scenario_file(const std::filesystem::path& path, const netsim::Scenario& scenario, std::string& error);
// Lab files: whether a file is one, by its "format", and reading one, which
// checks the lab as validate() does. Its id is the file's name.
bool is_lab_file(const std::filesystem::path& path);
std::optional<netsim::lab::Lab> read_lab_file(const std::filesystem::path& path, std::string& error);
// A path as UTF-8 text, for the interface.
std::string path_text(const std::filesystem::path& path);
std::filesystem::path path_from_text(const std::string& text);

// Where resend keeps what it needs between sessions: RESEND_STATE_DIR if
// set; otherwise $XDG_STATE_HOME/resend or ~/.local/state/resend on Linux,
// ~/Library/Application Support/resend on macOS, %LOCALAPPDATA%\resend on
// Windows. Created on first use.
std::filesystem::path state_directory();
// The user's home folder, or the current one.
std::filesystem::path home_directory();

// What the user chose once and keeps: how much the canvas moves, whether
// it plays sounds and how loud, a theme ("" is the default one), whether
// the first-run choice was made and the labs solved. Kept in settings.json
// in the state directory.
struct Settings {
    // "full", "reduced" or "off" (resend::Intensity).
    std::string effects = "full";
    bool sound = true;
    // "quiet", "medium" or "loud".
    std::string volume = "medium";
    std::string theme;
    bool welcomed = false;
    // The ids of the labs solved, in the order they were.
    std::vector<std::string> solved_labs;
    bool operator==(const Settings&) const = default;
};
// The defaults if the file is missing or unreadable.
Settings load_settings(const std::filesystem::path& directory);
bool save_settings(const std::filesystem::path& directory, const Settings& settings);

// Autosave and recovery after a crash. While the network has unsaved
// changes, the app writes it every few seconds to a file of its own in the
// state directory, and removes the file when it closes normally. A file
// left by an app that is no longer running is offered back at startup.
class Recovery {
public:
    explicit Recovery(std::filesystem::path directory);
    ~Recovery();
    Recovery(const Recovery&) = delete;
    Recovery& operator=(const Recovery&) = delete;

    // `document` is the file the network came from, if any.
    bool save(const netsim::Scenario& scenario, const std::optional<std::filesystem::path>& document);
    // Removes this app's file: the network is saved, or was discarded.
    void clear();

    struct Found {
        std::filesystem::path file;
        netsim::Scenario scenario;
        std::optional<std::filesystem::path> document;
    };
    // The most recent file left by an app that is no longer running.
    std::optional<Found> abandoned() const;
    // Removes a file found by abandoned(), recovered or not.
    static void discard(const Found& found);
    const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path directory_, file_;
};

// The contents of a folder for the app's own file dialog: folders first,
// then scenario files (.json), each sorted by name without regard to case.
// Hidden entries are left out.
class FileBrowser {
public:
    struct Entry {
        std::string name;
        bool folder = false;
        bool operator==(const Entry&) const = default;
    };
    explicit FileBrowser(std::filesystem::path folder);
    const std::filesystem::path& folder() const { return folder_; }
    const std::vector<Entry>& entries() const { return entries_; }
    // Why the folder could not be read, if it could not.
    const std::string& error() const { return error_; }
    // Shows another folder; false, and nothing changes, if it cannot be read.
    bool open(const std::filesystem::path& folder);
    bool enter(const std::string& name);
    bool up();
    void refresh();
    // The file a name typed for saving stands for: in this folder, with
    // .json added if it has no extension. Nothing for an empty name, or one
    // with a folder separator.
    std::optional<std::filesystem::path> target(const std::string& name) const;

private:
    bool load(const std::filesystem::path& folder);
    std::filesystem::path folder_;
    std::vector<Entry> entries_;
    std::string error_;
};

} // namespace resend
