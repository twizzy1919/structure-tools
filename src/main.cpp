// Structure Tools v0.8 - Pillars, Enclose and Shapes styles
//
// A small "Auto" button in the Edit tab opens the Structure Tools menu
// (style, direction, max height, Structure, Show ID).
//   - objects selected  -> only the selection is structured
//   - nothing selected  -> the whole level is scanned
// Pillar edge lines are hidden where a pillar cell touches an existing solid block or a
// slope whose flat side faces it.
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
#include <random>
#include <set>
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
        case 1: out = { Side::Top, Side::Right, Side::Bottom, Side::Left }; return true;
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

// Geometry of a slope after flips/rotation. sides[0]/lens[0] = the unrotated Bottom leg
// (2 cells long on the 2x1 slope), sides[1]/lens[1] = the unrotated Right leg (1 cell).
struct SlopeInfo {
    std::vector<Side> sides;
    int lens[2] = { 1, 1 };
    int h = 0; // index of the horizontal leg
    int v = 1; // index of the vertical leg
};

bool slopeInfo(GameObject* obj, SlopeInfo& si) {
    si.sides = { Side::Bottom, Side::Right };
    si.lens[0] = obj->m_objectID == ID_SLOPE2X1 ? 2 : 1;
    si.lens[1] = 1;
    if (!transformSides(obj, si.sides)) return false;
    si.h = (si.sides[0] == Side::Top || si.sides[0] == Side::Bottom) ? 0 : 1;
    si.v = 1 - si.h;
    return true;
}

// Existing solid things next to a pillar cell hide that cell's edge line.
struct NeighborIndex {
    struct Item { float x, y, hx, hy; bool leftFlat, rightFlat; };
    std::unordered_map<int, std::vector<Item>> columns;

    static int key(float x) { return static_cast<int>(std::floor(x / GRID)); }

    void add(GameObject* obj) {
        Item it{};
        it.x = obj->getPositionX();
        it.y = obj->getPositionY();
        if (obj->m_objectType == GameObjectType::Solid) {
            it.hx = it.hy = GRID / 2.f;
            it.leftFlat = it.rightFlat = true;
        } else if (obj->m_objectType == GameObjectType::Slope) {
            SlopeInfo si;
            if (!slopeInfo(obj, si)) return;
            it.hx = static_cast<float>(si.lens[si.h]) * GRID / 2.f;
            it.hy = static_cast<float>(si.lens[si.v]) * GRID / 2.f;
            it.leftFlat = has(si.sides, Side::Left);
            it.rightFlat = has(si.sides, Side::Right);
        } else {
            return;
        }
        columns[key(it.x)].push_back(it);
    }

