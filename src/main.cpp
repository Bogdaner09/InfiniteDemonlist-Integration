#include "Api.hpp"
#include "ListLayer.hpp"
#include "Thumbnails.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelCell.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/utils/web.hpp>

using namespace geode::prelude;

namespace {

constexpr float kRankScale = .4f;
// goldFont's digits, in points at scale 1: their box runs from the label's
// bottom edge up this far (83 of the font's 104-pixel line, at 4x).
constexpr float kGoldDigitHeight = 83.f / 4.f;

// GD's goldFont (and bigFont) draw "NO" where "#" should be, so the "#" is
// drawn by hand to match: gold strokes with a dark outline.
CCNode* hashSign(float height) {
    float const width = height * .85f;
    float const stroke = height * .085f;
    float const outline = stroke + .5f;
    float const lean = width * .14f;

    std::array<std::pair<CCPoint, CCPoint>, 4> const strokes = {{
        { { width * .32f - lean, 0.f }, { width * .32f + lean, height } },
        { { width * .70f - lean, 0.f }, { width * .70f + lean, height } },
        { { 0.f, height * .34f }, { width, height * .34f } },
        { { 0.f, height * .68f }, { width, height * .68f } },
    }};

    auto node = CCDrawNode::create();
    // All the outlines first, so no outline crosses over another stroke's gold.
    for (auto const& [from, to] : strokes) node->drawSegment(from, to, outline, { .16f, .08f, 0.f, 1.f });
    for (auto const& [from, to] : strokes) node->drawSegment(from, to, stroke, { 1.f, .8f, .2f, 1.f });
    node->setContentSize({ width, height });
    return node;
}

// The list placement for a cell's bottom-right corner, with its right edge at
// `right` and its bottom at `bottom`. goldFont's outline keeps it readable
// over bright thumbnails, where the thin, faded chatFont disappeared.
CCNode* rankLabel(idl::Level const& level, float right, float bottom) {
    auto node = CCNode::create();

    // Upcoming (unverified) levels aren't ranked yet.
    auto text = CCLabelBMFont::create(
        level.verified ? fmt::format("{} IDL", level.rank).c_str() : "Upcoming", "goldFont.fnt"
    );
    text->setScale(kRankScale);
    text->setAnchorPoint({ 1.f, 0.f });
    text->setPosition({ right, bottom });
    node->addChild(text);

    if (level.verified) {
        float const digitHeight = kGoldDigitHeight * kRankScale;
        auto hash = hashSign(digitHeight * .78f);
        auto const hashSize = hash->getContentSize();
        hash->setPosition({
            right - text->getScaledContentSize().width - hashSize.width - 1.5f,
            bottom + (digitHeight - hashSize.height) / 2.f,
        });
        node->addChild(hash);
    }
    return node;
}

}

// The demon button on the right side of the online search screen.
class $modify(IDLSearchLayer, LevelSearchLayer) {
    bool init(int type) {
        if (!LevelSearchLayer::init(type)) return false;

        auto sprite = CircleButtonSprite::createWithSpriteFrameName(
            "diffIcon_10_btn_001.png", 1.f, CircleBaseColor::Green, CircleBaseSize::Small
        );
        // The horns make the face sit high when centered by its bounds;
        // nudge it down so the face itself is centered in the circle.
        if (auto face = sprite->getTopNode()) face->setPositionY(face->getPositionY() - 1.5f);
        auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(IDLSearchLayer::onIDL));
        button->setID("idl-button"_spr);

        if (auto menu = this->getChildByID("other-filter-menu")) {
            menu->addChild(button);
            menu->updateLayout();
        }
        else {
            // Some other mod moved things around; fall back to our own spot.
            auto const winSize = CCDirector::get()->getWinSize();
            auto fallback = CCMenu::create();
            fallback->setPosition({ winSize.width - 30.f, 30.f });
            fallback->setID("idl-menu"_spr);
            fallback->addChild(button);
            this->addChild(fallback);
        }
        return true;
    }

    void onIDL(CCObject*) {
        CCDirector::get()->pushScene(CCTransitionFade::create(.5f, IDLListLayer::scene()));
    }
};

