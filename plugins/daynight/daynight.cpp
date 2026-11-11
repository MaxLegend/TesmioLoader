// daynight - one sunrise and one sunset per calendar day, as a tesmioloader
// plugin.
//
// THE BASE GAME RUNS THE TWO CLOCKS AT DIFFERENT SPEEDS
//
// The world object carries the calendar in two fields:
//
//   game+0x590   int    day of the year, 0..364
//   game+0x594   int    the year
//   game+0x59C   float  how far into the current day we are, 0..60
//
// and the day tick at 0x3346E0 is the whole of it: it adds
// `C3D_TIMER::PowerTime` to +0x59C every frame and, the moment that passes the
// 60.0f at 0x90AA78 (this build), zeroes it, increments +0x590 and wraps at
// 365 into +0x594. One calendar day is therefore 60 units of scaled game time
// and nothing else.
//
// The day and night themselves come from a completely separate state machine at
// 0x333BD0, and it steps once per **calendar day**:
//
//   phase = (day of year) % 13
//
//     0..5, 11, 12   daylight, with whatever weather the random roll picked
//     6              weathers/sunset2.ini
//     7              weathers/night.ini, night factor ramping 0 -> 1
//     8              weathers/night.ini, full night
//     9              weathers/sunset2.ini, night factor ramping 1 -> 0
//     10             weathers/day1.ini, forced clear
//
// so a full day/night cycle takes **thirteen in-game days**, and the night part
// of it lasts two of them. That is the desynchronisation: the date in the
// corner advances thirteen times for every sunrise.
//
//
// EXACTLY THREE PLACES DIVIDE BY 13, AND ONLY TWO OF THEM ARE THE WORLD'S
//
// Scanning .text for the magic constant 0x4EC4EC4F - what the compiler emits
// for a signed `% 13` - finds three sites and no more:
//
//   in the weather state machine at 0x333BD0. Reads game+0x590
//   in the light factor at 0x5CFC40. Reads game+0x590
//   in the lighting cross-fade at 0x8F50. Reads 0x9D54A0, which is
//   **not** the world object - a separate clock. Left alone.
//
// The light factor is the second copy of the same 13-phase logic, written out
// again. It returns a float - how lit the world is - and is read by the
// renderer and by two of the electricity functions. Patching only the state
// machine would have moved the sky and left the street lights on the old
// thirteen-day schedule.
//
//
// WHAT THIS PLUGIN DOES: IT LIES TO BOTH OF THEM ABOUT THE DATE
//
// Nothing is patched. Both functions are hooked, and each is handed a **day and
// a time-in-day re-derived from the position inside the current calendar day**
// rather than from the calendar itself:
//
//   u  = (day mod cycle_days) * 60 + time, over cycle_days * 60      0 .. 1
//   k  = floor(u * 13)                        which of the 13 phases  0 .. 12
//   t' = (u * 13 - k) * 60                    where inside it         0 .. 60
//   d' = a day congruent to k modulo 13, near the real one
//
// With `cycle_days = 13` this is the identity: k is day % 13, t' is t, d' is
// day. **Installing the plugin and leaving it at 13 would change nothing at
// all**; the default is 1, which is the synchronisation the plugin exists for.
//
// The two hooks differ in how the lie is delivered, and deliberately:
//
//   weather tick   WRITES to the world object - the weather index, the night
//                  factor and two flags - so it has to be handed the real
//                  object. The two fields are overwritten, the original is
//                  called, and they are put back. It runs on the simulation
//                  tick, immediately after the day tick that owns those
//                  fields, so nothing else is looking at them in between.
//
//   light factor   READS and returns a float; it writes nothing. Every object
//                  field it touches was read out of its disassembly -
//                  +0x5C4, +0x590, +0x59C, +0xE70, +0xE28, and no others - so
//                  it is handed a **scratch object of this plugin's own** with
//                  those five fields filled in. The world object is never
//                  touched, which is what makes it safe from the render
//                  thread.
//
// D' IS PICKED SO THAT IT LANDS IN THE SAME SEASON AS THE REAL DATE, and that
// is not decoration. The weather tick does not only take `day % 13` off the
// field: it also calls the snow-season test with it, and compares it against
// the winter boundaries per climate. A residue class modulo 13 has a
// representative every thirteen days, so taking "the next day at or after the
// real one" sweeps a thirteen-day window over the course of a single calendar
// day - and near a winter boundary that window straddles it, so the weather
// would flicker in and out of winter thirteen times a day. This plugin instead
// walks the representatives outward from the real date and takes the first one
// the game's own season test puts in the same season. Every season is at least
// a hundred days long, so one always exists.
//
//
// AND THE CYCLE THEN RUNS THIRTEEN TIMES TOO FAST, WHICH IS THE OTHER HALF
//
// Compressing the cycle into one calendar day does not slow the calendar, so
// the cycle comes out thirteen times faster than the base game's. A calendar
// day is 60 units of scaled game time, which is a few seconds of real time, so
// the whole thirteen-step cycle goes by in that. It reads as "night tried to
// start and changed its mind", because that is exactly what it looks like.
//
// `day_scale` is what makes a calendar day last longer. Two versions have been
// through this and got it wrong in opposite directions:
//
//   Version 1.x stretched the CALENDAR ALONE by giving back most of what the
//   day tick had just added to +0x59C. The date really did slow, and nothing
//   else did: production, consumption, movement, everything is integrated per
//   frame from C3D_TIMER::PowerTime, while pollution decay, loan repayment and
//   notification expiry are called once per calendar day, from the day tick's
//   own rollover branch and nowhere else. Slowing only the calendar made those
//   three fire `day_scale` times less often than the per-frame systems they
//   balance against - pollution never settled, loans never repaid, and the
//   population starved in a winter that both ran away and lasted too long.
//   See docs/07-pitfalls.md, "Slowing one clock in a game that keeps two".
//
//   Version 2.0 fixed that by scaling the WHOLE simulation clock - the shared
//   C3D_TIMER at 0x9D4EE0 that PowerTime/Power/PowerKmh all divide by - so
//   every per-day and every per-frame quantity slowed together and the economy
//   stayed exactly the base game's, only slower in real time. That is honest,
//   but it also slows walking, construction and vehicle speed by the same
//   factor, and at the default 13x that reads as the whole world stuck in
//   slow motion - reported after the fact, in game, as citizens walking like
//   they are underwater and vehicles crawling slower than pedestrians.
//
// VERSION 2.1 GOES BACK TO SLOWING THE CALENDAR ALONE, AND FIXES THE THING
// THAT MADE THAT WRONG THE FIRST TIME. `slow = calendar` is the default again:
// nothing but +0x59C is touched, so C3D_TIMER and everything that reads it -
// walking, vehicles, construction, animation, production - keeps the base
// game's own real-time pace. What changes is that the three functions that
// broke under version 1.x are no longer left to the day tick's own, now-rare,
// rollover:
//
//   pollution   0x4D5F50   decay the grid, accumulate residential exposure
//   loans       0x4B9660   one day of interest and repayment, drop the paid
//   notices     0x4CD380   age the notification/offer list, drop the expired
//
// This plugin calls all three itself, directly, at the cadence the UNSLOWED
// calendar would have used - tracked in a private accumulator fed by the true
// PowerTime delta the day tick adds each frame, independent of what gets left
// in +0x59C for display. So pollution decays, loans get paid and notices
// expire at the base game's own real-time rate, while the date, the day/night
// cycle and everything keyed to a specific day of the year - the weather roll,
// the price recompute, the per-city tick - keep the deliberately slowed pace
// that makes a sunrise and a sunset visible at all. `slow = world` (version
// 2.0's behaviour) is kept for anyone who wants the uniform, honest-but-slow
// alternative; `slow = none` turns day_scale off.
//
// The lighting cross-fade duration at game+0xED4 is the one number that does
// not scale itself. 0x333F80-equivalent writes 15.0f into it on every weather
// change - a quarter of a calendar day. Its own clock is the timer at
// 0xA558A0, not the simulation one, so it is not slowed by any mode here and
// the fade is rewritten to the same fraction of the new phase after every
// tick.
//
// WHAT IS NOT FIXED, AND IS THE BASE GAME'S: the weather-change code allocates
// two C3D_LIGHTING objects on every change and never frees the previous pair.
// Four changes per cycle used to mean four per thirteen days; it now means
// four per day. Roughly 5 KB of leak per real minute at 1x speed - the
// textures behind them come from the managed cache and are not duplicated.
//
// Every address is SOVIET64.exe v1.1.1.9 and is verified byte for byte before
// anything is hooked or called. See docs/15-daynight.md.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- the sites

