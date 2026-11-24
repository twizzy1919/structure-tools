// Structure Tools - Stage 1: Fill Pillars
//
// Select platform blocks in the editor and press "Pillars".
// For every column in the selection, the mod finds the LOWEST selected block/slope
// and places blocks straight down from it until it reaches the ground row or runs
// into another object.
//
// Off-grid handling: every new block uses the SAME X as the block above it and a Y
// that is an exact multiple of 30 below it, so pillars stay on the exact same
// (sub)grid as the platform they hang from, even if that platform is off-grid.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

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
// are the BOTTOM and the RIGHT side. A pillar only makes sense below a slope if
// one of its legs ends up on the bottom after flipping and rotating.

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

bool slopeHasFlatBottom(GameObject* obj) {
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

    return a == Side::Bottom || b == Side::Bottom;
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

        auto menu = CCMenu::create();
        menu->setID("structure-tools-menu"_spr);
        // Right edge, vertically centred. If this overlaps another mod's buttons,
        // just change these two numbers.
        menu->setPosition({ winSize.width - 48.f, winSize.height * 0.5f });

        auto pillarSpr = ButtonSprite::create("Pillars", "bigFont.fnt", "GJ_button_04.png", .6f);
        auto pillarBtn = CCMenuItemSpriteExtra::create(
            pillarSpr, this, menu_selector(StructureEditorUI::onFillPillars)
        );
        pillarBtn->setPosition({ 0.f, 18.f });
        menu->addChild(pillarBtn);

        auto pickSpr = ButtonSprite::create("Pick ID", "bigFont.fnt", "GJ_button_04.png", .6f);
        auto pickBtn = CCMenuItemSpriteExtra::create(
            pickSpr, this, menu_selector(StructureEditorUI::onPickFillBlock)
        );
        pickBtn->setPosition({ 0.f, -18.f });
        menu->addChild(pickBtn);

        this->addChild(menu, 100);
        return true;
    }

    // Remember the selected block's ID as the fill block.
    void onPickFillBlock(CCObject*) {
        auto selected = this->getSelectedObjects();
        if (!selected || selected->count() != 1) {
            alert("Select exactly <cy>one</c> block, then press <cg>Pick ID</c>.");
            return;
        }

        auto obj = static_cast<GameObject*>(selected->objectAtIndex(0));
        Mod::get()->setSettingValue<int64_t>("fill-block-id", obj->m_objectID);

        Notification::create(
            fmt::format("Fill block set to ID {}", obj->m_objectID),
            NotificationIcon::Success
        )->show();
    }

    void onFillPillars(CCObject*) {
        auto lel = m_editorLayer;

        auto selected = this->getSelectedObjects();
        if (!selected || selected->count() == 0) {
            alert("Select the platform blocks you want pillars under first.");
            return;
        }

        int fillId = static_cast<int>(Mod::get()->getSettingValue<int64_t>("fill-block-id"));
        float groundY = static_cast<float>(Mod::get()->getSettingValue<double>("ground-y"));
        int maxRows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-rows"));

        // 1) Lowest selected solid/slope per X column.
        //    Key = X doubled and rounded, so off-grid X values still group correctly.
        std::map<long, GameObject*> lowestPerColumn;
        for (auto obj : CCArrayExt<GameObject*>(selected)) {
            if (obj->m_objectType != GameObjectType::Solid &&
                obj->m_objectType != GameObjectType::Slope) {
                continue;
            }
            long key = std::lround(obj->getPositionX() * 2.f);
            auto it = lowestPerColumn.find(key);
            if (it == lowestPerColumn.end() ||
                obj->getPositionY() < it->second->getPositionY()) {
                lowestPerColumn[key] = obj;
            }
        }

        if (lowestPerColumn.empty()) {
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

        for (auto& [key, src] : lowestPerColumn) {
            if (placed >= MAX_TOTAL_BLOCKS) break;

            bool isSlope = src->m_objectType == GameObjectType::Slope;

            if (isSlope && !slopeHasFlatBottom(src)) {
                ++skippedSlopes;
                continue;
            }

            int id = fillId;
            if (id <= 0) {
                if (isSlope) { needFillId = true; continue; }
                id = src->m_objectID; // same block as the platform
            }

            float x = src->getPositionX();
            int before = placed;

            for (int row = 1; row <= maxRows; ++row) {
                float y = src->getPositionY() - GRID * static_cast<float>(row);
                if (y < groundY - 0.5f) break;     // reached the ground
                if (index.occupied(x, y)) break;   // hit something

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
                "Slopes need a fill block. Select one regular block and press "
                "<cg>Pick ID</c> (or set <cy>Fill Block ID</c> in the mod settings)."
            );
        }

        std::string msg = fmt::format("Placed {} blocks in {} columns", placed, columnsFilled);
        if (skippedSlopes > 0) {
            msg += fmt::format(" ({} slopes skipped)", skippedSlopes);
        }
        Notification::create(
            msg, placed > 0 ? NotificationIcon::Success : NotificationIcon::Warning
        )->show();
    }
};
