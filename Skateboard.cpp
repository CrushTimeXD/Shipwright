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
 */

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <algorithm>
#include <cmath>
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

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
}

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
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
// Safety valve in case a shield model is oriented differently than expected: cycles how it's laid flat
static s32 Cfg_BoardOrientation() {
    return CVarGetInteger(CVAR_SKATE("BoardOrientation"), 0);
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
constexpr s16 kPushCooldown = 9;
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

enum SkateAnim { ANIM_NONE, ANIM_RIDE, ANIM_CROUCH, ANIM_AIR_UP, ANIM_AIR_DOWN, ANIM_TUCK, ANIM_GRIND, ANIM_LAND };

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
    s16 chargeFrames = 0;
    bool charging = false;
    f32 lastRise = 0.0f;    // slope along the travel direction on the last grounded frame
    f32 impactSpeed = 0.0f; // speed going into this frame, before walls slowed us down
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
    sSkate.msg = text;
    sSkate.msgColor = color;
    sSkate.msgTimer = frames;
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
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        text += "  NEW BEST!";
        Sfx_PlaySfxCentered(NA_SE_SY_GET_RUPY);
    } else if (total >= 1000) {
        Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
    }
    ShowMsg(text, MSG_GOOD, 50);

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
    sSkate.combo.Clear();
    sSkate.session = 0;
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
    sSkate.spin = 0;
    sSkate.airTrickStart = (s32)sSkate.combo.names.size();
    sSkate.flipIdx = -1;
    sSkate.flipTimer = 0;
    sSkate.grabIdx = -1;
    sSkate.grabFrames = 0;
    if (launchVelY > player->actor.velocity.y) {
        player->actor.velocity.y = launchVelY;
    }
}

static void StartGrind(PlayState* play, Player* player, Input* input, const Vec3f& pos, s16 yaw) {
    if (sSkate.phase == PHASE_AIR) {
        FinishGrab();
        FinalizeSpin();
    }
    sSkate.phase = PHASE_GRIND;
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
    sSkate.speed += slopeAlong * Tune::kSlopeAccel;

    // Steering: tighter at low speed
    s16 turn = (s16)(-sx * Tune::kTurnRate * (1.2f - std::min(sSkate.speed, 12.0f) / 24.0f));
    sSkate.moveYaw += turn;

    if (sy > 0.3f && sSkate.speed < Tune::kCruiseMax * speedMult) {
        sSkate.speed += Tune::kCruiseAccel * sy * speedMult;
    } else if (sy < -0.3f) {
        Math_StepToF(&sSkate.speed, 0.0f, Tune::kBrake * -sy);
    }
    Math_StepToF(&sSkate.speed, 0.0f, Tune::kFriction + std::fabs(sx) * 0.02f);

    // Push
    if (CHECK_BTN_ALL(input->press.button, BTN_B) && sSkate.pushCooldown == 0 && !sSkate.charging) {
        if (sSkate.speed < Tune::kPushMax * speedMult) {
            sSkate.speed = std::min(sSkate.speed + Tune::kPushImpulse * speedMult, Tune::kPushMax * speedMult);
        }
        sSkate.pushCooldown = Tune::kPushCooldown;
        Player_PlaySfx(&player->actor, NA_SE_PL_WALK_GROUND + player->floorSfxOffset);
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
        PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
        SetAnim(play, player, ANIM_AIR_UP);
        return;
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

    if (!sSkate.charging) {
        if (sSkate.anim == ANIM_LAND) {
            if (player->skelAnime.curFrame >= player->skelAnime.endFrame) {
                SetAnim(play, player, ANIM_RIDE);
            }
        } else {
            SetAnim(play, player, ANIM_RIDE);
        }
    }

    if (Cfg_RollSound() && sSkate.speed > 1.5f) {
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
    PlaySfx(player, NA_SE_IT_SHIELD_BOUND);
    sSkate.anim = ANIM_NONE;
    SetAnim(play, player, ANIM_LAND);
}

static void UpdateAir(PlayState* play, Player* player, Input* input) {
    f32 sx = StickX(input);
    StickDir dir = GetStickDir(input);

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
    s16 baseBody = (s16)(sSkate.moveYaw + stanceSign * 0x4000 + (sSkate.fakie ? 0x8000 : 0));

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

    // Bleed speed if something (a wall, an actor) stopped us harder than we think
    sSkate.impactSpeed = sSkate.speed;
    f32 moved = Math_Vec3f_DistXZ(&player->actor.world.pos, &player->actor.prevPos);
    if (sSkate.phase != PHASE_GRIND && (player->actor.bgCheckFlags & BGCHECKFLAG_WALL) && moved + 1.5f < sSkate.speed) {
        sSkate.speed = std::max(moved, sSkate.speed * 0.6f);
    }

    switch (sSkate.phase) {
        case PHASE_GROUND:
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

    if (sSkate.msgTimer > 0) {
        sSkate.msgTimer--;
    }
    if (sSkate.toggleCooldown > 0) {
        sSkate.toggleCooldown--;
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
        if (leavingArea && Cfg_KeepBoard()) {
            sSkate.remountPending = true;
            sSkate.remountTimer = 100;
        } else if (!sSkate.bailPending && !sSkate.combo.Empty()) {
            ShowMsg("COMBO LOST", MSG_BAD, 30);
        }
        sSkate.combo.Clear();
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
    ClearRideState();
    sSkate.mountPending = false;
    sSkate.combo.Clear();
    sSkate.msgTimer = 0;
    // Hop back on in the next area (loading zones, doors, voids)
    sSkate.remountPending = (sSkate.remountPending || wasRiding) && Cfg_KeepBoard();
    sSkate.remountTimer = 100;
}

static void RegisterSkateboard() {
    bool enabled = SKATE_ENABLED;

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
