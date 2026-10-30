/**
 * The Pipkin Hollow - a Minish side story for Ship of Harkinian (needs Minish.cpp)
 * --------------------------------------------------------------------------------
 * After Ganon falls, hollow stumps sprout around Hyrule. Each has a tiny door at its base. Shrink, walk in, and you
 * find Pipkin Hollow: the hidden village of the Pipkins, a tiny folk living in the roots of Hyrule.
 *
 * Chapter 1:
 *  - Village: Elder Burdock, Tansy the gardener, Burl the beetle keeper, Moss the gate guard, and little Pip.
 *  - Quests: Tansy's five lost seedlings (reward: Dandelion Glide), find Pip who wandered out to Lake Hylia (reward:
 *    travel between stumps), bring Burl a bug in a bottle (reward: rupees).
 *  - The Rootworks: a dungeon where you have to switch sizes. Tiny tunnels, heavy switches only big Link can press,
 *    a chasm to glide across, a den of Gloomgrubs and the boss, the Gloomhorn beetle. Prize: the Heartseed.
 *
 * How it's built (no other game files change):
 *  - Stumps, the village and the dungeon are custom collision + vertex-colored models built in code (like the
 *    Skateboard mod's skate park). The village and dungeon float in an empty "pocket" of whatever area you entered
 *    from, high above the ground, closed in by their own walls and ceilings, so no new scenes are needed.
 *  - Pipkins, Gloomgrubs and the Gloomhorn are new actors with models built from simple shapes.
 *  - Dialogue uses Ship of Harkinian's custom text boxes. Progress is stored in the save file.
 */

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Enhancements/gameplaystats.h"
#include "soh/ShipInit.hpp"
#include "soh/SaveManager.h"
#include "soh/frame_interpolation.h" // OPEN_DISPS / CLOSE_DISPS need these declared with C linkage
#include "soh/SohGui/MenuTypes.h"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/SohGui.hpp"
#include "soh/Enhancements/nametag.h"
#include "soh/ActorDB.h"
#include "soh/util.h"

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
}

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// ---------------------------------------------------------------------------------------------------------------------
// The core Minish mod (Minish.cpp)
// ---------------------------------------------------------------------------------------------------------------------

typedef bool (*MinishGrowHandler)(PlayState* play, Player* player);
void Minish_SetGrowHandler(MinishGrowHandler handler);
bool Minish_IsEnabled();
bool Minish_IsTiny();
bool Minish_IsNormalSize();
bool Minish_IsChanging();
void Minish_SetTinyNow(Player* player);
bool Minish_StartGrowing(PlayState* play, Player* player);
void Minish_ShowMessage(const char* text);

namespace PipkinStory {

// OPEN_DISPS / CLOSE_DISPS declare these inside the function they're used in, which (in a namespace) would mean a
// different, C++ function. Declaring them with C linkage here makes them the game's functions.
extern "C" void FrameInterpolation_RecordOpenChild(const void* a, int b);
extern "C" void FrameInterpolation_RecordCloseChild(void);

// ---------------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------------

#define CVAR_STORY(name) CVAR_ENHANCEMENT("Minish.Story." name)

static bool Cfg_Story() {
    return CVarGetInteger(CVAR_STORY("Enabled"), 1) != 0;
}
static bool Cfg_Tracker() {
    return CVarGetInteger(CVAR_STORY("Tracker"), 1) != 0;
}

static bool StoryOn() {
    return Minish_IsEnabled() && Cfg_Story();
}

constexpr f32 kPi = 3.14159265f;
constexpr u16 kTextNpcBase = 0x9C40; // + NPC id: what each Pipkin says
constexpr u16 kTextSystem = 0x9C4F;  // story messages and menus
constexpr s32 kNumStumps = 5;

// ---------------------------------------------------------------------------------------------------------------------
// Save data (stored in the save file)
// ---------------------------------------------------------------------------------------------------------------------

enum QuestBit : u32 {
    Q_UNLOCKED = 1u << 0,    // beat Ganon (or unlocked from the menu)
    Q_INTRO = 1u << 1,       // saw the "stumps have sprouted" message
    Q_MET_ELDER = 1u << 2,   // heard the Elder's story
    Q_SEEDS_START = 1u << 3, // Tansy asked for her seedlings
    Q_SEEDS_DONE = 1u << 4,  // ...and taught you the Dandelion Glide
    Q_PIP_START = 1u << 5,   // Moss asked you to find Pip
    Q_PIP_FOUND = 1u << 6,   // you found Pip at Lake Hylia, he went home
    Q_PIP_DONE = 1u << 7,    // Moss gave you the Rootway Charm (stump travel)
    Q_BUG_START = 1u << 8,   // Burl wants a bug
    Q_BUG_DONE = 1u << 9,    // ...and got one
    Q_GATE_OPEN = 1u << 10,  // the Elder opened the Rootworks gate
    Q_SWITCH = 1u << 11,     // Rootworks: the heavy switch is down (big gate open)
    Q_GRUBS = 1u << 12,      // Rootworks: the Gloomgrub den is cleared
    Q_BOSS = 1u << 13,       // Rootworks: the Gloomhorn is defeated
    Q_HEARTSEED = 1u << 14,  // picked up the Heartseed
    Q_REWARD = 1u << 15,     // the Elder gave the final reward (chapter complete)
    Q_RW_SEEN = 1u << 16,    // first visit to the Rootworks
    Q_BOSS_SEEN = 1u << 17,  // boss intro shown
};

struct StorySave {
    u32 quests = 0;
    u8 seeds = 0;       // Tansy's seedlings collected (5 bits)
    u8 stumpPlaced = 0; // which stumps have found a spot
    u8 stumpFound = 0;  // which stumps you've walked into
    s16 stumpX[kNumStumps] = {};
    s16 stumpY[kNumStumps] = {};
    s16 stumpZ[kNumStumps] = {};
    s16 stumpYaw[kNumStumps] = {};
};
static StorySave sSave;

static bool Has(u32 bit) {
    return (sSave.quests & bit) != 0;
}
static void Set(u32 bit) {
    sSave.quests |= bit;
}

static s32 SeedCount() {
    s32 n = 0;
    for (s32 i = 0; i < 5; i++) {
        n += (sSave.seeds >> i) & 1;
    }
    return n;
}

static void SaveStory() {
    SaveManager::Instance->SaveData("quests", sSave.quests);
    SaveManager::Instance->SaveData("seeds", sSave.seeds);
    SaveManager::Instance->SaveData("stumpPlaced", sSave.stumpPlaced);
    SaveManager::Instance->SaveData("stumpFound", sSave.stumpFound);
    SaveManager::Instance->SaveArray("stumpX", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sSave.stumpX[i]); });
    SaveManager::Instance->SaveArray("stumpY", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sSave.stumpY[i]); });
    SaveManager::Instance->SaveArray("stumpZ", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sSave.stumpZ[i]); });
    SaveManager::Instance->SaveArray("stumpYaw", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->SaveData("", sSave.stumpYaw[i]); });
}

static void LoadStory() {
    sSave = StorySave();
    SaveManager::Instance->LoadData("quests", sSave.quests);
    SaveManager::Instance->LoadData("seeds", sSave.seeds);
    SaveManager::Instance->LoadData("stumpPlaced", sSave.stumpPlaced);
    SaveManager::Instance->LoadData("stumpFound", sSave.stumpFound);
    SaveManager::Instance->LoadArray("stumpX", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sSave.stumpX[i]); });
    SaveManager::Instance->LoadArray("stumpY", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sSave.stumpY[i]); });
    SaveManager::Instance->LoadArray("stumpZ", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sSave.stumpZ[i]); });
    SaveManager::Instance->LoadArray("stumpYaw", kNumStumps,
                                     [](size_t i) { SaveManager::Instance->LoadData("", sSave.stumpYaw[i]); });
}

// The save callback's first parameter differs between Ship of Harkinian versions; this adapts to either.
template <typename F> struct SaveCallback;
template <typename Ctx> struct SaveCallback<void (*)(Ctx, int, bool)> {
    static void Fn(Ctx, int, bool) {
        SaveStory();
    }
};

static void InitStory(bool) {
    sSave = StorySave(); // new file, or a file from before the mod: nothing done yet
}

static void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// The game doesn't save after the final battle (it rolls the credits), so beating Ganon is remembered in the
// settings, keyed to this save file's creation time.
static std::string UnlockKey() {
    return std::string(CVAR_STORY("Unlocked.")) + std::to_string(gSaveContext.ship.stats.fileCreatedAt);
}