// The weather and day/night state machine. Called once per tick with the world
// object in rcx and a "this is a fresh world" flag in dl.
#define RVA_WEATHER_TICK  0x333B50   // v1.1.1.13; was 0x333BD0

// How lit the world is, 0..1. Pure, reads five fields of the world object and
// returns a float in xmm0.
#define RVA_LIGHT_FACTOR  0x5CFBD0   // v1.1.1.13; was 0x5CFC40

// `char IsSnowSeason(world)` - reads world+0x590 and the climate out of the
// settings object, and answers whether that day of the year is in the snow
// part of it. Called by the weather tick, which is why the shadowed date has
// to agree with the real one about it.
#define RVA_SNOW_SEASON   0x334360   // v1.1.1.13; was 0x3343E0

// The C3D_TIMER every simulation rate in the game is integrated from. Only
// touched in `slow = world` mode.
#define RVA_SIM_TIMER     0x9D4EE0

// In the day tick: `lea rcx,[sim timer]` / `xor r9d,r9d` / `xor r8d,r8d` /
// `call qword ptr [PowerTime]`. Verified as a unit, because between them those
// bytes prove that 0x9D4EE0 is a C3D_TIMER and that this call is on it -
// needed only to confirm `slow = world` is scaling the right object.
#define RVA_DAYTICK_POWER 0x334A20   // v1.1.1.13; was 0x334AA0

// The three per-calendar-day passes that integrate real elapsed time rather
// than just bookkeeping the date, and so are the ones that desynchronise from
// the per-frame systems they balance when the calendar alone is slowed. Each
// takes the world object and nothing else, each is called from the day tick's
// rollover branch and from nowhere else, and each is pure bookkeeping over
// state that already exists - so calling it again is exactly "another day's
// worth happened", with no event, allocation or notification invented that the
// game itself would not eventually have made. Resolved and called directly,
// like the snow-season test - never hooked, because nothing here needs to
// intercept the day tick's own (rarer, but still genuine) calls to them.
#define RVA_DAILY_POLLUTION 0x4D5EE0  // v1.1.1.13; was 0x4D5F50.  decay the grid, accumulate exposure
#define RVA_DAILY_LOANS     0x4B95F0  // v1.1.1.13; was 0x4B9660.  one day of interest and repayment
#define RVA_DAILY_NOTICES   0x4CD310  // v1.1.1.13; was 0x4CD380.  age the notification list, drop the expired

// The 60.0f one calendar day lasts, as the day tick reads it.
#define RVA_DAY_LENGTH    0x90AA88   // v1.1.1.13; was 0x90AA78
#define VANILLA_DAY_LEN   60.0f

// `mov dword ptr [r10+0xED4], 0x41700000` - the cross-fade duration written on
// every weather change. The opcode is checked and the immediate is read rather
// than assumed.
#define RVA_FADE_MOV      0x3341A4   // v1.1.1.13; was 0x334224
#define RVA_FADE_IMM      (RVA_FADE_MOV + 7)
#define VANILLA_FADE      15.0f

// The three C3D_TIMER::Power* the executable imports. All three have the same
// shape and the same two frame-rate fields; the game uses PowerTime for time,
// Power for a plain rate and PowerKmh for vehicle speeds, so scaling one and
// not the others would leave the traffic running at full speed through a slow
// world. Only touched in `slow = world` mode.
#define SYM_POWER         "?Power@C3D_TIMER@@QEAAMM_N0@Z"
#define SYM_POWERTIME     "?PowerTime@C3D_TIMER@@QEAAMM_N0@Z"
#define SYM_POWERKMH      "?PowerKmh@C3D_TIMER@@QEAAMM_N0@Z"

// World object fields. Everything here was read off the disassembly of the two
// hooked functions and the day tick.
#define OFF_WEATHER_ON    0x5C4   // int   weather simulated at all
#define OFF_DAY           0x590   // int   day of the year, 0..364
#define OFF_YEAR          0x594   // int
#define OFF_DAYTIME       0x59C   // float 0..60 into the current day
#define OFF_SNOWING       0xE28   // int   read by the light factor
#define OFF_WEATHER       0xE70   // int   current weathers/*.ini
#define OFF_NIGHT         0xE74   // float night factor, 0..1
#define OFF_FADE_ELAPSED  0xED0   // float how far into a lighting cross-fade
#define OFF_FADE_LEN      0xED4   // float how long a lighting cross-fade takes

// The highest object offset the light factor reads, rounded up. Its scratch
// copy has to be at least this big.
#define LIGHT_SCRATCH     0xE30

// The snow-season test reads one field of its argument, +0x590.
#define SEASON_SCRATCH    0x5A0

// The number of phases in the game's own cycle. It is not a setting: it is how
// many cases the weather tick and the light factor are written with.
#define PHASES            13

#define DAYS_IN_YEAR      365

// The most catch-up daily passes one tick may run, so a long stall (alt-tab, a
// debugger break) cannot turn into a burst of hundreds of calls on the frame
// after.
#define MAX_CATCHUP       64

static const BYTE kWeatherTickPrologue[] = {
    0x40, 0x53,                                  // push rbx
    0x41, 0x55,                                  // push r13
    0x48, 0x83, 0xEC, 0x58,                      // sub  rsp,0x58
    0x83, 0xB9, 0xC4, 0x05, 0x00, 0x00, 0x00     // cmp  dword ptr [rcx+0x5C4],0
};

static const BYTE kLightFactorPrologue[] = {
    0x40, 0x53,                                  // push rbx
    0x48, 0x83, 0xEC, 0x50,                      // sub  rsp,0x50
    0x83, 0xB9, 0xC4, 0x05, 0x00, 0x00, 0x00,    // cmp  dword ptr [rcx+0x5C4],0
    0x48, 0x8B, 0xD9                             // mov  rbx,rcx
};