    // Is the cell centred at (x, y) covered by something whose facing side is flat?
    // needRightFlat = the thing is to the LEFT of our cell, so its right side faces us.
    bool covered(float x, float y, bool needRightFlat) const {
        int k = key(x);
        for (int c = k - 2; c <= k + 2; ++c) {
            auto f = columns.find(c);
            if (f == columns.end()) continue;
            for (auto const& it : f->second) {
                if (std::fabs(it.x - x) <= it.hx - GRID / 2.f + 3.f &&
                    std::fabs(it.y - y) <= it.hy - GRID / 2.f + 3.f &&
                    (needRightFlat ? it.rightFlat : it.leftFlat)) return true;
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

enum class SrcResult { Ok, Skip, Unknown, NeedsNeighbor };

// Turns one block/slope into zero or more pillar starting points.
SrcResult makeSources(GameObject* obj, std::string const& setting, std::vector<Source>& out) {
    int forced = setting == "Down" ? -1 : setting == "Up" ? +1 : 0;
    float px = obj->getPositionX();
    float py = obj->getPositionY();

    if (obj->m_objectType == GameObjectType::Slope) {
        SlopeInfo si;
        if (!slopeInfo(obj, si)) return SrcResult::Skip;

        int dir = si.sides[si.h] == Side::Bottom ? -1 : +1;
        if (forced != 0 && forced != dir) return SrcResult::Skip;

        // The vertical leg is the slope's height. First cell sits half a cell past its edge.
        float halfHeight = static_cast<float>(si.lens[si.v]) * GRID / 2.f;
        float yFirst = py + static_cast<float>(dir) * (halfHeight + GRID / 2.f);
        int cols = si.lens[si.h];
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
        else if (obj->m_objectID == 1) return SrcResult::NeedsNeighbor; // default block: copy the blocks beside it
        else return SrcResult::Skip; // side-only or no lines: no up/down to read
    }
    out.push_back({ px, py + static_cast<float>(dir) * GRID, dir });
    return SrcResult::Ok;
}

void alert(std::string const& text) {
    FLAlertLayer::create("Structure Tools", text.c_str(), "OK")->show();
}

} // namespace

// ---------------------------------------------------------------------------
// Enclose style helpers
// ---------------------------------------------------------------------------
namespace {

enum : int { BT = 1, BR = 2, BB = 4, BL = 8 };

int bitOf(Side s) {
    switch (s) {
        case Side::Top: return BT;
        case Side::Right: return BR;
        case Side::Bottom: return BB;
        default: return BL;
    }
}

struct DirInfo { int dx, dy, bit, opp; };
constexpr DirInfo DIRS[4] = {
    { 0, 1, BT, BB }, { 1, 0, BR, BL }, { 0, -1, BB, BT }, { -1, 0, BL, BR }
};

struct BodyCell { int flat = 0; int open = 0; };
using CellKey = std::pair<int, int>;

struct Variant { int id; float rot; };

// Block variant (ID + rotation) that has lines on exactly the exposed sides.
Variant blockForExposure(int mask, bool& partial) {
    switch (mask) {
        case 0: return { ID_NONE, 0.f };
        case BT: return { 2, 0.f };
        case BR: return { 2, 90.f };
        case BB: return { 2, 180.f };
        case BL: return { 2, 270.f };
        case BT | BL: return { 3, 0.f };
        case BT | BR: return { 3, 90.f };
        case BR | BB: return { 3, 180.f };
        case BB | BL: return { 3, 270.f };
        case BL | BR: return { 7, 0.f };
        case BT | BB: return { 7, 90.f };
        case BT | BL | BR: return { 6, 0.f };
        case BR | BT | BB: return { 6, 90.f };
        case BB | BR | BL: return { 6, 180.f };
        case BL | BB | BT: return { 6, 270.f };
        default: partial = true; return { 6, 0.f }; // all four edges: no 4-line block known
    }
}

// Rotation of a 45 degree slope (unrotated legs: bottom + right) for a given leg pair.
float slopeRotationForLegs(int legs) {
    switch (legs) {
        case BB | BR: return 0.f;
        case BL | BB: return 90.f;
        case BT | BL: return 180.f;
        default: return 270.f; // top + right
    }
}

// Which 3-unit step of the 30-unit grid a coordinate sits on (0..9), so structures on
// different (off-grid) sub-grids are processed separately.
int subgrid(float v) {
    float r = std::fmod(v, GRID);
    if (r < 0.f) r += GRID;
    return static_cast<int>(std::floor((r + 1.5f) / 3.f)) % 10;
}

} // namespace

namespace {

std::vector<std::string> const STYLES = { "Pillars", "Enclose", "Shapes" };
std::vector<std::string> const DIRECTIONS = { "Auto", "Down", "Up" };

std::string cycle(std::vector<std::string> const& list, std::string const& cur, int delta) {
    int idx = 0;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (list[i] == cur) idx = i;
    }
    int n = static_cast<int>(list.size());
    idx = ((idx + delta) % n + n) % n;
    return list[idx];
}

} // namespace

class $modify(StructureEditorUI, EditorUI) {
public:
    struct Fields {
        FLAlertLayer* panel = nullptr;
        CCLabelBMFont* styleLabel = nullptr;
        CCLabelBMFont* dirLabel = nullptr;
        CCLabelBMFont* rowsLabel = nullptr;
    };

    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        auto winSize = CCDirector::get()->getWinSize();
        float offX = static_cast<float>(Mod::get()->getSettingValue<double>("button-offset-x"));
        float offY = static_cast<float>(Mod::get()->getSettingValue<double>("button-offset-y"));

        auto menu = CCMenu::create();
        menu->setID("structure-tools-menu"_spr);
        menu->setPosition({ winSize.width / 2.f + offX, 19.f + offY });

        // One small button in the first empty slot (same size as the other Edit buttons).
        auto spr = ButtonSprite::create("Auto", "bigFont.fnt", "GJ_button_04.png", .6f);
        float w = std::max(1.f, spr->getContentSize().width * spr->getScale());
        spr->setScale(spr->getScale() * std::min(1.f, 38.f / w));
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(StructureEditorUI::onOpenMenu)
        );
        btn->setPosition({ 42.f, 0.f });
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

    // ---- menu -----------------------------------------------------------------

    CCLabelBMFont* addMenuLabel(CCNode* parent, char const* text, float x, float y, float scale, bool leftAlign) {
        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(scale);
        if (leftAlign) label->setAnchorPoint({ 0.f, 0.5f });
        label->setPosition({ x, y });
        parent->addChild(label);
        return label;
    }

    void addMenuButton(CCNode* parent, char const* text, float x, float y, float scale, SEL_MenuHandler handler) {
        auto spr = ButtonSprite::create(text, "bigFont.fnt", "GJ_button_04.png", .6f);
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, handler);
        btn->setPosition({ x, y });
        parent->addChild(btn);
    }

