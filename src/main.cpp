// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// ReSkateMusicMaker: a window over the music packer library (MusicPacker/packer.h). Songs in a
// list (dropped or added), their artist and title editable, then built into the game's Mods
// folder. A mod built here can be opened again from its project file and rebuilt.
// Drawn with Dear ImGui through the launcher's Direct3D 12 renderer (Launcher/gui_renderer.cpp).
#include "packer.h"
#include "ffmpeg_fetch.h"
#include "Engine/Core/Json/json.h"
#include "gui_renderer.h"
#include "miniz.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
namespace fs = std::filesystem;
using dingosdk::Json;
using dingosdk::launcher_gui::detail::Renderer;
using Microsoft::WRL::ComPtr;

constexpr int window_width = 1100, window_height = 700;
constexpr const char* bitrates[]{"128", "160", "192", "256", "320"};

float g_scale = 1.0f;
inline float S(float value) { return value * g_scale; }

std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) out += static_cast<char>(std::tolower(c));
    return out;
}

struct ExternalSong {
    std::string source; // e.g. "mod 'KevinMacLeod'" or "the game soundtrack"
    fs::path folder;    // folder of the mod if from a mod, empty if from game
};

std::string narrow(const std::wstring& text) {
    std::string utf8(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    return utf8;
}
std::wstring widen(const std::string& text) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), static_cast<int>(wide.size()));
    return wide;
}
template<std::size_t N> void copy_text(std::array<char, N>& to, const std::string& from) {
    const auto n = std::min(from.size(), N - 1);
    std::copy_n(from.data(), n, to.data());
    to[n] = '\0';
}

// ---- Settings: the game folder and where ffmpeg is, in %APPDATA%\ReSkateMusicMaker -----------------
struct Settings {
    fs::path game, ffmpeg;
};
fs::path settings_file() {
    PWSTR roaming{};
    fs::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) folder = fs::path(roaming) / L"ReSkateMusicPacker";
    CoTaskMemFree(roaming);
    return folder / L"settings.json";
}
Settings load_settings() {
    Settings settings;
    try {
        std::ifstream in(settings_file(), std::ios::binary);
        if (!in) return settings;
        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
        settings.game = widen(root.value("game", ""));
        settings.ffmpeg = widen(root.value("ffmpeg", ""));
    } catch (...) {}
    return settings;
}
void save_settings(const Settings& settings) {
    auto root = Json::object();
    root["game"] = narrow(settings.game.wstring());
    root["ffmpeg"] = narrow(settings.ffmpeg.wstring());
    std::error_code ignored;
    fs::create_directories(settings_file().parent_path(), ignored);
    std::ofstream(settings_file(), std::ios::binary) << root.dump(2) << "\n";
}

// ---- Windows bits --------------------------------------------------------------------------------
bool game_folder(const fs::path& folder) { return !folder.empty() && fs::exists(folder / L"Skate.exe"); }

// ffmpeg and ffprobe on PATH, or in the folder the user pointed at (added to this process's PATH).
bool find_ffmpeg(const fs::path& extra) {
    if (!extra.empty() && fs::exists(extra / L"ffmpeg.exe")) {
        std::wstring path(GetEnvironmentVariableW(L"PATH", nullptr, 0), L'\0');
        path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
        if (path.find(extra.wstring()) == std::wstring::npos) SetEnvironmentVariableW(L"PATH", (extra.wstring() + L";" + path).c_str());
    }
    wchar_t found[MAX_PATH];
    return SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) &&
           SearchPathW(nullptr, L"ffprobe.exe", nullptr, MAX_PATH, found, nullptr);
}

bool game_running() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool running = false;
    for (auto ok = Process32FirstW(snapshot, &entry); ok && !running; ok = Process32NextW(snapshot, &entry))
        running = _wcsicmp(entry.szExeFile, L"Skate.exe") == 0;
    CloseHandle(snapshot);
    return running;
}

// The shell's file dialog: audio files (several) or one folder.
std::vector<fs::path> pick(HWND owner, bool folder, bool image = false, bool package = false, fs::path initial = {}, bool savefile = false) {
    static fs::path last_audio_folder;
    std::vector<fs::path> result;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | (folder ? FOS_PICKFOLDERS : image ? 0 : FOS_ALLOWMULTISELECT) | FOS_FORCEFILESYSTEM);
    if (initial.empty() && !folder && !image && !package && !savefile) initial = last_audio_folder;
    if (!initial.empty() && fs::is_directory(initial)) {
        ComPtr<IShellItem> directory;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&directory)))) dialog->SetFolder(directory.Get());
    }
    if (!folder) {
        const COMDLG_FILTERSPEC filters[]{{savefile ? L"ReSkate Music Pack saves" : package ? L"Thunderstore packages" : image ? L"Images" : L"Audio", savefile ? L"*.rmp" : package ? L"*.zip" : image ? L"*.png;*.jpg;*.jpeg;*.webp;*.bmp" : L"*.mp3;*.flac;*.ogg;*.opus;*.wav;*.m4a;*.aac;*.wma;*.aiff;*.aif;*.webm;*.mka;*.mp4"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, filters);
    }
    if (FAILED(dialog->Show(owner))) return result;
    ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(&items))) return result;
    DWORD count{};
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path{};
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) result.emplace_back(path);
        CoTaskMemFree(path);
    }
    if (!folder && !image && !package && !savefile && !result.empty()) last_audio_folder = result.front().parent_path();
    return result;
}

fs::path pick_save(HWND owner, const fs::path& suggested = {}) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    const COMDLG_FILTERSPEC filters[]{{L"ReSkate Music Pack Save", L"*.rmp"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetDefaultExtension(L"rmp");
    fs::path initial = suggested.empty() ? fs::path{} : suggested.parent_path();
    PWSTR downloads{};
    if (initial.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &downloads))) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(downloads, nullptr, IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
        CoTaskMemFree(downloads);
    }
    if (!initial.empty() && fs::is_directory(initial)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
    }
    if (!suggested.empty()) dialog->SetFileName(suggested.filename().c_str());
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path{};
    fs::path result;
    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

// Thunderstore archives contain the built assets but intentionally do not contain the source audio
// or editor project. Extract them to a stable temporary folder so the package can still be inspected.
fs::path extract_thunderstore(const fs::path& archive) {
    const auto root = fs::temp_directory_path() / L"ReSkateMusicPacker" / L"thunderstore_import" /
                      (archive.stem().wstring() + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    fs::create_directories(root, ec);
    std::ifstream input(archive, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::byte> archiveBytes(raw.size());
    if (!raw.empty()) std::memcpy(archiveBytes.data(), raw.data(), raw.size());
    if (!input && archiveBytes.empty()) throw std::runtime_error("Could not read Thunderstore package: " + narrow(archive.wstring()));
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, archiveBytes.data(), archiveBytes.size(), 0))
        throw std::runtime_error("Could not open Thunderstore package: " + narrow(archive.wstring()));
    struct Guard { mz_zip_archive* zip; ~Guard() { mz_zip_reader_end(zip); } } guard{&zip};
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        char name[4096]{};
        if (!mz_zip_reader_get_filename(&zip, i, name, sizeof(name)) || mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        std::string entry(name);
        std::replace(entry.begin(), entry.end(), '\\', '/');
        fs::path relative = fs::path(widen(entry)).lexically_normal();
        if (relative.empty() || relative.is_absolute() || relative == L".." || relative.string().starts_with(".."))
            throw std::runtime_error("Thunderstore package contains an unsafe path.");
        const auto destination = root / relative;
        fs::create_directories(destination.parent_path(), ec);
        size_t extractedSize{};
        auto* extracted = mz_zip_reader_extract_to_heap(&zip, i, &extractedSize, 0);
        if (!extracted) throw std::runtime_error("Could not extract Thunderstore package entry: " + entry);
        std::ofstream out(destination, std::ios::binary);
        out.write(static_cast<const char*>(extracted), static_cast<std::streamsize>(extractedSize));
        mz_free(extracted);
        if (!out)
            throw std::runtime_error("Could not extract Thunderstore package entry: " + entry);
    }
    return root;
}