static bool StoryUnlocked() {
    if (Has(Q_UNLOCKED)) {
        return true;
    }
    if (gSaveContext.ship.stats.itemTimestamp[TIMESTAMP_DEFEAT_GANON] != 0 ||
        CVarGetInteger(UnlockKey().c_str(), 0) != 0) {
        Set(Q_UNLOCKED);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------------

static Vec3f V3(f32 x, f32 y, f32 z) {
    Vec3f v = { x, y, z };
    return v;
}
static Vec3f Add(const Vec3f& a, const Vec3f& b) {
    return V3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static Vec3f Sub(const Vec3f& a, const Vec3f& b) {
    return V3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static Vec3f Scale(const Vec3f& a, f32 s) {
    return V3(a.x * s, a.y * s, a.z * s);
}
static f32 Dot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec3f Cross(const Vec3f& a, const Vec3f& b) {
    return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static f32 DistXZ(const Vec3f& a, const Vec3f& b) {
    f32 dx = a.x - b.x;
    f32 dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}
// Rotate a local offset by a yaw (binary angle) around Y
static Vec3f RotY(const Vec3f& v, s16 yaw) {
    f32 s = Math_SinS(yaw);
    f32 c = Math_CosS(yaw);
    return V3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

static f32 FloorAt(PlayState* play, f32 x, f32 y, f32 z, CollisionPoly** outPoly = nullptr, s32* outBgId = nullptr) {
    Vec3f pos = { x, y, z };
    CollisionPoly* poly = nullptr;
    s32 bgId = BGCHECK_SCENE;
    f32 h = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &pos);
    if (outPoly != nullptr) {
        *outPoly = poly;
    }
    if (outBgId != nullptr) {
        *outBgId = bgId;
    }
    return h;
}

static void PlaySysSfx(u16 sfx) {
    Sfx_PlaySfxCentered(sfx);
}

// HUD line (separate from the core mod's line)
static std::string sHudMsg;
static s16 sHudTimer = 0;
static void Hud(const std::string& text, s16 frames = 50) {
    sHudMsg = text;
    sHudTimer = frames;
}

// ---------------------------------------------------------------------------------------------------------------------
// Text boxes
// ---------------------------------------------------------------------------------------------------------------------
// Every Pipkin talks through text id kTextNpcBase + its id; story messages and menus use kTextSystem. When the game
// opens one of those ids, OnOpenText fills the box with the text we picked.

enum Script {
    SCR_NONE,
    // Elder Burdock
    SCR_ELDER_INTRO,
    SCR_ELDER_PROGRESS,
    SCR_ELDER_OPEN_GATE,
    SCR_ELDER_GATE_HINT,
    SCR_ELDER_REWARD,
    SCR_ELDER_AFTER,
    // Tansy
    SCR_TANSY_START,
    SCR_TANSY_PROGRESS,
    SCR_TANSY_DONE,
    SCR_TANSY_AFTER,
    // Moss
    SCR_MOSS_WAIT_ELDER,
    SCR_MOSS_START,
    SCR_MOSS_WAIT_PIP,
    SCR_MOSS_PIP_HOME,
    SCR_MOSS_GUARD,
    SCR_MOSS_GATE_OPEN,
    // Pip
    SCR_PIP_LOST,
    SCR_PIP_HOME,
    // Burl
    SCR_BURL_START,
    SCR_BURL_ASK,
    SCR_BURL_THANKS,
    SCR_BURL_NO,
    SCR_BURL_NO_BUG,
    SCR_BURL_AFTER,
    // System
    SCR_SYS_INTRO,
    SCR_SYS_LEAVE_HOLLOW,
    SCR_SYS_TRAVEL,
    SCR_SYS_LEAVE_RW,
    SCR_SYS_RW_INTRO,
    SCR_SYS_BOSS_INTRO,
    SCR_SYS_HEARTSEED,
    SCR_SYS_NOT_YET,
};

struct Conversation {
    bool active = false;
    s32 npc = -1; // -1 = system message
    Script script = SCR_NONE;
    bool choiceOpen = false;
    s32 travelPick = 0; // stump shown in the travel menu
};
static Conversation sConv;
static std::string sBoxText;

static void BuildBox(bool* loadFromMessageTable) {
    CustomMessage msg(sBoxText, sBoxText, sBoxText);
    msg.AutoFormat();
    msg.LoadIntoFont();
    *loadFromMessageTable = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mesh builder: collision + vertex-colored triangles
// ---------------------------------------------------------------------------------------------------------------------

struct Col {
    u8 r, g, b;
};

enum MeshFlags : u8 {
    MF_DRAW = 1 << 0,    // visible
    MF_COLLIDE = 1 << 1, // solid
    MF_GLOW = 1 << 2,    // not shaded (glowing)
};
constexpr u8 MF_SOLID = MF_DRAW | MF_COLLIDE;

enum Surface { SURF_DIRT, SURF_GRASS, SURF_STONE, SURF_WOOD, SURF_COUNT };

class Mesh {
  public:
    std::vector<Vec3s> colVtx;
    std::vector<CollisionPoly> colPoly;
    std::vector<Vtx> vtx;
    std::vector<Gfx> gfx;
    CollisionHeader header;
    SurfaceType surfaces[SURF_COUNT] = { { { 0, 3 } }, { { 0, 8 } }, { { 0, 2 } }, { { 0, 10 } } };
    Vec3s camPos[3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    CamData camData[1] = { { CAM_SET_NONE, 0, camPos } };
    bool built = false;

    void Clear() {
        colVtx.clear();
        colPoly.clear();
        vtx.clear();
        gfx.clear();
        mVtxIndex.clear();
        built = false;
    }

    // `out` points away from the solid side, so collision faces the right way
    void Tri(Vec3f a, Vec3f b, Vec3f c, Col color, Vec3f out, u8 flags = MF_SOLID, s32 surface = SURF_DIRT) {
        Vec3f n = Cross(Sub(b, a), Sub(c, a));
        if (Dot(n, out) < 0.0f) {
            Vec3f t = b;
            b = c;
            c = t;
            n = V3(-n.x, -n.y, -n.z);
        }
        f32 len = sqrtf(Dot(n, n));
        if (len < 0.0001f) {
            return;
        }
        n = Scale(n, 1.0f / len);

        if (flags & MF_COLLIDE) {
            u16 idx[3] = { ColVertex(a), ColVertex(b), ColVertex(c) };
            CollisionPoly poly;
            memset(&poly, 0, sizeof(poly));
            poly.type = static_cast<u16>(surface);
            poly.flags_vIA = idx[0];
            poly.flags_vIB = idx[1];
            poly.vIC = idx[2];
            poly.normal.x = static_cast<s16>(n.x * 32767.0f);
            poly.normal.y = static_cast<s16>(n.y * 32767.0f);
            poly.normal.z = static_cast<s16>(n.z * 32767.0f);
            poly.dist = static_cast<s16>(lroundf(-Dot(n, a)));
            colPoly.push_back(poly);
        }

        if (flags & MF_DRAW) {
            f32 shade = 1.0f;
            if (!(flags & MF_GLOW)) {
                Vec3f light = V3(0.35f, 0.85f, 0.4f);
                shade = 0.5f + 0.5f * std::max(0.0f, Dot(n, light) / sqrtf(Dot(light, light)));
            }
            for (const Vec3f* p : { &a, &b, &c }) {
                Vtx v;
                memset(&v, 0, sizeof(v));
                v.v.ob[0] = static_cast<s16>(lroundf(p->x));
                v.v.ob[1] = static_cast<s16>(lroundf(p->y));
                v.v.ob[2] = static_cast<s16>(lroundf(p->z));
                v.v.cn[0] = static_cast<u8>(std::min(255.0f, static_cast<f32>(color.r) * shade));
                v.v.cn[1] = static_cast<u8>(std::min(255.0f, static_cast<f32>(color.g) * shade));
                v.v.cn[2] = static_cast<u8>(std::min(255.0f, static_cast<f32>(color.b) * shade));
                v.v.cn[3] = 255;
                vtx.push_back(v);
            }
        }
    }

    void Quad(Vec3f a, Vec3f b, Vec3f c, Vec3f d, Col color, Vec3f out, u8 flags = MF_SOLID, s32 surface = SURF_DIRT) {
        Tri(a, b, c, color, out, flags, surface);
        Tri(a, c, d, color, out, flags, surface);
    }

    // Axis-aligned box. `faces`: bit 0 top, 1 bottom, 2 -z, 3 +z, 4 -x, 5 +x
    void Box(f32 x0, f32 x1, f32 y0, f32 y1, f32 z0, f32 z1, Col top, Col side, u8 flags = MF_SOLID,
             s32 surface = SURF_DIRT, u8 faces = 0x3D) {
        if (faces & 0x01) {
            Quad(V3(x0, y1, z0), V3(x1, y1, z0), V3(x1, y1, z1), V3(x0, y1, z1), top, V3(0, 1, 0), flags, surface);
        }
        if (faces & 0x02) {
            Quad(V3(x0, y0, z0), V3(x1, y0, z0), V3(x1, y0, z1), V3(x0, y0, z1), side, V3(0, -1, 0), flags, surface);
        }
        if (faces & 0x04) {
            Quad(V3(x0, y0, z0), V3(x1, y0, z0), V3(x1, y1, z0), V3(x0, y1, z0), side, V3(0, 0, -1), flags, surface);
        }
        if (faces & 0x08) {
            Quad(V3(x0, y0, z1), V3(x1, y0, z1), V3(x1, y1, z1), V3(x0, y1, z1), side, V3(0, 0, 1), flags, surface);
        }
        if (faces & 0x10) {
            Quad(V3(x0, y0, z0), V3(x0, y0, z1), V3(x0, y1, z1), V3(x0, y1, z0), side, V3(-1, 0, 0), flags, surface);
        }
        if (faces & 0x20) {
            Quad(V3(x1, y0, z0), V3(x1, y0, z1), V3(x1, y1, z1), V3(x1, y1, z0), side, V3(1, 0, 0), flags, surface);
        }
    }

    // Vertical cylinder. inward = the solid is outside (a round room's wall)
    void Cylinder(f32 cx, f32 cz, f32 r, f32 y0, f32 y1, s32 sides, Col side, Col top, u8 flags = MF_SOLID,
                  bool inward = false, bool capTop = true, s32 surface = SURF_WOOD) {
        for (s32 i = 0; i < sides; i++) {
            f32 a0 = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(sides);
            f32 a1 = 2.0f * kPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
            f32 am = (a0 + a1) * 0.5f;
            Vec3f p0 = V3(cx + r * sinf(a0), 0.0f, cz + r * cosf(a0));
            Vec3f p1 = V3(cx + r * sinf(a1), 0.0f, cz + r * cosf(a1));
            Vec3f out = V3(sinf(am), 0.0f, cosf(am));
            if (inward) {
                out = Scale(out, -1.0f);
            }
            Quad(V3(p0.x, y0, p0.z), V3(p1.x, y0, p1.z), V3(p1.x, y1, p1.z), V3(p0.x, y1, p0.z), side, out, flags,
                 surface);
            if (capTop) {
                Tri(V3(cx, y1, cz), V3(p0.x, y1, p0.z), V3(p1.x, y1, p1.z), top, V3(0, 1, 0), flags, surface);
            }
        }
    }

    // Flat disc (floor)
    void Disc(f32 cx, f32 cz, f32 r, f32 y, s32 sides, Col color, u8 flags = MF_SOLID, s32 surface = SURF_GRASS,
              bool down = false) {
        for (s32 i = 0; i < sides; i++) {
            f32 a0 = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(sides);
            f32 a1 = 2.0f * kPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
            Tri(V3(cx, y, cz), V3(cx + r * sinf(a0), y, cz + r * cosf(a0)), V3(cx + r * sinf(a1), y, cz + r * cosf(a1)),
                color, V3(0, down ? -1.0f : 1.0f, 0), flags, surface);
        }
    }

    // Flat ring on the floor (a glowing circle)
    void Ring(f32 cx, f32 cz, f32 r0, f32 r1, f32 y, s32 sides, Col color) {
        for (s32 i = 0; i < sides; i++) {
            f32 a0 = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(sides);
            f32 a1 = 2.0f * kPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
            Quad(V3(cx + r0 * sinf(a0), y, cz + r0 * cosf(a0)), V3(cx + r1 * sinf(a0), y, cz + r1 * cosf(a0)),
                 V3(cx + r1 * sinf(a1), y, cz + r1 * cosf(a1)), V3(cx + r0 * sinf(a1), y, cz + r0 * cosf(a1)), color,
                 V3(0, 1, 0), MF_DRAW | MF_GLOW);
        }
    }

    // Ellipsoid (visual only)
    void Ellipsoid(Vec3f c, f32 rx, f32 ry, f32 rz, Col color, s32 lat = 6, s32 lon = 10, bool glow = false) {
        u8 flags = MF_DRAW | (glow ? MF_GLOW : 0);
        for (s32 i = 0; i < lat; i++) {
            f32 t0 = kPi * static_cast<f32>(i) / static_cast<f32>(lat) - kPi * 0.5f;
            f32 t1 = kPi * static_cast<f32>(i + 1) / static_cast<f32>(lat) - kPi * 0.5f;
            for (s32 j = 0; j < lon; j++) {
                f32 p0 = 2.0f * kPi * static_cast<f32>(j) / static_cast<f32>(lon);
                f32 p1 = 2.0f * kPi * static_cast<f32>(j + 1) / static_cast<f32>(lon);
                Vec3f u00 = V3(cosf(t0) * sinf(p0), sinf(t0), cosf(t0) * cosf(p0));
                Vec3f u01 = V3(cosf(t0) * sinf(p1), sinf(t0), cosf(t0) * cosf(p1));
                Vec3f u10 = V3(cosf(t1) * sinf(p0), sinf(t1), cosf(t1) * cosf(p0));
                Vec3f u11 = V3(cosf(t1) * sinf(p1), sinf(t1), cosf(t1) * cosf(p1));
                auto P = [&](const Vec3f& u) { return V3(c.x + u.x * rx, c.y + u.y * ry, c.z + u.z * rz); };
                Vec3f mid = Scale(Add(Add(u00, u01), Add(u10, u11)), 0.25f);
                if (i > 0) {
                    Tri(P(u00), P(u01), P(u11), color, mid, flags);
                }
                if (i < lat - 1) {
                    Tri(P(u00), P(u11), P(u10), color, mid, flags);
                }
            }
        }
    }

    // Cone / tapered tube between two points (visual only), for horns, stems and roots
    void Tube(Vec3f a, Vec3f b, f32 ra, f32 rb, Col color, s32 sides = 6, bool glow = false) {
        Vec3f axis = Sub(b, a);
        f32 len = sqrtf(Dot(axis, axis));
        if (len < 0.001f) {
            return;
        }
        axis = Scale(axis, 1.0f / len);
        Vec3f ref = (fabsf(axis.y) < 0.9f) ? V3(0, 1, 0) : V3(1, 0, 0);
        Vec3f u = Cross(axis, ref);
        u = Scale(u, 1.0f / sqrtf(Dot(u, u)));
        Vec3f w = Cross(axis, u);
        u8 flags = MF_DRAW | (glow ? MF_GLOW : 0);
        for (s32 i = 0; i < sides; i++) {
            f32 a0 = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(sides);
            f32 a1 = 2.0f * kPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
            Vec3f d0 = Add(Scale(u, cosf(a0)), Scale(w, sinf(a0)));
            Vec3f d1 = Add(Scale(u, cosf(a1)), Scale(w, sinf(a1)));
            Vec3f out = Add(d0, d1);
            Quad(Add(a, Scale(d0, ra)), Add(a, Scale(d1, ra)), Add(b, Scale(d1, rb)), Add(b, Scale(d0, rb)), color, out,
                 flags);
        }
    }

    // Collision header + display list. Call once, after everything is added.
    void Finalize() {
        memset(&header, 0, sizeof(header));
        Vec3s mn = { 32767, 32767, 32767 };
        Vec3s mx = { -32767, -32767, -32767 };
        for (const Vec3s& v : colVtx) {
            mn.x = std::min(mn.x, v.x);
            mn.y = std::min(mn.y, v.y);
            mn.z = std::min(mn.z, v.z);
            mx.x = std::max(mx.x, v.x);
            mx.y = std::max(mx.y, v.y);
            mx.z = std::max(mx.z, v.z);
        }
        if (colVtx.empty()) {
            mn = { 0, 0, 0 };
            mx = { 0, 0, 0 };
        }
        header.minBounds = mn;
        header.maxBounds = mx;
        header.numVertices = static_cast<u16>(colVtx.size());
        header.vtxList = colVtx.data();
        header.numPolygons = static_cast<u16>(colPoly.size());
        header.polyList = colPoly.data();
        header.surfaceTypeList = surfaces;
        header.cameraDataList = camData;
        header.cameraDataListLen = 1;
        header.numWaterBoxes = 0;
        header.waterBoxes = NULL;

        gfx.clear();
        const size_t kChunk = 30; // 10 triangles per vertex load
        for (size_t start = 0; start < vtx.size(); start += kChunk) {
            size_t count = std::min(kChunk, vtx.size() - start);
            gfx.push_back(gsSPVertex((uintptr_t)(vtx.data() + start), static_cast<u32>(count), 0));
            for (size_t t = 0; t + 2 < count; t += 3) {
                gfx.push_back(gsSP1Triangle(static_cast<u8>(t), static_cast<u8>(t + 1), static_cast<u8>(t + 2), 0));
            }
        }
        gfx.push_back(gsSPEndDisplayList());
        built = true;
    }

  private:
    std::map<u64, u16> mVtxIndex;

    u16 ColVertex(const Vec3f& p) {
        Vec3s s = { static_cast<s16>(lroundf(p.x)), static_cast<s16>(lroundf(p.y)), static_cast<s16>(lroundf(p.z)) };
        u64 key = (static_cast<u64>(static_cast<u16>(s.x)) << 32) | (static_cast<u64>(static_cast<u16>(s.y)) << 16) |
                  static_cast<u64>(static_cast<u16>(s.z));
        auto it = mVtxIndex.find(key);
        if (it != mVtxIndex.end()) {
            return it->second;
        }
        u16 idx = static_cast<u16>(colVtx.size());
        colVtx.push_back(s);
        mVtxIndex[key] = idx;
        return idx;
    }
};

// Shared draw setup: vertex colors multiplied by the primitive color (white normally, red when hit)
static Gfx sSetupDL[8];
static bool sSetupBuilt = false;

static void BuildSetupDL() {
    if (sSetupBuilt) {
        return;
    }
    u32 rm = Z_CMP | Z_UPD | CVG_DST_CLAMP | FORCE_BL | ZMODE_OPA;
    u32 blc1 = GBL_c1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1);
    u32 blc2 = GBL_c2(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1);
    Gfx setup[] = {
        gsDPPipeSync(),
        gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_OFF),
        gsDPSetCycleType(G_CYC_1CYCLE),
        gsDPSetRenderMode(rm | blc1, rm | blc2),
        gsDPSetCombineLERP(SHADE, 0, PRIMITIVE, 0, 0, 0, 0, 1, SHADE, 0, PRIMITIVE, 0, 0, 0, 0, 1),
        gsSPLoadGeometryMode(G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH),
        gsSPEndDisplayList(),
    };
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        sSetupDL[i] = setup[i];
    }
    sSetupBuilt = true;
}

// Draws a mesh with the current matrix
static void DrawMesh(PlayState* play, const Mesh& mesh, u8 r = 255, u8 g = 255, u8 b = 255) {
    if (!mesh.built || mesh.gfx.size() <= 1) {
        return;
    }
    BuildSetupDL();
    OPEN_DISPS(play->state.gfxCtx);
    gSPDisplayList(POLY_OPA_DISP++, sSetupDL);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, r, g, b, 255);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, const_cast<Gfx*>(mesh.gfx.data()));
    CLOSE_DISPS(play->state.gfxCtx);
}

// ---------------------------------------------------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------------------------------------------------

enum NpcId { NPC_ELDER, NPC_TANSY, NPC_MOSS, NPC_PIP, NPC_BURL, NPC_COUNT };

static const char* const kNpcNames[NPC_COUNT] = { "Elder Burdock", "Tansy", "Moss", "Pip", "Burl" };

struct PipkinLook {
    Col body;
    Col belly;
    Col sprout;
    f32 size;
};

static const PipkinLook kLooks[NPC_COUNT] = {
    { { 150, 140, 120 }, { 205, 195, 170 }, { 90, 150, 70 }, 1.05f }, // Elder: weathered clay
    { { 110, 175, 95 }, { 190, 225, 160 }, { 70, 160, 60 }, 1.0f },   // Tansy: leaf green
    { { 55, 105, 70 }, { 120, 165, 120 }, { 45, 120, 55 }, 1.1f },    // Moss: deep green
    { { 235, 200, 85 }, { 250, 235, 160 }, { 110, 185, 70 }, 0.75f }, // Pip: sunny yellow
    { { 150, 95, 55 }, { 205, 160, 115 }, { 95, 140, 60 }, 1.15f },   // Burl: chestnut
};

// Pipkins are round, seed-shaped little folk with a sprout growing out of their heads.
// Model units: about 600 tall, facing +z. Body and sprout are separate so the sprout can sway.
static Mesh sPipkinBody[NPC_COUNT];
static Mesh sPipkinSprout[NPC_COUNT];

static void BuildPipkin(s32 id) {
    Mesh& m = sPipkinBody[id];
    Mesh& s = sPipkinSprout[id];
    m.Clear();
    s.Clear();
    const PipkinLook& look = kLooks[id];
    Col dark = { static_cast<u8>(look.body.r * 0.7f), static_cast<u8>(look.body.g * 0.7f),
                 static_cast<u8>(look.body.b * 0.7f) };
    Col eye = { 25, 20, 30 };
    Col white = { 255, 255, 255 };
    Col cheek = { 245, 140, 150 };

    m.Ellipsoid(V3(0, 260, 0), 220, 260, 200, look.body, 7, 12);
    m.Ellipsoid(V3(0, 215, 85), 150, 165, 125, look.belly, 6, 10);
    for (f32 sx : { -1.0f, 1.0f }) {
        m.Ellipsoid(V3(sx * 75, 335, 170), 32, 46, 24, eye, 5, 8);
        m.Ellipsoid(V3(sx * 64, 352, 190), 10, 12, 6, white, 3, 6, true);
        m.Ellipsoid(V3(sx * 130, 282, 145), 36, 22, 16, cheek, 4, 8);
        m.Ellipsoid(V3(sx * 90, 25, 60), 70, 30, 90, dark, 4, 8);        // feet
        m.Ellipsoid(V3(sx * 210, 230, 20), 45, 70, 45, look.body, 4, 8); // arms
    }
    m.Ellipsoid(V3(0, 268, 196), 26, 10, 8, eye, 3, 6); // mouth

    if (id == NPC_ELDER) {
        m.Ellipsoid(V3(0, 195, 150), 135, 115, 75, Col{ 240, 240, 230 }, 5, 10); // beard
        for (f32 sx : { -1.0f, 1.0f }) {
            m.Ellipsoid(V3(sx * 85, 395, 165), 50, 14, 18, Col{ 235, 235, 225 }, 3, 6); // bushy brows
        }
    }
    if (id == NPC_MOSS) {
        m.Ellipsoid(V3(-245, 240, 70), 28, 165, 115, Col{ 60, 140, 60 }, 5, 8); // leaf shield
        m.Tube(V3(240, 120, 40), V3(250, 560, 60), 14, 10, Col{ 120, 85, 50 }); // twig spear
        m.Ellipsoid(V3(250, 590, 60), 26, 50, 26, Col{ 200, 200, 210 }, 4, 6);  // thorn tip
    }
    if (id == NPC_BURL) {
        m.Ellipsoid(V3(0, 120, 120), 200, 60, 90, Col{ 120, 80, 50 }, 4, 10); // apron
    }

    // Sprout (origin at the top of the head)
    Col stem = { 80, 140, 60 };
    s.Tube(V3(0, 0, 0), V3(0, 150, 0), 18, 12, stem);
    if (id == NPC_BURL) {
        // two curly feelers instead of leaves
        for (f32 sx : { -1.0f, 1.0f }) {
            s.Tube(V3(0, 100, 0), V3(sx * 110, 210, 20), 12, 8, stem);
            s.Ellipsoid(V3(sx * 115, 220, 20), 30, 30, 30, look.sprout, 4, 6);
        }
    } else {
        for (f32 sx : { -1.0f, 1.0f }) {
            s.Ellipsoid(V3(sx * 80, 160, 0), 85, 18, 42, look.sprout, 4, 8);
        }
        if (id == NPC_ELDER) {
            s.Ellipsoid(V3(0, 175, -85), 42, 18, 85, look.sprout, 4, 8);
        }
        if (id == NPC_TANSY) {
            for (s32 i = 0; i < 5; i++) {
                f32 a = 2.0f * kPi * static_cast<f32>(i) / 5.0f;
                s.Ellipsoid(V3(45 * sinf(a), 200, 45 * cosf(a)), 40, 16, 40, Col{ 250, 150, 200 }, 3, 6);
            }
            s.Ellipsoid(V3(0, 205, 0), 25, 18, 25, Col{ 255, 220, 80 }, 3, 6, true);
        }
    }
    m.Finalize();
    s.Finalize();
}

// Gloomgrub: a fat, segmented grub with glowing gloom spots. Model units, ~1500 long, facing +z.
static Mesh sGrubMesh;
static void BuildGrub() {
    Mesh& m = sGrubMesh;
    m.Clear();
    Col body = { 95, 55, 120 };
    Col bodyDark = { 70, 40, 90 };
    Col glow = { 120, 255, 140 };
    Col eye = { 255, 60, 60 };
    m.Ellipsoid(V3(0, 300, 450), 300, 280, 300, body, 6, 10);
    m.Ellipsoid(V3(0, 260, 0), 320, 260, 330, bodyDark, 6, 10);
    m.Ellipsoid(V3(0, 220, -420), 260, 220, 280, body, 6, 10);
    m.Ellipsoid(V3(0, 180, -760), 180, 160, 200, bodyDark, 5, 8);
    for (f32 sx : { -1.0f, 1.0f }) {
        m.Ellipsoid(V3(sx * 130, 390, 690), 45, 45, 30, eye, 3, 6, true);
        m.Ellipsoid(V3(sx * 120, 230, 720), 50, 40, 120, Col{ 40, 25, 30 }, 3, 6); // mandibles
        m.Ellipsoid(V3(sx * 160, 470, 60), 60, 30, 60, glow, 3, 6, true);
        m.Ellipsoid(V3(sx * 120, 420, -400), 50, 25, 50, glow, 3, 6, true);
    }
    m.Finalize();
}

// Gloomhorn: the Rootworks boss, a huge horned beetle. Model units (x10), ~1400 long, facing +z.
static Mesh sBeetleBody;
static Mesh sBeetleBelly;
static Mesh sBeetleLeg;
static void BuildBeetle() {
    sBeetleBody.Clear();
    sBeetleBelly.Clear();
    sBeetleLeg.Clear();
    Col shell = { 70, 40, 95 };
    Col shellDark = { 45, 25, 65 };
    Col glow = { 120, 255, 140 };
    Col eye = { 255, 70, 60 };
    Mesh& m = sBeetleBody;
    m.Ellipsoid(V3(0, 450, -50), 520, 300, 720, shell, 7, 14);
    m.Box(-12, 12, 640, 760, -700, 600, shellDark, shellDark, MF_DRAW); // shell seam
    m.Ellipsoid(V3(0, 380, 740), 300, 230, 250, shellDark, 6, 10);      // head
    m.Ellipsoid(V3(0, 480, 980), 85, 85, 190, shell, 5, 8);             // horn
    m.Ellipsoid(V3(0, 610, 1130), 65, 75, 130, shell, 5, 8);
    m.Ellipsoid(V3(0, 730, 1210), 45, 65, 75, Col{ 200, 190, 210 }, 4, 6);
    for (f32 sx : { -1.0f, 1.0f }) {
        m.Ellipsoid(V3(sx * 170, 470, 930), 50, 50, 35, eye, 4, 6, true);
        m.Ellipsoid(V3(sx * 150, 280, 960), 60, 50, 170, Col{ 40, 25, 30 }, 4, 6); // mandibles
        m.Ellipsoid(V3(sx * 260, 690, -120), 80, 30, 110, glow, 3, 6, true);
        m.Ellipsoid(V3(sx * 210, 680, 300), 60, 25, 80, glow, 3, 6, true);
    }
    m.Ellipsoid(V3(0, 745, -420), 70, 25, 90, glow, 3, 6, true);
    m.Finalize();

    // Pale underside: shows when it's flipped on its back
    sBeetleBelly.Ellipsoid(V3(0, 300, -50), 470, 160, 660, Col{ 225, 205, 150 }, 5, 12);
    sBeetleBelly.Finalize();

    // One leg, hinged at the origin, pointing along +x
    sBeetleLeg.Tube(V3(0, 0, 0), V3(260, 120, 0), 55, 40, shellDark);
    sBeetleLeg.Tube(V3(260, 120, 0), V3(430, -260, 0), 40, 22, shellDark);
    sBeetleLeg.Finalize();
}

// Tansy's glowing seedlings and the Heartseed (model units, ~100 = 1 world unit)
static Mesh sSeedlingMesh;
static Mesh sHeartseedMesh;
static void BuildPickups() {
    sSeedlingMesh.Clear();
    sSeedlingMesh.Ellipsoid(V3(0, 0, 0), 150, 190, 150, Col{ 160, 255, 140 }, 5, 8, true);
    sSeedlingMesh.Tube(V3(0, 150, 0), V3(0, 300, 0), 20, 14, Col{ 70, 160, 60 }, 5, true);
    for (f32 sx : { -1.0f, 1.0f }) {
        sSeedlingMesh.Ellipsoid(V3(sx * 80, 310, 0), 90, 18, 40, Col{ 90, 210, 80 }, 3, 6, true);
    }
    sSeedlingMesh.Finalize();

    sHeartseedMesh.Clear();
    sHeartseedMesh.Ellipsoid(V3(0, 0, 0), 300, 380, 300, Col{ 255, 120, 150 }, 7, 12, true);
    sHeartseedMesh.Ellipsoid(V3(0, 40, 0), 200, 260, 200, Col{ 255, 220, 230 }, 5, 10, true);
    sHeartseedMesh.Tube(V3(0, 330, 0), V3(0, 600, 0), 40, 25, Col{ 90, 200, 80 }, 6, true);
    for (f32 sx : { -1.0f, 1.0f }) {
        sHeartseedMesh.Ellipsoid(V3(sx * 150, 600, 0), 170, 30, 70, Col{ 110, 230, 90 }, 4, 8, true);
    }
    sHeartseedMesh.Finalize();
}

static bool sModelsBuilt = false;
static void BuildModels() {
    if (sModelsBuilt) {
        return;
    }
    for (s32 i = 0; i < NPC_COUNT; i++) {
        BuildPipkin(i);
    }
    BuildGrub();
    BuildBeetle();
    BuildPickups();
    sModelsBuilt = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The stumps
// ---------------------------------------------------------------------------------------------------------------------

struct StumpDef {
    s16 scene;
    u16 entrance; // any entrance into the area (you're moved to the stump once it's loaded)
    const char* name;
};

static const StumpDef kStumps[kNumStumps] = {
    { SCENE_KOKIRI_FOREST, ENTR_KOKIRI_FOREST_0, "Kokiri Forest" },
    { SCENE_LOST_WOODS, ENTR_LOST_WOODS_SOUTH_EXIT, "Lost Woods" },
    { SCENE_HYRULE_FIELD, ENTR_HYRULE_FIELD_PAST_BRIDGE_SPAWN, "Hyrule Field" },
    { SCENE_KAKARIKO_VILLAGE, ENTR_KAKARIKO_VILLAGE_FRONT_GATE, "Kakariko Village" },
    { SCENE_LAKE_HYLIA, ENTR_LAKE_HYLIA_NORTH_EXIT, "Lake Hylia" },
};
constexpr s32 kPipStump = 4; // Pip wandered out to Lake Hylia

static s32 StumpForScene(s16 scene) {
    for (s32 i = 0; i < kNumStumps; i++) {
        if (kStumps[i].scene == scene) {
            return i;
        }
    }
    return -1;
}

static bool StumpPlaced(s32 i) {
    return (sSave.stumpPlaced >> i) & 1;
}

static Vec3f StumpPos(s32 i) {
    return V3(sSave.stumpX[i], sSave.stumpY[i], sSave.stumpZ[i]);
}

// The tiny door at the stump's base
static Vec3f StumpDoorPos(s32 i) {
    return Add(StumpPos(i), RotY(V3(0, 0, 58), sSave.stumpYaw[i]));
}

namespace StumpShape {
constexpr f32 kRadius = 55.0f;
constexpr f32 kHeight = 50.0f;
} // namespace StumpShape

static Mesh sStumpMesh;
static void BuildStumpMesh() {
    Mesh& m = sStumpMesh;
    m.Clear();
    using namespace StumpShape;
    Col bark = { 105, 72, 45 };
    Col barkDark = { 70, 46, 28 };
    Col wood = { 185, 150, 100 };
    Col ring = { 140, 105, 65 };
    Col moss = { 90, 170, 80 };
    m.Cylinder(0, 0, kRadius, -10, kHeight, 14, bark, wood, MF_SOLID, false, true, SURF_WOOD);
    for (f32 r : { 15.0f, 28.0f, 41.0f }) {
        m.Ring(0, 0, r, r + 2.5f, kHeight + 0.4f, 14, ring);
    }
    // roots
    for (s32 i = 0; i < 6; i++) {
        s16 yaw = static_cast<s16>(i * 0x2AAA + 0x1555);
        Vec3f a = RotY(V3(0, 18, kRadius - 6), yaw);
        Vec3f b = RotY(V3(0, -4, kRadius + 34), yaw);
        m.Tube(a, b, 11, 4, barkDark, 5);
    }
    // the tiny door, with a glowing frame
    m.Box(-6.5f, 6.5f, 0, 15, kRadius - 1.0f, kRadius + 0.6f, Col{ 25, 15, 10 }, Col{ 25, 15, 10 }, MF_DRAW);
    m.Box(-8.5f, -6.5f, 0, 17, kRadius - 1.0f, kRadius + 1.0f, Col{ 140, 255, 150 }, Col{ 140, 255, 150 },
          MF_DRAW | MF_GLOW);
    m.Box(6.5f, 8.5f, 0, 17, kRadius - 1.0f, kRadius + 1.0f, Col{ 140, 255, 150 }, Col{ 140, 255, 150 },
          MF_DRAW | MF_GLOW);
    m.Box(-8.5f, 8.5f, 15, 17, kRadius - 1.0f, kRadius + 1.0f, Col{ 140, 255, 150 }, Col{ 140, 255, 150 },
          MF_DRAW | MF_GLOW);
    // moss and a little sprout on top
    m.Ellipsoid(V3(-18, kHeight, 10), 16, 3, 12, moss, 3, 8);
    m.Tube(V3(12, kHeight, -8), V3(14, kHeight + 16, -8), 2, 1.5f, Col{ 80, 150, 60 }, 5);
    m.Ellipsoid(V3(19, kHeight + 16, -8), 7, 1.5f, 3, Col{ 100, 200, 80 }, 3, 6);
    m.Ellipsoid(V3(9, kHeight + 17, -8), 7, 1.5f, 3, Col{ 100, 200, 80 }, 3, 6);
    m.Finalize();
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipkin Hollow (Minish scale: Link is ~7 units tall here)
// ---------------------------------------------------------------------------------------------------------------------

namespace Hollow {
constexpr f32 kRadius = 300.0f;
constexpr f32 kHeight = 220.0f;
const Vec3f kArrive = { 0.0f, 0.0f, 235.0f };
const Vec3f kRing = { 0.0f, 0.0f, 250.0f };
constexpr f32 kRingRadius = 22.0f;
const Vec3f kGate = { 0.0f, 0.0f, -285.0f };
const Vec3f kGateInside = { 0.0f, 0.0f, -245.0f };
const Vec3f kSeeds[5] = {
    { 0.0f, 8.0f, -72.0f },     // behind the Elder's tree
    { -180.0f, 62.0f, 180.0f }, // top of the pebble steps
    { 237.0f, 78.0f, 200.0f },  // end of the log ramp
    { -50.0f, 8.0f, -200.0f },  // inside the rock tunnel
    { 250.0f, 48.0f, -90.0f },  // on the root ledge
};
struct NpcSpot {
    Vec3f pos;
    s16 yaw;
};
const NpcSpot kNpcSpots[NPC_COUNT] = {
    { { 0.0f, 0.0f, 62.0f }, 0 },          // Elder, in front of his tree
    { { 172.0f, 0.0f, 128.0f }, -0x2000 }, // Tansy, by her garden
    { { 45.0f, 0.0f, -262.0f }, 0 },       // Moss, at the Rootworks gate
    { { -45.0f, 0.0f, -262.0f }, 0 },      // Pip (once he's home), next to his big brother
    { { -172.0f, 0.0f, 128.0f }, 0x2000 }, // Burl, by the beetle pen
};
const Vec3f kHouses[4] = {
    { -170.0f, 0.0f, 80.0f }, { 170.0f, 0.0f, 80.0f }, { -150.0f, 0.0f, -150.0f }, { 150.0f, 0.0f, -150.0f }
};
const Vec3f kPetBeetle = { -232.0f, 0.0f, 0.0f };
} // namespace Hollow

static Mesh sHollowMesh;

static void BuildHollow() {
    using namespace Hollow;
    Mesh& m = sHollowMesh;
    m.Clear();
    Col grass = { 85, 145, 65 };
    Col bark = { 95, 65, 40 };
    Col barkDark = { 65, 42, 26 };
    Col ceiling = { 55, 38, 25 };
    Col stone = { 150, 145, 135 };
    Col stoneDark = { 105, 100, 92 };
    Col log = { 125, 88, 55 };
    Col soil = { 110, 80, 55 };
    Col glow = { 255, 230, 140 };
    Col greenGlow = { 140, 255, 150 };

    // Floor, wall and dome
    m.Disc(0, 0, kRadius, 0, 16, grass, MF_SOLID, SURF_GRASS);
    m.Cylinder(0, 0, kRadius, -20, kHeight, 20, bark, bark, MF_SOLID, true, false, SURF_WOOD);
    for (s32 i = 0; i < 20; i++) {
        f32 a0 = 2.0f * kPi * static_cast<f32>(i) / 20.0f;
        f32 a1 = 2.0f * kPi * static_cast<f32>(i + 1) / 20.0f;
        m.Tri(V3(0, kHeight + 90, 0), V3(kRadius * sinf(a0), kHeight, kRadius * cosf(a0)),
              V3(kRadius * sinf(a1), kHeight, kRadius * cosf(a1)), ceiling, V3(0, -1, 0), MF_DRAW);
    }
    // Roots arching down the walls, and hanging glow-bulbs
    for (s32 i = 0; i < 10; i++) {
        s16 yaw = static_cast<s16>(i * 0x1999 + 0x0CCC);
        Vec3f a = RotY(V3(0, kHeight + 40, kRadius * 0.55f), yaw);
        Vec3f b = RotY(V3(0, kHeight - 10, kRadius - 4), yaw);
        Vec3f c = RotY(V3(0, 10, kRadius - 2), yaw);
        m.Tube(a, b, 9, 7, barkDark, 6);
        m.Tube(b, c, 7, 10, barkDark, 6);
        Vec3f bulb = RotY(V3(0, kHeight - 45 - static_cast<f32>(i % 3) * 18, kRadius * 0.6f), yaw);
        m.Tube(Add(bulb, V3(0, 6, 0)), RotY(V3(0, kHeight + 30, kRadius * 0.6f), yaw), 1, 1, barkDark, 4);
        m.Ellipsoid(bulb, 7, 9, 7, glow, 4, 8, true);
    }

    // The Elder's tree in the middle, reaching up into the dome
    m.Cylinder(0, 0, 40, 0, kHeight + 60, 12, bark, bark, MF_SOLID, false, false, SURF_WOOD);
    m.Box(-9, 9, 0, 24, 39, 41.5f, Col{ 30, 18, 12 }, Col{ 30, 18, 12 }, MF_DRAW); // door
    m.Ellipsoid(V3(0, 28, 41), 9, 4, 1.5f, greenGlow, 3, 6, true);
    for (s32 i = 0; i < 5; i++) {
        s16 yaw = static_cast<s16>(i * 0x3333);
        m.Tube(RotY(V3(0, 14, 34), yaw), RotY(V3(0, -2, 62), yaw), 8, 3, barkDark, 5);
    }

    // Mushroom houses (stems are solid, caps are just for looks)
    const Col caps[4] = { { 190, 70, 55 }, { 215, 180, 120 }, { 160, 110, 170 }, { 210, 150, 60 } };
    for (s32 i = 0; i < 4; i++) {
        const Vec3f& h = kHouses[i];
        m.Cylinder(h.x, h.z, 22, 0, 52, 10, Col{ 235, 225, 200 }, Col{ 235, 225, 200 }, MF_SOLID, false, false,
                   SURF_WOOD);
        m.Ellipsoid(V3(h.x, 56, h.z), 58, 30, 58, caps[i], 5, 12);
        m.Ellipsoid(V3(h.x, 50, h.z), 50, 6, 50, Col{ 225, 210, 185 }, 3, 12);
        for (s32 d = 0; d < 3; d++) {
            f32 a = static_cast<f32>(d) * 2.1f + static_cast<f32>(i);
            m.Ellipsoid(V3(h.x + 40 * sinf(a), 72, h.z + 40 * cosf(a)), 10, 5, 10, Col{ 250, 245, 230 }, 3, 6);
        }
        // door facing the middle
        s16 toMiddle = Math_Atan2S(-h.z, -h.x);
        Vec3f d = Add(h, RotY(V3(0, 0, 22.5f), toMiddle));
        Vec3f d2 = Add(h, RotY(V3(0, 0, 23.5f), toMiddle));
        m.Tube(V3(d.x, 0, d.z), V3(d2.x, 14, d2.z), 6, 5, Col{ 40, 25, 15 }, 6);
        Vec3f w = Add(h, RotY(V3(0, 30, 22.5f), toMiddle));
        m.Ellipsoid(w, 4, 4, 1.5f, glow, 3, 6, true);
    }

    // Pebble steps (seedling on top)
    m.Box(-120, -80, 0, 18, 160, 200, stone, stoneDark, MF_SOLID, SURF_STONE, 0x3D);
    m.Box(-160, -120, 0, 36, 160, 200, stone, stoneDark, MF_SOLID, SURF_STONE, 0x3D);
    m.Box(-200, -160, 0, 54, 160, 200, stone, stoneDark, MF_SOLID, SURF_STONE, 0x3D);

    // Fallen log ramp up to a ledge (seedling at the end)
    m.Quad(V3(100, 0, 185), V3(220, 70, 185), V3(220, 70, 215), V3(100, 0, 215), log, V3(-0.5f, 1, 0), MF_SOLID,
           SURF_WOOD);
    m.Tri(V3(100, 0, 185), V3(220, 70, 185), V3(220, 0, 185), barkDark, V3(0, 0, -1), MF_SOLID, SURF_WOOD);
    m.Tri(V3(100, 0, 215), V3(220, 70, 215), V3(220, 0, 215), barkDark, V3(0, 0, 1), MF_SOLID, SURF_WOOD);
    m.Box(220, 255, 0, 70, 185, 215, log, barkDark, MF_SOLID, SURF_WOOD, 0x3D);

    // Big rock with a tunnel through it (seedling inside). Roof top is high enough that you don't pop onto it.
    m.Box(-80, -20, 0, 60, -230, -210, stone, stoneDark, MF_SOLID, SURF_STONE, 0x3D);
    m.Box(-80, -20, 0, 60, -190, -170, stone, stoneDark, MF_SOLID, SURF_STONE, 0x3D);
    m.Box(-80, -20, 26, 60, -210, -190, stone, stoneDark, MF_SOLID, SURF_STONE, 0x03); // roof: top + ceiling
    m.Box(-80, -20, 26, 60, -210, -190, stone, stoneDark, MF_DRAW, SURF_STONE, 0x30);  // roof ends (look only)

    // Root ledge with two steps (seedling on top)
    m.Box(200, 230, 0, 20, -120, -60, soil, barkDark, MF_SOLID, SURF_DIRT, 0x3D);
    m.Box(230, 272, 0, 40, -120, -60, soil, barkDark, MF_SOLID, SURF_DIRT, 0x3D);

    // Tansy's garden rows
    for (s32 row = 0; row < 3; row++) {
        f32 x = 205.0f + static_cast<f32>(row) * 22.0f;
        m.Box(x - 7, x + 7, 0, 2, 0, 60, soil, soil, MF_DRAW, SURF_DIRT, 0x3D);
        for (s32 k = 0; k < 4; k++) {
            f32 z = 8.0f + static_cast<f32>(k) * 15.0f;
            m.Tube(V3(x, 2, z), V3(x, 9, z), 0.8f, 0.6f, Col{ 70, 150, 60 }, 4);
            m.Ellipsoid(V3(x + 2.5f, 9, z), 3, 0.8f, 1.5f, Col{ 110, 200, 80 }, 2, 5);
            m.Ellipsoid(V3(x - 2.5f, 9, z), 3, 0.8f, 1.5f, Col{ 110, 200, 80 }, 2, 5);
        }
    }

    // Burl's beetle pen (fence posts)
    for (s32 i = 0; i < 9; i++) {
        f32 t = static_cast<f32>(i) / 8.0f;
        f32 z = -40.0f + 80.0f * t;
        m.Box(-205, -201, 0, 14, z - 2, z + 2, log, log, MF_DRAW);
    }
    m.Box(-205, -201, 9, 11, -40, 40, log, log, MF_DRAW);

    // Exit ring and the Rootworks gate
    m.Ring(kRing.x, kRing.z, kRingRadius - 3, kRingRadius, 0.5f, 20, greenGlow);
    m.Ring(kRing.x, kRing.z, 6, 8, 0.5f, 12, greenGlow);
    m.Box(-38, 38, 0, 90, -300, -296, Col{ 85, 55, 35 }, Col{ 85, 55, 35 }, MF_DRAW);   // gate frame
    m.Box(-30, -1, 0, 80, -297, -293, Col{ 120, 85, 55 }, Col{ 120, 85, 55 }, MF_DRAW); // left door
    m.Box(1, 30, 0, 80, -297, -293, Col{ 120, 85, 55 }, Col{ 120, 85, 55 }, MF_DRAW);   // right door
    m.Ellipsoid(V3(0, 60, -292), 10, 10, 2, Col{ 120, 255, 140 }, 4, 8, true);          // seal
    m.Finalize();
}

// ---------------------------------------------------------------------------------------------------------------------
// The Rootworks (normal scale: big Link and tiny Link both fit, in different places)
// ---------------------------------------------------------------------------------------------------------------------

namespace Rootworks {
const Vec3f kArrive = { 0.0f, 0.0f, 330.0f };
constexpr f32 kPadRadius = 30.0f;
const Vec3f kSwitch = { -380.0f, 0.0f, 200.0f };
constexpr f32 kSwitchRadius = 24.0f;
const Vec3f kHeartseed = { 0.0f, 40.0f, -1360.0f };
const Vec3f kBossSpawn = { 0.0f, 0.0f, -1450.0f };
const Vec3f kGrubSpawns[4] = {
    { -120.0f, 0.0f, -880.0f }, { 120.0f, 0.0f, -880.0f }, { -60.0f, 0.0f, -960.0f }, { 80.0f, 0.0f, -800.0f }
};
const Vec3f kArenaEntry = { 0.0f, 0.0f, -1090.0f };
// Places too cramped to grow (tunnels and the low den)
struct Zone {
    f32 x0, x1, z0, z1;
};
const Zone kNoGrow[4] = { { -320.0f, -180.0f, 175.0f, 225.0f },
                          { -30.0f, 30.0f, -780.0f, -680.0f },
                          { -200.0f, 200.0f, -1000.0f, -760.0f },
                          { -30.0f, 30.0f, -1080.0f, -980.0f } };
const Zone kDen = { -200.0f, 200.0f, -1000.0f, -760.0f };
const Zone kArena = { -350.0f, 350.0f, -1660.0f, -1060.0f };
} // namespace Rootworks

static Mesh sRootworksMesh;

// Wall plane at x, spanning z0..z1 and y0..y1, facing +x (dir = 1) or -x (dir = -1)
static void WallX(Mesh& m, f32 x, f32 z0, f32 z1, f32 y0, f32 y1, f32 dir, Col c) {
    m.Quad(V3(x, y0, z0), V3(x, y0, z1), V3(x, y1, z1), V3(x, y1, z0), c, V3(dir, 0, 0), MF_SOLID, SURF_WOOD);
}
static void WallZ(Mesh& m, f32 z, f32 x0, f32 x1, f32 y0, f32 y1, f32 dir, Col c) {
    m.Quad(V3(x0, y0, z), V3(x1, y0, z), V3(x1, y1, z), V3(x0, y1, z), c, V3(0, 0, dir), MF_SOLID, SURF_WOOD);
}
static void FloorQ(Mesh& m, f32 x0, f32 x1, f32 z0, f32 z1, f32 y, Col c, s32 surf = SURF_DIRT) {
    m.Quad(V3(x0, y, z0), V3(x1, y, z0), V3(x1, y, z1), V3(x0, y, z1), c, V3(0, 1, 0), MF_SOLID, surf);
}
static void CeilQ(Mesh& m, f32 x0, f32 x1, f32 z0, f32 z1, f32 y, Col c) {
    m.Quad(V3(x0, y, z0), V3(x1, y, z0), V3(x1, y, z1), V3(x0, y, z1), c, V3(0, -1, 0), MF_SOLID, SURF_WOOD);
}
// Wall with a rectangular opening (doorway or tunnel mouth) along z (plane x = const)
static void WallXHole(Mesh& m, f32 x, f32 z0, f32 z1, f32 y0, f32 y1, f32 dir, f32 hz0, f32 hz1, f32 hy, Col c) {
    WallX(m, x, z0, hz0, y0, y1, dir, c);
    WallX(m, x, hz1, z1, y0, y1, dir, c);
    WallX(m, x, hz0, hz1, hy, y1, dir, c);
}
static void WallZHole(Mesh& m, f32 z, f32 x0, f32 x1, f32 y0, f32 y1, f32 dir, f32 hx0, f32 hx1, f32 hy, Col c) {
    WallZ(m, z, x0, hx0, y0, y1, dir, c);
    WallZ(m, z, hx1, x1, y0, y1, dir, c);
    WallZ(m, z, hx0, hx1, hy, y1, dir, c);
}
// Tunnel along x or z with floor, ceiling and two side walls
static void TunnelX(Mesh& m, f32 x0, f32 x1, f32 z0, f32 z1, f32 h, Col floor, Col wall) {
    FloorQ(m, x0, x1, z0, z1, 0, floor);
    CeilQ(m, x0, x1, z0, z1, h, wall);
    WallZ(m, z0, x0, x1, 0, h, 1, wall);
    WallZ(m, z1, x0, x1, 0, h, -1, wall);
}
static void TunnelZ(Mesh& m, f32 x0, f32 x1, f32 z0, f32 z1, f32 h, Col floor, Col wall) {
    FloorQ(m, x0, x1, z0, z1, 0, floor);
    CeilQ(m, x0, x1, z0, z1, h, wall);
    WallX(m, x0, z0, z1, 0, h, 1, wall);
    WallX(m, x1, z0, z1, 0, h, -1, wall);
}

static void BuildRootworks() {
    Mesh& m = sRootworksMesh;
    m.Clear();
    Col soil = { 90, 62, 42 };
    Col soilDark = { 62, 42, 28 };
    Col wall = { 78, 54, 36 };
    Col wallDark = { 55, 38, 25 };
    Col root = { 110, 78, 50 };
    Col step = { 120, 95, 70 };
    Col glow = { 130, 255, 160 };
    Col shroom = { 120, 200, 255 };

    // R1: entry hall
    FloorQ(m, -200, 200, 0, 400, 0, soil);
    CeilQ(m, -200, 200, 0, 400, 200, wallDark);
    WallZ(m, 400, -200, 200, 0, 200, -1, wall);
    WallX(m, 200, 0, 400, 0, 200, -1, wall);
    WallXHole(m, -200, 0, 400, 0, 200, 1, 190, 210, 30, wall);
    WallZHole(m, 0, -200, 200, 0, 200, 1, -60, 60, 150, wall);
    m.Ring(Rootworks::kArrive.x, Rootworks::kArrive.z, Rootworks::kPadRadius - 4, Rootworks::kPadRadius, 0.6f, 20,
           glow);
    // T1 and R2: the switch room
    TunnelX(m, -300, -200, 190, 210, 30, soil, wallDark);
    FloorQ(m, -460, -300, 120, 280, 0, soil);
    CeilQ(m, -460, -300, 120, 280, 160, wallDark);
    WallX(m, -460, 120, 280, 0, 160, 1, wall);
    WallZ(m, 120, -460, -300, 0, 160, 1, wall);
    WallZ(m, 280, -460, -300, 0, 160, -1, wall);
    WallXHole(m, -300, 120, 280, 0, 160, -1, 190, 210, 30, wall);

    // R3: the chasm hall
    FloorQ(m, -250, 250, -160, 0, 0, soil);
    FloorQ(m, -250, 250, -460, -160, -160, soilDark);
    FloorQ(m, -250, 250, -700, -460, 0, soil);
    WallZ(m, -160, -250, 250, -160, 0, -1, wallDark);
    WallZ(m, -460, -250, 250, -160, 0, 1, wallDark);
    CeilQ(m, -250, 250, -700, 0, 260, wallDark);
    WallX(m, -250, -700, 0, -160, 260, 1, wall);
    WallX(m, 250, -700, 0, -160, 260, -1, wall);
    WallZHole(m, 0, -250, 250, 0, 260, -1, -60, 60, 150, wall);
    WallZHole(m, -700, -250, 250, 0, 260, 1, -10, 10, 30, wall);
    // stairs out of the pit (east side)
    for (s32 i = 0; i < 8; i++) {
        f32 z0 = -400.0f + 30.0f * static_cast<f32>(i);
        f32 top = -160.0f + 20.0f * static_cast<f32>(i + 1);
        m.Box(190, 250, -160, top, z0, z0 + 30.0f, step, wallDark, MF_SOLID, SURF_WOOD, 0x01 | 0x04 | 0x10);
    }
    // stairs up to the gliding perch (west side)
    for (s32 j = 0; j < 7; j++) {
        f32 z1 = -20.0f * static_cast<f32>(j);
        f32 top = 20.0f * static_cast<f32>(j + 1);
        m.Box(-250, -190, 0, top, z1 - 20.0f, z1, step, wallDark, MF_SOLID, SURF_WOOD, 0x01 | 0x08 | 0x20);
    }
    m.Box(-250, -190, 0, 140, -160, -140, root, wallDark, MF_SOLID, SURF_WOOD, 0x01 | 0x04 | 0x20);
    // gloom seeping in the pit
    for (s32 i = 0; i < 6; i++) {
        f32 x = -180.0f + 70.0f * static_cast<f32>(i);
        m.Ellipsoid(V3(x, -158, -300 + 40.0f * sinf(static_cast<f32>(i))), 28, 3, 22, Col{ 60, 30, 80 }, 3, 8);
    }

    // T2, R4 (the Gloomgrub den, too low to grow), T3
    TunnelZ(m, -10, 10, -760, -700, 30, soil, wallDark);
    FloorQ(m, -200, 200, -1000, -760, 0, soilDark);
    CeilQ(m, -200, 200, -1000, -760, 40, wallDark);
    WallX(m, -200, -1000, -760, 0, 40, 1, wall);
    WallX(m, 200, -1000, -760, 0, 40, -1, wall);
    WallZHole(m, -760, -200, 200, 0, 40, -1, -10, 10, 30, wall);
    WallZHole(m, -1000, -200, 200, 0, 40, 1, -10, 10, 30, wall);
    TunnelZ(m, -10, 10, -1060, -1000, 30, soil, wallDark);

    // R5: the arena
    FloorQ(m, -350, 350, -1660, -1060, 0, soil);
    CeilQ(m, -350, 350, -1660, -1060, 260, wallDark);
    WallX(m, -350, -1660, -1060, 0, 260, 1, wall);
    WallX(m, 350, -1660, -1060, 0, 260, -1, wall);
    WallZ(m, -1660, -350, 350, 0, 260, 1, wall);
    WallZHole(m, -1060, -350, 350, 0, 260, -1, -10, 10, 30, wall);

    // Decoration: roots along the walls, glowing mushrooms for light
    struct Deco {
        Vec3f a, b;
    };
    const Deco roots[] = {
        { { -200, 190, 60 }, { -150, 0, 120 } },      { { 200, 180, 300 }, { 150, 0, 360 } },
        { { -250, 250, -50 }, { -200, -160, -300 } }, { { 250, 240, -600 }, { 180, 0, -680 } },
        { { -350, 240, -1100 }, { -260, 0, -1200 } }, { { 350, 240, -1600 }, { 250, 0, -1500 } },
        { { -350, 240, -1550 }, { -280, 0, -1620 } }, { { 350, 230, -1150 }, { 300, 0, -1080 } },
    };
    for (const Deco& d : roots) {
        m.Tube(d.a, d.b, 16, 10, root, 6);
    }
    const Vec3f shrooms[] = { { -180, 0, 380 },   { 180, 0, 20 },    { -440, 0, 140 },   { -230, 0, -20 },
                              { 230, 0, -680 },   { -230, 0, -680 }, { -180, 0, -980 },  { 180, 0, -780 },
                              { -330, 0, -1640 }, { 330, 0, -1640 }, { -330, 0, -1080 }, { 330, 0, -1080 } };
    for (const Vec3f& p : shrooms) {
        m.Tube(p, Add(p, V3(0, 12, 0)), 2.5f, 2, Col{ 220, 220, 200 }, 5);
        m.Ellipsoid(Add(p, V3(0, 13, 0)), 10, 5, 10, shroom, 3, 8, true);
    }
    m.Finalize();
}

// The two gates: the big gate (opened by the heavy switch) and the den's tiny root door (opened by clearing it)
enum DoorId { DOOR_BIG, DOOR_DEN, DOOR_COUNT };
static Mesh sDoorMesh[DOOR_COUNT];
static Mesh sSwitchMesh;
static const Vec3f kDoorPos[DOOR_COUNT] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1000.0f } };
static const f32 kDoorHeight[DOOR_COUNT] = { 150.0f, 30.0f };

static void BuildDoors() {
    Col wood = { 120, 84, 52 };
    Col dark = { 80, 55, 33 };
    Col glow = { 130, 255, 160 };
    Mesh& big = sDoorMesh[DOOR_BIG];
    big.Clear();
    big.Box(-60, 60, 0, 150, -12, 12, dark, wood, MF_SOLID, SURF_WOOD, 0x01 | 0x04 | 0x08);
    for (f32 x : { -40.0f, 0.0f, 40.0f }) {
        big.Box(x - 6, x + 6, 0, 150, -14, 14, dark, dark, MF_DRAW);
    }
    big.Ellipsoid(V3(0, 90, 14), 14, 14, 3, glow, 4, 8, true);
    big.Ellipsoid(V3(0, 90, -14), 14, 14, 3, glow, 4, 8, true);
    big.Finalize();

    Mesh& den = sDoorMesh[DOOR_DEN];
    den.Clear();
    den.Box(-10, 10, 0, 30, -6, 6, dark, wood, MF_SOLID, SURF_WOOD, 0x01 | 0x04 | 0x08);
    for (f32 x : { -5.0f, 5.0f }) {
        den.Tube(V3(x, 0, 6.5f), V3(x * 0.4f, 30, 6.5f), 2.5f, 1.5f, dark, 5);
    }
    den.Ellipsoid(V3(0, 15, 6.5f), 3, 3, 1, glow, 3, 6, true);
    den.Finalize();

    sSwitchMesh.Clear();
    sSwitchMesh.Cylinder(0, 0, Rootworks::kSwitchRadius, 0, 3, 14, Col{ 150, 120, 80 }, Col{ 200, 170, 110 }, MF_DRAW);
    sSwitchMesh.Ring(0, 0, Rootworks::kSwitchRadius - 6, Rootworks::kSwitchRadius - 3, 3.3f, 14, Col{ 130, 255, 160 });
    sSwitchMesh.Ellipsoid(V3(0, 3, 0), 7, 2, 7, Col{ 130, 255, 160 }, 3, 8, true);
    sSwitchMesh.Finalize();
}

static bool sWorldMeshesBuilt = false;
static void BuildWorldMeshes() {
    if (sWorldMeshesBuilt) {
        return;
    }
    BuildStumpMesh();
    BuildHollow();
    BuildRootworks();
    BuildDoors();
    sWorldMeshesBuilt = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pockets: an empty space in the current area, high above everything, where the Hollow and the Rootworks go
// ---------------------------------------------------------------------------------------------------------------------

struct Bounds {
    f32 x0, x1, y0, y1, z0, z1;
};
static const Bounds kHollowBounds = { -310.0f, 310.0f, -20.0f, 320.0f, -310.0f, 310.0f };
static const Bounds kRootworksBounds = { -470.0f, 360.0f, -170.0f, 270.0f, -1670.0f, 410.0f };

// Finds a spot inside the area's collision box with nothing underneath the pocket for a good distance.
// Positions outside the box would mostly work too, but some collision checks skip them, so stay inside if possible.
static Vec3f FindPocket(PlayState* play, const Bounds& b) {
    const Vec3f& mn = play->colCtx.minBounds;
    const Vec3f& mx = play->colCtx.maxBounds;
    f32 top = mx.y - 10.0f;
    f32 floorY = top - b.y1;
    f32 bestMargin = -1.0e9f;
    Vec3f best = V3((mn.x + mx.x) * 0.5f, mx.y + 200.0f - b.y0, (mn.z + mx.z) * 0.5f);

    f32 xLo = mn.x - b.x0 + 10.0f;
    f32 xHi = mx.x - b.x1 - 10.0f;
    f32 zLo = mn.z - b.z0 + 10.0f;
    f32 zHi = mx.z - b.z1 - 10.0f;
    if (xLo <= xHi && zLo <= zHi) {
        for (s32 ix = 0; ix < 8; ix++) {
            for (s32 iz = 0; iz < 8; iz++) {
                f32 cx = xLo + (xHi - xLo) * static_cast<f32>(ix) / 7.0f;
                f32 cz = zLo + (zHi - zLo) * static_cast<f32>(iz) / 7.0f;
                f32 highest = mn.y;
                for (s32 sx = 0; sx < 5; sx++) {
                    for (s32 sz = 0; sz < 5; sz++) {
                        f32 x = cx + b.x0 + (b.x1 - b.x0) * static_cast<f32>(sx) / 4.0f;
                        f32 z = cz + b.z0 + (b.z1 - b.z0) * static_cast<f32>(sz) / 4.0f;
                        CollisionPoly* poly = nullptr;
                        f32 y = FloorAt(play, x, top, z, &poly);
                        if (poly != nullptr) {
                            highest = std::max(highest, y);
                        }
                    }
                }
                f32 margin = (floorY + b.y0) - highest;
                if (margin > bestMargin) {
                    bestMargin = margin;
                    best = V3(cx, floorY, cz);
                }
            }
        }
    }
    if (bestMargin < 150.0f) {
        // Not enough room inside the box: go above it
        best.y = mx.y + 200.0f - b.y0;
    }
    return best;
}

// Room left in the area's moving-collision budget
static bool DynaBudgetFor(PlayState* play, const Mesh& mesh) {
    DynaCollisionContext* dyna = &play->colCtx.dyna;
    s32 polys = 0;
    s32 verts = 0;
    for (s32 i = 0; i < BG_ACTOR_MAX; i++) {
        // in use, and not on its way out
        if ((dyna->bgActorFlags[i] & 1) && !(dyna->bgActorFlags[i] & 2) && dyna->bgActors[i].colHeader != nullptr) {
            polys += dyna->bgActors[i].colHeader->numPolygons;
            verts += dyna->bgActors[i].colHeader->numVertices;
        }
    }
    return polys + static_cast<s32>(mesh.colPoly.size()) + 32 < dyna->polyListMax &&
           verts + static_cast<s32>(mesh.colVtx.size()) + 32 < dyna->vtxListMax;
}

// ---------------------------------------------------------------------------------------------------------------------
// Actors: area geometry, gates, stumps
// ---------------------------------------------------------------------------------------------------------------------

enum AreaKind { AREA_HOLLOW, AREA_ROOTWORKS };

enum Loc { LOC_WORLD, LOC_HOLLOW, LOC_ROOTWORKS };
static Loc sLoc = LOC_WORLD;
static Vec3f sOrigin = { 0.0f, 0.0f, 0.0f }; // pocket origin of the area you're in
static Actor* sAreaActor = nullptr;
static Actor* sDoorActors[DOOR_COUNT] = { nullptr, nullptr };
static Actor* sStumpActor = nullptr;

static s16 sAreaId = -1;
static s16 sDoorId = -1;
static s16 sStumpId = -1;

struct DoorActor {
    DynaPolyActor dyna;
    s16 door;
    s16 opening;
    s16 timer;
};

static Vec3f Local(const Vec3f& world) {
    return Sub(world, sOrigin);
}
static Vec3f World(const Vec3f& local) {
    return Add(local, sOrigin);
}

static void DrawAt(PlayState* play, const Mesh& mesh, Vec3f pos, s16 yaw, f32 scale, u8 r = 255, u8 g = 255,
                   u8 b = 255) {
    Matrix_Push();
    Matrix_Translate(pos.x, pos.y, pos.z, MTXMODE_NEW);
    Matrix_RotateY(static_cast<f32>(BINANG_TO_RAD(yaw)), MTXMODE_APPLY);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    DrawMesh(play, mesh, r, g, b);
    Matrix_Pop();
}

static bool SeedTaken(s32 i) {
    return (sSave.seeds >> i) & 1;
}

static void AreaActor_Init(Actor* thisx, PlayState* play) {
    DynaPolyActor* dyna = (DynaPolyActor*)thisx;
    DynaPolyActor_Init(dyna, 0);
    Actor_SetScale(thisx, 1.0f);
    thisx->world.rot.x = thisx->world.rot.y = thisx->world.rot.z = 0;
    thisx->shape.rot = thisx->world.rot;
    thisx->room = -1;
    const Mesh& mesh = (thisx->params == AREA_HOLLOW) ? sHollowMesh : sRootworksMesh;
    dyna->bgId = DynaPoly_SetBgActor(play, &play->colCtx.dyna, thisx, const_cast<CollisionHeader*>(&mesh.header));
}

static void AreaActor_Destroy(Actor* thisx, PlayState* play) {
    DynaPolyActor* dyna = (DynaPolyActor*)thisx;
    DynaPoly_DeleteBgActor(play, &play->colCtx.dyna, dyna->bgId);
    if (sAreaActor == thisx) {
        sAreaActor = nullptr;
    }
}

static void AreaActor_Update(Actor*, PlayState*) {
}

static void AreaActor_Draw(Actor* thisx, PlayState* play) {
    s16 spin = static_cast<s16>(play->gameplayFrames * 0x300);
    if (thisx->params == AREA_HOLLOW) {
        DrawMesh(play, sHollowMesh);
        // seedlings still waiting to be found
        if (Has(Q_SEEDS_START) && !Has(Q_SEEDS_DONE)) {
            for (s32 i = 0; i < 5; i++) {
                if (SeedTaken(i)) {
                    continue;
                }
                Vec3f p = Add(thisx->world.pos, Hollow::kSeeds[i]);
                p.y += 1.5f * Math_SinS(static_cast<s16>(spin * 2 + i * 0x3000));
                DrawAt(play, sSeedlingMesh, p, spin, 0.03f);
            }
        }
        // Burl's pet beetle (once it's brought home a bug, it's happy and bouncy)
        Vec3f bp = Add(thisx->world.pos, Hollow::kPetBeetle);
        if (Has(Q_BUG_DONE)) {
            bp.y += fabsf(2.0f * Math_SinS(static_cast<s16>(play->gameplayFrames * 0x900)));
        }
        DrawAt(play, sBeetleBody, bp, static_cast<s16>(0x4000 + 0x800 * Math_SinS(spin)), 0.012f, 190, 160, 120);
    } else {
        DrawMesh(play, sRootworksMesh);
        // heavy switch
        Vec3f sw = Add(thisx->world.pos, Rootworks::kSwitch);
        f32 h = Has(Q_SWITCH) ? 0.4f : 3.0f;
        Matrix_Push();
        Matrix_Translate(sw.x, sw.y, sw.z, MTXMODE_NEW);
        Matrix_Scale(1.0f, h / 3.0f, 1.0f, MTXMODE_APPLY);
        DrawMesh(play, sSwitchMesh, Has(Q_SWITCH) ? 140 : 255, 255, Has(Q_SWITCH) ? 140 : 255);
        Matrix_Pop();
        // the Heartseed, once the Gloomhorn is gone
        if (Has(Q_BOSS) && !Has(Q_HEARTSEED)) {
            Vec3f p = Add(thisx->world.pos, Rootworks::kHeartseed);
            p.y += 4.0f * Math_SinS(static_cast<s16>(spin * 2));
            DrawAt(play, sHeartseedMesh, p, spin, 0.08f);
        }
    }
}

static void DoorActor_Init(Actor* thisx, PlayState* play) {
    DoorActor* door = (DoorActor*)thisx;
    DynaPolyActor_Init(&door->dyna, 0);
    Actor_SetScale(thisx, 1.0f);
    thisx->room = -1;
    door->door = thisx->params;
    door->opening = 0;
    door->timer = 0;
    door->dyna.bgId = DynaPoly_SetBgActor(play, &play->colCtx.dyna, thisx, &sDoorMesh[door->door].header);
}

static void DoorActor_Destroy(Actor* thisx, PlayState* play) {
    DoorActor* door = (DoorActor*)thisx;
    DynaPoly_DeleteBgActor(play, &play->colCtx.dyna, door->dyna.bgId);
    for (Actor*& a : sDoorActors) {
        if (a == thisx) {
            a = nullptr;
        }
    }
}

static bool DoorShouldOpen(s32 door) {
    return (door == DOOR_BIG) ? Has(Q_SWITCH) : Has(Q_GRUBS);
}

static void DoorActor_Update(Actor* thisx, PlayState* play) {
    DoorActor* door = (DoorActor*)thisx;
    if (!door->opening && DoorShouldOpen(door->door)) {
        door->opening = 1;
        door->timer = 0;
        Audio_PlayActorSound2(thisx, NA_SE_EV_BLOCK_SHAKE);
        Rumble_Request(0.0f, 180, 20, 100);
    }
    if (door->opening == 1) {
        door->timer++;
        f32 target = thisx->home.pos.y - kDoorHeight[door->door] - 4.0f;
        Math_StepToF(&thisx->world.pos.y, target, kDoorHeight[door->door] / 45.0f);
        if (thisx->world.pos.y <= target) {
            door->opening = 2;
            Audio_PlayActorSound2(thisx, NA_SE_EV_STONEDOOR_STOP);
        }
    }
    (void)play;
}

static void DoorActor_Draw(Actor* thisx, PlayState* play) {
    DoorActor* door = (DoorActor*)thisx;
    DrawMesh(play, sDoorMesh[door->door]);
}

static void StumpActor_Init(Actor* thisx, PlayState* play) {
    DynaPolyActor* dyna = (DynaPolyActor*)thisx;
    DynaPolyActor_Init(dyna, 0);
    Actor_SetScale(thisx, 1.0f);
    thisx->room = -1;
    thisx->world.rot.x = thisx->world.rot.z = 0;
    thisx->shape.rot = thisx->world.rot;
    dyna->bgId = DynaPoly_SetBgActor(play, &play->colCtx.dyna, thisx, &sStumpMesh.header);

    NameTagOptions options = {};
    options.tag = "minish_stump";
    options.yOffset = 90;
    options.textColor = { 150, 255, 160, 255 };
    NameTag_RegisterForActorWithOptions(thisx, "Hollow Stump", options);
}

static void StumpActor_Destroy(Actor* thisx, PlayState* play) {
    DynaPolyActor* dyna = (DynaPolyActor*)thisx;
    DynaPoly_DeleteBgActor(play, &play->colCtx.dyna, dyna->bgId);
    if (sStumpActor == thisx) {
        sStumpActor = nullptr;
    }
}

static void StumpActor_Update(Actor*, PlayState*) {
}

static void StumpActor_Draw(Actor*, PlayState* play) {
    DrawMesh(play, sStumpMesh);
}

static s16 RegisterActor(const char* name, s32 category, u32 flags, size_t size, ActorFunc init, ActorFunc destroy,
                         ActorFunc update, ActorFunc draw) {
    ActorDBInit info;
    info.name = name;
    info.desc = name;
    info.category = category;
    info.flags = flags;
    info.objectId = OBJECT_GAMEPLAY_KEEP;
    info.instanceSize = size;
    info.init = init;
    info.destroy = destroy;
    info.update = update;
    info.draw = draw;
    return static_cast<s16>(ActorDB::Instance->AddEntry(info).entry.id);
}

constexpr u32 kNoCull = (1 << 4) | (1 << 5); // update and draw even when off screen

static void RegisterWorldActors() {
    if (sAreaId >= 0) {
        return;
    }
    sAreaId = RegisterActor("Pipkin_Area", ACTORCAT_BG, kNoCull, sizeof(DynaPolyActor), AreaActor_Init,
                            AreaActor_Destroy, AreaActor_Update, AreaActor_Draw);
    sDoorId = RegisterActor("Pipkin_Gate", ACTORCAT_BG, kNoCull, sizeof(DoorActor), DoorActor_Init, DoorActor_Destroy,
                            DoorActor_Update, DoorActor_Draw);
    sStumpId = RegisterActor("Pipkin_Stump", ACTORCAT_BG, kNoCull, sizeof(DynaPolyActor), StumpActor_Init,
                             StumpActor_Destroy, StumpActor_Update, StumpActor_Draw);
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipkins (NPCs)
// ---------------------------------------------------------------------------------------------------------------------

struct PipkinActor {
    Actor actor;
    ColliderCylinder col;
    s16 npc;
    s16 homeYaw;
    s16 frames;
    s16 squeakTimer;
};

static ColliderCylinderInit sPipkinColInit = {
    { COLTYPE_NONE, AT_NONE, AC_NONE, OC1_ON | OC1_TYPE_ALL, OC2_TYPE_2, COLSHAPE_CYLINDER },
    { ELEMTYPE_UNK0, { 0, 0, 0 }, { 0, 0, 0 }, TOUCH_NONE, BUMP_NONE, OCELEM_ON },
    { 3, 7, 0, { 0, 0, 0 } },
};

static s16 sPipkinId = -1;
static Actor* sNpcActors[NPC_COUNT] = {};
static bool sConvTalkingTo[NPC_COUNT] = {};

static void PipkinActor_Init(Actor* thisx, PlayState* play) {
    PipkinActor* p = (PipkinActor*)thisx;
    p->npc = static_cast<s16>(std::clamp<s32>(thisx->params, 0, NPC_COUNT - 1));
    f32 size = kLooks[p->npc].size;
    Actor_SetScale(thisx, 0.01f * size);
    thisx->room = -1;
    ActorShape_Init(&thisx->shape, 0.0f, ActorShadow_DrawCircle, 6.0f);
    Collider_InitCylinder(play, &p->col);
    Collider_SetCylinder(play, &p->col, thisx, &sPipkinColInit);
    thisx->colChkInfo.mass = MASS_IMMOVABLE;
    thisx->targetMode = 0;
    p->homeYaw = thisx->shape.rot.y;
    p->frames = static_cast<s16>(p->npc * 37);
    sNpcActors[p->npc] = thisx;
}

static void PipkinActor_Destroy(Actor* thisx, PlayState* play) {
    PipkinActor* p = (PipkinActor*)thisx;
    Collider_DestroyCylinder(play, &p->col);
    if (sNpcActors[p->npc] == thisx) {
        sNpcActors[p->npc] = nullptr;
    }
}

static void PipkinActor_Update(Actor* thisx, PlayState* play) {
    PipkinActor* p = (PipkinActor*)thisx;
    p->frames++;
    if (p->squeakTimer > 0) {
        p->squeakTimer--;
    }

    bool linkClose = thisx->xzDistToPlayer < 45.0f && fabsf(thisx->yDistToPlayer) < 30.0f;
    s16 targetYaw = linkClose ? thisx->yawTowardsPlayer : p->homeYaw;
    Math_SmoothStepToS(&thisx->shape.rot.y, targetYaw, 4, 0x800, 0x80);
    thisx->world.rot.y = thisx->shape.rot.y;

    if (Actor_ProcessTalkRequest(thisx, play)) {
        // the conversation itself is run by the story (see OnOpenText)
    } else if (Minish_IsTiny() && !sConv.active && thisx->xzDistToPlayer < 24.0f &&
               fabsf(thisx->yDistToPlayer) < 12.0f) {
        thisx->textId = static_cast<u16>(kTextNpcBase + p->npc);
        Actor_OfferTalk(thisx, play, 24.0f);
    } else if (!Minish_IsTiny() && p->npc == NPC_PIP && thisx->xzDistToPlayer < 90.0f && p->squeakTimer == 0) {
        Hud("A tiny voice squeaks: \"Eek! A giant!\" Something small hides by the stump.", 80);
        Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_JR_CRY);
        p->squeakTimer = 200;
    }

    Collider_UpdateCylinder(thisx, &p->col);
    CollisionCheck_SetOC(play, &play->colChkCtx, &p->col.base);
    Actor_SetFocus(thisx, 6.0f * kLooks[p->npc].size);
}

static void PipkinActor_Draw(Actor* thisx, PlayState* play) {
    PipkinActor* p = (PipkinActor*)thisx;
    f32 bob = Math_SinS(static_cast<s16>(p->frames * 0x500));
    bool talking = sConv.active && sConv.npc == p->npc;
    f32 amp = talking ? 0.06f : 0.03f;
    Matrix_Push();
    Matrix_Scale(1.0f + amp * bob, 1.0f - amp * bob, 1.0f + amp * bob, MTXMODE_APPLY);
    DrawMesh(play, sPipkinBody[p->npc]);
    Matrix_Translate(0.0f, 515.0f, 0.0f, MTXMODE_APPLY);
    Matrix_RotateZ(0.18f * Math_SinS(static_cast<s16>(p->frames * 0x380)), MTXMODE_APPLY);
    DrawMesh(play, sPipkinSprout[p->npc]);
    Matrix_Pop();
}

static Actor* SpawnPipkin(PlayState* play, s32 npc, Vec3f pos, s16 yaw) {
    Actor* a = Actor_Spawn(&play->actorCtx, play, sPipkinId, pos.x, pos.y, pos.z, 0, yaw, 0, static_cast<s16>(npc));
    if (a != nullptr) {
        a->room = -1;
    }
    return a;
}

// ---------------------------------------------------------------------------------------------------------------------
// Gloomgrubs
// ---------------------------------------------------------------------------------------------------------------------

struct GrubActor {
    Actor actor;
    ColliderCylinder col;
    s16 hp;
    s16 hurt;
    s16 recoil;
    s16 frames;
};

static ColliderCylinderInit sGrubColInit = {
    { COLTYPE_HIT0, AT_ON | AT_TYPE_ENEMY, AC_ON | AC_TYPE_PLAYER, OC1_ON | OC1_TYPE_ALL, OC2_TYPE_1,
      COLSHAPE_CYLINDER },
    { ELEMTYPE_UNK0,
      { 0xFFCFFFFF, 0x00, 0x08 },
      { 0xFFCFFFFF, 0x00, 0x00 },
      TOUCH_ON | TOUCH_SFX_NORMAL,
      BUMP_ON,
      OCELEM_ON },
    { 7, 6, 0, { 0, 0, 0 } },
};

static s16 sGrubId = -1;

static void GrubActor_Init(Actor* thisx, PlayState* play) {
    GrubActor* g = (GrubActor*)thisx;
    Actor_SetScale(thisx, 0.01f);
    thisx->room = -1;
    ActorShape_Init(&thisx->shape, 0.0f, ActorShadow_DrawCircle, 8.0f);
    Collider_InitCylinder(play, &g->col);
    Collider_SetCylinder(play, &g->col, thisx, &sGrubColInit);
    thisx->colChkInfo.mass = 50;
    thisx->colChkInfo.health = 2;
    thisx->gravity = -1.0f;
    thisx->targetMode = 0;
    g->hp = 2;
    g->frames = static_cast<s16>(Rand_ZeroOne() * 100.0f);
}

static void GrubActor_Destroy(Actor* thisx, PlayState* play) {
    GrubActor* g = (GrubActor*)thisx;
    Collider_DestroyCylinder(play, &g->col);
}

static void GrubActor_Update(Actor* thisx, PlayState* play) {
    GrubActor* g = (GrubActor*)thisx;
    g->frames++;
    if (g->hurt > 0) {
        g->hurt--;
    }
    if (g->recoil > 0) {
        g->recoil--;
    }

    if (g->recoil > 0) {
        thisx->speedXZ = -2.0f;
    } else if (thisx->xzDistToPlayer < 220.0f) {
        Math_ScaledStepToS(&thisx->world.rot.y, thisx->yawTowardsPlayer, 0x280);
        Math_StepToF(&thisx->speedXZ, 1.3f, 0.2f);
    } else {
        thisx->world.rot.y = static_cast<s16>(thisx->world.rot.y + 0x100);
        Math_StepToF(&thisx->speedXZ, 0.4f, 0.1f);
    }
    thisx->shape.rot.y = thisx->world.rot.y;
    Actor_MoveXZGravity(thisx);
    Actor_UpdateBgCheckInfo(play, thisx, 8.0f, 6.0f, 8.0f, 5);

    if ((g->frames % 18) == 0 && thisx->speedXZ > 0.5f) {
        Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_JR_WALK);
    }

    if (g->col.base.acFlags & AC_HIT) {
        g->col.base.acFlags &= ~AC_HIT;
        if (g->hurt == 0) {
            g->hp--;
            g->hurt = 14;
            g->recoil = 8;
            if (g->hp <= 0) {
                Vec3f zero = { 0.0f, 0.0f, 0.0f };
                Vec3f pos = thisx->world.pos;
                pos.y += 4.0f;
                EffectSsDeadDb_Spawn(play, &pos, &zero, &zero, 30, 7, 255, 255, 255, 255, 120, 255, 140, 1, 8, true);
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_JR_DEAD);
                if (Rand_ZeroOne() < 0.5f) {
                    Item_DropCollectible(play, &thisx->world.pos, ITEM00_HEART);
                }
                Actor_Kill(thisx);
                return;
            }
            Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_JR_DAM1);
        }
    }
    if (g->col.base.atFlags & AT_HIT) {
        g->col.base.atFlags &= ~AT_HIT;
        g->recoil = 20;
    }

    Collider_UpdateCylinder(thisx, &g->col);
    CollisionCheck_SetOC(play, &play->colChkCtx, &g->col.base);
    if (g->hurt == 0) {
        CollisionCheck_SetAC(play, &play->colChkCtx, &g->col.base);
    }
    if (g->recoil == 0) {
        CollisionCheck_SetAT(play, &play->colChkCtx, &g->col.base);
    }
    Actor_SetFocus(thisx, 4.0f);
}

