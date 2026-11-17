#pragma once

#include "Api.hpp"

#include <Geode/Geode.hpp>

#include <string>
#include <vector>

namespace idl::thumbnails {

// Where a level's thumbnail comes from, in the list's order of preference:
// a self-hosted image (thumbnail_path), then a YouTube video kept just for its
// thumbnail (thumbnail_video_id), then the verification video (video_id).
struct Source {
    std::string key;                   // cache key and file name; empty = no thumbnail
    std::string previewUrl;            // small and quick, shown while the full one loads
    std::vector<std::string> fullUrls; // tried in order; the first that exists wins
};

Source sourceFor(Level const& level);

// Gets a thumbnail. The callback runs on the main thread, once with the
// preview and again with the full image when that arrives. It isn't called
// if nothing could be loaded.
void get(Source const& source, geode::Function<void(cocos2d::CCTexture2D*)> callback);

// Adds the thumbnail to the right side of a list cell. `parent` should be a
// node drawn behind the cell's content (its background layer); `cell` is the
// cell itself.
void attach(cocos2d::CCNode* parent, cocos2d::CCNode* cell, cocos2d::CCSize const& size, Source const& source);

}
