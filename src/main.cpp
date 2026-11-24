// Structure Tools v0.3 - Auto Structure (Pillars style)
//
// One "Auto" button in the Edit tab. Press it:
//   - with objects selected  -> only the selection is structured
//   - with nothing selected  -> the whole level is scanned
//
// Direction is read from the lines on each block (Direction setting = Auto):
//   line on TOP, none on bottom    -> platform faces up   -> pillar grows DOWN
//   line on BOTTOM, none on top    -> platform is upside down -> pillar grows UP
//   (a 3-line block therefore points away from its empty side; a Top+Left block goes down)
// Slopes use whichever flat side faces away from the slope.
//
// Pillar block variants (IDs confirmed in-game):
//   2 = top line only, 3 = top+left, 5 = no lines, 6 = top+left+right, 7 = left+right
//   1743 = 45 degree slope
// A pillar cell picks its look from its neighbours (other pillar columns):
//   no neighbour either side -> ID 7        (lines left + right)
//   neighbour on the right   -> ID 2 rot 270 (line on the left only)
//   neighbour on the left    -> ID 2 rot 90  (line on the right only)
//   neighbours both sides    -> ID 5        (no lines)
// Spikes are never added.
//
// Off-grid: every new block uses the same X as its source and a Y exactly 30 units
// further, so pillars stay on the platform's own sub-grid.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr float GRID = 30.f;
constexpr float OVERLAP = 27.f;
constexpr int MAX_TOTAL_BLOCKS = 20000;

constexpr int ID_TOP = 2;
constexpr int ID_NONE = 5;
constexpr int ID_SIDES = 7;
constexpr int ID_SLOPE45 = 1743;

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
// clockwise quarter turn: top -> right -> bottom -> left -> top
Side rotateCW(Side s) {
    switch (s) {
        case Side::Top: return Side::Right;
        case Side::Right: return Side::Bottom;
        case Side::Bottom: return Side::Left;
        default: return Side::Top;
    }
}

// Applies the object's flips and rotation to a list of sides. false = not a multiple of 90.
bool transformSides(GameObject* obj, std::vector<Side>& sides) {
    float rot = std::fmod(obj->getRotation(), 360.f);
    if (rot < 0.f) rot += 360.f;
    float q = rot / 90.f;
    int n = static_cast<int>(std::lround(q));
    if (std::fabs(q - static_cast<float>(n)) > 0.02f) return false;
    n %= 4;
    for (auto& s : sides) {
        if (obj->m_isFlipX) s = flipH(s);
        if (obj->m_isFlipY) s = flipV(s);
        for (int i = 0; i < n; ++i) s = rotateCW(s);
    }
    return true;
}

bool has(std::vector<Side> const& v, Side s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Lines each known block variant has when unrotated.
bool baseLines(int id, std::vector<Side>& out) {
    switch (id) {
        case 2: out = { Side::Top }; return true;
        case 3: out = { Side::Top, Side::Left }; return true;
        case 5: out = {}; return true;
        case 6: out = { Side::Top, Side::Left, Side::Right }; return true;
        case 7: out = { Side::Left, Side::Right }; return true;
        default: return false;
    }
}

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

struct OccupancyIndex {
    std::unordered_map<int, std::vector<GameObject*>> columns;
    static int columnKey(float x) { return static_cast<int>(std::floor(x / GRID)); }
    void add(GameObject* o) { columns[columnKey(o->getPositionX())].push_back(o); }
    bool occupied(float x, float y) const {
        int k = columnKey(x);
        for (int c = k - 1; c <= k + 1; ++c) {
            auto it = columns.find(c);
            if (it == columns.end()) continue;
            for (auto* o : it->second) {
                if (std::fabs(o->getPositionX() - x) < OVERLAP &&
                    std::fabs(o->getPositionY() - y) < OVERLAP) return true;
            }
        }
        return false;
    }
};

// One planned pillar: a column of cells growing from a source object.
struct Pillar {
    float x = 0.f;
    std::vector<float> ys;
};

// Direction for one source object: -1 down, +1 up, 0 = can't tell / skip.
int decideDirection(GameObject* obj, std::string const& setting, bool wholeLevel) {
    int forced = setting == "Down" ? -1 : setting == "Up" ? +1 : 0;
    bool isSlope = obj->m_objectType == GameObjectType::Slope;

    if (isSlope) {
        // unrotated 45 slope: flat legs are Bottom and Right
        std::vector<Side> legs = { Side::Bottom, Side::Right };
        if (!transformSides(obj, legs)) return 0;
        bool b = has(legs, Side::Bottom), t = has(legs, Side::Top);
        if (forced < 0) return b ? -1 : 0;
        if (forced > 0) return t ? +1 : 0;
        return b ? -1 : (t ? +1 : 0);
    }

    if (forced != 0) return forced;

    std::vector<Side> lines;
    if (!baseLines(obj->m_objectID, lines)) return 0;
    if (!transformSides(obj, lines)) return 0;
    bool t = has(lines, Side::Top), b = has(lines, Side::Bottom);
    if (t && !b) return -1;
    if (b && !t) return +1;
    (void)wholeLevel;
    return 0;
}

void alert(char const* text) {
    FLAlertLayer::create("Structure Tools", text, "OK")->show();
}

} // namespace

class $modify(StructureEditorUI, EditorUI) {
public:
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        auto winSize = CCDirector::get()->getWinSize();
        float offX = static_cast<float>(Mod::get()->getSettingValue<double>("button-offset-x"));
        float offY = static_cast<float>(Mod::get()->getSettingValue<double>("button-offset-y"));

