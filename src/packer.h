// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// ReSkateMusicPacker as a library: songs in, an add-only music mod out. packer.cpp says how.
// Needs ffmpeg and ffprobe on PATH. Not thread-safe: one scan or pack at a time (they share a
// scratch folder in %TEMP%).
#pragma once
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace music {

struct SongInfo {
    std::filesystem::path file;
    std::string artist, title;             // from the tags, else the "Artist - Title" file name
    std::string playlist;                  // custom playlist; if empty, uses PackOptions.playlist
    double seconds = 0;
    std::vector<std::string> problems;     // why the song may not pack; empty means fine
    std::filesystem::path artwork;        // optional image; packed as a square PNG
    bool has_embedded_artwork = false;    // detected by scan(); manual artwork overrides it
};

struct PackOptions {
    std::filesystem::path game, output;
    std::string name = "ReSkateMusic";     // the mod's name
    std::string playlist;                  // the songs' playlist in the music menu
    int bitrate = 192;                     // kbps, 64-320
    bool normalize = true;                 // EBU R128 loudness normalization (-16 LUFS) via ffmpeg loudnorm
    std::map<std::string, std::filesystem::path> playlist_artwork;
    std::set<std::string> generated_playlist_artwork; // playlist names; manual images take priority
};

// One step of pack(). `stage` is "encoding" (detail empty), "encoded" (detail: length, packets
// and size), "building" or "writing"; the last two are about the whole mod and have song == count.
struct Progress {
    std::size_t song, count;
    const char* stage;
    std::string detail;
};
using ProgressFn = std::function<void(const Progress&)>;

struct PackResult {
    std::size_t songs, audioKb;
    std::filesystem::path output;
};

// Thrown by pack() when `cancel` was set; nothing is left at the output folder.
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Cancelled") {}
};

// Whether text is fit for an artist, title or playlist name: 1-255 characters, no control characters.
bool usable_name(const std::string& text);

// Folds smart quotes and UTF-8/mojibake apostrophes onto their ASCII forms, so a tag cannot
// smuggle multi-byte sequences into an asset name or the song id.
std::string sanitize_text(const std::string& text);
// The asset-path slug of a song: ASCII letters and digits only, since it becomes an asset path.
// Smart quotes are folded first, and every other run of punctuation collapses to one underscore.
std::string slug_of(const std::string& artist, const std::string& title);
// Case-insensitive uniqueness for asset slugs: returns `base`, or base_2, base_3, ... until the
// lowered result is unused, and records it in `used`. The bundle stores asset names lowercased,
// so two titles differing only in case must not resolve to the same asset.
std::string unique_slug(const std::string& base, std::set<std::string>& used);

// Tags and length of each file, without encoding. Never throws for a bad file: that is a problem entry.
std::vector<SongInfo> scan(std::span<const std::filesystem::path> files);

// Extracts the first attached picture into the encode cache. Empty if none or unreadable.
std::filesystem::path embedded_artwork(const std::filesystem::path& track);
// Proportionally resized 512x512 PNG with dark padding; shared by packing and previews.
std::vector<std::byte> image_artwork_png(const std::filesystem::path& image);
// Decodes one source track to a temporary WAV for the GUI's in-app preview.
std::filesystem::path preview_audio(const std::filesystem::path& source);
// Deterministic 512x512 text cover, also used by the GUI preview. UTF-8 playlist name.
std::vector<std::byte> playlist_artwork_png(const std::string& name);

// Builds the mod into options.output (any files already there are overwritten). Throws
// std::runtime_error with a message fit to show, or Cancelled. `progress` and `cancel` may be null;
// cancel is checked between songs. Songs keep their given order; artist/title override the tags.
PackResult pack(const PackOptions& options, const std::vector<SongInfo>& songs, const ProgressFn& progress = {},
                const std::atomic<bool>* cancel = nullptr);

// What a mod was made from. pack() saves it in the mod as reskate-music-project.json (source files
// as absolute paths), so the mod can be reopened, changed and packed again into the same folder.
struct Project {
    std::string name, playlist;
    int bitrate = 192;
    bool normalize = true;
    std::vector<SongInfo> songs;   // file, artist and title; scan() the files for length and problems
    std::map<std::string, std::filesystem::path> playlist_artwork;
    std::set<std::string> generated_playlist_artwork;
};
// Throws std::runtime_error, with a message fit to show, when the folder has no project or a bad one.
Project load_project(const std::filesystem::path& mod);

struct ThunderstoreOptions {
    std::string author = "Author";
    std::string version = "1.0.0";
    std::string description;
    std::filesystem::path icon;     // optional custom icon.png; if empty, uses mod's icon.png or generates a default
    std::filesystem::path output;   // output .zip path; if empty, saves as <Author>-<Name>-<Version>.zip beside the mod
    bool readme_credit = true;      // include "Packaged with ReSkate Music Packer" link in auto-generated README.md
};

// Packages a built mod folder as a Thunderstore-compatible .zip.
std::filesystem::path export_thunderstore(const std::filesystem::path& modFolder, const ThunderstoreOptions& options = {});

} // namespace music
