/**
 * Minish mod for Ship of Harkinian
 * --------------------------------
 * Press a button (D-Pad Up by default) and Link spins and shrinks down to Minish size. Press it again to grow back.
 *
 * While tiny, the world is huge:
 *  - Link's model, sword reach, hitbox and shadow all shrink with him.
 *  - His collision shrinks too: he squeezes through narrow gaps, swims in shallow water, and can't climb up
 *    ledges (they're far too tall for him). Two heights are fixed in the engine and don't shrink: he still steps
 *    up anything under ~26 units, and the floor check looks ~50 units above him.
 *  - He moves slower (relative to the world), but much faster for his size.
 *  - The camera shrinks with him so he stays the same size on screen.
 *  - He's too small to open doors or lift pots, rocks and bushes.
 *  - Falls don't hurt (option): he's light as a leaf.
 *
 * How it plugs into the game, without changing any other file:
 *  - Size: Link's actor scale (the game's own "Link size" code leaves it alone at normal size).
 *  - Collision and ledges: Link reads all his body measurements from one table (`ageProperties`). While tiny,
 *    he gets a shrunk copy of that table.
 *  - Speed: the game's run speed limit register, scaled while tiny (the game resets it when you change boots or
 *    enter water, so it's re-applied every frame).
 *  - Camera: the game's camera is built for a full-size Link. Right before the frame is drawn, the view is shrunk
 *    around Link's feet by the same factor, which is exactly what the camera would do for a smaller Link.
 *
 * Later: shrink spots (portals) and Minish-sized areas. See "Minish spots" near the bottom.
 */

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include "soh/SohGui/MenuTypes.h"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/SohGui.hpp"
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
// Settings
// ---------------------------------------------------------------------------------------------------------------------

#define CVAR_MINISH(name) CVAR_ENHANCEMENT("Minish." name)
#define CVAR_MINISH_ENABLED CVAR_MINISH("Enabled")
#define MINISH_ENABLED CVarGetInteger(CVAR_MINISH_ENABLED, 0)

static s32 Cfg_Button() {
    return CVarGetInteger(CVAR_MINISH("Btn"), BTN_DUP);
}
// Size while tiny, relative to normal (0.1 = a tenth of Link's height)
static f32 Cfg_Size() {
    return std::clamp(CVarGetFloat(CVAR_MINISH("Size"), 0.1f), 0.05f, 0.5f);
}
// Top speed while tiny, relative to Link's normal top speed
static f32 Cfg_Speed() {
    return std::clamp(CVarGetFloat(CVAR_MINISH("Speed"), 0.55f), 0.15f, 1.0f);
}
static bool Cfg_Camera() {
    return CVarGetInteger(CVAR_MINISH("Camera"), 1) != 0;
}
// 1 = the camera shrinks exactly as much as Link, bigger = it stays further back
static f32 Cfg_CameraDistance() {
    return std::clamp(CVarGetFloat(CVAR_MINISH("CameraDistance"), 1.0f), 0.5f, 3.0f);
}
static bool Cfg_FeatherFall() {
    return CVarGetInteger(CVAR_MINISH("FeatherFall"), 1) != 0;
}
static bool Cfg_ShowPosition() {
    return CVarGetInteger(CVAR_MINISH("ShowPosition"), 0) != 0;
}

namespace Tune {
constexpr f32 kNormalScale = 0.01f;   // Link's actor scale at normal size
constexpr s32 kTransformFrames = 20;  // one second
constexpr f32 kSpinTurns = 2.0f;      // full turns while changing size
constexpr f32 kNoLedge = 100000.0f;   // "this ledge is too tall to climb" for every ledge
constexpr f32 kMinWallRadius = 4.0f;  // don't shrink the wall check below this, so walls stay solid
constexpr s16 kMinCylinderRadius = 2; // hitbox radius floor
constexpr f32 kNormalZNear = 10.0f;   // the game's near clipping plane
constexpr f32 kMinZNear = 1.0f;
constexpr f32 kCamSmoothing = 0.25f;  // how fast the camera follows a size change
constexpr f32 kTargetCamBlend = 0.5f; // Z-targeting and talking cameras shrink only half as much, to keep the
                                      // other actor in view
} // namespace Tune

// ---------------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------------

enum MinishPhase { PHASE_NORMAL, PHASE_SHRINKING, PHASE_TINY, PHASE_GROWING };

struct MinishState {
    MinishPhase phase = PHASE_NORMAL;
    s32 timer = 0;
    f32 k = 1.0f;     // current size relative to normal (1 = normal size)
    f32 tinyK = 0.1f; // size when fully tiny (locked in when shrinking starts)
    s16 spinYaw = 0;  // facing when the spin started