static void GrubActor_Draw(Actor* thisx, PlayState* play) {
    GrubActor* g = (GrubActor*)thisx;
    f32 w = Math_SinS(static_cast<s16>(g->frames * 0xC00));
    Matrix_Push();
    Matrix_RotateY(0.18f * w, MTXMODE_APPLY);
    Matrix_Scale(1.0f - 0.05f * w, 1.0f + 0.05f * w, 1.0f + 0.08f * w, MTXMODE_APPLY);
    if (g->hurt > 0 && (g->hurt & 2)) {
        DrawMesh(play, sGrubMesh, 255, 80, 80);
    } else {
        DrawMesh(play, sGrubMesh);
    }
    Matrix_Pop();
}

static s32 CountGrubs(PlayState* play) {
    s32 n = 0;
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next) {
        if (a->id == sGrubId && a->update != nullptr) {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// The Gloomhorn (boss)
// ---------------------------------------------------------------------------------------------------------------------
// It hunts by sight, and its eyes are made for big prey: when Link is tiny it charges blindly at where it last saw
// him, runs right over him and slams into the wall. Stunned on its back, it can be hurt, but only by a full-size
// sword. So: shrink to dodge the charge, grow to strike.

enum BeetleState { BS_SLEEP, BS_WALK, BS_WINDUP, BS_CHARGE, BS_SKID, BS_STUN, BS_RECOVER, BS_DEATH };

struct BeetleActor {
    Actor actor;
    ColliderCylinder col;
    s16 state;
    s16 timer;
    s16 hp;
    s16 hurt;
    s16 frames;
    s16 tickled;
    s16 walkFor;
    f32 flip;
    f32 legSpeed;
};

static ColliderCylinderInit sBeetleColInit = {
    { COLTYPE_METAL, AT_ON | AT_TYPE_ENEMY, AC_ON | AC_TYPE_PLAYER, OC1_ON | OC1_TYPE_ALL, OC2_TYPE_1,
      COLSHAPE_CYLINDER },
    { ELEMTYPE_UNK0,
      { 0xFFCFFFFF, 0x00, 0x10 },
      { 0xFFCFFFFF, 0x00, 0x00 },
      TOUCH_ON | TOUCH_SFX_NORMAL,
      BUMP_ON,
      OCELEM_ON },
    { 55, 70, 18, { 0, 0, 0 } },
};

static s16 sBeetleId = -1;
static Actor* sBeetleActor = nullptr;
constexpr s16 kBeetleHp = 6;

static void Beetle_SetState(BeetleActor* b, s16 state) {
    b->state = state;
    b->timer = 0;
}

static void BeetleActor_Init(Actor* thisx, PlayState* play) {
    BeetleActor* b = (BeetleActor*)thisx;
    Actor_SetScale(thisx, 0.1f);
    thisx->room = -1;
    ActorShape_Init(&thisx->shape, 0.0f, ActorShadow_DrawCircle, 90.0f);
    Collider_InitCylinder(play, &b->col);
    Collider_SetCylinder(play, &b->col, thisx, &sBeetleColInit);
    thisx->colChkInfo.mass = MASS_IMMOVABLE;
    thisx->colChkInfo.health = static_cast<u8>(kBeetleHp);
    thisx->gravity = -2.0f;
    thisx->targetMode = 5;
    b->hp = kBeetleHp;
    Beetle_SetState(b, BS_SLEEP);
    sBeetleActor = thisx;
}

static void BeetleActor_Destroy(Actor* thisx, PlayState* play) {
    BeetleActor* b = (BeetleActor*)thisx;
    Collider_DestroyCylinder(play, &b->col);
    if (sBeetleActor == thisx) {
        sBeetleActor = nullptr;
    }
}

static bool PlayerInArena(PlayState* play) {
    Player* player = GET_PLAYER(play);
    Vec3f l = Local(player->actor.world.pos);
    const Rootworks::Zone& a = Rootworks::kArena;
    return l.x > a.x0 && l.x < a.x1 && l.z > a.z0 && l.z < a.z1 - 30.0f;
}

static void StartBossMusic() {
    Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_BOSS);
}

static void BeetleActor_Update(Actor* thisx, PlayState* play) {
    BeetleActor* b = (BeetleActor*)thisx;
    Player* player = GET_PLAYER(play);
    bool bigLink = Minish_IsNormalSize();
    b->frames++;
    b->timer++;
    if (b->hurt > 0) {
        b->hurt--;
    }

    switch (b->state) {
        case BS_SLEEP:
            thisx->speedXZ = 0.0f;
            if (PlayerInArena(play) && Has(Q_BOSS_SEEN) && !sConv.active) {
                StartBossMusic();
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_CRY1);
                Beetle_SetState(b, BS_WALK);
                b->walkFor = 50;
            }
            break;
        case BS_WALK:
            Math_ScaledStepToS(&thisx->world.rot.y, thisx->yawTowardsPlayer, 0x220);
            Math_StepToF(&thisx->speedXZ, 2.4f, 0.3f);
            b->legSpeed = 1.0f;
            if ((b->frames % 12) == 0) {
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_WALK);
            }
            if (b->timer > b->walkFor) {
                Beetle_SetState(b, BS_WINDUP);
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_UNARI);
            }
            break;
        case BS_WINDUP:
            Math_StepToF(&thisx->speedXZ, 0.0f, 0.6f);
            if (bigLink) {
                Math_ScaledStepToS(&thisx->world.rot.y, thisx->yawTowardsPlayer, 0x600);
            }
            b->legSpeed = 2.5f;
            if ((b->timer % 6) == 0) {
                Vec3f pos = thisx->world.pos;
                Actor_SpawnFloorDustRing(play, thisx, &pos, 40.0f, 3, 4.0f, 120, 10, false);
            }
            if (b->timer >= 34) {
                Beetle_SetState(b, BS_CHARGE);
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_CRY1);
            }
            break;
        case BS_CHARGE:
            Math_StepToF(&thisx->speedXZ, 13.0f, 1.5f);
            if (bigLink) {
                Math_ScaledStepToS(&thisx->world.rot.y, thisx->yawTowardsPlayer, 0x170); // homes in on big prey
            }
            b->legSpeed = 3.0f;
            if ((b->frames % 4) == 0) {
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_WALK);
            }
            if (thisx->bgCheckFlags & BGCHECKFLAG_WALL) {
                // Slammed into the wall: flipped on its back
                Beetle_SetState(b, BS_STUN);
                thisx->speedXZ = 0.0f;
                b->tickled = 0;
                b->col.base.colType = COLTYPE_HIT0;
                b->col.dim.yShift = 0;
                b->col.dim.height = 45;
                Audio_PlayActorSound2(thisx, NA_SE_EV_STONE_BOUND);
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_DOWN);
                s16 quake = Quake_Add(GET_ACTIVE_CAM(play), 3);
                Quake_SetSpeed(quake, 20000);
                Quake_SetQuakeValues(quake, 6, 0, 0, 0);
                Quake_SetCountdown(quake, 12);
                Rumble_Request(0.0f, 255, 20, 150);
            } else if (b->col.base.atFlags & AT_HIT) {
                b->col.base.atFlags &= ~AT_HIT;
                Beetle_SetState(b, BS_SKID);
            } else if (b->timer > 80) {
                Beetle_SetState(b, BS_SKID);
            }
            break;
        case BS_SKID:
            Math_StepToF(&thisx->speedXZ, 0.0f, 0.8f);
            b->legSpeed = 1.0f;
            if (b->timer > 30) {
                Beetle_SetState(b, BS_WALK);
                b->walkFor = static_cast<s16>(50 + Rand_ZeroOne() * 40.0f);
            }
            break;
        case BS_STUN:
            Math_StepToF(&b->flip, 1.0f, 0.12f);
            thisx->speedXZ = 0.0f;
            b->legSpeed = 4.0f;
            if (b->timer > 130) {
                Beetle_SetState(b, BS_RECOVER);
            }
            break;
        case BS_RECOVER:
            Math_StepToF(&b->flip, 0.0f, 0.06f);
            b->legSpeed = 2.0f;
            if (b->flip <= 0.0f) {
                b->col.base.colType = COLTYPE_METAL;
                b->col.dim.yShift = 18;
                b->col.dim.height = 70;
                Beetle_SetState(b, BS_WALK);
                b->walkFor = static_cast<s16>(40 + Rand_ZeroOne() * 40.0f);
            }
            break;
        case BS_DEATH:
            thisx->speedXZ = 0.0f;
            thisx->shape.rot.y = static_cast<s16>(thisx->shape.rot.y + 0x1000);
            if ((b->timer % 5) == 0) {
                Vec3f pos = thisx->world.pos;
                pos.x += Rand_CenteredFloat(80.0f);
                pos.y += 20.0f + Rand_ZeroOne() * 50.0f;
                pos.z += Rand_CenteredFloat(80.0f);
                Vec3f zero = { 0.0f, 0.0f, 0.0f };
                EffectSsDeadDb_Spawn(play, &pos, &zero, &zero, 120, 10, 255, 255, 255, 255, 120, 255, 140, 1, 9, true);
            }
            if (b->timer >= 70) {
                Set(Q_BOSS);
                Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_BOSS_CLEAR);
                Hud("The Gloomhorn is gone! Its gloom fades away...", 90);
                Actor_Kill(thisx);
                return;
            }
            break;
    }

    // Getting hit
    if (b->col.base.acFlags & AC_HIT) {
        b->col.base.acFlags &= ~AC_HIT;
        if (b->state == BS_STUN && b->hurt == 0) {
            if (bigLink) {
                b->hp--;
                b->hurt = 12;
                Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_DAM1);
                if (b->hp <= 0) {
                    Beetle_SetState(b, BS_DEATH);
                    Audio_PlayActorSound2(thisx, NA_SE_EN_GOMA_DEAD);
                    Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_STOP);
                }
            } else if (!b->tickled) {
                b->tickled = 1;
                Hud("Your tiny sword only tickles it! Grow big to strike!", 90);
            }
        }
    }

    if (b->state != BS_DEATH) {
        thisx->shape.rot.y = thisx->world.rot.y;
    }
    Actor_MoveXZGravity(thisx);
    Actor_UpdateBgCheckInfo(play, thisx, 40.0f, 62.0f, 60.0f, 5);

    Collider_UpdateCylinder(thisx, &b->col);
    if (b->state != BS_DEATH) {
        CollisionCheck_SetOC(play, &play->colChkCtx, &b->col.base);
        CollisionCheck_SetAC(play, &play->colChkCtx, &b->col.base);
        if (b->state == BS_WALK || b->state == BS_CHARGE || b->state == BS_WINDUP) {
            CollisionCheck_SetAT(play, &play->colChkCtx, &b->col.base);
        }
    }
    Actor_SetFocus(thisx, b->state == BS_STUN ? 25.0f : 50.0f);
    (void)player;
}