    void onOpenMenu(CCObject*) {
        // Blank lines give the popup room for the controls.
        auto alert = FLAlertLayer::create(
            nullptr, "Structure Tools", " \n \n \n \n \n \n \n \n", "Close", nullptr, 320.f
        );
        auto menu = alert->m_buttonMenu; // origin = screen centre
        if (!menu) { alert->show(); return; }

        m_fields->panel = alert;

        auto style = Mod::get()->getSettingValue<std::string>("style");
        auto dir = Mod::get()->getSettingValue<std::string>("direction");
        int rows = std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-rows")), 1, 30);

        // Origin of this menu is the Close button; the box spans about +26 (bottom) to +229 (top).
        float y1 = 165.f, y2 = 130.f, y3 = 95.f, y4 = 55.f;

        addMenuLabel(menu, "Style", -135.f, y1, .5f, true);
        addMenuButton(menu, "<", 10.f, y1, .6f, menu_selector(StructureEditorUI::onStylePrev));
        m_fields->styleLabel = addMenuLabel(menu, style.c_str(), 70.f, y1, .5f, false);
        addMenuButton(menu, ">", 130.f, y1, .6f, menu_selector(StructureEditorUI::onStyleNext));

        addMenuLabel(menu, "Direction", -135.f, y2, .5f, true);
        addMenuButton(menu, "<", 10.f, y2, .6f, menu_selector(StructureEditorUI::onDirPrev));
        m_fields->dirLabel = addMenuLabel(menu, dir.c_str(), 70.f, y2, .5f, false);
        addMenuButton(menu, ">", 130.f, y2, .6f, menu_selector(StructureEditorUI::onDirNext));

        addMenuLabel(menu, "Max height", -135.f, y3, .5f, true);
        addMenuButton(menu, "-", 10.f, y3, .6f, menu_selector(StructureEditorUI::onRowsMinus));
        m_fields->rowsLabel = addMenuLabel(menu, std::to_string(rows).c_str(), 70.f, y3, .5f, false);
        addMenuButton(menu, "+", 130.f, y3, .6f, menu_selector(StructureEditorUI::onRowsPlus));

        addMenuButton(menu, "Structure", -60.f, y4, .8f, menu_selector(StructureEditorUI::onRunFromMenu));
        addMenuButton(menu, "Show ID", 70.f, y4, .8f, menu_selector(StructureEditorUI::onShowId));

        alert->show();
    }

    void onStylePrev(CCObject*) { this->changeStyle(-1); }
    void onStyleNext(CCObject*) { this->changeStyle(+1); }
    void changeStyle(int delta) {
        auto v = cycle(STYLES, Mod::get()->getSettingValue<std::string>("style"), delta);
        Mod::get()->setSettingValue<std::string>("style", v);
        if (m_fields->styleLabel) m_fields->styleLabel->setString(v.c_str());
    }