// `mov dword ptr [r10+0xED4], imm32`
static const BYTE kFadeMov[] = { 0x41, 0xC7, 0x82, 0xD4, 0x0E, 0x00, 0x00 };

// The snow-season test, minus the displacement of its one rip-relative load.
static const BYTE kSnowHead[]  = { 0x48, 0x8B, 0x05 };                     // mov rax,[rip+X]
static const BYTE kSnowTail[]  = { 0x48, 0x8B, 0x90, 0xD8, 0x0E, 0x00, 0x00,   // mov rdx,[rax+0xED8]
                                   0x44, 0x8B, 0x82, 0xEC, 0x08, 0x00, 0x00 }; // mov r8d,[rdx+0x8EC]

// The three daily passes, verified by prologue before being trusted as
// function pointers. `cmp [rcx+0x5D4],0` in the pollution one is the same
// "is pollution simulated at all" gate the emitter and sampler use - calling
// it when the setting is off is already a no-op inside the function itself.
static const BYTE kDailyPollutionSig[] = {
    0x4C, 0x8B, 0xDC,                                        // mov  r11,rsp
    0x57,                                                     // push rdi
    0x48, 0x83, 0xEC, 0x70,                                  // sub  rsp,0x70
    0x83, 0xB9, 0xD4, 0x05, 0x00, 0x00, 0x00,                // cmp  dword ptr [rcx+0x5D4],0
    0x48, 0x8B, 0xF9                                         // mov  rdi,rcx
};
static const BYTE kDailyLoansSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,                            // mov  [rsp+0x10],rbx
    0x57,                                                     // push rdi
    0x48, 0x83, 0xEC, 0x30,                                  // sub  rsp,0x30
    0x48, 0x8B, 0x91, 0xD8, 0x0B, 0x01, 0x00                 // mov  rdx,[rcx+0x10BD8]
};
static const BYTE kDailyNoticesSig[] = {
    0x48, 0x89, 0x74, 0x24, 0x18,                            // mov  [rsp+0x18],rsi
    0x57,                                                     // push rdi
    0x48, 0x83, 0xEC, 0x20,                                  // sub  rsp,0x20
    0x48, 0x8B, 0xB1, 0x50, 0xD1, 0x00, 0x00                 // mov  rsi,[rcx+0xD150]
};

// ---------------------------------------------------------------- settings

// What `day_scale` stretches - see daynight.ini for the full case. SLOW_CALENDAR
// is the default: only +0x59C is touched, and the three daily-pass functions
// above are driven manually to keep them at the un-slowed cadence. SLOW_WORLD
// is 2.0's approach - the shared C3D_TIMER, scaling literally everything the
// simulation runs off alike - kept for anyone who wants that trade-off on
// purpose. SLOW_NONE is `day_scale = 1`, no stretch at all.
enum { SLOW_NONE = 0, SLOW_CALENDAR = 1, SLOW_WORLD = 2 };

static int   g_enabled    = 1;
static int   g_cycleDays  = 1;          // PHASES is the base game exactly
static float g_offset     = 0.0f;       // rotates the cycle, 0..1 of a cycle
static int   g_fade       = 1;
static int   g_probe      = 0;
static int   g_slow       = SLOW_CALENDAR;

// How many times longer a calendar day lasts than the base game's. 0 means
// `auto`: PHASES / cycle_days, which is the value that leaves one whole
// day/night cycle taking exactly as long in real time as the base game's did.
static float g_dayScale   = 0.0f;

static float g_dayLen     = VANILLA_DAY_LEN;
static float g_fadeLen    = VANILLA_FADE;

// Edge detection for "this is the first tick of a new phase". The base game
// gets that for free - a phase is a day, and the day tick leaves +0x59C at
// exactly 0.0f - and the weather roll and the overcast timer both key on it.
static int   g_lastPhase  = -1;

// What the day timer was left at last tick, for SLOW_CALENDAR: both for
// shrinking what gets displayed and for recovering the true, un-slowed delta
// the day tick added this frame. See SlowTheCalendar.
static float g_written    = -1.0f;

static unsigned  g_ticks       = 0;

// Probe bookkeeping: real milliseconds per phase, per weather change and per
// calendar day, which is the only way to tell "too fast to see" from "not
// happening".
static ULONGLONG g_phaseAt     = 0;
static ULONGLONG g_dayAt       = 0;
static ULONGLONG g_weatherAt   = 0;
static int       g_lastWeather = -1;
static int       g_lastDay     = -1;
static int       g_phasesInDay = 0;

typedef void  (*t_WeatherTick)(void* world, char fresh);
typedef float (*t_LightFactor)(void* world);
typedef char  (*t_SnowSeason)(void* world);
typedef void  (*t_DailyPass)(void* world);

// float C3D_TIMER::Power*(float v, bool ignorePause, bool realTime) - rcx is
// `this`, xmm1 the value, r8b and r9b the two flags. One shape for all three.
typedef float (*t_Power)(void* self, float v, char ignorePause, char realTime);

static t_WeatherTick o_WeatherTick;
static t_LightFactor o_LightFactor;
static t_SnowSeason  g_SnowSeason;      // called, never hooked

static t_Power o_Power;
static t_Power o_PowerTime;
static t_Power o_PowerKmh;

// The one timer the simulation is integrated from, and what its game-time
// answers are divided by. Both are written once, before any hook is installed.
static void*  g_simTimer = 0;

// `slow = world`'s own contribution - day_scale when that mode is active, 1.0
// otherwise. Kept separate from vehicle_scale/sim_scale below because the two
// compose: `slow = world` plus a vehicle_scale of your own multiplies them,
// which is exactly "start from the uniform trade-off, then correct vehicles
// further" rather than two competing sources of truth.
static float  g_simDiv      = 1.0f;

// Independent of `slow` and `day_scale` entirely, and always available: two
// separate multipliers over the same three C3D_TIMER::Power* the clock-scale
// mechanism already hooks. `PowerKmh` is used for nothing but converting a
// vehicle's speed into distance, at every site checked - vehicle+0x1708 or an
// adjacent vehicle-record field appears at every call - so it is a clean,
// single-purpose lever. `Power` and `PowerTime` are not: between them they
// cover camera zoom, UI axes (all with realTime=true and therefore already
// excluded), building rotation, and - the part this exists for - walking,
// construction and production, all sharing the same two imports with no
// narrower distinction the executable itself draws. So `sim_scale` is
// necessarily "everything but vehicles and the calendar" as one dial, not
// "just walking" - see daynight.ini for the full explanation of why a finer
// split was not attempted.
static float g_vehicleScale = 1.0f;
static float g_simScale     = 1.0f;

// The three daily passes - resolved and called directly, like g_SnowSeason.
// Only meaningful in `slow = calendar`.
static t_DailyPass g_DailyPollution;
static t_DailyPass g_DailyLoans;
static t_DailyPass g_DailyNotices;