static void BeetleActor_Draw(Actor* thisx, PlayState* play) {
    BeetleActor* b = (BeetleActor*)thisx;
    u8 r = 255, g = 255, bl = 255;
    if (b->hurt > 0 && (b->hurt & 2)) {
        g = bl = 80;
    }
    if (b->state == BS_DEATH && (b->timer & 2)) {
        r = g = bl = 255;
        g = 180;
    }
    f32 shrink = (b->state == BS_DEATH) ? std::max(0.05f, 1.0f - static_cast<f32>(b->timer) / 75.0f) : 1.0f;
    f32 shake = (b->state == BS_WINDUP) ? 6.0f * Math_SinS(static_cast<s16>(b->frames * 0x4000)) : 0.0f;

    Matrix_Push();
    Matrix_Scale(shrink, shrink, shrink, MTXMODE_APPLY);
    Matrix_Translate(shake, 0.0f, 0.0f, MTXMODE_APPLY);
    if (b->flip > 0.0f) {
        // roll onto its back around its middle
        Matrix_Translate(0.0f, 450.0f, 0.0f, MTXMODE_APPLY);
        Matrix_RotateZ(b->flip * kPi, MTXMODE_APPLY);
        Matrix_Translate(0.0f, -450.0f + 100.0f * b->flip, 0.0f, MTXMODE_APPLY);
    }
    DrawMesh(play, sBeetleBody, r, g, bl);
    DrawMesh(play, sBeetleBelly, r, g, bl);
    f32 phase = static_cast<f32>(b->frames) * 0.35f * b->legSpeed;
    s32 i = 0;
    for (f32 z : { -380.0f, 0.0f, 380.0f }) {
        for (f32 side : { -1.0f, 1.0f }) {
            f32 swing = 0.35f * sinf(phase + static_cast<f32>(i) * 1.7f);
            Matrix_Push();
            Matrix_Translate(side * 400.0f, 330.0f, z, MTXMODE_APPLY);
            Matrix_RotateY(side < 0.0f ? kPi : 0.0f, MTXMODE_APPLY);
            Matrix_RotateY(swing, MTXMODE_APPLY);
            Matrix_RotateZ(0.15f * sinf(phase * 1.3f + static_cast<f32>(i)), MTXMODE_APPLY);
            DrawMesh(play, sBeetleLeg, r, g, bl);
            Matrix_Pop();
            i++;
        }
    }
    Matrix_Pop();
}

