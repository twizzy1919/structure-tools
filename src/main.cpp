// Structure Tools v0.2 - Pillars (down and up)
//
// Select platform blocks in the editor, then press "Down" or "Up" in the Edit tab.
// For every column in the selection the mod finds the LOWEST selected block (Down)
// or the HIGHEST (Up) and places blocks straight away from it, stopping after
// "Max Pillar Height" blocks, at the ground (Down only), or at the first object in the way.
//
// Off-grid handling: every new block uses the SAME X as the block it grows from and a
// Y that is an exact multiple of 30 away, so pillars stay on the exact same (sub)grid
// as their platform, even if that platform is off-grid.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr float GRID = 30.f;

// Two grid-aligned neighbours are exactly 30 units apart. Anything closer than
// this on both axes is treated as overlapping (catches half-grid / off-grid objects).
constexpr float OVERLAP = 27.f;

// Hard cap so a bad selection can never freeze the editor.
constexpr int MAX_TOTAL_BLOCKS = 20000;

// ---------------------------------------------------------------------------
// Slope orientation
// ---------------------------------------------------------------------------
// An unrotated 45 degree slope is a right triangle whose two flat edges (legs)
// are the BOTTOM and the RIGHT side. A pillar only makes sense next to a slope if
// one of its legs faces the pillar direction after flipping and rotating.

enum class Side { Top, Right, Bottom, Left };

Side flipH(Side s) {
    if (s == Side::Left) return Side::Right;
    if (s == Side::Right) return Side::Left;
    return s;
}

Side flipV(Side s) {
    if (s == Side::Top) return Side::Bottom;
    if (s == Side::Bottom) return Side::Top;
    return s;
}

// Clockwise quarter turn (cocos rotation is clockwise-positive): bottom -> left -> top -> right
Side rotateCW(Side s) {
    switch (s) {
        case Side::Bottom: return Side::Left;
        case Side::Left: return Side::Top;
        case Side::Top: return Side::Right;
        default: return Side::Bottom;
    }
}

bool slopeHasFlatSide(GameObject* obj, Side wanted) {
    float rot = std::fmod(obj->getRotation(), 360.f);
    if (rot < 0.f) rot += 360.f;

    float quarterTurns = rot / 90.f;
    int n = static_cast<int>(std::lround(quarterTurns));
    // Only handle exact multiples of 90 degrees; anything else is left alone.
    if (std::fabs(quarterTurns - static_cast<float>(n)) > 0.02f) return false;
    n %= 4;

    Side a = Side::Bottom;
    Side b = Side::Right;

    // The sprite is flipped first, then rotated.
    if (obj->m_isFlipX) { a = flipH(a); b = flipH(b); }
    if (obj->m_isFlipY) { a = flipV(a); b = flipV(b); }
    for (int i = 0; i < n; ++i) { a = rotateCW(a); b = rotateCW(b); }

    return a == wanted || b == wanted;
}

// ---------------------------------------------------------------------------
// Occupancy lookup
// ---------------------------------------------------------------------------

bool isBlocking(GameObject* obj) {
    switch (obj->m_objectType) {
        case GameObjectType::Solid:
        case GameObjectType::Slope:
        case GameObjectType::Hazard:
            return true;
        default:
            return false;
    }
}

// Objects bucketed by X column so lookups don't scan the whole level.
struct OccupancyIndex {
    std::unordered_map<int, std::vector<GameObject*>> columns;

    static int columnKey(float x) {
        return static_cast<int>(std::floor(x / GRID));
    }

    void add(GameObject* obj) {
        columns[columnKey(obj->getPositionX())].push_back(obj);
    }

    bool occupied(float x, float y) const {
        int k = columnKey(x);
        for (int c = k - 1; c <= k + 1; ++c) {
            auto it = columns.find(c);
            if (it == columns.end()) continue;
            for (auto* obj : it->second) {
                if (std::fabs(obj->getPositionX() - x) < OVERLAP &&
                    std::fabs(obj->getPositionY() - y) < OVERLAP) {
                    return true;
                }
            }
        }
        return false;
    }
};

void alert(char const* text) {
    FLAlertLayer::create("Structure Tools", text, "OK")->show();
}

} // namespace

class $modify(StructureEditorUI, EditorUI) {
public:
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        auto winSize = CCDirector::get()->getWinSize();

        // The Edit tab's bottom row has 5 empty slots to the right of "Warp".
        // We sit in them. Nudge with the "Button Offset" settings if needed.
        float offX = static_cast<float>(Mod::get()->getSettingValue<double>("button-offset-x"));
        float offY = static_cast<float>(Mod::get()->getSettingValue<double>("button-offset-y"));

        auto menu = CCMenu::create();
        menu->setID("structure-tools-menu"_spr);
        menu->setPosition({ winSize.width / 2.f + offX, 19.f + offY });

        char const* labels[3] = { "Down", "Up", "Pick" };
        SEL_MenuHandler handlers[3] = {
            menu_selector(StructureEditorUI::onPillarsDown),
            menu_selector(StructureEditorUI::onPillarsUp),
            menu_selector(StructureEditorUI::onPickFillBlock),
        };
        float xs[3] = { 42.f, 86.f, 130.f };

        ButtonSprite* sprites[3];
        float widest = 1.f;
        for (int i = 0; i < 3; ++i) {
            sprites[i] = ButtonSprite::create(labels[i], "bigFont.fnt", "GJ_button_04.png", .6f);
            widest = std::max(widest, sprites[i]->getContentSize().width * sprites[i]->getScale());
        }