// The true, un-slowed time accumulated since the last daily-pass catch-up, in
// the day tick's own units (0..g_dayLen). See DriveDailyPasses.
static float g_dailyAccum = 0.0f;

// Cleared for good the first time reading the season faults, which can only
// happen if the season test's globals are not up yet. The date then falls back
// to the nearest representative, which is what version 1.x always used.
static int    g_seasonSafe = 1;

// The light factor's stand-in object. Thread local and zero initialised once,
// so the render thread and the simulation thread cannot be handed the same one
// and nothing but the five fields below is ever written into it.
static __declspec(thread) BYTE t_scratch[LIGHT_SCRATCH];

// And the snow-season test's, which needs one field.
static __declspec(thread) BYTE t_season[SEASON_SCRATCH];

// -------------------------------------------------------- the clock scale
//
// Installed whenever any of `slow = world`, vehicle_scale or sim_scale asks
// for it - see NeedClockHooks. One wrapper per imported Power*, and they are
// deliberately as small as they look: PowerTime alone is called thousands of
// times a frame.
//
// `realTime` true means the caller asked for wall-clock time on purpose - the
// callee divides by this[4], the unscaled frame rate, instead of this[0]. Those
// callers are the ones that must keep running at full speed whatever the game
// speed is, so they are passed through untouched.
//
// `g_simDiv` is `slow = world`'s contribution (1.0 unless that mode is active);
// `g_simScale`/`g_vehicleScale` are the independent, always-available dials.
// They multiply rather than compete: `slow = world` plus your own
// vehicle_scale stacks the uniform trade-off with a further correction on top
// of it.

static float h_Power(void* self, float v, char ignorePause, char realTime)
{
    float r = o_Power(self, v, ignorePause, realTime);
    if (!realTime && self == g_simTimer) r /= (g_simDiv * g_simScale);
    return r;
}

static float h_PowerTime(void* self, float v, char ignorePause, char realTime)
{
    float r = o_PowerTime(self, v, ignorePause, realTime);
    if (!realTime && self == g_simTimer) r /= (g_simDiv * g_simScale);
    return r;
}

// Temporary: which functions call PowerKmh on the shared timer, the way
// `ResourceGet` was originally mapped by logging every (name, result) pair
// rather than guessing from disassembly - see docs/03-reverse-engineering.md.
// A vehicle window reported showing a scaled "Maximum speed" figure. The
// window's own drawing code was checked first and does not call Power/
// PowerTime/PowerKmh at all, so whatever it displays has to be a plain field
// - either genuinely unaffected, or written earlier by one of PowerKmh's real
// callers and read back later. The first round only caught the vehicle
// standing still (every v was 0), so this round samples a few *nonzero*
// calls per caller instead of stopping at the first sighting - moving values
// are what settles whether a given site's arithmetic could plausibly be
// feeding that field. Removed once the caller is identified.
#define KMH_MAX_CALLERS 16
#define KMH_SAMPLES_EACH 4
static void* g_kmhCaller[KMH_MAX_CALLERS];
static int   g_kmhSamples[KMH_MAX_CALLERS];
static int   g_kmhCallerCount = 0;

static float h_PowerKmh(void* self, float v, char ignorePause, char realTime)
{
    float r = o_PowerKmh(self, v, ignorePause, realTime);
    if (!realTime && self == g_simTimer)
    {
        if (g_probe && (v > 0.0001f || v < -0.0001f))
        {
            void* caller = _ReturnAddress();
            int idx = -1;
            for (int i = 0; i < g_kmhCallerCount; i++)
                if (g_kmhCaller[i] == caller) { idx = i; break; }
            if (idx < 0 && g_kmhCallerCount < KMH_MAX_CALLERS)
            {
                idx = g_kmhCallerCount++;
                g_kmhCaller[idx]  = caller;
                g_kmhSamples[idx] = 0;
            }
            if (idx >= 0 && g_kmhSamples[idx] < KMH_SAMPLES_EACH)
            {
                g_kmhSamples[idx]++;
                Logf("daynight  PowerKmh probe: caller +0x%llX  v=%.4f -> %.4f scaled "
                     "(%.4f unscaled)  [sample %d/%d]",
                     (unsigned long long)((BYTE*)caller - g_exeBase), v,
                     r / (g_simDiv * g_vehicleScale), r,
                     g_kmhSamples[idx], KMH_SAMPLES_EACH);
            }
        }
        r /= (g_simDiv * g_vehicleScale);
    }
    return r;
}

// ---------------------------------------------------------------- the mapping

// Where inside the 13-phase cycle the calendar currently is: which phase, and
// where inside it rescaled onto the 0..60 the game expects, so every
// proportion inside a phase survives untouched.
static void Phase(int day, float t, float* outTime, int* outPhase)
{
    // cycle_days = 13 is the base game, and is short circuited rather than
    // computed: the general path would come back to the same answer only to
    // within a rounding error, and "changing nothing changes nothing" is worth
    // more than the three lines.
    if (g_cycleDays == PHASES && g_offset == 0.0f)
    {
        int k = day % PHASES;
        if (k < 0) k += PHASES;
        *outTime = t; *outPhase = k;
        return;
    }

    if (t < 0.0f)       t = 0.0f;
    if (t > g_dayLen)   t = g_dayLen;

    int dc = day % g_cycleDays;
    if (dc < 0) dc += g_cycleDays;

    // 0..1 across one whole cycle, then rotated by `offset`.
    float u = ((float)dc * g_dayLen + t) / ((float)g_cycleDays * g_dayLen) + g_offset;
    u -= (float)(int)u;
    if (u < 0.0f) u += 1.0f;

    float up = u * (float)PHASES;
    int   k  = (int)up;
    if (k < 0)          k = 0;
    if (k > PHASES - 1) k = PHASES - 1;

    *outTime  = (up - (float)k) * g_dayLen;
    *outPhase = k;
}

// Every day of the year congruent to `k` modulo 13, ordered by how far it is
// from the real date and clipped to the year. The candidates are thirteen
// apart, so eight of them span a hundred days either way and at least one
// always survives the clipping.
static int Candidates(int day, int k, int* out, int max)
{
    int m = (k - day) % PHASES;
    if (m < 0) m += PHASES;

    int d[8], dist[8], n = 0;
    for (int j = -3; j <= 4 && n < 8; j++)
    {
        int c = day + m - PHASES * j;
        if (c < 0 || c >= DAYS_IN_YEAR) continue;

        int e = c - day;
        if (e < 0) e = -e;

        int p = n++;
        while (p > 0 && dist[p - 1] > e) { dist[p] = dist[p - 1]; d[p] = d[p - 1]; p--; }
        dist[p] = e;
        d[p]    = c;
    }

    int r = (n < max) ? n : max;
    for (int i = 0; i < r; i++) out[i] = d[i];
    return r;
}