constexpr u32 kFlagTargetable = 1 << 0;
constexpr u32 kFlagHostile = 1 << 2;
constexpr u32 kFlagFriendly = 1 << 3;

static void RegisterCharacterActors() {
    if (sPipkinId >= 0) {
        return;
    }
    sPipkinId = RegisterActor("Pipkin", ACTORCAT_NPC, kFlagTargetable | kFlagFriendly | kNoCull, sizeof(PipkinActor),
                              PipkinActor_Init, PipkinActor_Destroy, PipkinActor_Update, PipkinActor_Draw);
    sGrubId = RegisterActor("Gloomgrub", ACTORCAT_ENEMY, kFlagTargetable | kFlagHostile | (1 << 4), sizeof(GrubActor),
                            GrubActor_Init, GrubActor_Destroy, GrubActor_Update, GrubActor_Draw);
    sBeetleId =
        RegisterActor("Gloomhorn", ACTORCAT_ENEMY, kFlagTargetable | kFlagHostile | kNoCull, sizeof(BeetleActor),
                      BeetleActor_Init, BeetleActor_Destroy, BeetleActor_Update, BeetleActor_Draw);
}

// ---------------------------------------------------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------------------------------------------------

static s32 sSceneFrames = 0;
static s32 sEnteredStump = -1;   // the stump you came in through
static s32 sPendingArrival = -1; // stump travel to another area: put Link at this stump once it loads
static s16 sGrowIn = 0;          // grow back this many frames from now
static bool sStumpSpawnTried = false;
static bool sDoorHintShown = false;
static bool sGrubsSpawned = false;
static Actor* sWorldPip = nullptr;
static s16 sGateMsgCooldown = 0;
static s16 sSwitchMsgCooldown = 0;
static s16 sDoorCooldown = 0; // after coming out of a stump (or failing to get in), don't walk right back in
static bool sHollowPocketValid = false;
static Vec3f sHollowPocket = { 0.0f, 0.0f, 0.0f };
static bool sRwPocketValid = false;
static Vec3f sRwPocket = { 0.0f, 0.0f, 0.0f };