    // What we changed on Link and in the game, so it can be put back exactly
    bool managing = false;
    s16 runLimitBase = 600;   // the game's own run speed limit
    s16 runLimitWritten = -1; // what we last wrote over it
    s16 cylinderRadiusBase = 12;

    // Camera
    f32 camK = 1.0f;
    bool viewTouched = false;
    bool haveLastView = false;
    Vec3f lastInEye{}, lastInAt{}, lastOutEye{}, lastOutAt{};

    // On-screen message
    std::string msg;
    s16 msgTimer = 0;
    s16 doorMsgCooldown = 0;
    s16 liftMsgCooldown = 0;
};

static MinishState sMinish;
static PlayerAgeProperties sTinyProps;            // Link's body measurements, shrunk
static PlayerAgeProperties* sOrigProps = nullptr; // the game's table we copied it from

static void ShowMsg(const std::string& text, s16 frames = 40) {
    sMinish.msg = text;
    sMinish.msgTimer = frames;
}

static f32 Lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

static f32 EaseInOut(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static bool IsTransforming() {
    return sMinish.phase == PHASE_SHRINKING || sMinish.phase == PHASE_GROWING;
}

// Ladders, vines and crawlspaces: the game puts Link back to normal size for these (they move him with animations
// that only line up at normal size). We go along with it and shrink him again once he's done.
static bool GameForcesNormalSize(Player* player) {
    return (player->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) || (player->stateFlags2 & PLAYER_STATE2_CRAWLING);
}

// ---------------------------------------------------------------------------------------------------------------------
// Applying the size
// ---------------------------------------------------------------------------------------------------------------------

static void BuildTinyProps(const PlayerAgeProperties& orig, f32 k) {
    sTinyProps = orig;
    sTinyProps.ceilingCheckHeight = orig.ceilingCheckHeight * k;
    sTinyProps.unk_0C = orig.unk_0C * k; // how high Link looks for a ledge in front of him

    // Ledges: every ledge is far too tall for a Minish to climb or hang from
    sTinyProps.unk_10 = Tune::kNoLedge;
    sTinyProps.unk_14 = Tune::kNoLedge;
    sTinyProps.unk_18 = Tune::kNoLedge;
    sTinyProps.unk_1C = Tune::kNoLedge;
    sTinyProps.unk_34 = Tune::kNoLedge; // don't grab the edge when walking off it

    // Water depths (wading, when to start swimming, how deep Link floats)
    sTinyProps.unk_24 = orig.unk_24 * k;
    sTinyProps.unk_28 = orig.unk_28 * k;
    sTinyProps.unk_2C = orig.unk_2C * k;
    sTinyProps.unk_30 = orig.unk_30 * k;

    // How far Link keeps from walls (lets him squeeze through gaps)
    sTinyProps.wallCheckRadius =
        std::max(orig.wallCheckRadius * k, std::min(orig.wallCheckRadius, Tune::kMinWallRadius));
}

static void ApplySize(Player* player, f32 k) {
    if (!sMinish.managing) {
        sMinish.managing = true;
        sMinish.cylinderRadiusBase = player->cylinder.dim.radius;
        sMinish.runLimitBase = R_RUN_SPEED_LIMIT;
        sMinish.runLimitWritten = -1;
    }

    // Model, sword reach, animations and shadow all follow the actor scale
    Actor_SetScale(&player->actor, Tune::kNormalScale * k);

    // Body measurements. The game gives Link a fresh table in every new area, so keep watching for it.
    if (player->ageProperties != &sTinyProps) {
        sOrigProps = player->ageProperties;
        sMinish.cylinderRadiusBase = player->cylinder.dim.radius;
    }
    if (sOrigProps != nullptr) {
        BuildTinyProps(*sOrigProps, k);
        player->ageProperties = &sTinyProps;
    }

    // Hitbox (its height already follows his body)
    player->cylinder.dim.radius =
        std::max<s16>(Tune::kMinCylinderRadius, static_cast<s16>(std::lround(sMinish.cylinderRadiusBase * k)));

    // Top speed. The game rewrites this when you change boots, enter water or a new area: take its new value.
    if (R_RUN_SPEED_LIMIT != sMinish.runLimitWritten) {
        sMinish.runLimitBase = R_RUN_SPEED_LIMIT;
    }
    f32 progress = (1.0f - k) / std::max(1.0f - sMinish.tinyK, 0.01f); // 0 at normal size, 1 when fully tiny
    f32 speedK = Lerp(1.0f, Cfg_Speed(), std::clamp(progress, 0.0f, 1.0f));
    sMinish.runLimitWritten =
        static_cast<s16>(std::max(1.0f, std::round(static_cast<f32>(sMinish.runLimitBase) * speedK)));
    R_RUN_SPEED_LIMIT = sMinish.runLimitWritten;

    // Light as a leaf: keep resetting where the fall started, so landing never counts as a big fall
    if (Cfg_FeatherFall() && !(player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && player->actor.velocity.y < 0.0f &&
        !(player->stateFlags1 & PLAYER_STATE1_IN_WATER)) {
        player->fallStartHeight = static_cast<s16>(player->actor.world.pos.y);
    }
}

// Put everything we changed back the way the game had it
static void RestoreSize(Player* player) {
    if (!sMinish.managing) {
        return;
    }
    if (player != nullptr) {
        if (player->ageProperties == &sTinyProps && sOrigProps != nullptr) {
            player->ageProperties = sOrigProps;
        }
        if (!GameForcesNormalSize(player) && GameInteractor_GetLinkSize() == GI_LINK_SIZE_NORMAL) {
            Actor_SetScale(&player->actor, Tune::kNormalScale);
        }
        player->cylinder.dim.radius = sMinish.cylinderRadiusBase;
    }
    if (R_RUN_SPEED_LIMIT == sMinish.runLimitWritten) {
        R_RUN_SPEED_LIMIT = sMinish.runLimitBase;
    }
    sMinish.runLimitWritten = -1;
    sMinish.managing = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shrinking and growing
// ---------------------------------------------------------------------------------------------------------------------

// nullptr = OK, "" = not now (no message), anything else = tell the player why
static const char* WhyCantChange(PlayState* play, Player* player) {
    if (play->pauseCtx.state != 0 || play->msgCtx.msgMode != MSGMODE_NONE || Player_InCsMode(play) ||
        play->transitionTrigger != TRANS_TRIGGER_OFF || GameInteractor_GetLinkSize() != GI_LINK_SIZE_NORMAL) {
        return "";
    }
    if (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) {
        return "Not while riding!";
    }
    if (player->stateFlags1 & PLAYER_STATE1_IN_WATER) {
        return "Not while swimming!";
    }
    if (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) {
        return "Put that down first!";
    }
    if (player->stateFlags1 &
        (PLAYER_STATE1_LOADING | PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD | PLAYER_STATE1_GETTING_ITEM |
         PLAYER_STATE1_CHARGING_SPIN_ATTACK | PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE |
         PLAYER_STATE1_FIRST_PERSON | PLAYER_STATE1_CLIMBING_LADDER | PLAYER_STATE1_DAMAGED | PLAYER_STATE1_IN_ITEM_CS |
         PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_FLOOR_DISABLED)) {
        return "";
    }
    if (player->stateFlags2 & (PLAYER_STATE2_CRAWLING | PLAYER_STATE2_FROZEN)) {
        return "";
    }
    if (!(player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) || player->meleeWeaponState != 0) {
        return "";
    }
    return nullptr;
}

// Is there room for full-size Link here? (ceiling above, and not wedged between two walls)
static bool RoomToGrow(PlayState* play, Player* player) {
    if (sOrigProps == nullptr) {
        return true;
    }
    CollisionContext* col = &play->colCtx;
    CollisionPoly* poly;
    s32 bgId;
    f32 ceilingY;
    Vec3f pos = player->actor.world.pos;
    if (BgCheck_EntityCheckCeiling(col, &ceilingY, &pos, sOrigProps->ceilingCheckHeight + 4.0f, &poly, &bgId,
                                   &player->actor)) {
        return false;
    }

    // Walls on two opposite sides closer than full-size Link is wide
    f32 radius = sOrigProps->wallCheckRadius;
    for (s32 i = 0; i < 4; i++) {
        s16 yaw = static_cast<s16>(i * 0x2000);
        f32 dx = Math_SinS(yaw) * radius;
        f32 dz = Math_CosS(yaw) * radius;
        Vec3f from = { pos.x, pos.y + 20.0f, pos.z };
        Vec3f a = { pos.x + dx, from.y, pos.z + dz };
        Vec3f b = { pos.x - dx, from.y, pos.z - dz };
        Vec3f hit;
        bool hitA = BgCheck_EntityLineTest1(col, &from, &a, &hit, &poly, true, false, false, true, &bgId);
        bool hitB = BgCheck_EntityLineTest1(col, &from, &b, &hit, &poly, true, false, false, true, &bgId);
        if (hitA && hitB) {
            return false;
        }
    }
    return true;
}

static void Sparkles(PlayState* play, Player* player, s32 count) {
    static Color_RGBA8 sPrim = { 255, 255, 220, 255 };
    static Color_RGBA8 sEnv = { 80, 220, 120, 255 };
    f32 size = std::max(sMinish.k, 0.35f);
    for (s32 i = 0; i < count; i++) {
        Vec3f pos = player->actor.world.pos;
        pos.x += Rand_CenteredFloat(40.0f * size);
        pos.y += Rand_ZeroOne() * 60.0f * size;
        pos.z += Rand_CenteredFloat(40.0f * size);
        Vec3f vel = { 0.0f, 0.6f, 0.0f };
        Vec3f accel = { 0.0f, 0.05f, 0.0f };
        EffectSsKiraKira_SpawnDispersed(play, &pos, &vel, &accel, &sPrim, &sEnv, static_cast<s16>(1200.0f * size), 14);
    }
}

static void StartShrink(PlayState* play, Player* player) {
    sMinish.phase = PHASE_SHRINKING;
    sMinish.timer = 0;
    sMinish.tinyK = Cfg_Size();
    sMinish.spinYaw = player->actor.shape.rot.y;
    Player_PlaySfx(&player->actor, NA_SE_EV_BUTTERFRY_TO_FAIRY);
    Sparkles(play, player, 8);
}

static void StartGrow(PlayState* play, Player* player) {
    sMinish.phase = PHASE_GROWING;
    sMinish.timer = 0;
    sMinish.spinYaw = player->actor.shape.rot.y;
    Player_PlaySfx(&player->actor, NA_SE_EV_FROG_GROW_UP);
    Sparkles(play, player, 8);
}

static void UpdateTransform(PlayState* play, Player* player) {
    sMinish.timer++;
    f32 t = static_cast<f32>(sMinish.timer) / static_cast<f32>(Tune::kTransformFrames);
    f32 e = EaseInOut(t);
    bool shrinking = sMinish.phase == PHASE_SHRINKING;
    sMinish.k = shrinking ? Lerp(1.0f, sMinish.tinyK, e) : Lerp(sMinish.tinyK, 1.0f, e);

    // Spin (shape only: where he's facing for movement and the camera doesn't change)
    s32 spin = static_cast<s32>(e * Tune::kSpinTurns * 65536.0f);
    player->actor.shape.rot.y = static_cast<s16>((sMinish.spinYaw + spin) & 0xFFFF);
    player->linearVelocity = 0.0f;

    if ((sMinish.timer & 1) == 0) {
        Sparkles(play, player, 2);
    }

    if (sMinish.timer >= Tune::kTransformFrames) {
        player->actor.shape.rot.y = player->yaw;
        Sparkles(play, player, 6);
        if (shrinking) {
            sMinish.k = sMinish.tinyK;
            sMinish.phase = PHASE_TINY;
            Player_PlaySfx(&player->actor, NA_SE_EV_FAIRY_DASH);
        } else {
            sMinish.k = 1.0f;
            sMinish.phase = PHASE_NORMAL;
            Player_PlaySfx(&player->actor, NA_SE_PL_MAGIC_SOUL_FLASH);
        }
    }
}

// Something took over (cutscene, damage knocked him out of it...) in the middle of a change: finish it instantly
static void FinishTransformNow(Player* player) {
    if (!IsTransforming()) {
        return;
    }
    player->actor.shape.rot.y = player->yaw;
    if (sMinish.phase == PHASE_SHRINKING) {
        sMinish.phase = PHASE_TINY;
        sMinish.k = sMinish.tinyK;
    } else {
        sMinish.phase = PHASE_NORMAL;
        sMinish.k = 1.0f;
    }
}

static void ResetState() {
    sMinish.phase = PHASE_NORMAL;
    sMinish.k = 1.0f;
    sMinish.timer = 0;
    sMinish.msgTimer = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Minish spots (for later)
// ---------------------------------------------------------------------------------------------------------------------
// The plan: special places (tree stumps, pots, holes in walls) where you can shrink, and Minish-sized areas to explore
// once you're small (built from custom collision, like the Skateboard mod's skate park).
// For now Link can shrink anywhere. This is the one place that decides it.
static bool CanShrinkHere(PlayState* play, Player* player) {
    (void)play;
    (void)player;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------------------------------

static void OnPlayerUpdate() {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        return;
    }

    if (sMinish.msgTimer > 0) {
        sMinish.msgTimer--;
    }
    if (sMinish.doorMsgCooldown > 0) {
        sMinish.doorMsgCooldown--;
    }
    if (sMinish.liftMsgCooldown > 0) {
        sMinish.liftMsgCooldown--;
    }

    // Button: shrink / grow
    Input* input = &play->state.input[0];
    s32 mask = Cfg_Button();
    if (mask != 0 && CHECK_BTN_ALL(input->cur.button, mask) && (input->press.button & mask) && !IsTransforming()) {
        const char* why = WhyCantChange(play, player);
        if (why != nullptr) {
            if (why[0] != '\0') {
                ShowMsg(why);
                Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
            }
        } else if (sMinish.phase == PHASE_NORMAL) {
            if (CanShrinkHere(play, player)) {
                StartShrink(play, player);
            } else {
                ShowMsg("You can't shrink here.");
                Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
            }
        } else if (!RoomToGrow(play, player)) {
            ShowMsg("No room to grow here!");
            Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        } else {
            StartGrow(play, player);
        }
    }

    // Riding Epona or the game taking control mid-change
    if (IsTransforming() && (player->stateFlags1 & (PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_DAMAGED |
                                                    PLAYER_STATE1_DEAD | PLAYER_STATE1_ON_HORSE))) {
        FinishTransformNow(player);
    }
    if (sMinish.phase == PHASE_TINY && (player->stateFlags1 & PLAYER_STATE1_ON_HORSE)) {
        // Too small to ride. Mounting happened anyway (calling Epona), so grow back for the ride.
        ResetState();
        ShowMsg("You grew back to ride Epona.");
    }

    if (IsTransforming()) {
        UpdateTransform(play, player);
    }

    if (sMinish.phase == PHASE_NORMAL) {
        RestoreSize(player);
    } else if (GameForcesNormalSize(player)) {
        RestoreSize(player); // just for the ladder / crawlspace
    } else {
        ApplySize(player, sMinish.k);
    }
}

// Runs right before Link's action each frame
static void BeforePlayerAction(Player* player, Input* input, bool* should) {
    if (IsTransforming()) {
        *should = false; // hold still while spinning
        player->linearVelocity = 0.0f;
        return;
    }
    if (sMinish.phase != PHASE_TINY || GameForcesNormalSize(player)) {
        return;
    }
    // Too small to lift pots, rocks, bushes, Cuccos... (items, chests and bottles still work)
    if (player->interactRangeActor != nullptr && player->getItemId == GI_NONE &&
        !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)) {
        if (input != nullptr && CHECK_BTN_ALL(input->press.button, BTN_A) && sMinish.liftMsgCooldown == 0) {
            ShowMsg("Too small to lift that!");
            sMinish.liftMsgCooldown = 30;
        }
        player->interactRangeActor = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------------------------------------------------

// Cameras that follow Link around (as opposed to fixed angles, drawn backgrounds, doors and cutscene cameras)
static bool IsFollowSetting(s16 setting) {
    switch (setting) {
        case CAM_SET_NORMAL0:
        case CAM_SET_NORMAL1:
        case CAM_SET_NORMAL2:
        case CAM_SET_NORMAL3:
        case CAM_SET_NORMAL4:
        case CAM_SET_DUNGEON0:
        case CAM_SET_DUNGEON1:
        case CAM_SET_DUNGEON2:
        case CAM_SET_BOSS_GOHMA:
        case CAM_SET_BOSS_DODONGO:
        case CAM_SET_BOSS_BARINADE:
        case CAM_SET_BOSS_PHANTOM_GANON:
        case CAM_SET_BOSS_VOLVAGIA:
        case CAM_SET_BOSS_BONGO:
        case CAM_SET_BOSS_MORPHA:
        case CAM_SET_BOSS_TWINROVA_PLATFORM:
        case CAM_SET_BOSS_TWINROVA_FLOOR:
        case CAM_SET_BOSS_GANONDORF:
        case CAM_SET_BOSS_GANON:
        case CAM_SET_TOWER_CLIMB:
        case CAM_SET_FREE0:
        case CAM_SET_BIG_OCTO:
        case CAM_SET_FISHING:
        case CAM_SET_JABU_TENTACLE:
        case CAM_SET_DIRECTED_YAW:
            return true;
        default:
            return false;
    }
}

// How much the camera should shrink for the camera that's running now. 0 = leave it alone.
static f32 CameraShrinkWeight(PlayState* play) {
    if (play->csCtx.state != CS_STATE_IDLE || play->activeCamera != 0) { // 0 = the main (player) camera
        return 0.0f;
    }
    Camera* cam = GET_ACTIVE_CAM(play);
    if (cam == nullptr || !IsFollowSetting(cam->setting)) {
        return 0.0f;
    }
    switch (cam->mode) {
        case CAM_MODE_NORMAL:
        case CAM_MODE_TARGET:
        case CAM_MODE_CLIMB:
        case CAM_MODE_FIRST_PERSON:
        case CAM_MODE_AIM_ADULT:
        case 0x08: // Z-target bow aiming (named differently in different versions)
        case CAM_MODE_HOOKSHOT:
        case CAM_MODE_AIM_BOOMERANG:
        case CAM_MODE_AIM_CHILD:
        case CAM_MODE_CLIMBZ:
        case CAM_MODE_JUMP:
        case CAM_MODE_HANG:
        case CAM_MODE_HANGZ:
        case CAM_MODE_FREEFALL:
        case CAM_MODE_CHARGE:
        case CAM_MODE_STILL:
        case CAM_MODE_PUSHPULL:
            return 1.0f; // follow, parallel (Z), first person, aiming, jumping
        case CAM_MODE_FOLLOWTARGET:
        case CAM_MODE_TALK:
        case CAM_MODE_BATTLE:
        case CAM_MODE_FOLLOWBOOMERANG:
            return Tune::kTargetCamBlend; // locked on to something / talking: keep the other actor in view
        default:
            return 0.0f;
    }
}

static void RestoreView(PlayState* play) {
    if (sMinish.viewTouched) {
        play->view.zNear = Tune::kNormalZNear;
        sMinish.viewTouched = false;
    }
    sMinish.haveLastView = false;
}

static bool SameVec(const Vec3f& a, const Vec3f& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static void OnDrawBegin() {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        RestoreView(play);
        return;
    }

    f32 weight = CameraShrinkWeight(play);
    f32 target = 1.0f;
    if (Cfg_Camera() && sMinish.phase != PHASE_NORMAL && !GameForcesNormalSize(player)) {
        f32 tiny = std::clamp(sMinish.k * Cfg_CameraDistance(), sMinish.k, 1.0f);
        target = Lerp(1.0f, tiny, weight);
    }

    if (weight <= 0.0f) {
        // Not a camera we touch: snap, so there's no drift when we get control back
        sMinish.camK = target;
        RestoreView(play);
        return;
    }

    if (IsTransforming()) {
        sMinish.camK = target; // the size change is already smooth
    } else {
        sMinish.camK += (target - sMinish.camK) * Tune::kCamSmoothing;
        if (std::fabs(target - sMinish.camK) < 0.001f) {
            sMinish.camK = target;
        }
    }
    f32 f = sMinish.camK;
    if (f >= 0.999f) {
        RestoreView(play);
        return;
    }

    View* view = &play->view;
    // While paused the camera doesn't update, so the view still holds what we wrote last frame
    Vec3f inEye = view->eye;
    Vec3f inAt = view->lookAt;
    if (sMinish.haveLastView && SameVec(inEye, sMinish.lastOutEye) && SameVec(inAt, sMinish.lastOutAt)) {
        inEye = sMinish.lastInEye;
        inAt = sMinish.lastInAt;
    }

    // Shrink the whole camera rig around Link's feet
    Vec3f c = player->actor.world.pos;
    Vec3f at = { c.x + (inAt.x - c.x) * f, c.y + (inAt.y - c.y) * f, c.z + (inAt.z - c.z) * f };
    Vec3f eye = { c.x + (inEye.x - c.x) * f, c.y + (inEye.y - c.y) * f, c.z + (inEye.z - c.z) * f };

    // Don't let the closer camera end up inside a rock or the ground between it and Link
    Vec3f hit;
    CollisionPoly* poly;
    s32 bgId;
    if (BgCheck_CameraLineTest1(&play->colCtx, &at, &eye, &hit, &poly, true, true, true, true, &bgId)) {
        Vec3f d = { at.x - hit.x, at.y - hit.y, at.z - hit.z };
        f32 len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len > 0.001f) {
            f32 push = std::min(len * 0.5f, std::max(2.0f, 20.0f * f));
            eye = { hit.x + d.x / len * push, hit.y + d.y / len * push, hit.z + d.z / len * push };
        }
    }

    view->eye = eye;
    view->lookAt = at;
    view->zNear = std::clamp(Tune::kNormalZNear * f, Tune::kMinZNear, Tune::kNormalZNear);
    sMinish.viewTouched = true;

    sMinish.haveLastView = true;
    sMinish.lastInEye = inEye;
    sMinish.lastInAt = inAt;
    sMinish.lastOutEye = eye;
    sMinish.lastOutAt = at;
}

// ---------------------------------------------------------------------------------------------------------------------
// HUD: messages and (optionally) where you are, for planning Minish spots
// ---------------------------------------------------------------------------------------------------------------------

static std::string PositionText(PlayState* play, Player* player) {
    char buf[96];
    snprintf(buf, sizeof(buf), "SCENE %d ROOM %d  X %d Y %d Z %d", play->sceneNum, play->roomCtx.curRoom.num,
             static_cast<s32>(player->actor.world.pos.x), static_cast<s32>(player->actor.world.pos.y),
             static_cast<s32>(player->actor.world.pos.z));
    return buf;
}

static void HudPrintCentered(GfxPrint* printer, s32 row, const std::string& text) {
    std::string clipped = text.size() > 38 ? text.substr(0, 38) : text;
    GfxPrint_SetPos(printer, std::max<s32>(0, (40 - static_cast<s32>(clipped.size())) / 2), row);
    GfxPrint_Printf(printer, "%s", clipped.c_str());
}

static void OnDrawEnd() {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(play);
    bool showPos = Cfg_ShowPosition() && player != nullptr;
    if (sMinish.msgTimer <= 0 && !showPos) {
        return;
    }

    OPEN_DISPS(play->state.gfxCtx);

    GfxPrint printer;
    Gfx* polyOpa = POLY_OPA_DISP;
    Gfx* gfx = Graph_GfxPlusOne(polyOpa);
    gSPDisplayList(OVERLAY_DISP++, gfx);

    GfxPrint_Init(&printer);
    GfxPrint_Open(&printer, gfx);
    if (sMinish.msgTimer > 0) {
        GfxPrint_SetColor(&printer, 150, 255, 170, 255);
        HudPrintCentered(&printer, 22, sMinish.msg);
    }
    if (showPos) {
        GfxPrint_SetColor(&printer, 200, 200, 200, 255);
        HudPrintCentered(&printer, 28, PositionText(play, player));
    }
    gfx = GfxPrint_Close(&printer);
    GfxPrint_Destroy(&printer);

    gSPEndDisplayList(gfx++);
    Graph_BranchDlist(polyOpa, gfx);
    POLY_OPA_DISP = gfx;

    CLOSE_DISPS(play->state.gfxCtx);
}

static void OnPlayDestroy() {
    // The next area starts with a fresh view and a fresh Link; the size gets re-applied on his first update
    sMinish.viewTouched = false;
    sMinish.haveLastView = false;
    if (gPlayState != nullptr) {
        Player* player = GET_PLAYER(gPlayState);
        if (player != nullptr) {
            FinishTransformNow(player);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------------------------------

static void RegisterMinish() {
    bool enabled = MINISH_ENABLED;

    if (!enabled) {
        // Turned off while tiny: put Link back to normal
        Player* player = (gPlayState != nullptr) ? GET_PLAYER(gPlayState) : nullptr;
        if (player != nullptr && IsTransforming()) {
            player->actor.shape.rot.y = player->yaw;
        }
        ResetState();
        RestoreSize(player);
        if (gPlayState != nullptr) {
            RestoreView(gPlayState);
        }
    }

    COND_VB_SHOULD(VB_EXECUTE_PLAYER_ACTION_FUNC, enabled, {
        Player* player = va_arg(args, Player*);
        Input* input = va_arg(args, Input*);
        BeforePlayerAction(player, input, should);
    });

    // Too small to open doors
    COND_VB_SHOULD(VB_BE_ABLE_TO_OPEN_DOORS, enabled, {
        if (sMinish.phase != PHASE_NORMAL && gPlayState != nullptr && !GameForcesNormalSize(GET_PLAYER(gPlayState))) {
            *should = false;
            if (sMinish.doorMsgCooldown == 0) {
                ShowMsg("Too small to open doors!");
                sMinish.doorMsgCooldown = 30;
            }
        }
    });

    COND_HOOK(OnPlayerUpdate, enabled, OnPlayerUpdate);
    COND_HOOK(OnPlayDrawBegin, enabled, OnDrawBegin);
    COND_HOOK(OnPlayDrawEnd, enabled, OnDrawEnd);
    COND_HOOK(OnPlayDestroy, enabled, OnPlayDestroy);
    COND_HOOK(OnLoadGame, enabled, [](int32_t) { ResetState(); });
    COND_HOOK(OnExitGame, enabled, [](int32_t) {
        ResetState();
        sMinish.managing = false; // the game state is gone, nothing left to restore
        if (R_RUN_SPEED_LIMIT == sMinish.runLimitWritten) {
            R_RUN_SPEED_LIMIT = sMinish.runLimitBase;
        }
        sMinish.runLimitWritten = -1;
    });
}

static RegisterShipInitFunc initFunc(RegisterMinish, { CVAR_MINISH_ENABLED });

// ---------------------------------------------------------------------------------------------------------------------
// Menu: Enhancements > Extra Modes > Minish
// ---------------------------------------------------------------------------------------------------------------------

static void RegisterMinishWidgets() {
    WidgetPath path = { "Enhancements", "Extra Modes", SECTION_COLUMN_3 };
    auto disabledIfOff = [](WidgetInfo& info) { info.options->disabled = !MINISH_ENABLED; };

    SohGui::mSohMenu->AddWidget(path, "Minish", WIDGET_SEPARATOR_TEXT);

    SohGui::mSohMenu->AddWidget(path, "Enable Minish", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_MINISH_ENABLED)
        .Options(UIWidgets::CheckboxOptions().Tooltip(
            "Shrink Link down to Minish size with a button press (D-Pad Up by default), press it again to grow "
            "back.\n\n"
            "While tiny, Link squeezes through narrow gaps, swims in shallow water,\n"
            "and the camera shrinks with him. He's too small to open doors, lift things or climb ledges."));

    SohGui::mSohMenu->AddWidget(path, "Shrink / Grow Button:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar(CVAR_MINISH("Btn"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::BtnSelectorOptions().DefaultValue(BTN_DUP).Tooltip(
            "Button (or combination) that shrinks Link and grows him back."));

    SohGui::mSohMenu->AddWidget(path, "Minish Size: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_MINISH("Size"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::FloatSliderOptions().Min(0.05f).Max(0.5f).DefaultValue(0.1f).Format("%.2fx").Tooltip(
            "How small Link gets. 0.10 = a tenth of his height. Takes effect the next time you shrink."));

    SohGui::mSohMenu->AddWidget(path, "Minish Speed: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_MINISH("Speed"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::FloatSliderOptions().Min(0.15f).Max(1.0f).DefaultValue(0.55f).Format("%.2fx").Tooltip(
            "Top speed while tiny, compared to normal. Still very fast for his size!"));

    SohGui::mSohMenu->AddWidget(path, "Shrink the Camera Too", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_MINISH("Camera"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
            "The camera follows Link down to his size, so the world looks huge.\n"
            "Rooms with drawn backgrounds (houses, shops, Castle Town market) keep their fixed camera."));

    SohGui::mSohMenu->AddWidget(path, "Camera Distance: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_MINISH("CameraDistance"))
        .PreFunc([](WidgetInfo& info) { info.options->disabled = !MINISH_ENABLED || !Cfg_Camera(); })
        .Options(UIWidgets::FloatSliderOptions().Min(0.5f).Max(3.0f).DefaultValue(1.0f).Format("%.2fx").Tooltip(
            "1.00 = the camera keeps Link the same size on screen as normal. Higher = further back."));

    SohGui::mSohMenu->AddWidget(path, "Feather Fall While Tiny", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_MINISH("FeatherFall"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip("Falls never hurt while you're tiny."));

    SohGui::mSohMenu->AddWidget(path, "Show My Position", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_MINISH("ShowPosition"))
        .PreFunc(disabledIfOff)
        .Options(UIWidgets::CheckboxOptions().Tooltip(
            "Shows the area number and Link's position at the bottom of the screen.\n"
            "Handy for picking spots for Minish portals and Minish areas."));

    SohGui::mSohMenu->AddWidget(path, "Copy My Position", WIDGET_BUTTON)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !MINISH_ENABLED;
            info.options->disabled = gPlayState == nullptr;
            info.options->disabledTooltip = "Load a save file first.";
        })
        .Callback([](WidgetInfo&) {
            if (gPlayState == nullptr) {
                return;
            }
            Player* player = GET_PLAYER(gPlayState);
            if (player == nullptr) {
                return;
            }
            std::string text = SohUtils::GetSceneName(gPlayState->sceneNum) + " - " + PositionText(gPlayState, player) +
                               " FACING " + std::to_string(player->actor.shape.rot.y);
            ImGui::SetClipboardText(text.c_str());
            ShowMsg("Position copied!");
        });
}

static RegisterMenuInitFunc menuInitFunc(RegisterMinishWidgets);
