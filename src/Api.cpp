#include "Api.hpp"

#include <Geode/utils/web.hpp>

#include <memory>
#include <thread>
#include <unordered_map>

using namespace geode::prelude;

namespace idl {

namespace {

std::vector<Level> s_levels;
bool s_loaded = false;

// Callbacks waiting on a detail request already in flight, by list id.
std::unordered_map<int, std::vector<Function<void()>>> s_pendingDetails;

std::string stringField(matjson::Value const& obj, std::string_view key) {
    auto value = obj.get(key);
    if (!value || !value.unwrap().isString()) return "";
    return value.unwrap().asString().unwrapOr("");
}

std::optional<int> intField(matjson::Value const& obj, std::string_view key) {
    auto value = obj.get(key);
    if (!value) return std::nullopt;
    auto const& v = value.unwrap();
    if (v.isNumber()) return static_cast<int>(v.asInt().unwrapOr(0));
    // gd_level_id comes back as a string
    if (v.isString()) {
        auto parsed = numFromString<int>(v.asString().unwrapOr(""));
        if (parsed) return parsed.unwrap();
    }
    return std::nullopt;
}

Level parseLevel(matjson::Value const& obj) {
    Level level;
    level.id = intField(obj, "id").value_or(0);
    level.slug = stringField(obj, "slug");
    level.placement = intField(obj, "placement").value_or(0);
    if (auto verifiedAt = obj.get("verified_at")) level.verified = !verifiedAt.unwrap().isNull();
    level.name = stringField(obj, "name");
    level.publisher = stringField(obj, "publisher");
    level.verifier = stringField(obj, "verifier");
    level.victor = stringField(obj, "victor");
    level.videoId = stringField(obj, "video_id");
    // Older versions of the API named these thumbnail_id and thumbnail_link.
    level.thumbnailVideoId = stringField(obj, "thumbnail_video_id");
    if (level.thumbnailVideoId.empty()) level.thumbnailVideoId = stringField(obj, "thumbnail_id");
    level.thumbnailPath = stringField(obj, "thumbnail_path");
    if (level.thumbnailPath.empty()) level.thumbnailPath = stringField(obj, "thumbnail_link");
    level.progress = intField(obj, "progress");
    level.length = intField(obj, "level_length").value_or(0);
    if (auto creators = obj.get("creators"); creators && creators.unwrap().isArray()) {
        for (auto const& creator : creators.unwrap()) {
            if (creator.isString()) level.creators.push_back(creator.asString().unwrapOr(""));
        }
    }
    return level;
}

// GD level ids the list's API doesn't have yet, by slug. Only used while the
// API's gd_level_id is null, so a fix on the list's side takes over.
int missingGdLevelId(std::string_view slug) {
    if (slug == "fiberglass") return 152421515;
    return 0;
}

// A failed lookup isn't remembered, so the next page load asks again.
void finishDetail(int listId, std::optional<int> gdLevelId) {
    for (auto& level : s_levels) {
        if (level.id == listId && gdLevelId) {
            level.detailLoaded = true;
            level.gdLevelId = *gdLevelId > 0 ? *gdLevelId : missingGdLevelId(level.slug);
        }
    }
    auto node = s_pendingDetails.extract(listId);
    if (node.empty()) return;
    for (auto& callback : node.mapped()) callback();
}

}

std::string Level::author() const {
    if (!publisher.empty()) return publisher;
    if (!creators.empty()) return creators.front();
    return verifier;
}

std::string Level::youtubeUrl() const {
    return fmt::format("https://www.youtube.com/watch?v={}", videoId);
}

void httpGet(std::string url, Function<void(HttpResult)> callback) {
    // Blocking request on a thread of its own. Geode's async::spawn would do
    // the same, but instantiating it has crashed MSVC on this toolchain.
    std::thread([url = std::move(url), callback = std::move(callback)]() mutable {
        auto response = web::WebRequest()
            .userAgent(fmt::format("IDLIntegration/{} (Geode)", Mod::get()->getVersion().toNonVString()))
            .timeout(std::chrono::seconds(20))
            .getSync(url);

        HttpResult result;
        result.code = response.code();
        result.ok = response.ok();
        result.body = std::move(response).data();

        queueInMainThread([callback = std::move(callback), result = std::move(result)]() mutable {
            callback(std::move(result));
        });
    }).detach();
}

std::vector<Level>& levels() {
    return s_levels;
}

bool levelsLoaded() {
    return s_loaded;
}

Level* levelAt(size_t index) {
    return index < s_levels.size() ? &s_levels[index] : nullptr;
}

void loadLevels(Function<void(std::optional<std::string>)> callback) {
    httpGet(fmt::format("{}/api/levels", kSite), [callback = std::move(callback)](HttpResult result) mutable {
        if (!result.ok) {
            callback(result.code > 0
                ? fmt::format("The list's server answered with error {}.", result.code)
                : std::string("Couldn't reach infinitedemonlist.com."));
            return;
        }
        auto json = matjson::parse(result.text());
        if (!json || !json.unwrap().isArray()) {
            callback(std::string("The list's server sent something unexpected."));
            return;
        }

        // Keep the GD ids we already looked up, so a refresh doesn't redo them.
        std::unordered_map<int, int> known;
        for (auto const& level : s_levels) {
            if (level.detailLoaded) known[level.id] = level.gdLevelId;
        }

        std::vector<Level> parsed;
        for (auto const& obj : json.unwrap()) {
            auto level = parseLevel(obj);
            if (auto it = known.find(level.id); it != known.end()) {
                level.detailLoaded = true;
                level.gdLevelId = it->second;
            }
            parsed.push_back(std::move(level));
        }
        std::stable_sort(parsed.begin(), parsed.end(), [](Level const& a, Level const& b) {
            return a.placement < b.placement;
        });
        // The API's placements count upcoming (unverified) levels too; the
        // list's real ranking skips them.
        int rank = 0;
        for (auto& level : parsed) level.rank = level.verified ? ++rank : 0;

        s_levels = std::move(parsed);
        s_loaded = true;
        callback(std::nullopt);
    });
}

void loadDetails(std::vector<size_t> indices, Function<void()> callback) {
    // Shared countdown; the last answer to come back fires the callback.
    struct Waiter {
        size_t remaining = 0;
        Function<void()> done;
    };
    auto waiter = std::make_shared<Waiter>();
    waiter->done = std::move(callback);

    std::vector<int> toRequest;
    for (auto index : indices) {
        auto level = levelAt(index);
        if (!level || level->detailLoaded) continue;
        waiter->remaining++;
        auto& queue = s_pendingDetails[level->id];
        bool const first = queue.empty();
        queue.push_back([waiter] {
            if (--waiter->remaining == 0 && waiter->done) waiter->done();
        });
        if (first) toRequest.push_back(level->id);
    }

    if (waiter->remaining == 0) {
        waiter->done();
        return;
    }

    for (auto listId : toRequest) {
        httpGet(fmt::format("{}/api/levels/{}", kSite, listId), [listId](HttpResult result) {
            std::optional<int> gdLevelId;
            if (result.ok) {
                if (auto json = matjson::parse(result.text())) {
                    if (auto level = json.unwrap().get("level")) {
                        // a null gd_level_id means the level isn't released
                        gdLevelId = intField(level.unwrap(), "gd_level_id").value_or(0);
                    }
                }
            }
            else {
                log::warn("Couldn't load IDL level {} (HTTP {})", listId, result.code);
            }
            finishDetail(listId, gdLevelId);
        });
    }
}

}