struct StumpSearch {
    bool active = false;
    bool done = false;
    s32 ring = 0;
    Vec3f center = { 0.0f, 0.0f, 0.0f };
};
static StumpSearch sSearch;

enum WarpKind { W_NONE, W_STUMP_TO_HOLLOW, W_HOLLOW_TO_STUMP, W_HOLLOW_TO_RW, W_RW_TO_HOLLOW, W_HEARTSEED };
struct WarpState {
    WarpKind kind = W_NONE;
    s32 timer = 0;
    s32 stump = -1;
};
static WarpState sWarp;
constexpr s32 kFade = 10;

static void ResetRuntime() {
    sLoc = LOC_WORLD;
    sAreaActor = nullptr;
    for (Actor*& a : sDoorActors) {
        a = nullptr;
    }
    for (Actor*& a : sNpcActors) {
        a = nullptr;
    }
    sBeetleActor = nullptr;
    sStumpActor = nullptr;
    sWorldPip = nullptr;
    sWarp = WarpState();
    sConv = Conversation();
    sSceneFrames = 0;
    sStumpSpawnTried = false;
    sDoorHintShown = false;
    sGrubsSpawned = false;
    sHollowPocketValid = false;
    sRwPocketValid = false;
    sSearch = StumpSearch();
    sGrowIn = 0;
    sDoorCooldown = 0;
}

