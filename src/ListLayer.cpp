#include "ListLayer.hpp"
#include "Api.hpp"

#include <algorithm>

using namespace geode::prelude;

namespace {

constexpr int kPageSize = 10;
constexpr float kListWidth = 356.f;
constexpr float kListHeight = 220.f;
constexpr float kListViewHeight = kListHeight;

// The list type that makes CustomListView build LevelCells. In GD 2.2081 on
// Windows, getListCell and loadCell both send 5 to LevelCell; 4 (what the
// SDK's enum calls BoomListType::Level) builds an AchievementCell and crashes
// in AchievementCell::loadFromDict when handed a level.
constexpr BoomListType kLevelListType = static_cast<BoomListType>(5);

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// The double arrow for "last page": two page arrows overlapping.
CCNode* lastPageSprite() {
    auto node = CCNode::create();
    auto first = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    auto second = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    for (auto arrow : { first, second }) {
        arrow->setFlipX(true);
        arrow->setScale(.45f);
        node->addChild(arrow);
    }
    auto const size = first->getScaledContentSize();
    node->setContentSize({ size.width * 1.6f, size.height });
    first->setPosition({ size.width * .5f, size.height / 2.f });
    second->setPosition({ size.width * 1.1f, size.height / 2.f });
    return node;
}

// A square GD button with a sprite or label on top.
CCSprite* squareButton(char const* background, CCNode* top) {
    auto sprite = CCSprite::create(background);
    sprite->setScale(.75f);
    auto const size = sprite->getContentSize();
    top->setPosition(size / 2.f);
    sprite->addChild(top);
    return sprite;
}

GJGameLevel* makePlaceholder(idl::Level const& entry) {
    // Upcoming levels may not be on the GD servers, so the cell gets a level
    // object of our own; the cell hook draws it differently.
    auto level = GJGameLevel::create();
    level->m_levelName = entry.name;
    level->m_creatorName = entry.author();
    level->m_levelID = -entry.placement;
    level->m_levelType = GJLevelType::SearchResult;
    level->m_demon = 1;
    level->m_demonDifficulty = 6;
    return level;
}

}