    void onDirPrev(CCObject*) { this->changeDir(-1); }
    void onDirNext(CCObject*) { this->changeDir(+1); }
    void changeDir(int delta) {
        auto v = cycle(DIRECTIONS, Mod::get()->getSettingValue<std::string>("direction"), delta);
        Mod::get()->setSettingValue<std::string>("direction", v);
        if (m_fields->dirLabel) m_fields->dirLabel->setString(v.c_str());
    }

    void onRowsMinus(CCObject*) { this->changeRows(-1); }
    void onRowsPlus(CCObject*) { this->changeRows(+1); }
    void changeRows(int delta) {
        int rows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-rows")) + delta;
        rows = std::clamp(rows, 1, 30);
        Mod::get()->setSettingValue<int64_t>("max-rows", rows);
        if (m_fields->rowsLabel) m_fields->rowsLabel->setString(std::to_string(rows).c_str());
    }

    void onShowId(CCObject*) {
        auto selected = this->getSelectedObjects();
        if (!selected || selected->count() == 0) {
            Notification::create("Select a block first", NotificationIcon::Warning)->show();
            return;
        }
        std::map<int, int> counts;
        for (auto obj : CCArrayExt<GameObject*>(selected)) ++counts[obj->m_objectID];
        std::string msg = "Selected IDs: ";
        int n = 0;
        for (auto& [id, c] : counts) {
            if (n++) msg += ", ";
            msg += std::to_string(id);
            if (c > 1) msg += fmt::format(" (x{})", c);
            if (n >= 10) break;
        }
        Notification::create(msg, NotificationIcon::Success)->show();
    }

    void onRunFromMenu(CCObject*) {
        if (m_fields->panel) {
            m_fields->panel->keyBackClicked(); // closes the popup
            m_fields->panel = nullptr;
        }
        auto style = Mod::get()->getSettingValue<std::string>("style");
        if (style == "Pillars") this->runPillars();
        else if (style == "Enclose") this->runEnclose(false);
        else if (style == "Shapes") this->runEnclose(true);
    }

