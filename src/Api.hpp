#pragma once

#include <Geode/Geode.hpp>

#include <optional>
#include <string>
#include <vector>

namespace idl {

// One entry of https://infinitedemonlist.com/api/levels
struct Level {
    int id = 0;                  // the list's own id, used by /api/levels/{id}
    std::string slug;            // e.g. "fiberglass"
    int placement = 0;           // the API's own placement, upcoming levels included
    bool verified = false;       // has a verified_at; upcoming levels don't
    int rank = 0;                // placement among verified levels only; 0 if upcoming
    std::string name;
    std::string publisher;       // may be empty for unreleased levels
    std::vector<std::string> creators;
    std::string verifier;
    std::string victor;
    std::string videoId;
    std::string thumbnailVideoId; // a YouTube video used only for its thumbnail
    std::string thumbnailPath;    // a self-hosted image, site-relative, e.g. "/kom.jpg"
    std::optional<int> progress; // verification progress of an unverified level
    int length = 0;              // seconds

    // Filled in from /api/levels/{id}; the list endpoint doesn't carry it.
    bool detailLoaded = false;
    int gdLevelId = 0;           // 0 = not on the GD servers (unreleased)

    std::string author() const;
    std::string youtubeUrl() const;
};

struct HttpResult {
    bool ok = false;
    int code = 0;
    geode::ByteVector body;

    std::string text() const { return std::string(body.begin(), body.end()); }
};

// Runs a GET on a worker thread and calls back on the main thread.
void httpGet(std::string url, geode::Function<void(HttpResult)> callback);

// The whole list, cached for the session once it has loaded.
std::vector<Level>& levels();
bool levelsLoaded();
Level* levelAt(size_t index);

// Loads (or reloads) the list. The callback gets an error message, or
// nothing on success.
void loadLevels(geode::Function<void(std::optional<std::string>)> callback);

// Makes sure the given entries know their GD level id. Calls back once all
// of them have answered (failures leave gdLevelId at 0).
void loadDetails(std::vector<size_t> indices, geode::Function<void()> callback);

constexpr char const* kSite = "https://infinitedemonlist.com";

}
