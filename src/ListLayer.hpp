#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/LoadingSpinner.hpp>

#include <string>
#include <vector>

// The Infinite Demonlist browser, opened from the online search screen.
class IDLListLayer : public cocos2d::CCLayer,
                     public LevelManagerDelegate,
                     public SetIDPopupDelegate,
                     public SetTextPopupDelegate {
public:
    static IDLListLayer* create();
    static cocos2d::CCScene* scene();

    ~IDLListLayer() override;

protected:
    bool init() override;
    void keyBackClicked() override;

    void onBack(cocos2d::CCObject*);
    void onPrevPage(cocos2d::CCObject*);
    void onNextPage(cocos2d::CCObject*);
    void onLastPage(cocos2d::CCObject*);
    void onPickPage(cocos2d::CCObject*);
    void onToggleMode(cocos2d::CCObject*);
    void onRefresh(cocos2d::CCObject*);
    void onSearch(cocos2d::CCObject*);

    // LevelManagerDelegate
    void loadLevelsFinished(cocos2d::CCArray* levels, char const* key) override;
    void loadLevelsFinished(cocos2d::CCArray* levels, char const* key, int type) override;
    void loadLevelsFailed(char const* key) override;
    void loadLevelsFailed(char const* key, int type) override;
    void setupPageInfo(gd::string info, char const* key) override {}

    // SetIDPopupDelegate
    void setIDPopupClosed(SetIDPopup* popup, int value) override;

    // SetTextPopupDelegate
    void setTextPopupClosed(SetTextPopup* popup, gd::string text) override;

private:
    void fetchList();
    void loadAllDetails();
    void updateMode();
    void setQuery(std::string query);
    void updateSearchButton();
    void applySearch();
    void goToPage(int page);
    void loadPage();
    void requestGDLevels();
    void showPage(cocos2d::CCArray* gdLevels);
    void setListView(cocos2d::CCArray* levels);
    void setLoading(bool loading);
    void showMessage(std::string const& message);
    void updateControls();
    void untagLevels();
    void detachFromLevelManager();
    int pageCount() const;

    GJListLayer* m_list = nullptr;
    CCMenuItemSpriteExtra* m_searchButton = nullptr;
    geode::LoadingSpinner* m_spinner = nullptr;
    cocos2d::CCLabelBMFont* m_countLabel = nullptr;
    cocos2d::CCLabelBMFont* m_messageLabel = nullptr;
    cocos2d::CCLabelBMFont* m_pageButtonLabel = nullptr;
    CCMenuItemSpriteExtra* m_prevButton = nullptr;
    CCMenuItemSpriteExtra* m_nextButton = nullptr;
    CCMenuItemSpriteExtra* m_lastButton = nullptr;
    CCMenuItemSpriteExtra* m_pageButton = nullptr;
    CCMenuItemSpriteExtra* m_modeButton = nullptr;
    cocos2d::CCSprite* m_modeSprite = nullptr;
    cocos2d::CCLabelBMFont* m_modeLabel = nullptr;
    cocos2d::CCLabelBMFont* m_titleLabel = nullptr;

    std::vector<size_t> m_filtered;   // indices into idl::levels()
    std::vector<size_t> m_pageEntries;
    std::string m_query;
    std::string m_searchKey;
    int m_page = 0;
    int m_generation = 0;             // bumps on every page load, to drop stale answers
    bool m_listLoading = false;
    bool m_unverifiedMode = false;   // showing the levels that aren't on the GD servers

    // Level objects this layer tagged, so the tags come off when it closes.
    geode::Ref<cocos2d::CCArray> m_tagged;
};