    // Style 2: close blocks/slopes in with the fewest blocks that give every exposed edge
    // a line. Selection = only the selection; nothing selected = the whole level.
    // Touching structures merge into one; one-cell gaps between structures are filled.
    void runEnclose(bool shapes) {
        auto lel = m_editorLayer;
        auto selected = this->getSelectedObjects();
        bool wholeLevel = !selected || selected->count() == 0;
        CCArray* pool = wholeLevel ? lel->m_objects : selected;
        int chance = static_cast<int>(Mod::get()->getSettingValue<int64_t>("corner-slope-chance"));
        int shapePad = static_cast<int>(Mod::get()->getSettingValue<int64_t>("shape-padding"));
        int shapeCut = static_cast<int>(Mod::get()->getSettingValue<int64_t>("shape-max-cut"));
        int shapeLink = static_cast<int>(Mod::get()->getSettingValue<int64_t>("shape-link-distance"));

        struct Item { float cx, cy; int flat, open; GameObject* block; int lines; };
        std::vector<Item> items;
        int skipped = 0;
        std::set<int> unknownIds;

        for (auto obj : CCArrayExt<GameObject*>(pool)) {
            int id = obj->m_objectID;
            float px = obj->getPositionX(), py = obj->getPositionY();

            if (obj->m_objectType == GameObjectType::Slope) {
                if (wholeLevel && id != ID_SLOPE45 && id != ID_SLOPE2X1) continue;
                SlopeInfo si;
                if (!slopeInfo(obj, si)) { ++skipped; continue; }
                int W = si.lens[si.h], H = si.lens[si.v];
                int hBit = bitOf(si.sides[si.h]), vBit = bitOf(si.sides[si.v]);
                for (int i = 0; i < W; ++i) {
                    for (int j = 0; j < H; ++j) {
                        float cx = px + (static_cast<float>(i) - static_cast<float>(W - 1) / 2.f) * GRID;
                        float cy = py + (static_cast<float>(j) - static_cast<float>(H - 1) / 2.f) * GRID;
                        int flat = 0;
                        if (hBit == BT && j == H - 1) flat |= hBit;
                        if (hBit == BB && j == 0) flat |= hBit;
                        if (vBit == BR && i == W - 1) flat |= vBit;
                        if (vBit == BL && i == 0) flat |= vBit;
                        items.push_back({ cx, cy, flat, flat, nullptr, 0 });
                    }
                }
            } else if (obj->m_objectType == GameObjectType::Solid) {
                if (wholeLevel && !(id == 1 || id == 2 || id == 3 || id == 6)) {
                    // 5 and 7 are pillar fillers; anything else is reported.
                    if (id != ID_NONE && id != ID_SIDES) unknownIds.insert(id);
                    continue;
                }
                std::vector<Side> lines;
                int open = 0, lm = 15;
                GameObject* block = nullptr;
                if (baseLines(id, lines)) {
                    if (!transformSides(obj, lines)) { ++skipped; continue; }
                    lm = 0;
                    for (auto sd : lines) lm |= bitOf(sd);
                    // Blocks with no top or bottom line are pillar pieces or walls: leave alone.
                    if (wholeLevel && !(lm & (BT | BB))) continue;
                    open = 15 & ~lm;
                    if (id != 1) block = obj; // pattern blocks can be swapped to fix seams
                } else {
                    unknownIds.insert(id); // treated as a closed block
                }
                items.push_back({ px, py, 15, open, block, lm });
            }
        }

        if (items.empty()) {
            alert("No blocks or slopes found to enclose.");
            return;
        }

        std::map<std::pair<int, int>, std::vector<Item>> groups;
        for (auto& it : items) groups[{ subgrid(it.cx), subgrid(it.cy) }].push_back(it);

        OccupancyIndex index, solidIndex;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            if (isBlocking(obj)) index.add(obj);
            if (obj->m_objectType == GameObjectType::Solid) solidIndex.add(obj);
        }

        struct Body { int flat = 0; int open = 0; GameObject* block = nullptr; int lines = 0; };

        auto created = CCArray::create();
        auto removed = CCArray::create();
        int placed = 0, slopes = 0, fixedLines = 0;
        bool partial = false;
        std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> pct(1, 100);

