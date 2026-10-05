/**
 * Skateboard mod for Ship of Harkinian
 * ------------------------------------
 * Lets Link ride a board (one of his shields) with momentum physics, ollies,
 * flip tricks, grabs, spins, ledge/rail grinding and a Tony Hawk style combo system.
 *
 * How it plugs into the game:
 *  - Riding is a real player action (`Skate_Action`), installed with Player_SetupAction just like the
 *    game's own actions. Anything that normally interrupts Link (damage, water, cutscenes, loading zones)
 *    replaces the action, and the mod notices and cleans up.
 *  - Movement uses the player's own physics/collision: we only steer `yaw`, `linearVelocity` and
 *    `velocity.y`, so walls, floors, slopes, voids and exits all keep working.
 *  - Grinding works on any ledge/edge in the level geometry: a spot counts as a "rail" when there is floor
 *    under the board but a drop on at least one side.
 *
 * Default controls while on the board:
 *  Toggle board ........ D-Pad Down (configurable)
 *  Stick up / down ..... roll forward / brake        Stick left / right ... carve (ground), spin (air), balance (grind)
 *  B (ground) .......... push                         A ...................... hold to crouch, release to ollie
 *  B + direction (air) . flip tricks                  R + direction (air) .... grabs (hold for more points)
 *  R near a ledge ...... grind (hold R while landing on an edge, or press R while rolling along one)
 *  A against a wall .... wall jump (in the air)       Bombs button (or Z) .... bomb hop
 *  R next to Epona ..... hold on and get towed, steer with the stick, let go to slingshot
 *
 * Extras: seamless loading zones while riding, per-area quests (spell Z-E-L-D-A, clear gaps, big combo), and the
 * Hyrule Skate Park in Hyrule Field (half pipe, Triforce funbox, rails, treasures to collect).
 */

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include "soh/SohGui/MenuTypes.h"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/SohGui.hpp"
#include "soh/Enhancements/nametag.h"
#include "soh/util.h"
#include "soh/ActorDB.h"

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
}

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;

// Player functions from z_player.c (C linkage, not static)
s32 Player_SetupAction(PlayState* play, Player* player, PlayerActionFunc actionFunc, s32 flags);
s32 Player_SetupWaitForPutAway(PlayState* play, Player* player, AfterPutAwayFunc func);
void func_80853080(Player* player, PlayState* play); // Set up the idle action
void Player_PlayVoiceSfx(Player* player, u16 sfxId);
void Player_Action_WaitForPutAway(Player* player, PlayState* play);
void Player_Action_Idle(Player* player, PlayState* play);
void Player_Action_80840450(Player* player, PlayState* play); // targeting idle
void Player_Action_808407CC(Player* player, PlayState* play); // targeting idle
void Player_Action_80840DE4(Player* player, PlayState* play); // side walk
void Player_Action_808414F8(Player* player, PlayState* play); // back walk
void Player_Action_8084170C(Player* player, PlayState* play);
void Player_Action_808417FC(Player* player, PlayState* play);
void Player_Action_8084193C(Player* player, PlayState* play); // targeted walk
void Player_Action_80842180(Player* player, PlayState* play); // run
void Player_Action_80845CA4(Player* player, PlayState* play); // walking into / out of an area
s32 func_80845C68(PlayState* play, s32 arg1);                 // sets the void-out respawn point after walking in

// Epona (z_en_horse.c, not static)
void EnHorse_InitFleePlayer(EnHorse* horse);
void EnHorse_StartIdleRidable(EnHorse* horse);
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------------

#define CVAR_SKATE(name) CVAR_ENHANCEMENT("Skateboard." name)
#define CVAR_SKATE_ENABLED CVAR_SKATE("Enabled")
#define SKATE_ENABLED CVarGetInteger(CVAR_SKATE_ENABLED, 0)

enum SkateBoardModel { BOARD_DEKU_SHIELD, BOARD_HYLIAN_SHIELD, BOARD_MIRROR_SHIELD };
enum SkateStance { STANCE_REGULAR, STANCE_GOOFY };

