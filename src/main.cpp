// Structure Tools v0.4 - Auto Structure (Pillars style)
//
// One "Auto" button in the Edit tab:
//   - objects selected  -> only the selection is structured
//   - nothing selected  -> the whole level is scanned
//
// Direction (Direction setting = Auto):
//   Blocks: line on TOP (none on bottom) -> pillar grows DOWN; line on BOTTOM -> grows UP.
//   Slopes: every right-triangle slope has one HORIZONTAL flat leg. If that leg is on the
//           bottom the pillar grows down from it, if it is on the top the pillar grows up.
//           The 2x1 slope (1744) has a 2-cell leg and a 1-cell leg: when the long leg is
//           horizontal the pillar is 2 wide, when the slope is stood on its end the
//           horizontal leg is the short one and the pillar is 1 wide.
//
// Block IDs (confirmed in-game):
//   2 = top line, 3 = top+left, 5 = no lines, 6 = top+left+right, 7 = left+right
//   1743 = 45 degree slope, 1744 = 2x1 slope
// Pillar cell look is chosen from neighbouring pillar columns:
//   no neighbours -> ID 7;  neighbour right -> ID 2 rot 270;  neighbour left -> ID 2 rot 90;
//   neighbours both sides -> ID 5.
// Spikes are never added.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
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
constexpr int ID_SLOPE2X1 = 1744;

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

// Applies the object's flips and rotation. false = rotation is not a multiple of 90.
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

// A pillar starting point. y = centre of the FIRST cell to place, dir = -1 down / +1 up.
struct Source {
    float x = 0.f;
    float y = 0.f;
    int dir = 0;
};

struct Pillar {
    float x = 0.f;
    std::vector<float> ys;
};

enum class SrcResult { Ok, Skip, Unknown };

// Turns one block/slope into zero or more pillar starting points.
SrcResult makeSources(GameObject* obj, std::string const& setting, std::vector<Source>& out) {
    int forced = setting == "Down" ? -1 : setting == "Up" ? +1 : 0;
    float px = obj->getPositionX();
    float py = obj->getPositionY();

    if (obj->m_objectType == GameObjectType::Slope) {
        // Unrotated slope: flat legs are Bottom (long leg on the 2x1) and Right (1 cell).
        std::vector<Side> sides = { Side::Bottom, Side::Right };
        int lens[2] = { obj->m_objectID == ID_SLOPE2X1 ? 2 : 1, 1 };
        if (!transformSides(obj, sides)) return SrcResult::Skip;

        int h = (sides[0] == Side::Top || sides[0] == Side::Bottom) ? 0 : 1; // horizontal leg
        int v = 1 - h;                                                         // vertical leg
        int dir = sides[h] == Side::Bottom ? -1 : +1;
        if (forced != 0 && forced != dir) return SrcResult::Skip;

        // The vertical leg is the slope's height. First cell sits half a cell past its edge.
        float halfHeight = static_cast<float>(lens[v]) * GRID / 2.f;
        float yFirst = py + static_cast<float>(dir) * (halfHeight + GRID / 2.f);
        int cols = lens[h];
        for (int i = 0; i < cols; ++i) {
            float x = px + (static_cast<float>(i) - static_cast<float>(cols - 1) / 2.f) * GRID;
            out.push_back({ x, yFirst, dir });
        }
        return SrcResult::Ok;
    }

    int dir = forced;
    if (dir == 0) {
        std::vector<Side> lines;
        if (!baseLines(obj->m_objectID, lines)) return SrcResult::Unknown;
        if (!transformSides(obj, lines)) return SrcResult::Skip;
        bool t = has(lines, Side::Top), b = has(lines, Side::Bottom);
        if (t && !b) dir = -1;
        else if (b && !t) dir = +1;
        else return SrcResult::Skip; // side-only or no lines: no up/down to read
    }
    out.push_back({ px, py + static_cast<float>(dir) * GRID, dir });
    return SrcResult::Ok;
}