// The day to hand the state machine: congruent to the phase modulo 13, as near
// the real date as that allows, and - when the game's own snow-season test can
// be reached - on the same side of every winter boundary as the real date.
//
// Without the second condition a single calendar day sweeps a thirteen-day
// window over the date, and near a boundary that window straddles it: the
// weather then flips in and out of winter thirteen times a day. Every season is
// at least a hundred days long and the representatives are thirteen apart, so a
// matching one always exists.
//
// The test is asked on a **scratch object of the plugin's own**, the same trick
// the light factor uses: the season test's 120-ish bytes reference their
// argument exactly twice and both are `[arg+0x590]`, everything else being an
// absolute global. So the world object is never written to answer a question
// about it.
static int PickDay(int day, int k)
{
    int cand[8];
    int n = Candidates(day, k, cand, 8);
    if (n == 0) return day;
    if (cand[0] == day) return day;             // includes the identity case

    int chosen = cand[0];
    if (g_seasonSafe && g_SnowSeason)
    {
        BYTE* s = t_season;
        __try
        {
            *(int*)(s + OFF_DAY) = day;
            int want = g_SnowSeason(s) ? 1 : 0;

            for (int i = 0; i < n; i++)
            {
                *(int*)(s + OFF_DAY) = cand[i];
                if ((g_SnowSeason(s) ? 1 : 0) == want) { chosen = cand[i]; break; }
            }
        }
        __except (FaultFilter("daynight season", GetExceptionInformation()))
        {
            g_seasonSafe = 0;
            chosen = cand[0];
            Logf("daynight  the snow-season test faulted - the shadowed date is "
                 "the nearest one from here on, and winter may start a few days out");
        }
    }
    return chosen;
}

// ------------------------------------------------------- SLOW_CALENDAR only
//
// Slows the calendar day and nothing else: movement, vehicles, construction
// and every other per-frame rate keep the base game's own pace, because
// nothing here touches C3D_TIMER. It gives back most of what the day tick just
// added to +0x59C. The field stays on its own 0..60, so the rollover test and
// every +0x59C/60 the engine computes elsewhere stay exactly right.
//
// `outDelta` is the true, un-slowed time the day tick added this frame -
// `raw - g_written` when the day did not roll over, or the wrapped amount when
// it did. `outRolled` says whether the day tick's own rollover branch ran this
// frame - both feed DriveDailyPasses, which is what keeps pollution, loans and
// notifications off version 1.x's desync.
static float SlowTheCalendar(BYTE* world, float* outDelta, bool* outRolled)
{
    float raw = *(float*)(world + OFF_DAYTIME);

    if (g_written < 0.0f)
    {
        // The first tick of a session: the field came out of the save and none
        // of it is this tick's advance, so there is no delta to account for.
        g_written  = raw;
        *outDelta  = 0.0f;
        *outRolled = false;
        return raw;
    }

    bool rolled = raw < g_written;
    *outDelta  = rolled ? (g_dayLen - g_written) + raw : raw - g_written;
    *outRolled = rolled;

    float out = raw;
    if (!rolled && g_dayScale > 1.0f)
        out = g_written + (raw - g_written) / g_dayScale;

    if (out > g_dayLen) out = g_dayLen;
    *(float*)(world + OFF_DAYTIME) = out;
    g_written = out;
    return out;
}

// Runs the three real-time-integrating passes - pollution, loans, notices - at
// the cadence the un-slowed calendar would have used, tracked independently of
// what SlowTheCalendar leaves on display. `delta` is what the day tick truly
// added this frame; `rolled` says its own rollover branch already fired once
// this frame, covering one of the crossings below, which must not be paid for
// twice.
//
// Bounded to MAX_CATCHUP so a long stall cannot turn into a burst of calls.
static void DriveDailyPasses(void* world, float delta, bool rolled)
{
    if (!g_DailyPollution && !g_DailyLoans && !g_DailyNotices) return;
    if (g_dayLen <= 0.0f) return;

    g_dailyAccum += delta;

    int crossings = 0;
    while (g_dailyAccum >= g_dayLen && crossings < MAX_CATCHUP)
    {
        g_dailyAccum -= g_dayLen;
        crossings++;
    }
    if (rolled && crossings > 0) crossings--;   // the day tick already paid for one

    for (int i = 0; i < crossings; i++)
    {
        __try
        {
            if (g_DailyPollution) g_DailyPollution(world);
            if (g_DailyLoans)     g_DailyLoans(world);
            if (g_DailyNotices)   g_DailyNotices(world);
        }
        __except (FaultFilter("daynight daily pass", GetExceptionInformation()))
        {
            g_DailyPollution = 0; g_DailyLoans = 0; g_DailyNotices = 0;
            Logf("daynight  a daily catch-up pass faulted - no longer driving them; "
                 "pollution, loans and notices will fall back to the slowed "
                 "calendar's own, rarer pace");
            break;
        }
    }

    if (g_probe && crossings > 0)
        Logf("daynight  %d catch-up day(s) for pollution/loans/notices  "
             "(accumulator %.2f/%.0f)", crossings, g_dailyAccum, g_dayLen);
}

// ---------------------------------------------------------------- the hooks

