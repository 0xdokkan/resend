#include "app/files.hpp"

#include "core/json.hpp"
#include "core/text.hpp"
#include "explain/explain.hpp"
#include "scenario/scenario_json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace resend {
namespace fs = std::filesystem;
using netsim::text::tr;

namespace {
constexpr const char* recovery_format = "resend-autosave";
constexpr const char* recovery_prefix = "autosave-";

long process_id() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

bool running(long id) {
#ifdef _WIN32
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
    if (!process) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return kill(static_cast<pid_t>(id), 0) == 0 || errno == EPERM;
#endif
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool write_text(const fs::path& path, const std::string& text, std::string& error) {
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(process_id());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = tr("cannot write to {0}", {path_text(path.parent_path().empty() ? fs::path(".") : path.parent_path())});
            return false;
        }
        file << text;
        file.flush();
        if (!file) {
            file.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            error = tr("the disk refused the data");
            return false;
        }
    }
    std::error_code failure;
    fs::rename(temporary, path, failure);
    if (failure) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        error = failure.message();
        return false;
    }
    return true;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<fs::path> environment_path(const char* name) {
#ifdef _WIN32
    // Wide variables: user names may be anything.
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    if (const wchar_t* value = _wgetenv(wide.c_str()); value && *value) return fs::path(value);
#else
    if (const char* value = std::getenv(name); value && *value) return fs::path(value);
#endif
    return std::nullopt;
}
} // namespace