        // One common scale so all three buttons fit the slots (about 38 units wide max).
        float fit = std::min(1.f, 38.f / widest);
        for (int i = 0; i < 3; ++i) {
            sprites[i]->setScale(sprites[i]->getScale() * fit);
            auto btn = CCMenuItemSpriteExtra::create(sprites[i], this, handlers[i]);
            btn->setPosition({ xs[i], 0.f });
            menu->addChild(btn);
        }

        this->addChild(menu, 100);

        // Only show the buttons while the Edit tab is open (and not while playtesting).
        this->schedule(schedule_selector(StructureEditorUI::syncToolsVisibility));
        this->syncToolsVisibility(0.f);

        return true;
    }

    void syncToolsVisibility(float) {
        auto menu = this->getChildByID("structure-tools-menu"_spr);
        if (!menu) return;

        bool show = m_editButtonBar && m_editButtonBar->isVisible() &&
                    m_editorLayer && m_editorLayer->m_playbackMode == PlaybackMode::Not;
        menu->setVisible(show);
    }

    // Remember the selected block's ID as the block pillars are built from.
    void onPickFillBlock(CCObject*) {
        auto selected = this->getSelectedObjects();
        if (!selected || selected->count() != 1) {
            alert("Select exactly <cy>one</c> block (the variant you want pillars made from), then press <cg>Pick</c>.");
            return;
        }

        auto obj = static_cast<GameObject*>(selected->objectAtIndex(0));
        Mod::get()->setSettingValue<int64_t>("fill-block-id", obj->m_objectID);

        Notification::create(
            fmt::format("Pillar block set to ID {}", obj->m_objectID),
            NotificationIcon::Success
        )->show();
    }

    void onPillarsDown(CCObject*) { this->fillPillars(-1); }
    void onPillarsUp(CCObject*) { this->fillPillars(+1); }

    // dir = -1: grow downward from the lowest selected block of each column
    // dir = +1: grow upward from the highest selected block of each column
    void fillPillars(int dir) {
        auto lel = m_editorLayer;

        auto selected = this->getSelectedObjects();
        if (!selected || selected->count() == 0) {
            alert("Select the platform blocks you want pillars on first.");
            return;
        }

        int fillId = static_cast<int>(Mod::get()->getSettingValue<int64_t>("fill-block-id"));
        float groundY = static_cast<float>(Mod::get()->getSettingValue<double>("ground-y"));
        int maxRows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-rows"));
        Side wantedSide = dir < 0 ? Side::Bottom : Side::Top;

        // 1) Extreme selected solid/slope per X column (lowest for Down, highest for Up).
        //    Key = X doubled and rounded, so off-grid X values still group correctly.
        std::map<long, GameObject*> extremePerColumn;
        for (auto obj : CCArrayExt<GameObject*>(selected)) {
            if (obj->m_objectType != GameObjectType::Solid &&
                obj->m_objectType != GameObjectType::Slope) {
                continue;
            }
            long key = std::lround(obj->getPositionX() * 2.f);
            auto it = extremePerColumn.find(key);
            bool better = it == extremePerColumn.end() ||
                (dir < 0 ? obj->getPositionY() < it->second->getPositionY()
                         : obj->getPositionY() > it->second->getPositionY());
            if (better) extremePerColumn[key] = obj;
        }

        if (extremePerColumn.empty()) {
            alert("None of the selected objects are blocks or slopes.");
            return;
        }

        // 2) Index everything that can block a pillar.
        OccupancyIndex index;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            if (isBlocking(obj)) index.add(obj);
        }

        // 3) Build the pillars.
        auto created = CCArray::create();
        int placed = 0;
        int columnsFilled = 0;
        int skippedSlopes = 0;
        bool needFillId = false;
        bool usedOwnBlock = false;

        for (auto& [key, src] : extremePerColumn) {
            if (placed >= MAX_TOTAL_BLOCKS) break;

            bool isSlope = src->m_objectType == GameObjectType::Slope;

            if (isSlope && !slopeHasFlatSide(src, wantedSide)) {
                ++skippedSlopes;
                continue;
            }

            int id = fillId;
            if (id <= 0) {
                if (isSlope) { needFillId = true; continue; }
                id = src->m_objectID; // fallback: same block as the platform
                usedOwnBlock = true;
            }

            float x = src->getPositionX();
            int before = placed;

            for (int row = 1; row <= maxRows; ++row) {
                float y = src->getPositionY() + static_cast<float>(dir) * GRID * static_cast<float>(row);
                if (dir < 0 && y < groundY - 0.5f) break; // reached the ground
                if (index.occupied(x, y)) break;          // hit something

                // true = "no undo": we register ONE grouped undo step below.
                auto newObj = lel->createObject(id, { x, y }, true);
                if (!newObj) break;

                created->addObject(newObj);
                ++placed;
                if (placed >= MAX_TOTAL_BLOCKS) break;
            }

            if (placed > before) ++columnsFilled;
        }

        if (created->count() > 0) {
            lel->addToUndoList(
                UndoObject::createWithArray(created, UndoCommand::Paste), false
            );
        }

        // 4) Tell the user what happened.
        if (needFillId) {
            alert(
                "Slopes need a pillar block. Select one block (the variant you want "
                "pillars made from) and press <cg>Pick</c>."
            );
        }

        std::string msg = fmt::format("Placed {} blocks in {} columns", placed, columnsFilled);
        if (skippedSlopes > 0) {
            msg += fmt::format(" ({} slopes skipped)", skippedSlopes);
        }
        if (usedOwnBlock && placed > 0) {
            msg += " - no pillar block picked, used the platform's own block";
        }
        Notification::create(
            msg, placed > 0 ? NotificationIcon::Success : NotificationIcon::Warning
        )->show();
    }
};