// A dropped or chosen folder brings in its audio files, including subfolders, sorted by name.
std::vector<fs::path> expand(const std::vector<fs::path>& paths) {
    static const std::set<std::wstring> audio{
        L".mp3", L".flac", L".ogg", L".opus", L".wav", L".m4a", L".aac", L".wma",
        L".aiff", L".aif", L".webm", L".mka", L".mp4"
    };
    std::vector<fs::path> files;
    for (const auto& path : paths) {
        std::error_code error;
        if (!fs::is_directory(path, error)) { files.push_back(path); continue; }
        std::vector<fs::path> inside;
        for (const auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, error)) {
            if (error) break;
            std::error_code ec;
            if (!entry.is_regular_file(ec)) continue;
            auto extension = entry.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
            if (audio.contains(extension)) inside.push_back(entry.path());
        }
        std::sort(inside.begin(), inside.end());
        files.insert(files.end(), inside.begin(), inside.end());
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

// A folder name from the mod's name: letters, digits, '-' and '_' ("Kevin MacLeod" -> KevinMacLeod).
std::wstring folder_name(const std::string& name) {
    std::string out;
    for (unsigned char c : name) if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
    return widen(out.empty() ? "ReSkateMusic" : out);
}

// ---- The window's state --------------------------------------------------------------------------
struct Row {
    fs::path file;
    std::array<char, 256> artist{}, title{};
    std::array<char, 128> playlist{};
    double seconds{};
    std::vector<std::string> problems; // from scan()
    bool scanned{};
    fs::path artwork;
    bool has_embedded_artwork{};
    bool source_available{true};
};

struct ArtworkPreviewResult {
    std::uint64_t generation{};
    std::vector<unsigned char> pixels;
    UINT width{}, height{};
    std::string error;
};

// A finished ffmpeg download: the install folder on success, or a message fit to show.
struct FfmpegInstallResult {
    bool ok{};
    fs::path folder;
    std::string error;
};

struct App {
    Settings settings;
    bool ffmpeg{};
    bool settings_open{};
    std::array<char, 128> name{}, playlist{};
    int bitrate = 2; // index into bitrates
    bool normalize = true;
    std::map<std::string, fs::path> playlist_artwork;
    std::set<std::string> generated_playlist_artwork;
    Renderer* renderer{};
    ImTextureID artwork_preview{};
    std::string artwork_preview_label, artwork_preview_error;
    std::uint64_t artwork_preview_generation{}; // UI-owned; workers capture a value, never read it
    bool artwork_preview_loading{};
    std::vector<Row> rows;
    fs::path output; // empty: Mods\<folder_name(name)>
    fs::path save_file;
    bool thunderstore_read_only{}; // imported packages have no source audio to rebuild
    std::uint64_t last_autosave{};
    std::string autosave_signature;
    std::string status;
    bool status_error{};

    // One background job at a time (the packer shares a scratch folder): scanning added files or building.
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> busy{}, cancel{};
    std::string job;                                    // "Scanning" / "Building"
    float progress{};
    std::string progress_text;
    std::vector<std::pair<fs::path, music::SongInfo>> scanned;  // finished scans to apply
    std::optional<std::pair<bool, std::string>> finished;       // a build's result: ok, message
    std::optional<ArtworkPreviewResult> artwork_preview_ready; // guarded by mutex
    std::optional<FfmpegInstallResult> ffmpeg_install;          // a download's result, guarded by mutex

    std::vector<fs::path> dropped;
    std::mutex dropped_mutex;

    bool show_export_ts = false;
    bool show_artwork_modal = false;
    bool show_playlist_artwork_prompt = false;
    std::array<char, 64> ts_author{"Author"};
    std::array<char, 32> ts_version{"1.0.0"};
    std::array<char, 256> ts_description{};
    fs::path ts_icon;
    fs::path ts_output_folder;
    bool ts_open_explorer = true;
    bool ts_readme_credit = true;

    std::string active_playlist_filter;
    std::optional<std::string> select_playlist_tab;
    std::vector<std::string> custom_playlists;
    bool show_new_playlist_modal = false;
    std::array<char, 64> new_playlist_input{};
    int new_playlist_row_target = -1;

    std::map<std::string, ExternalSong> external_songs;
} *g_app;

void set_status(App& app, std::string text, bool error = false) { app.status = std::move(text); app.status_error = error; }
void clear_artwork_preview(App& app) {
    ++app.artwork_preview_generation; // Invalidate any result still being prepared.
    app.artwork_preview_loading = false;
    if (app.artwork_preview) app.renderer->release_texture(app.artwork_preview);
    app.artwork_preview = {};
    app.artwork_preview_label.clear();
    app.artwork_preview_error.clear();
}

fs::path output_folder(const App& app) {
    return !app.output.empty() ? app.output : app.settings.game / L"Mods" / folder_name(app.name.data());
}

Json editor_save_json(const App& app);

// Keep the editor state independent of Build. The last few backups make the destructive New Mod
// button recoverable even if the user changed tags or playlists since the previous build.
void autosave_project(App& app, bool force = false) {
    if (app.thunderstore_read_only || app.rows.empty() || !app.name[0] || !app.playlist[0]) return;
    const auto now = GetTickCount64();
    if (!force && now - app.last_autosave < 1000) return;
    const auto folder = !app.output.empty() ? app.output : settings_file().parent_path() / L"autosave";
    const auto path = folder / L"reskate-music-project.json";
    try {
        const auto serialized = editor_save_json(app).dump(2) + "\n";
        if (serialized == app.autosave_signature) {
            app.last_autosave = now;
            return;
        }
        fs::create_directories(folder);
        if (fs::exists(path)) {
            std::error_code ec;
            for (int slot = 4; slot >= 1; --slot) {
                const auto from = folder / (L"reskate-music-project.json.bak" + (slot == 1 ? std::wstring{} : L"." + std::to_wstring(slot - 1)));
                const std::wstring backupName = L"reskate-music-project.json.bak." + std::to_wstring(slot);
                const auto to = folder / backupName;
                if (fs::exists(from, ec)) { fs::remove(to, ec); fs::rename(from, to, ec); }
            }
            fs::copy_file(path, folder / L"reskate-music-project.json.bak", fs::copy_options::overwrite_existing, ec);
        }
        const auto temporary = path.wstring() + L".tmp";
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << serialized;
        out.close();
        std::error_code ec;
        fs::remove(path, ec);
        fs::rename(temporary, path, ec);
        if (ec) fs::copy_file(temporary, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(temporary, ec);
        app.autosave_signature = serialized;
        app.last_autosave = now;
    } catch (...) {
        // Autosave must never interrupt editing; the normal Build error still reports write failures.
    }
}

Json editor_save_json(const App& app) {
    auto root = Json::object();
    root["schema"] = 1;
    root["name"] = app.name.data();
    root["playlist"] = app.playlist.data();
    root["bitrate"] = std::stoi(bitrates[app.bitrate]);
    root["normalize"] = app.normalize;
    auto playlistArt = Json::object();
    for (const auto& [name, image] : app.playlist_artwork)
        if (!image.empty()) playlistArt[name] = narrow(fs::absolute(image).wstring());
    root["playlist_artwork"] = std::move(playlistArt);
    root["generated_playlist_artwork"] = Json::array();
    for (const auto& name : app.generated_playlist_artwork) root["generated_playlist_artwork"].push_back(name);
    auto songs = Json::array();
    for (const auto& row : app.rows) {
        auto song = Json::object();
        song["file"] = narrow(fs::absolute(row.file).wstring());
        song["artist"] = row.artist.data();
        song["title"] = row.title.data();
        if (row.playlist[0]) song["playlist"] = row.playlist.data();
        if (!row.artwork.empty()) song["artwork"] = narrow(fs::absolute(row.artwork).wstring());
        songs.push_back(std::move(song));
    }
    root["songs"] = std::move(songs);
    return root;
}

void save_editor_file(App& app, const fs::path& path) {
    if (path.empty() || app.rows.empty()) return;
    try {
        fs::create_directories(path.parent_path());
        if (fs::exists(path)) fs::copy_file(path, path.wstring() + L".bak", fs::copy_options::overwrite_existing);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write save file: " + narrow(path.wstring()));
        out << editor_save_json(app).dump(2) << "\n";
        set_status(app, "Saved " + narrow(path.filename().wstring()) + ".");
    } catch (const std::exception& error) { set_status(app, error.what(), true); }
}

std::vector<std::byte> read_save_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
    if (!in && raw.empty()) throw std::runtime_error("Cannot read " + narrow(path.wstring()));
    std::vector<std::byte> bytes(raw.size());
    if (!raw.empty()) std::memcpy(bytes.data(), raw.data(), raw.size());
    return bytes;
}

void save_portable_file(App& app, const fs::path& path) {
    if (path.empty() || app.rows.empty()) return;
    const auto staging = fs::temp_directory_path() / L"ReSkateMusicPacker" /
                         (L"portable_save-" + std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(staging);
        auto root = editor_save_json(app);
        auto copy_asset = [&](const fs::path& source, const fs::path& relative) {
            if (source.empty() || !fs::is_regular_file(source)) throw std::runtime_error("Missing save asset: " + narrow(source.wstring()));
            const auto destination = staging / relative;
            fs::create_directories(destination.parent_path());
            std::error_code ec;
            fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
            if (ec) throw std::runtime_error("Could not copy save asset: " + narrow(source.wstring()));
            return relative.generic_string();
        };
        auto& songs = root["songs"];
        for (std::size_t i = 0; i < songs.size(); ++i) {
            const auto source = fs::path(widen(songs[i]["file"].string()));
            const auto relative = fs::path(L"audio") / (std::to_wstring(i) + source.extension().wstring());
            songs[i]["file"] = copy_asset(source, relative);
            if (songs[i].contains("artwork")) {
                const auto artwork = fs::path(widen(songs[i]["artwork"].string()));
                songs[i]["artwork"] = copy_asset(artwork, fs::path(L"artwork") / (L"track-" + std::to_wstring(i) + artwork.extension().wstring()));
            }
        }
        auto& playlistArt = root["playlist_artwork"];
        std::size_t playlistIndex = 0;
        for (auto& [name, image] : playlistArt.items()) {
            const auto source = fs::path(widen(image.string()));
            image = copy_asset(source, fs::path(L"artwork") / (L"playlist-" + std::to_wstring(playlistIndex++) + source.extension().wstring()));
        }
        std::ofstream manifest(staging / L"save.json", std::ios::binary);
        manifest << root.dump(2) << "\n";
        manifest.close();

        mz_zip_archive zip{};
        if (!mz_zip_writer_init_heap(&zip, 0, 0)) throw std::runtime_error("Could not create portable save");
        struct Guard { mz_zip_archive* zip; ~Guard() { mz_zip_writer_end(zip); } } guard{&zip};
        for (const auto& entry : fs::recursive_directory_iterator(staging)) {
            if (!entry.is_regular_file()) continue;
            const auto name = fs::relative(entry.path(), staging).generic_string();
            const auto bytes = read_save_bytes(entry.path());
            if (!mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(), static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)))
                throw std::runtime_error("Could not add portable save entry: " + name);
        }
        void* buffer{};
        std::size_t size{};
        if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) || !buffer)
            throw std::runtime_error("Could not finalize portable save");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
        mz_free(buffer);
        if (!out) throw std::runtime_error("Could not write portable save: " + narrow(path.wstring()));
        set_status(app, "Saved portable " + narrow(path.filename().wstring()) + ".");
    } catch (const std::exception& error) { set_status(app, error.what(), true); }
    std::error_code ignored;
    fs::remove_all(staging, ignored);
}