        auto menu = CCMenu::create();
        menu->setID("structure-tools-menu"_spr);
        menu->setPosition({ winSize.width / 2.f + offX, 19.f + offY });

        auto spr = ButtonSprite::create("Auto", "bigFont.fnt", "GJ_button_04.png", .6f);
        float w = std::max(1.f, spr->getContentSize().width * spr->getScale());
        spr->setScale(spr->getScale() * std::min(1.f, 80.f / w));
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(StructureEditorUI::onAutoStructure)
        );
        btn->setPosition({ 86.f, 0.f });
        menu->addChild(btn);
        this->addChild(menu, 100);

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

    void onAutoStructure(CCObject*) {
        // Only one style exists so far; the setting is here for the next ones.
        auto style = Mod::get()->getSettingValue<std::string>("style");
        if (style == "Pillars") this->runPillars();
    }

    void runPillars() {
        auto lel = m_editorLayer;
        auto setting = Mod::get()->getSettingValue<std::string>("direction");
        float groundY = static_cast<float>(Mod::get()->getSettingValue<double>("ground-y"));
        int maxRows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-rows"));

        // 1) Source objects: the selection, or the whole level if nothing is selected.
        auto selected = this->getSelectedObjects();
        bool wholeLevel = !selected || selected->count() == 0;
        CCArray* pool = wholeLevel ? lel->m_objects : selected;

        // Extreme source per (column, direction): lowest for down, highest for up.
        std::map<std::pair<long, int>, GameObject*> sources;
        int skipped = 0;
        for (auto obj : CCArrayExt<GameObject*>(pool)) {
            bool solid = obj->m_objectType == GameObjectType::Solid;
            bool slope = obj->m_objectType == GameObjectType::Slope;
            if (!solid && !slope) continue;

            if (wholeLevel) {
                // only scan blocks/slopes from the line family we know
                int id = obj->m_objectID;
                bool known = id == 2 || id == 3 || id == 6 || id == ID_SLOPE45;
                if (!known) continue;
            }

            int dir = decideDirection(obj, setting, wholeLevel);
            if (dir == 0) { ++skipped; continue; }

            std::pair<long, int> key = { std::lround(obj->getPositionX() * 2.f), dir };
            auto it = sources.find(key);
            bool better = it == sources.end() ||
                (dir < 0 ? obj->getPositionY() < it->second->getPositionY()
                         : obj->getPositionY() > it->second->getPositionY());
            if (better) sources[key] = obj;
        }

        if (sources.empty()) {
            alert(
                "No platform found. Auto direction reads the lines on the block: a "
                "<cy>top line</c> grows the pillar down, a <cy>bottom line</c> grows it up. "
                "Set <cy>Direction</c> in the mod settings if your blocks are another variant."
            );
            return;
        }

        // 2) Occupancy of the level.
        OccupancyIndex index;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            if (isBlocking(obj)) index.add(obj);
        }

        // 3) Plan every pillar first, so neighbours are known before variants are chosen.
        std::vector<Pillar> pillars;
        int total = 0;
        for (auto& [key, src] : sources) {
            if (total >= MAX_TOTAL_BLOCKS) break;
            int dir = key.second;
            Pillar p;
            p.x = src->getPositionX();
            for (int row = 1; row <= maxRows; ++row) {
                float y = src->getPositionY() + static_cast<float>(dir) * GRID * static_cast<float>(row);
                if (dir < 0 && y < groundY - 0.5f) break;
                if (index.occupied(p.x, y)) break;
                p.ys.push_back(y);
                ++total;
            }
            if (!p.ys.empty()) pillars.push_back(std::move(p));
        }

        if (pillars.empty()) {
            Notification::create("Nothing to fill (blocked or already at the ground)", NotificationIcon::Warning)->show();
            return;
        }

        // Lookup of pillars by X (half-unit precision) for neighbour checks.
        std::unordered_map<long, std::vector<Pillar const*>> byX;
        for (auto& p : pillars) byX[std::lround(p.x * 2.f)].push_back(&p);

        auto hasCell = [&](float x, float y) {
            long k = std::lround(x * 2.f);
            for (long d = -1; d <= 1; ++d) {
                auto it = byX.find(k + d);
                if (it == byX.end()) continue;
                for (auto* q : it->second) {
                    if (std::fabs(q->x - x) > 3.f) continue;
                    for (float qy : q->ys) {
                        if (std::fabs(qy - y) < 3.f) return true;
                    }
                }
            }
            return false;
        };

        // 4) Place blocks with the right variant.
        auto created = CCArray::create();
        int placed = 0;
        for (auto& p : pillars) {
            for (float y : p.ys) {
                bool left = hasCell(p.x - GRID, y);
                bool right = hasCell(p.x + GRID, y);

                int id;
                float rot = 0.f;
                if (!left && !right) id = ID_SIDES;
                else if (!left && right) { id = ID_TOP; rot = 270.f; } // line on the left
                else if (left && !right) { id = ID_TOP; rot = 90.f; }  // line on the right
                else id = ID_NONE;

                auto obj = lel->createObject(id, { p.x, y }, true);
                if (!obj) continue;
                if (rot != 0.f) obj->setRotation(rot);
                created->addObject(obj);
                ++placed;
            }
        }

        if (created->count() > 0) {
            lel->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        }

        std::string msg = fmt::format("Placed {} blocks in {} pillars", placed, pillars.size());
        if (skipped > 0) msg += fmt::format(" ({} skipped)", skipped);
        Notification::create(msg, placed > 0 ? NotificationIcon::Success : NotificationIcon::Warning)->show();
    }
};