void alert(std::string const& text) {
    FLAlertLayer::create("Structure Tools", text.c_str(), "OK")->show();
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
        auto style = Mod::get()->getSettingValue<std::string>("style");
        if (style == "Pillars") this->runPillars();
    }

    void runPillars() {
        auto lel = m_editorLayer;
        auto setting = Mod::get()->getSettingValue<std::string>("direction");
        float groundY = static_cast<float>(Mod::get()->getSettingValue<double>("ground-y"));
        int maxRows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-rows"));

        auto selected = this->getSelectedObjects();
        bool wholeLevel = !selected || selected->count() == 0;
        CCArray* pool = wholeLevel ? lel->m_objects : selected;

        // 1) Starting points; keep the extreme one per (column, direction).
        std::map<std::pair<long, int>, Source> sources;
        int skipped = 0;
        std::set<int> unknownIds;
        for (auto obj : CCArrayExt<GameObject*>(pool)) {
            bool solid = obj->m_objectType == GameObjectType::Solid;
            bool slope = obj->m_objectType == GameObjectType::Slope;
            if (!solid && !slope) continue;

            int id = obj->m_objectID;
            if (wholeLevel) {
                bool known = id == 2 || id == 3 || id == 6 ||
                             id == ID_SLOPE45 || id == ID_SLOPE2X1;
                if (!known) {
                    // 5 and 7 are the pillar fillers themselves; everything else is reported.
                    if (id != ID_NONE && id != ID_SIDES) unknownIds.insert(id);
                    continue;
                }
            }

            std::vector<Source> list;
            auto res = makeSources(obj, setting, list);
            if (res == SrcResult::Unknown) { unknownIds.insert(id); ++skipped; continue; }
            if (res == SrcResult::Skip) { ++skipped; continue; }

            for (auto& s : list) {
                std::pair<long, int> key = { std::lround(s.x * 2.f), s.dir };
                auto it = sources.find(key);
                bool better = it == sources.end() ||
                    (s.dir < 0 ? s.y < it->second.y : s.y > it->second.y);
                if (better) sources[key] = s;
            }
        }

        if (sources.empty()) {
            std::string msg =
                "No platform found. Auto direction reads the lines on the block: a "
                "<cy>top line</c> grows the pillar down, a <cy>bottom line</c> grows it up. "
                "Set <cy>Direction</c> in the mod settings if your blocks are another variant.";
            if (!unknownIds.empty()) {
                msg += "\n\nUnknown block IDs: ";
                int n = 0;
                for (int i : unknownIds) { if (n++) msg += ", "; msg += std::to_string(i); if (n >= 12) break; }
            }
            alert(msg);
            return;
        }

        // 2) Occupancy of the level.
        OccupancyIndex index;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            if (isBlocking(obj)) index.add(obj);
        }

        // 3) Plan every pillar first so neighbours are known before variants are chosen.
        std::vector<Pillar> pillars;
        int total = 0;
        int blockedAtStart = 0;
        for (auto& [key, src] : sources) {
            if (total >= MAX_TOTAL_BLOCKS) break;
            Pillar p;
            p.x = src.x;
            for (int row = 0; row < maxRows; ++row) {
                float y = src.y + static_cast<float>(src.dir) * GRID * static_cast<float>(row);
                if (src.dir < 0 && y < groundY - 0.5f) break;
                if (index.occupied(p.x, y)) break;
                p.ys.push_back(y);
                ++total;
            }
            if (p.ys.empty()) ++blockedAtStart;
            else pillars.push_back(std::move(p));
        }

        if (pillars.empty()) {
            Notification::create("Nothing to fill (blocked or already at the ground)", NotificationIcon::Warning)->show();
            return;
        }

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
        if (blockedAtStart > 0) msg += fmt::format(" ({} blocked)", blockedAtStart);
        Notification::create(msg, placed > 0 ? NotificationIcon::Success : NotificationIcon::Warning)->show();

        // Explain what was ignored so missing pillars can be diagnosed.
        if (!unknownIds.empty()) {
            std::string detail =
                "Some blocks were ignored because their IDs aren't in my table yet. "
                "Tell me these IDs and which edges have lines on each, and I'll add them.\n\nIDs: ";
            int n = 0;
            for (int i : unknownIds) { if (n++) detail += ", "; detail += std::to_string(i); if (n >= 12) break; }
            alert(detail);
        }
    }
};