class $modify(IDLLevelCell, LevelCell) {
    void loadFromLevel(GJGameLevel* level) {
        LevelCell::loadFromLevel(level);
        if (!level) return;

        auto tag = typeinfo_cast<CCInteger*>(level->getUserObject("entry"_spr));
        if (!tag) return;
        auto entry = idl::levelAt(static_cast<size_t>(tag->getValue()));
        if (!entry) return;

        // Cells can be reloaded; don't stack our nodes.
        if (m_backgroundLayer) {
            if (auto old = m_backgroundLayer->getChildByID("thumbnail"_spr)) old->removeFromParent();
        }
        if (auto old = m_mainLayer->getChildByID("rank-label"_spr)) old->removeFromParent();
        if (auto old = m_mainLayer->getChildByID("placeholder"_spr)) old->removeFromParent();

        if (level->getUserObject("placeholder"_spr)) this->drawPlaceholder(*entry);

        if (m_backgroundLayer) {
            idl::thumbnails::attach(
                m_backgroundLayer, this, { m_width, m_height }, idl::thumbnails::sourceFor(*entry)
            );
        }
        {
            auto rank = rankLabel(*entry, m_width - 5.f, 3.f);
            rank->setID("rank-label"_spr);
            m_mainLayer->addChild(rank, 10);

            // Integrated Demonlist puts its AREDL rank in the same corner. Its
            // hook may run after ours, so check on the next frame.
            WeakRef<CCLayer> weakMain = m_mainLayer;
            queueInMainThread([weakMain] {
                auto main = weakMain.lock();
                if (!main) return;
                if (auto other = main->getChildByID("hiimjustin000.integrated_demonlist/level-rank-label")) {
                    other->setVisible(false);
                }
            });
        }
    }

    // Levels that aren't on the GD servers (unreleased, or still being
    // verified) can't be opened, so the cell shows their status and video.
    void drawPlaceholder(idl::Level const& entry) {
        // Hide rather than remove: LevelCell keeps pointers to its nodes.
        for (auto child : CCArrayExt<CCNode*>(m_mainLayer->getChildren())) {
            child->setVisible(false);
            if (auto menu = typeinfo_cast<CCMenu*>(child)) menu->setEnabled(false);
        }

        float const h = m_height;
        auto root = CCNode::create();
        root->setContentSize({ m_width, h });
        root->setID("placeholder"_spr);
        m_mainLayer->addChild(root, 5);

        auto face = CCSprite::createWithSpriteFrameName("difficulty_10_btn_001.png");
        face->setScale(std::min(1.f, h / 90.f) * .9f);
        face->setPosition({ 27.f, h / 2.f + 6.f });
        root->addChild(face);

        auto name = CCLabelBMFont::create(entry.name.c_str(), "bigFont.fnt");
        name->setAnchorPoint({ 0.f, .5f });
        name->limitLabelWidth(200.f, .8f, .1f);
        name->setPosition({ 53.f, h * .73f });
        root->addChild(name);

        auto author = CCLabelBMFont::create(fmt::format("By {}", entry.author()).c_str(), "goldFont.fnt");
        author->setAnchorPoint({ 0.f, .5f });
        author->limitLabelWidth(200.f, .65f, .1f);
        author->setPosition({ 53.f, h * .48f });
        root->addChild(author);

        std::string status;
        ccColor3B color { 200, 200, 200 };
        if (entry.gdLevelId > 0) {
            status = "Not found on the servers";
            color = { 255, 120, 120 };
        }
        else if (entry.progress) {
            status = entry.verifier.empty()
                ? fmt::format("Unverified - best {}%", *entry.progress)
                : fmt::format("Unverified - {}% by {}", *entry.progress, entry.verifier);
            color = { 255, 190, 90 };
        }
        else if (!entry.verifier.empty()) {
            status = fmt::format("Unreleased - verified by {}", entry.verifier);
        }
        else {
            status = "Unreleased";
        }
        auto statusLabel = CCLabelBMFont::create(status.c_str(), "bigFont.fnt");
        statusLabel->setAnchorPoint({ 0.f, .5f });
        statusLabel->limitLabelWidth(200.f, .4f, .1f);
        statusLabel->setColor(color);
        statusLabel->setPosition({ 53.f, h * .22f });
        root->addChild(statusLabel);

        if (!entry.videoId.empty()) {
            auto menu = CCMenu::create();
            menu->setPosition({ 0.f, 0.f });
            menu->setContentSize({ m_width, h });
            root->addChild(menu);

            auto sprite = ButtonSprite::create("Video", 0, false, "bigFont.fnt", "GJ_button_01.png", 30.f, .6f);
            auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(IDLLevelCell::onIDLVideo));
            button->setUserObject(CCString::create(entry.youtubeUrl()));
            button->setPosition({ m_width - 50.f, h / 2.f });
            button->setID("video-button"_spr);
            menu->addChild(button);
        }
    }

    void onIDLVideo(CCObject* sender) {
        auto node = typeinfo_cast<CCNode*>(sender);
        if (!node) return;
        auto url = typeinfo_cast<CCString*>(node->getUserObject());
        if (url) web::openLinkInBrowser(url->getCString());
    }
};