// Scans other installed mods in Mods/ and the game's content cache for existing songs to warn on clash.
void refresh_external_songs(App& app) {
    app.external_songs.clear();
    if (!game_folder(app.settings.game)) return;

    std::error_code ec;
    const auto mods_folder = app.settings.game / L"Mods";
    if (fs::exists(mods_folder, ec) && fs::is_directory(mods_folder, ec)) {
        for (const auto& entry : fs::directory_iterator(mods_folder, ec)) {
            if (!entry.is_directory(ec)) continue;
            const auto mod_path = entry.path();
            const auto mod_name = narrow(mod_path.filename().wstring());

            const auto music_json = mod_path / L"reskate-music.json";
            if (fs::exists(music_json, ec)) {
                try {
                    std::ifstream in(music_json, std::ios::binary);
                    if (in) {
                        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
                        if (root.contains("playlists") && root["playlists"].is_array()) {
                            for (const auto& pl : root["playlists"]) {
                                if (pl.contains("songs") && pl["songs"].is_array()) {
                                    for (const auto& s : pl["songs"]) {
                                        if (s.is_string()) {
                                            app.external_songs.try_emplace(lower(s.get<std::string>()),
                                                ExternalSong{"mod '" + mod_name + "'", mod_path});
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {}
            }

            const auto proj_json = mod_path / L"reskate-music-project.json";
            if (fs::exists(proj_json, ec)) {
                try {
                    std::ifstream in(proj_json, std::ios::binary);
                    if (in) {
                        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
                        if (root.contains("songs") && root["songs"].is_array()) {
                            for (const auto& s : root["songs"]) {
                                const auto artist = s.value("artist", "");
                                const auto title = s.value("title", "");
                                if (!artist.empty() && !title.empty()) {
                                    app.external_songs.try_emplace(lower(artist + " - " + title),
                                        ExternalSong{"mod '" + mod_name + "'", mod_path});
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }
    }

    try {
        PWSTR local_appdata{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_appdata))) {
            const auto cache_dir = fs::path(local_appdata) / L"ReSkate" / L"cache";
            CoTaskMemFree(local_appdata);
            if (fs::exists(cache_dir, ec) && fs::is_directory(cache_dir, ec)) {
                for (const auto& entry : fs::recursive_directory_iterator(cache_dir, fs::directory_options::skip_permission_denied, ec)) {
                    if (entry.is_regular_file(ec) && entry.path().extension() == L".cache") {
                        std::ifstream in(entry.path(), std::ios::binary);
                        if (!in) continue;
                        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        for (std::size_t i = 0; i + 4 < buf.size(); ++i) {
                            if (buf[i] == 0x12) {
                                std::size_t len = buf[i + 1];
                                std::size_t header_len = 2;
                                if (len & 0x80) {
                                    len = (len & 0x7F) | ((buf[i + 2] & 0x7F) << 7);
                                    header_len = 3;
                                }
                                if (len >= 5 && len < 200 && i + header_len + len <= buf.size()) {
                                    std::string s(reinterpret_cast<const char*>(&buf[i + header_len]), len);
                                    if (s.find(" - ") != std::string::npos && s.find('\0') == std::string::npos) {
                                        bool printable = true;
                                        for (char c : s) {
                                            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
                                                printable = false;
                                                break;
                                            }
                                        }
                                        if (printable) {
                                            app.external_songs.try_emplace(lower(s), ExternalSong{"the game soundtrack", {}});
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {}
}

void start(App& app, std::string job, std::function<void()> work) {
    if (app.worker.joinable()) app.worker.join();
    app.busy = true;
    app.cancel = false;
    app.job = std::move(job);
    app.progress = 0;
    app.progress_text.clear();
    app.worker = std::thread([&app, work = std::move(work)] {
        work();
        app.busy = false;
    });
}

void request_artwork_preview(App& app, std::string label, fs::path source, bool generated,
    std::vector<std::pair<fs::path, fs::path>> tracks = {}) {
    if (app.busy) return;
    clear_artwork_preview(app);
    app.artwork_preview_label = label;
    app.artwork_preview_loading = true;
    const auto generation = app.artwork_preview_generation;
    // Resolve embedded art, run FFmpeg and decode pixels off the UI thread. Inputs are
    // snapshots; row edits and renderer resources are never accessed by this worker.
    start(app, "Loading artwork", [&app, label = std::move(label), source = std::move(source), generated,
        tracks = std::move(tracks), generation]() mutable {
        ArtworkPreviewResult result;
        result.generation = generation;
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Com { HRESULT status; ~Com() { if (SUCCEEDED(status)) CoUninitialize(); } } cleanup{com};
        try {
            if (FAILED(com)) throw std::runtime_error("Could not initialise artwork decoding.");
            if (source.empty() && !generated)
                for (const auto& [file, artwork] : tracks) {
                    if (app.cancel) throw music::Cancelled();
                    source = artwork.empty() ? music::embedded_artwork(file) : artwork;
                    if (!source.empty()) break;
                }
            if (app.cancel) throw music::Cancelled();
            if (!generated && source.empty()) throw std::runtime_error("No artwork available for this cover.");
            const auto png = generated ? music::playlist_artwork_png(label) : music::image_artwork_png(source);
            if (app.cancel) throw music::Cancelled();
            std::vector<unsigned char> bytes(png.size());
            std::memcpy(bytes.data(), png.data(), png.size());
            if (!dingosdk::launcher_gui::detail::decode_image(bytes, ImVec2(512, 512), result.pixels, result.width, result.height))
                throw std::runtime_error("Could not preview the cover image.");
        } catch (const std::exception& error) { result.error = error.what(); }
        std::lock_guard lock(app.mutex);
        app.artwork_preview_ready = std::move(result);
    });
}

void apply_artwork_preview(App& app) {
    std::optional<ArtworkPreviewResult> ready;
    {
        std::lock_guard lock(app.mutex);
        ready = std::move(app.artwork_preview_ready);
        app.artwork_preview_ready.reset();
    }
    if (!ready || ready->generation != app.artwork_preview_generation) return;
    app.artwork_preview_loading = false;
    app.artwork_preview_error = std::move(ready->error);
    if (!app.artwork_preview_error.empty()) return;
    // Direct3D resources remain owned by the UI thread.
    try {
        const auto texture = app.renderer->upload_texture(ready->pixels, ready->width, ready->height);
        if (!texture) app.artwork_preview_error = "Could not display the cover preview.";
        else app.artwork_preview = texture;
    } catch (const std::exception& error) { app.artwork_preview_error = error.what(); }
}

std::vector<std::pair<fs::path, fs::path>> playlist_tracks(const App& app, const std::string& playlist_name) {
    std::vector<std::pair<fs::path, fs::path>> tracks;
    for (const auto& row : app.rows) {
        const auto pl = row.playlist[0] ? row.playlist.data() : app.playlist.data();
        if (playlist_name == pl) tracks.emplace_back(row.file, row.artwork);
    }
    return tracks;
}

void request_playlist_preview(App& app, const std::string& playlist_name) {
    auto it = app.playlist_artwork.find(playlist_name);
    fs::path source = (it != app.playlist_artwork.end()) ? it->second : fs::path{};
    const bool gen = app.generated_playlist_artwork.contains(playlist_name);
    auto tracks = playlist_tracks(app, playlist_name);
    request_artwork_preview(app, playlist_name, std::move(source), gen, std::move(tracks));
}

// Downloads and installs the pinned ffmpeg build on the one-job worker. Never runs by itself: the
// button that calls this shows the source, size and licence first.
void install_ffmpeg(App& app) {
    if (app.busy) return;
    start(app, "Downloading ffmpeg", [&app] {
        FfmpegInstallResult result;
        try {
            const fs::path directory = music::default_install_dir();
            music::ensure_ffmpeg(directory, music::ffmpeg_url(), music::ffmpeg_sha256(),
                [&app](const music::DownloadProgress& step) {
                    std::lock_guard lock(app.mutex);
                    constexpr double megabytes = 1024.0 * 1024.0;
                    const auto received = static_cast<int>(static_cast<double>(step.received) / megabytes);
                    if (step.total) {
                        app.progress = static_cast<float>(static_cast<double>(step.received) / static_cast<double>(step.total));
                        app.progress_text = std::to_string(received) + " / " +
                                            std::to_string(static_cast<int>(static_cast<double>(step.total) / megabytes)) + " MB";
                    } else app.progress_text = std::to_string(received) + " MB";
                }, &app.cancel);
            result.ok = true;
            result.folder = directory;
        } catch (const music::Cancelled&) {
            result.error = "Cancelled; ffmpeg was not installed.";
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        std::lock_guard lock(app.mutex);
        app.ffmpeg_install = std::move(result);
    });
}

void add_files(App& app, const std::vector<fs::path>& paths, const std::string& target_playlist = "") {
    auto files = expand(paths);
    std::erase_if(files, [&](const fs::path& file) {
        return std::any_of(app.rows.begin(), app.rows.end(), [&](const Row& row) { return row.file == file; });
    });
    if (files.empty() || app.busy) return;
    const std::string pl = !target_playlist.empty() ? target_playlist : app.active_playlist_filter;
    for (const auto& file : files) {
        Row row{file};
        if (!pl.empty() && pl != (app.playlist[0] ? app.playlist.data() : "Default")) {
            copy_text(row.playlist, pl);
        }
        app.rows.push_back(std::move(row));
    }
    start(app, "Scanning", [&app, files] {
        for (std::size_t i = 0; i < files.size() && !app.cancel; ++i) {
            const std::vector<fs::path> one{files[i]};
            auto info = music::scan(one)[0];
            std::lock_guard lock(app.mutex);
            app.scanned.emplace_back(files[i], std::move(info));
            app.progress = static_cast<float>(i + 1) / static_cast<float>(files.size());
            app.progress_text = narrow(files[i].filename().wstring());
        }
    });
}

void readd_audio(App& app, std::size_t index, HWND window) {
    if (app.busy || index >= app.rows.size()) return;
    const auto files = pick(window, false);
    if (files.empty()) return;
    const auto file = files.front();
    auto& row = app.rows[index];
    row.file = file;
    row.scanned = false;
    row.source_available = true;
    row.seconds = 0;
    row.problems.clear();
    app.thunderstore_read_only = false;
    start(app, "Scanning", [&app, file] {
        auto info = music::scan(std::vector<fs::path>{file})[0];
        std::lock_guard lock(app.mutex);
        app.scanned.emplace_back(file, std::move(info));
        app.progress = 1.0f;
    });
    set_status(app, "Re-added source audio for track " + std::to_string(index + 1) + ".");
}

void open_mod(App& app, const fs::path& folder) {
    try {
        const auto project = music::load_project(folder);
        clear_artwork_preview(app);
        app.rows.clear();
        app.active_playlist_filter.clear();
        app.custom_playlists.clear();
        app.select_playlist_tab.reset();
        copy_text(app.name, project.name);
        copy_text(app.playlist, project.playlist);
        app.bitrate = 2;
        for (int i = 0; i < 5; ++i) if (std::stoi(bitrates[i]) == project.bitrate) app.bitrate = i;
        app.normalize = project.normalize;
        app.playlist_artwork = project.playlist_artwork;
        app.generated_playlist_artwork = project.generated_playlist_artwork;
        app.output = folder;
        app.save_file.clear();
        app.autosave_signature.clear();
        app.thunderstore_read_only = false;
        {
            std::lock_guard lock(app.mutex);
            app.scanned.clear();
        }
        refresh_external_songs(app);
        std::vector<fs::path> files;
        for (const auto& song : project.songs) {
            Row row{song.file};
            copy_text(row.artist, song.artist);
            copy_text(row.title, song.title);
            copy_text(row.playlist, song.playlist);
            row.artwork = song.artwork;
            row.source_available = fs::exists(song.file);
            if (!row.source_available) {
                row.scanned = true;
                row.problems.push_back("source audio file is missing; re-add it from the row menu");
            }
            app.rows.push_back(row);
            if (row.source_available) files.push_back(song.file);
        }
        // Scan for lengths and problems; the project's artist/title stay (see apply_scans).
        start(app, "Scanning", [&app, files] {
            for (std::size_t i = 0; i < files.size() && !app.cancel; ++i) {
                const std::vector<fs::path> one{files[i]};
                auto info = music::scan(one)[0];
                std::lock_guard lock(app.mutex);
                app.scanned.emplace_back(files[i], std::move(info));
                app.progress = static_cast<float>(i + 1) / static_cast<float>(files.size());
            }
        });
        set_status(app, "Opened " + narrow(folder.filename().wstring()) + ".");
    } catch (const std::exception& error) {
        set_status(app, error.what(), true);
    }
}

void open_saved_file(App& app, const fs::path& path) {
    try {
        const auto bytes = read_save_bytes(path);
        fs::path saveRoot = path.parent_path();
        fs::path manifestPath = path;
        if (bytes.size() >= 2 && bytes[0] == std::byte{'P'} && bytes[1] == std::byte{'K'}) {
            saveRoot = extract_thunderstore(path);
            manifestPath = saveRoot / L"save.json";
        }
        std::ifstream in(manifestPath, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read save file: " + narrow(path.wstring()));
        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
        if (root.value("schema", 0) != 1) throw std::runtime_error("unsupported save schema");
        music::Project project;
        project.name = root.at("name").string();
        project.playlist = root.at("playlist").string();
        project.bitrate = root.at("bitrate").get<int>();
        project.normalize = root.contains("normalize") ? root.at("normalize").get<bool>() : true;
        if (root.contains("generated_playlist_artwork"))
            for (const auto& name : root.at("generated_playlist_artwork")) project.generated_playlist_artwork.insert(name.string());
        const auto resolve = [&](const std::string& value) {
            const fs::path candidate = widen(value);
            return candidate.is_absolute() ? candidate : saveRoot / candidate;
        };
        if (root.contains("playlist_artwork"))
            for (const auto& [name, image] : root.at("playlist_artwork").items())
                project.playlist_artwork[name] = resolve(image.string());
        for (const auto& song : root.at("songs")) {
            music::SongInfo info{resolve(song.at("file").string()), song.at("artist").string(), song.at("title").string()};
            if (song.contains("playlist")) info.playlist = song.at("playlist").string();
            if (song.contains("artwork")) info.artwork = resolve(song.at("artwork").string());
            project.songs.push_back(std::move(info));
        }
        clear_artwork_preview(app);
        app.rows.clear();
        app.active_playlist_filter.clear();
        app.custom_playlists.clear();
        app.select_playlist_tab.reset();
        copy_text(app.name, project.name);
        copy_text(app.playlist, project.playlist);
        app.bitrate = 2;
        for (int i = 0; i < 5; ++i) if (std::stoi(bitrates[i]) == project.bitrate) app.bitrate = i;
        app.normalize = project.normalize;
        app.playlist_artwork = project.playlist_artwork;
        app.generated_playlist_artwork = project.generated_playlist_artwork;
        app.output.clear();
        app.autosave_signature.clear();
        app.thunderstore_read_only = false;
        {
            std::lock_guard lock(app.mutex);
            app.scanned.clear();
        }
        refresh_external_songs(app);
        std::vector<fs::path> files;
        for (const auto& song : project.songs) {
            Row row{song.file};
            copy_text(row.artist, song.artist);
            copy_text(row.title, song.title);
            copy_text(row.playlist, song.playlist);
            row.artwork = song.artwork;
            row.source_available = fs::exists(song.file);
            if (!row.source_available) {
                row.scanned = true;
                row.problems.push_back("source audio file is missing; re-add it from the row menu");
            } else files.push_back(song.file);
            app.rows.push_back(std::move(row));
        }
        start(app, "Scanning", [&app, files] {
            for (const auto& file : files) {
                const auto info = music::scan(std::vector<fs::path>{file})[0];
                std::lock_guard lock(app.mutex);
                app.scanned.emplace_back(file, info);
                app.progress = files.empty() ? 1.0f : static_cast<float>(app.scanned.size()) / static_cast<float>(files.size());
            }
        });
        set_status(app, "Loaded save " + narrow(path.filename().wstring()) + ".");
    } catch (const std::exception& error) { set_status(app, error.what(), true); }
}

void open_thunderstore(App& app, fs::path package) {
    try {
        const auto folder = fs::is_directory(package) ? package : extract_thunderstore(package);
        const auto manifestPath = folder / L"manifest.json";
        const auto musicPath = folder / L"reskate-music.json";
        if (!fs::exists(manifestPath) || !fs::exists(musicPath))
            throw std::runtime_error("This is not a ReSkate Thunderstore package (manifest.json or reskate-music.json is missing).");

        std::ifstream manifestIn(manifestPath, std::ios::binary);
        std::ifstream musicIn(musicPath, std::ios::binary);
        const auto manifest = Json::parse(std::string(std::istreambuf_iterator<char>(manifestIn), {}));
        const auto metadata = Json::parse(std::string(std::istreambuf_iterator<char>(musicIn), {}));
        if (!metadata.contains("playlists") || !metadata["playlists"].is_array())
            throw std::runtime_error("The package has no readable music playlists.");

        clear_artwork_preview(app);
        app.rows.clear();
        app.active_playlist_filter.clear();
        app.custom_playlists.clear();
        app.select_playlist_tab.reset();
        app.playlist_artwork.clear();
        app.generated_playlist_artwork.clear();
        {
            std::lock_guard lock(app.mutex);
            app.scanned.clear();
        }
        copy_text(app.name, manifest.value("name", narrow(folder.filename().wstring())));
        app.playlist.fill(0);
        app.output = folder;
        app.save_file.clear();
        app.autosave_signature.clear();
        app.thunderstore_read_only = true;

        bool firstPlaylist = true;
        for (const auto& playlist : metadata["playlists"]) {
            const auto playlistName = playlist.value("name", "Imported playlist");
            const bool isDefaultPlaylist = firstPlaylist;
            if (isDefaultPlaylist) { copy_text(app.playlist, playlistName); firstPlaylist = false; }
            else app.custom_playlists.push_back(playlistName);
            if (playlist.contains("artwork") && playlist["artwork"].is_string()) {
                const auto artwork = fs::path(widen(playlist["artwork"].string()));
                app.playlist_artwork[playlistName] = folder / artwork;
            }
            if (playlist.contains("songs") && playlist["songs"].is_array()) {
                for (const auto& value : playlist["songs"]) {
                    if (!value.is_string()) continue;
                    const auto id = value.string();
                    const auto split = id.find(" - ");
                    if (split == std::string::npos) continue;
                    Row row{folder / L"[Thunderstore package]"};
                    copy_text(row.artist, id.substr(0, split));
                    copy_text(row.title, id.substr(split + 3));
                    if (!isDefaultPlaylist) copy_text(row.playlist, playlistName);
                    row.scanned = true;
                    row.source_available = false;
                    if (metadata.contains("song_artwork") && metadata["song_artwork"].contains(id))
                        row.artwork = folder / fs::path(widen(metadata["song_artwork"][id].string()));
                    app.rows.push_back(std::move(row));
                }
            }
        }
        refresh_external_songs(app);
        set_status(app, "Opened Thunderstore package in read-only mode. Source audio is not included.");
    } catch (const std::exception& error) {
        set_status(app, error.what(), true);
    }
}

void open_path(App& app, const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec) && fs::exists(path / L"reskate-music.json", ec)) open_thunderstore(app, path);
    else if (path.extension() == L".zip") open_thunderstore(app, path);
    else if (fs::is_directory(path, ec)) open_mod(app, path);
    else add_files(app, {path});
}

void apply_scans(App& app) {
    std::lock_guard lock(app.mutex);
    for (auto& [file, info] : app.scanned)
        for (auto& row : app.rows)
            if (row.file == file && !row.scanned) {
                row.scanned = true;
                row.seconds = info.seconds;
                row.has_embedded_artwork = info.has_embedded_artwork;
                // Problems about the tags fall away once artist and title are filled in by hand.
                std::erase_if(info.problems, [](const std::string& p) { return p.find("tag") != std::string::npos; });
                row.problems = info.problems;
                if (!row.artist[0]) copy_text(row.artist, info.artist);
                if (!row.title[0]) copy_text(row.title, info.title);
            }
    app.scanned.clear();
}

// Problems that block building, per row (empty: fine) and for the whole mod.
std::vector<std::string> row_problems(const App& app, std::size_t index) {
    const auto& row = app.rows[index];
    auto problems = row.problems;
    if (!row.source_available) problems.push_back("source audio must be re-added");
    if (!music::usable_name(row.artist.data())) problems.push_back("needs an artist");
    if (!music::usable_name(row.title.data())) problems.push_back("needs a title");
    if (row.playlist[0] && !music::usable_name(row.playlist.data())) problems.push_back("invalid playlist name");
    const auto id = std::string(row.artist.data()) + " - " + row.title.data();
    for (std::size_t other = 0; other < app.rows.size(); ++other)
        if (other != index && std::string(app.rows[other].artist.data()) + " - " + app.rows[other].title.data() == id) {
            problems.push_back("same artist and title as song " + std::to_string(other + 1));
            break;
        }
    if (row.artist[0] && row.title[0]) {
        if (auto it = app.external_songs.find(lower(id)); it != app.external_songs.end()) {
            std::error_code ec;
            const auto current_mod = output_folder(app);
            bool same_mod = false;
            if (!it->second.folder.empty()) {
                same_mod = fs::equivalent(it->second.folder, current_mod, ec);
                if (ec) {
                    ec.clear();
                    const auto existing = fs::weakly_canonical(it->second.folder, ec);
                    ec.clear();
                    const auto current = fs::weakly_canonical(current_mod, ec);
                    same_mod = !ec && existing == current;
                }
            }
            if (it->second.folder.empty() || !same_mod) {
                problems.push_back("already in " + it->second.source);
            }
        }
    }
    return problems;
}

void build(App& app) {
    autosave_project(app, true);
    music::PackOptions options;
    options.game = app.settings.game;
    options.output = output_folder(app);
    options.name = app.name.data();
    options.playlist = app.playlist.data();
    options.bitrate = std::stoi(bitrates[app.bitrate]);
    options.normalize = app.normalize;
    options.playlist_artwork = app.playlist_artwork;
    options.generated_playlist_artwork = app.generated_playlist_artwork;
    std::vector<music::SongInfo> songs;
    for (const auto& row : app.rows) {
        music::SongInfo song{row.file, row.artist.data(), row.title.data(), row.playlist.data()};
        song.artwork = row.artwork;
        songs.push_back(std::move(song));
    }
    start(app, "Building", [&app, options, songs] {
        std::pair<bool, std::string> result;
        try {
            const auto packed = music::pack(options, songs, [&app](const music::Progress& step) {
                std::lock_guard lock(app.mutex);
                const auto done = static_cast<float>(step.song) + (std::string(step.stage) == "encoded" ? 1.f : 0.f);
                app.progress = std::min(done / static_cast<float>(step.count + 1), 1.f);
                app.progress_text = step.song < step.count ? std::string(step.stage) + " song " + std::to_string(step.song + 1) + " of " +
                                                                 std::to_string(step.count)
                                                           : std::string(step.stage) + " the mod";
            }, &app.cancel);
            result = {true, "Built " + std::to_string(packed.songs) + " song(s) into " + narrow(packed.output.wstring()) +
                                ". Start the game through the ReSkate launcher to hear them."};
        } catch (const music::Cancelled&) {
            result = {false, "Cancelled; nothing was written."};
        } catch (const std::exception& error) {
            result = {false, error.what()};
        }
        std::lock_guard lock(app.mutex);
        app.finished = result;
    });
}

// ---- Drawing -------------------------------------------------------------------------------------
void apply_theme() {
    auto& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(S(16.0f), S(14.0f));
    style.FramePadding      = ImVec2(S(9.0f),  S(6.0f));
    style.CellPadding       = ImVec2(S(8.0f),  S(6.0f));
    style.ItemSpacing       = ImVec2(S(8.0f),  S(8.0f));
    style.ItemInnerSpacing  = ImVec2(S(6.0f),  S(6.0f));
    style.ScrollbarSize     = S(13.0f);
    style.GrabMinSize       = S(10.0f);

    style.WindowRounding    = S(0.0f);
    style.ChildRounding     = S(8.0f);
    style.FrameRounding     = S(6.0f);
    style.PopupRounding     = S(8.0f);
    style.ScrollbarRounding = S(6.0f);
    style.GrabRounding      = S(4.0f);
    style.TabRounding       = S(6.0f);

    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;


    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.13f, 0.14f, 0.17f, 0.98f);
    colors[ImGuiCol_Border]               = ImVec4(0.22f, 0.24f, 0.28f, 0.70f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.54f, 0.58f, 1.00f);

    colors[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.21f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.29f, 0.36f, 1.00f);

    colors[ImGuiCol_TitleBg]              = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.09f, 0.10f, 0.12f, 0.75f);
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);

    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.11f, 0.13f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.26f, 0.29f, 0.35f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.38f, 0.46f, 0.90f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.42f, 0.47f, 0.56f, 1.00f);

    colors[ImGuiCol_CheckMark]            = ImVec4(0.22f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.22f, 0.58f, 0.95f, 0.90f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.30f, 0.68f, 1.00f, 1.00f);

    colors[ImGuiCol_Button]               = ImVec4(0.19f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.26f, 0.31f, 0.39f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.16f, 0.20f, 0.25f, 1.00f);

    colors[ImGuiCol_Header]               = ImVec4(0.18f, 0.21f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.28f, 0.34f, 0.42f, 1.00f);

    colors[ImGuiCol_Separator]            = ImVec4(0.22f, 0.24f, 0.28f, 0.80f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.28f, 0.32f, 0.38f, 0.90f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(0.35f, 0.42f, 0.52f, 1.00f);

    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.22f, 0.58f, 0.95f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.22f, 0.58f, 0.95f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.22f, 0.58f, 0.95f, 0.95f);

    colors[ImGuiCol_TableHeaderBg]        = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]    = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_TableBorderLight]     = ImVec4(0.18f, 0.20f, 0.24f, 0.70f);
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.12f, 0.13f, 0.16f, 0.70f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(0.14f, 0.15f, 0.18f, 0.70f);

    colors[ImGuiCol_Tab]                  = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.24f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_TabActive]            = ImVec4(0.19f, 0.23f, 0.29f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.16f, 0.18f, 0.23f, 1.00f);

    colors[ImGuiCol_PlotHistogram]        = ImVec4(0.18f, 0.52f, 0.92f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 0.60f, 1.00f, 1.00f);

    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.05f, 0.05f, 0.07f, 0.65f);
}

void setup_page(App& app, HWND window) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.22f, 0.65f, 1.0f, 1.0f), "Game Setup");
    ImGui::Spacing();
    ImGui::TextWrapped("Pick the folder skate. is installed in (the one with Skate.exe and ReSkateLauncher.exe).");
    ImGui::Spacing();
    if (ImGui::Button("Choose the game folder...", ImVec2(S(220), S(32)))) {
        const auto folders = pick(window, true);
        if (!folders.empty()) {
            if (game_folder(folders[0])) {
                app.settings.game = folders[0];
                save_settings(app.settings);
                refresh_external_songs(app);
                set_status(app, "");
            } else set_status(app, "That folder has no Skate.exe.", true);
        }
    }
}

// The one-click installer, shared by first-run setup and the Settings dialog. Downloading only ever
// starts from a click here; the source, size and licence sit under the button.
void ffmpeg_download_controls(App& app, float width) {
    if (app.busy && app.job == "Downloading ffmpeg") {
        std::lock_guard lock(app.mutex);
        const std::string label = app.progress_text.empty() ? "Starting..." : app.progress_text;
        ImGui::ProgressBar(app.progress, ImVec2(width, S(26)), label.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1, S(26)))) app.cancel = true;
        return;
    }
    ImGui::BeginDisabled(app.busy);
    const bool clicked = ImGui::Button("Download ffmpeg automatically", ImVec2(width, S(32)));
    ImGui::EndDisabled();
    ImGui::TextDisabled("Static LGPL build from BtbN/FFmpeg-Builds on GitHub (~163 MB). "
                        "Downloaded only when you click, then checked against a pinned SHA-256.");
    if (clicked) install_ffmpeg(app);
}

void ffmpeg_page(App& app, HWND window) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.22f, 0.65f, 1.0f, 1.0f), "FFmpeg Dependency Required");
    ImGui::Spacing();
    ImGui::TextWrapped("The music packer uses ffmpeg to read and encode songs, and cannot find ffmpeg.exe and ffprobe.exe. "
                       "Install ffmpeg (for example a Windows build linked from the official download page), then "
                       "point at the folder that holds ffmpeg.exe.");
    ImGui::Spacing();
    ffmpeg_download_controls(app, S(300));
    ImGui::Spacing();
    ImGui::TextDisabled("- or install it yourself -");
    ImGui::Spacing();
    if (ImGui::Button("Open the ffmpeg download page", ImVec2(S(240), S(32))))
        ShellExecuteW(nullptr, L"open", L"https://ffmpeg.org/download.html", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("Locate ffmpeg...", ImVec2(S(160), S(32)))) {
        const auto folders = pick(window, true);
        if (!folders.empty()) {
            auto folder = folders[0];
            if (!fs::exists(folder / L"ffmpeg.exe") && fs::exists(folder / L"bin" / L"ffmpeg.exe")) folder /= L"bin";
            if (find_ffmpeg(folder)) {
                app.settings.ffmpeg = folder;
                save_settings(app.settings);
                app.ffmpeg = true;
                set_status(app, "");
            } else set_status(app, "That folder has no ffmpeg.exe and ffprobe.exe.", true);
        }
    }
}

// Reconfigures the game folder and ffmpeg after first-run setup; reachable from any page.
void settings_modal(App& app, HWND window) {
    if (app.settings_open) { ImGui::OpenPopup("Settings"); app.settings_open = false; }
    if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(540));

    ImGui::TextColored(ImVec4(0.22f, 0.65f, 1.0f, 1.0f), "Game folder");
    ImGui::TextWrapped("%s", app.settings.game.empty() ? "Not set." : narrow(app.settings.game.wstring()).c_str());
    if (ImGui::Button("Choose game folder...", ImVec2(S(190), S(28)))) {
        const auto folders = pick(window, true);
        if (!folders.empty()) {
            if (game_folder(folders[0])) {
                app.settings.game = folders[0];
                save_settings(app.settings);
                refresh_external_songs(app);
                set_status(app, "");
            } else set_status(app, "That folder has no Skate.exe.", true);
        }
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.22f, 0.65f, 1.0f, 1.0f), "ffmpeg");
    ImGui::TextWrapped("%s", app.ffmpeg ? "ffmpeg.exe and ffprobe.exe found."
                                        : "Not found - the packer needs ffmpeg.exe and ffprobe.exe.");
    if (!app.ffmpeg) {
        ffmpeg_download_controls(app, S(250));
        ImGui::Spacing();
    }
    if (ImGui::Button("Open the ffmpeg download page", ImVec2(S(230), S(28))))
        ShellExecuteW(nullptr, L"open", L"https://ffmpeg.org/download.html", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("Locate ffmpeg...", ImVec2(S(150), S(28)))) {
        const auto folders = pick(window, true);
        if (!folders.empty()) {
            auto folder = folders[0];
            if (!fs::exists(folder / L"ffmpeg.exe") && fs::exists(folder / L"bin" / L"ffmpeg.exe")) folder /= L"bin";
            if (find_ffmpeg(folder)) {
                app.settings.ffmpeg = folder;
                save_settings(app.settings);
                app.ffmpeg = true;
                set_status(app, "");
            } else set_status(app, "That folder has no ffmpeg.exe and ffprobe.exe.", true);
        }
    }

    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(S(120), S(28)))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void songs_page(App& app, HWND window) {
    const bool busy = app.busy;

    const std::string defaultPlaylistName = app.playlist[0] ? app.playlist.data() : "Default";
    std::vector<std::string> unique_playlists;
    std::map<std::string, std::pair<std::size_t, double>> playlist_stats;
    unique_playlists.push_back(defaultPlaylistName);
    playlist_stats[defaultPlaylistName] = {0, 0.0};

    for (const auto& cpl : app.custom_playlists) {
        if (std::find(unique_playlists.begin(), unique_playlists.end(), cpl) == unique_playlists.end()) {
            unique_playlists.push_back(cpl);
            playlist_stats[cpl] = {0, 0.0};
        }
    }

    for (const auto& r : app.rows) {
        const std::string pl = r.playlist[0] ? r.playlist.data() : defaultPlaylistName;
        if (std::find(unique_playlists.begin(), unique_playlists.end(), pl) == unique_playlists.end()) {
            unique_playlists.push_back(pl);
        }
        auto& stats = playlist_stats[pl];
        stats.first++;
        if (r.scanned) stats.second += r.seconds;
    }

    if (!app.active_playlist_filter.empty() &&
        std::find(unique_playlists.begin(), unique_playlists.end(), app.active_playlist_filter) == unique_playlists.end()) {
        app.active_playlist_filter.clear();
    }

    std::string artBtnLabel;
    std::string artTooltip;
    if (unique_playlists.size() > 1) {
        artBtnLabel = "Covers (" + std::to_string(unique_playlists.size()) + ")";
        artTooltip = "Configure artwork for each of the " + std::to_string(unique_playlists.size()) + " playlists in this mod";
    } else {
        const std::string activePlaylist = app.playlist[0] ? app.playlist.data() : "";
        artBtnLabel = "Cover: ";
        if (activePlaylist.empty()) {
            artBtnLabel += "Auto";
            artTooltip = "Automatic cover: using first track's album art (or none). Enter playlist name to customize.";
        } else if (const auto it = app.playlist_artwork.find(activePlaylist); it != app.playlist_artwork.end() && !it->second.empty()) {
            const auto fname = narrow(it->second.filename().wstring());
            artBtnLabel += (fname.size() > 10 ? fname.substr(0, 8) + ".." : fname);
            artTooltip = "Custom cover image: " + narrow(it->second.wstring());
        } else if (app.generated_playlist_artwork.contains(activePlaylist)) {
            artBtnLabel += "Text";
            artTooltip = "Generated text cover styled with playlist name";
        } else {
            artBtnLabel += "Auto";
            artTooltip = "Automatic cover: using first track's album art (or none). Click to customize.";
        }
    }

    const auto& style = ImGui::GetStyle();
    const float new_btn_w = ImGui::CalcTextSize("New Mod").x + style.FramePadding.x * 2.0f;
    const float open_btn_w = ImGui::CalcTextSize("Open Mod...").x + style.FramePadding.x * 2.0f;
    const float file_btn_w = ImGui::CalcTextSize("File...").x + style.FramePadding.x * 2.0f;
    const float name_input_w = S(170.0f);
    const float playlist_input_w = S(160.0f);
    const float cover_btn_w = ImGui::CalcTextSize(artBtnLabel.c_str()).x + style.FramePadding.x * 2.0f;
    const float bitrate_w = S(80.0f);
    const float norm_w = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Normalize").x;
    const float settings_btn_w = ImGui::CalcTextSize("Settings...").x + style.FramePadding.x * 2.0f;
    const float group_gap = S(14.0f);

    const float total_header_w = new_btn_w + style.ItemSpacing.x + open_btn_w + style.ItemSpacing.x + file_btn_w
        + group_gap + name_input_w + style.ItemSpacing.x + playlist_input_w + style.ItemSpacing.x + cover_btn_w
        + group_gap + bitrate_w + style.ItemSpacing.x + norm_w + style.ItemSpacing.x + settings_btn_w;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.18f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.26f, 0.33f, 0.80f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12.0f), S(8.0f)));

    if (ImGui::BeginChild("header_toolbar", ImVec2(0, S(74.0f)), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w > total_header_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - total_header_w) * 0.5f);
        }

        ImGui::BeginDisabled(busy);
        if (ImGui::Button("New Mod", ImVec2(new_btn_w, 0))) {
            autosave_project(app, true);
            app.rows.clear();
            app.output.clear();
            app.save_file.clear();
            app.autosave_signature.clear();
            app.thunderstore_read_only = false;
            app.active_playlist_filter.clear();
            app.custom_playlists.clear();
            app.select_playlist_tab.reset();
            app.name.fill(0);
            app.playlist.fill(0);
            app.normalize = true;
            app.playlist_artwork.clear();
            app.generated_playlist_artwork.clear();
            clear_artwork_preview(app);
            set_status(app, "");
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Mod...", ImVec2(open_btn_w, 0))) {
            const auto mod_folder = app.settings.game / L"Mods";
            const auto open_folder = fs::is_directory(mod_folder) ? mod_folder : app.settings.game;
            const auto folders = pick(window, true, false, false, open_folder);
            if (!folders.empty()) open_path(app, folders[0]);
            else {
                const auto packages = pick(window, false, false, true, open_folder);
                if (!packages.empty()) open_path(app, packages[0]);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("File...", ImVec2(file_btn_w, 0))) ImGui::OpenPopup("file_menu");
        if (ImGui::BeginPopup("file_menu")) {
            if (ImGui::MenuItem("Load Save...")) {
                const auto files = pick(window, false, false, false, app.save_file.parent_path(), true);
                if (!files.empty()) { app.save_file = files[0]; open_saved_file(app, files[0]); }
            }
            if (ImGui::MenuItem("Save As...")) {
                const auto file = pick_save(window, app.save_file.empty() ? fs::path(std::string(app.name.data()) + ".rmp") : app.save_file);
                if (!file.empty()) { app.save_file = file; save_editor_file(app, file); }
            }
            if (ImGui::MenuItem("Save Portable...")) {
                const auto file = pick_save(window, app.save_file.empty() ? fs::path(std::string(app.name.data()) + ".rmp") : app.save_file);
                if (!file.empty()) { app.save_file = file; save_portable_file(app, file); }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, group_gap);
        ImGui::SetNextItemWidth(name_input_w);
        if (ImGui::InputTextWithHint("##name", "Mod name", app.name.data(), app.name.size()) && !app.output.empty() &&
            app.output.parent_path() == app.settings.game / L"Mods")
            app.output.clear(); // a renamed new mod goes to its new folder; an opened one stays where it is
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mod name (used for the mod folder in Mods/)");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(playlist_input_w);
        ImGui::InputTextWithHint("##playlist", "Default playlist", app.playlist.data(), app.playlist.size());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default playlist name in skate. Songs use this unless overridden in the table below.");

        ImGui::SameLine();
        if (ImGui::Button(artBtnLabel.c_str(), ImVec2(cover_btn_w, 0))) {
            ImGui::OpenPopup("PlaylistCoverPopup");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(artTooltip.c_str());
            if (app.artwork_preview) {
                ImGui::Spacing();
                ImGui::Image(app.artwork_preview, ImVec2(S(120), S(120)));
            }
            ImGui::EndTooltip();
        }

        if (ImGui::BeginPopup("PlaylistCoverPopup")) {
            static int selected_pl_idx = 0;
            if (selected_pl_idx >= static_cast<int>(unique_playlists.size())) selected_pl_idx = 0;

            bool selection_changed = false;
            if (unique_playlists.size() > 1) {
                std::vector<const char*> pl_ptrs;
                for (const auto& n : unique_playlists) pl_ptrs.push_back(n.c_str());
                ImGui::SetNextItemWidth(S(200));
                if (ImGui::Combo("Playlist##cover_combo", &selected_pl_idx, pl_ptrs.data(), static_cast<int>(pl_ptrs.size()))) {
                    selection_changed = true;
                }
                ImGui::Separator();
            }

            const auto pName = unique_playlists[selected_pl_idx];
            const bool isDef = (pName == defaultPlaylistName);
            ImGui::Text("Playlist: %s%s", pName.c_str(), isDef ? " (default)" : "");
            ImGui::Separator();

            if (app.playlist_artwork.contains(pName) && !app.playlist_artwork[pName].empty()) {
                ImGui::Text("Active: %s", narrow(app.playlist_artwork[pName].filename().wstring()).c_str());
            } else if (app.generated_playlist_artwork.contains(pName)) {
                ImGui::TextUnformatted("Active: Generated text cover");
            } else {
                ImGui::TextUnformatted("Active: Automatic (from first track)");
            }

            const bool artwork_busy = busy || app.artwork_preview_loading;
            if (selection_changed || (app.artwork_preview_label != pName && !artwork_busy)) {
                request_playlist_preview(app, pName);
            }

            ImGui::Spacing();
            if (app.artwork_preview_loading && app.artwork_preview_label == pName) {
                ImGui::BeginChild("cover_preview_box", ImVec2(S(128), S(128)), true);
                ImGui::TextDisabled("Loading preview...");
                ImGui::EndChild();
            } else if (app.artwork_preview && app.artwork_preview_label == pName) {
                ImGui::Image(app.artwork_preview, ImVec2(S(128), S(128)));
            } else {
                ImGui::BeginChild("cover_preview_box", ImVec2(S(128), S(128)), true);
                ImGui::Spacing();
                ImGui::TextDisabled("No cover");
                if (!app.artwork_preview_error.empty() && app.artwork_preview_label == pName) {
                    ImGui::Spacing();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(110));
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", app.artwork_preview_error.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndChild();
            }
            ImGui::Spacing();

            ImGui::BeginDisabled(artwork_busy);
            if (ImGui::Button("Choose Image...")) {
                const auto files = pick(window, false, true);
                if (!files.empty()) {
                    app.playlist_artwork[pName] = files[0];
                    app.generated_playlist_artwork.erase(pName);
                    request_playlist_preview(app, pName);
                }
            }

            const bool isGen = app.generated_playlist_artwork.contains(pName);
            if (ImGui::Button(isGen ? "Re-generate Text Cover" : "Generate Text Cover")) {
                app.generated_playlist_artwork.insert(pName);
                app.playlist_artwork[pName].clear();
                request_playlist_preview(app, pName);
            }

            if (ImGui::Button("Use Automatic")) {
                app.playlist_artwork[pName].clear();
                app.generated_playlist_artwork.erase(pName);
                request_playlist_preview(app, pName);
            }

            if (unique_playlists.size() > 1) {
                ImGui::Spacing();
                if (ImGui::Button("Generate Text for All Playlists")) {
                    for (const auto& n : unique_playlists) {
                        app.generated_playlist_artwork.insert(n);
                        app.playlist_artwork[n].clear();
                    }
                    request_playlist_preview(app, pName);
                }
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::Button("Manage Track Artwork...")) {
                app.show_artwork_modal = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::SameLine(0, group_gap);
        ImGui::SetNextItemWidth(bitrate_w);
        ImGui::Combo("##bitrate", &app.bitrate, bitrates, 5);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opus audio bitrate (kbps)");
        ImGui::SameLine();
        ImGui::Checkbox("Normalize", &app.normalize);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Equalise track loudness using EBU R128 (-15 LUFS)");
        ImGui::SameLine();
        if (ImGui::Button("Settings...", ImVec2(settings_btn_w, 0))) app.settings_open = true;
        ImGui::EndDisabled();

        ImGui::Spacing();
        const std::string builds_into = "Builds into: " + narrow(output_folder(app).wstring());
        const float builds_into_w = ImGui::CalcTextSize(builds_into.c_str()).x;
        if (avail_w > builds_into_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - builds_into_w) * 0.5f);
        }
        ImGui::TextDisabled("%s", builds_into.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    ImGui::Spacing();

    if (app.show_artwork_modal) {
        ImGui::OpenPopup("Track and playlist artwork");
        app.show_artwork_modal = false;
    }


    if (ImGui::BeginPopupModal("Track and playlist artwork", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(700));
        ImGui::TextWrapped("Track covers use embedded album art unless you choose an image. Playlists use their first track cover, or you can choose an image or generate a text cover.");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(busy);
        const auto preview = [&](const std::string& label, const fs::path& source, bool generated) {
            request_artwork_preview(app, label, source, generated);
        };
        const auto choose = [&](const std::string& label, fs::path& image) {
            bool selected = false;
            ImGui::PushID(label.c_str());
            ImGui::TextWrapped("%s", label.c_str());
            ImGui::TextDisabled("%s", image.empty() ? "Automatic cover" : narrow(image.filename().wstring()).c_str());
            if (!image.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(image.wstring()).c_str());
            if (ImGui::Button("Choose image...")) {
                const auto files = pick(window, false, true);
                if (!files.empty()) { image = files[0]; selected = true; preview(label, image, false); }
            }
            ImGui::SameLine();
            if (ImGui::Button("Use automatic")) { image.clear(); clear_artwork_preview(app); }
            ImGui::PopID();
            return selected;
        };
        if (ImGui::BeginChild("covers", ImVec2(S(480), S(360)))) {
            ImGui::TextUnformatted("Playlists");
            std::set<std::string> names;
            if (app.playlist[0]) names.insert(app.playlist.data());
            for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
            ImGui::PushID("playlists");
            for (const auto& name : names) {
                if (choose(name, app.playlist_artwork[name])) app.generated_playlist_artwork.erase(name);
                ImGui::PushID(name.c_str());
                bool generated = app.generated_playlist_artwork.contains(name);
                if (ImGui::Checkbox("Generate text cover", &generated)) {
                    if (generated) {
                        app.generated_playlist_artwork.insert(name);
                        app.playlist_artwork[name].clear();
                        request_playlist_preview(app, name);
                    } else {
                        app.generated_playlist_artwork.erase(name);
                        request_playlist_preview(app, name);
                    }
                }
                if (ImGui::Button("Preview")) {
                    request_playlist_preview(app, name);
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::PopID();
            ImGui::TextUnformatted("Tracks");
            for (std::size_t i = 0; i < app.rows.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                auto& row = app.rows[i];
                choose(std::string(row.artist.data()) + " - " + row.title.data(), row.artwork);
                if (row.artwork.empty()) ImGui::TextDisabled("%s", row.has_embedded_artwork ? "Embedded album art detected" : "No embedded album art detected");
                if (ImGui::Button("Preview"))
                    request_artwork_preview(app, std::string(row.artist.data()) + " - " + row.title.data(),
                        row.artwork, false, {{row.file, {}}});
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("cover-preview", ImVec2(S(220), S(360)))) {
            ImGui::TextWrapped("%s", app.artwork_preview_label.empty() ? "Cover preview" : app.artwork_preview_label.c_str());
            if (app.artwork_preview_loading) ImGui::TextWrapped("Loading artwork...");
            else if (app.artwork_preview) ImGui::Image(app.artwork_preview, ImVec2(S(200), S(200)));
            else ImGui::TextWrapped("Choose a cover or click Preview.");
            if (!app.artwork_preview_error.empty()) ImGui::TextWrapped("%s", app.artwork_preview_error.c_str());
        }
        ImGui::EndChild();
        ImGui::EndDisabled();
        if (ImGui::Button("Done", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (app.show_new_playlist_modal) {
        ImGui::OpenPopup("New Playlist");
        app.show_new_playlist_modal = false;
    }
    if (ImGui::BeginPopupModal("New Playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Enter a name for the new playlist:");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter_pressed = ImGui::InputText("##new_pl_name", app.new_playlist_input.data(), app.new_playlist_input.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        const bool has_name = music::usable_name(app.new_playlist_input.data());
        ImGui::BeginDisabled(!has_name);
        if (ImGui::Button("Create", ImVec2(S(100), 0)) || (enter_pressed && has_name)) {
            const std::string new_pl = app.new_playlist_input.data();
            if (std::find(app.custom_playlists.begin(), app.custom_playlists.end(), new_pl) == app.custom_playlists.end()) {
                app.custom_playlists.push_back(new_pl);
            }
            if (app.new_playlist_row_target >= 0 && app.new_playlist_row_target < static_cast<int>(app.rows.size())) {
                copy_text(app.rows[app.new_playlist_row_target].playlist, new_pl);
                app.new_playlist_row_target = -1;
            }
            app.active_playlist_filter = new_pl;
            app.select_playlist_tab = new_pl;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            app.new_playlist_row_target = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The songs.
    if (app.rows.empty()) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float card_w = std::min(avail.x - S(48.0f), S(720.0f));
        const float card_h = std::min(avail.y - S(20.0f), S(310.0f));
        const float offset_x = std::max(0.0f, (avail.x - card_w) * 0.5f);
        const float offset_y = std::max(0.0f, (avail.y - card_h) * 0.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset_y);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.31f, 0.42f, 0.75f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28.0f), S(20.0f)));

        if (ImGui::BeginChild("empty_state_card", ImVec2(card_w, card_h), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            auto center_text = [](const char* text, const ImVec4* color = nullptr) {
                const float text_w = ImGui::CalcTextSize(text).x;
                const float card_avail = ImGui::GetContentRegionAvail().x;
                if (card_avail > text_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (card_avail - text_w) * 0.5f);
                if (color) ImGui::TextColored(*color, "%s", text);
                else ImGui::TextUnformatted(text);
            };
            auto center_text_disabled = [](const char* text) {
                const float text_w = ImGui::CalcTextSize(text).x;
                const float card_avail = ImGui::GetContentRegionAvail().x;
                if (card_avail > text_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (card_avail - text_w) * 0.5f);
                ImGui::TextDisabled("%s", text);
            };

            // Audio waveform equalizer graphic
            const float icon_w = S(72.0f);
            const float icon_h = S(42.0f);
            const float card_avail = ImGui::GetContentRegionAvail().x;
            if (card_avail > icon_w) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (card_avail - icon_w) * 0.5f);
            }
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(icon_w, icon_h));

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const float heights[] = { 10.0f, 18.0f, 28.0f, 38.0f, 42.0f, 38.0f, 28.0f, 18.0f, 10.0f };
            const float bar_w = S(4.5f);
            const float bar_gap = S(3.8f);
            const int bar_count = 9;
            const float bars_total_w = bar_count * bar_w + (bar_count - 1) * bar_gap;
            const float start_x = p0.x + (icon_w - bars_total_w) * 0.5f;
            const float center_y = p0.y + icon_h * 0.5f;

            for (int b = 0; b < bar_count; ++b) {
                const float bh = heights[b] * (icon_h / 42.0f);
                const float bx0 = start_x + static_cast<float>(b) * (bar_w + bar_gap);
                const float by0 = center_y - bh * 0.5f;
                const float bx1 = bx0 + bar_w;
                const float by1 = center_y + bh * 0.5f;
                const float t = 1.0f - std::abs(static_cast<float>(b) - 4.0f) / 5.0f;
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    0.18f + 0.14f * t,
                    0.52f + 0.28f * t,
                    0.92f + 0.08f * t,
                    0.70f + 0.30f * t
                ));
                draw_list->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), col, S(2.5f));
            }

            ImGui::Dummy(ImVec2(0, S(8.0f)));
            const ImVec4 title_col(0.96f, 0.97f, 1.0f, 1.0f);
            center_text("Drop Audio Files or Folders Here", &title_col);
            ImGui::Spacing();
            center_text_disabled("Drag and drop music directly from File Explorer, or browse below");
            center_text_disabled("Supports MP3, FLAC, WAV, OGG, Opus, AAC, M4A, AIFF, and more");

            ImGui::Dummy(ImVec2(0, S(16.0f)));

            const float btn_w1 = S(165.0f);
            const float btn_w2 = S(145.0f);
            const float btn_spacing = S(14.0f);
            const float total_btn_w = btn_w1 + btn_spacing + btn_w2;
            if (card_avail > total_btn_w) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (card_avail - total_btn_w) * 0.5f);
            }

            ImGui::BeginDisabled(busy);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.86f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.56f, 0.96f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.40f, 0.74f, 1.00f));
            if (ImGui::Button("Browse Songs...", ImVec2(btn_w1, S(34.0f)))) {
                add_files(app, pick(window, false));
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0, btn_spacing);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.24f, 0.30f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.32f, 0.40f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.20f, 0.25f, 1.00f));
            if (ImGui::Button("Add Folder...", ImVec2(btn_w2, S(34.0f)))) {
                add_files(app, pick(window, true));
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();

            ImGui::Dummy(ImVec2(0, S(16.0f)));
            center_text_disabled("Metadata and embedded album covers are detected automatically.");
            center_text_disabled("You can customize playlists, track order, and artwork once tracks are added.");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        if (!app.status.empty()) {
            ImGui::Spacing();
            const float status_w = ImGui::CalcTextSize(app.status.c_str()).x;
            if (avail.x > status_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - status_w) * 0.5f);
            ImGui::TextColored(app.status_error ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f) : ImVec4(0.40f, 0.85f, 0.50f, 1.0f),
                               "%s", app.status.c_str());
        }
    } else {
        std::vector<std::vector<std::string>> problems(app.rows.size());
        std::size_t blocked = 0;
        for (std::size_t i = 0; i < app.rows.size(); ++i) {
            problems[i] = row_problems(app, i);
            if (!problems[i].empty()) ++blocked;
        }

        // Playlist filter tabs (when multiple playlists exist or custom playlists created)
        if (unique_playlists.size() > 1 || !app.custom_playlists.empty()) {
            if (ImGui::BeginTabBar("playlist_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
                const std::string all_label = "All (" + std::to_string(app.rows.size()) + ")###tab_all";
                ImGuiTabItemFlags all_flags = 0;
                if (app.select_playlist_tab && app.select_playlist_tab->empty()) {
                    all_flags |= ImGuiTabItemFlags_SetSelected;
                }
                if (ImGui::BeginTabItem(all_label.c_str(), nullptr, all_flags)) {
                    app.active_playlist_filter.clear();
                    ImGui::EndTabItem();
                }

                for (const auto& pl : unique_playlists) {
                    const auto count = playlist_stats[pl].first;
                    const std::string tab_label = pl + " (" + std::to_string(count) + ")###tab_" + pl;
                    ImGuiTabItemFlags tab_flags = 0;
                    if (app.select_playlist_tab && *app.select_playlist_tab == pl) {
                        tab_flags |= ImGuiTabItemFlags_SetSelected;
                    }
                    bool tab_open = true;
                    bool* p_open = (pl != defaultPlaylistName && count == 0) ? &tab_open : nullptr;
                    if (ImGui::BeginTabItem(tab_label.c_str(), p_open, tab_flags)) {
                        app.active_playlist_filter = pl;
                        ImGui::EndTabItem();
                    }
                    if (p_open && !tab_open) {
                        std::erase(app.custom_playlists, pl);
                        if (app.active_playlist_filter == pl) app.active_playlist_filter.clear();
                    }
                }

                if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
                    app.show_new_playlist_modal = true;
                    app.new_playlist_input.fill(0);
                    app.new_playlist_row_target = -1;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new playlist");

                ImGui::EndTabBar();
                app.select_playlist_tab.reset();
            }
            ImGui::Spacing();
        }

        const float footer = ImGui::GetFrameHeightWithSpacing() * 3.5f + S(16.0f);
        std::optional<std::pair<std::size_t, std::size_t>> up_target, down_target;
        std::optional<std::size_t> remove;
        if (ImGui::BeginTable("songs", 7,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0, ImGui::GetContentRegionAvail().y - footer))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, S(32));
            ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Playlist (?)", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, S(64));
            ImGui::TableSetupColumn("Problems", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, S(175));

            const int columns_count = ImGui::TableGetColumnCount();
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int column_n = 0; column_n < columns_count; column_n++) {
                if (!ImGui::TableSetColumnIndex(column_n)) continue;
                const char* name = ImGui::TableGetColumnName(column_n);
                ImGui::PushID(column_n);
                ImGui::TableHeader(name);
                if (column_n == 3 && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Playlist for this song.\nLeave blank to use the default playlist, or select/type a custom name to group songs into separate playlists.");
                }
                ImGui::PopID();
            }

            std::size_t visible_count = 0;
            static int active_context_row = -1;
            for (std::size_t i = 0; i < app.rows.size(); ++i) {
                auto& row = app.rows[i];
                const std::string row_pl = row.playlist[0] ? row.playlist.data() : defaultPlaylistName;
                if (!app.active_playlist_filter.empty() && row_pl != app.active_playlist_filter) {
                    continue;
                }
                ++visible_count;

                // Find prev index matching current filter
                std::optional<std::size_t> prev_matching;
                for (std::size_t k = i; k > 0; --k) {
                    const std::size_t check_idx = k - 1;
                    const std::string check_pl = app.rows[check_idx].playlist[0] ? app.rows[check_idx].playlist.data() : defaultPlaylistName;
                    if (app.active_playlist_filter.empty() || check_pl == app.active_playlist_filter) {
                        prev_matching = check_idx;
                        break;
                    }
                }
                // Find next index matching current filter
                std::optional<std::size_t> next_matching;
                for (std::size_t check_idx = i + 1; check_idx < app.rows.size(); ++check_idx) {
                    const std::string check_pl = app.rows[check_idx].playlist[0] ? app.rows[check_idx].playlist.data() : defaultPlaylistName;
                    if (app.active_playlist_filter.empty() || check_pl == app.active_playlist_filter) {
                        next_matching = check_idx;
                        break;
                    }
                }

                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();

                // # column with full-row selectable context target
                ImGui::TableNextColumn();
                char row_label[32];
                std::snprintf(row_label, sizeof(row_label), "%zu", i + 1);
                ImGui::Selectable(row_label, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(Right-click row for options)", narrow(row.file.wstring()).c_str());

                if (active_context_row == static_cast<int>(i)) {
                    ImGui::OpenPopup("row_context");
                    active_context_row = -1;
                }

                // Row context menu (from right-click on row or '...' button)
                if (ImGui::BeginPopupContextItem("row_context")) {
                    ImGui::TextDisabled("%s - %s", row.artist.data(), row.title.data());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Re-add Source Audio...", nullptr, false, !busy)) {
                        readd_audio(app, i, window);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Choose the original audio file for this track so the mod can be rebuilt.");
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Assign to Playlist")) {
                        for (const auto& pl_name : unique_playlists) {
                            const bool is_curr = (row.playlist[0] ? row.playlist.data() == pl_name : pl_name == defaultPlaylistName);
                            if (ImGui::MenuItem((pl_name + (pl_name == defaultPlaylistName ? " (default)" : "")).c_str(), nullptr, is_curr)) {
                                if (pl_name == defaultPlaylistName) {
                                    row.playlist.fill(0);
                                } else {
                                    copy_text(row.playlist, pl_name);
                                }
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("+ New Playlist...")) {
                            app.new_playlist_row_target = static_cast<int>(i);
                            app.new_playlist_input.fill(0);
                            app.show_new_playlist_modal = true;
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("Artwork")) {
                        if (ImGui::MenuItem("Choose Image...")) {
                            const auto files = pick(window, false, true);
                            if (!files.empty()) {
                                row.artwork = files[0];
                            }
                        }
                        if (!row.artwork.empty() && ImGui::MenuItem("Use Embedded / Automatic Artwork")) {
                            row.artwork.clear();
                        }
                        if (ImGui::MenuItem("Manage All Artwork...")) {
                            app.show_artwork_modal = true;
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Move Up", nullptr, false, prev_matching.has_value() && !busy)) {
                        if (prev_matching) up_target = std::make_pair(i, *prev_matching);
                    }
                    if (ImGui::MenuItem("Move Down", nullptr, false, next_matching.has_value() && !busy)) {
                        if (next_matching) down_target = std::make_pair(i, *next_matching);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Remove Song", nullptr, false, !busy)) {
                        remove = i;
                    }
                    ImGui::EndPopup();
                }

                // Artist column
                ImGui::BeginDisabled(busy);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##artist", row.artist.data(), row.artist.size());

                // Title column
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##title", row.title.data(), row.title.size());

                // Playlist column
                ImGui::TableNextColumn();
                const float arrow_w = ImGui::GetFrameHeight();
                const float spacing = style.ItemSpacing.x;
                ImGui::SetNextItemWidth(std::max(S(30.0f), ImGui::GetContentRegionAvail().x - arrow_w - spacing));
                ImGui::InputTextWithHint("##playlist", app.playlist[0] ? app.playlist.data() : "Default", row.playlist.data(), row.playlist.size());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Playlist for this track.\nLeave blank to inherit \"%s\", or select/type a custom name.", app.playlist[0] ? app.playlist.data() : "Default");
                }
                ImGui::SameLine(0, spacing);
                if (ImGui::ArrowButton("##pl_arrow", ImGuiDir_Down)) {
                    ImGui::OpenPopup("PlaylistCellMenu");
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose from existing playlists or create new");
                if (ImGui::BeginPopup("PlaylistCellMenu")) {
                    ImGui::TextDisabled("Assign to Playlist");
                    ImGui::Separator();
                    for (const auto& pl_name : unique_playlists) {
                        const bool is_curr = (row.playlist[0] ? row.playlist.data() == pl_name : pl_name == defaultPlaylistName);
                        if (ImGui::MenuItem((pl_name + (pl_name == defaultPlaylistName ? " (default)" : "")).c_str(), nullptr, is_curr)) {
                            if (pl_name == defaultPlaylistName) {
                                row.playlist.fill(0);
                            } else {
                                copy_text(row.playlist, pl_name);
                            }
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("+ New Playlist...")) {
                        app.new_playlist_row_target = static_cast<int>(i);
                        app.new_playlist_input.fill(0);
                        app.show_new_playlist_modal = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::EndDisabled();

                // Length column
                ImGui::TableNextColumn();
                if (row.scanned) ImGui::Text("%d:%02d", static_cast<int>(row.seconds) / 60, static_cast<int>(row.seconds) % 60);
                else ImGui::TextDisabled("...");

                // Problems column
                ImGui::TableNextColumn();
                if (!problems[i].empty()) {
                    std::string text;
                    for (const auto& p : problems[i]) text += (text.empty() ? "" : "; ") + p;
                    ImGui::TextColored(ImVec4(1, 0.55f, 0.35f, 1), "%s", text.c_str());
                }

                // Actions column
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(busy);
                ImGui::BeginDisabled(!prev_matching.has_value());
                if (ImGui::ArrowButton("up", ImGuiDir_Up)) up_target = std::make_pair(i, *prev_matching);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move song up");
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!next_matching.has_value());
                if (ImGui::ArrowButton("down", ImGuiDir_Down)) down_target = std::make_pair(i, *next_matching);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move song down");
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.48f, 0.16f, 0.16f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.22f, 0.22f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.15f, 0.15f, 1.00f));
                if (ImGui::Button("Remove")) remove = i;
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this song");

                ImGui::SameLine();
                if (ImGui::Button("...##more_actions")) {
                    active_context_row = static_cast<int>(i);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("More options (right-click track)");
                ImGui::EndDisabled();

                ImGui::PopID();
            }

            if (visible_count == 0 && !app.active_playlist_filter.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::Dummy(ImVec2(0, S(8)));
                ImGui::TextDisabled("No songs in \"%s\" yet.", app.active_playlist_filter.c_str());
                ImGui::TextDisabled("Drag audio files here or click 'Add songs...' below to add tracks to this playlist.");
                ImGui::Dummy(ImVec2(0, S(8)));
            }

            if (ImGui::BeginPopupContextWindow("table_empty_context", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
                if (ImGui::MenuItem("Add Songs...")) add_files(app, pick(window, false));
                if (ImGui::MenuItem("Add Folder...")) add_files(app, pick(window, true));
                ImGui::Separator();
                if (ImGui::MenuItem("+ New Playlist...")) {
                    app.new_playlist_row_target = -1;
                    app.new_playlist_input.fill(0);
                    app.show_new_playlist_modal = true;
                }
                if (ImGui::MenuItem("Manage Artwork...")) {
                    app.show_artwork_modal = true;
                }
                ImGui::EndPopup();
            }

            ImGui::EndTable();
        }
        if (up_target) std::swap(app.rows[up_target->first], app.rows[up_target->second]);
        if (down_target) std::swap(app.rows[down_target->first], app.rows[down_target->second]);
        if (remove) app.rows.erase(app.rows.begin() + static_cast<std::ptrdiff_t>(*remove));

        ImGui::Spacing();
        // Footer: add songs/folder, summary of count and duration, build / export
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Add songs...", ImVec2(S(105), S(26)))) add_files(app, pick(window, false));
        if (!app.active_playlist_filter.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add songs directly into \"%s\"", app.active_playlist_filter.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Add folder...", ImVec2(S(105), S(26)))) add_files(app, pick(window, true));
        if (!app.active_playlist_filter.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add folder tracks directly into \"%s\"", app.active_playlist_filter.c_str());
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        double total_seconds = 0.0;
        bool all_scanned = true;
        for (const auto& r : app.rows) {
            if (r.scanned) total_seconds += r.seconds;
            else all_scanned = false;
        }
        std::string summary;
        if (unique_playlists.size() > 1) {
            summary = std::to_string(unique_playlists.size()) + " playlists | ";
        }
        summary += std::to_string(app.rows.size()) + " song" + (app.rows.size() == 1 ? "" : "s");
        if (!all_scanned) {
            summary += " (scanning tags...)";
        } else {
            const int total_sec = static_cast<int>(total_seconds);
            const int hours = total_sec / 3600;
            const int mins = (total_sec % 3600) / 60;
            const int secs = total_sec % 60;
            char dur_buf[64];
            if (hours > 0) {
                std::snprintf(dur_buf, sizeof(dur_buf), " (%d:%02d:%02d)", hours, mins, secs);
            } else {
                std::snprintf(dur_buf, sizeof(dur_buf), " (%d:%02d)", mins, secs);
            }
            summary += dur_buf;
        }
        const float summary_w = ImGui::CalcTextSize(summary.c_str()).x;
        const float rem_w = ImGui::GetContentRegionAvail().x;
        if (rem_w > summary_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rem_w - summary_w);
        }
        ImGui::TextDisabled("%s", summary.c_str());
        if (unique_playlists.size() > 1 && ImGui::IsItemHovered()) {
            if (ImGui::BeginTooltip()) {
                ImGui::TextUnformatted("Playlist Breakdown");
                ImGui::Separator();
                for (const auto& pl : unique_playlists) {
                    const auto& stats = playlist_stats[pl];
                    const int sec = static_cast<int>(stats.second);
                    const int h = sec / 3600;
                    const int m = (sec % 3600) / 60;
                    const int s = sec % 60;
                    char buf[64];
                    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
                    else std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
                    ImGui::BulletText("%s: %zu song%s (%s)", pl.c_str(), stats.first, stats.first == 1 ? "" : "s", buf);
                }
                ImGui::EndTooltip();
            }
        }

        std::string block;
        if (app.thunderstore_read_only) block = "Imported Thunderstore packages are read-only; source audio is not included.";
        else if (!music::usable_name(app.name.data())) block = "Give the mod a name.";
        else if (!music::usable_name(app.playlist.data())) block = "Give the playlist a name.";
        else if (blocked) block = std::to_string(blocked) + " song(s) need fixing.";
        else if (std::any_of(app.rows.begin(), app.rows.end(), [](const Row& r) { return !r.scanned; })) block = "Reading the songs...";

        if (busy) {
            std::lock_guard lock(app.mutex);
            ImGui::ProgressBar(app.progress, ImVec2(S(-120), S(28)), (app.job + ": " + app.progress_text).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(-1, S(28)))) app.cancel = true;
        } else {
            if (!app.status.empty())
                ImGui::TextColored(app.status_error ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f) : ImVec4(0.40f, 0.85f, 0.50f, 1.0f), "%s", app.status.c_str());
            else if (!block.empty()) ImGui::TextDisabled("%s", block.c_str());
            else if (game_running()) ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f), "skate. is running: the mod takes effect the next time it starts.");
            else ImGui::TextUnformatted("");

            const float button_width = block.empty() ? (ImGui::GetContentRegionAvail().x - S(10)) * 0.65f : ImGui::GetContentRegionAvail().x;
            ImGui::BeginDisabled(!block.empty());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.78f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.54f, 0.90f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.38f, 0.68f, 1.00f));
            if (ImGui::Button(("Build into Mods\\" + narrow(output_folder(app).filename().wstring())).c_str(), ImVec2(button_width, S(32)))) {
                set_status(app, "");
                std::set<std::string> names;
                if (app.playlist[0]) names.insert(app.playlist.data());
                for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
                bool any_automatic = false;
                for (const auto& name : names) {
                    bool hasImage = app.playlist_artwork.contains(name) && !app.playlist_artwork[name].empty();
                    bool hasGen = app.generated_playlist_artwork.contains(name);
                    if (!hasImage && !hasGen) {
                        any_automatic = true;
                        break;
                    }
                }
                if (any_automatic) {
                    app.show_playlist_artwork_prompt = true;
                } else {
                    build(app);
                }
            }
            ImGui::PopStyleColor(3);

            if (block.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Export Thunderstore...", ImVec2(-1, S(32)))) {
                    app.show_export_ts = true;
                    if (!app.ts_description[0]) {
                        copy_text(app.ts_description, "Adds " + std::to_string(app.rows.size()) + " song(s) to skate.");
                    }
                }
            }
            ImGui::EndDisabled();
        }
    }

    if (app.show_playlist_artwork_prompt) {
        ImGui::OpenPopup("Playlist Artwork Setup");
        app.show_playlist_artwork_prompt = false;
        std::set<std::string> names;
        if (app.playlist[0]) names.insert(app.playlist.data());
        for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
        const auto pName = names.empty() ? "Playlist" : *names.begin();
        request_playlist_preview(app, pName);
    }
    if (ImGui::BeginPopupModal("Playlist Artwork Setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::set<std::string> names;
        if (app.playlist[0]) names.insert(app.playlist.data());
        for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
        std::vector<std::string> unconfigured;
        for (const auto& name : names) {
            bool hasImage = app.playlist_artwork.contains(name) && !app.playlist_artwork[name].empty();
            bool hasGen = app.generated_playlist_artwork.contains(name);
            if (!hasImage && !hasGen) unconfigured.push_back(name);
        }

        ImGui::Spacing();
        ImGui::Text("Playlist artwork has not been selected.");
        ImGui::TextDisabled("skate. displays playlist covers in its in-game music menu.");
        ImGui::Spacing();

        static int selected_idx = 0;
        if (selected_idx >= static_cast<int>(unconfigured.size())) selected_idx = 0;

        const auto pName = unconfigured.empty()
            ? (app.playlist[0] ? std::string(app.playlist.data()) : "Playlist")
            : unconfigured[selected_idx];

        // Left side: Preview
        ImGui::BeginGroup();
        if (app.artwork_preview_loading) {
            ImGui::BeginChild("cover_preview_box", ImVec2(S(180), S(180)), true);
            ImGui::TextWrapped("Loading preview...");
            ImGui::EndChild();
        } else if (app.artwork_preview && app.artwork_preview_label == pName) {
            ImGui::Image(app.artwork_preview, ImVec2(S(180), S(180)));
        } else {
            ImGui::BeginChild("cover_preview_box", ImVec2(S(180), S(180)), true);
            ImGui::Spacing();
            ImGui::TextDisabled("No cover");
            if (!app.artwork_preview_error.empty()) {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(160));
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", app.artwork_preview_error.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
        }
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // Right side: controls
        ImGui::BeginGroup();
        const bool artwork_busy = busy || app.artwork_preview_loading;
        if (unconfigured.size() > 1) {
            std::vector<const char*> pl_ptrs;
            for (const auto& name : unconfigured) pl_ptrs.push_back(name.c_str());
            ImGui::BeginDisabled(artwork_busy);
            if (ImGui::Combo("Playlist", &selected_idx, pl_ptrs.data(), static_cast<int>(pl_ptrs.size()))) {
                const auto& curName = unconfigured[selected_idx];
                request_playlist_preview(app, curName);
            }
            ImGui::EndDisabled();
        } else {
            ImGui::Text("Playlist: %s", pName.c_str());
        }
        ImGui::Spacing();

        if (!app.playlist_artwork[pName].empty()) {
            ImGui::Text("Active: %s", narrow(app.playlist_artwork[pName].filename().wstring()).c_str());
        } else if (app.generated_playlist_artwork.contains(pName)) {
            ImGui::TextUnformatted("Active: Generated text cover");
        } else {
            ImGui::TextUnformatted("Active: Automatic (from first track)");
        }
        ImGui::Spacing();

        ImGui::BeginDisabled(artwork_busy);
        if (ImGui::Button("Generate Text Cover", ImVec2(S(200), S(28)))) {
            app.generated_playlist_artwork.insert(pName);
            app.playlist_artwork[pName].clear();
            request_playlist_preview(app, pName);
        }

        if (ImGui::Button("Choose Image...", ImVec2(S(200), S(28)))) {
            const auto files = pick(window, false, true);
            if (!files.empty()) {
                app.playlist_artwork[pName] = files[0];
                app.generated_playlist_artwork.erase(pName);
                request_playlist_preview(app, pName);
            }
        }

        if (ImGui::Button("Use Automatic", ImVec2(S(200), S(28)))) {
            app.playlist_artwork[pName].clear();
            app.generated_playlist_artwork.erase(pName);
            request_playlist_preview(app, pName);
        }

        if (unconfigured.size() > 1) {
            ImGui::Spacing();
            if (ImGui::Button("Generate Text for All", ImVec2(S(200), S(28)))) {
                for (const auto& name : unconfigured) {
                    app.generated_playlist_artwork.insert(name);
                    app.playlist_artwork[name].clear();
                }
                request_playlist_preview(app, pName);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginDisabled(artwork_busy);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.78f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.54f, 0.90f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.38f, 0.68f, 1.00f));
        if (ImGui::Button("Build", ImVec2(S(120), S(30)))) {
            build(app);
            clear_artwork_preview(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(100), S(30)))) {
            clear_artwork_preview(app);
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (app.show_export_ts) {
        ImGui::OpenPopup("Export Thunderstore Package");
        app.show_export_ts = false;
    }
    if (ImGui::BeginPopupModal("Export Thunderstore Package", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Spacing();
        ImGui::Text("Export a Thunderstore-compatible .zip package ready for upload.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(S(320));
        ImGui::InputText("Author / Namespace", app.ts_author.data(), app.ts_author.size());
        ImGui::SetNextItemWidth(S(320));
        ImGui::InputText("Version", app.ts_version.data(), app.ts_version.size());
        ImGui::SetNextItemWidth(S(320));
        ImGui::InputText("Description", app.ts_description.data(), app.ts_description.size());
        ImGui::Spacing();
        if (!app.ts_icon.empty()) {
            ImGui::Text("Icon: %s", narrow(app.ts_icon.filename().wstring()).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(app.ts_icon.wstring()).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Clear##ts_icon")) app.ts_icon.clear();
        } else {
            ImGui::TextDisabled("Icon: Default (auto-generated 256x256)");
            ImGui::SameLine();
            if (ImGui::Button("Browse...##ts_icon")) {
                const auto picked = pick(window, false, true);
                if (!picked.empty()) app.ts_icon = picked[0];
            }
        }
        ImGui::Spacing();

        const auto mod = output_folder(app);
        const auto defaultFolder = mod.parent_path();
        const auto effectiveFolder = !app.ts_output_folder.empty() ? app.ts_output_folder : defaultFolder;

        ImGui::Text("Output folder: %s", narrow(effectiveFolder.filename().wstring().empty() ? effectiveFolder.wstring() : effectiveFolder.filename().wstring()).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(effectiveFolder.wstring()).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Browse...##ts_out")) {
            const auto folders = pick(window, true);
            if (!folders.empty()) app.ts_output_folder = folders[0];
        }
        if (!app.ts_output_folder.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Reset##ts_out")) app.ts_output_folder.clear();
        }

        const auto authorStr = app.ts_author[0] ? app.ts_author.data() : "Author";
        const auto versionStr = app.ts_version[0] ? app.ts_version.data() : "1.0.0";
        const auto modNameStr = app.name[0] ? app.name.data() : "ReSkateMusic";
        const auto expectedZipName = std::string(authorStr) + "-" + modNameStr + "-" + versionStr + ".zip";
        ImGui::TextDisabled("Package: %s", expectedZipName.c_str());

        ImGui::Spacing();
        ImGui::Checkbox("Show in File Explorer when finished", &app.ts_open_explorer);
        ImGui::Checkbox("Include \"Packaged with ReSkate Music Packer\" link in README", &app.ts_readme_credit);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.78f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.54f, 0.90f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.38f, 0.68f, 1.00f));
        if (ImGui::Button("Export ZIP", ImVec2(S(140), S(30)))) {
            try {
                if (!fs::exists(mod / L"layout.toc")) {
                    set_status(app, "Build the mod first before exporting.", true);
                } else {
                    music::ThunderstoreOptions opts;
                    opts.author = app.ts_author.data();
                    opts.version = app.ts_version.data();
                    opts.description = app.ts_description.data();
                    opts.icon = app.ts_icon;
                    opts.output = effectiveFolder;
                    opts.readme_credit = app.ts_readme_credit;
                    const auto zip = music::export_thunderstore(mod, opts);
                    set_status(app, "Exported: " + narrow(zip.wstring()));
                    if (app.ts_open_explorer) {
                        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(zip.c_str());
                        if (pidl) {
                            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                            ILFree(pidl);
                        } else {
                            ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                (L"/select,\"" + zip.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                        }
                    }
                }
            } catch (const std::exception& error) {
                set_status(app, error.what(), true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(100), S(30)))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void frame(App& app, HWND window) {
    apply_scans(app);
    autosave_project(app);
    apply_artwork_preview(app);
    {
        std::optional<FfmpegInstallResult> install;
        {
            std::lock_guard lock(app.mutex);
            install = std::move(app.ffmpeg_install);
            app.ffmpeg_install.reset();
        }
        if (install) {
            if (install->ok && find_ffmpeg(install->folder)) {
                app.settings.ffmpeg = install->folder;
                save_settings(app.settings);
                app.ffmpeg = true;
                set_status(app, "ffmpeg is ready.");
            } else if (install->ok) {
                set_status(app, "The ffmpeg download finished, but ffmpeg.exe and ffprobe.exe were not found.", true);
            } else {
                set_status(app, install->error.empty() ? "The ffmpeg download failed." : install->error, true);
            }
        }
    }
    {
        std::lock_guard lock(app.mutex);
        if (app.finished) {
            set_status(app, app.finished->second, !app.finished->first);
            if (app.finished->first) refresh_external_songs(app);
            app.finished.reset();
        }
    }
    {
        std::vector<fs::path> dropped;
        {
            std::lock_guard lock(app.dropped_mutex);
            dropped.swap(app.dropped);
        }
        if (!dropped.empty() && game_folder(app.settings.game) && app.ffmpeg) {
            if (dropped.size() == 1 && (dropped[0].extension() == L".zip" ||
                (fs::is_directory(dropped[0]) && fs::exists(dropped[0] / L"reskate-music.json"))))
                open_path(app, dropped[0]);
            else add_files(app, dropped);
        }
    }
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("music", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (!game_folder(app.settings.game)) setup_page(app, window);
    else if (!app.ffmpeg) ffmpeg_page(app, window);
    else songs_page(app, window);
    if (!game_folder(app.settings.game) || !app.ffmpeg)
        if (!app.status.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", app.status.c_str());
    ImGui::End();
    settings_modal(app, window);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    switch (message) {
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (g_app) {
            std::lock_guard lock(g_app->dropped_mutex);
            for (UINT i = 0; i < count; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                g_app->dropped.emplace_back(path);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int run_cli(int argc, wchar_t** argv) {
    music::PackOptions options;
    std::vector<fs::path> positional;
    bool thunderstore = false;
    bool get_ffmpeg = false;
    fs::path ffmpeg_dir;
    std::map<std::string, fs::path> trackArtwork;
    std::string author = "Author", version = "1.0.0";
    bool readme_credit = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            std::printf("ReSkate Music Packer (CLI mode)\n\n"
                        "Usage:\n"
                        "  ReSkateMusicPacker.exe <game folder> <song folder> [output folder] [options]\n\n"
                        "Options:\n"
                        "  --name <mod name>          Display name of the mod (default: folder name)\n"
                        "  --playlist <playlist>      Playlist name shown in-game (default: folder name)\n"
                        "  --bitrate <kbps>           Opus bitrate 64-320 kbps (default: 192)\n"
                        "  --no-normalize             Disable EBU R128 loudness normalization\n"
                        "  --thunderstore             Export a Thunderstore-ready zip package\n"
                        "  --author <name>            Thunderstore package author (default: Author)\n"
                        "  --version <x.y.z>          Thunderstore package version (default: 1.0.0)\n"
                        "  --no-readme-credit         Omit packer credit link from the README\n"
                        "  --playlist-artwork <name> <image>  Cover for a playlist\n"
                        "  --track-artwork <id> <image>       Cover for Artist - Title\n"
                        "  --generate-playlist-artwork <name> Text cover for a playlist\n"
                        "  --get-ffmpeg [folder]      Download ffmpeg into a folder (default: next to this exe or\n"
                        "                             %%LOCALAPPDATA%%) and print where it went\n"
                        "  --gui                      Launch graphical user interface\n"
                        "  --help, -h                 Show this help text\n");
            return 0;
        }
        else if (arg == L"--name" && i + 1 < argc) options.name = narrow(argv[++i]);
        else if (arg == L"--bitrate" && i + 1 < argc) options.bitrate = std::stoi(argv[++i]);
        else if (arg == L"--playlist" && i + 1 < argc) options.playlist = narrow(argv[++i]);
        else if (arg == L"--no-normalize") options.normalize = false;
        else if (arg == L"--thunderstore") thunderstore = true;
        else if (arg == L"--author" && i + 1 < argc) author = narrow(argv[++i]);
        else if (arg == L"--version" && i + 1 < argc) version = narrow(argv[++i]);
        else if (arg == L"--no-readme-credit") readme_credit = false;
        else if (arg == L"--playlist-artwork" && i + 2 < argc) {
            const auto name = narrow(argv[++i]);
            options.playlist_artwork[name] = argv[++i];
        }
        else if (arg == L"--track-artwork" && i + 2 < argc) {
            const auto id = narrow(argv[++i]);
            trackArtwork[id] = argv[++i];
        }
        else if (arg == L"--generate-playlist-artwork" && i + 1 < argc)
            options.generated_playlist_artwork.insert(narrow(argv[++i]));
        else if (arg == L"--get-ffmpeg") {
            get_ffmpeg = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-' && argv[i + 1][0] != L'\0') ffmpeg_dir = argv[++i];
        }
        else if (arg == L"--gui") {}
        else positional.emplace_back(arg);
    }
    if (get_ffmpeg) {
        try {
            const fs::path directory = ffmpeg_dir.empty() ? music::default_install_dir() : ffmpeg_dir;
            std::printf("Downloading ffmpeg into %s ...\n", narrow(directory.wstring()).c_str());
            music::ensure_ffmpeg(directory, music::ffmpeg_url(), music::ffmpeg_sha256(),
                [](const music::DownloadProgress& step) {
                    if (step.total)
                        std::printf("\r  %llu / %llu MB", static_cast<unsigned long long>(step.received >> 20),
                                    static_cast<unsigned long long>(step.total >> 20));
                    else
                        std::printf("\r  %llu MB", static_cast<unsigned long long>(step.received >> 20));
                    std::fflush(stdout);
                });
            std::printf("\nffmpeg -> %s\n", narrow(directory.wstring()).c_str());
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "error: %s\n", error.what());
            return 2;
        }
    }
    if (positional.size() < 2 || positional.size() > 3 || options.bitrate < 64 || options.bitrate > 320) {
        std::fprintf(stderr, "Usage: ReSkateMusicPacker.exe <game folder> <song folder> [output folder] [options]\n"
                             "Run 'ReSkateMusicPacker.exe --help' for details, or run with no arguments for GUI.\n");
        return 1;
    }
    const auto& songFolder = positional[1];
    if (options.playlist.empty()) options.playlist = narrow(fs::path(songFolder).filename().wstring());
    if (!music::usable_name(options.playlist)) {
        std::fprintf(stderr, "error: the playlist name is empty, too long, or has control characters\n");
        return 1;
    }
    options.game = positional[0];
    options.output = positional.size() > 2 ? positional[2] : songFolder.parent_path() / (songFolder.filename().wstring() + L"_mod");
    try {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(songFolder)) {
            if (!entry.is_regular_file()) continue;
            static const std::set<std::string> imageExtensions{".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".avif", ".tif", ".tiff", ".ico"};
            if (imageExtensions.contains(lower(narrow(entry.path().extension().wstring())))) continue;
            const auto matchesImage = [&](const auto& item) {
                std::error_code error;
                return fs::equivalent(entry.path(), item.second, error);
            };
            if (std::any_of(trackArtwork.begin(), trackArtwork.end(), matchesImage) ||
                std::any_of(options.playlist_artwork.begin(), options.playlist_artwork.end(), matchesImage)) continue;
            files.push_back(entry.path());
        }
        std::ranges::sort(files);
        if (files.empty()) throw std::runtime_error("No songs in " + narrow(songFolder.wstring()));

        auto songs = music::scan(files);
        for (const auto& [id, image] : trackArtwork) {
            const auto found = std::find_if(songs.begin(), songs.end(), [&](const auto& song) {
                return song.artist + " - " + song.title == id;
            });
            if (found == songs.end()) throw std::runtime_error("Artwork names an unknown track: " + id);
            found->artwork = image;
        }
        for (const auto& [name, image] : options.playlist_artwork) {
            if (name != options.playlist) throw std::runtime_error("Artwork names an unknown playlist: " + name);
        }
        for (const auto& name : options.generated_playlist_artwork)
            if (name != options.playlist) throw std::runtime_error("Generated artwork names an unknown playlist: " + name);
        const auto result = music::pack(options, songs, [&](const music::Progress& step) {
            const std::string stage = step.stage;
            if (stage == "encoding") std::printf("%s - %s\n", songs[step.song].artist.c_str(), songs[step.song].title.c_str());
            else if (stage == "encoded") std::printf("  %s\n", step.detail.c_str());
        });
        std::printf("%zu song(s), %zu KB of audio -> %s\n", result.songs, result.audioKb, narrow(result.output.wstring()).c_str());
        if (thunderstore) {
            const auto zip = music::export_thunderstore(result.output, {
                .author = author,
                .version = version,
                .readme_credit = readme_credit
            });
            std::printf("Thunderstore package -> %s\n", narrow(zip.wstring()).c_str());
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}

} // namespace



int run_gui(HINSTANCE instance, int /*cmd_show*/) {

    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_scale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    const auto work_width = static_cast<float>(work.right - work.left);
    const auto work_height = static_cast<float>(work.bottom - work.top);
    if (work_width > 0 && work_height > 0)
        g_scale = std::clamp(std::min(work_width * 0.98f / window_width, work_height * 0.96f / window_height),
                             0.62f, g_scale);

    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = L"ReSkateMusicMaker";
    RegisterClassExW(&type);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const int width = static_cast<int>(S(window_width));
    const int height = static_cast<int>(S(window_height));
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, style, FALSE);
    const int win_w = rect.right - rect.left;
    const int win_h = rect.bottom - rect.top;
    const int win_x = work.left + (static_cast<int>(work_width) - win_w) / 2;
    const int win_y = work.top + (static_cast<int>(work_height) - win_h) / 2;
    const auto window = CreateWindowExW(0, type.lpszClassName, L"ReSkate Music Packer", style, win_x, win_y,
                                        win_w, win_h, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    const auto font = fs::path(windows) / L"Fonts" / L"segoeui.ttf";
    if (fs::exists(font)) io.Fonts->AddFontFromFileTTF(narrow(font.wstring()).c_str(), S(18.0f));
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(g_scale);
    ImGui_ImplWin32_Init(window);
    Renderer renderer;
    if (!renderer.init(window)) {
        MessageBoxW(nullptr, L"This PC's graphics driver cannot draw the window.", L"ReSkate Music Packer", MB_ICONERROR);
        return 1;
    }

    auto app_storage = std::make_unique<App>();
    auto& app = *app_storage;
    app.renderer = &renderer;
    g_app = &app;
    app.settings = load_settings();
    app.ffmpeg = find_ffmpeg(app.settings.ffmpeg);
    refresh_external_songs(app);
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOWNORMAL);

    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(window)) { Sleep(50); continue; }
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        frame(app, window);
        ImGui::Render();
        renderer.render();
    }
    app.cancel = true;
    if (app.worker.joinable()) app.worker.join();
    if (app.artwork_preview) renderer.release_texture(app.artwork_preview);
    g_app = nullptr;
    renderer.shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(window);
    if (com) CoUninitialize();
    return 0;
}


int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int cmd_show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool force_gui = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--gui") force_gui = true;
    }
    if (argc > 1 && !force_gui) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            std::ios::sync_with_stdio();
        }
        const int result = run_cli(argc, argv);
        LocalFree(argv);
        return result;
    }
    LocalFree(argv);
    return run_gui(instance, cmd_show);
}