// The state machine. It writes the weather index, the night factor and two
// flags into the world object, so it gets the real object with two fields
// temporarily rewritten rather than a copy.
static void h_WeatherTick(BYTE* world, char fresh)
{
    int   dayWas   = 0;
    float timeWas  = 0.0f;
    int   phase    = -1;
    bool  shadowed = false;

    // A non-zero second argument is the world-load path, and the only caller
    // that takes it is 0x294318. Forget the last phase there: the first tick
    // after a load has no previous one to have crossed a boundary from.
    if (fresh)
    {
        g_lastPhase  = -1;
        g_written    = -1.0f;
        g_dailyAccum = 0.0f;
    }

    if (g_enabled && world)
    {
        __try
        {
            dayWas = *(int*)(world + OFF_DAY);

            if (g_slow == SLOW_CALENDAR)
            {
                float delta; bool rolled;
                timeWas = SlowTheCalendar(world, &delta, &rolled);
                DriveDailyPasses(world, delta, rolled);
            }
            else
            {
                timeWas = *(float*)(world + OFF_DAYTIME);
            }

            float time = timeWas;
            Phase(dayWas, timeWas, &time, &phase);
            int day = PickDay(dayWas, phase);

            // A phase boundary has to look exactly like a day boundary did:
            // the game's random weather roll and its "clear the overcast"
            // timer both fire off +0x59C being 0.0f on the nose. The very
            // first tick is not a boundary, and must pass the real figure
            // through - that is what keeps cycle_days = 13 the identity.
            if (g_lastPhase >= 0 && phase != g_lastPhase)
            {
                time = 0.0f;
                if (g_probe)
                {
                    ULONGLONG now = GetTickCount64();
                    Logf("daynight  phase %2d -> %2d  day %3d (as %3d)  t %6.2f  weather %d  "
                         "night %.2f  (%llu ms in the last phase)",
                         g_lastPhase, phase, dayWas, day, timeWas,
                         *(int*)(world + OFF_WEATHER), *(float*)(world + OFF_NIGHT),
                         g_phaseAt ? now - g_phaseAt : 0);
                    g_phaseAt = now;
                }
                g_phasesInDay++;
            }
            g_lastPhase = phase;

            *(int*)  (world + OFF_DAY)     = day;
            *(float*)(world + OFF_DAYTIME) = time;
            shadowed = true;
        }
        __except (FaultFilter("daynight weather tick", GetExceptionInformation()))
        {
            g_enabled = 0;
            shadowed  = false;
            Logf("daynight  disabled after a fault - the game keeps its own cycle");
        }
    }

    o_WeatherTick(world, fresh);

    if (shadowed)
    {
        __try
        {
            // Unconditionally, including over the one write the original makes
            // to +0x59C itself. That write zeroes the *calendar day timer* when
            // the weather changes under the editor flag at +0x1090 - harmless
            // when a phase was a day, and a lost day now.
            *(int*)  (world + OFF_DAY)     = dayWas;
            *(float*)(world + OFF_DAYTIME) = timeWas;

            // The weather-change code has just written a fixed 15.0f in here if
            // the weather changed. A phase is shorter now, so the fade has to be.
            if (g_fade) *(float*)(world + OFF_FADE_LEN) = g_fadeLen;
        }
        __except (FaultFilter("daynight restore", GetExceptionInformation()))
        {
            g_enabled = 0;
            Logf("daynight  disabled after a fault putting the date back");
        }
    }

    // Everything below is the probe, and it is all after the restore so it
    // reads what the rest of the game will see. Three edges are worth a line
    // each, and between them they say whether the cycle is running at all and
    // how fast: the date, the lighting the engine was told to load, and a
    // periodic figure to catch a value that is stuck.
    if (g_probe && world)
    {
        __try
        {
            ULONGLONG now     = GetTickCount64();
            int       weather = *(int*)  (world + OFF_WEATHER);
            float     night   = *(float*)(world + OFF_NIGHT);

            if (weather != g_lastWeather)
            {
                static const char* kName[] = { "day1", "sunset2", "night", "overcast1" };
                Logf("daynight  lighting %-9s -> %-9s  day %3d  t %6.2f  phase %2d  "
                     "fade %.2f  (%llu ms since the last change)",
                     (g_lastWeather >= 0 && g_lastWeather < 4) ? kName[g_lastWeather] : "?",
                     (weather >= 0 && weather < 4) ? kName[weather] : "?",
                     dayWas, timeWas, phase,
                     *(float*)(world + OFF_FADE_LEN),
                     g_weatherAt ? now - g_weatherAt : 0);
                g_lastWeather = weather;
                g_weatherAt   = now;
            }

            if (dayWas != g_lastDay)
            {
                if (g_lastDay >= 0)
                    Logf("daynight  day %3d -> %3d  took %llu ms of real time, "
                         "%d phase change(s) in it",
                         g_lastDay, dayWas, g_dayAt ? now - g_dayAt : 0, g_phasesInDay);
                g_lastDay     = dayWas;
                g_dayAt       = now;
                g_phasesInDay = 0;
            }

            if ((++g_ticks & 0xFF) == 0)
                Logf("daynight  probe day %3d  t %6.2f/%.0f  phase %2d  weather %d  "
                     "night %.3f  fade %.2f/%.2f  sim clock /%.2f  daily accum %.2f",
                     dayWas, timeWas, g_dayLen, phase, weather, night,
                     *(float*)(world + OFF_FADE_ELAPSED),
                     *(float*)(world + OFF_FADE_LEN), g_simDiv, g_dailyAccum);
        }
        __except (FaultFilter("daynight probe", GetExceptionInformation())) { }
    }
}

// How lit the world is. Pure, so it is handed a stand-in object rather than the
// real one - nothing shared is written and the render thread cannot see a
// half-applied date. The season does not reach this one: it reads the snowing
// flag at +0xE28 straight off the real world, so the nearest representative of
// the phase is all it needs and no world write is done from the render side.
static float h_LightFactor(BYTE* world)
{
    if (!g_enabled || !world) return o_LightFactor(world);

    BYTE* s = t_scratch;
    __try
    {
        int   day  = *(int*)  (world + OFF_DAY);
        float time = *(float*)(world + OFF_DAYTIME);
        int   phase = 0;
        Phase(day, time, &time, &phase);

        int cand[2];
        if (Candidates(day, phase, cand, 2) > 0) day = cand[0];

        *(int*)  (s + OFF_WEATHER_ON) = *(int*)(world + OFF_WEATHER_ON);
        *(int*)  (s + OFF_WEATHER)    = *(int*)(world + OFF_WEATHER);
        *(int*)  (s + OFF_SNOWING)    = *(int*)(world + OFF_SNOWING);
        *(int*)  (s + OFF_DAY)        = day;
        *(float*)(s + OFF_DAYTIME)    = time;
    }
    __except (FaultFilter("daynight light factor", GetExceptionInformation()))
    {
        g_enabled = 0;
        Logf("daynight  disabled after a fault reading the world object");
        return o_LightFactor(world);
    }

    return o_LightFactor(s);
}

// ---------------------------------------------------------------- setup

static void ReadSettings()
{
    const char* ini = "plugins\\daynight.ini";
    char v[64];

    g_enabled   = H->configInt(ini, "daynight", "enabled", g_enabled);
    g_cycleDays = H->configInt(ini, "daynight", "cycle_days", g_cycleDays);
    g_fade      = H->configInt(ini, "daynight", "fade", g_fade);
    g_probe     = H->configInt(ini, "daynight", "probe", g_probe);

    if (H->configString(ini, "daynight", "offset", v, sizeof(v), "") && v[0])
        g_offset = (float)atof(v);

    if (H->configString(ini, "daynight", "slow", v, sizeof(v), "calendar") && v[0])
    {
        Trim(v);
        if      (_stricmp(v, "none")  == 0) g_slow = SLOW_NONE;
        else if (_stricmp(v, "world") == 0) g_slow = SLOW_WORLD;
        else                                 g_slow = SLOW_CALENDAR;
    }

    if (g_cycleDays < 1)            g_cycleDays = 1;
    if (g_cycleDays > DAYS_IN_YEAR) g_cycleDays = DAYS_IN_YEAR;

    g_offset -= (float)(int)g_offset;
    if (g_offset < 0.0f) g_offset += 1.0f;

    // `auto` is PHASES / cycle_days, and it is the value that leaves one whole
    // day/night cycle taking exactly as long in real time as the base game's
    // did - which is what makes the result watchable rather than a flicker. It
    // is also 1.0 at cycle_days = 13, so that case still changes nothing.
    g_dayScale = 0.0f;
    if (H->configString(ini, "daynight", "day_scale", v, sizeof(v), "auto") && v[0])
    {
        Trim(v);
        if (_stricmp(v, "auto") != 0) g_dayScale = (float)atof(v);
    }
    if (g_dayScale <= 0.0f) g_dayScale = (float)PHASES / (float)g_cycleDays;
    if (g_dayScale < 1.0f)  g_dayScale = 1.0f;      // shortening the day is not on offer
    if (g_dayScale > 100.0f) g_dayScale = 100.0f;

    if (g_slow == SLOW_NONE) g_dayScale = 1.0f;

    // Independent of `slow` and `day_scale` entirely - unlike the day, these
    // are allowed to go either way: > 1 slower, < 1 faster, which is what
    // rebalancing vehicles against pedestrians actually needs.
    g_vehicleScale = 1.0f;
    if (H->configString(ini, "daynight", "vehicle_scale", v, sizeof(v), "1") && v[0])
        g_vehicleScale = (float)atof(v);
    if (g_vehicleScale < 0.1f) g_vehicleScale = 0.1f;
    if (g_vehicleScale > 10.0f) g_vehicleScale = 10.0f;

    g_simScale = 1.0f;
    if (H->configString(ini, "daynight", "sim_scale", v, sizeof(v), "1") && v[0])
        g_simScale = (float)atof(v);
    if (g_simScale < 0.1f) g_simScale = 0.1f;
    if (g_simScale > 10.0f) g_simScale = 10.0f;
}

