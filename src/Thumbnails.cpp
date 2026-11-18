#include "Thumbnails.hpp"

#include <Geode/utils/web.hpp>

#include <array>
#include <filesystem>
#include <list>
#include <thread>
#include <unordered_map>

using namespace geode::prelude;

namespace idl::thumbnails {

namespace {

struct Entry {
    Ref<CCTexture2D> texture;  // the best one loaded so far
    bool loading = false;
    bool done = false;         // nothing better is coming
    // Called on every new texture until the entry is done.
    std::vector<Function<void(CCTexture2D*)>> listeners;
};

std::unordered_map<std::string, Entry> s_entries;

// Most recently used last. A 1280x720 thumbnail is ~3.7 MB of video memory,
// so only the newest few stay in memory; the disk cache covers the rest.
std::list<std::string> s_recent;
constexpr size_t kMaxCachedTextures = 30;

constexpr char const* kCacheFolder = "thumbnail-cache";
// Folders earlier versions cached into, cleared on the first load.
constexpr std::array kOldCacheFolders = { "thumbnails", "thumbnails-hd" };

// Fades a thumbnail in this long after it arrives.
constexpr float kFadeTime = .3f;
// How far the slanted left edge leans, in points.
constexpr float kSlant = 26.f;
// Gap between the thumbnail and its cell's top and bottom edges, in points.
// 0.5 each side leaves a 1-point divider between neighbors, about as wide as
// GD's own line; 1.5 read as a thick band.
constexpr float kEdgeInset = .5f;
// Marks the picture sprites inside a thumbnail, so a sharper one can
// replace a blurrier one.
constexpr int kPictureTag = 1;

std::optional<ByteVector> download(std::string const& url) {
    auto response = web::WebRequest()
        .userAgent("IDLIntegration (Geode)")
        .timeout(std::chrono::seconds(20))
        .getSync(url);
    if (!response.ok()) return std::nullopt;
    auto data = std::move(response).data();
    if (data.empty()) return std::nullopt;
    return data;
}

std::optional<ByteVector> readCache(std::filesystem::path const& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return std::nullopt;
    auto read = file::readBinary(path);
    if (!read || read.unwrap().empty()) return std::nullopt;
    return std::move(read).unwrap();
}

void writeCache(std::filesystem::path const& path, ByteVector const& data) {
    (void)file::createDirectoryAll(path.parent_path());
    if (auto written = file::writeBinary(path, data); !written) {
        log::warn("Couldn't cache thumbnail {}: {}", path.filename().string(), written.unwrapErr());
    }
}

// File names come from API data, so keep them to a safe alphabet.
std::string safeName(std::string const& text) {
    std::string name;
    for (char c : text) {
        name += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    }
    return name;
}

CCTexture2D* decode(ByteVector& data) {
    auto image = new CCImage();
    bool const ok = image->initWithImageData(data.data(), static_cast<int>(data.size()));
    CCTexture2D* texture = nullptr;
    if (ok) {
        texture = new CCTexture2D();
        if (texture->initWithImage(image)) {
            texture->autorelease();
        }
        else {
            texture->release();
            texture = nullptr;
        }
    }
    image->release();
    return texture;
}

void touch(std::string const& key) {
    s_recent.remove(key);
    s_recent.push_back(key);
    // Drop the oldest finished ones; sprites still showing them keep their
    // own reference to the texture.
    for (auto it = s_recent.begin(); s_recent.size() > kMaxCachedTextures && it != s_recent.end();) {
        auto entry = s_entries.find(*it);
        if (entry != s_entries.end() && entry->second.loading) {
            ++it;
            continue;
        }
        if (entry != s_entries.end()) s_entries.erase(entry);
        it = s_recent.erase(it);
    }
}

// Runs on the main thread for each image the loader thread finds.
void deliver(std::string const& key, std::optional<ByteVector> data, bool last, std::filesystem::path cacheFile) {
    auto found = s_entries.find(key);
    if (found == s_entries.end()) return;
    auto& entry = found->second;

    if (data) {
        if (auto texture = decode(*data)) {
            entry.texture = texture;
            for (auto& listener : entry.listeners) listener(texture);
        }
        else if (!cacheFile.empty()) {
            // A broken cache file; drop it so the next load downloads again.
            std::error_code error;
            std::filesystem::remove(cacheFile, error);
        }
    }

    if (last) {
        entry.loading = false;
        entry.done = true;
        entry.listeners.clear();
        if (entry.texture) touch(key);
        else log::debug("No thumbnail for {}", key);
    }
}

void load(Source source, std::filesystem::path saveDir, bool clearOldCache) {
    std::error_code error;
    if (clearOldCache) {
        for (auto folder : kOldCacheFolders) std::filesystem::remove_all(saveDir / folder, error);
    }

    auto const fullFile = saveDir / kCacheFolder / (source.key + ".jpg");
    auto const previewFile = saveDir / kCacheFolder / (source.key + "-preview.jpg");
    auto post = [&](std::optional<ByteVector> data, bool last, std::filesystem::path cacheFile = {}) {
        queueInMainThread([key = source.key, data = std::move(data), last, cacheFile = std::move(cacheFile)]() mutable {
            deliver(key, std::move(data), last, std::move(cacheFile));
        });
    };

    // Already have the full one: nothing else to do.
    if (auto cached = readCache(fullFile)) {
        post(std::move(cached), true, fullFile);
        return;
    }

    // The preview first, so the cell isn't empty while the big one loads.
    if (!source.previewUrl.empty()) {
        if (auto cached = readCache(previewFile)) {
            post(std::move(cached), false, previewFile);
        }
        else if (auto preview = download(source.previewUrl)) {
            writeCache(previewFile, *preview);
            post(std::move(preview), false);
        }
    }

    // A size the video doesn't have answers 404, so try them in turn.
    std::optional<ByteVector> full;
    for (auto const& url : source.fullUrls) {
        full = download(url);
        if (full) break;
    }
    if (full) {
        writeCache(fullFile, *full);
        std::filesystem::remove(previewFile, error);
    }
    post(std::move(full), true);
}

// The 4:3 YouTube sizes (sddefault, hqdefault) carry black bars above and
// below a 16:9 picture; this is the picture without them.
CCRect pictureRect(CCTexture2D* texture) {
    auto const full = texture->getContentSize();
    CCRect rect { 0.f, 0.f, full.width, full.height };
    if (full.height > 0 && full.width / full.height < 1.5f) {
        rect.size.height = full.width * 9.f / 16.f;
        rect.origin.y = (full.height - rect.size.height) / 2.f;
    }
    return rect;
}

// The Level Thumbnails mod draws its own picture on every level cell; in
// this list the YouTube one replaces it.
void hideOtherThumbnails(CCNode* node, int depth = 0) {
    if (!node || depth > 2) return;
    for (auto child : CCArrayExt<CCNode*>(node->getChildren())) {
        if (std::string_view(child->getID()).starts_with("cdc.level_thumbnails/")) {
            child->setVisible(false);
            continue;
        }
        hideOtherThumbnails(child, depth + 1);
    }
}

}

Source sourceFor(Level const& level) {
    Source source;
    if (!level.thumbnailPath.empty()) {
        // Self-hosted on the list's site; one image, no smaller version.
        source.key = "site-" + safeName(level.thumbnailPath);
        source.fullUrls.push_back(
            level.thumbnailPath.starts_with("http")
                ? level.thumbnailPath
                : fmt::format("{}{}{}", kSite, level.thumbnailPath.starts_with("/") ? "" : "/", level.thumbnailPath)
        );
        return source;
    }

    auto const& videoId = level.thumbnailVideoId.empty() ? level.videoId : level.thumbnailVideoId;
    if (videoId.empty()) return source;

    source.key = "yt-" + safeName(videoId);
    // mqdefault (320x180) exists for every video and loads fast.
    source.previewUrl = fmt::format("https://i.ytimg.com/vi/{}/mqdefault.jpg", videoId);
    // maxresdefault is 1280x720 but not every video has it; sddefault
    // (640x480) covers most of the ones that don't.
    source.fullUrls = {
        fmt::format("https://i.ytimg.com/vi/{}/maxresdefault.jpg", videoId),
        fmt::format("https://i.ytimg.com/vi/{}/sddefault.jpg", videoId),
    };
    return source;
}

void get(Source const& source, Function<void(CCTexture2D*)> callback) {
    if (source.key.empty()) return;

    auto& entry = s_entries[source.key];
    if (entry.texture) {
        Ref<CCTexture2D> texture = entry.texture;
        touch(source.key);
        callback(texture);
    }
    // touch() may have dropped other entries, never this one: it's newest.
    auto& current = s_entries[source.key];
    if (current.done) return;

    // Hear about the sharper one when it lands.
    current.listeners.push_back(std::move(callback));
    if (current.loading) return;
    current.loading = true;

    static bool s_clearedOldCache = false;
    bool const clearOldCache = !s_clearedOldCache;
    s_clearedOldCache = true;

    std::thread(load, source, Mod::get()->getSaveDir(), clearOldCache).detach();
}

void attach(CCNode* parent, CCNode* cell, CCSize const& size, Source const& source) {
    if (!parent || !cell || size.width <= 0 || size.height <= 0 || source.key.empty()) return;

    float const width = size.width;
    float const height = size.height;
    // Kept clear of the cell's top and bottom edges, where LevelCell draws
    // its faint divider lines; edge to edge, neighboring thumbnails ran into
    // each other and the dividers vanished over dark pictures.
    float const bottom = kEdgeInset;
    float const top = height - kEdgeInset;
    float const innerHeight = top - bottom;
    float const thumbWidth = innerHeight * 16.f / 9.f;
    float const left = width - thumbWidth;

    // A parallelogram with a slanted left edge, like the cell's art is
    // sliding in from the right. The slant cuts into the picture's own left
    // edge rather than reaching past it, so the picture can be sized to the
    // cell's height and show top to bottom without being zoomed in.
    auto stencil = CCDrawNode::create();
    CCPoint corners[4] = {
        { left, bottom },
        { width, bottom },
        { width, top },
        { left + kSlant, top },
    };
    stencil->drawPolygon(corners, 4, { 1.f, 1.f, 1.f, 1.f }, 0.f, { 0.f, 0.f, 0.f, 0.f }, BorderAlignment::Outside);

    auto clip = CCClippingNode::create(stencil);
    clip->setContentSize(size);
    clip->setID("thumbnail"_spr);
    parent->addChild(clip);

    WeakRef<CCClippingNode> weakClip = clip;
    WeakRef<CCNode> weakCell = cell;
    get(source, [weakClip, weakCell, width, height, innerHeight, thumbWidth](CCTexture2D* texture) {
        auto clip = weakClip.lock();
        if (!clip || !texture) return;

        auto sprite = CCSprite::createWithTexture(texture, pictureRect(texture));
        auto const spriteSize = sprite->getContentSize();
        if (spriteSize.width <= 0 || spriteSize.height <= 0) return;
        // Full height for 16:9 pictures (all the YouTube ones); anything
        // narrower is widened just enough to reach the slanted edge.
        float const scale = std::max(thumbWidth / spriteSize.width, innerHeight / spriteSize.height);
        sprite->setScale(scale);
        sprite->setAnchorPoint({ 1.f, .5f });
        sprite->setPosition({ width, height / 2.f });
        sprite->setOpacity(0);
        sprite->runAction(CCFadeTo::create(kFadeTime, 255));

        // A sharper version replacing the preview: fade it in on top, then
        // drop the old one.
        bool const first = clip->getChildByTag(kPictureTag) == nullptr;
        for (auto old : CCArrayExt<CCNode*>(clip->getChildren())) {
            if (old->getTag() != kPictureTag) continue;
            old->setTag(-1);
            old->runAction(CCSequence::createWithTwoActions(
                CCDelayTime::create(kFadeTime),
                CallFuncExt::create([weakOld = WeakRef<CCNode>(old)] {
                    if (auto old = weakOld.lock()) old->removeFromParent();
                })
            ));
        }
        clip->addChild(sprite, 0, kPictureTag);
        if (!first) return;

        // The other mod loads its picture asynchronously too, so keep
        // checking for a few seconds after ours shows up.
        if (auto cell = weakCell.lock()) {
            hideOtherThumbnails(cell);
            clip->runAction(CCRepeat::create(
                CCSequence::createWithTwoActions(
                    CCDelayTime::create(.5f),
                    CallFuncExt::create([weakCell] {
                        if (auto cell = weakCell.lock()) hideOtherThumbnails(cell);
                    })
                ),
                8
            ));
        }
    });
}

}