IDLListLayer* IDLListLayer::create() {
    auto ret = new IDLListLayer();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

CCScene* IDLListLayer::scene() {
    auto scene = CCScene::create();
    scene->addChild(IDLListLayer::create());
    return scene;
}

IDLListLayer::~IDLListLayer() {
    this->untagLevels();
    this->detachFromLevelManager();
}

bool IDLListLayer::init() {
    if (!CCLayer::init()) return false;

    m_tagged = CCArray::create();
    this->setID("IDLListLayer");

    auto const winSize = CCDirector::get()->getWinSize();

    auto background = CCSprite::create("GJ_gradientBG.png");
    background->setAnchorPoint({ 0.f, 0.f });
    background->setScaleX((winSize.width + 10.f) / background->getContentWidth());
    background->setScaleY((winSize.height + 10.f) / background->getContentHeight());
    background->setPosition({ -5.f, -5.f });
    // The same blue as GD's own level browser.
    background->setColor({ 0, 102, 255 });
    background->setID("background");
    this->addChild(background, -2);

    addSideArt(this, SideArt::Bottom, SideArtStyle::Layer);

    // The list itself
    m_list = GJListLayer::create(
        CustomListView::create(CCArray::create(), kLevelListType, kListViewHeight, kListWidth),
        "Infinite Demonlist",
        { 191, 114, 62, 255 },
        kListWidth,
        kListHeight,
        0
    );
    m_list->setPosition({ (winSize.width - kListWidth) / 2.f, (winSize.height - kListHeight) / 2.f - 5.f });
    m_list->setID("list-layer");
    this->addChild(m_list, 2);

    // GJListLayer keeps no pointer to its title, so find it by its text.
    for (auto child : CCArrayExt<CCNode*>(m_list->getChildren())) {
        auto label = typeinfo_cast<CCLabelBMFont*>(child);
        if (label && std::string_view(label->getString()) == "Infinite Demonlist") {
            m_titleLabel = label;
            break;
        }
    }

    m_messageLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_messageLabel->setScale(.45f);
    m_messageLabel->setAlignment(kCCTextAlignmentCenter);
    m_messageLabel->setPosition({ kListWidth / 2.f, kListViewHeight / 2.f });
    m_messageLabel->setID("message-label"_spr);
    m_list->addChild(m_messageLabel, 10);

    m_spinner = LoadingSpinner::create(50.f);
    m_spinner->setPosition({ kListWidth / 2.f, kListViewHeight / 2.f });
    m_spinner->setVisible(false);
    m_spinner->setID("loading-spinner"_spr);
    m_list->addChild(m_spinner, 10);

    // Everything around the list
    auto menu = CCMenu::create();
    menu->setPosition({ 0.f, 0.f });
    menu->setID("button-menu");
    this->addChild(menu, 5);

    auto backButton = CCMenuItemSpriteExtra::create(
        CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"), this, menu_selector(IDLListLayer::onBack)
    );
    backButton->setPosition({ 25.f, winSize.height - 25.f });
    backButton->setID("back-button");

    // Below the back arrow, like in GD's Saved Levels: opens Find Level, or
    // clears the search while one is active.
    m_searchButton = CCMenuItemSpriteExtra::create(
        CCSprite::createWithSpriteFrameName("gj_findBtn_001.png"), this, menu_selector(IDLListLayer::onSearch)
    );
    m_searchButton->setPosition({ 24.f, winSize.height - 68.f });
    m_searchButton->setID("search-button");
    menu->addChild(m_searchButton);
    menu->addChild(backButton);

    auto prevSprite = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    m_prevButton = CCMenuItemSpriteExtra::create(prevSprite, this, menu_selector(IDLListLayer::onPrevPage));
    m_prevButton->setPosition({ 24.f, winSize.height / 2.f });
    m_prevButton->setID("prev-page-button");
    menu->addChild(m_prevButton);

    auto nextSprite = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    nextSprite->setFlipX(true);
    m_nextButton = CCMenuItemSpriteExtra::create(nextSprite, this, menu_selector(IDLListLayer::onNextPage));
    m_nextButton->setPosition({ winSize.width - 24.f, winSize.height / 2.f });
    m_nextButton->setID("next-page-button");
    menu->addChild(m_nextButton);

    m_countLabel = CCLabelBMFont::create("", "goldFont.fnt");
    m_countLabel->setAnchorPoint({ 1.f, 1.f });
    m_countLabel->setScale(.5f);
    m_countLabel->setPosition({ winSize.width - 7.f, winSize.height - 3.f });
    m_countLabel->setID("level-count-label");
    this->addChild(m_countLabel, 5);

    m_pageButtonLabel = CCLabelBMFont::create("1", "bigFont.fnt");
    m_pageButton = CCMenuItemSpriteExtra::create(
        squareButton("GJ_button_02.png", m_pageButtonLabel), this, menu_selector(IDLListLayer::onPickPage)
    );
    m_pageButton->setPosition({ winSize.width - 23.5f, winSize.height - 37.5f });
    m_pageButton->setID("page-button");
    menu->addChild(m_pageButton);

    // The N/A face with "Upcoming" under it switches to the levels you
    // can't play yet. Gray when off, full color when on, like the search
    // screen's difficulty filters.
    auto modeNode = CCNode::create();
    // diffIcon is the bare face; the difficulty_00_btn sprite has "NA"
    // drawn into it.
    m_modeSprite = CCSprite::createWithSpriteFrameName("diffIcon_00_btn_001.png");
    m_modeSprite->setScale(.95f);
    m_modeLabel = CCLabelBMFont::create("Upcoming", "bigFont.fnt");
    // Narrow enough to stay inside the screen's right edge.
    m_modeLabel->limitLabelWidth(44.f, .3f, .1f);
    auto const faceSize = m_modeSprite->getScaledContentSize();
    auto const labelSize = m_modeLabel->getScaledContentSize();
    float const nodeWidth = std::max(faceSize.width, labelSize.width);
    float const nodeHeight = faceSize.height + labelSize.height + 1.f;
    modeNode->setContentSize({ nodeWidth, nodeHeight });
    m_modeSprite->setPosition({ nodeWidth / 2.f, nodeHeight - faceSize.height / 2.f });
    m_modeLabel->setPosition({ nodeWidth / 2.f, labelSize.height / 2.f });
    modeNode->addChild(m_modeSprite);
    modeNode->addChild(m_modeLabel);
    m_modeButton = CCMenuItemSpriteExtra::create(modeNode, this, menu_selector(IDLListLayer::onToggleMode));
    m_modeButton->setPosition({ winSize.width - 26.f, winSize.height - 78.f });
    m_modeButton->setID("unverified-button");
    menu->addChild(m_modeButton);

    m_lastButton = CCMenuItemSpriteExtra::create(lastPageSprite(), this, menu_selector(IDLListLayer::onLastPage));
    m_lastButton->setPosition({ winSize.width - 23.5f, winSize.height - 122.f });
    m_lastButton->setID("last-page-button");
    menu->addChild(m_lastButton);

    auto refreshButton = CCMenuItemSpriteExtra::create(
        CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png"), this, menu_selector(IDLListLayer::onRefresh)
    );
    refreshButton->setPosition({ winSize.width - 26.f, 26.f });
    refreshButton->setID("refresh-button");
    menu->addChild(refreshButton);

    auto infoButton = InfoAlertButton::create(
        "Infinite Demonlist",
        "Levels from <cy>infinitedemonlist.com</c>, in list order.\n\n"
        "The <cg>search button</c> finds levels by name or publisher (press it again to clear the "
        "search), and the <cl>page button</c> "
        "jumps to any page.\n\n"
        "The <co>Upcoming</c> button switches to the levels that aren't verified or on the Geometry Dash "
        "servers yet, with who's verifying them and a button to their video.",
        1.f
    );
    infoButton->setPosition({ 25.f, 25.f });
    infoButton->setID("info-button");
    menu->addChild(infoButton);

    this->setKeypadEnabled(true);

    this->updateMode();
    this->updateControls();
    if (idl::levelsLoaded()) this->loadAllDetails();
    else this->fetchList();

    return true;
}

void IDLListLayer::keyBackClicked() {
    this->onBack(nullptr);
}

void IDLListLayer::onBack(CCObject*) {
    CCDirector::get()->popSceneWithTransition(.5f, PopTransition::kPopTransitionFade);
}

void IDLListLayer::onPrevPage(CCObject*) {
    this->goToPage(m_page - 1);
}

void IDLListLayer::onNextPage(CCObject*) {
    this->goToPage(m_page + 1);
}

void IDLListLayer::onLastPage(CCObject*) {
    this->goToPage(this->pageCount() - 1);
}

void IDLListLayer::onPickPage(CCObject*) {
    if (m_listLoading) return;
    auto popup = SetIDPopup::create(m_page + 1, 1, this->pageCount(), "Go to Page", "Go", true, 1, 60.f, false, false);
    popup->m_delegate = this;
    popup->show();
}

void IDLListLayer::onToggleMode(CCObject*) {
    if (m_listLoading) return;
    m_unverifiedMode = !m_unverifiedMode;
    this->updateMode();
    this->applySearch();
}

void IDLListLayer::updateMode() {
    auto const color = m_unverifiedMode ? ccColor3B { 255, 255, 255 } : ccColor3B { 125, 125, 125 };
    m_modeSprite->setColor(color);
    m_modeLabel->setColor(color);
    if (m_titleLabel) {
        m_titleLabel->setString(m_unverifiedMode ? "Upcoming Demons" : "Infinite Demonlist");
    }
}

void IDLListLayer::onRefresh(CCObject*) {
    if (m_listLoading) return;
    // Clears GD's cached search results for this page too, so percentages
    // and download counts come back fresh.
    if (!m_searchKey.empty()) {
        GameLevelManager::get()->m_storedLevels->removeObjectForKey(m_searchKey);
    }
    this->fetchList();
}

void IDLListLayer::onSearch(CCObject*) {
    // While a search is showing, the button is the gray one with the red X
    // and clears it, as in GD's Saved Levels.
    if (!m_query.empty()) {
        this->setQuery("");
        return;
    }
    // The popup GD's Saved Levels uses, set up the same way.
    auto popup = SetTextPopup::create("", "Search", 30, "Find Level", "OK", true, 60.f);
    popup->m_delegate = this;
    popup->show();
}

void IDLListLayer::setTextPopupClosed(SetTextPopup* popup, gd::string text) {
    if (popup && popup->m_cancelled) return;
    this->setQuery(std::string(text));
}

void IDLListLayer::setQuery(std::string query) {
    // Only spaces counts as no search.
    if (query.find_first_not_of(' ') == std::string::npos) query.clear();
    if (query == m_query) return;
    m_query = std::move(query);
    this->updateSearchButton();
    // Still loading the list: it filters by m_query once it's in.
    if (m_listLoading) return;
    this->applySearch();
}

void IDLListLayer::updateSearchButton() {
    m_searchButton->setSprite(CCSprite::createWithSpriteFrameName(
        m_query.empty() ? "gj_findBtn_001.png" : "gj_findBtnOff_001.png"
    ));
}

void IDLListLayer::setIDPopupClosed(SetIDPopup* popup, int value) {
    if (popup && popup->m_cancelled) return;
    this->goToPage(value - 1);
}

void IDLListLayer::fetchList() {
    m_listLoading = true;
    m_generation++;
    this->detachFromLevelManager();
    this->setListView(CCArray::create());
    this->showMessage("");
    this->setLoading(true);

    WeakRef<IDLListLayer> weak = this;
    idl::loadLevels([weak](std::optional<std::string> error) {
        auto self = weak.lock();
        if (!self) return;
        self->m_listLoading = false;
        if (error) {
            self->setLoading(false);
            self->showMessage(fmt::format("Couldn't load the list.\n{}", *error));
            self->updateControls();
            return;
        }
        self->loadAllDetails();
    });
}

// Which levels are playable is only known from each level's detail page, so
// sorting them into the two lists needs every detail first. Lookups are
// remembered for the session, so this is only slow the first time.
void IDLListLayer::loadAllDetails() {
    m_listLoading = true;
    m_generation++;
    this->setListView(CCArray::create());
    this->showMessage("");
    this->setLoading(true);

    std::vector<size_t> all(idl::levels().size());
    for (size_t i = 0; i < all.size(); i++) all[i] = i;

    WeakRef<IDLListLayer> weak = this;
    idl::loadDetails(std::move(all), [weak] {
        auto self = weak.lock();
        if (!self) return;
        self->m_listLoading = false;
        self->applySearch();
    });
}

void IDLListLayer::applySearch() {
    m_filtered.clear();
    auto const query = lowercase(m_query);
    auto const& levels = idl::levels();
    for (size_t i = 0; i < levels.size(); i++) {
        auto const& level = levels[i];
        // Upcoming: not verified yet (verified_at is null), even if it's
        // already on the GD servers, or not on the servers at all. A failed
        // lookup of a verified level stays in the main list, where the GD
        // search gets another try at it.
        bool const upcoming = !level.verified || (level.detailLoaded && level.gdLevelId <= 0);
        if (upcoming != m_unverifiedMode) continue;
        if (query.empty()
            || lowercase(level.name).find(query) != std::string::npos
            || lowercase(level.publisher).find(query) != std::string::npos) {
            m_filtered.push_back(i);
        }
    }
    m_page = 0;
    this->loadPage();
}

void IDLListLayer::goToPage(int page) {
    if (m_listLoading) return;
    page = std::clamp(page, 0, this->pageCount() - 1);
    if (page == m_page && !m_pageEntries.empty()) return;
    m_page = page;
    this->loadPage();
}

int IDLListLayer::pageCount() const {
    return std::max(1, static_cast<int>((m_filtered.size() + kPageSize - 1) / kPageSize));
}

void IDLListLayer::loadPage() {
    m_generation++;
    int const generation = m_generation;
    this->detachFromLevelManager();
    m_searchKey.clear();

    m_pageEntries.clear();
    size_t const first = static_cast<size_t>(m_page) * kPageSize;
    for (size_t i = first; i < std::min(m_filtered.size(), first + kPageSize); i++) {
        m_pageEntries.push_back(m_filtered[i]);
    }
    this->updateControls();
    this->setListView(CCArray::create());

    if (m_pageEntries.empty()) {
        this->setLoading(false);
        if (!m_query.empty()) this->showMessage(fmt::format("No demons match \"{}\".", m_query));
        else if (m_unverifiedMode) this->showMessage("Every level on the list\nis playable right now.");
        else this->showMessage("The list is empty.");
        return;
    }

    this->showMessage("");
    this->setLoading(true);

    // Upcoming levels that are already on the GD servers get real cells
    // too; requestGDLevels skips the ones that aren't.

    // Normally already known from loadAllDetails; retries any that failed.
    WeakRef<IDLListLayer> weak = this;
    idl::loadDetails(m_pageEntries, [weak, generation] {
        auto self = weak.lock();
        if (!self || self->m_generation != generation) return;
        self->requestGDLevels();
    });
}

void IDLListLayer::requestGDLevels() {
    std::string ids;
    for (auto index : m_pageEntries) {
        auto level = idl::levelAt(index);
        if (!level || level->gdLevelId <= 0) continue;
        if (!ids.empty()) ids += ',';
        ids += std::to_string(level->gdLevelId);
    }
    if (ids.empty()) {
        this->showPage(nullptr);
        return;
    }

    // Search type 19 looks up a comma-separated list of level ids.
    auto search = GJSearchObject::create(SearchType::Type19, ids);
    m_searchKey = search->getKey();

    auto glm = GameLevelManager::get();
    if (auto stored = glm->getStoredOnlineLevels(m_searchKey.c_str())) {
        this->showPage(stored);
        return;
    }
    glm->m_levelManagerDelegate = this;
    glm->getOnlineLevels(search);
}

void IDLListLayer::loadLevelsFinished(CCArray* levels, char const* key) {
    this->loadLevelsFinished(levels, key, -1);
}

void IDLListLayer::loadLevelsFinished(CCArray* levels, char const* key, int) {
    if (!key || m_searchKey != key) return;
    this->detachFromLevelManager();
    this->showPage(levels);
}

void IDLListLayer::loadLevelsFailed(char const* key) {
    this->loadLevelsFailed(key, -1);
}

void IDLListLayer::loadLevelsFailed(char const* key, int) {
    if (!key || m_searchKey != key) return;
    this->detachFromLevelManager();
    this->setLoading(false);
    this->setListView(CCArray::create());
    this->showMessage("Couldn't get these levels from\nthe Geometry Dash servers.\nTry the refresh button.");
}

void IDLListLayer::showPage(CCArray* gdLevels) {
    this->untagLevels();

    std::unordered_map<int, GJGameLevel*> byId;
    if (gdLevels) {
        for (auto level : CCArrayExt<GJGameLevel*>(gdLevels)) {
            byId[level->m_levelID.value()] = level;
        }
    }

    auto levels = CCArray::create();
    for (auto index : m_pageEntries) {
        auto entry = idl::levelAt(index);
        if (!entry) continue;

        GJGameLevel* level = nullptr;
        if (entry->gdLevelId > 0) {
            if (auto it = byId.find(entry->gdLevelId); it != byId.end()) level = it->second;
        }
        bool const placeholder = level == nullptr;
        if (placeholder) level = makePlaceholder(*entry);

        // The cell hook reads these to know it's drawing an IDL entry.
        level->setUserObject("entry"_spr, CCInteger::create(static_cast<int>(index)));
        level->setUserObject("placeholder"_spr, placeholder ? CCBool::create(true) : nullptr);
        levels->addObject(level);
        m_tagged->addObject(level);
    }

    this->setLoading(false);
    this->setListView(levels);
}

void IDLListLayer::setListView(CCArray* levels) {
    if (m_list->m_listView) m_list->m_listView->removeFromParent();
    m_list->m_listView = CustomListView::create(levels, kLevelListType, kListViewHeight, kListWidth);
    m_list->addChild(m_list->m_listView, 6, 9);
}

void IDLListLayer::setLoading(bool loading) {
    m_spinner->setVisible(loading);
}

void IDLListLayer::showMessage(std::string const& message) {
    m_messageLabel->setString(message.c_str());
    m_messageLabel->setVisible(!message.empty());
}

void IDLListLayer::updateControls() {
    int const total = static_cast<int>(m_filtered.size());
    int const pages = this->pageCount();

    if (total == 0) {
        m_countLabel->setString(idl::levelsLoaded() ? "0 demons" : "");
    }
    else {
        int const first = m_page * kPageSize + 1;
        int const last = std::min(total, first + kPageSize - 1);
        m_countLabel->setString(fmt::format("{} to {} of {}", first, last, total).c_str());
    }

    m_prevButton->setVisible(m_page > 0);
    m_nextButton->setVisible(m_page < pages - 1);
    m_lastButton->setVisible(m_page < pages - 1);
    m_pageButton->setVisible(total > 0);

    m_pageButtonLabel->setString(std::to_string(m_page + 1).c_str());
    m_pageButtonLabel->limitLabelWidth(28.f, .8f, .3f);
}

void IDLListLayer::untagLevels() {
    if (!m_tagged) return;
    for (auto level : CCArrayExt<GJGameLevel*>(m_tagged.data())) {
        level->setUserObject("entry"_spr, nullptr);
        level->setUserObject("placeholder"_spr, nullptr);
    }
    m_tagged->removeAllObjects();
}

void IDLListLayer::detachFromLevelManager() {
    auto glm = GameLevelManager::get();
    if (glm->m_levelManagerDelegate == this) glm->m_levelManagerDelegate = nullptr;
}