// Whether anything needs the shared C3D_TIMER touched at all - `slow = world`,
// or either of the two independent dials away from their 1.0 default.
static bool NeedClockHooks()
{
    return (g_dayScale > 1.0f && g_slow == SLOW_WORLD) ||
           g_vehicleScale != 1.0f || g_simScale != 1.0f;
}

// Reads the two constants this plugin scales against out of the executable
// rather than carrying copies of them, and refuses if either is not what this
// build is known to have.
static bool ReadConstants()
{
    float dayLen = *(const float*)(g_exeBase + RVA_DAY_LENGTH);
    if (dayLen != VANILLA_DAY_LEN)
    {
        Logf("daynight  day length at 0x%X is %.3f, expected %.0f - refusing",
             RVA_DAY_LENGTH, dayLen, VANILLA_DAY_LEN);
        return false;
    }
    g_dayLen = dayLen;

    const BYTE* mov = g_exeBase + RVA_FADE_MOV;
    if (memcmp(mov, kFadeMov, sizeof(kFadeMov)) != 0)
    {
        Logf("daynight  rva 0x%X is not the cross-fade mov - fade left alone", RVA_FADE_MOV);
        g_fade = 0;
        return true;
    }

    float fade = *(const float*)(g_exeBase + RVA_FADE_IMM);
    if (fade != VANILLA_FADE)
    {
        Logf("daynight  cross-fade at 0x%X is %.3f, expected %.0f - fade left alone",
             RVA_FADE_IMM, fade, VANILLA_FADE);
        g_fade = 0;
        return true;
    }

    // The same fraction of a phase the base game used: a quarter of it. The
    // day scale belongs in here because the cross-fade's own clock is not the
    // simulation one, so a phase lasts `day_scale` times longer in the units
    // the fade is measured in. With `day_scale = auto` the two cancel and the
    // answer is the vanilla 15 again.
    g_fadeLen = fade * (float)g_cycleDays * g_dayScale / (float)PHASES;
    return true;
}

// The snow-season test, checked instruction by instruction apart from the
// displacement of its one rip-relative load. Not fatal: without it the date
// handed to the state machine is simply the nearest representative, which is
// what version 1.x always used.
static void ResolveSeason()
{
    const BYTE* p = g_exeBase + RVA_SNOW_SEASON;
    if (memcmp(p, kSnowHead, sizeof(kSnowHead)) == 0 &&
        memcmp(p + 7, kSnowTail, sizeof(kSnowTail)) == 0)
    {
        g_SnowSeason = (t_SnowSeason)p;
        return;
    }
    Logf("daynight  rva 0x%X is not the snow-season test - the shadowed date will "
         "be the nearest one, and winter may start a few days out", RVA_SNOW_SEASON);
}

// The three daily-pass functions, resolved by prologue the same way. Any that
// fails to match is simply not driven - DriveDailyPasses skips whichever of
// the three it does not have, so a mismatch degrades to "that one system runs
// at the slowed calendar's own rate" rather than refusing the whole plugin.
static void ResolveDailyPasses()
{
    const BYTE* pol  = g_exeBase + RVA_DAILY_POLLUTION;
    const BYTE* loan = g_exeBase + RVA_DAILY_LOANS;
    const BYTE* note = g_exeBase + RVA_DAILY_NOTICES;

    if (memcmp(pol, kDailyPollutionSig, sizeof(kDailyPollutionSig)) == 0)
        g_DailyPollution = (t_DailyPass)pol;
    else
        Logf("daynight  rva 0x%X is not the pollution pass - it will decay at the "
             "slowed calendar's own rate, same as version 1.x", RVA_DAILY_POLLUTION);

    if (memcmp(loan, kDailyLoansSig, sizeof(kDailyLoansSig)) == 0)
        g_DailyLoans = (t_DailyPass)loan;
    else
        Logf("daynight  rva 0x%X is not the loan pass - loans will barely move, "
             "same as version 1.x", RVA_DAILY_LOANS);

    if (memcmp(note, kDailyNoticesSig, sizeof(kDailyNoticesSig)) == 0)
        g_DailyNotices = (t_DailyPass)note;
    else
        Logf("daynight  rva 0x%X is not the notification pass - offers and notices "
             "will linger", RVA_DAILY_NOTICES);
}

// Proves that 0x9D4EE0 is the C3D_TIMER the calendar is driven from, out of the
// day tick's own call to PowerTime on it:
//
//   lea  rcx,[rip+X]          ->  0x9D4EE0
//   xor  r9d,r9d              ->  realTime  = false
//   xor  r8d,r8d              ->  ignorePause = false
//   call qword ptr [rip+Y]    ->  the PowerTime import slot
//
// Both displacements are resolved and compared rather than matched as bytes,
// so this survives the executable moving and refuses if anything else does.
// `slow = world` only.
static bool VerifySimTimer()
{
    const BYTE* p = g_exeBase + RVA_DAYTICK_POWER;

    if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D)
    {
        Logf("daynight  rva 0x%X is not a `lea rcx` - cannot confirm the simulation clock",
             RVA_DAYTICK_POWER);
        return false;
    }
    const BYTE* timer = p + 7 + *(const int*)(p + 3);
    if (timer != g_exeBase + RVA_SIM_TIMER)
    {
        Logf("daynight  the day tick's timer is +0x%llX, expected +0x%X - refusing to scale it",
             (unsigned long long)(timer - g_exeBase), RVA_SIM_TIMER);
        return false;
    }

    static const BYTE kFlags[] = { 0x45, 0x33, 0xC9, 0x45, 0x33, 0xC0 };  // xor r9d / xor r8d
    if (memcmp(p + 7, kFlags, sizeof(kFlags)) != 0 || p[13] != 0xFF || p[14] != 0x15)
    {
        Logf("daynight  rva 0x%X is not the day tick's PowerTime call - refusing to scale the clock",
             RVA_DAYTICK_POWER);
        return false;
    }

    void** site = (void**)(p + 19 + *(const int*)(p + 15));
    void** slot = FindIatSlot(g_exe, DLL_ENGINE, SYM_POWERTIME);
    if (!slot || site != slot)
    {
        Logf("daynight  the day tick does not call PowerTime through the import slot "
             "- refusing to scale the clock");
        return false;
    }
    return true;
}