static s32 FoundStumps() {
    s32 n = 0;
    for (s32 i = 0; i < kNumStumps; i++) {
        n += (sSave.stumpFound >> i) & 1;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// Dialogue
// ---------------------------------------------------------------------------------------------------------------------

static Script PickScript(s32 npc) {
    switch (npc) {
        case NPC_ELDER:
            if (!Has(Q_MET_ELDER)) {
                return SCR_ELDER_INTRO;
            }
            if (Has(Q_REWARD)) {
                return SCR_ELDER_AFTER;
            }
            if (Has(Q_HEARTSEED)) {
                return SCR_ELDER_REWARD;
            }
            if (Has(Q_GATE_OPEN)) {
                return SCR_ELDER_GATE_HINT;
            }
            if (Has(Q_SEEDS_DONE) && Has(Q_PIP_DONE) && Has(Q_BUG_DONE)) {
                return SCR_ELDER_OPEN_GATE;
            }
            return SCR_ELDER_PROGRESS;
        case NPC_TANSY:
            if (!Has(Q_SEEDS_START)) {
                return SCR_TANSY_START;
            }
            if (!Has(Q_SEEDS_DONE)) {
                return SeedCount() >= 5 ? SCR_TANSY_DONE : SCR_TANSY_PROGRESS;
            }
            return SCR_TANSY_AFTER;
        case NPC_MOSS:
            if (!Has(Q_MET_ELDER)) {
                return SCR_MOSS_WAIT_ELDER;
            }
            if (!Has(Q_PIP_START)) {
                return SCR_MOSS_START;
            }
            if (!Has(Q_PIP_FOUND)) {
                return SCR_MOSS_WAIT_PIP;
            }
            if (!Has(Q_PIP_DONE)) {
                return SCR_MOSS_PIP_HOME;
            }
            return Has(Q_GATE_OPEN) ? SCR_MOSS_GATE_OPEN : SCR_MOSS_GUARD;
        case NPC_PIP:
            return (sLoc == LOC_WORLD) ? SCR_PIP_LOST : SCR_PIP_HOME;
        case NPC_BURL:
            if (!Has(Q_BUG_START)) {
                return SCR_BURL_START;
            }
            if (!Has(Q_BUG_DONE)) {
                return Inventory_HasSpecificBottle(ITEM_BUG) ? SCR_BURL_ASK : SCR_BURL_NO_BUG;
            }
            return SCR_BURL_AFTER;
        default:
            return SCR_NONE;
    }
}

static std::string ElderTodo() {
    std::vector<std::string> todo;
    if (!Has(Q_SEEDS_DONE)) {
        todo.push_back("%gTansy's%w seedlings");
    }
    if (!Has(Q_PIP_DONE)) {
        todo.push_back(Has(Q_PIP_START) ? "finding Pip for %gMoss%w" : "%gMoss%w at the north gate");
    }
    if (!Has(Q_BUG_DONE)) {
        todo.push_back("a bug for %gBurl%w");
    }
    std::string s;
    for (size_t i = 0; i < todo.size(); i++) {
        if (i > 0) {
            s += (i + 1 == todo.size()) ? " and " : ", ";
        }
        s += todo[i];
    }
    return s;
}

static std::string ScriptText(Script script) {
    const std::string two = CustomMessage::TWO_WAY_CHOICE();
    const std::string three = CustomMessage::THREE_WAY_CHOICE();
    switch (script) {
        case SCR_ELDER_INTRO:
            return "%gElder Burdock:%w Well, well! A giant who can shrink down to our size! Welcome to %yPipkin "
                   "Hollow%w, young one.^"
                   "We Pipkins live in the roots beneath Hyrule. Our hollow stumps let us peek out at the big world.^"
                   "Since the Dark King fell, the land above has healed... but down here, a %rgloom%w has crept up "
                   "from the deep roots.^"
                   "It swallowed our %yHeartseed%w, the seed that keeps our roots alive, and dragged it down into the "
                   "%rRootworks%w.^"
                   "I won't send you down there unprepared. And my folk need help too: %gTansy%w, %gBurl%w, and "
                   "%gMoss%w at the north gate.^"
                   "Help all three, then come back to me, and I'll have the Rootworks gate opened.";
        case SCR_ELDER_PROGRESS:
            return "%gElder Burdock:%w The Rootworks gate stays shut until you've helped my folk.^Still to do: " +
                   ElderTodo() + ".";
        case SCR_ELDER_OPEN_GATE:
            return "%gElder Burdock:%w You've helped every one of them! The whole Hollow is talking about the kind "
                   "giant.^"
                   "Moss! Open the %rRootworks%w gate!^"
                   "Down there, roots twist into tunnels only a Pipkin could squeeze through, and doors only a giant "
                   "could budge.^"
                   "You'll need to be both, young one. Shrink, grow, and bring back our %yHeartseed%w!";
        case SCR_ELDER_GATE_HINT:
            return "%gElder Burdock:%w The Rootworks lie beyond the north gate.^"
                   "Tiny tunnels for tiny feet, heavy switches for heavy giants. And if you fall... glide!";
        case SCR_ELDER_REWARD:
            return "%gElder Burdock:%w The Heartseed! You brought it home!^"
                   "Listen... the roots are humming again. Every stump in Hyrule will be green by spring.^"
                   "Take this, young one. A Pipkin's thanks for a giant's heart.^"
                   "...There are other hollows out there. Other roots, other folk. When they need you, we'll send "
                   "word.";
        case SCR_ELDER_AFTER:
            return "%gElder Burdock:%w The Heartseed hums in my tree now. Come by any time, young giant. The Hollow "
                   "is your home too.";
        case SCR_TANSY_START:
            return "%gTansy:%w Oh no, oh no, oh no! My %ylittle seedlings%w rolled out of their beds last night!^"
                   "There were five of them, glowing green. They're scattered all over the Hollow: up on rocks, down "
                   "in holes, behind things...^"
                   "Could you find them for me? You're small enough now to reach anywhere!";
        case SCR_TANSY_PROGRESS:
            return "%gTansy:%w You've found %y" + std::to_string(SeedCount()) +
                   " of 5%w seedlings! Keep looking: try high places, tunnels, and behind the Elder's tree.";
        case SCR_TANSY_DONE:
            return "%gTansy:%w All five! Oh, they're glowing so happily!^"
                   "Here, let me teach you something. When a dandelion seed falls, it floats...^"
                   "%gYou learned the Dandelion Glide!%w While tiny, hold %rA%w in the air to float down slowly and "
                   "drift far.";
        case SCR_TANSY_AFTER:
            return "%gTansy:%w My garden's never been greener. Don't forget: hold %rA%w while falling to glide!";
        case SCR_MOSS_WAIT_ELDER:
            return "%gMoss:%w Halt! Who goes... oh. A giant. A small giant? Go talk to %gElder Burdock%w by the big "
                   "tree first.";
        case SCR_MOSS_START:
            return "%gMoss:%w You're the shrinking giant the Elder told us about! Please, I need your help!^"
                   "My little brother %yPip%w snuck out through one of our stumps, the one by %bLake Hylia%w!^"
                   "The big world is so dangerous for a Pipkin. Fish! Birds! Giants! Please find him and send him "
                   "home!^"
                   "Oh, and he's scared of giants. You'll have to be %rtiny%w to talk to him.";
        case SCR_MOSS_WAIT_PIP:
            return "%gMoss:%w Pip went out through the stump at %bLake Hylia%w. Be tiny when you find him, or he'll "
                   "hide!";
        case SCR_MOSS_PIP_HOME:
            return "%gMoss:%w Pip's home! Safe and sound! Thank you, thank you!^"
                   "Take this, the %yRootway Charm%w. All our stumps are connected by roots, you know.^"
                   "%gYou got the Rootway Charm!%w When you leave through the exit ring, you can come out at any "
                   "stump you've visited.";
        case SCR_MOSS_GUARD:
            return "%gMoss:%w I guard the Rootworks gate. Only the Elder can tell me to open it.";
        case SCR_MOSS_GATE_OPEN:
            return "%gMoss:%w The gate is open. Be careful down there... the gloom makes the bugs big and mean.";
        case SCR_PIP_LOST:
            return "%gPip:%w Wow, you're tiny like me! I just wanted to see the lake... it's SO big and SO blue!^"
                   "But there's a fish out there with HUGE teeth, and now I'm too scared to go back by myself.^"
                   "...Moss sent you? Then I'm going home right now! Bye, tiny giant!";
        case SCR_PIP_HOME:
            return "%gPip:%w The lake was amazing! But I'm never going out alone again. Moss hugged me so hard my "
                   "sprout almost fell off!";
        case SCR_BURL_START:
            return "%gBurl:%w Burl's the name, beetles are my game! Well... beetle. Just the one, now.^"
                   "Since the gloom came, the wild bugs have all gone mean. My poor beetle's lonely in that big "
                   "pen.^"
                   "You giants carry bugs around in %ybottles%w, don't you? Bring me a nice plump one!";
        case SCR_BURL_ASK:
            return "%gBurl:%w Is that a %ybug%w in your bottle? Let it go in my pen?&" + two + "%gYes&No%w";
        case SCR_BURL_THANKS:
            return "%gBurl:%w Look at those legs! Look at that shine! My beetle's got a friend!^"
                   "Here, take these rupees I dug up. Giants like shiny things, right?";
        case SCR_BURL_NO:
            return "%gBurl:%w Hmph. Bugs are happier in a pen than in a bottle, you know.";
        case SCR_BURL_NO_BUG:
            return "%gBurl:%w No bug yet? Look under rocks in the big world and catch one with an %yempty bottle%w.";
        case SCR_BURL_AFTER:
            return "%gBurl:%w Look at them go! Best beetles in the Hollow, and that's a fact.";
        case SCR_SYS_INTRO:
            return "Since the Dark King's defeat, strange %ghollow stumps%w have sprouted across Hyrule.^"
                   "Each one has a tiny door at its base... much too small for you.^"
                   "But you can shrink! Look for the stumps in %gKokiri Forest%w, the %gLost Woods%w, %gHyrule "
                   "Field%w, %gKakariko Village%w and %gLake Hylia%w.";
        case SCR_SYS_LEAVE_HOLLOW: {
            s32 s = (sEnteredStump >= 0) ? sEnteredStump : 0;
            return std::string("Leave through the %g") + kStumps[s].name + "%w stump?&" + two + "%gYes&No%w";
        }
        case SCR_SYS_TRAVEL:
            return std::string("Come out at the %g") + kStumps[sConv.travelPick].name + "%w stump?&" + three +
                   "%gGo there&Next stump&Stay%w";
        case SCR_SYS_LEAVE_RW:
            return "Leave the Rootworks and go back to Pipkin Hollow?&" + two + "%gYes&No%w";
        case SCR_SYS_RW_INTRO:
            return "%rThe Rootworks%w^"
                   "Roots twist in every direction. Some tunnels are barely wider than a Pipkin, and some doors are "
                   "far "
                   "too heavy for one.^"
                   "To leave, stand on the glowing pad and press the Minish button.";
        case SCR_SYS_BOSS_INTRO:
            return "%rGLOOMHORN%w, the Gloom-Swollen Beetle^"
                   "Its glowing eyes lock onto anything big that moves... but would they even notice something tiny?";
        case SCR_SYS_HEARTSEED:
            return "%gYou got the Heartseed!%w^It's warm and pulses gently, like a tiny heart. Bring it home to "
                   "%gElder Burdock%w!";
        default:
            return "...";
    }
}

static void SysMessage(PlayState* play, Script script, s32 travelPick = 0) {
    sConv = Conversation();
    sConv.active = true;
    sConv.npc = -1;
    sConv.script = script;
    sConv.travelPick = travelPick;
    sBoxText = ScriptText(script);
    Message_StartTextbox(play, kTextSystem, nullptr);
}

static bool sContinuing = false;

static void Continue(PlayState* play, Script script) {
    sConv.script = script;
    sBoxText = ScriptText(script);
    sContinuing = true;
    Message_ContinueTextbox(play, static_cast<u16>(sConv.npc >= 0 ? kTextNpcBase + sConv.npc : kTextSystem));
}

static void OnStoryOpenText(uint16_t* textId, bool* loadFromMessageTable) {
    u16 id = *textId;
    if (id >= kTextNpcBase && id < kTextNpcBase + NPC_COUNT) {
        s32 npc = id - kTextNpcBase;
        if (!(sContinuing && sConv.active && sConv.npc == npc)) {
            sConv = Conversation();
            sConv.active = true;
            sConv.npc = npc;
            sConv.script = PickScript(npc);
            sBoxText = ScriptText(sConv.script);
        }
        sContinuing = false;
        BuildBox(loadFromMessageTable);
    } else if (id == kTextSystem && sConv.active) {
        sContinuing = false;
        BuildBox(loadFromMessageTable);
    }
}

static void StartWarp(WarpKind kind, s32 stump);
static void ItemFanfare() {
    Audio_PlayFanfare(NA_BGM_ITEM_GET | 0x900);
}

static void ConversationEnded(PlayState* play, Script script) {
    switch (script) {
        case SCR_ELDER_INTRO:
            Set(Q_MET_ELDER);
            break;
        case SCR_ELDER_OPEN_GATE:
            Set(Q_GATE_OPEN);
            PlaySysSfx(NA_SE_SY_CORRECT_CHIME);
            Hud("The Rootworks gate is open! (north side of the Hollow)", 90);
            break;
        case SCR_ELDER_REWARD:
            Set(Q_REWARD);
            Item_Give(play, ITEM_HEART_CONTAINER);
            Audio_PlayFanfare(NA_BGM_HEART_GET | 0x900);
            Hud("You got a Heart Container! Chapter 1 complete!", 150);
            break;
        case SCR_TANSY_START:
            Set(Q_SEEDS_START);
            break;
        case SCR_TANSY_DONE:
            Set(Q_SEEDS_DONE);
            ItemFanfare();
            break;
        case SCR_MOSS_START:
            Set(Q_PIP_START);
            break;
        case SCR_MOSS_PIP_HOME:
            Set(Q_PIP_DONE);
            ItemFanfare();
            break;
        case SCR_PIP_LOST:
            Set(Q_PIP_FOUND);
            if (sWorldPip != nullptr) {
                Vec3f zero = { 0.0f, 0.0f, 0.0f };
                Vec3f pos = sWorldPip->world.pos;
                pos.y += 3.0f;
                static Color_RGBA8 sPrim = { 255, 255, 220, 255 };
                static Color_RGBA8 sEnv = { 80, 220, 120, 255 };
                for (s32 i = 0; i < 6; i++) {
                    Vec3f p = pos;
                    p.x += Rand_CenteredFloat(8.0f);
                    p.z += Rand_CenteredFloat(8.0f);
                    EffectSsKiraKira_SpawnDispersed(play, &p, &zero, &zero, &sPrim, &sEnv, 400, 12);
                }
                Actor_Kill(sWorldPip);
                sWorldPip = nullptr;
            }
            PlaySysSfx(NA_SE_EV_FAIRY_DASH);
            Hud("Pip scampers through the tiny door, heading home!", 90);
            break;
        case SCR_BURL_START:
            Set(Q_BUG_START);
            break;
        case SCR_BURL_THANKS:
            Set(Q_BUG_DONE);
            Rupees_ChangeBy(100);
            PlaySysSfx(NA_SE_SY_GET_RUPY);
            Hud("You got 100 rupees!", 60);
            break;
        case SCR_SYS_INTRO:
            Set(Q_INTRO);
            break;
        case SCR_SYS_HEARTSEED:
            StartWarp(W_HEARTSEED, -1);
            break;
        default:
            break;
    }
}

static s32 NextFoundStump(s32 from) {
    for (s32 k = 1; k <= kNumStumps; k++) {
        s32 i = (from + k) % kNumStumps;
        if ((sSave.stumpFound >> i) & 1) {
            return i;
        }
    }
    return from;
}

static void HandleChoice(PlayState* play, u8 choice) {
    switch (sConv.script) {
        case SCR_BURL_ASK:
            if (choice == 0) {
                Inventory_ReplaceItem(play, ITEM_BUG, ITEM_BOTTLE);
                Continue(play, SCR_BURL_THANKS);
            } else {
                Continue(play, SCR_BURL_NO);
            }
            break;
        case SCR_SYS_LEAVE_HOLLOW:
            Message_CloseTextbox(play);
            sConv.script = SCR_NONE;
            if (choice == 0) {
                StartWarp(W_HOLLOW_TO_STUMP, sEnteredStump >= 0 ? sEnteredStump : 0);
            }
            break;
        case SCR_SYS_TRAVEL:
            if (choice == 1) {
                sConv.travelPick = NextFoundStump(sConv.travelPick);
                Continue(play, SCR_SYS_TRAVEL);
            } else {
                Message_CloseTextbox(play);
                s32 pick = sConv.travelPick;
                sConv.script = SCR_NONE;
                if (choice == 0) {
                    StartWarp(W_HOLLOW_TO_STUMP, pick);
                }
            }
            break;
        case SCR_SYS_LEAVE_RW:
            Message_CloseTextbox(play);
            sConv.script = SCR_NONE;
            if (choice == 0) {
                StartWarp(W_RW_TO_HOLLOW, -1);
            }
            break;
        default:
            break;
    }
}

static void UpdateConversation(PlayState* play) {
    if (!sConv.active) {
        return;
    }
    MessageContext* msgCtx = &play->msgCtx;
    if (Message_GetState(msgCtx) == TEXT_STATE_CHOICE && Message_ShouldAdvance(play)) {
        HandleChoice(play, msgCtx->choiceIndex);
        return;
    }
    if (msgCtx->msgMode == MSGMODE_NONE) {
        Script ended = sConv.script;
        sConv = Conversation();
        ConversationEnded(play, ended);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Spawning the areas
// ---------------------------------------------------------------------------------------------------------------------

static void KillTracked(Actor*& a) {
    if (a != nullptr) {
        Actor_Kill(a);
        a = nullptr;
    }
}

static void DespawnArea(PlayState* play) {
    KillTracked(sAreaActor);
    for (Actor*& d : sDoorActors) {
        KillTracked(d);
    }
    for (Actor*& n : sNpcActors) {
        if (n != nullptr && n != sWorldPip) {
            Actor_Kill(n);
        }
        n = nullptr;
    }
    if (sBeetleActor != nullptr) {
        Actor_Kill(sBeetleActor);
        sBeetleActor = nullptr;
    }
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next) {
        if (a->id == sGrubId) {
            Actor_Kill(a);
        }
    }
    sGrubsSpawned = false;
}

static bool SpawnHollow(PlayState* play) {
    if (!DynaBudgetFor(play, sHollowMesh)) {
        return false;
    }
    if (!sHollowPocketValid) {
        sHollowPocket = FindPocket(play, kHollowBounds);
        sHollowPocketValid = true;
    }
    sOrigin = sHollowPocket;
    sAreaActor = Actor_Spawn(&play->actorCtx, play, sAreaId, sOrigin.x, sOrigin.y, sOrigin.z, 0, 0, 0, AREA_HOLLOW);
    if (sAreaActor == nullptr) {
        return false;
    }
    for (s32 i = 0; i < NPC_COUNT; i++) {
        if (i == NPC_PIP && !Has(Q_PIP_FOUND)) {
            continue;
        }
        Actor* a = SpawnPipkin(play, i, World(Hollow::kNpcSpots[i].pos), Hollow::kNpcSpots[i].yaw);
        sNpcActors[i] = a;
    }
    return true;
}

static bool SpawnRootworks(PlayState* play) {
    if (!DynaBudgetFor(play, sRootworksMesh)) {
        return false;
    }
    if (!sRwPocketValid) {
        sRwPocket = FindPocket(play, kRootworksBounds);
        sRwPocketValid = true;
    }
    sOrigin = sRwPocket;
    sAreaActor = Actor_Spawn(&play->actorCtx, play, sAreaId, sOrigin.x, sOrigin.y, sOrigin.z, 0, 0, 0, AREA_ROOTWORKS);
    if (sAreaActor == nullptr) {
        return false;
    }
    for (s32 d = 0; d < DOOR_COUNT; d++) {
        if (!DoorShouldOpen(d)) {
            Vec3f p = World(kDoorPos[d]);
            sDoorActors[d] = Actor_Spawn(&play->actorCtx, play, sDoorId, p.x, p.y, p.z, 0, 0, 0, static_cast<s16>(d));
        }
    }
    if (!Has(Q_GRUBS)) {
        for (const Vec3f& g : Rootworks::kGrubSpawns) {
            Vec3f p = World(g);
            Actor_Spawn(&play->actorCtx, play, sGrubId, p.x, p.y + 2.0f, p.z, 0,
                        static_cast<s16>(Rand_ZeroOne() * 65535.0f), 0, 0);
        }
        sGrubsSpawned = true;
    }
    if (!Has(Q_BOSS)) {
        Vec3f p = World(Rootworks::kBossSpawn);
        sBeetleActor = Actor_Spawn(&play->actorCtx, play, sBeetleId, p.x, p.y + 2.0f, p.z, 0, 0, 0, 0);
    }
    return true;
}

static void PlaceLink(PlayState* play, Vec3f pos, s16 yaw) {
    Player* player = GET_PLAYER(play);
    player->actor.world.pos = pos;
    player->actor.prevPos = pos;
    player->actor.home.pos = pos;
    player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw = yaw;
    player->linearVelocity = 0.0f;
    player->actor.speedXZ = 0.0f;
    player->actor.velocity = V3(0.0f, 0.0f, 0.0f);
    player->fallStartHeight = static_cast<s16>(pos.y);
    if (play->cameraPtrs[0] != nullptr) {
        Camera_InitPlayerSettings(play->cameraPtrs[0], player);
    }
    FrameInterpolation_DontInterpolateCamera();
}

static void PlayAreaMusic(u16 seq) {
    Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | seq);
}

static void RestoreSceneMusic(PlayState* play) {
    gSaveContext.seqId = static_cast<u8>(NA_BGM_DISABLED);
    gSaveContext.natureAmbienceId = 0xFF;
    Environment_PlaySceneSequence(play);
}

static Vec3f StumpFront(PlayState* play, s32 i) {
    Vec3f p = Add(StumpPos(i), RotY(V3(0, 0, 75), sSave.stumpYaw[i]));
    CollisionPoly* poly = nullptr;
    f32 y = FloorAt(play, p.x, p.y + 60.0f, p.z, &poly);
    if (poly != nullptr) {
        p.y = y;
    }
    return p;
}

static void StartWarp(WarpKind kind, s32 stump) {
    if (sWarp.kind != W_NONE) {
        return;
    }
    sWarp.kind = kind;
    sWarp.timer = 0;
    sWarp.stump = stump;
    PlaySysSfx(kind == W_STUMP_TO_HOLLOW || kind == W_HOLLOW_TO_RW ? NA_SE_OC_SECRET_WARP_IN
                                                                   : NA_SE_OC_SECRET_WARP_OUT);
}

static void UpdateWarp(PlayState* play, Player* player) {
    if (sWarp.kind == W_NONE) {
        return;
    }
    sWarp.timer++;
    s32 t = sWarp.timer;
    s32 alpha = 255;
    if (t <= kFade) {
        alpha = t * 255 / kFade;
    } else if (t > kFade * 2) {
        alpha = std::max(0, 255 - (t - kFade * 2) * 255 / kFade);
    }
    play->envCtx.fillScreen = true;
    play->envCtx.screenFillColor[0] = 0;
    play->envCtx.screenFillColor[1] = 0;
    play->envCtx.screenFillColor[2] = 0;
    play->envCtx.screenFillColor[3] = static_cast<u8>(alpha);

    WarpKind kind = sWarp.kind;
    if (t == kFade) {
        // Step 1: clear out the area you're leaving
        if (kind != W_STUMP_TO_HOLLOW) {
            DespawnArea(play);
        }
        if (kind == W_HOLLOW_TO_STUMP && kStumps[sWarp.stump].scene != play->sceneNum) {
            // The stump is in another area: load it, and come out at the stump once it's there
            sPendingArrival = sWarp.stump;
            sLoc = LOC_WORLD;
            play->nextEntranceIndex = kStumps[sWarp.stump].entrance;
            play->transitionTrigger = TRANS_TRIGGER_START;
            play->transitionType = TRANS_TYPE_FADE_BLACK;
            gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK;
            sWarp = WarpState();
            return;
        }
    } else if (t == kFade + 1) {
        // Step 2: build the area you're going to and put Link there
        switch (kind) {
            case W_STUMP_TO_HOLLOW:
                if (!SpawnHollow(play)) {
                    Hud("The roots here are too tangled to get through. Try another stump.", 90);
                    sDoorCooldown = 150;
                    break;
                }
                sLoc = LOC_HOLLOW;
                sEnteredStump = sWarp.stump;
                sSave.stumpFound |= static_cast<u8>(1 << sWarp.stump);
                PlaceLink(play, World(Hollow::kArrive), static_cast<s16>(-0x8000));
                Minish_SetTinyNow(player);
                PlayAreaMusic(NA_BGM_KOKIRI);
                break;
            case W_HOLLOW_TO_STUMP:
                sLoc = LOC_WORLD;
                sDoorCooldown = 90;
                PlaceLink(play, StumpFront(play, sWarp.stump), sSave.stumpYaw[sWarp.stump]);
                RestoreSceneMusic(play);
                break;
            case W_HOLLOW_TO_RW:
                if (!SpawnRootworks(play)) {
                    SpawnHollow(play);
                    PlaceLink(play, World(Hollow::kGateInside), 0);
                    Hud("Something blocks the way... (this area has no room left for the Rootworks)", 90);
                    break;
                }
                sLoc = LOC_ROOTWORKS;
                PlaceLink(play, World(Rootworks::kArrive), static_cast<s16>(-0x8000));
                PlayAreaMusic(NA_BGM_INSIDE_DEKU_TREE);
                break;
            case W_RW_TO_HOLLOW:
            case W_HEARTSEED:
                SpawnHollow(play);
                sLoc = LOC_HOLLOW;
                PlaceLink(play, World(Hollow::kGateInside), 0);
                Minish_SetTinyNow(player);
                PlayAreaMusic(NA_BGM_KOKIRI);
                break;
            default:
                break;
        }
    } else if (t >= kFade * 3) {
        play->envCtx.fillScreen = false;
        sWarp = WarpState();
        if (kind == W_HOLLOW_TO_STUMP) {
            sGrowIn = 5;
        } else if (kind == W_HOLLOW_TO_RW && sLoc == LOC_ROOTWORKS && !Has(Q_RW_SEEN)) {
            Set(Q_RW_SEEN);
            SysMessage(play, SCR_SYS_RW_INTRO);
        } else if (kind == W_HEARTSEED) {
            Hud("Bring the Heartseed to Elder Burdock!", 120);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Stumps in the big world
// ---------------------------------------------------------------------------------------------------------------------

static bool StumpSpotOk(PlayState* play, const Vec3f& c, f32 refY, f32* outY) {
    CollisionPoly* poly = nullptr;
    s32 bgId = BGCHECK_SCENE;
    f32 h0 = FloorAt(play, c.x, refY + 300.0f, c.z, &poly, &bgId);
    if (poly == nullptr || bgId != BGCHECK_SCENE || COLPOLY_GET_NORMAL(poly->normal.y) < 0.9f ||
        SurfaceType_GetSceneExitIndex(&play->colCtx, poly, bgId) != 0 ||
        SurfaceType_GetFloorType(&play->colCtx, poly, bgId) != 0 || fabsf(h0 - refY) > 120.0f) {
        return false;
    }
    f32 waterY = h0;
    WaterBox* waterBox;
    if (WaterBox_GetSurface1(play, &play->colCtx, c.x, c.z, &waterY, &waterBox) && waterY > h0 - 5.0f) {
        return false;
    }
    for (s32 i = 0; i < 8; i++) {
        Vec3f p = Add(c, RotY(V3(0, 0, 80), static_cast<s16>(i * 0x2000)));
        CollisionPoly* pp = nullptr;
        s32 pb = BGCHECK_SCENE;
        f32 h = FloorAt(play, p.x, h0 + 60.0f, p.z, &pp, &pb);
        if (pp == nullptr || pb != BGCHECK_SCENE || COLPOLY_GET_NORMAL(pp->normal.y) < 0.85f || fabsf(h - h0) > 8.0f) {
            return false;
        }
    }
    for (f32 dy : { 25.0f, 60.0f }) {
        f32 y = h0 + dy;
        Vec3f a = V3(c.x - 90.0f, y, c.z);
        Vec3f b = V3(c.x + 90.0f, y, c.z);
        Vec3f d = V3(c.x, y, c.z - 90.0f);
        Vec3f e = V3(c.x, y, c.z + 90.0f);
        Vec3f hit;
        CollisionPoly* hp;
        s32 hb;
        if (BgCheck_EntityLineTest1(&play->colCtx, &a, &b, &hit, &hp, true, false, false, false, &hb) ||
            BgCheck_EntityLineTest1(&play->colCtx, &d, &e, &hit, &hp, true, false, false, false, &hb)) {
            return false;
        }
    }
    *outY = h0;
    return true;
}

static void SpawnStumpActor(PlayState* play, s32 i) {
    Vec3f p = StumpPos(i);
    sStumpActor =
        Actor_Spawn(&play->actorCtx, play, sStumpId, p.x, p.y, p.z, 0, sSave.stumpYaw[i], 0, static_cast<s16>(i));
}

static void UpdateStumpSearch(PlayState* play, Player* player, s32 i) {
    static const f32 kRings[] = { 260.0f, 340.0f, 420.0f, 520.0f, 640.0f, 780.0f };
    if (!sSearch.active) {
        sSearch.active = true;
        sSearch.ring = 0;
        sSearch.center = player->actor.world.pos;
    }
    f32 r = kRings[sSearch.ring];
    for (s32 a = 0; a < 16; a++) {
        s16 yaw = static_cast<s16>(a * 0x1000 + sSearch.ring * 0x800);
        Vec3f c = Add(sSearch.center, RotY(V3(0, 0, r), yaw));
        f32 y;
        if (StumpSpotOk(play, c, sSearch.center.y, &y)) {
            sSave.stumpX[i] = static_cast<s16>(c.x);
            sSave.stumpY[i] = static_cast<s16>(y);
            sSave.stumpZ[i] = static_cast<s16>(c.z);
            // door faces where you were standing
            sSave.stumpYaw[i] = Math_Atan2S(sSearch.center.z - c.z, sSearch.center.x - c.x);
            sSave.stumpPlaced |= static_cast<u8>(1 << i);
            sSearch.done = true;
            SpawnStumpActor(play, i);
            PlaySysSfx(NA_SE_SY_CORRECT_CHIME);
            Hud("A hollow stump has sprouted nearby!", 90);
            return;
        }
    }
    sSearch.ring++;
    if (sSearch.ring >= static_cast<s32>(sizeof(kRings) / sizeof(kRings[0]))) {
        sSearch.done = true; // try again next visit
    }
}

static void UpdateWorld(PlayState* play, Player* player) {
    s32 i = StumpForScene(play->sceneNum);
    if (i < 0) {
        return;
    }
    if (!StumpPlaced(i)) {
        if (!sSearch.done && sSceneFrames > 40 && (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) &&
            !(player->stateFlags1 & (PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_IN_WATER | PLAYER_STATE1_ON_HORSE))) {
            UpdateStumpSearch(play, player, i);
        }
        return;
    }
    if (sStumpActor == nullptr && !sStumpSpawnTried && sSceneFrames >= 2) {
        sStumpSpawnTried = true;
        SpawnStumpActor(play, i);
    }

    // Travelled here through the roots: come out at the stump
    if (sPendingArrival == i && sSceneFrames >= 3) {
        sPendingArrival = -1;
        PlaceLink(play, StumpFront(play, i), sSave.stumpYaw[i]);
        sGrowIn = 8;
        sDoorCooldown = 90;
    }

    // Pip, lost at Lake Hylia
    if (i == kPipStump && Has(Q_PIP_START) && !Has(Q_PIP_FOUND) && sWorldPip == nullptr && sSceneFrames >= 4) {
        Vec3f p = Add(StumpPos(i), RotY(V3(24, 0, 68), sSave.stumpYaw[i]));
        CollisionPoly* poly = nullptr;
        f32 y = FloorAt(play, p.x, p.y + 60.0f, p.z, &poly);
        if (poly != nullptr) {
            p.y = y;
        }
        sWorldPip = SpawnPipkin(play, NPC_PIP, p, static_cast<s16>(sSave.stumpYaw[i] + 0x2000));
        sNpcActors[NPC_PIP] = nullptr; // not part of the Hollow
    }

    // The tiny door
    if (sDoorCooldown > 0) {
        sDoorCooldown--;
    }
    if (sWarp.kind == W_NONE && !sConv.active && sDoorCooldown == 0) {
        Vec3f door = StumpDoorPos(i);
        f32 d = DistXZ(player->actor.world.pos, door);
        f32 dy = fabsf(player->actor.world.pos.y - door.y);
        if (Minish_IsTiny() && d < 13.0f && dy < 20.0f) {
            StartWarp(W_STUMP_TO_HOLLOW, i);
        } else if (Minish_IsNormalSize() && d < 50.0f && dy < 40.0f && !sDoorHintShown) {
            sDoorHintShown = true;
            Hud("There's a tiny door at the base of the stump. Shrink to fit through!", 100);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// In the Hollow and the Rootworks
// ---------------------------------------------------------------------------------------------------------------------

static bool InZone(const Vec3f& local, const Rootworks::Zone& z) {
    return local.x > z.x0 && local.x < z.x1 && local.z > z.z0 && local.z < z.z1;
}

static void UpdateHollow(PlayState* play, Player* player) {
    (void)play;
    Vec3f l = Local(player->actor.world.pos);
    // Tansy's seedlings
    if (Has(Q_SEEDS_START) && !Has(Q_SEEDS_DONE)) {
        for (s32 i = 0; i < 5; i++) {
            if (SeedTaken(i)) {
                continue;
            }
            Vec3f d = Sub(l, Hollow::kSeeds[i]);
            d.y += 3.0f;
            if (Dot(d, d) < 11.0f * 11.0f) {
                sSave.seeds |= static_cast<u8>(1 << i);
                if (SeedCount() >= 5) {
                    PlaySysSfx(NA_SE_SY_CORRECT_CHIME);
                    Hud("All five seedlings! Bring them back to Tansy.", 90);
                } else {
                    PlaySysSfx(NA_SE_SY_GET_RUPY);
                    Hud("Seedling " + std::to_string(SeedCount()) + " of 5!", 50);
                }
            }
        }
    }
    // The Rootworks gate
    if (sGateMsgCooldown > 0) {
        sGateMsgCooldown--;
    }
    if (DistXZ(l, Hollow::kGate) < 28.0f && sWarp.kind == W_NONE) {
        if (Has(Q_GATE_OPEN)) {
            StartWarp(W_HOLLOW_TO_RW, -1);
        } else if (sGateMsgCooldown == 0) {
            sGateMsgCooldown = 120;
            Hud("The Rootworks gate is sealed tight. Moss guards it.", 80);
        }
    }
}

static void UpdateRootworks(PlayState* play, Player* player) {
    Vec3f l = Local(player->actor.world.pos);
    if (sSwitchMsgCooldown > 0) {
        sSwitchMsgCooldown--;
    }
    // Heavy switch: needs a full-size Link standing on it
    if (!Has(Q_SWITCH) && DistXZ(l, Rootworks::kSwitch) < Rootworks::kSwitchRadius && l.y < 12.0f &&
        (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND)) {
        if (Minish_IsNormalSize()) {
            Set(Q_SWITCH);
            Audio_PlayActorSound2(&player->actor, NA_SE_EV_FOOT_SWITCH);
            PlaySysSfx(NA_SE_SY_CORRECT_CHIME);
            Hud("Ka-chunk! Somewhere, a heavy gate rumbles open...", 90);
        } else if (sSwitchMsgCooldown == 0) {
            sSwitchMsgCooldown = 120;
            Hud("You're too light to press this switch down...", 80);
        }
    }
    // The den is cleared when every Gloomgrub is gone
    if (!Has(Q_GRUBS) && sGrubsSpawned && sSceneFrames > 5 && CountGrubs(play) == 0) {
        Set(Q_GRUBS);
        PlaySysSfx(NA_SE_SY_CORRECT_CHIME);
        Hud("The den is clear! A root door creaks open.", 90);
    }
    // Boss intro
    if (!Has(Q_BOSS) && !Has(Q_BOSS_SEEN) && PlayerInArena(play) && !sConv.active) {
        Set(Q_BOSS_SEEN);
        SysMessage(play, SCR_SYS_BOSS_INTRO);
    }
    // The Heartseed
    if (Has(Q_BOSS) && !Has(Q_HEARTSEED) && sWarp.kind == W_NONE && !sConv.active) {
        if (DistXZ(l, Rootworks::kHeartseed) < 30.0f && l.y < 90.0f) {
            Set(Q_HEARTSEED);
            ItemFanfare();
            SysMessage(play, SCR_SYS_HEARTSEED);
        }
    }
}

static bool InNoGrowZone(PlayState* play, Player* player) {
    (void)play;
    if (sLoc != LOC_ROOTWORKS) {
        return false;
    }
    Vec3f l = Local(player->actor.world.pos);
    for (const Rootworks::Zone& z : Rootworks::kNoGrow) {
        if (InZone(l, z)) {
            return true;
        }
    }
    return false;
}

// Called by the core mod when the player asks to grow while tiny
static bool OnGrowPressed(PlayState* play, Player* player) {
    if (sWarp.kind != W_NONE || sConv.active) {
        return true;
    }
    if (sLoc == LOC_HOLLOW) {
        if (DistXZ(Local(player->actor.world.pos), Hollow::kRing) < Hollow::kRingRadius) {
            if (Has(Q_PIP_DONE) && FoundStumps() >= 2) {
                SysMessage(play, SCR_SYS_TRAVEL, (sEnteredStump >= 0) ? sEnteredStump : NextFoundStump(0));
            } else {
                SysMessage(play, SCR_SYS_LEAVE_HOLLOW);
            }
        } else {
            Minish_ShowMessage("Too cramped to grow in here! Use the glowing exit ring.");
        }
        return true;
    }
    if (sLoc == LOC_ROOTWORKS) {
        if (DistXZ(Local(player->actor.world.pos), Rootworks::kArrive) < Rootworks::kPadRadius) {
            SysMessage(play, SCR_SYS_LEAVE_RW);
            return true;
        }
        if (InNoGrowZone(play, player)) {
            Minish_ShowMessage("Too cramped to grow here!");
            return true;
        }
    }
    return false;
}

// Dandelion Glide: hold A in the air while tiny to float
static void UpdateGlide(PlayState* play, Player* player) {
    if (!Has(Q_SEEDS_DONE) || !Minish_IsTiny()) {
        return;
    }
    Input* input = &play->state.input[0];
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) || (player->stateFlags1 & PLAYER_STATE1_IN_WATER) ||
        !CHECK_BTN_ALL(input->cur.button, BTN_A) || player->actor.velocity.y > -1.0f) {
        return;
    }
    player->actor.velocity.y = -1.0f;
    if (player->linearVelocity < 3.0f) {
        player->linearVelocity = 3.0f;
    }
    if ((play->gameplayFrames % 4) == 0) {
        static Color_RGBA8 sPrim = { 255, 255, 255, 255 };
        static Color_RGBA8 sEnv = { 200, 255, 200, 255 };
        Vec3f pos = player->actor.world.pos;
        pos.y += 9.0f;
        Vec3f vel = { Rand_CenteredFloat(0.3f), 0.2f, Rand_CenteredFloat(0.3f) };
        Vec3f accel = { 0.0f, 0.0f, 0.0f };
        EffectSsKiraKira_SpawnDispersed(play, &pos, &vel, &accel, &sPrim, &sEnv, 200, 10);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------------------------------

static void StoryUpdate() {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        return;
    }
    sSceneFrames++;
    if (sHudTimer > 0) {
        sHudTimer--;
    }
    if (!StoryUnlocked()) {
        return;
    }
    BuildModels();
    BuildWorldMeshes();
    RegisterWorldActors();
    RegisterCharacterActors();

    UpdateConversation(play);
    UpdateWarp(play, player);

    // First time: tell the player what's going on
    if (!Has(Q_INTRO) && sSceneFrames > 60 && !sConv.active && sWarp.kind == W_NONE &&
        play->msgCtx.msgMode == MSGMODE_NONE && (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) &&
        !Player_InCsMode(play)) {
        SysMessage(play, SCR_SYS_INTRO);
    }

    switch (sLoc) {
        case LOC_WORLD:
            UpdateWorld(play, player);
            break;
        case LOC_HOLLOW:
            UpdateHollow(play, player);
            break;
        case LOC_ROOTWORKS:
            UpdateRootworks(play, player);
            break;
    }

    if (sGrowIn > 0 && --sGrowIn == 0 && Minish_IsTiny()) {
        Minish_StartGrowing(play, player);
    }
    UpdateGlide(play, player);
}

static void StoryPlayDestroy() {
    ResetRuntime();
}

// ---------------------------------------------------------------------------------------------------------------------
// HUD: story messages and the quest tracker
// ---------------------------------------------------------------------------------------------------------------------

static std::vector<std::string> Goals() {
    std::vector<std::string> g;
    if (!Has(Q_MET_ELDER)) {
        g.push_back(sLoc == LOC_WORLD ? "SHRINK AND ENTER A HOLLOW STUMP" : "TALK TO ELDER BURDOCK");
        return g;
    }
    if (Has(Q_REWARD)) {
        return g;
    }
    if (Has(Q_HEARTSEED)) {
        g.push_back("BRING THE HEARTSEED TO THE ELDER");
        return g;
    }
    if (Has(Q_GATE_OPEN)) {
        if (sLoc != LOC_ROOTWORKS) {
            g.push_back("THE ROOTWORKS (NORTH GATE)");
        } else if (Has(Q_BOSS)) {
            g.push_back("TAKE THE HEARTSEED");
        } else if (!Has(Q_SWITCH)) {
            g.push_back("FIND A WAY PAST THE BIG GATE");
        } else if (!Has(Q_GRUBS)) {
            g.push_back("CROSS THE CHASM, CLEAR THE DEN");
        } else {
            g.push_back("DEFEAT THE GLOOMHORN");
        }
        return g;
    }
    if (!Has(Q_SEEDS_START)) {
        g.push_back("TALK TO TANSY");
    } else if (!Has(Q_SEEDS_DONE)) {
        g.push_back(SeedCount() >= 5 ? "BACK TO TANSY" : "SEEDLINGS " + std::to_string(SeedCount()) + "/5");
    }
    if (!Has(Q_PIP_START)) {
        g.push_back("TALK TO MOSS");
    } else if (!Has(Q_PIP_FOUND)) {
        g.push_back("FIND PIP (LAKE HYLIA)");
    } else if (!Has(Q_PIP_DONE)) {
        g.push_back("BACK TO MOSS");
    }
    if (!Has(Q_BUG_START)) {
        g.push_back("TALK TO BURL");
    } else if (!Has(Q_BUG_DONE)) {
        g.push_back("A BUG FOR BURL");
    }
    if (Has(Q_SEEDS_DONE) && Has(Q_PIP_DONE) && Has(Q_BUG_DONE)) {
        g.push_back("TALK TO THE ELDER");
    }
    return g;
}

static void PrintCentered(GfxPrint* printer, s32 row, const std::string& text) {
    std::string clipped = text.size() > 38 ? text.substr(0, 38) : text;
    GfxPrint_SetPos(printer, std::max<s32>(0, (40 - static_cast<s32>(clipped.size())) / 2), row);
    GfxPrint_Printf(printer, "%s", clipped.c_str());
}

static void StoryDrawHud() {
    PlayState* play = gPlayState;
    if (play == nullptr || !StoryUnlocked()) {
        return;
    }
    bool showTracker = Cfg_Tracker() && (sLoc != LOC_WORLD || Minish_IsTiny()) && play->msgCtx.msgMode == MSGMODE_NONE;
    std::vector<std::string> lines;
    if (showTracker) {
        std::string line;
        for (const std::string& goal : Goals()) {
            std::string add = line.empty() ? goal : "  " + goal;
            if (line.size() + add.size() > 38) {
                lines.push_back(line);
                line = goal;
            } else {
                line += add;
            }
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (lines.size() > 2) {
            lines.resize(2);
        }
    }
    bool showMsg = sHudTimer > 0 && !sHudMsg.empty();
    if (!showMsg && lines.empty()) {
        return;
    }

    OPEN_DISPS(play->state.gfxCtx);
    GfxPrint printer;
    Gfx* polyOpa = POLY_OPA_DISP;
    Gfx* gfx = Graph_GfxPlusOne(polyOpa);
    gSPDisplayList(OVERLAY_DISP++, gfx);
    GfxPrint_Init(&printer);
    GfxPrint_Open(&printer, gfx);
    if (showMsg) {
        // long messages wrap onto two lines
        GfxPrint_SetColor(&printer, 255, 255, 200, 255);
        if (sHudMsg.size() <= 38) {
            PrintCentered(&printer, 20, sHudMsg);
        } else {
            size_t cut = sHudMsg.rfind(' ', 38);
            if (cut == std::string::npos) {
                cut = 38;
            }
            PrintCentered(&printer, 19, sHudMsg.substr(0, cut));
            PrintCentered(&printer, 20, sHudMsg.substr(cut + 1));
        }
    }
    GfxPrint_SetColor(&printer, 150, 255, 160, 255);
    for (size_t i = 0; i < lines.size(); i++) {
        PrintCentered(&printer, 25 + static_cast<s32>(i), lines[i]);
    }
    gfx = GfxPrint_Close(&printer);
    GfxPrint_Destroy(&printer);
    gSPEndDisplayList(gfx++);
    Graph_BranchDlist(polyOpa, gfx);
    POLY_OPA_DISP = gfx;
    CLOSE_DISPS(play->state.gfxCtx);
}

// ---------------------------------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------------------------------

static void RegisterStory() {
    bool on = StoryOn();
    Minish_SetGrowHandler(on ? OnGrowPressed : nullptr);
    if (!on && gPlayState != nullptr && sLoc != LOC_WORLD) {
        DespawnArea(gPlayState);
        sLoc = LOC_WORLD;
    }

    COND_HOOK(OnPlayerUpdate, on, StoryUpdate);
    COND_HOOK(OnPlayDrawEnd, on, StoryDrawHud);
    COND_HOOK(OnPlayDestroy, on, StoryPlayDestroy);
    COND_HOOK(OnOpenText, on, OnStoryOpenText);
    COND_HOOK(OnLoadGame, on, [](int32_t) {
        ResetRuntime();
        sPendingArrival = -1;
        sEnteredStump = -1;
    });

    // Hold Link still while the screen fades and during story messages
    COND_VB_SHOULD(VB_EXECUTE_PLAYER_ACTION_FUNC, on, {
        Player* player = va_arg(args, Player*);
        if (sWarp.kind != W_NONE || (sConv.active && sConv.npc < 0)) {
            *should = false;
            player->linearVelocity = 0.0f;
        }
    });

    // Remember beating Ganon (the game doesn't save after the final battle)
    COND_ID_HOOK(OnBossDefeat, ACTOR_BOSS_GANON2, true, [](void*) {
        CVarSetInteger(UnlockKey().c_str(), 1);
        SaveCVars();
    });
}

static void RegisterStorySave() {
    static bool sDone = false;
    if (sDone) {
        return;
    }
    sDone = true;
    SaveManager::Instance->AddInitFunction(InitStory);
    SaveManager::Instance->AddLoadFunction("minishStory", 1, LoadStory);
    SaveManager::Instance->AddSaveFunction("minishStory", 1, &SaveCallback<SaveManager::SaveFunc>::Fn, true,
                                           SECTION_PARENT_NONE);
}

static RegisterShipInitFunc sStoryInit(RegisterStory, { CVAR_STORY("Enabled"), CVAR_ENHANCEMENT("Minish.Enabled") });
static RegisterShipInitFunc sStorySaveInit(RegisterStorySave, {});

// ---------------------------------------------------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------------------------------------------------

static std::string Journal() {
    if (gPlayState == nullptr) {
        return "Story Journal: load a save file to see your progress.";
    }
    if (!StoryUnlocked()) {
        return "Story Journal: unlocks after you beat Ganon.";
    }
    auto mark = [](bool done) { return done ? "[x] " : "[ ] "; };
    std::string s = "Story Journal (Chapter 1: Pipkin Hollow)";
    s += std::string("\n") + mark(Has(Q_MET_ELDER)) + "Meet Elder Burdock in Pipkin Hollow";
    s += std::string("\n") + mark(Has(Q_SEEDS_DONE)) + "Tansy's seedlings (" + std::to_string(SeedCount()) +
         "/5) - Dandelion Glide";
    s += std::string("\n") + mark(Has(Q_PIP_DONE)) + "Find Pip at Lake Hylia - Rootway Charm";
    s += std::string("\n") + mark(Has(Q_BUG_DONE)) + "A bug for Burl - 100 rupees";
    s += std::string("\n") + mark(Has(Q_GATE_OPEN)) + "The Rootworks gate opens";
    s += std::string("\n") + mark(Has(Q_SWITCH)) + "Rootworks: the heavy switch";
    s += std::string("\n") + mark(Has(Q_GRUBS)) + "Rootworks: the Gloomgrub den";
    s += std::string("\n") + mark(Has(Q_BOSS)) + "Rootworks: the Gloomhorn";
    s += std::string("\n") + mark(Has(Q_REWARD)) + "Bring the Heartseed home - Heart Container";
    s += "\nStumps found: " + std::to_string(FoundStumps()) + "/5";
    return s;
}

static void RegisterStoryWidgets() {
    WidgetPath path = { "Enhancements", "Extra Modes", SECTION_COLUMN_3 };
    auto disabledIfOff = [](WidgetInfo& info) {
        info.options->disabled = !Minish_IsEnabled();
        info.options->disabledTooltip = "Turn on Enable Minish first.";
    };

    SohGui::mSohMenu->AddWidget(path, "Pipkin Hollow (Minish Story)", WIDGET_SEPARATOR_TEXT);

    SohGui::mSohMenu->AddWidget(path, "Pipkin Hollow Story", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_STORY("Enabled"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "A side story for after the game: after you beat Ganon, hollow stumps sprout around Hyrule.\n"
            "Shrink, walk through the tiny door at a stump's base, and find the Pipkins' hidden village.\n"
            "Needs Enable Minish."));

    SohGui::mSohMenu->AddWidget(path, "Story Tracker", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_STORY("Tracker"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "Shows your next story goals at the bottom of the screen while you're tiny or in the Hollow."));

    SohGui::mSohMenu->AddWidget(path, "Unlock the Story Now", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !Minish_IsEnabled() || !Cfg_Story();
            info.options->disabled = gPlayState == nullptr || StoryUnlocked();
            info.options->disabledTooltip = gPlayState == nullptr ? "Load a save file first." : "Already unlocked.";
        })
        .Options(UIWidgets::ButtonOptions().Tooltip(
            "The story unlocks by itself when you beat Ganon. If you beat him before installing this mod,\n"
            "load that save file and press this. (Saved with your file the next time you save.)"))
        .Callback([](WidgetInfo&) { Set(Q_UNLOCKED); });

    SohGui::mSohMenu->AddWidget(path, "Story Journal", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        info.isHidden = !Minish_IsEnabled() || !Cfg_Story();
        info.name = Journal();
    });

    SohGui::mSohMenu->AddWidget(path, "Move This Area's Stump Here", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !Minish_IsEnabled() || !Cfg_Story();
            info.options->disabled = gPlayState == nullptr || !StoryUnlocked() ||
                                     StumpForScene(gPlayState->sceneNum) < 0 || sLoc != LOC_WORLD;
            info.options->disabledTooltip =
                "Stand in Kokiri Forest, the Lost Woods, Hyrule Field, Kakariko Village or Lake Hylia.";
        })
        .Options(UIWidgets::ButtonOptions().Tooltip("Puts this area's hollow stump right in front of Link."))
        .Callback([](WidgetInfo&) {
            if (gPlayState == nullptr) {
                return;
            }
            s32 i = StumpForScene(gPlayState->sceneNum);
            if (i < 0) {
                return;
            }
            Player* player = GET_PLAYER(gPlayState);
            Vec3f p = Add(player->actor.world.pos, RotY(V3(0, 0, 110), player->actor.shape.rot.y));
            CollisionPoly* poly = nullptr;
            f32 y = FloorAt(gPlayState, p.x, p.y + 100.0f, p.z, &poly);
            sSave.stumpX[i] = static_cast<s16>(p.x);
            sSave.stumpY[i] = static_cast<s16>(poly != nullptr ? y : player->actor.world.pos.y);
            sSave.stumpZ[i] = static_cast<s16>(p.z);
            sSave.stumpYaw[i] = static_cast<s16>(player->actor.shape.rot.y + 0x8000);
            sSave.stumpPlaced |= static_cast<u8>(1 << i);
            KillTracked(sStumpActor);
            sStumpSpawnTried = false;
        });

    SohGui::mSohMenu->AddWidget(path, "Reset Story Progress", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !Minish_IsEnabled() || !Cfg_Story();
            info.options->disabled = gPlayState == nullptr || sLoc != LOC_WORLD;
            info.options->disabledTooltip = "Load a save file and leave the Minish areas first.";
        })
        .Options(UIWidgets::ButtonOptions().Tooltip(
            "Starts the Pipkin Hollow story over for this save file (the stumps stay where they are)."))
        .Callback([](WidgetInfo&) {
            sSave.quests &= (Q_UNLOCKED | Q_INTRO);
            sSave.seeds = 0;
            sSave.stumpFound = 0;
        });
}

static RegisterMenuInitFunc sStoryMenuInit(RegisterStoryWidgets);

} // namespace PipkinStory