        for (auto& gkv : groups) {
            auto& group = gkv.second;
            float ox = group[0].cx, oy = group[0].cy;
            auto worldX = [&](int ix) { return ox + static_cast<float>(ix) * GRID; };
            auto worldY = [&](int iy) { return oy + static_cast<float>(iy) * GRID; };

            std::map<CellKey, Body> body;
            for (auto& it : group) {
                float fx = (it.cx - ox) / GRID, fy = (it.cy - oy) / GRID;
                int ix = static_cast<int>(std::lround(fx)), iy = static_cast<int>(std::lround(fy));
                if (std::fabs(fx - static_cast<float>(ix)) * GRID > 4.f ||
                    std::fabs(fy - static_cast<float>(iy)) * GRID > 4.f) { ++skipped; continue; }
                auto& b = body[{ ix, iy }];
                b.flat |= it.flat;
                b.open |= it.open;
                if (it.block) { b.block = it.block; b.lines = it.lines; }
            }

            // A) Cells to fill: neighbours of open edges, completed to the bounding rectangle
            //    (small structures only) so the result is a clean medium-sized shape.
            std::set<CellKey> fill;
            std::set<CellKey> visited;
            std::map<CellKey, std::pair<float, int>> forced; // Shapes: cell -> (rotation, leg mask)
            if (!shapes) for (auto const& kv : body) {
                CellKey k0 = kv.first;
                if (visited.count(k0)) continue;
                std::vector<CellKey> cluster, stack = { k0 };
                visited.insert(k0);
                while (!stack.empty()) {
                    auto c = stack.back();
                    stack.pop_back();
                    cluster.push_back(c);
                    for (auto const& d : DIRS) {
                        CellKey n = { c.first + d.dx, c.second + d.dy };
                        if (body.count(n) && !visited.count(n)) { visited.insert(n); stack.push_back(n); }
                    }
                }

                std::set<CellKey> wanted;
                for (auto& c : cluster) {
                    int open = body[c].open;
                    for (auto const& d : DIRS) {
                        if (!(open & d.bit)) continue;
                        CellKey n = { c.first + d.dx, c.second + d.dy };
                        if (body.count(n)) continue;
                        if (index.occupied(worldX(n.first), worldY(n.second))) continue; // already built
                        wanted.insert(n);
                    }
                }
                if (wanted.empty()) continue;

                int minX = k0.first, maxX = k0.first, minY = k0.second, maxY = k0.second;
                auto grow = [&](CellKey const& c) {
                    minX = std::min(minX, c.first); maxX = std::max(maxX, c.first);
                    minY = std::min(minY, c.second); maxY = std::max(maxY, c.second);
                };
                for (auto& c : cluster) grow(c);
                for (auto& c : wanted) grow(c);

                if ((maxX - minX + 1) * (maxY - minY + 1) <= 64) {
                    for (int x = minX; x <= maxX; ++x) {
                        for (int y = minY; y <= maxY; ++y) {
                            if (!body.count({ x, y })) fill.insert({ x, y });
                        }
                    }
                } else {
                    for (auto& c : wanted) fill.insert(c);
                }
            }

            // A') Shapes: one chamfered blob around every group of nearby blocks/slopes.
            //     Rectangle = bounding box + random padding, then each corner is cut with a
            //     diagonal of 45 degree slopes (cut size limited so no block/slope is touched).
            if (shapes) {
                std::set<CellKey> seen;
                for (auto const& kv : body) {
                    CellKey k0 = kv.first;
                    if (seen.count(k0)) continue;
                    std::vector<CellKey> cluster, stack = { k0 };
                    seen.insert(k0);
                    while (!stack.empty()) {
                        auto c = stack.back();
                        stack.pop_back();
                        cluster.push_back(c);
                        for (int dx = -shapeLink; dx <= shapeLink; ++dx) {
                            for (int dy = -shapeLink; dy <= shapeLink; ++dy) {
                                CellKey n = { c.first + dx, c.second + dy };
                                if (body.count(n) && !seen.count(n)) { seen.insert(n); stack.push_back(n); }
                            }
                        }
                    }

                    int minX = k0.first, maxX = k0.first, minY = k0.second, maxY = k0.second;
                    for (auto& c : cluster) {
                        minX = std::min(minX, c.first); maxX = std::max(maxX, c.first);
                        minY = std::min(minY, c.second); maxY = std::max(maxY, c.second);
                    }

                    std::uniform_int_distribution<int> padDist(std::max(1, shapePad - 1), shapePad + 1);
                    int x0 = minX - padDist(rng), x1 = maxX + padDist(rng);
                    int y0 = minY - padDist(rng), y1 = maxY + padDist(rng);
                    int W = x1 - x0 + 1, H = y1 - y0 + 1;
                    if (W * H > 1600) { ++skipped; continue; }

                    struct Corner { int cx, cy, sx, sy, k; float rot; int legs; };
                    Corner corners[4] = {
                        { x0, y1, +1, -1, 0, 0.f,   BB | BR }, // top-left: legs bottom + right
                        { x1, y1, -1, -1, 0, 90.f,  BB | BL }, // top-right
                        { x1, y0, -1, +1, 0, 180.f, BT | BL }, // bottom-right
                        { x0, y0, +1, +1, 0, 270.f, BT | BR }, // bottom-left
                    };
                    for (auto& cn : corners) {
                        int minSum = 1 << 20;
                        for (auto& c : cluster) {
                            int dx = (c.first - cn.cx) * cn.sx;
                            int dy = (c.second - cn.cy) * cn.sy;
                            minSum = std::min(minSum, dx + dy);
                        }
                        int cap = std::min({ minSum, std::min(W, H) / 2, shapeCut });
                        if (cap >= 1) {
                            std::uniform_int_distribution<int> kDist(std::max(1, cap / 2), cap);
                            cn.k = kDist(rng);
                        }
                    }

                    for (int x = x0; x <= x1; ++x) {
                        for (int y = y0; y <= y1; ++y) {
                            CellKey c = { x, y };
                            if (body.count(c)) continue;
                            bool removedCell = false;
                            bool slopeCell = false;
                            float rot = 0.f;
                            int legs = 0;
                            for (auto& cn : corners) {
                                if (cn.k < 1) continue;
                                int sum = (x - cn.cx) * cn.sx + (y - cn.cy) * cn.sy;
                                if (sum <= cn.k - 2) removedCell = true;
                                else if (sum == cn.k - 1) { slopeCell = true; rot = cn.rot; legs = cn.legs; }
                            }
                            if (removedCell) continue;
                            fill.insert(c);
                            if (slopeCell) forced[c] = { rot, legs };
                        }
                    }
                }
            }

            // Fill one-cell gaps between structures that are almost touching.
            {
                std::set<CellKey> foot;
                for (auto const& kv : body) foot.insert(kv.first);
                for (auto& c : fill) foot.insert(c);
                std::vector<CellKey> gaps;
                for (auto& f : foot) {
                    for (auto const& d : DIRS) {
                        CellKey mid = { f.first + d.dx, f.second + d.dy };
                        CellKey beyond = { f.first + 2 * d.dx, f.second + 2 * d.dy };
                        if (!foot.count(mid) && foot.count(beyond)) gaps.push_back(mid);
                    }
                }
                for (auto& g : gaps) fill.insert(g);
            }

            // Never build on top of existing objects.
            for (auto it = fill.begin(); it != fill.end();) {
                if (index.occupied(worldX(it->first), worldY(it->second))) it = fill.erase(it);
                else ++it;
            }
            if (fill.empty() || fill.size() > 6000) { if (!fill.empty()) ++skipped; continue; }

            // Which edges of a cell are still exposed once everything is placed.
            auto exposureOf = [&](CellKey const& c) {
                int e = 0;
                for (auto const& d : DIRS) {
                    CellKey n = { c.first + d.dx, c.second + d.dy };
                    bool covered = false;
                    if (fill.count(n) > 0) {
                        auto fz = forced.find(n);
                        covered = fz == forced.end() || (fz->second.second & d.opp);
                    }
                    if (!covered) {
                        auto bi = body.find(n);
                        if (bi != body.end() && (bi->second.flat & d.opp)) covered = true;
                    }
                    if (!covered) covered = solidIndex.occupied(worldX(n.first), worldY(n.second));
                    if (!covered) e |= d.bit;
                }
                return e;
            };

            // B) New blocks / corner slopes.
            for (auto& c : fill) {
                int exposed = exposureOf(c);
                int id;
                float rot = 0.f;
                bool corner = exposed == (BT | BR) || exposed == (BR | BB) ||
                              exposed == (BB | BL) || exposed == (BT | BL);
                auto fz = forced.find(c);
                if (fz != forced.end()) {
                    id = ID_SLOPE45;
                    rot = fz->second.first;
                    ++slopes;
                } else if (!shapes && corner && pct(rng) <= chance) {
                    id = ID_SLOPE45;
                    rot = slopeRotationForLegs(15 & ~exposed); // legs face the neighbours
                    ++slopes;
                } else {
                    auto v = blockForExposure(exposed, partial);
                    id = v.id;
                    rot = v.rot;
                }
                auto obj = lel->createObject(id, { worldX(c.first), worldY(c.second) }, true);
                if (!obj) continue;
                if (rot != 0.f) obj->setRotation(rot);
                created->addObject(obj);
                ++placed;
            }

            // C) Existing blocks that now touch new blocks may have a line that became a
            //    seam: swap them for the variant with the right lines.
            for (auto& kv : body) {
                auto& b = kv.second;
                if (!b.block) continue;
                bool nextToFill = false;
                for (auto const& d : DIRS) {
                    if (fill.count({ kv.first.first + d.dx, kv.first.second + d.dy })) nextToFill = true;
                }
                if (!nextToFill) continue;
                int e = exposureOf(kv.first);
                if (e == b.lines) continue;
                bool p = false;
                auto v = blockForExposure(e, p);
                if (p) continue;
                auto nb = lel->createObject(v.id, { b.block->getPositionX(), b.block->getPositionY() }, true);
                if (!nb) continue;
                if (v.rot != 0.f) nb->setRotation(v.rot);
                created->addObject(nb);
                removed->addObject(b.block);
                ++fixedLines;
            }
        }