// Every Power* the executable imports, or none of them: scaling the time but
// not the vehicle speeds would be a worse desynchronisation than the one this
// is here to remove. Installed for `slow = world`, for vehicle_scale, for
// sim_scale, or any combination - see NeedClockHooks.
static bool ScaleSimClock()
{
    if (!VerifySimTimer()) return false;

    if (!FindIatSlot(g_exe, DLL_ENGINE, SYM_POWER) ||
        !FindIatSlot(g_exe, DLL_ENGINE, SYM_POWERTIME) ||
        !FindIatSlot(g_exe, DLL_ENGINE, SYM_POWERKMH))
    {
        Logf("daynight  not all three C3D_TIMER::Power* are imported - refusing to "
             "scale the clock, because scaling some of them is worse than none");
        return false;
    }

    g_simTimer = g_exeBase + RVA_SIM_TIMER;
    g_simDiv   = (g_slow == SLOW_WORLD) ? g_dayScale : 1.0f;

    bool ok = PatchIat(g_exe, DLL_ENGINE, SYM_POWERTIME, (void*)h_PowerTime,
                       (void**)&o_PowerTime, "C3D_TIMER::PowerTime");
    ok = PatchIat(g_exe, DLL_ENGINE, SYM_POWER, (void*)h_Power,
                  (void**)&o_Power, "C3D_TIMER::Power") && ok;
    ok = PatchIat(g_exe, DLL_ENGINE, SYM_POWERKMH, (void*)h_PowerKmh,
                  (void**)&o_PowerKmh, "C3D_TIMER::PowerKmh") && ok;

    if (!ok)
    {
        // A slot that would not take the swap leaves that one function
        // unscaled; put back whichever did and run unstretched instead.
        void* back = 0;
        if (o_PowerTime) PatchIat(g_exe, DLL_ENGINE, SYM_POWERTIME, (void*)o_PowerTime, &back, "restore");
        if (o_Power)     PatchIat(g_exe, DLL_ENGINE, SYM_POWER,     (void*)o_Power,     &back, "restore");
        if (o_PowerKmh)  PatchIat(g_exe, DLL_ENGINE, SYM_POWERKMH,  (void*)o_PowerKmh,  &back, "restore");
        g_simTimer = 0;
        g_simDiv   = 1.0f;
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "daynight";
    info->version = "2.1";

    ReadSettings();
    if (!g_enabled)
    {
        Logf("daynight  disabled - the cycle stays 13 calendar days long");
        return 1;                       // nothing hooked, no reason to stay
    }
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_enabled) return 1;

    if (!ReadConstants()) return 1;
    ResolveSeason();

    if (!InstallInlineHook(g_exeBase + RVA_WEATHER_TICK, (void*)h_WeatherTick,
                           (void**)&o_WeatherTick, kWeatherTickPrologue,
                           sizeof(kWeatherTickPrologue), "weather tick"))
        return 1;

    // The second copy of the same logic. Without it the sky follows the new
    // cycle and the street lights follow the old one, which is worse than
    // either on its own - so a refusal here takes the whole plugin out rather
    // than leaving half of it installed.
    if (!InstallInlineHook(g_exeBase + RVA_LIGHT_FACTOR, (void*)h_LightFactor,
                           (void**)&o_LightFactor, kLightFactorPrologue,
                           sizeof(kLightFactorPrologue), "light factor"))
    {
        Logf("daynight  the light factor could not be hooked - sky and lights "
             "would disagree, so nothing is changed");
        g_enabled = 0;                  // the first hook stays, and does nothing
        return 1;
    }

    // The shared clock is touched at most once, regardless of how many of
    // `slow = world`, vehicle_scale and sim_scale are asking for it - the
    // messages below all read off this single, already-known result rather
    // than each calling ScaleSimClock (and re-logging its hook attempts)
    // separately.
    bool needClock = NeedClockHooks();
    bool clockOk   = needClock ? ScaleSimClock() : true;
    if (needClock && !clockOk)
    {
        g_vehicleScale = 1.0f;
        g_simScale     = 1.0f;
    }

    bool calendarIsIdentity = (g_cycleDays == PHASES && g_offset == 0.0f && g_dayScale == 1.0f);
    if (calendarIsIdentity)
        Logf("daynight  cycle_days = %d, day_scale = 1 is the base game - hooked, "
             "changes nothing to the calendar", PHASES);
    else
    {
        Logf("daynight  one day/night cycle every %d calendar day(s): "
             "%.2f day units per phase, %.2f of them a fade",
             g_cycleDays, (float)g_cycleDays * g_dayLen / (float)PHASES,
             g_fade ? g_fadeLen : VANILLA_FADE);

        if (g_dayScale > 1.0f && g_slow == SLOW_CALENDAR)
        {
            ResolveDailyPasses();
            Logf("daynight  calendar stretched %.2fx and NOTHING ELSE IS - walking, "
                 "vehicles, construction and production keep the base game's own "
                 "pace. Pollution decay, loan repayment and notification expiry are "
                 "driven directly at the un-slowed rate so they do not fall behind "
                 "the per-frame systems they balance (%s%s%s)",
                 g_dayScale,
                 g_DailyPollution ? "pollution ok" : "pollution NOT DRIVEN",
                 g_DailyLoans     ? ", loans ok"   : ", loans NOT DRIVEN",
                 g_DailyNotices   ? ", notices ok" : ", notices NOT DRIVEN");
        }
        else if (g_dayScale > 1.0f && g_slow == SLOW_WORLD)
        {
            if (clockOk)
                Logf("daynight  slow = world: the whole simulation clock is scaled "
                     "%.2fx along with the calendar, so a day lasts %.2fx longer and "
                     "the cycle runs at %.2fx the base game's pace. EVERY rate in the "
                     "game slows with it, in step - including walking, vehicles and "
                     "construction",
                     g_dayScale, g_dayScale, (float)g_cycleDays * g_dayScale / (float)PHASES);
            else
                Logf("daynight  slow = world could not scale the clock - the cycle now "
                     "fits in one in-game day, a few seconds of real time, and nothing "
                     "else is affected");
        }
        else
        {
            Logf("daynight  calendar day left at its own length, so the whole cycle now "
                 "fits in one in-game day - a few seconds of real time. Set day_scale "
                 "= auto if that is too fast to see");
        }

        if (g_offset != 0.0f)
            Logf("daynight  cycle rotated by %.3f of itself", g_offset);
        if (!g_fade)
            Logf("daynight  cross-fade left at %.0f - it may not finish inside a phase",
                 VANILLA_FADE);
        if (!g_SnowSeason)
            Logf("daynight  no season test - the weather may change season a few days off");
    }

    // vehicle_scale and sim_scale are independent of everything above.
    if (clockOk && g_vehicleScale != 1.0f)
        Logf("daynight  vehicle_scale %.2fx - vehicle speed only, independent of "
             "walking, production and the calendar", g_vehicleScale);
    if (clockOk && g_simScale != 1.0f)
        Logf("daynight  sim_scale %.2fx - everything the shared clock drives except "
             "vehicles: walking, construction, production, animation. Independent of "
             "the calendar and of vehicle_scale", g_simScale);
    if (!clockOk && needClock && !(g_dayScale > 1.0f && g_slow == SLOW_WORLD))
        Logf("daynight  the simulation clock could not be scaled - vehicle_scale and "
             "sim_scale are not applied");

    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