std::string path_text(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

fs::path path_from_text(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::optional<netsim::Scenario> read_scenario_file(const fs::path& path, std::string& error) {
    const auto text = read_text(path);
    if (!text) {
        error = tr("cannot open the file");
        return std::nullopt;
    }
    auto scenario = netsim::scenario_from_json(*text, error);
    if (!scenario) return std::nullopt;
    const auto issues = netsim::validate(*scenario);
    if (!issues.empty()) {
        error = issues.front().subject + ": " + netsim::explain::describe(issues.front().problem);
        return std::nullopt;
    }
    return scenario;
}

bool is_lab_file(const fs::path& path) {
    const auto text = read_text(path);
    netsim::json::Error ignored;
    const auto root = text ? netsim::json::parse(*text, ignored) : std::nullopt;
    const auto* format = root ? root->find("format") : nullptr;
    return format && format->string() && *format->string() == "resend-lab";
}

std::optional<netsim::lab::Lab> read_lab_file(const fs::path& path, std::string& error) {
    const auto text = read_text(path);
    if (!text) {
        error = tr("cannot open the file");
        return std::nullopt;
    }
    auto lab = netsim::lab::lab_from_json(*text, error);
    if (!lab) return std::nullopt;
    if (const auto issues = netsim::lab::validate(*lab); !issues.empty()) {
        error = issues.front();
        return std::nullopt;
    }
    lab->id = path.stem().string();
    return lab;
}

bool write_scenario_file(const fs::path& path, const netsim::Scenario& scenario, std::string& error) {
    return write_text(path, netsim::scenario_to_json(scenario), error);
}

fs::path home_directory() {
#ifdef _WIN32
    if (auto home = environment_path("USERPROFILE")) return *home;
#else
    if (auto home = environment_path("HOME")) return *home;
#endif
    std::error_code ignored;
    return fs::current_path(ignored);
}

fs::path state_directory() {
    fs::path directory;
    if (auto chosen = environment_path("RESEND_STATE_DIR")) {
        directory = *chosen;
    } else {
#if defined(_WIN32)
        directory = environment_path("LOCALAPPDATA").value_or(home_directory()) / "resend";
#elif defined(__APPLE__)
        directory = home_directory() / "Library" / "Application Support" / "resend";
#else
        directory = environment_path("XDG_STATE_HOME").value_or(home_directory() / ".local" / "state") / "resend";
#endif
    }
    std::error_code ignored;
    fs::create_directories(directory, ignored);
    return directory;
}

Settings load_settings(const fs::path& directory) {
    Settings settings;
    const auto text = read_text(directory / "settings.json");
    netsim::json::Error ignored;
    const auto root = text ? netsim::json::parse(*text, ignored) : std::nullopt;
    if (!root) return settings;
    // Before effects had three levels, reduced motion was a switch.
    if (const auto* reduced = root->find("reduced-motion"); reduced && reduced->boolean() && *reduced->boolean()) settings.effects = "reduced";
    if (const auto* effects = root->find("effects"); effects && effects->string() &&
        (*effects->string() == "full" || *effects->string() == "reduced" || *effects->string() == "off"))
        settings.effects = *effects->string();
    if (const auto* sound = root->find("sound"); sound && sound->boolean()) settings.sound = *sound->boolean();
    if (const auto* volume = root->find("volume"); volume && volume->string() &&
        (*volume->string() == "quiet" || *volume->string() == "medium" || *volume->string() == "loud"))
        settings.volume = *volume->string();
    if (const auto* theme = root->find("theme"); theme && theme->string()) settings.theme = *theme->string();
    if (const auto* welcomed = root->find("welcomed"); welcomed && welcomed->boolean()) settings.welcomed = *welcomed->boolean();
    if (const auto* solved = root->find("solved-labs"); solved && solved->array_items())
        for (const auto& item : *solved->array_items())
            if (item.string() && std::find(settings.solved_labs.begin(), settings.solved_labs.end(), *item.string()) == settings.solved_labs.end())
                settings.solved_labs.push_back(*item.string());
    return settings;
}

bool save_settings(const fs::path& directory, const Settings& settings) {
    auto root = netsim::json::Value::object();
    root.set("effects", settings.effects);
    root.set("sound", settings.sound);
    root.set("volume", settings.volume);
    root.set("theme", settings.theme);
    root.set("welcomed", settings.welcomed);
    auto solved = netsim::json::Value::array();
    for (const auto& id : settings.solved_labs) solved.push(id);
    root.set("solved-labs", std::move(solved));
    std::error_code ignored;
    fs::create_directories(directory, ignored);
    std::string error;
    return write_text(directory / "settings.json", netsim::json::write(root), error);
}

Recovery::Recovery(fs::path directory)
    : directory_(std::move(directory)), file_(directory_ / (recovery_prefix + std::to_string(process_id()) + ".json")) {}

Recovery::~Recovery() = default;

bool Recovery::save(const netsim::Scenario& scenario, const std::optional<fs::path>& document) {
    netsim::json::Error ignored;
    auto contents = netsim::json::parse(netsim::scenario_to_json(scenario), ignored);
    if (!contents) return false;
    auto root = netsim::json::Value::object();
    root.set("format", recovery_format);
    root.set("version", 1);
    root.set("document", document ? netsim::json::Value(path_text(*document)) : netsim::json::Value());
    root.set("scenario", std::move(*contents));
    std::error_code created;
    fs::create_directories(directory_, created);
    std::string error;
    return write_text(file_, netsim::json::write(root), error);
}

void Recovery::clear() {
    std::error_code ignored;
    fs::remove(file_, ignored);
}

std::optional<Recovery::Found> Recovery::abandoned() const {
    std::error_code failure;
    std::optional<Found> best;
    fs::file_time_type newest{};
    for (const auto& item : fs::directory_iterator(directory_, failure)) {
        const auto name = path_text(item.path().filename());
        if (!name.starts_with(recovery_prefix) || !name.ends_with(".json") || item.path() == file_) continue;
        const auto digits = name.substr(std::char_traits<char>::length(recovery_prefix), name.size() - 5 - std::char_traits<char>::length(recovery_prefix));
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
        if (digits.size() < 12 && running(std::stol(digits))) continue;
        const auto text = read_text(item.path());
        netsim::json::Error ignored;
        const auto root = text ? netsim::json::parse(*text, ignored) : std::nullopt;
        const auto* format = root ? root->find("format") : nullptr;
        const auto* scenario = root ? root->find("scenario") : nullptr;
        if (!format || !format->string() || *format->string() != recovery_format || !scenario) continue;
        std::string error;
        auto network = netsim::scenario_from_json(netsim::json::write(*scenario), error);
        if (!network || !netsim::validate(*network).empty()) continue;
        std::error_code no_time;
        const auto time = fs::last_write_time(item.path(), no_time);
        if (best && time <= newest) continue;
        newest = time;
        Found found{item.path(), std::move(*network), std::nullopt};
        if (const auto* document = root->find("document"); document && document->string()) found.document = path_from_text(*document->string());
        best = std::move(found);
    }
    return best;
}

void Recovery::discard(const Found& found) {
    std::error_code ignored;
    fs::remove(found.file, ignored);
}

FileBrowser::FileBrowser(fs::path folder) {
    if (!load(folder)) load(home_directory());
}

bool FileBrowser::load(const fs::path& folder) {
    std::error_code failure;
    auto absolute = fs::weakly_canonical(fs::absolute(folder, failure), failure);
    if (failure) absolute = folder;
    std::vector<Entry> entries;
    fs::directory_iterator items(absolute, fs::directory_options::skip_permission_denied, failure);
    if (failure) {
        error_ = path_text(absolute) + ": " + failure.message();
        return false;
    }
    for (const auto& item : items) {
        const auto name = path_text(item.path().filename());
        if (name.empty() || name.front() == '.') continue;
        std::error_code ignored;
        if (item.is_directory(ignored)) entries.push_back({name, true});
        else if (lower(path_text(item.path().extension())) == ".json") entries.push_back({name, false});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        const auto x = lower(a.name), y = lower(b.name);
        return x != y ? x < y : a.name < b.name;
    });
    folder_ = absolute;
    entries_ = std::move(entries);
    error_.clear();
    return true;
}

bool FileBrowser::open(const fs::path& folder) { return load(folder); }

bool FileBrowser::enter(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
    return load(folder_ / path_from_text(name));
}

bool FileBrowser::up() {
    if (!folder_.has_parent_path() || folder_.parent_path() == folder_) return false;
    return load(folder_.parent_path());
}

void FileBrowser::refresh() { load(folder_); }

std::optional<fs::path> FileBrowser::target(const std::string& name) const {
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name == "." || name == "..")
        return std::nullopt;
    auto file = path_from_text(name);
    if (!file.has_extension()) file += ".json";
    return folder_ / file;
}

} // namespace resend