        if (created->count() == 0) {
            Notification::create("Already closed in - nothing to add", NotificationIcon::Warning)->show();
            return;
        }

        // Undo: the new blocks are one step, swapped blocks are a second step.
        lel->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        if (removed->count() > 0) {
            lel->addToUndoList(UndoObject::createWithArray(removed, UndoCommand::Delete), false);
            if (!wholeLevel) this->deselectAll();
            for (auto obj : CCArrayExt<GameObject*>(removed)) lel->removeObject(obj, true);
        }

        std::string msg = fmt::format("{} with {} blocks ({} slopes)", shapes ? "Shaped" : "Enclosed", placed - slopes, slopes);
        if (fixedLines > 0) msg += fmt::format(", fixed {} lines (Ctrl+Z twice)", fixedLines);
        if (skipped > 0) msg += fmt::format(" ({} skipped)", skipped);
        if (partial) msg += " - a lone cell needs 4 lines, left one open";
        Notification::create(msg, NotificationIcon::Success)->show();

        if (!unknownIds.empty()) {
            std::string detail = "These block IDs aren't in my table, so I treated them as already closed: ";
            int n = 0;
            for (int i : unknownIds) { if (n++) detail += ", "; detail += std::to_string(i); if (n >= 12) break; }
            alert(detail);
        }
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
        std::vector<GameObject*> pending;
        std::map<std::pair<long, long>, int> decided;
        auto addSource = [&](Source const& s) {
            std::pair<long, int> key = { std::lround(s.x * 2.f), s.dir };
            auto it = sources.find(key);
            bool better = it == sources.end() ||
                (s.dir < 0 ? s.y < it->second.y : s.y > it->second.y);
            if (better) sources[key] = s;
        };
        for (auto obj : CCArrayExt<GameObject*>(pool)) {
            bool solid = obj->m_objectType == GameObjectType::Solid;
            bool slope = obj->m_objectType == GameObjectType::Slope;
            if (!solid && !slope) continue;

            int id = obj->m_objectID;
            if (wholeLevel) {
                bool known = id == 1 || id == 2 || id == 3 || id == 6 ||
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
            if (res == SrcResult::NeedsNeighbor) { pending.push_back(obj); continue; }

            if (obj->m_objectType == GameObjectType::Solid && !list.empty()) {
                decided[{ std::lround(obj->getPositionX() * 2.f), std::lround(obj->getPositionY() * 2.f) }] = list[0].dir;
            }
            for (auto& s : list) addSource(s);
        }

        // Default blocks (ID 1) have lines on every side, so copy the direction of the
        // decided block directly beside them. Repeat so a row of them can chain.
        for (int pass = 0; pass < 4 && !pending.empty(); ++pass) {
            std::vector<GameObject*> still;
            for (auto obj : pending) {
                long px2 = std::lround(obj->getPositionX() * 2.f);
                long py2 = std::lround(obj->getPositionY() * 2.f);
                int dir = 0;
                for (long dx : { -60L, 60L }) {
                    auto it = decided.find({ px2 + dx, py2 });
                    if (it != decided.end()) { dir = it->second; break; }
                }
                if (dir == 0) { still.push_back(obj); continue; }
                decided[{ px2, py2 }] = dir;
                addSource({ obj->getPositionX(),
                            obj->getPositionY() + static_cast<float>(dir) * GRID, dir });
            }
            if (still.size() == pending.size()) break;
            pending = std::move(still);
        }
        skipped += static_cast<int>(pending.size());

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

        NeighborIndex nbr;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) nbr.add(obj);

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
                bool left = hasCell(p.x - GRID, y) || nbr.covered(p.x - GRID, y, true);
                bool right = hasCell(p.x + GRID, y) || nbr.covered(p.x + GRID, y, false);

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