static s32 Cfg_ToggleMask() {
    return CVarGetInteger(CVAR_SKATE("ToggleBtn"), BTN_DDOWN);
}
static s32 Cfg_BoardModel() {
    return CVarGetInteger(CVAR_SKATE("BoardModel"), BOARD_DEKU_SHIELD);
}
static f32 Cfg_BoardSize() {
    return CVarGetFloat(CVAR_SKATE("BoardSize"), 1.0f);
}
static f32 Cfg_BoardHeight() {
    return CVarGetFloat(CVAR_SKATE("BoardHeight"), 0.0f);
}
static s32 Cfg_Stance() {
    return CVarGetInteger(CVAR_SKATE("Stance"), STANCE_REGULAR);
}
static bool Cfg_BailsHurt() {
    return CVarGetInteger(CVAR_SKATE("BailsHurt"), 0) != 0;
}
static bool Cfg_ShowHud() {
    return CVarGetInteger(CVAR_SKATE("ShowHud"), 1) != 0;
}
static bool Cfg_RupeeReward() {
    return CVarGetInteger(CVAR_SKATE("RupeeReward"), 0) != 0;
}
static bool Cfg_KeepBoard() {
    return CVarGetInteger(CVAR_SKATE("KeepBetweenAreas"), 1) != 0;
}
static bool Cfg_RollSound() {
    return CVarGetInteger(CVAR_SKATE("RollingSound"), 1) != 0;
}
static bool Cfg_WaterSkate() {
    return CVarGetInteger(CVAR_SKATE("WaterSkating"), 1) != 0;
}
// Safety valve in case a shield model is oriented differently than expected: cycles how it's laid flat
static s32 Cfg_BoardOrientation() {
    return CVarGetInteger(CVAR_SKATE("BoardOrientation"), 0);
}
static bool Cfg_Seamless() {
    return CVarGetInteger(CVAR_SKATE("SeamlessAreas"), 1) != 0;
}
static bool Cfg_Quests() {
    return CVarGetInteger(CVAR_SKATE("Quests"), 1) != 0;
}
static bool Cfg_QuestRupees() {
    return CVarGetInteger(CVAR_SKATE("QuestRupees"), 1) != 0;
}
static bool Cfg_WallJumps() {
    return CVarGetInteger(CVAR_SKATE("WallJumps"), 1) != 0;
}
static bool Cfg_FreeBombs() {
    return CVarGetInteger(CVAR_SKATE("FreeBombs"), 0) != 0;
}
static f32 Cfg_SpeedMult() {
    return CVarGetFloat(CVAR_SKATE("SpeedMult"), 1.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tuning (units are game units per 20fps logic frame, angles are binary angles: 0x10000 = 360 degrees)
// ---------------------------------------------------------------------------------------------------------------------

static constexpr f32 kPi = 3.14159265358979f;

namespace Tune {
constexpr f32 kGravity = -1.0f;
constexpr f32 kPushImpulse = 2.6f;
constexpr f32 kPushMax = 9.0f;      // pushing can't go faster than this
constexpr f32 kCruiseAccel = 0.12f; // holding stick up
constexpr f32 kCruiseMax = 6.5f;
constexpr f32 kBrake = 0.45f;
constexpr f32 kFriction = 0.035f;
constexpr f32 kSlopeAccel = 0.9f;
constexpr f32 kMaxSpeed = 15.0f;
constexpr f32 kOllieBase = 6.5f;
constexpr f32 kOllieCharge = 3.5f;
constexpr s16 kOllieChargeFrames = 12;
constexpr s16 kTurnRate = 0x0380;
constexpr s16 kSpinRate = 0x0C00;
constexpr s16 kLandTolerance = 0x2600; // how far off a clean 180 you may land
constexpr f32 kRampLaunch = 0.9f;
constexpr f32 kGrindMinSpeed = 2.5f;
constexpr f32 kGrindFriction = 0.025f;
constexpr f32 kWallBailSpeed = 11.0f;
constexpr s16 kPushFrames = 12;      // length of one push stroke (also the minimum time between pushes)
constexpr s16 kPushContactStart = 3; // the kicking foot touches the ground on this frame of the stroke...
constexpr s16 kPushContactEnd = 8;   // ...and leaves it here; the speed is added in between
constexpr f32 kWallJumpVelY = 9.0f;
constexpr s16 kWallJumpMax = 3;    // wall jumps per jump
constexpr s16 kWallJumpBuffer = 6; // A can be pressed this many frames before touching the wall
constexpr s16 kBombFuse = 10;
constexpr f32 kBombRadius = 110.0f;
constexpr f32 kBombLaunchY = 15.0f;
constexpr f32 kBombBoost = 4.0f;
constexpr f32 kTowGrabRange = 120.0f;
constexpr f32 kTowDistance = 85.0f; // how far behind Epona's middle Link hangs on
constexpr f32 kTowLoseGrip = 260.0f;
constexpr f32 kTowSlingshot = 4.0f;
constexpr s32 kQuestComboGoal = 5000;
constexpr s32 kQuestGapGoal = 3;
} // namespace Tune

// ---------------------------------------------------------------------------------------------------------------------
// Trick tables
// ---------------------------------------------------------------------------------------------------------------------

enum StickDir { DIR_NONE, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_UPLEFT, DIR_UPRIGHT, DIR_DOWNLEFT, DIR_DOWNRIGHT };

struct FlipTrick {
    const char* name;
    s32 roll;  // around the board's long axis (kickflip)
    s32 pitch; // end over end (impossible)
    s32 yaw;   // shove-it
    s16 frames;
    s32 points;
};

static const FlipTrick sFlipTricks[] = {
    /* DIR_NONE      */ { "KICKFLIP", 0x10000, 0, 0, 11, 100 },
    /* DIR_UP        */ { "IMPOSSIBLE", 0, 0x10000, 0, 13, 150 },
    /* DIR_DOWN      */ { "POP SHOVE-IT", 0, 0, 0x8000, 9, 75 },
    /* DIR_LEFT      */ { "KICKFLIP", 0x10000, 0, 0, 11, 100 },
    /* DIR_RIGHT     */ { "HEELFLIP", -0x10000, 0, 0, 11, 100 },
    /* DIR_UPLEFT    */ { "VARIAL KICKFLIP", 0x10000, 0, 0x8000, 13, 150 },
    /* DIR_UPRIGHT   */ { "VARIAL HEELFLIP", -0x10000, 0, -0x8000, 13, 150 },
    /* DIR_DOWNLEFT  */ { "360 FLIP", 0x10000, 0, 0x10000, 15, 250 },
    /* DIR_DOWNRIGHT */ { "HARDFLIP", 0x10000, 0, -0x8000, 15, 200 },
};

struct GrabTrick {
    const char* name;
    s16 boardPitch;
    s16 boardRoll;
    s32 points;
};

static const GrabTrick sGrabTricks[] = {
    /* DIR_NONE      */ { "GRAB", 0, 0, 0 }, // unused, R with a neutral stick means "look for a grind"
    /* DIR_UP        */ { "NOSEGRAB", -0x1800, 0, 150 },
    /* DIR_DOWN      */ { "TAILGRAB", 0x1800, 0, 150 },
    /* DIR_LEFT      */ { "MELON", 0, 0x1400, 150 },
    /* DIR_RIGHT     */ { "INDY", 0, -0x1400, 150 },
    /* DIR_UPLEFT    */ { "JAPAN", -0x1000, 0x1000, 200 },
    /* DIR_UPRIGHT   */ { "MADONNA", -0x1800, -0x1800, 250 },
    /* DIR_DOWNLEFT  */ { "BENIHANA", 0x2000, 0x1000, 250 },
    /* DIR_DOWNRIGHT */ { "STALEFISH", 0x1000, -0x1400, 200 },
};

struct GrindTrick {
    const char* name;
    s16 boardYaw;
    s16 boardPitch;
    s32 points;
};

static const GrindTrick sGrindTricks[] = {
    /* DIR_NONE      */ { "50-50", 0, 0, 100 },
    /* DIR_UP        */ { "NOSEGRIND", 0, -0x0C00, 120 },
    /* DIR_DOWN      */ { "5-0 GRIND", 0, 0x0C00, 120 },
    /* DIR_LEFT      */ { "BOARDSLIDE", 0x4000, 0, 110 },
    /* DIR_RIGHT     */ { "LIPSLIDE", -0x4000, 0, 130 },
    /* DIR_UPLEFT    */ { "CROOKED GRIND", 0x1000, -0x0A00, 150 },
    /* DIR_UPRIGHT   */ { "OVERCROOK", -0x1000, -0x0A00, 150 },
    /* DIR_DOWNLEFT  */ { "SMITH GRIND", 0x1000, 0x0A00, 150 },
    /* DIR_DOWNRIGHT */ { "FEEBLE GRIND", -0x1000, 0x0A00, 150 },
};

// ---------------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------------

enum SkatePhase { PHASE_GROUND, PHASE_AIR, PHASE_GRIND };

enum SkateAnim {
    ANIM_NONE,
    ANIM_RIDE,
    ANIM_CROUCH,
    ANIM_AIR_UP,
    ANIM_AIR_DOWN,
    ANIM_TUCK,
    ANIM_GRIND,
    ANIM_LAND,
    ANIM_PUSH,
    ANIM_TOW
};

enum HudMsgColor { MSG_GOOD, MSG_BAD, MSG_INFO };

struct Combo {
    std::vector<std::string> names;
    s32 base = 0;
    std::unordered_map<std::string, s32> usage;

    void Clear() {
        names.clear();
        base = 0;
        usage.clear();
    }

    // Repeating the same trick in one combo is worth less each time (like the THPS games).
    s32 Add(const std::string& name, s32 points) {
        s32 used = usage[name]++;
        s32 value = points >> std::min(used, 3);
        names.push_back(name);
        base += value;
        return value;
    }

    s32 Mult() const {
        return std::max<s32>(1, (s32)names.size());
    }

    bool Empty() const {
        return names.empty();
    }
};

struct SkateState {
    bool active = false;       // Link is currently in Skate_Action
    bool mountPending = false; // waiting for items to be put away
    bool bailPending = false;
    bool remountPending = false;
    s16 remountTimer = 0;
    s16 toggleCooldown = 0;

    SkatePhase phase = PHASE_GROUND;
    SkateAnim anim = ANIM_NONE;
    s16 moveYaw = 0;
    f32 speed = 0.0f;
    bool fakie = false;
    s16 pushCooldown = 0;
    s16 pushTimer = 0;        // counts down through a push stroke
    f32 pushBlend = 0.0f;     // 0 = standing sideways on the board, 1 = turned forward to push
    f32 pushStrideEnd = 0.0f; // last animation frame of the stride we play for the kick
    s16 chargeFrames = 0;
    bool charging = false;
    f32 lastRise = 0.0f;    // slope along the travel direction on the last grounded frame
    f32 impactSpeed = 0.0f; // speed going into this frame, before walls slowed us down
    bool onWater = false;   // riding on top of a water surface
    bool onPark = false;    // rolling on the skate park
    bool ollied = false;
    s16 landTimer = 0;

    // air
    s32 spin = 0;
    s32 airTrickStart = 0; // index in combo.names where this jump's tricks begin
    s32 flipIdx = -1;
    s16 flipTimer = 0;
    s32 grabIdx = -1;
    s16 grabFrames = 0;
    s16 noGrindTimer = 0;
    s16 aBuffer = 0;   // recent A press in the air, for wall jumps
    s16 wallJumps = 0; // wall jumps during this jump

    // gap detection
    bool airTracking = false;
    bool fromGrind = false;
    bool airOverWater = false;
    Vec3f takeoffPos = { 0.0f, 0.0f, 0.0f };
    f32 airLowestFloor = 0.0f;

    // bomb hop
    bool bombActive = false;
    Vec3f bombPos = { 0.0f, 0.0f, 0.0f };
    f32 bombVelY = 0.0f;
    s16 bombTimer = 0;

    // Epona tow
    EnHorse* towHorse = nullptr;
    s32 towFrames = 0;

    // seamless loading zones
    bool carryPending = false; // rode into a loading zone, waiting for the next area
    bool carryActive = false;  // the next area has loaded: hop back on as soon as Link appears
    s16 carryTimer = 0;
    f32 carrySpeed = 0.0f;
    s32 sceneFrames = 0;

    // grind
    s32 grindIdx = 0;
    s16 grindFrames = 0;
    s16 grindYaw = 0;
    f32 balance = 0.0f;

    // visuals
    s32 boardYawBase = 0; // persistent after shove-its
    s16 boardYaw = 0, boardPitch = 0, boardRoll = 0;
    f32 boardDrop = 0.0f;
    s16 tiltX = 0, tiltZ = 0;
    Vec3s logicRot = { 0, 0, 0 };
    Vec3s visualRot = { 0, 0, 0 };

    // scoring + hud
    Combo combo;
    s32 session = 0;
    std::string msg;
    s16 msgTimer = 0;
    HudMsgColor msgColor = MSG_INFO;
};

struct HudMsg {
    std::string text;
    HudMsgColor color;
    s16 frames;
};
static std::vector<HudMsg> sMsgQueue; // messages waiting their turn behind the one on screen

static SkateState sSkate;
static Input* sActionInput = nullptr;

void Skate_Action(Player* player, PlayState* play);

// ---------------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------------

static f32 AgeScale() {
    return LINK_IS_ADULT ? 1.3f : 1.0f;
}

// Mirrored World flips the stick horizontally, the same way the game does for Link's own movement
static s32 RawStickX(Input* input) {
    return CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -input->rel.stick_x : input->rel.stick_x;
}

static f32 StickX(Input* input) {
    return std::clamp(static_cast<f32>(RawStickX(input)) / 60.0f, -1.0f, 1.0f);
}

static f32 StickY(Input* input) {
    return std::clamp(input->rel.stick_y / 60.0f, -1.0f, 1.0f);
}

static StickDir GetStickDir(Input* input) {
    s32 x = RawStickX(input);
    s32 y = input->rel.stick_y;
    if (x * x + y * y < 28 * 28) {
        return DIR_NONE;
    }
    f32 deg = atan2f((f32)y, (f32)x) * (180.0f / kPi); // 0 = right, 90 = up
    if (deg < 0) {
        deg += 360.0f;
    }
    s32 sector = (s32)((deg + 22.5f) / 45.0f) % 8;
    static const StickDir sSectors[] = { DIR_RIGHT, DIR_UPRIGHT,  DIR_UP,   DIR_UPLEFT,
                                         DIR_LEFT,  DIR_DOWNLEFT, DIR_DOWN, DIR_DOWNRIGHT };
    return sSectors[sector];
}

static void ShowMsg(const std::string& text, HudMsgColor color, s16 frames = 40) {
    if (sSkate.msgTimer <= 0) {
        sSkate.msg = text;
        sSkate.msgColor = color;
        sSkate.msgTimer = frames;
    } else if (sMsgQueue.size() < 4) {
        // Show it right after the current one, a bit shorter so the queue doesn't lag behind the action
        sMsgQueue.push_back({ text, color, (s16)std::max(25, frames * 3 / 4) });
    }
}

static void UpdateMessages() {
    if (sSkate.msgTimer > 0) {
        sSkate.msgTimer--;
    }
    if (sSkate.msgTimer <= 0 && !sMsgQueue.empty()) {
        sSkate.msg = sMsgQueue.front().text;
        sSkate.msgColor = sMsgQueue.front().color;
        sSkate.msgTimer = sMsgQueue.front().frames;
        sMsgQueue.erase(sMsgQueue.begin());
    }
}

static void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

static bool IsActorAlive(PlayState* play, Actor* target, s32 category) {
    for (Actor* actor = play->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
        if (actor == target) {
            return actor->update != NULL;
        }
    }
    return false;
}

static void DrawItemAt(PlayState* play, void* key, Vec3f pos, s16 gid, f32 scale, s16 yaw) {
    OPEN_DISPS(play->state.gfxCtx);
    FrameInterpolation_RecordOpenChild(key, 0);

    Lights* lights = LightContext_NewLights(&play->lightCtx, play->state.gfxCtx);
    Lights_BindAll(lights, play->lightCtx.listHead, &pos);
    Lights_Draw(lights, play->state.gfxCtx);

    Matrix_Translate(pos.x, pos.y, pos.z, MTXMODE_NEW);
    Matrix_RotateY(static_cast<f32>(BINANG_TO_RAD(yaw)), MTXMODE_APPLY);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    GetItem_Draw(play, gid);

    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(play->state.gfxCtx);
}

static void PlaySfx(Player* player, u16 sfx) {
    Player_PlaySfx(&player->actor, sfx);
}

static void PlayLoopSfx(Player* player, u16 sfx, f32 freq, f32 vol) {
    static f32 sFreq;
    static f32 sVol;
    sFreq = freq;
    sVol = vol;
    Audio_PlaySfxGeneral(sfx, &player->actor.projectedPos, 4, &sFreq, &sVol, &gSfxDefaultReverb);
}

static f32 FloorAt(PlayState* play, f32 x, f32 y, f32 z) {
    Vec3f pos = { x, y, z };
    CollisionPoly* poly;
    s32 bgId;
    return BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &pos);
}

// Water surface under Link, if there is water above the floor here
static bool GetWaterSurface(PlayState* play, Player* player, f32* outY) {
    f32 y = player->actor.world.pos.y;
    WaterBox* waterBox;
    if (WaterBox_GetSurface1(play, &play->colCtx, player->actor.world.pos.x, player->actor.world.pos.z, &y,
                             &waterBox) &&
        y > player->actor.floorHeight + 1.0f) {
        *outY = y;
        return true;
    }
    return false;
}

static void SpawnSplash(PlayState* play, Player* player, s16 size) {
    Vec3f pos = player->actor.world.pos;
    EffectSsGSplash_Spawn(play, &pos, NULL, NULL, 0, size);
    EffectSsGRipple_Spawn(play, &pos, 150, 500, 0);
}

// Something is grindable when there is floor right under the board, but it drops away on at least one side
// (perpendicular to the direction of travel). That covers walls, fences, ledges, curbs, platform edges...
static bool IsGrindEdge(PlayState* play, f32 x, f32 z, f32 refY, s16 yaw, f32* outY) {
    f32 s = AgeScale();
    f32 center = FloorAt(play, x, refY + 18.0f * s, z);

    if (center < refY - 26.0f * s || center > refY + 18.0f * s) {
        return false;
    }

    f32 rightX = Math_CosS(yaw);
    f32 rightZ = -Math_SinS(yaw);
    f32 side = 16.0f * s;
    f32 left = FloorAt(play, x - rightX * side, center + 4.0f, z - rightZ * side);
    f32 right = FloorAt(play, x + rightX * side, center + 4.0f, z + rightZ * side);
    f32 drop = 30.0f * s;

    if ((center - left) > drop || (center - right) > drop) {
        if (outY != nullptr) {
            *outY = center;
        }
        return true;
    }
    return false;
}

// Looks for a ledge near Link that runs roughly along his direction of travel.
// On success returns the snapped position and the ledge direction.
static bool FindGrindLine(PlayState* play, Player* player, s16 travelYaw, Vec3f* outPos, s16* outYaw) {
    f32 s = AgeScale();
    Vec3f pos = player->actor.world.pos;
    f32 rightX = Math_CosS(travelYaw);
    f32 rightZ = -Math_SinS(travelYaw);
    static const f32 sOffsets[] = { 0.0f, 6.0f, -6.0f, 12.0f, -12.0f, 18.0f, -18.0f, 24.0f, -24.0f };

    for (f32 off : sOffsets) {
        f32 x = pos.x + rightX * off * s;
        f32 z = pos.z + rightZ * off * s;
        f32 y;
        if (!IsGrindEdge(play, x, z, pos.y, travelYaw, &y)) {
            continue;
        }
        // Find the yaw that keeps us on the edge a bit further ahead, preferring the smallest correction.
        for (s32 k = 0; k <= 6; k++) {
            for (s32 sign = 1; sign >= -1; sign -= 2) {
                if (k == 0 && sign == -1) {
                    continue;
                }
                s16 yaw = travelYaw + (s16)(sign * k * 0x400);
                f32 y1, y2;
                if (IsGrindEdge(play, x + Math_SinS(yaw) * 20.0f * s, z + Math_CosS(yaw) * 20.0f * s, y, yaw, &y1) &&
                    IsGrindEdge(play, x + Math_SinS(yaw) * 40.0f * s, z + Math_CosS(yaw) * 40.0f * s, y, yaw, &y2)) {
                    outPos->x = x;
                    outPos->y = y;
                    outPos->z = z;
                    *outYaw = yaw;
                    return true;
                }
            }
        }
    }
    return false;
}

static void SetAnim(PlayState* play, Player* player, SkateAnim anim) {
    if (sSkate.anim == anim) {
        return;
    }
    sSkate.anim = anim;

    LinkAnimationHeader* header = nullptr;
    switch (anim) {
        case ANIM_RIDE:
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_wait_free;
            LinkAnimation_Change(play, &player->skelAnime, header, 1.0f, 0.0f, 0.0f, ANIMMODE_LOOP, -6.0f);
            break;
        case ANIM_CROUCH:
        case ANIM_TUCK: {
            // Hold the squashed first frames of the landing animation as a crouch
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_short_landing_free;
            f32 frame = std::min<f32>(2.0f, Animation_GetLastFrame(header));
            LinkAnimation_Change(play, &player->skelAnime, header, 0.0f, frame, frame, ANIMMODE_ONCE, -4.0f);
            break;
        }
        case ANIM_AIR_UP:
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_jump;
            LinkAnimation_Change(play, &player->skelAnime, header, 1.0f, 0.0f, Animation_GetLastFrame(header),
                                 ANIMMODE_ONCE, -3.0f);
            break;
        case ANIM_AIR_DOWN:
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_landing_wait;
            LinkAnimation_Change(play, &player->skelAnime, header, 1.0f, 0.0f, 0.0f, ANIMMODE_LOOP, -6.0f);
            break;
        case ANIM_GRIND:
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_down_slope_slip;
            LinkAnimation_Change(play, &player->skelAnime, header, 1.0f, 0.0f, 0.0f, ANIMMODE_LOOP, -4.0f);
            break;
        case ANIM_PUSH:
            // One stride of Link's run cycle: the planted leg sweeps back along the ground while the arms swing.
            // We drive the frame ourselves (play speed 0) so the stroke lines up with the speed boost.
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_run_free;
            sSkate.pushStrideEnd = Animation_GetLastFrame(header) * 0.5f;
            LinkAnimation_Change(play, &player->skelAnime, header, 0.0f, 0.0f, 0.0f, ANIMMODE_LOOP, -3.0f);
            break;
        case ANIM_TOW:
            // Arms out in front, holding on
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_carryB_wait;
            LinkAnimation_Change(play, &player->skelAnime, header, 1.0f, 0.0f, 0.0f, ANIMMODE_LOOP, -4.0f);
            break;
        case ANIM_LAND:
            header = (LinkAnimationHeader*)gPlayerAnim_link_normal_short_landing_free;
            LinkAnimation_Change(play, &player->skelAnime, header, 1.0f, 0.0f, Animation_GetLastFrame(header),
                                 ANIMMODE_ONCE, -2.0f);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Area quests: spell Z-E-L-D-A, clear gaps, land a big combo. Available in every outdoor area of Hyrule.
// Progress is stored in the settings file (shipofharkinian.json), so it's shared between save files.
// ---------------------------------------------------------------------------------------------------------------------

enum QuestBits { QUEST_LETTERS = 1 << 0, QUEST_GAPS = 1 << 1, QUEST_COMBO = 1 << 2, QUEST_ALL = 7 };

static constexpr s32 kLetterCount = 5;
static const char* const kLetterChars[kLetterCount] = { "Z", "E", "L", "D", "A" };
static const char* const kLetterTag = "skate_letter";

struct QuestLetter {
    Vec3f pos;
    bool collected;
    Actor tagAnchor; // stand-in actor so SoH's name tag system can float the letter above the rupee
};

static QuestLetter sLetters[kLetterCount];
static bool sLettersReady = false;
static bool sGoalsShown = false; // the "GOALS" reminder shows once per visit to an area
static u32 sQuestRng = 1;

static bool IsQuestArea(s32 scene) {
    return scene >= SCENE_HYRULE_FIELD && scene <= SCENE_LON_LON_RANCH;
}

static std::string QuestKey(const char* what, s32 scene) {
    return std::string(CVAR_SKATE("Quest.")) + what + "." + std::to_string(scene);
}

static s32 QuestDone(s32 scene) {
    return CVarGetInteger(QuestKey("Done", scene).c_str(), 0);
}

static s32 LettersMask(s32 scene) {
    return CVarGetInteger(QuestKey("Letters", scene).c_str(), 0);
}

struct StoredGap {
    s32 x1, z1, x2, z2;
};

static std::vector<StoredGap> LoadGaps(s32 scene) {
    std::vector<StoredGap> gaps;
    std::string data = CVarGetString(QuestKey("Gaps", scene).c_str(), "");
    size_t start = 0;
    while (start < data.size()) {
        size_t end = data.find(';', start);
        if (end == std::string::npos) {
            end = data.size();
        }
        std::string entry = data.substr(start, end - start);
        s32 v[4] = { 0, 0, 0, 0 };
        s32 n = 0;
        size_t p = 0;
        while (n < 4 && p <= entry.size()) {
            size_t comma = entry.find(',', p);
            if (comma == std::string::npos) {
                comma = entry.size();
            }
            if (comma > p) {
                v[n++] = (s32)std::strtol(entry.substr(p, comma - p).c_str(), nullptr, 10);
            }
            p = comma + 1;
        }
        if (n == 4) {
            gaps.push_back({ v[0], v[1], v[2], v[3] });
        }
        start = end + 1;
    }
    return gaps;
}

static void SaveGaps(s32 scene, const std::vector<StoredGap>& gaps) {
    std::string data;
    for (const StoredGap& g : gaps) {
        data += std::to_string(g.x1) + "," + std::to_string(g.z1) + "," + std::to_string(g.x2) + "," +
                std::to_string(g.z2) + ";";
    }
    CVarSetString(QuestKey("Gaps", scene).c_str(), data.c_str());
    SaveCVars();
}

static std::string UpperCase(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
    }
    return text;
}

static void CompleteQuest(s32 scene, s32 bit, const char* name) {
    s32 done = QuestDone(scene);
    if (done & bit) {
        return;
    }
    done |= bit;
    CVarSetInteger(QuestKey("Done", scene).c_str(), done);
    SaveCVars();

    ShowMsg(std::string("QUEST COMPLETE: ") + name, MSG_GOOD, 70);
    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
    if (Cfg_QuestRupees()) {
        Rupees_ChangeBy(20);
    }
    if (done == QUEST_ALL) {
        ShowMsg(UpperCase(SohUtils::GetSceneName(scene)) + " MASTERED!", MSG_GOOD, 90);
        Sfx_PlaySfxCentered(NA_SE_SY_GET_ITEM);
        if (Cfg_QuestRupees()) {
            Rupees_ChangeBy(50);
        }
    }
}

// Remembers a gap by where it starts and lands. Returns true the first time a gap is cleared.
static bool RegisterGap(s32 scene, const Vec3f& from, const Vec3f& to) {
    std::vector<StoredGap> gaps = LoadGaps(scene);
    for (const StoredGap& g : gaps) {
        f32 d1 = sqrtf(SQ((f32)g.x1 - from.x) + SQ((f32)g.z1 - from.z));
        f32 d2 = sqrtf(SQ((f32)g.x2 - to.x) + SQ((f32)g.z2 - to.z));
        if (d1 < 250.0f && d2 < 250.0f) {
            return false;
        }
    }
    gaps.push_back({ (s32)from.x, (s32)from.z, (s32)to.x, (s32)to.z });
    SaveGaps(scene, gaps);
    return true;
}

static f32 QuestRand() {
    sQuestRng = sQuestRng * 1664525u + 1013904223u;
    return (f32)(sQuestRng >> 8) / 16777216.0f;
}

static void LetterTagDraw(Actor*, PlayState*) {
}

static void ClearLetters() {
    NameTag_RemoveAllByTag(kLetterTag);
    sLettersReady = false;
}

// Scatters the five letters around wherever Link entered the area: on reachable ground, some floating at ollie
// height, some out on the water when water skating is on. Each way into an area gets its own layout, but a letter
// you've collected stays collected for that area.
static void PlaceLetters(PlayState* play, Player* player) {
    f32 age = AgeScale();
    Vec3f anchor = player->actor.world.pos;
    f32 anchorFloor = player->actor.floorHeight;
    f32 maxRadius = (play->sceneNum == SCENE_HYRULE_FIELD) ? 2400.0f : 1300.0f;
    s32 mask = LettersMask(play->sceneNum);

    sQuestRng = (u32)(play->sceneNum * 7919 + gSaveContext.entranceIndex * 104729 + 12345);

    Vec3f spots[kLetterCount];
    s32 placed = 0;
    for (s32 pass = 0; pass < 3 && placed < kLetterCount; pass++) {
        // Each pass is less picky, so even cramped areas end up with all five letters
        f32 heightRange = 260.0f + (f32)pass * 200.0f;
        f32 spacing = 350.0f - (f32)pass * 120.0f;
        f32 radius = maxRadius - (f32)pass * 300.0f;
        for (s32 tries = 0; tries < 400 && placed < kLetterCount; tries++) {
            f32 angle = QuestRand() * 2.0f * kPi;
            f32 r = 200.0f + QuestRand() * (radius - 200.0f);
            f32 x = anchor.x + sinf(angle) * r;
            f32 z = anchor.z + cosf(angle) * r;

            Vec3f probe = { x, anchorFloor + heightRange, z };
            CollisionPoly* poly = NULL;
            s32 bgId;
            f32 y = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
            if (poly == NULL || y < anchorFloor - heightRange || COLPOLY_GET_NORMAL(poly->normal.y) < 0.75f) {
                continue;
            }

            f32 waterY = y;
            WaterBox* waterBox;
            if (WaterBox_GetSurface1(play, &play->colCtx, x, z, &waterY, &waterBox) && waterY > y + 1.0f) {
                if (!Cfg_WaterSkate()) {
                    continue;
                }
                y = waterY;
            }

            bool tooClose = false;
            for (s32 i = 0; i < placed; i++) {
                if (sqrtf(SQ(spots[i].x - x) + SQ(spots[i].z - z)) < spacing) {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose) {
                continue;
            }

            f32 lift = (QuestRand() < 0.4f) ? 55.0f : 25.0f; // the high ones need an ollie
            spots[placed++] = { x, y + lift * age, z };
        }
    }
    // Last resort: line them up in front of Link
    while (placed < kLetterCount) {
        f32 r = 150.0f + (f32)placed * 120.0f;
        s16 yaw = player->actor.shape.rot.y;
        spots[placed] = { anchor.x + Math_SinS(yaw) * r, anchorFloor + 25.0f * age, anchor.z + Math_CosS(yaw) * r };
        placed++;
    }

    // Spell it out as you go: Z is the closest, A the farthest
    std::sort(spots, spots + kLetterCount, [&](const Vec3f& a, const Vec3f& b) {
        return SQ(a.x - anchor.x) + SQ(a.z - anchor.z) < SQ(b.x - anchor.x) + SQ(b.z - anchor.z);
    });

    NameTag_RemoveAllByTag(kLetterTag);
    for (s32 i = 0; i < kLetterCount; i++) {
        QuestLetter& letter = sLetters[i];
        letter.pos = spots[i];
        letter.collected = (mask & (1 << i)) != 0;
        memset(&letter.tagAnchor, 0, sizeof(Actor));
        letter.tagAnchor.draw = LetterTagDraw;
        letter.tagAnchor.isDrawn = true;
        letter.tagAnchor.world.pos = letter.pos;
        letter.tagAnchor.xyzDistToPlayerSq = 0.0f;
        if (!letter.collected) {
            NameTagOptions options = {};
            options.tag = kLetterTag;
            options.yOffset = 8;
            options.textColor = { 255, 215, 60, 255 };
            options.noZBuffer = true;
            NameTag_RegisterForActorWithOptions(&letter.tagAnchor, kLetterChars[i], options);
        }
    }
    sLettersReady = true;
}

static std::string LettersProgressText(s32 mask) {
    std::string text;
    for (s32 i = 0; i < kLetterCount; i++) {
        text += (mask & (1 << i)) ? kLetterChars[i] : "_";
        if (i < kLetterCount - 1) {
            text += " ";
        }
    }
    return text;
}

static void CollectLetter(PlayState* play, Player* player, s32 index);

static void UpdateLetters(PlayState* play, Player* player, bool riding) {
    if (!sLettersReady) {
        return;
    }
    f32 age = AgeScale();
    for (s32 i = 0; i < kLetterCount; i++) {
        QuestLetter& letter = sLetters[i];
        if (letter.collected) {
            continue;
        }
        f32 dx = letter.pos.x - player->actor.world.pos.x;
        f32 dz = letter.pos.z - player->actor.world.pos.z;
        f32 dy = letter.pos.y - (player->actor.world.pos.y + 20.0f * age);
        letter.tagAnchor.xyzDistToPlayerSq = SQ(dx) + SQ(dz) + SQ(dy);
        if (riding && SQ(dx) + SQ(dz) < SQ(40.0f * age) && std::fabs(dy) < 45.0f * age) {
            CollectLetter(play, player, i);
        }
    }
}

static void CollectLetter(PlayState* play, Player* player, s32 index) {
    QuestLetter& letter = sLetters[index];
    letter.collected = true;
    NameTag_RemoveAllForActor(&letter.tagAnchor);

    s32 scene = play->sceneNum;
    s32 mask = LettersMask(scene) | (1 << index);
    CVarSetInteger(QuestKey("Letters", scene).c_str(), mask);
    SaveCVars();

    sSkate.combo.Add(std::string("LETTER ") + kLetterChars[index], 100);
    Sfx_PlaySfxCentered(NA_SE_SY_GET_RUPY);
    ShowMsg(LettersProgressText(mask), MSG_INFO, 35);

    static Color_RGBA8 sPrim = { 255, 255, 180, 255 };
    static Color_RGBA8 sEnv = { 255, 200, 0, 255 };
    for (s32 i = 0; i < 6; i++) {
        Vec3f vel = { Rand_CenteredFloat(4.0f), 2.0f + Rand_ZeroOne() * 3.0f, Rand_CenteredFloat(4.0f) };
        Vec3f accel = { 0.0f, -0.3f, 0.0f };
        EffectSsKiraKira_SpawnSmall(play, &letter.pos, &vel, &accel, &sPrim, &sEnv);
    }

    if (mask == (1 << kLetterCount) - 1) {
        CompleteQuest(scene, QUEST_LETTERS, "Z-E-L-D-A");
    }
    (void)player;
}

static void DrawLetters(PlayState* play) {
    if (!sLettersReady) {
        return;
    }
    for (s32 i = 0; i < kLetterCount; i++) {
        QuestLetter& letter = sLetters[i];
        if (letter.collected) {
            continue;
        }
        Vec3f pos = letter.pos;
        pos.y += 3.0f * Math_SinS((s16)(play->gameplayFrames * 0x400 + i * 0x3000)); // gentle bob
        DrawItemAt(play, &letter, pos, GID_RUPEE_GOLD, 0.45f * AgeScale(), (s16)(play->gameplayFrames * 0x300));
    }
}

// Where's the next letter? "AHEAD 320", "LEFT 150"... relative to the direction you're rolling
static std::string NextLetterHint(Player* player) {
    s32 best = -1;
    f32 bestDist = 0.0f;
    for (s32 i = 0; i < kLetterCount; i++) {
        if (sLetters[i].collected) {
            continue;
        }
        f32 d = Math_Vec3f_DistXZ(&player->actor.world.pos, &sLetters[i].pos);
        if (best < 0 || d < bestDist) {
            best = i;
            bestDist = d;
        }
    }
    if (best < 0) {
        return "";
    }
    s16 rel = (s16)(Math_Vec3f_Yaw(&player->actor.world.pos, &sLetters[best].pos) - sSkate.moveYaw);
    const char* dir = "AHEAD";
    if (ABS(rel) > 0x6000) {
        dir = "BEHIND";
    } else if (rel > 0x1800) {
        dir = "LEFT";
    } else if (rel < -0x1800) {
        dir = "RIGHT";
    }
    return std::string(kLetterChars[best]) + " " + dir + " " + std::to_string((s32)bestDist);
}

static std::string QuestHudLine(s32 scene) {
    s32 done = QuestDone(scene);
    s32 gaps = (s32)LoadGaps(scene).size();
    std::string text = LettersProgressText(LettersMask(scene));
    text += "  GAPS " + std::to_string(std::min(gaps, Tune::kQuestGapGoal)) + "/" + std::to_string(Tune::kQuestGapGoal);
    text += (done & QUEST_COMBO) ? "  5K OK" : "  5K -";
    return text;
}

// ---------------------------------------------------------------------------------------------------------------------
// Hyrule Skate Park: a custom actor with its own collision and vertex-colored geometry, placed in Hyrule Field.
//
// Layout (seen from above, +z is north). The deck is 1200 x 1200, raised a little above the field:
//
//        [ Goron funbox ]  [========= north quarter pipe =========]  [ Master Sword rail ]
//
//   west                            /\   <- Triforce (3 golden funboxes,                    east
//   entry ramp                     /__\     jump the hole in the middle)                    entry ramp
//                                 /\  /\   (bottom pieces)
//                                /__\/__\  (hole in the middle)
//                               [ launch ramp ]
//
//        [ Master Sword rail ]  [========= south quarter pipe =========]  [ Goron funbox ]
//
// The two quarter pipes face each other and make a half pipe. Riding up one sends you straight up ("vert"), and
// you come back down into it. Floating above the park: the three Spiritual Stones (one over each Triforce piece),
// a Heart Container over the north lip and the Ocarina of Time over the south lip. Collect them all.
// ---------------------------------------------------------------------------------------------------------------------

namespace Park {
constexpr f32 kHalf = 600.0f;    // deck half size
constexpr f32 kRampLen = 200.0f; // entry ramps on the east and west edges
constexpr f32 kRampHalfWidth = 140.0f;
constexpr f32 kQpHalfWidth = 350.0f; // quarter pipes span x in [-350, 350]
constexpr f32 kQpRadius = 260.0f;
constexpr f32 kQpAngle = 55.0f; // steepest part of the curve (degrees). Steeper would count as a wall.
constexpr s32 kQpSegments = 6;
constexpr f32 kQpLip = 560.0f;    // z of the lip; the platform behind it reaches the deck edge
constexpr f32 kBoxHeight = 45.0f; // funboxes, rails and Triforce pieces: tall enough to grind as adult Link
constexpr f32 kRailHalfWidth = 5.0f;
constexpr f32 kTriSide = 420.0f; // side length of the whole Triforce
} // namespace Park

enum ParkGoalBits {
    PARK_STONES = 1 << 0,
    PARK_VERT = 1 << 1,
    PARK_TRIGAP = 1 << 2,
    PARK_COMBO = 1 << 3,
    PARK_ALL = 15
};

struct ParkTreasure {
    Vec3f local; // position relative to the park center (deck top = 0)
    s16 gid;
    f32 scale;
    const char* name;
    s32 goal; // which goal it counts toward
};

static ParkTreasure sParkTreasures[] = {
    { { 0.0f, 0.0f, 0.0f }, GID_KOKIRI_EMERALD, 0.9f, "KOKIRI EMERALD", PARK_STONES },
    { { 0.0f, 0.0f, 0.0f }, GID_GORON_RUBY, 0.9f, "GORON RUBY", PARK_STONES },
    { { 0.0f, 0.0f, 0.0f }, GID_ZORA_SAPPHIRE, 0.9f, "ZORA SAPPHIRE", PARK_STONES },
    { { 0.0f, 0.0f, 0.0f }, GID_HEART_CONTAINER, 1.2f, "HEART CONTAINER", PARK_VERT },
    { { 0.0f, 0.0f, 0.0f }, GID_OCARINA_TIME, 1.4f, "OCARINA OF TIME", PARK_VERT },
};
static constexpr s32 kParkTreasureCount = 5;

struct ParkState {
    // where it is (saved in the settings)
    bool placed = false;
    Vec3f center = { 0.0f, 0.0f, 0.0f }; // deck top center
    f32 groundMin = 0.0f;                // lowest terrain under the footprint (for the skirt)
    f32 rampFootY[2] = { 0.0f, 0.0f };   // terrain height at the east / west ramp feet

    // the live actor
    DynaPolyActor* actor = nullptr;
    s32 bgId = -1;
    s16 actorId = -1;
    bool respawn = false;

    // automatic spot search, spread over several frames
    bool searching = false;
    s32 searchIdx = 0;
    s32 searchCount = 0;
    s32 searchCols = 0;
    f32 bestScore = 0.0f;
    Vec3f bestCenter = { 0.0f, 0.0f, 0.0f };
    bool found = false;

    // goals
    bool overTriHole = false;
    bool announced = false;
    bool failed = false; // couldn't spawn in this visit (e.g. the area's collision budget is full)
};
static ParkState sPark;

// Built geometry. Rebuilt only while no park actor exists, so the game never sees these move.
static std::vector<Vec3s> sParkColVtx;
static std::vector<CollisionPoly> sParkColPoly;
static SurfaceType sParkSurfaces[2] = { { { 0, 2 } }, { { 0, 13 } } }; // concrete, metal (rails)
static Vec3s sParkCamPos[3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
static CamData sParkCamData[1] = { { CAM_SET_NONE, 0, sParkCamPos } };
static CollisionHeader sParkCol;
static std::vector<Vtx> sParkVtx;
static std::vector<Gfx> sParkGfx;

static bool Cfg_Park() {
    return CVarGetInteger(CVAR_SKATE("Park"), 1) != 0;
}

static s32 ParkDone() {
    return CVarGetInteger(CVAR_SKATE("Park.Done"), 0);
}

static s32 ParkTreasureMask() {
    return CVarGetInteger(CVAR_SKATE("Park.Treasures"), 0);
}

// ---- geometry builder ----------------------------------------------------------------------------------------------

template <typename T> static void SetVtxPos(T& dst, f32 value) {
    dst = static_cast<T>(value);
}

static Vec3f V3(f32 x, f32 y, f32 z) {
    Vec3f v = { x, y, z };
    return v;
}

static Vec3f Sub(const Vec3f& a, const Vec3f& b) {
    return V3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static Vec3f Cross(const Vec3f& a, const Vec3f& b) {
    return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static f32 Dot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct ParkColor {
    u8 r, g, b;
};

// One triangle. `out` points away from the solid, so the collision faces the right way.
static void ParkTri(Vec3f a, Vec3f b, Vec3f c, ParkColor color, Vec3f out, bool collide = true, s32 surface = 0) {
    Vec3f n = Cross(Sub(b, a), Sub(c, a));
    if (Dot(n, out) < 0.0f) {
        Vec3f t = b;
        b = c;
        c = t;
        n = V3(-n.x, -n.y, -n.z);
    }
    f32 len = sqrtf(Dot(n, n));
    if (len < 0.001f) {
        return;
    }
    n = V3(n.x / len, n.y / len, n.z / len);

    if (collide) {
        // Corners are shared between faces to keep the vertex count low (the engine has a fixed budget)
        u16 idx[3];
        const Vec3f* corners[3] = { &a, &b, &c };
        for (s32 k = 0; k < 3; k++) {
            Vec3s s = { (s16)lroundf(corners[k]->x), (s16)lroundf(corners[k]->y), (s16)lroundf(corners[k]->z) };
            size_t found = sParkColVtx.size();
            for (size_t j = 0; j < sParkColVtx.size(); j++) {
                if (sParkColVtx[j].x == s.x && sParkColVtx[j].y == s.y && sParkColVtx[j].z == s.z) {
                    found = j;
                    break;
                }
            }
            if (found == sParkColVtx.size()) {
                sParkColVtx.push_back(s);
            }
            idx[k] = (u16)found;
        }
        CollisionPoly poly;
        memset(&poly, 0, sizeof(poly));
        poly.type = (u16)surface;
        poly.flags_vIA = idx[0];
        poly.flags_vIB = idx[1];
        poly.vIC = idx[2];
        poly.normal.x = (s16)(n.x * 32767.0f);
        poly.normal.y = (s16)(n.y * 32767.0f);
        poly.normal.z = (s16)(n.z * 32767.0f);
        poly.dist = (s16)lroundf(-Dot(n, a));
        sParkColPoly.push_back(poly);
    }

    // Simple sun shading so the shapes read clearly
    Vec3f light = V3(0.35f, 0.85f, 0.4f);
    f32 shade = 0.55f + 0.45f * std::max(0.0f, Dot(n, light) / sqrtf(Dot(light, light)));
    for (const Vec3f* p : { &a, &b, &c }) {
        Vtx v;
        memset(&v, 0, sizeof(v));
        SetVtxPos(v.v.ob[0], p->x);
        SetVtxPos(v.v.ob[1], p->y);
        SetVtxPos(v.v.ob[2], p->z);
        v.v.cn[0] = (u8)std::min(255.0f, color.r * shade);
        v.v.cn[1] = (u8)std::min(255.0f, color.g * shade);
        v.v.cn[2] = (u8)std::min(255.0f, color.b * shade);
        v.v.cn[3] = 255;
        sParkVtx.push_back(v);
    }
}

static void ParkQuad(Vec3f a, Vec3f b, Vec3f c, Vec3f d, ParkColor color, Vec3f out, bool collide = true,
                     s32 surface = 0) {
    ParkTri(a, b, c, color, out, collide, surface);
    ParkTri(a, c, d, color, out, collide, surface);
}

// Axis-aligned box sitting on y0, top at y1 (no bottom face)
static void ParkBox(f32 x0, f32 x1, f32 z0, f32 z1, f32 y0, f32 y1, ParkColor top, ParkColor side, s32 surface = 0) {
    ParkQuad(V3(x0, y1, z0), V3(x1, y1, z0), V3(x1, y1, z1), V3(x0, y1, z1), top, V3(0, 1, 0), true, surface);
    ParkQuad(V3(x0, y0, z0), V3(x1, y0, z0), V3(x1, y1, z0), V3(x0, y1, z0), side, V3(0, 0, -1), true, surface);
    ParkQuad(V3(x0, y0, z1), V3(x1, y0, z1), V3(x1, y1, z1), V3(x0, y1, z1), side, V3(0, 0, 1), true, surface);
    ParkQuad(V3(x0, y0, z0), V3(x0, y0, z1), V3(x0, y1, z1), V3(x0, y1, z0), side, V3(-1, 0, 0), true, surface);
    ParkQuad(V3(x1, y0, z0), V3(x1, y0, z1), V3(x1, y1, z1), V3(x1, y1, z0), side, V3(1, 0, 0), true, surface);
}

// Ramp along z: rises from height 0 at zLow to height h at zHigh (zHigh may be smaller than zLow)
static void ParkKickerZ(f32 x0, f32 x1, f32 zLow, f32 zHigh, f32 h, ParkColor top, ParkColor side, bool backFace) {
    f32 dir = (zHigh > zLow) ? 1.0f : -1.0f;
    ParkQuad(V3(x0, 0, zLow), V3(x1, 0, zLow), V3(x1, h, zHigh), V3(x0, h, zHigh), top, V3(0, 1, -dir * 0.5f));
    ParkTri(V3(x0, 0, zLow), V3(x0, 0, zHigh), V3(x0, h, zHigh), side, V3(-1, 0, 0));
    ParkTri(V3(x1, 0, zLow), V3(x1, 0, zHigh), V3(x1, h, zHigh), side, V3(1, 0, 0));
    if (backFace) {
        ParkQuad(V3(x0, 0, zHigh), V3(x1, 0, zHigh), V3(x1, h, zHigh), V3(x0, h, zHigh), side, V3(0, 0, dir));
    }
}

// Quarter pipe across x in [-w, w]. `dir` = +1: the lip is at +kQpLip and the ramp faces south, -1: mirrored.
static void ParkQuarterPipe(f32 dir, f32 bottom, ParkColor curve, ParkColor deck, ParkColor wall) {
    using namespace Park;
    f32 w = kQpHalfWidth;
    f32 maxA = kQpAngle * kPi / 180.0f;
    f32 zStart = kQpLip - kQpRadius * sinf(maxA);
    f32 top = kQpRadius * (1.0f - cosf(maxA));

    Vec3f profile[kQpSegments + 1]; // (unused x) z, y
    for (s32 i = 0; i <= kQpSegments; i++) {
        f32 a = maxA * (f32)i / (f32)kQpSegments;
        profile[i] = V3(0.0f, kQpRadius * (1.0f - cosf(a)), dir * (zStart + kQpRadius * sinf(a)));
    }
    // the curve
    for (s32 i = 0; i < kQpSegments; i++) {
        f32 a = maxA * ((f32)i + 0.5f) / (f32)kQpSegments;
        Vec3f out = V3(0.0f, cosf(a), -dir * sinf(a));
        ParkQuad(V3(-w, profile[i].y, profile[i].z), V3(w, profile[i].y, profile[i].z),
                 V3(w, profile[i + 1].y, profile[i + 1].z), V3(-w, profile[i + 1].y, profile[i + 1].z), curve, out);
    }
    // platform behind the lip, out to the deck edge
    f32 lipZ = dir * kQpLip;
    f32 edgeZ = dir * kHalf;
    ParkQuad(V3(-w, top, lipZ), V3(w, top, lipZ), V3(w, top, edgeZ), V3(-w, top, edgeZ), deck, V3(0, 1, 0));
    // back wall down to the ground
    ParkQuad(V3(-w, bottom, edgeZ), V3(w, bottom, edgeZ), V3(w, top, edgeZ), V3(-w, top, edgeZ), wall, V3(0, 0, dir));
    // side walls (fan from the bottom of the curve)
    for (f32 sx : { -w, w }) {
        Vec3f out = V3(sx > 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
        Vec3f corner = V3(sx, 0.0f, edgeZ);
        for (s32 i = 0; i < kQpSegments; i++) {
            ParkTri(corner, V3(sx, profile[i].y, profile[i].z), V3(sx, profile[i + 1].y, profile[i + 1].z), wall, out);
        }
        ParkTri(corner, V3(sx, profile[kQpSegments].y, profile[kQpSegments].z), V3(sx, top, edgeZ), wall, out);
    }
}

// Triangular prism (Triforce piece) with corners a, b, c on the deck
static void ParkPrism(Vec3f a, Vec3f b, Vec3f c, f32 h, ParkColor top, ParkColor side) {
    Vec3f center = V3((a.x + b.x + c.x) / 3.0f, 0.0f, (a.z + b.z + c.z) / 3.0f);
    ParkTri(V3(a.x, h, a.z), V3(b.x, h, b.z), V3(c.x, h, c.z), top, V3(0, 1, 0));
    const Vec3f* pts[3] = { &a, &b, &c };
    for (s32 i = 0; i < 3; i++) {
        const Vec3f& p = *pts[i];
        const Vec3f& q = *pts[(i + 1) % 3];
        Vec3f mid = V3((p.x + q.x) * 0.5f, 0.0f, (p.z + q.z) * 0.5f);
        Vec3f out = Sub(mid, center);
        ParkQuad(V3(p.x, 0, p.z), V3(q.x, 0, q.z), V3(q.x, h, q.z), V3(p.x, h, p.z), side, out);
    }
}

static void TriforcePoints(Vec3f* T, Vec3f* L, Vec3f* R, Vec3f* mTL, Vec3f* mTR, Vec3f* mLR) {
    f32 h = Park::kTriSide * 0.8660254f;
    *T = V3(0.0f, 0.0f, h * 2.0f / 3.0f);
    *L = V3(-Park::kTriSide * 0.5f, 0.0f, -h / 3.0f);
    *R = V3(Park::kTriSide * 0.5f, 0.0f, -h / 3.0f);
    *mTL = V3((T->x + L->x) * 0.5f, 0.0f, (T->z + L->z) * 0.5f);
    *mTR = V3((T->x + R->x) * 0.5f, 0.0f, (T->z + R->z) * 0.5f);
    *mLR = V3((L->x + R->x) * 0.5f, 0.0f, (L->z + R->z) * 0.5f);
}

static bool PointInTri(f32 px, f32 pz, const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    f32 d1 = (px - b.x) * (a.z - b.z) - (a.x - b.x) * (pz - b.z);
    f32 d2 = (px - c.x) * (b.z - c.z) - (b.x - c.x) * (pz - c.z);
    f32 d3 = (px - a.x) * (c.z - a.z) - (c.x - a.x) * (pz - a.z);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

static void BuildParkGeometry() {
    using namespace Park;
    sParkColVtx.clear();
    sParkColPoly.clear();
    sParkVtx.clear();
    sParkGfx.clear();

    const ParkColor stone = { 205, 198, 186 };
    const ParkColor stoneDark = { 125, 115, 104 };
    const ParkColor hylianBlue = { 55, 85, 200 };
    const ParkColor blueDark = { 35, 50, 130 };
    const ParkColor gold = { 245, 200, 60 };
    const ParkColor goldDark = { 190, 140, 35 };
    const ParkColor silver = { 215, 222, 238 };
    const ParkColor kokiri = { 70, 175, 75 };
    const ParkColor kokiriDark = { 40, 115, 45 };
    const ParkColor goron = { 185, 85, 55 };
    const ParkColor goronDark = { 125, 55, 35 };

    f32 bottom = sPark.groundMin - sPark.center.y - 30.0f; // skirt reaches below the lowest terrain

    // Deck
    ParkQuad(V3(-kHalf, 0, -kHalf), V3(kHalf, 0, -kHalf), V3(kHalf, 0, kHalf), V3(-kHalf, 0, kHalf), stone,
             V3(0, 1, 0));
    // Skirt (outside walls of the deck). The quarter pipes' back walls cover the middle of the north/south edges.
    for (f32 sz : { -1.0f, 1.0f }) {
        for (f32 sx : { -1.0f, 1.0f }) {
            f32 xa = sx * kQpHalfWidth;
            f32 xb = sx * kHalf;
            ParkQuad(V3(xa, bottom, sz * kHalf), V3(xb, bottom, sz * kHalf), V3(xb, 0, sz * kHalf),
                     V3(xa, 0, sz * kHalf), stoneDark, V3(0, 0, sz));
        }
    }
    for (f32 sx : { -1.0f, 1.0f }) {
        f32 x = sx * kHalf;
        // leave the entry ramp opening at z in [-kRampHalfWidth, kRampHalfWidth] (the ramp covers it)
        ParkQuad(V3(x, bottom, -kHalf), V3(x, bottom, -kRampHalfWidth), V3(x, 0, -kRampHalfWidth), V3(x, 0, -kHalf),
                 stoneDark, V3(sx, 0, 0));
        ParkQuad(V3(x, bottom, kRampHalfWidth), V3(x, bottom, kHalf), V3(x, 0, kHalf), V3(x, 0, kRampHalfWidth),
                 stoneDark, V3(sx, 0, 0));
        // entry ramp down to the field
        f32 footY = sPark.rampFootY[sx > 0 ? 0 : 1] - sPark.center.y - 4.0f;
        f32 xOut = sx * (kHalf + kRampLen);
        ParkQuad(V3(x, 0, -kRampHalfWidth), V3(x, 0, kRampHalfWidth), V3(xOut, footY, kRampHalfWidth),
                 V3(xOut, footY, -kRampHalfWidth), stone, V3(0, 1, 0));
        for (f32 sz : { -1.0f, 1.0f }) {
            f32 z = sz * kRampHalfWidth;
            ParkTri(V3(x, 0, z), V3(xOut, footY, z), V3(x, footY, z), stoneDark, V3(0, 0, sz));
        }
    }

    // Half pipe: two quarter pipes facing each other
    ParkQuarterPipe(1.0f, bottom, hylianBlue, gold, blueDark);
    ParkQuarterPipe(-1.0f, bottom, hylianBlue, gold, blueDark);

    // Triforce: three golden funboxes, with the classic hole in the middle
    Vec3f T, L, R, mTL, mTR, mLR;
    TriforcePoints(&T, &L, &R, &mTL, &mTR, &mLR);
    ParkPrism(T, mTL, mTR, kBoxHeight, gold, goldDark);
    ParkPrism(mTL, L, mLR, kBoxHeight, gold, goldDark);
    ParkPrism(mTR, mLR, R, kBoxHeight, gold, goldDark);
    // launch ramp up onto the two bottom pieces
    ParkKickerZ(L.x, R.x, L.z - 150.0f, L.z, kBoxHeight, kokiri, kokiriDark, false);

    // Master Sword rails (north-east and south-west) with kickers in front
    ParkBox(470.0f - kRailHalfWidth, 470.0f + kRailHalfWidth, 190.0f, 520.0f, 0.0f, kBoxHeight, silver, silver, 1);
    ParkKickerZ(440.0f, 500.0f, 120.0f, 175.0f, 28.0f, kokiri, kokiriDark, true);
    ParkBox(-470.0f - kRailHalfWidth, -470.0f + kRailHalfWidth, -520.0f, -190.0f, 0.0f, kBoxHeight, silver, silver, 1);
    ParkKickerZ(-500.0f, -440.0f, -120.0f, -175.0f, 28.0f, kokiri, kokiriDark, true);

    // Goron funboxes (north-west and south-east) with a kicker on the inner end
    ParkBox(-560.0f, -390.0f, 230.0f, 520.0f, 0.0f, kBoxHeight, goron, goronDark);
    ParkKickerZ(-560.0f, -390.0f, 150.0f, 230.0f, kBoxHeight, kokiri, kokiriDark, false);
    ParkBox(390.0f, 560.0f, -520.0f, -230.0f, 0.0f, kBoxHeight, goron, goronDark);
    ParkKickerZ(390.0f, 560.0f, -150.0f, -230.0f, kBoxHeight, kokiri, kokiriDark, false);

    // Treasures: a Spiritual Stone over each Triforce piece, vert treasures over the lips
    f32 qpTop = kQpRadius * (1.0f - cosf(kQpAngle * kPi / 180.0f));
    Vec3f pieces[3] = { V3((T.x + mTL.x + mTR.x) / 3.0f, 0, (T.z + mTL.z + mTR.z) / 3.0f),
                        V3((mTL.x + L.x + mLR.x) / 3.0f, 0, (mTL.z + L.z + mLR.z) / 3.0f),
                        V3((mTR.x + mLR.x + R.x) / 3.0f, 0, (mTR.z + mLR.z + R.z) / 3.0f) };
    for (s32 i = 0; i < 3; i++) {
        sParkTreasures[i].local = V3(pieces[i].x, kBoxHeight + 85.0f, pieces[i].z); // ollie off the piece
    }
    sParkTreasures[3].local = V3(0.0f, qpTop + 110.0f, kQpLip - 10.0f); // big vert air
    sParkTreasures[4].local = V3(0.0f, qpTop + 110.0f, -kQpLip + 10.0f);

    // Collision header
    memset(&sParkCol, 0, sizeof(sParkCol));
    Vec3s mn = { 32767, 32767, 32767 };
    Vec3s mx = { -32767, -32767, -32767 };
    for (const Vec3s& v : sParkColVtx) {
        mn.x = std::min(mn.x, v.x);
        mn.y = std::min(mn.y, v.y);
        mn.z = std::min(mn.z, v.z);
        mx.x = std::max(mx.x, v.x);
        mx.y = std::max(mx.y, v.y);
        mx.z = std::max(mx.z, v.z);
    }
    sParkCol.minBounds = mn;
    sParkCol.maxBounds = mx;
    sParkCol.numVertices = (u16)sParkColVtx.size();
    sParkCol.vtxList = sParkColVtx.data();
    sParkCol.numPolygons = (u16)sParkColPoly.size();
    sParkCol.polyList = sParkColPoly.data();
    sParkCol.surfaceTypeList = sParkSurfaces;
    sParkCol.cameraDataList = sParkCamData;
    sParkCol.cameraDataListLen = 1;
    sParkCol.numWaterBoxes = 0;
    sParkCol.waterBoxes = NULL;

    // Display list: flat vertex colors, depth tested, no texture
    u32 rm = Z_CMP | Z_UPD | CVG_DST_CLAMP | FORCE_BL | ZMODE_OPA;
    u32 blc1 = GBL_c1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1);
    u32 blc2 = GBL_c2(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1);
    sParkGfx.push_back(gsDPPipeSync());
    sParkGfx.push_back(gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_OFF));
    sParkGfx.push_back(gsDPSetCycleType(G_CYC_1CYCLE));
    sParkGfx.push_back(gsDPSetRenderMode(rm | blc1, rm | blc2));
    sParkGfx.push_back(gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE));
    sParkGfx.push_back(gsSPLoadGeometryMode(G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH));
    const size_t kChunk = 30; // 10 triangles per vertex load
    for (size_t start = 0; start < sParkVtx.size(); start += kChunk) {
        size_t count = std::min(kChunk, sParkVtx.size() - start);
        sParkGfx.push_back(gsSPVertex((uintptr_t)(sParkVtx.data() + start), (u32)count, 0));
        for (size_t t = 0; t + 2 < count; t += 3) {
            sParkGfx.push_back(gsSP1Triangle((u8)t, (u8)(t + 1), (u8)(t + 2), 0));
        }
    }
    sParkGfx.push_back(gsSPEndDisplayList());
}

// ---- the actor ------------------------------------------------------------------------------------------------------

static void ParkActor_Init(Actor* thisx, PlayState* play) {
    DynaPolyActor* dyna = (DynaPolyActor*)thisx;
    DynaPolyActor_Init(dyna, 0);
    Actor_SetScale(thisx, 1.0f);
    thisx->world.rot.x = thisx->world.rot.y = thisx->world.rot.z = 0;
    thisx->shape.rot = thisx->world.rot;
    dyna->bgId = DynaPoly_SetBgActor(play, &play->colCtx.dyna, thisx, &sParkCol);
    sPark.bgId = dyna->bgId;

    NameTagOptions options = {};
    options.tag = "skate_park";
    options.yOffset = 150;
    options.textColor = { 255, 215, 60, 255 };
    NameTag_RegisterForActorWithOptions(thisx, "HYRULE SKATE PARK", options);
}

static void ParkActor_Destroy(Actor* thisx, PlayState* play) {
    DynaPolyActor* dyna = (DynaPolyActor*)thisx;
    DynaPoly_DeleteBgActor(play, &play->colCtx.dyna, dyna->bgId);
    if (sPark.actor == dyna) {
        sPark.actor = nullptr;
        sPark.bgId = -1;
    }
}

static void ParkActor_Update(Actor*, PlayState*) {
}

static void ParkActor_Draw(Actor*, PlayState* play) {
    if (sParkGfx.empty()) {
        return;
    }
    OPEN_DISPS(play->state.gfxCtx);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, sParkGfx.data());
    CLOSE_DISPS(play->state.gfxCtx);

    s32 mask = ParkTreasureMask();
    s16 spin = (s16)(play->gameplayFrames * 0x280);
    for (s32 i = 0; i < kParkTreasureCount; i++) {
        if (mask & (1 << i)) {
            continue;
        }
        const ParkTreasure& t = sParkTreasures[i];
        Matrix_Push();
        Matrix_Translate(t.local.x, t.local.y + 4.0f * Math_SinS((s16)(spin * 2 + i * 0x2000)), t.local.z,
                         MTXMODE_APPLY);
        Matrix_RotateY(static_cast<f32>(BINANG_TO_RAD(spin)), MTXMODE_APPLY);
        Matrix_Scale(t.scale, t.scale, t.scale, MTXMODE_APPLY);
        GetItem_Draw(play, t.gid);
        Matrix_Pop();
    }

    // A big golden Triforce floating over the park, visible from far away
    Matrix_Push();
    Matrix_Translate(0.0f, 420.0f, 0.0f, MTXMODE_APPLY);
    Matrix_RotateY(static_cast<f32>(BINANG_TO_RAD((s16)(play->gameplayFrames * 0x100))), MTXMODE_APPLY);
    Matrix_Scale(4.0f, 4.0f, 4.0f, MTXMODE_APPLY);
    GetItem_Draw(play, GID_TRIFORCE_PIECE);
    Matrix_Pop();

    // Hylian Shields on the outside of the quarter pipes
    for (f32 dir : { -1.0f, 1.0f }) {
        Matrix_Push();
        Matrix_Translate(0.0f, 60.0f, dir * (Park::kHalf + 3.0f), MTXMODE_APPLY);
        Matrix_RotateY(dir > 0 ? 0.0f : kPi, MTXMODE_APPLY);
        Matrix_Scale(3.0f, 3.0f, 3.0f, MTXMODE_APPLY);
        GetItem_Draw(play, GID_SHIELD_HYLIAN);
        Matrix_Pop();
    }
}

static s16 ParkActorId() {
    if (sPark.actorId < 0) {
        ActorDBInit init;
        init.name = "Skate_Park";
        init.desc = "Hyrule Skate Park";
        init.category = ACTORCAT_BG;
        init.flags = ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
        init.objectId = OBJECT_GAMEPLAY_KEEP;
        init.instanceSize = sizeof(DynaPolyActor);
        init.init = ParkActor_Init;
        init.destroy = ParkActor_Destroy;
        init.update = ParkActor_Update;
        init.draw = ParkActor_Draw;
        sPark.actorId = (s16)ActorDB::Instance->AddEntry(init).entry.id;
    }
    return sPark.actorId;
}

// ---- placement ------------------------------------------------------------------------------------------------------

struct TerrainSample {
    bool ok;
    f32 minY, maxY;
};

// Checks the field under a park centered at (cx, cz): solid, flat-ish, dry ground, no exits, no walls.
// `detail` = false does a quick 3x3 pre-check.
static TerrainSample SampleTerrain(PlayState* play, f32 cx, f32 cz, f32 probeY, bool detail) {
    using namespace Park;
    TerrainSample result = { false, 0.0f, 0.0f };
    s32 n = detail ? 7 : 3;
    f32 hx = kHalf + kRampLen;
    f32 hz = kHalf;
    bool first = true;
    for (s32 ix = 0; ix < n; ix++) {
        for (s32 iz = 0; iz < n; iz++) {
            f32 x = cx - hx + 2.0f * hx * (f32)ix / (f32)(n - 1);
            f32 z = cz - hz + 2.0f * hz * (f32)iz / (f32)(n - 1);
            Vec3f probe = { x, probeY, z };
            CollisionPoly* poly = NULL;
            s32 bgId = BGCHECK_SCENE;
            f32 y = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
            if (poly == NULL || COLPOLY_GET_NORMAL(poly->normal.y) < 0.85f ||
                SurfaceType_GetSceneExitIndex(&play->colCtx, poly, bgId) != 0 ||
                SurfaceType_GetFloorType(&play->colCtx, poly, bgId) != 0) {
                return result;
            }
            f32 waterY = y;
            WaterBox* waterBox;
            if (WaterBox_GetSurface1(play, &play->colCtx, x, z, &waterY, &waterBox) && waterY > y) {
                return result;
            }
            if (first) {
                result.minY = result.maxY = y;
                first = false;
            } else {
                result.minY = std::min(result.minY, y);
                result.maxY = std::max(result.maxY, y);
            }
        }
    }
    if (detail) {
        // No walls, fences or rocks across the footprint
        f32 y = result.maxY + 30.0f;
        for (s32 i = 0; i < 5; i++) {
            f32 t = -1.0f + 0.5f * (f32)i;
            Vec3f a = { cx - hx, y, cz + t * hz };
            Vec3f b = { cx + hx, y, cz + t * hz };
            Vec3f c = { cx + t * kHalf, y, cz - hz };
            Vec3f d = { cx + t * kHalf, y, cz + hz };
            Vec3f hit;
            CollisionPoly* poly;
            s32 bgId;
            if (BgCheck_EntityLineTest1(&play->colCtx, &a, &b, &hit, &poly, true, false, false, false, &bgId) ||
                BgCheck_EntityLineTest1(&play->colCtx, &c, &d, &hit, &poly, true, false, false, false, &bgId)) {
                return result;
            }
        }
    }
    result.ok = true;
    return result;
}

static void SaveParkSpot() {
    CVarSetInteger(CVAR_SKATE("Park.Placed"), 1);
    CVarSetFloat(CVAR_SKATE("Park.X"), sPark.center.x);
    CVarSetFloat(CVAR_SKATE("Park.Z"), sPark.center.z);
    SaveCVars();
}

static bool LoadParkSpot() {
    if (!CVarGetInteger(CVAR_SKATE("Park.Placed"), 0)) {
        return false;
    }
    sPark.center.x = CVarGetFloat(CVAR_SKATE("Park.X"), 0.0f);
    sPark.center.z = CVarGetFloat(CVAR_SKATE("Park.Z"), 0.0f);
    return true;
}

static f32 GroundAt(PlayState* play, f32 x, f32 z, f32 probeY) {
    Vec3f probe = { x, probeY, z };
    CollisionPoly* poly = NULL;
    s32 bgId;
    f32 y = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
    return (poly != NULL) ? y : probeY - 2000.0f;
}

// Measures the terrain at the saved spot and spawns the park on it
static void SpawnPark(PlayState* play) {
    using namespace Park;
    f32 probeY = (f32)play->colCtx.maxBounds.y + 100.0f;
    f32 cx = sPark.center.x;
    f32 cz = sPark.center.z;
    f32 minY = 0.0f, maxY = 0.0f;
    bool first = true;
    for (s32 ix = 0; ix < 7; ix++) {
        for (s32 iz = 0; iz < 7; iz++) {
            f32 y = GroundAt(play, cx - kHalf + 2.0f * kHalf * (f32)ix / 6.0f,
                             cz - kHalf + 2.0f * kHalf * (f32)iz / 6.0f, probeY);
            if (first) {
                minY = maxY = y;
                first = false;
            } else {
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    sPark.center.y = maxY + 6.0f; // deck sits just above the highest bump
    sPark.groundMin = minY;
    sPark.rampFootY[0] = GroundAt(play, cx + kHalf + kRampLen, cz, probeY);
    sPark.rampFootY[1] = GroundAt(play, cx - kHalf - kRampLen, cz, probeY);

    BuildParkGeometry();
    Actor* actor =
        Actor_Spawn(&play->actorCtx, play, ParkActorId(), sPark.center.x, sPark.center.y, sPark.center.z, 0, 0, 0, 0);
    sPark.actor = (DynaPolyActor*)actor;
    sPark.placed = true;
    sPark.failed = (actor == NULL);
}

static void RemovePark(PlayState* play) {
    if (sPark.actor != nullptr && IsActorAlive(play, &sPark.actor->actor, ACTORCAT_BG)) {
        NameTag_RemoveAllForActor(&sPark.actor->actor);
        Actor_Kill(&sPark.actor->actor);
    }
}

// Looks for the flattest open spot in Hyrule Field, a few candidates per frame so the game doesn't stall
static void StartParkSearch(PlayState* play) {
    sPark.searching = true;
    sPark.found = false;
    sPark.searchIdx = 0;
    f32 margin = 1400.0f;
    f32 w = (f32)(play->colCtx.maxBounds.x - play->colCtx.minBounds.x) - 2.0f * margin;
    f32 d = (f32)(play->colCtx.maxBounds.z - play->colCtx.minBounds.z) - 2.0f * margin;
    sPark.searchCols = std::max(1, (s32)(w / 350.0f));
    s32 rows = std::max(1, (s32)(d / 350.0f));
    sPark.searchCount = sPark.searchCols * rows;
}

static void UpdateParkSearch(PlayState* play) {
    f32 margin = 1400.0f;
    f32 probeY = (f32)play->colCtx.maxBounds.y + 100.0f;
    f32 midX = ((f32)play->colCtx.minBounds.x + (f32)play->colCtx.maxBounds.x) * 0.5f;
    f32 midZ = ((f32)play->colCtx.minBounds.z + (f32)play->colCtx.maxBounds.z) * 0.5f;
    for (s32 n = 0; n < 12 && sPark.searchIdx < sPark.searchCount; n++, sPark.searchIdx++) {
        f32 x = (f32)play->colCtx.minBounds.x + margin + 350.0f * (f32)(sPark.searchIdx % sPark.searchCols);
        f32 z = (f32)play->colCtx.minBounds.z + margin + 350.0f * (f32)(sPark.searchIdx / sPark.searchCols);
        if (!SampleTerrain(play, x, z, probeY, false).ok) {
            continue;
        }
        TerrainSample s = SampleTerrain(play, x, z, probeY, true);
        if (!s.ok) {
            continue;
        }
        // flatter is better; among equally flat spots, prefer the middle of the field
        f32 score = (s.maxY - s.minY) + 0.01f * sqrtf(SQ(x - midX) + SQ(z - midZ));
        if (!sPark.found || score < sPark.bestScore) {
            sPark.found = true;
            sPark.bestScore = score;
            sPark.bestCenter = V3(x, 0.0f, z);
        }
    }
    if (sPark.searchIdx >= sPark.searchCount) {
        sPark.searching = false;
        if (sPark.found) {
            sPark.center = sPark.bestCenter;
            SaveParkSpot();
            SpawnPark(play);
            ShowMsg("A SKATE PARK APPEARED IN HYRULE FIELD!", MSG_GOOD, 80);
        }
    }
}

// ---- goals ----------------------------------------------------------------------------------------------------------

static bool InParkArea(Player* player, Vec3f* local) {
    if (sPark.actor == nullptr) {
        return false;
    }
    Vec3f l = Sub(player->actor.world.pos, sPark.center);
    if (local != nullptr) {
        *local = l;
    }
    return std::fabs(l.x) < Park::kHalf + Park::kRampLen && std::fabs(l.z) < Park::kHalf + 50.0f;
}

static void CompleteParkGoal(s32 bit, const char* name) {
    s32 done = ParkDone();
    if (done & bit) {
        return;
    }
    done |= bit;
    CVarSetInteger(CVAR_SKATE("Park.Done"), done);
    SaveCVars();
    ShowMsg(std::string("PARK GOAL: ") + name, MSG_GOOD, 70);
    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
    if (Cfg_QuestRupees()) {
        Rupees_ChangeBy(30);
    }
    if (done == PARK_ALL) {
        ShowMsg("HYRULE SKATE PARK MASTERED!", MSG_GOOD, 100);
        Sfx_PlaySfxCentered(NA_SE_SY_GET_ITEM);
        if (Cfg_QuestRupees()) {
            Rupees_ChangeBy(100);
        }
    }
}

static void UpdateParkTreasures(PlayState* play, Player* player) {
    Vec3f local;
    if (!InParkArea(player, &local)) {
        return;
    }
    f32 age = AgeScale();
    s32 mask = ParkTreasureMask();
    for (s32 i = 0; i < kParkTreasureCount; i++) {
        if (mask & (1 << i)) {
            continue;
        }
        const ParkTreasure& t = sParkTreasures[i];
        f32 dx = t.local.x - local.x;
        f32 dz = t.local.z - local.z;
        f32 dy = t.local.y - (local.y + 25.0f * age);
        if (SQ(dx) + SQ(dz) < SQ(45.0f * age) && std::fabs(dy) < 50.0f * age) {
            mask |= 1 << i;
            CVarSetInteger(CVAR_SKATE("Park.Treasures"), mask);
            SaveCVars();
            sSkate.combo.Add(t.name, 500);
            ShowMsg(std::string("GOT THE ") + t.name + "!", MSG_GOOD, 45);
            Sfx_PlaySfxCentered(NA_SE_SY_GET_ITEM);
            Vec3f world = V3(sPark.center.x + t.local.x, sPark.center.y + t.local.y, sPark.center.z + t.local.z);
            static Color_RGBA8 sPrim = { 255, 255, 200, 255 };
            static Color_RGBA8 sEnv = { 120, 200, 255, 255 };
            for (s32 k = 0; k < 8; k++) {
                Vec3f vel = { Rand_CenteredFloat(5.0f), 2.0f + Rand_ZeroOne() * 4.0f, Rand_CenteredFloat(5.0f) };
                Vec3f accel = { 0.0f, -0.3f, 0.0f };
                EffectSsKiraKira_SpawnSmall(play, &world, &vel, &accel, &sPrim, &sEnv);
            }
        }
    }
    if ((mask & 7) == 7) {
        CompleteParkGoal(PARK_STONES, "SPIRITUAL STONES");
    }
    if ((mask & 24) == 24) {
        CompleteParkGoal(PARK_VERT, "VERT TREASURES");
    }
}

static std::string ParkHudLine() {
    s32 mask = ParkTreasureMask();
    s32 done = ParkDone();
    s32 stones = (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1);
    s32 vert = ((mask >> 3) & 1) + ((mask >> 4) & 1);
    std::string text = "STONES " + std::to_string(stones) + "/3  VERT " + std::to_string(vert) + "/2";
    text += (done & PARK_TRIGAP) ? "  TRI-GAP OK" : "  TRI-GAP -";
    text += (done & PARK_COMBO) ? "  10K OK" : "  10K -";
    return text;
}

static std::string ParkDirectionHint(Player* player) {
    f32 dist = Math_Vec3f_DistXZ(&player->actor.world.pos, &sPark.center);
    s16 rel = (s16)(Math_Vec3f_Yaw(&player->actor.world.pos, &sPark.center) - sSkate.moveYaw);
    const char* dir = "AHEAD";
    if (ABS(rel) > 0x6000) {
        dir = "BEHIND";
    } else if (rel > 0x1800) {
        dir = "LEFT";
    } else if (rel < -0x1800) {
        dir = "RIGHT";
    }
    return std::string("SKATE PARK ") + dir + " " + std::to_string((s32)dist);
}

// True when Link's floor is part of the park (the park's ramps get skate-park physics)
static bool OnParkFloor(Player* player) {
    return sPark.actor != nullptr && sPark.bgId >= 0 && player->actor.floorBgId == sPark.bgId &&
           (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND);
}

// Called every air frame and on landing, for the Triforce gap
static void ParkAirTrack(Player* player) {
    Vec3f local;
    if (!InParkArea(player, &local)) {
        return;
    }
    Vec3f T, L, R, mTL, mTR, mLR;
    TriforcePoints(&T, &L, &R, &mTL, &mTR, &mLR);
    if (local.y > Park::kBoxHeight - 10.0f && PointInTri(local.x, local.z, mTL, mTR, mLR)) {
        sPark.overTriHole = true;
    }
}

static void ParkCheckTriforceGap(Player* player) {
    Vec3f local;
    if (!sPark.overTriHole || !InParkArea(player, &local)) {
        sPark.overTriHole = false;
        return;
    }
    sPark.overTriHole = false;
    Vec3f T, L, R, mTL, mTR, mLR;
    TriforcePoints(&T, &L, &R, &mTL, &mTR, &mLR);
    bool onPiece = std::fabs(local.y - Park::kBoxHeight) < 12.0f &&
                   (PointInTri(local.x, local.z, T, mTL, mTR) || PointInTri(local.x, local.z, mTL, L, mLR) ||
                    PointInTri(local.x, local.z, mTR, mLR, R));
    if (onPiece) {
        sSkate.combo.Add("TRIFORCE GAP", 1500);
        ShowMsg("TRIFORCE GAP!", MSG_GOOD, 45);
        Sfx_PlaySfxCentered(NA_SE_SY_TRE_BOX_APPEAR);
        CompleteParkGoal(PARK_TRIGAP, "TRIFORCE GAP");
    }
}

// Per frame, from OnPlayerUpdate
static void UpdatePark(PlayState* play, Player* player, bool riding) {
    bool wanted = Cfg_Park() && play->sceneNum == SCENE_HYRULE_FIELD;
    if (!wanted) {
        if (sPark.actor != nullptr) {
            RemovePark(play);
        }
        return;
    }
    if (sPark.respawn) {
        // Waiting for the old park to be cleaned up before building the new one
        if (sPark.actor == nullptr || !IsActorAlive(play, &sPark.actor->actor, ACTORCAT_BG)) {
            sPark.actor = nullptr;
            sPark.respawn = false;
            SpawnPark(play);
        }
        return;
    }
    if (sPark.actor == nullptr && !sPark.searching && !sPark.failed && sSkate.sceneFrames >= 2) {
        if (LoadParkSpot()) {
            SpawnPark(play);
        } else {
            StartParkSearch(play);
        }
    }
    if (sPark.searching) {
        UpdateParkSearch(play);
        return;
    }
    if (sPark.actor == nullptr) {
        return;
    }
    if (riding) {
        UpdateParkTreasures(play, player);
        if (!sPark.announced && InParkArea(player, nullptr)) {
            sPark.announced = true;
            if (ParkDone() != PARK_ALL) {
                ShowMsg("HYRULE SKATE PARK", MSG_INFO, 50);
            }
        }
    }
}

// The quarter pipes send you straight up when you reach the lip (they end in a flat platform, so without this
// you'd just roll onto it). You drift back over the ramp and land on it again.
static bool ParkAtLip(Player* player) {
    Vec3f local;
    if (!OnParkFloor(player) || !InParkArea(player, &local)) {
        return false;
    }
    if (std::fabs(local.x) > Park::kQpHalfWidth || std::fabs(local.z) < Park::kQpLip - 18.0f ||
        std::fabs(local.z) > Park::kQpLip) {
        return false;
    }
    f32 outward = Math_CosS(sSkate.moveYaw) * (local.z > 0.0f ? 1.0f : -1.0f);
    return outward > 0.5f && sSkate.speed > 1.0f;
}

// ---------------------------------------------------------------------------------------------------------------------
// Combo handling
// ---------------------------------------------------------------------------------------------------------------------

static void BankCombo() {
    if (sSkate.combo.Empty()) {
        return;
    }
    s32 total = sSkate.combo.base * sSkate.combo.Mult();
    sSkate.session += total;

    std::string text = "+" + std::to_string(total);
    if (total > CVarGetInteger(CVAR_SKATE("BestCombo"), 0)) {
        CVarSetInteger(CVAR_SKATE("BestCombo"), total);
        SaveCVars();
        text += "  NEW BEST!";
        Sfx_PlaySfxCentered(NA_SE_SY_GET_RUPY);
    } else if (total >= 1000) {
        Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
    }
    ShowMsg(text, MSG_GOOD, 50);

    if (gPlayState != nullptr && total >= 10000 && InParkArea(GET_PLAYER(gPlayState), nullptr)) {
        CompleteParkGoal(PARK_COMBO, "10,000 COMBO");
    }

    if (Cfg_Quests() && gPlayState != nullptr && IsQuestArea(gPlayState->sceneNum) && total >= Tune::kQuestComboGoal) {
        CompleteQuest(gPlayState->sceneNum, QUEST_COMBO, "5000 COMBO");
    }

    if (Cfg_RupeeReward() && total >= 1000) {
        Rupees_ChangeBy((s16)std::min(total / 1000, 50));
    }
    sSkate.combo.Clear();
}

// Adds the spin from this jump to the combo, either as a prefix ("360 KICKFLIP") or as its own trick.
static void FinalizeSpin() {
    s32 halfTurns = (std::abs(sSkate.spin) + 0x4000) / 0x8000;
    if (halfTurns <= 0) {
        return;
    }
    std::string degrees = std::to_string(halfTurns * 180);
    s32 points = halfTurns * 100;

    if ((s32)sSkate.combo.names.size() > sSkate.airTrickStart) {
        std::string& first = sSkate.combo.names[sSkate.airTrickStart];
        first = degrees + " " + first;
        sSkate.combo.base += points;
    } else {
        sSkate.combo.Add(degrees, points);
    }
}

static void FinishGrab() {
    if (sSkate.grabIdx > 0) {
        const GrabTrick& grab = sGrabTricks[sSkate.grabIdx];
        sSkate.combo.Add(grab.name, grab.points + std::min<s32>(sSkate.grabFrames, 60) * 15);
    }
    sSkate.grabIdx = -1;
    sSkate.grabFrames = 0;
}

static void FinishGrind() {
    const GrindTrick& grind = sGrindTricks[sSkate.grindIdx];
    sSkate.combo.Add(grind.name, grind.points + sSkate.grindFrames * 10);
    sSkate.grindFrames = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mount / dismount / bail
// ---------------------------------------------------------------------------------------------------------------------

static void StartAir(Player* player, f32 launchVelY);

// ---- Gaps -----------------------------------------------------------------------------------------------------------

// Called when a jump ends (landing, or locking onto a rail). Decides whether it cleared a gap.
static void CheckGap(PlayState* play, Player* player, bool toRail) {
    if (!sSkate.airTracking) {
        return;
    }
    sSkate.airTracking = false;

    f32 age = AgeScale();
    Vec3f land = player->actor.world.pos;
    f32 dist = Math_Vec3f_DistXZ(&sSkate.takeoffPos, &land);
    f32 low = std::min(sSkate.takeoffPos.y, land.y);
    f32 depth = low - sSkate.airLowestFloor;
    f32 rise = land.y - sSkate.takeoffPos.y;
    bool overWater = sSkate.airOverWater && !sSkate.onWater;

    const char* type = nullptr;
    if (sSkate.fromGrind && toRail && dist > 100.0f * age) {
        type = "RAIL TRANSFER";
    } else if (dist >= 180.0f * age && (depth >= 80.0f * age || overWater)) {
        if (toRail) {
            type = "GAP TO RAIL";
        } else if (overWater) {
            type = "WATER GAP";
        } else if (rise > 40.0f * age) {
            type = "STEP UP GAP";
        } else {
            type = "CANYON GAP";
        }
    } else if (rise < -250.0f * age) {
        type = "BIG DROP";
    }
    if (type == nullptr) {
        return;
    }

    s32 points = 250 + (s32)(dist * 0.5f) + (s32)std::max(0.0f, depth * 0.5f);
    if (Cfg_Quests() && IsQuestArea(play->sceneNum) && RegisterGap(play->sceneNum, sSkate.takeoffPos, land)) {
        points *= 2;
        ShowMsg(std::string("NEW GAP! ") + type, MSG_GOOD, 45);
        Sfx_PlaySfxCentered(NA_SE_SY_TRE_BOX_APPEAR);
        if ((s32)LoadGaps(play->sceneNum).size() >= Tune::kQuestGapGoal) {
            CompleteQuest(play->sceneNum, QUEST_GAPS, "3 GAPS");
        }
    }
    sSkate.combo.Add(type, points);
}

// ---- Epona tow ------------------------------------------------------------------------------------------------------

// Adult Epona, standing around (called with Epona's Song), close enough to grab
static EnHorse* FindEpona(PlayState* play, Player* player) {
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_BG].head; actor != NULL; actor = actor->next) {
        if (actor->id != ACTOR_EN_HORSE || actor->update == NULL) {
            continue;
        }
        EnHorse* horse = (EnHorse*)actor;
        if (horse->type != HORSE_EPONA || (horse->stateFlags & ENHORSE_INACTIVE)) {
            continue;
        }
        if (horse->action != ENHORSE_ACT_IDLE && horse->action != ENHORSE_ACT_FOLLOW_PLAYER) {
            continue;
        }
        if (Actor_WorldDistXZToActor(&player->actor, actor) < Tune::kTowGrabRange) {
            return horse;
        }
    }
    return nullptr;
}

static void SetTowHeading(EnHorse* horse, s16 yaw) {
    // Epona's "run away" behaviour gallops toward her home point, so we keep moving that point ahead of her
    horse->actor.home.pos.x = horse->actor.world.pos.x + Math_SinS(yaw) * 1000.0f;
    horse->actor.home.pos.y = horse->actor.world.pos.y;
    horse->actor.home.pos.z = horse->actor.world.pos.z + Math_CosS(yaw) * 1000.0f;
}

static void StartTow(PlayState* play, Player* player, EnHorse* horse) {
    sSkate.towHorse = horse;
    sSkate.towFrames = 0;
    sSkate.pushTimer = 0;
    EnHorse_InitFleePlayer(horse);
    horse->actor.world.rot.y = horse->actor.shape.rot.y = sSkate.moveYaw;
    SetTowHeading(horse, sSkate.moveYaw);
    Audio_PlaySfxGeneral(NA_SE_EV_HORSE_NEIGH, &horse->actor.projectedPos, 4, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    ShowMsg("HANG ON!", MSG_INFO, 30);
    sSkate.anim = ANIM_NONE;
    SetAnim(play, player, ANIM_TOW);
}

static void EndTow(PlayState* play, bool slingshot) {
    EnHorse* horse = sSkate.towHorse;
    sSkate.towHorse = nullptr;
    sSkate.anim = ANIM_NONE;
    if (horse != nullptr && IsActorAlive(play, &horse->actor, ACTORCAT_BG)) {
        horse->actor.home.pos = horse->actor.world.pos;
        EnHorse_StartIdleRidable(horse);
    }
    if (slingshot && sSkate.towFrames > 15) {
        sSkate.speed = std::min(sSkate.speed + Tune::kTowSlingshot, Tune::kMaxSpeed * 1.3f);
        sSkate.combo.Add("EPONA TOW", 100 + std::min(sSkate.towFrames, 200) * 5);
        ShowMsg("SLINGSHOT!", MSG_GOOD, 30);
    }
    sSkate.towFrames = 0;
}

// Returns true while towing (Epona sets the speed and direction instead of the player)
static bool UpdateTow(PlayState* play, Player* player, Input* input) {
    EnHorse* horse = sSkate.towHorse;
    if (horse == nullptr) {
        return false;
    }
    if (!IsActorAlive(play, &horse->actor, ACTORCAT_BG) || horse->action != ENHORSE_ACT_FLEE_PLAYER) {
        sSkate.towHorse = nullptr;
        sSkate.anim = ANIM_NONE;
        return false;
    }
    if (!CHECK_BTN_ALL(input->cur.button, BTN_R)) {
        EndTow(play, true);
        return false;
    }

    // Steer Epona with the stick
    f32 sx = StickX(input);
    SetTowHeading(horse, (s16)(horse->actor.world.rot.y - (s16)(sx * 0x2800)));

    // Hang on behind her
    Vec3f target = horse->actor.world.pos;
    target.x -= Math_SinS(horse->actor.shape.rot.y) * Tune::kTowDistance;
    target.z -= Math_CosS(horse->actor.shape.rot.y) * Tune::kTowDistance;
    f32 dist = Math_Vec3f_DistXZ(&player->actor.world.pos, &target);
    if (dist > Tune::kTowLoseGrip) {
        EndTow(play, false);
        ShowMsg("LOST GRIP", MSG_BAD, 30);
        return false;
    }
    if (dist > 2.0f) {
        Math_ScaledStepToS(&sSkate.moveYaw, Math_Vec3f_Yaw(&player->actor.world.pos, &target), 0x1800);
    }
    sSkate.speed = std::clamp(horse->actor.speedXZ + dist * 0.3f, 0.0f, 18.0f);
    sSkate.towFrames++;

    if (sSkate.phase == PHASE_GROUND) {
        SetAnim(play, player, ANIM_TOW);
        if (Cfg_RollSound() && sSkate.speed > 1.5f && !sSkate.onWater) {
            PlayLoopSfx(player, NA_SE_PL_SLIP_LEVEL, 0.7f + sSkate.speed * 0.03f,
                        std::min(0.25f + sSkate.speed * 0.04f, 0.8f));
        }
    }
    return true;
}

// ---- Bomb hop -------------------------------------------------------------------------------------------------------

static s32 BombButtonMask() {
    static const u16 sButtons[] = {
        BTN_B, BTN_CLEFT, BTN_CDOWN, BTN_CRIGHT, BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT
    };
    for (s32 i = 1; i < 8; i++) {
        if (gSaveContext.equips.buttonItems[i] == ITEM_BOMB) {
            return sButtons[i];
        }
    }
    return BTN_Z; // bombs aren't on a button: use Z
}

static void DropBomb(Player* player) {
    if (sSkate.bombActive) {
        return;
    }
    if (!Cfg_FreeBombs()) {
        if (INV_CONTENT(ITEM_BOMB) != ITEM_BOMB || AMMO(ITEM_BOMB) <= 0) {
            ShowMsg("NO BOMBS", MSG_BAD, 25);
            Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
            return;
        }
        Inventory_ChangeAmmo(ITEM_BOMB, -1);
    }
    sSkate.bombActive = true;
    sSkate.bombTimer = Tune::kBombFuse;
    sSkate.bombPos = player->actor.world.pos;
    sSkate.bombPos.x -= Math_SinS(sSkate.moveYaw) * 8.0f;
    sSkate.bombPos.z -= Math_CosS(sSkate.moveYaw) * 8.0f;
    sSkate.bombVelY = (sSkate.phase == PHASE_AIR) ? std::min(0.0f, player->actor.velocity.y) - 2.0f : 0.0f;
    PlaySfx(player, NA_SE_IT_BOMB_IGNIT);
}

static void ExitGrindToAir(Player* player, f32 hop);

static void ExplodeBomb(PlayState* play, Player* player, bool riding) {
    sSkate.bombActive = false;

    Vec3f zero = { 0.0f, 0.0f, 0.0f };
    Vec3f effPos = sSkate.bombPos;
    effPos.y += 10.0f;
    EffectSsBomb2_SpawnLayered(play, &effPos, &zero, &zero, 100, 19);
    Vec3f groundPos = sSkate.bombPos;
    EffectSsBlast_SpawnWhiteShockwave(play, &groundPos, &zero, &zero);
    PlaySfx(player, NA_SE_IT_BOMB_EXPLOSION);

    f32 dist = Math_Vec3f_DistXYZ(&player->actor.world.pos, &sSkate.bombPos);
    Rumble_Request(dist, 0xFF, 0x14, 0x96);
    s16 quake = Quake_Add(GET_ACTIVE_CAM(play), 3);
    Quake_SetSpeed(quake, 25000);
    Quake_SetQuakeValues(quake, 3, 0, 0, 0);
    Quake_SetCountdown(quake, 6);

    f32 radius = Tune::kBombRadius * AgeScale();
    if (!riding || dist > radius) {
        return;
    }
    // The closer you are, the bigger the blast-off
    f32 strength = 1.0f - 0.5f * (dist / radius);
    f32 launch = Tune::kBombLaunchY * strength * sqrtf(AgeScale());
    if (sSkate.phase == PHASE_GRIND) {
        ExitGrindToAir(player, launch);
    } else if (sSkate.phase == PHASE_GROUND) {
        StartAir(player, launch);
    } else {
        player->actor.velocity.y = std::max(player->actor.velocity.y, launch);
    }
    sSkate.speed = std::min(sSkate.speed + Tune::kBombBoost * strength, Tune::kMaxSpeed * 1.3f);
    sSkate.combo.Add("BOMB HOP", 300);
    sSkate.anim = ANIM_NONE;
    SetAnim(play, player, ANIM_AIR_UP);
}

static void UpdateBomb(PlayState* play, Player* player, bool riding) {
    if (!sSkate.bombActive) {
        return;
    }
    // Fall until it hits the floor (or bobs on water)
    sSkate.bombVelY = std::max(sSkate.bombVelY - 1.5f, -20.0f);
    sSkate.bombPos.y += sSkate.bombVelY;
    f32 floorY = FloorAt(play, sSkate.bombPos.x, sSkate.bombPos.y + 30.0f, sSkate.bombPos.z);
    f32 waterY = sSkate.bombPos.y + 30.0f;
    WaterBox* waterBox;
    if (WaterBox_GetSurface1(play, &play->colCtx, sSkate.bombPos.x, sSkate.bombPos.z, &waterY, &waterBox) &&
        waterY > floorY) {
        floorY = waterY;
    }
    if (floorY > BGCHECK_Y_MIN && sSkate.bombPos.y < floorY) {
        sSkate.bombPos.y = floorY;
        sSkate.bombVelY = 0.0f;
    }

    // Fuse sparks
    static Color_RGBA8 sPrim = { 255, 255, 150, 255 };
    static Color_RGBA8 sEnv = { 255, 100, 0, 255 };
    Vec3f spark = sSkate.bombPos;
    spark.y += 14.0f;
    Vec3f vel = { Rand_CenteredFloat(1.5f), 1.0f, Rand_CenteredFloat(1.5f) };
    Vec3f accel = { 0.0f, -0.2f, 0.0f };
    EffectSsKiraKira_SpawnSmall(play, &spark, &vel, &accel, &sPrim, &sEnv);

    if (--sSkate.bombTimer <= 0) {
        ExplodeBomb(play, player, riding);
    }
}

static void ResetVisuals(Player* player) {
    player->actor.shape.yOffset = 0.0f;
    player->actor.shape.rot.x = 0;
    player->actor.shape.rot.z = 0;
    player->headLimbRot.y = 0;
    player->upperLimbRot.y = 0;
}

static void ClearRideState() {
    sSkate.active = false;
    sSkate.bailPending = false;
    sSkate.charging = false;
    sSkate.chargeFrames = 0;
    sSkate.flipIdx = -1;
    sSkate.flipTimer = 0;
    sSkate.grabIdx = -1;
    sSkate.grabFrames = 0;
    sSkate.spin = 0;
    sSkate.anim = ANIM_NONE;
    sSkate.pushTimer = 0;
    sSkate.pushBlend = 0.0f;
    if (sSkate.towHorse != nullptr && gPlayState != nullptr) {
        EndTow(gPlayState, false);
    }
    sSkate.towHorse = nullptr;
    sSkate.airTracking = false;
}

static void Dismount(PlayState* play, Player* player) {
    BankCombo();
    ResetVisuals(player);
    player->actor.shape.rot.y = player->yaw = sSkate.moveYaw;
    ClearRideState();
    sSkate.toggleCooldown = 10;
    func_80853080(player, play); // back to standing idle
}

static void Bail(PlayState* play, Player* player, const char* reason) {
    sSkate.combo.Clear();
    ShowMsg(reason, MSG_BAD, 45);
    ResetVisuals(player);
    player->actor.shape.rot.y = sSkate.moveYaw;
    PlaySfx(player, NA_SE_PL_BODY_HIT);
    Player_PlayVoiceSfx(player, NA_SE_VO_LI_DAMAGE_S);
    // Knock Link down. Next frame the game swaps in its own knockback action, which ends the ride.
    Actor_SetPlayerKnockbackLarge(play, &player->actor, 4.0f + sSkate.speed * 0.3f, sSkate.moveYaw, 6.0f,
                                  Cfg_BailsHurt() ? 4 : 0);
    sSkate.bailPending = true;
    sSkate.speed = 0.0f;
}

static void Skate_AfterPutAway(PlayState* play, Player* player) {
    sSkate.mountPending = false;
    Player_SetupAction(play, player, Skate_Action, 0);

    sSkate.active = true;
    sSkate.bailPending = false;
    sSkate.phase = PHASE_GROUND;
    sSkate.anim = ANIM_NONE;
    sSkate.moveYaw = player->actor.shape.rot.y;
    sSkate.speed = std::max(0.0f, player->linearVelocity);
    sSkate.fakie = false;
    sSkate.boardYawBase = 0;
    sSkate.spin = 0;
    sSkate.flipIdx = -1;
    sSkate.grabIdx = -1;
    sSkate.noGrindTimer = 0;
    if (sSkate.carryActive) {
        // Rolled in from the previous area: keep the speed, the combo and the session score
        sSkate.speed = sSkate.carrySpeed;
        sSkate.carryPending = sSkate.carryActive = false;
    } else {
        sSkate.carryPending = false;
        sSkate.combo.Clear();
        sSkate.session = 0;
        if (Cfg_Quests() && IsQuestArea(play->sceneNum) && QuestDone(play->sceneNum) != QUEST_ALL && !sGoalsShown) {
            sGoalsShown = true;
            ShowMsg("GOALS: SPELL ZELDA, 3 GAPS, 5000 COMBO", MSG_INFO, 70);
        }
    }
    player->stateFlags3 |= PLAYER_STATE3_MIDAIR;

    SetAnim(play, player, ANIM_RIDE);
    PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
}

static bool IsMountableAction(Player* player) {
    PlayerActionFunc f = player->actionFunc;
    return f == Player_Action_Idle || f == Player_Action_80842180 || f == Player_Action_80840450 ||
           f == Player_Action_808407CC || f == Player_Action_80840DE4 || f == Player_Action_808414F8 ||
           f == Player_Action_8084170C || f == Player_Action_808417FC || f == Player_Action_8084193C;
}

static bool CanMount(PlayState* play, Player* player) {
    const u32 blocked1 = PLAYER_STATE1_LOADING | PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD |
                         PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_CARRYING_ACTOR | PLAYER_STATE1_HANGING_OFF_LEDGE |
                         PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_FIRST_PERSON | PLAYER_STATE1_CLIMBING_LADDER |
                         PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_USING_BOOMERANG | PLAYER_STATE1_DAMAGED |
                         PLAYER_STATE1_IN_WATER | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE;

    return (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && !(player->stateFlags1 & blocked1) &&
           !(player->stateFlags2 & PLAYER_STATE2_CRAWLING) && player->csAction == 0 && player->actor.parent == NULL &&
           play->csCtx.state == CS_STATE_IDLE && play->transitionTrigger == TRANS_TRIGGER_OFF &&
           play->msgCtx.msgMode == MSGMODE_NONE && IsMountableAction(player);
}

static void TryMount(PlayState* play, Player* player) {
    sSkate.mountPending = true;
    Player_SetupWaitForPutAway(play, player, Skate_AfterPutAway);
}

// ---------------------------------------------------------------------------------------------------------------------
// The riding action, one call per game frame
// ---------------------------------------------------------------------------------------------------------------------

static void StartAir(Player* player, f32 launchVelY) {
    sSkate.phase = PHASE_AIR;
    sSkate.pushTimer = 0;
    sSkate.spin = 0;
    sSkate.airTrickStart = (s32)sSkate.combo.names.size();
    sSkate.flipIdx = -1;
    sSkate.flipTimer = 0;
    sSkate.grabIdx = -1;
    sSkate.grabFrames = 0;
    sSkate.wallJumps = 0;
    sSkate.aBuffer = 0;
    if (launchVelY > player->actor.velocity.y) {
        player->actor.velocity.y = launchVelY;
    }
    sPark.overTriHole = false;

    // Gap tracking: remember where we took off and watch what we fly over
    sSkate.airTracking = true;
    sSkate.fromGrind = false;
    sSkate.airOverWater = false;
    sSkate.takeoffPos = player->actor.world.pos;
    sSkate.airLowestFloor = player->actor.world.pos.y;
}

static void StartGrind(PlayState* play, Player* player, Input* input, const Vec3f& pos, s16 yaw) {
    if (sSkate.phase == PHASE_AIR) {
        FinishGrab();
        FinalizeSpin();
        CheckGap(play, player, true);
    }
    sSkate.airTracking = false;
    sSkate.phase = PHASE_GRIND;
    sSkate.pushTimer = 0;
    sSkate.grindIdx = GetStickDir(input);
    sSkate.grindFrames = 0;
    sSkate.grindYaw = yaw;
    sSkate.moveYaw = yaw;
    sSkate.spin = 0;
    sSkate.flipIdx = -1;
    sSkate.flipTimer = 0;
    sSkate.balance = Rand_CenteredFloat(0.1f);
    sSkate.speed = std::max(sSkate.speed, 4.0f);

    player->actor.world.pos.x = pos.x;
    player->actor.world.pos.z = pos.z;
    player->actor.world.pos.y = pos.y;
    player->actor.velocity.y = 0.0f;

    PlaySfx(player, NA_SE_IT_SHIELD_REFLECT_SW);
    SetAnim(play, player, ANIM_GRIND);
}

static void ExitGrindToAir(Player* player, f32 hop) {
    FinishGrind();
    StartAir(player, hop);
    sSkate.fromGrind = true;
    sSkate.noGrindTimer = 8;
}

static void UpdateGround(PlayState* play, Player* player, Input* input) {
    f32 sx = StickX(input);
    f32 sy = StickY(input);
    f32 ageMul = AgeScale();
    f32 speedMult = Cfg_SpeedMult();

    // Slope: the horizontal part of the floor normal points downhill
    f32 slopeAlong = 0.0f;
    f32 rise = 0.0f;
    if (player->actor.floorPoly != NULL && (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND)) {
        f32 nx = COLPOLY_GET_NORMAL(player->actor.floorPoly->normal.x);
        f32 ny = COLPOLY_GET_NORMAL(player->actor.floorPoly->normal.y);
        f32 nz = COLPOLY_GET_NORMAL(player->actor.floorPoly->normal.z);
        slopeAlong = nx * Math_SinS(sSkate.moveYaw) + nz * Math_CosS(sSkate.moveYaw);
        if (ny > 0.2f) {
            rise = -slopeAlong / ny;
        }
    }
    if (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) {
        sSkate.lastRise = rise;
    }
    if (sSkate.onWater) {
        sSkate.lastRise = 0.0f;
        Math_StepToF(&sSkate.speed, 0.0f, 0.02f); // water drags a little more than pavement
    }
    sSkate.onPark = OnParkFloor(player);
    // Skate park ramps keep more of your momentum going up, so you can pump the half pipe
    f32 slopeAccel = (sSkate.onPark && slopeAlong < 0.0f) ? 0.35f : Tune::kSlopeAccel;
    sSkate.speed += slopeAlong * slopeAccel;

    // Stalled on a slope facing uphill: roll back down backwards (fakie)
    if (sSkate.speed < 0.5f && slopeAlong < -0.08f && !sSkate.charging) {
        sSkate.moveYaw = (s16)(sSkate.moveYaw + 0x8000);
        sSkate.fakie = !sSkate.fakie;
        sSkate.speed = std::fabs(sSkate.speed) + 0.5f;
    }

    // Quarter pipe lip: launch straight up
    if (ParkAtLip(player)) {
        f32 vy = std::max(sSkate.speed, 3.0f) * 1.4f;
        StartAir(player, vy);
        sSkate.speed = -1.0f; // drift back over the ramp so you land on it
        sSkate.charging = false;
        sSkate.chargeFrames = 0;
        PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
        sSkate.anim = ANIM_NONE;
        SetAnim(play, player, ANIM_AIR_UP);
        return;
    }

    // Steering: tighter at low speed
    s16 turn = (s16)(-sx * Tune::kTurnRate * (1.2f - std::min(sSkate.speed, 12.0f) / 24.0f));
    sSkate.moveYaw += turn;

    if (sy > 0.3f && sSkate.speed < Tune::kCruiseMax * speedMult) {
        sSkate.speed += Tune::kCruiseAccel * sy * speedMult;
    } else if (sy < -0.3f) {
        Math_StepToF(&sSkate.speed, 0.0f, Tune::kBrake * -sy);
    }
    Math_StepToF(&sSkate.speed, 0.0f, Tune::kFriction + std::fabs(sx) * 0.02f);

    // Push: tap B for one stroke, hold B to keep pushing
    bool wantPush = CHECK_BTN_ALL(input->press.button, BTN_B) || CHECK_BTN_ALL(input->cur.button, BTN_B);
    if (wantPush && sSkate.pushCooldown == 0 && !sSkate.charging && !CHECK_BTN_ALL(input->cur.button, BTN_A)) {
        sSkate.pushTimer = Tune::kPushFrames;
        sSkate.pushCooldown = Tune::kPushFrames;
        sSkate.anim = ANIM_NONE; // restart the stroke even if we were already pushing
        SetAnim(play, player, ANIM_PUSH);
    }
    if (sSkate.pushTimer > 0) {
        s16 stroke = Tune::kPushFrames - sSkate.pushTimer; // 0, 1, 2, ...

        // The foot is on the ground: add the speed a bit at a time
        if (stroke >= Tune::kPushContactStart && stroke < Tune::kPushContactEnd) {
            f32 perFrame = Tune::kPushImpulse * speedMult / (f32)(Tune::kPushContactEnd - Tune::kPushContactStart);
            if (sSkate.speed < Tune::kPushMax * speedMult) {
                sSkate.speed = std::min(sSkate.speed + perFrame, Tune::kPushMax * speedMult);
            }
        }

        // Foot hits the ground: scuff sound and a puff of dust (or a splash on water)
        if (stroke == Tune::kPushContactStart) {
            Vec3f footPos = player->actor.world.pos;
            footPos.x -= Math_SinS(sSkate.moveYaw) * 8.0f * AgeScale();
            footPos.z -= Math_CosS(sSkate.moveYaw) * 8.0f * AgeScale();
            if (sSkate.onWater) {
                PlaySfx(player, NA_SE_PL_WALK_WATER1);
                EffectSsGRipple_Spawn(play, &footPos, 60, 250, 0);
            } else {
                PlaySfx(player, (u16)(NA_SE_PL_WALK_GROUND + player->floorSfxOffset));
                Actor_SpawnFloorDustRing(play, &player->actor, &footPos, 4.0f, 2, 2.0f, 80, 12, false);
            }
        }
        sSkate.pushTimer--;
    }

    // Crouch + ollie
    if (CHECK_BTN_ALL(input->cur.button, BTN_A)) {
        sSkate.charging = true;
        sSkate.chargeFrames = std::min<s16>(sSkate.chargeFrames + 1, Tune::kOllieChargeFrames);
        SetAnim(play, player, ANIM_CROUCH);
    } else if (sSkate.charging) {
        f32 charge = (f32)sSkate.chargeFrames / Tune::kOllieChargeFrames;
        f32 vy = (Tune::kOllieBase + Tune::kOllieCharge * charge) * sqrtf(ageMul);
        sSkate.charging = false;
        sSkate.chargeFrames = 0;
        sSkate.ollied = true;
        StartAir(player, vy + std::max(0.0f, rise) * sSkate.speed * Tune::kRampLaunch);
        PlaySfx(player, sSkate.onWater ? NA_SE_PL_JUMP_WATER1 : NA_SE_IT_SHIELD_BOUND);
        SetAnim(play, player, ANIM_AIR_UP);
        return;
    }

    // Grab Epona: press R next to her (takes priority over grinding)
    if (CHECK_BTN_ALL(input->press.button, BTN_R) && sSkate.towHorse == nullptr) {
        EnHorse* horse = FindEpona(play, player);
        if (horse != nullptr) {
            StartTow(play, player, horse);
            return;
        }
    }

    // Grind from the ground: press R while rolling along a ledge
    if (CHECK_BTN_ALL(input->press.button, BTN_R) && sSkate.speed > Tune::kGrindMinSpeed) {
        Vec3f pos;
        s16 yaw;
        if (FindGrindLine(play, player, sSkate.moveYaw, &pos, &yaw)) {
            StartGrind(play, player, input, pos, yaw);
            player->actor.velocity.y = 3.0f;
            return;
        }
    }

    // Walls: head-on hits stop you, hard ones knock you off
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_WALL) && sSkate.impactSpeed > 3.0f) {
        s16 diff = player->actor.wallYaw - (s16)(sSkate.moveYaw + 0x8000);
        if (ABS(diff) < 0x2800) {
            if (sSkate.impactSpeed > Tune::kWallBailSpeed) {
                Bail(play, player, "SLAMMED!");
                return;
            }
            sSkate.speed = 0.0f;
            PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
        }
    }

    sSkate.speed = std::clamp(sSkate.speed, 0.0f, Tune::kMaxSpeed * speedMult);

    if (sSkate.charging) {
        sSkate.pushTimer = 0;
    } else if (sSkate.pushTimer > 0) {
        // keep the push stroke playing
    } else {
        if (sSkate.anim == ANIM_LAND) {
            if (player->skelAnime.curFrame >= player->skelAnime.endFrame) {
                SetAnim(play, player, ANIM_RIDE);
            }
        } else {
            SetAnim(play, player, ANIM_RIDE);
        }
    }

    if (sSkate.onWater) {
        // Wake behind the board
        if (sSkate.speed > 1.0f && (play->gameplayFrames % 3) == 0) {
            Vec3f wake = player->actor.world.pos;
            wake.x -= Math_SinS(sSkate.moveYaw) * 10.0f;
            wake.z -= Math_CosS(sSkate.moveYaw) * 10.0f;
            EffectSsGRipple_Spawn(play, &wake, 80, (s16)(250 + sSkate.speed * 20.0f), 0);
        }
        if (Cfg_RollSound() && sSkate.speed > 1.0f) {
            PlayLoopSfx(player, NA_SE_PL_SLIP_WATER1_LEVEL, 0.8f + sSkate.speed * 0.03f,
                        std::min(0.3f + sSkate.speed * 0.05f, 0.9f));
        }
    } else if (Cfg_RollSound() && sSkate.speed > 1.5f) {
        PlayLoopSfx(player, NA_SE_PL_SLIP_LEVEL, 0.7f + sSkate.speed * 0.03f,
                    std::min(0.25f + sSkate.speed * 0.04f, 0.8f));
    }
}

static void Land(PlayState* play, Player* player) {
    // A board still spinning under you, or a hand still on it, means you eat it.
    if (sSkate.flipIdx >= 0 && sSkate.flipTimer > 2) {
        Bail(play, player, "BAIL! STILL FLIPPING");
        return;
    }
    if (sSkate.grabIdx > 0 && sSkate.grabFrames > 0) {
        Bail(play, player, "BAIL! LET GO OF THE GRAB");
        return;
    }

    s32 absSpin = std::abs(sSkate.spin);
    s32 halfTurns = (absSpin + 0x4000) / 0x8000;
    s32 residual = absSpin - halfTurns * 0x8000;
    if (std::abs(residual) > Tune::kLandTolerance) {
        Bail(play, player, "BAIL! OVER-ROTATED");
        return;
    }
    if (halfTurns & 1) {
        sSkate.fakie = !sSkate.fakie;
    }
    if (sSkate.flipIdx >= 0) {
        // Close enough: count the last couple of frames of the flip as caught
        sSkate.boardYawBase += sFlipTricks[sSkate.flipIdx].yaw;
    }

    FinalizeSpin();
    CheckGap(play, player, false);
    ParkCheckTriforceGap(player);
    BankCombo();

    sSkate.phase = PHASE_GROUND;
    sSkate.spin = 0;
    sSkate.flipIdx = -1;
    sSkate.flipTimer = 0;
    sSkate.boardRoll = sSkate.boardPitch = 0;
    sSkate.ollied = false;
    sSkate.landTimer = 4;
    // A little speed is lost on impact; bigger drops cost more.
    sSkate.speed *= std::clamp(1.0f + player->actor.velocity.y * 0.01f, 0.8f, 1.0f);
    if (sSkate.onWater) {
        PlaySfx(player, NA_SE_PL_LAND_WATER1);
        SpawnSplash(play, player, (s16)(150 * AgeScale()));
    } else {
        PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
    }
    sSkate.anim = ANIM_NONE;
    SetAnim(play, player, ANIM_LAND);
}

static void UpdateAir(PlayState* play, Player* player, Input* input) {
    f32 sx = StickX(input);
    StickDir dir = GetStickDir(input);

    ParkAirTrack(player);

    // Gap tracking: the deepest floor and any water we pass over
    if (sSkate.airTracking) {
        f32 floorY = player->actor.floorHeight;
        if (floorY <= BGCHECK_Y_MIN + 1.0f) {
            floorY = player->actor.world.pos.y - 2000.0f; // nothing below at all
        }
        sSkate.airLowestFloor = std::min(sSkate.airLowestFloor, floorY);
        f32 waterY;
        if (GetWaterSurface(play, player, &waterY) && waterY < player->actor.world.pos.y) {
            sSkate.airOverWater = true;
        }
    }

    // Wall jump: press A as you hit a wall (a few frames early is fine)
    if (CHECK_BTN_ALL(input->press.button, BTN_A)) {
        sSkate.aBuffer = Tune::kWallJumpBuffer;
    } else if (sSkate.aBuffer > 0) {
        sSkate.aBuffer--;
    }
    if (Cfg_WallJumps() && sSkate.aBuffer > 0 && sSkate.wallJumps < Tune::kWallJumpMax &&
        (player->actor.bgCheckFlags & BGCHECKFLAG_WALL)) {
        s16 into = (s16)(player->actor.wallYaw - (s16)(sSkate.moveYaw + 0x8000));
        if (ABS(into) < 0x3800) {
            // Bounce off: mirror the direction of travel around the wall
            sSkate.moveYaw = (s16)(2 * player->actor.wallYaw - sSkate.moveYaw + 0x8000);
            sSkate.speed = std::max(sSkate.impactSpeed * 0.9f, 5.0f);
            player->actor.velocity.y = Tune::kWallJumpVelY * sqrtf(AgeScale());
            sSkate.wallJumps++;
            sSkate.aBuffer = 0;
            sSkate.noGrindTimer = 4;
            sSkate.combo.Add("WALLPLANT", 250);
            PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
            Player_PlayVoiceSfx(player, NA_SE_VO_LI_AUTO_JUMP);
            Vec3f wallPos = player->actor.world.pos;
            wallPos.y += 10.0f;
            Actor_SpawnFloorDustRing(play, &player->actor, &wallPos, 6.0f, 3, 3.0f, 100, 15, false);
            sSkate.anim = ANIM_NONE;
            SetAnim(play, player, ANIM_AIR_UP);
        }
    }

    // Spin with the stick
    sSkate.spin += (s32)(-sx * Tune::kSpinRate);

    // Flip tricks
    if (sSkate.flipTimer > 0) {
        sSkate.flipTimer--;
        if (sSkate.flipTimer == 0) {
            const FlipTrick& flip = sFlipTricks[sSkate.flipIdx];
            sSkate.boardYawBase += flip.yaw;
            sSkate.flipIdx = -1;
        }
    } else if (CHECK_BTN_ALL(input->press.button, BTN_B) && sSkate.grabIdx < 0) {
        const FlipTrick& flip = sFlipTricks[dir];
        sSkate.flipIdx = dir;
        sSkate.flipTimer = flip.frames;
        sSkate.combo.Add(flip.name, flip.points);
        PlaySfx(player, NA_SE_IT_SWORD_SWING);
    }

    // Grabs (R + direction). R with a neutral stick is "grind mode"
    if (CHECK_BTN_ALL(input->cur.button, BTN_R)) {
        if (sSkate.grabIdx < 0 && sSkate.flipTimer == 0 && dir != DIR_NONE &&
            CHECK_BTN_ALL(input->press.button, BTN_R)) {
            sSkate.grabIdx = dir;
            sSkate.grabFrames = 0;
        } else if (sSkate.grabIdx > 0) {
            sSkate.grabFrames++;
        }
    } else if (sSkate.grabIdx > 0) {
        FinishGrab();
    }

    // Grinds: holding R while coming down onto a ledge locks you onto it
    if (CHECK_BTN_ALL(input->cur.button, BTN_R) && player->actor.velocity.y <= 2.0f && sSkate.noGrindTimer == 0 &&
        sSkate.speed > Tune::kGrindMinSpeed - 1.0f) {
        Vec3f pos;
        s16 yaw;
        if (FindGrindLine(play, player, sSkate.moveYaw, &pos, &yaw)) {
            // A board mid-flip can't lock on, but we're forgiving: finish the flip instantly
            if (sSkate.flipIdx >= 0) {
                sSkate.boardYawBase += sFlipTricks[sSkate.flipIdx].yaw;
                sSkate.flipIdx = -1;
                sSkate.flipTimer = 0;
            }
            // Over-rotation snaps to the nearest half turn when locking onto a grind
            s32 halfTurns = (std::abs(sSkate.spin) + 0x4000) / 0x8000;
            if (halfTurns & 1) {
                sSkate.fakie = !sSkate.fakie;
            }
            StartGrind(play, player, input, pos, yaw);
            return;
        }
    }

    // Small amount of air drag
    Math_StepToF(&sSkate.speed, 0.0f, 0.01f);

    if (sSkate.grabIdx > 0) {
        SetAnim(play, player, ANIM_TUCK);
    } else if (player->actor.velocity.y > 0.0f) {
        SetAnim(play, player, ANIM_AIR_UP);
    } else {
        SetAnim(play, player, ANIM_AIR_DOWN);
    }
}

static void UpdateGrind(PlayState* play, Player* player, Input* input) {
    f32 s = AgeScale();
    f32 sx = StickX(input);
    Vec3f pos = player->actor.world.pos;

    sSkate.grindFrames++;

    // Balance: the longer you grind the harder it gets
    f32 difficulty = 1.0f + sSkate.grindFrames / 90.0f;
    sSkate.balance += (sSkate.balance * 0.07f + Rand_CenteredFloat(0.02f)) * difficulty;
    sSkate.balance -= sx * 0.07f;
    if (std::fabs(sSkate.balance) >= 1.0f) {
        FinishGrind();
        Bail(play, player, "BAIL! LOST BALANCE");
        return;
    }

    // Jump off
    if (CHECK_BTN_ALL(input->press.button, BTN_A)) {
        ExitGrindToAir(player, Tune::kOllieBase * sqrtf(s));
        PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
        SetAnim(play, player, ANIM_AIR_UP);
        return;
    }

    // Follow the ledge: try straight ahead, then small corrections for curves
    f32 step = std::max(sSkate.speed, 4.0f);
    bool found = false;
    f32 nextY = pos.y;
    static const s16 sCorrections[] = { 0, 0x300, -0x300, 0x600, -0x600, 0x900, -0x900 };
    for (s16 corr : sCorrections) {
        s16 yaw = sSkate.grindYaw + corr;
        if (IsGrindEdge(play, pos.x + Math_SinS(yaw) * step, pos.z + Math_CosS(yaw) * step, pos.y, yaw, &nextY)) {
            sSkate.grindYaw = yaw;
            found = true;
            break;
        }
    }
    if (!found) {
        // End of the rail: pop off and keep flying
        ExitGrindToAir(player, 3.0f);
        SetAnim(play, player, ANIM_AIR_DOWN);
        return;
    }

    // Downhill rails speed you up, uphill ones slow you down
    sSkate.speed += (pos.y - nextY) * 0.08f;
    Math_StepToF(&sSkate.speed, 0.0f, Tune::kGrindFriction);
    if (sSkate.speed < Tune::kGrindMinSpeed) {
        ExitGrindToAir(player, 2.0f);
        SetAnim(play, player, ANIM_AIR_DOWN);
        return;
    }
    sSkate.speed = std::min(sSkate.speed, Tune::kMaxSpeed * Cfg_SpeedMult());
    sSkate.moveYaw = sSkate.grindYaw;
    player->actor.velocity.y = 0.0f;

    // Sparks
    if ((play->gameplayFrames % 2) == 0) {
        static Color_RGBA8 sPrim = { 255, 255, 200, 255 };
        static Color_RGBA8 sEnv = { 255, 160, 0, 255 };
        Vec3f sparkPos = pos;
        sparkPos.y += 2.0f;
        Vec3f vel = { Rand_CenteredFloat(3.0f), 2.0f + Rand_ZeroOne() * 2.0f, Rand_CenteredFloat(3.0f) };
        Vec3f accel = { 0.0f, -0.6f, 0.0f };
        EffectSsKiraKira_SpawnSmall(play, &sparkPos, &vel, &accel, &sPrim, &sEnv);
    }
    PlayLoopSfx(player, NA_SE_PL_SLIP_LEVEL, 1.5f, 0.9f);
    SetAnim(play, player, ANIM_GRIND);
}

static void UpdateVisuals(Player* player) {
    s16 stanceSign = (Cfg_Stance() == STANCE_GOOFY) ? -1 : 1;
    // Turn from sideways (riding stance) to facing forward while pushing, then back
    bool faceForward = (sSkate.pushTimer > 0 || sSkate.towHorse != nullptr) && sSkate.phase == PHASE_GROUND;
    Math_StepToF(&sSkate.pushBlend, faceForward ? 1.0f : 0.0f, 0.25f);
    s16 stanceOffset = (s16)(stanceSign * 0x4000 + (sSkate.fakie ? 0x8000 : 0));
    s16 baseBody = (s16)(sSkate.moveYaw + (s16)(stanceOffset * (1.0f - sSkate.pushBlend)));

    sSkate.boardYaw = sSkate.moveYaw + (s16)sSkate.boardYawBase;
    sSkate.boardPitch = 0;
    sSkate.boardRoll = 0;
    sSkate.boardDrop = 0.0f;
    s16 targetTiltX = 0;
    s16 targetTiltZ = 0;
    s16 bodyExtraYaw = 0;

    switch (sSkate.phase) {
        case PHASE_AIR: {
            sSkate.boardYaw += (s16)sSkate.spin;
            bodyExtraYaw = (s16)sSkate.spin;
            if (sSkate.flipIdx >= 0) {
                const FlipTrick& flip = sFlipTricks[sSkate.flipIdx];
                f32 t = 1.0f - (f32)sSkate.flipTimer / flip.frames;
                sSkate.boardRoll = (s16)(s32)(static_cast<f32>(flip.roll) * t);
                sSkate.boardPitch = (s16)(s32)(static_cast<f32>(flip.pitch) * t);
                sSkate.boardYaw += (s16)(s32)(static_cast<f32>(flip.yaw) * t);
                sSkate.boardDrop = 6.0f * AgeScale() * sinf(t * kPi);
            }
            if (sSkate.grabIdx > 0) {
                const GrabTrick& grab = sGrabTricks[sSkate.grabIdx];
                sSkate.boardPitch = grab.boardPitch;
                sSkate.boardRoll = grab.boardRoll;
                targetTiltX = grab.boardPitch / 3;
                targetTiltZ = grab.boardRoll / 3;
            }
            break;
        }
        case PHASE_GRIND: {
            const GrindTrick& grind = sGrindTricks[sSkate.grindIdx];
            sSkate.boardYaw += grind.boardYaw;
            sSkate.boardPitch = grind.boardPitch;
            bodyExtraYaw = grind.boardYaw;
            targetTiltZ = (s16)(sSkate.balance * 0x1800);
            break;
        }
        default:
            // Lean into the push, or lean back while being towed
            targetTiltX = (s16)((sSkate.towHorse != nullptr ? -0x0600 : 0x0700) * sSkate.pushBlend);
            break;
    }

    Math_ScaledStepToS(&sSkate.tiltX, targetTiltX, 0x400);
    Math_ScaledStepToS(&sSkate.tiltZ, targetTiltZ, 0x600);

    sSkate.visualRot.x = sSkate.tiltX;
    sSkate.visualRot.y = baseBody + bodyExtraYaw;
    sSkate.visualRot.z = sSkate.tiltZ;

    // Look where you're going (head and torso turn toward the direction of travel)
    s16 lookYaw = (s16)(sSkate.moveYaw - baseBody);
    if (sSkate.phase == PHASE_AIR) {
        lookYaw = 0;
    }
    player->upperLimbRot.y = (s16)(lookYaw * 0.35f);
    player->headLimbRot.y = (s16)(lookYaw * 0.45f);
    player->unk_6AE_rotFlags |= UNK6AE_ROT_HEAD_Y | UNK6AE_ROT_UPPER_Y;

    // Stand on top of the board
    player->actor.shape.yOffset = (3.0f * AgeScale() + Cfg_BoardHeight()) / player->actor.scale.y;
}

void Skate_Action(Player* player, PlayState* play) {
    Input* input = (sActionInput != nullptr) ? sActionInput : &play->state.input[0];
    sActionInput = nullptr;

    if (sSkate.bailPending || !SKATE_ENABLED) {
        // Knockback didn't take for some reason, or the mod was switched off: step off cleanly.
        Dismount(play, player);
        return;
    }

    player->stateFlags3 |= PLAYER_STATE3_MIDAIR; // stop the game from turning a roll off a ledge into a fall
    player->stateFlags2 |= PLAYER_STATE2_DISABLE_ROTATION_ALWAYS;
    player->actor.gravity = Tune::kGravity;
    player->actor.minVelocityY = -20.0f;

    if (sSkate.anim == ANIM_PUSH) {
        // Ease through one stride so the kick starts slow, sweeps back, and settles
        f32 t = 1.0f - (f32)sSkate.pushTimer / Tune::kPushFrames;
        player->skelAnime.curFrame = sSkate.pushStrideEnd * (0.5f - 0.5f * cosf(t * kPi));
    }
    LinkAnimation_Update(play, &player->skelAnime);

    if (sSkate.pushCooldown > 0) {
        sSkate.pushCooldown--;
    }
    if (sSkate.noGrindTimer > 0) {
        sSkate.noGrindTimer--;
    }
    if (sSkate.landTimer > 0) {
        sSkate.landTimer--;
    }

    bool grounded = (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    // While rolling, tiny bumps and steps down don't count as leaving the ground
    bool nearFloor = (player->actor.world.pos.y - player->actor.floorHeight) < 10.0f * AgeScale() &&
                     player->actor.velocity.y <= 0.0f;

    // Water skating: hold Link on top of the surface as long as he's on the board.
    // The game makes Link swim once his feet are deep enough under the surface, so we catch him before that,
    // including when a fast fall would carry him past the surface within the next frame.
    sSkate.onWater = false;
    f32 waterY;
    if (Cfg_WaterSkate() && sSkate.phase != PHASE_GRIND && player->actor.velocity.y <= 0.0f &&
        GetWaterSurface(play, player, &waterY)) {
        f32 y = player->actor.world.pos.y;
        f32 nextY = y + player->actor.velocity.y + Tune::kGravity;
        if (y >= waterY - 30.0f && (y <= waterY + 1.0f || (sSkate.phase == PHASE_AIR && nextY <= waterY))) {
            player->actor.world.pos.y = waterY;
            player->actor.velocity.y = 0.0f;
            sSkate.onWater = true;
            grounded = true;
            nearFloor = true;
        }
    }

    // Bleed speed if something (a wall, an actor) stopped us harder than we think
    sSkate.impactSpeed = sSkate.speed;
    f32 moved = Math_Vec3f_DistXZ(&player->actor.world.pos, &player->actor.prevPos);
    if (sSkate.phase != PHASE_GRIND && (player->actor.bgCheckFlags & BGCHECKFLAG_WALL) && moved + 1.5f < sSkate.speed) {
        sSkate.speed = std::max(moved, sSkate.speed * 0.6f);
    }

    // Bomb hop: drop a bomb with your Bombs button (or Z if bombs aren't equipped)
    if (sSkate.phase != PHASE_GRIND && CHECK_BTN_ANY(input->press.button, BombButtonMask())) {
        DropBomb(player);
    }

    // Towed by Epona: she sets our speed and direction, we still do everything else
    bool towing = (sSkate.phase != PHASE_GRIND) && UpdateTow(play, player, input);
    if (towing && sSkate.phase == PHASE_GROUND && CHECK_BTN_ALL(input->press.button, BTN_A)) {
        // Let go with an ollie
        EndTow(play, true);
        towing = false;
        StartAir(player, Tune::kOllieBase * sqrtf(AgeScale()));
        PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
        SetAnim(play, player, ANIM_AIR_UP);
    }

    switch (sSkate.phase) {
        case PHASE_GROUND:
            if (towing && (grounded || nearFloor)) {
                break; // just roll along behind her
            }
            if (!grounded && (!nearFloor || sSkate.lastRise > 0.15f)) {
                // Rolled off an edge or the top of a ramp: launch along the slope
                f32 launch = std::max(0.0f, sSkate.lastRise) * sSkate.speed * Tune::kRampLaunch;
                StartAir(player, launch);
                sSkate.charging = false;
                sSkate.chargeFrames = 0;
                SetAnim(play, player, launch > 2.0f ? ANIM_AIR_UP : ANIM_AIR_DOWN);
                UpdateAir(play, player, input);
            } else {
                UpdateGround(play, player, input);
            }
            break;
        case PHASE_AIR:
            if (grounded && player->actor.velocity.y <= 0.0f) {
                Land(play, player);
                if (sSkate.bailPending) {
                    return;
                }
            } else {
                UpdateAir(play, player, input);
            }
            break;
        case PHASE_GRIND:
            UpdateGrind(play, player, input);
            break;
    }

    if (sSkate.bailPending) {
        return;
    }

    // Step off (only with both feet on the ground)
    s32 toggleMask = Cfg_ToggleMask();
    if (sSkate.phase == PHASE_GROUND && toggleMask != 0 && CHECK_BTN_ALL(input->cur.button, toggleMask) &&
        CHECK_BTN_ANY(input->press.button, toggleMask)) {
        Dismount(play, player);
        return;
    }

    // Gliding on water: no gravity, so Link stays exactly on the surface until he ollies
    player->actor.gravity = (sSkate.onWater && sSkate.phase == PHASE_GROUND) ? 0.0f : Tune::kGravity;

    player->yaw = sSkate.moveYaw;
    player->linearVelocity = sSkate.speed;
    player->actor.shape.rot.y = sSkate.moveYaw;
    player->actor.shape.rot.x = 0;
    player->actor.shape.rot.z = 0;
    sSkate.logicRot = player->actor.shape.rot;

    UpdateVisuals(player);
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing: the board and the HUD
// ---------------------------------------------------------------------------------------------------------------------

static bool IsRiding(Player* player) {
    return sSkate.active && player->actionFunc == Skate_Action;
}

// The camera follows Link's model rotation, so Link "faces forward" during the game update and only turns
// sideways into his skating stance while the frame is drawn.
static void OnDrawBegin() {
    if (gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr && IsRiding(player)) {
        player->actor.shape.rot = sSkate.visualRot;
    }
}

static void DrawBoard(PlayState* play, Player* player) {
    f32 age = AgeScale();
    f32 size = Cfg_BoardSize() * age;
    s16 gid;
    f32 width, thick, length;

    switch (Cfg_BoardModel()) {
        case BOARD_HYLIAN_SHIELD:
            gid = GID_SHIELD_HYLIAN;
            width = 0.17f, thick = 0.12f, length = 0.20f;
            break;
        case BOARD_MIRROR_SHIELD:
            gid = GID_SHIELD_MIRROR;
            width = 0.17f, thick = 0.12f, length = 0.20f;
            break;
        case BOARD_DEKU_SHIELD:
        default:
            gid = GID_SHIELD_DEKU;
            width = 0.20f, thick = 0.12f, length = 0.31f; // stretch the round shield into a deck shape
            break;
    }

    Vec3f pos = player->actor.world.pos;
    pos.y += 1.5f * age + Cfg_BoardHeight() - sSkate.boardDrop;

    OPEN_DISPS(play->state.gfxCtx);
    FrameInterpolation_RecordOpenChild(&sSkate, 0);

    Lights* lights = LightContext_NewLights(&play->lightCtx, play->state.gfxCtx);
    Lights_BindAll(lights, play->lightCtx.listHead, &pos);
    Lights_Draw(lights, play->state.gfxCtx);

    Matrix_Translate(pos.x, pos.y, pos.z, MTXMODE_NEW);
    Matrix_RotateY(static_cast<f32>(BINANG_TO_RAD(sSkate.boardYaw)), MTXMODE_APPLY);
    Matrix_RotateX(static_cast<f32>(BINANG_TO_RAD(sSkate.boardPitch)), MTXMODE_APPLY);
    Matrix_RotateZ(static_cast<f32>(BINANG_TO_RAD(sSkate.boardRoll)), MTXMODE_APPLY);
    Matrix_Scale(width * size, thick * size, length * size, MTXMODE_APPLY);
    // Get-item models stand upright facing the camera; lay the shield flat, face up
    switch (Cfg_BoardOrientation()) {
        case 1:
            Matrix_RotateX(kPi / 2.0f, MTXMODE_APPLY);
            break;
        case 2:
            Matrix_RotateZ(kPi / 2.0f, MTXMODE_APPLY);
            break;
        case 3:
            break;
        default:
            Matrix_RotateX(-kPi / 2.0f, MTXMODE_APPLY);
            break;
    }
    GetItem_Draw(play, gid);

    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(play->state.gfxCtx);
}

static void HudPrintCentered(GfxPrint* printer, s32 row, const std::string& text) {
    std::string clipped = text.size() > 38 ? "..." + text.substr(text.size() - 35) : text;
    GfxPrint_SetPos(printer, std::max<s32>(0, (40 - (s32)clipped.size()) / 2), row);
    GfxPrint_Printf(printer, "%s", clipped.c_str());
}

static void HudDraw(GfxPrint* printer) {
    if (sSkate.active) {
        GfxPrint_SetColor(printer, 255, 255, 255, 255);
        HudPrintCentered(printer, 2, "SCORE " + std::to_string(sSkate.session));

        // Current combo, including the trick you're in the middle of
        std::string comboText;
        for (const std::string& name : sSkate.combo.names) {
            comboText += (comboText.empty() ? "" : " + ") + name;
        }
        s32 base = sSkate.combo.base;
        s32 mult = sSkate.combo.Mult();
        if (sSkate.phase == PHASE_GRIND) {
            comboText += std::string(comboText.empty() ? "" : " + ") + sGrindTricks[sSkate.grindIdx].name;
            base += sGrindTricks[sSkate.grindIdx].points + sSkate.grindFrames * 10;
            mult = (s32)sSkate.combo.names.size() + 1;
        } else if (sSkate.grabIdx > 0) {
            comboText += std::string(comboText.empty() ? "" : " + ") + sGrabTricks[sSkate.grabIdx].name;
            base += sGrabTricks[sSkate.grabIdx].points + std::min<s32>(sSkate.grabFrames, 60) * 15;
            mult = (s32)sSkate.combo.names.size() + 1;
        }

        if (!comboText.empty()) {
            GfxPrint_SetColor(printer, 255, 255, 255, 255);
            HudPrintCentered(printer, 4, comboText);
            GfxPrint_SetColor(printer, 255, 220, 60, 255);
            HudPrintCentered(printer, 5, std::to_string(base) + " X " + std::to_string(mult));
        }

        if (sSkate.phase == PHASE_GRIND) {
            // Balance meter: [----|----]
            std::string meter(17, '-');
            s32 idx = std::clamp((s32)((sSkate.balance + 1.0f) * 8.0f), 0, 16);
            meter[8] = '|';
            meter[idx] = 'O';
            bool danger = std::fabs(sSkate.balance) > 0.65f;
            GfxPrint_SetColor(printer, 255, danger ? 80 : 255, danger ? 80 : 255, 255);
            HudPrintCentered(printer, 7, "[" + meter + "]");
        } else if (sSkate.charging) {
            s32 bars = sSkate.chargeFrames * 10 / Tune::kOllieChargeFrames;
            GfxPrint_SetColor(printer, 120, 200, 255, 255);
            HudPrintCentered(printer, 7, "POP " + std::string(bars, '=') + std::string(10 - bars, ' '));
        }
    }

    bool inPark = sSkate.active && gPlayState != nullptr && InParkArea(GET_PLAYER(gPlayState), nullptr);
    if (inPark) {
        GfxPrint_SetColor(printer, ParkDone() == PARK_ALL ? 120 : 255, 230, 120, 255);
        HudPrintCentered(printer, 25, ParkHudLine());
    } else if (sSkate.active && gPlayState != nullptr && sPark.actor != nullptr) {
        GfxPrint_SetColor(printer, 180, 220, 255, 255);
        HudPrintCentered(printer, 27, ParkDirectionHint(GET_PLAYER(gPlayState)));
    }

    if (!inPark && sSkate.active && Cfg_Quests() && gPlayState != nullptr && IsQuestArea(gPlayState->sceneNum)) {
        s32 scene = gPlayState->sceneNum;
        GfxPrint_SetColor(printer, QuestDone(scene) == QUEST_ALL ? 120 : 255, 230, 120, 255);
        HudPrintCentered(printer, 25, QuestHudLine(scene));
        if (sLettersReady && LettersMask(scene) != (1 << kLetterCount) - 1) {
            Player* player = GET_PLAYER(gPlayState);
            GfxPrint_SetColor(printer, 200, 200, 200, 255);
            HudPrintCentered(printer, 26, "NEXT " + NextLetterHint(player));
        }
    }

    if (sSkate.msgTimer > 0) {
        switch (sSkate.msgColor) {
            case MSG_GOOD:
                GfxPrint_SetColor(printer, 120, 255, 120, 255);
                break;
            case MSG_BAD:
                GfxPrint_SetColor(printer, 255, 90, 90, 255);
                break;
            default:
                GfxPrint_SetColor(printer, 255, 255, 255, 255);
                break;
        }
        HudPrintCentered(printer, 9, sSkate.msg);
    }
}

static void DrawHud(PlayState* play) {
    OPEN_DISPS(play->state.gfxCtx);

    GfxPrint printer;
    Gfx* polyOpa = POLY_OPA_DISP;
    Gfx* gfx = Graph_GfxPlusOne(polyOpa);
    gSPDisplayList(OVERLAY_DISP++, gfx);

    GfxPrint_Init(&printer);
    GfxPrint_Open(&printer, gfx);
    HudDraw(&printer);
    gfx = GfxPrint_Close(&printer);
    GfxPrint_Destroy(&printer);

    gSPEndDisplayList(gfx++);
    Graph_BranchDlist(polyOpa, gfx);
    POLY_OPA_DISP = gfx;

    CLOSE_DISPS(play->state.gfxCtx);
}

static void OnDrawEnd() {
    if (gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }
    if (IsRiding(player)) {
        DrawBoard(gPlayState, player);
        player->actor.shape.rot = sSkate.logicRot; // back to the camera-friendly rotation
    }
    if (Cfg_Quests()) {
        DrawLetters(gPlayState);
    }
    if (sSkate.bombActive) {
        // Swells up just before it goes off
        f32 scale = 0.35f + ((sSkate.bombTimer < 4) ? (4 - sSkate.bombTimer) * 0.04f : 0.0f);
        DrawItemAt(gPlayState, &sSkate.bombPos, sSkate.bombPos, GID_BOMB, scale, sSkate.moveYaw);
    }
    if (Cfg_ShowHud() && (IsRiding(player) || sSkate.msgTimer > 0)) {
        DrawHud(gPlayState);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame bookkeeping: mounting, noticing when the game took Link off the board, keeping the board between areas
// ---------------------------------------------------------------------------------------------------------------------

static void OnPlayerUpdate() {
    if (gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }

    UpdateMessages();
    if (sSkate.toggleCooldown > 0) {
        sSkate.toggleCooldown--;
    }
    sSkate.sceneFrames++;

    bool riding = sSkate.active && player->actionFunc == Skate_Action;

    // Quests: place the letters once Link is standing in an outdoor area, then check for pickups
    if (Cfg_Quests() && IsQuestArea(gPlayState->sceneNum)) {
        if (!sLettersReady && sSkate.sceneFrames > 10 && (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) &&
            player->actor.floorHeight > BGCHECK_Y_MIN) {
            PlaceLetters(gPlayState, player);
        }
        UpdateLetters(gPlayState, player, riding);
    } else if (sLettersReady) {
        ClearLetters();
    }

    UpdateBomb(gPlayState, player, riding);
    UpdatePark(gPlayState, player, riding);

    // Seamless loading zones: we rolled into a new area. Skip Link's walk-in and put him straight back on the board.
    if (sSkate.carryPending && !sSkate.carryActive && ++sSkate.carryTimer > 30) {
        sSkate.carryPending = false; // the area change never happened
    }
    if (sSkate.carryActive) {
        if (sSkate.sceneFrames > 60) {
            sSkate.carryPending = sSkate.carryActive = false;
            sSkate.combo.Clear();
        } else if (!sSkate.active && !sSkate.mountPending && sSkate.sceneFrames >= 3 &&
                   player->actionFunc == Player_Action_80845CA4) {
            func_8005B1A4(Play_GetCamera(gPlayState, 0));
            func_80845C68(gPlayState, gSaveContext.respawn[RESPAWN_MODE_DOWN].data);
            sSkate.remountPending = false;
            TryMount(gPlayState, player);
            return;
        }
    }

    // Mount request was interrupted (hit while putting items away, etc.)
    if (sSkate.mountPending && player->actionFunc != Player_Action_WaitForPutAway &&
        player->actionFunc != Skate_Action) {
        sSkate.mountPending = false;
    }

    // The game replaced our action (damage, swimming, cutscene, bail knockback...)
    if (sSkate.active && player->actionFunc != Skate_Action) {
        bool leavingArea =
            gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || (player->stateFlags1 & PLAYER_STATE1_LOADING);
        bool keepCombo = false;
        if (leavingArea && Cfg_KeepBoard()) {
            sSkate.remountPending = true;
            sSkate.remountTimer = 100;
            // Rode into a loading zone: swap the fade for an instant cut and carry our speed and combo across
            if (Cfg_Seamless() && gPlayState->transitionTrigger == TRANS_TRIGGER_START &&
                gSaveContext.respawnFlag == 0) {
                gPlayState->transitionType = TRANS_TYPE_INSTANT;
                gSaveContext.nextTransitionType = TRANS_TYPE_INSTANT;
                sSkate.carryPending = true;
                sSkate.carryActive = false;
                sSkate.carryTimer = 0;
                sSkate.carrySpeed = sSkate.speed;
                keepCombo = true;
            }
        } else if (!sSkate.bailPending && !sSkate.combo.Empty()) {
            ShowMsg("COMBO LOST", MSG_BAD, 30);
        }
        if (!keepCombo) {
            sSkate.combo.Clear();
        }
        ResetVisuals(player);
        ClearRideState();
    }

    if (sSkate.active || sSkate.mountPending) {
        return;
    }

    Input* input = &gPlayState->state.input[0];
    s32 toggleMask = Cfg_ToggleMask();
    bool togglePressed = toggleMask != 0 && CHECK_BTN_ALL(input->cur.button, toggleMask) &&
                         CHECK_BTN_ANY(input->press.button, toggleMask);

    if (sSkate.remountPending) {
        if (sSkate.remountTimer-- <= 0) {
            sSkate.remountPending = false;
        } else if (CanMount(gPlayState, player)) {
            sSkate.remountPending = false;
            TryMount(gPlayState, player);
            return;
        }
    }

    if (togglePressed && sSkate.toggleCooldown == 0 && CanMount(gPlayState, player)) {
        TryMount(gPlayState, player);
    }
}

static void OnPlayDestroy() {
    bool wasRiding = sSkate.active || sSkate.mountPending;
    sSkate.towHorse = nullptr; // the area's actors are going away; don't touch Epona
    ClearRideState();
    sSkate.mountPending = false;
    sSkate.carryActive = sSkate.carryPending;
    if (!sSkate.carryPending) {
        sSkate.combo.Clear();
    }
    sSkate.msgTimer = 0;
    sMsgQueue.clear();
    sSkate.bombActive = false;
    sSkate.sceneFrames = 0;
    sGoalsShown = false;
    ClearLetters();
    sPark.actor = nullptr; // destroyed with the area
    sPark.bgId = -1;
    sPark.searching = false;
    sPark.respawn = false;
    sPark.failed = false;
    sPark.announced = false;
    sPark.overTriHole = false;
    // Hop back on in the next area (loading zones, doors, voids)
    sSkate.remountPending = (sSkate.remountPending || wasRiding) && Cfg_KeepBoard();
    sSkate.remountTimer = 100;
}

static void RegisterSkateboard() {
    bool enabled = SKATE_ENABLED;
    if (!enabled) {
        ClearLetters();
    }

    // Grab the input the game is about to hand the player action (it is blanked during some transitions)
    COND_VB_SHOULD(VB_EXECUTE_PLAYER_ACTION_FUNC, enabled, {
        Player* player = va_arg(args, Player*);
        Input* input = va_arg(args, Input*);
        if (player->actionFunc == Skate_Action) {
            sActionInput = input;
        }
    });

    COND_HOOK(OnPlayerUpdate, enabled, OnPlayerUpdate);
    COND_HOOK(OnPlayDrawBegin, enabled, OnDrawBegin);
    COND_HOOK(OnPlayDrawEnd, enabled, OnDrawEnd);
    COND_HOOK(OnPlayDestroy, enabled, OnPlayDestroy);
}

static RegisterShipInitFunc initFunc(RegisterSkateboard, { CVAR_SKATE_ENABLED });

// ---------------------------------------------------------------------------------------------------------------------
// Menu: Enhancements > Extra Modes > Skateboard
// ---------------------------------------------------------------------------------------------------------------------

static std::map<int32_t, const char*> sBoardModels = {
    { BOARD_DEKU_SHIELD, "Deku Shield" },
    { BOARD_HYLIAN_SHIELD, "Hylian Shield" },
    { BOARD_MIRROR_SHIELD, "Mirror Shield" },
};

static std::map<int32_t, const char*> sOrientations = {
    { 0, "Face Up" },
    { 1, "Face Down" },
    { 2, "Rolled" },
    { 3, "As Modeled" },
};

static std::map<int32_t, const char*> sStances = {
    { STANCE_REGULAR, "Regular" },
    { STANCE_GOOFY, "Goofy" },
};

static void RegisterSkateboardWidgets() {
    WidgetPath path = { "Enhancements", "Extra Modes", SECTION_COLUMN_3 };
    auto disabledIfOff = [](WidgetInfo& info) { info.options->disabled = !SKATE_ENABLED; };

    SohGui::mSohMenu->AddWidget(path, "Skateboard", WIDGET_SEPARATOR_TEXT);

    SohGui::mSohMenu->AddWidget(path, "Enable Skateboard", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE_ENABLED)
        .Options(UIWidgets::CheckboxOptions().Tooltip(
            "Ride one of Link's shields like a skateboard.\n\n"
            "D-Pad Down (default): hop on / off\n"
            "Stick up/down: roll / brake    Stick left/right: carve, spin in the air, balance on grinds\n"
            "B: push    Hold A: crouch, release to ollie\n"
            "B + direction in the air: flip tricks\n"
            "R + direction in the air: grabs (hold for more points)\n"
            "Hold R while landing on a ledge, or press R while rolling along one: grind\n"
            "Land straight (in 180 degree steps) and let go of grabs before touching down!"));

    SohGui::mSohMenu->AddWidget(path, "Board On/Off Button:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_SKATE("ToggleBtn"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::BtnSelectorOptions().DefaultValue(BTN_DDOWN).Tooltip(
            "Button (or combination) that hops on and off the board."));

    SohGui::mSohMenu->AddWidget(path, "Board", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SKATE("BoardModel"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::ComboboxOptions()
                     .ComboMap(sBoardModels)
                     .DefaultIndex(BOARD_DEKU_SHIELD)
                     .Tooltip("Which shield model to use as the board. You don't need to own it."));

    SohGui::mSohMenu->AddWidget(path, "Board Size: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SKATE("BoardSize"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::FloatSliderOptions().Min(0.5f).Max(2.0f).DefaultValue(1.0f).Format("%.2fx"));

    SohGui::mSohMenu->AddWidget(path, "Board Height: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SKATE("BoardHeight"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::FloatSliderOptions().Min(-5.0f).Max(5.0f).DefaultValue(0.0f).Format("%.1f").Tooltip(
            "Nudge the board up or down if it clips into Link's feet."));

    SohGui::mSohMenu->AddWidget(path, "Board Orientation", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SKATE("BoardOrientation"))
        .PreFunc(disabledIfOff)
        .Options(
            UIWidgets::ComboboxOptions()
                .ComboMap(sOrientations)
                .DefaultIndex(0)
                .Tooltip("If the board shows up standing on its edge instead of lying flat, try the other options."));

    SohGui::mSohMenu->AddWidget(path, "Stance", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SKATE("Stance"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::ComboboxOptions().ComboMap(sStances).DefaultIndex(STANCE_REGULAR));

    SohGui::mSohMenu->AddWidget(path, "Top Speed: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SKATE("SpeedMult"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::FloatSliderOptions().Min(0.5f).Max(1.5f).DefaultValue(1.0f).Format("%.2fx"));

    SohGui::mSohMenu->AddWidget(path, "Skate On Water", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("WaterSkating"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "Ride across lakes and rivers as long as you stay on the board.\n"
            "Step off or bail over deep water and Link falls in and swims."));

    SohGui::mSohMenu->AddWidget(path, "Seamless Loading Zones", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("SeamlessAreas"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "While on the board, loading zones between areas cut instantly instead of fading to black,\n"
            "and you roll out the other side with the same speed and your combo still going."));

    SohGui::mSohMenu->AddWidget(path, "Wall Jumps", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("WallJumps"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "Press A as you hit a wall in the air to bounce off it (up to 3 times per jump)."));

    SohGui::mSohMenu->AddWidget(path, "Free Bomb Hops", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("FreeBombs"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().Tooltip(
            "Bomb hops normally use a bomb from your Bomb Bag. Tick this to hop without bombs.\n"
            "Drop a bomb with the button your Bombs are on (or Z if they aren't equipped)."));

    SohGui::mSohMenu->AddWidget(path, "Area Quests", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("Quests"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "Every outdoor area has three skate goals:\n"
            "- Spell Z-E-L-D-A: collect the five golden letters (on the board)\n"
            "- Clear 3 different gaps: jumps over pits, water, or from rail to rail\n"
            "- Land a 5,000 point combo"));

    SohGui::mSohMenu->AddWidget(path, "Quest Rupee Rewards", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("QuestRupees"))
        .PreFunc([](WidgetInfo& info) { info.options->disabled = !SKATE_ENABLED || !Cfg_Quests(); })
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "20 rupees per quest, plus 50 for finishing all three in an area."));

    SohGui::mSohMenu->AddWidget(path, "Quest Log", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        info.isHidden = !SKATE_ENABLED || !Cfg_Quests();
        std::string log = "Quest Log (letters / gaps / 5,000 combo):";
        s32 mastered = 0;
        for (s32 scene = SCENE_HYRULE_FIELD; scene <= SCENE_LON_LON_RANCH; scene++) {
            s32 done = QuestDone(scene);
            s32 letters = 0;
            s32 mask = LettersMask(scene);
            for (s32 i = 0; i < kLetterCount; i++) {
                letters += (mask >> i) & 1;
            }
            s32 gaps = std::min((s32)LoadGaps(scene).size(), Tune::kQuestGapGoal);
            if (done == QUEST_ALL) {
                mastered++;
            }
            log += "\n" + SohUtils::GetSceneName(scene) + ": " + std::to_string(letters) + "/5, " +
                   std::to_string(gaps) + "/3, " + ((done & QUEST_COMBO) ? "done" : "-") +
                   (done == QUEST_ALL ? "  (mastered)" : "");
        }
        log += "\nAreas mastered: " + std::to_string(mastered) + "/19";
        info.name = log;
    });

    SohGui::mSohMenu->AddWidget(path, "Reset Quest Progress", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !SKATE_ENABLED || !Cfg_Quests(); })
        .Callback([](WidgetInfo&) {
            for (s32 scene = SCENE_HYRULE_FIELD; scene <= SCENE_LON_LON_RANCH; scene++) {
                CVarClear(QuestKey("Done", scene).c_str());
                CVarClear(QuestKey("Letters", scene).c_str());
                CVarClear(QuestKey("Gaps", scene).c_str());
            }
            CVarClear(CVAR_SKATE("Park.Done"));
            CVarClear(CVAR_SKATE("Park.Treasures"));
            SaveCVars();
            ClearLetters(); // they get placed again, uncollected
        });

    SohGui::mSohMenu->AddWidget(path, "Hyrule Skate Park", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("Park"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "An Ocarina of Time themed skate park in Hyrule Field: half pipe, Triforce funbox, Master Sword rails,\n"
            "Goron funboxes. Grab the Spiritual Stones and the vert treasures, clear the Triforce gap and land a\n"
            "10,000 point combo. The first time you visit Hyrule Field it finds a flat open spot for itself."));

    SohGui::mSohMenu->AddWidget(path, "Teleport to the Skate Park", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !SKATE_ENABLED || !Cfg_Park();
            info.options->disabled =
                gPlayState == nullptr || gPlayState->sceneNum != SCENE_HYRULE_FIELD || sPark.actor == nullptr;
            info.options->disabledTooltip = "Only in Hyrule Field, once the park is there.";
        })
        .Callback([](WidgetInfo&) {
            if (gPlayState == nullptr || sPark.actor == nullptr) {
                return;
            }
            Player* player = GET_PLAYER(gPlayState);
            Vec3f pos = { sPark.center.x + Park::kHalf + Park::kRampLen + 60.0f, sPark.rampFootY[0] + 20.0f,
                          sPark.center.z };
            player->actor.world.pos = player->actor.home.pos = player->actor.prevPos = pos;
            player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw = (s16)0xC000; // face the park
            player->linearVelocity = 0.0f;
        });

    SohGui::mSohMenu->AddWidget(path, "Move the Skate Park Here", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !SKATE_ENABLED || !Cfg_Park();
            info.options->disabled = gPlayState == nullptr || gPlayState->sceneNum != SCENE_HYRULE_FIELD;
            info.options->disabledTooltip = "Stand where you want it in Hyrule Field.";
        })
        .Callback([](WidgetInfo&) {
            if (gPlayState == nullptr || gPlayState->sceneNum != SCENE_HYRULE_FIELD) {
                return;
            }
            Player* player = GET_PLAYER(gPlayState);
            sPark.center.x = player->actor.world.pos.x;
            sPark.center.z = player->actor.world.pos.z;
            SaveParkSpot();
            sPark.searching = false;
            sPark.failed = false;
            RemovePark(gPlayState);
            sPark.respawn = true;
        });

    SohGui::mSohMenu->AddWidget(path, "Find a New Park Spot Automatically", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !SKATE_ENABLED || !Cfg_Park(); })
        .Callback([](WidgetInfo&) {
            CVarClear(CVAR_SKATE("Park.Placed"));
            SaveCVars();
            sPark.failed = false;
            if (gPlayState != nullptr) {
                RemovePark(gPlayState);
            }
        });

    SohGui::mSohMenu->AddWidget(path, "Show Trick HUD", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("ShowHud"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "Trick names, combo score, grind balance meter and ollie power."));

    SohGui::mSohMenu->AddWidget(path, "Rolling Sound", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("RollingSound"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true));

    SohGui::mSohMenu->AddWidget(path, "Keep Board Between Areas", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("KeepBetweenAreas"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "Hop straight back on after doors, loading zones and void-outs."));

    SohGui::mSohMenu->AddWidget(path, "Bails Hurt", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("BailsHurt"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().Tooltip("Falling off the board costs a little health."));

    SohGui::mSohMenu->AddWidget(path, "Rupees For Big Combos", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SKATE("RupeeReward"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().Tooltip("Earn 1 rupee per 1,000 points in a landed combo (max 50)."));

    SohGui::mSohMenu->AddWidget(path, "Best Combo", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        info.name = "Best Combo: " + std::to_string(CVarGetInteger(CVAR_SKATE("BestCombo"), 0));
    });

    SohGui::mSohMenu->AddWidget(path, "Reset Best Combo", WIDGET_BUTTON).Callback([](WidgetInfo&) {
        CVarSetInteger(CVAR_SKATE("BestCombo"), 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    });
}

static RegisterMenuInitFunc menuInitFunc(RegisterSkateboardWidgets);
