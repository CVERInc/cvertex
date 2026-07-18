// railstorm.c — a rail shooter. The ship never stops; the world comes at it.
//
// Star Fox's whole trick in one sentence: you don't fly TO the depth, the depth flies AT
// you. So the player's z never moves — it sits at 0 forever — and everything else (rocks,
// fighters, stars) starts far out on +z and counts down to meet it. Steering is lateral
// only: x and y move the ship inside a bounded window in front of the lens, and the camera
// trails that window at a fraction of its own, so banking hard visibly shoves the ship off
// centre and it snaps back as you level out. That lag is the whole feel of a chase cam —
// remove it and the ship is welded to the crosshair, which reads as flat no matter how much
// geometry is rushing past it.
//
// Every moving body here is a handful of boxes (box(), copied verbatim from the engine's proven
// pattern: axis-aligned half-extents, analytic outward normals, so backface culling and
// flat-shaded gloss are never in question). A composite — ship, fighter, asteroid — is a
// short table of {mesh, local offset}; add_part() rotates each offset by the object's own
// current orientation before placing it, so the parts translate AND spin together as one
// rigid body. No new geometry technique, just the box built five times and rotated as one.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// ---- fixed point --------------------------------------------------------------
#define U (1 << 16)                                   // 16.16, 1<<16 = one world unit
#define FX(v) ((int32_t)((int64_t)(v) * U / 100))      // v in hundredths of a unit
static int32_t mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- tuning ---------------------------------------------------------------------
#define SHIP_LIM_X   FX(240)          // how far the ship can steer off the centreline
#define SHIP_LIM_Y   FX(160)
#define SHIP_SPEED   FX(12)           // lateral units per frame at full stick
#define CAM_BACK     FX(520)          // the lens sits this far behind the ship's own z=0
#define CAM_UP       FX(95)
#define CAM_FOLLOW   FX(65)           // percent-ish: the camera only chases 65% of the ship's offset
#define BANK_MAX     150              // angle units (1024 = 360deg) at full roll
#define PITCH_MAX    55
#define YAW_DIV      3

#define FWD_BASE      FX(58)          // world-approach speed, ramps with wave
#define FWD_WAVE_STEP FX(4)
#define FWD_MAX       FX(115)
#define FIGHTER_EXTRA FX(14)          // fighters close a little faster than rocks

#define SPAWN_Z       FX(7200)        // where a new threat is born, far out on +z
#define BOLT_SPEED    FX(260)
#define BOLT_MAXZ     FX(7600)
#define BOLT_HIT_TOL  FX(230)         // must exceed bolt+enemy closing speed per frame, or hits tunnel through
#define BOLT_R        FX(18)
#define ASTEROID_R    FX(78)
#define FIGHTER_R     FX(52)
#define PLAYER_HIT_AST FX(150)
#define PLAYER_HIT_FTR FX(128)

#define FIRE_CD       9               // frames between shots
#define IFRAMES       50              // brief safety after a hit
#define HURT_FRAMES   24

#define SHAKE_HIT     FX(34)
#define SHAKE_KILL    FX(15)
#define SHAKE_DEATH   FX(60)
#define SHAKE_DECAY   FX(82)          // multiplied in each frame: ~18% decay/frame

#define WAVE_LEN      600             // frames per wave (~10s)
#define SPAWN_BASE    62
#define SPAWN_DECAY   5
#define SPAWN_MIN     18

#define MAX_ENEMY 20
#define MAX_BOLT  12
#define MAX_SHARD 96
#define NUM_STARS 32
#define STAR_SPEED FX(70)
#define STAR_RANGE FX(9000)
#define SHARD_LIFE 22

#define MAX_INST 320
#define START_LIVES 3

#define EV_FIRE  1
#define EV_KILL  2
#define EV_HIT   4
#define EV_DEATH 8

// ---- runtime mesh pool: every shape in the game is a box, built once -------------
#define MAXBOX 20
static V3   bv[MAXBOX][8];
static Tri  bt[MAXBOX][12];
static Mesh bm[MAXBOX];
static int  nbox;

static const int8_t  VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                   {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t  FN[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static int box(int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    if (nbox >= MAXBOX) return 0;
    int i = nbox++;
    for (int v = 0; v < 8; v++) {
        bv[i][v].x = VP[v][0] * sx; bv[i][v].y = VP[v][1] * sy; bv[i][v].z = VP[v][2] * sz;
    }
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &bt[i][f * 2 + k];
            t->a = FQ[f][0]; t->b = FQ[f][1 + k]; t->c = FQ[f][2 + k]; t->ci = ci;
            t->nx = (int16_t)(FN[f][0] * 32767); t->ny = (int16_t)(FN[f][1] * 32767); t->nz = (int16_t)(FN[f][2] * 32767);
        }
    bm[i].v = bv[i]; bm[i].nv = 8; bm[i].t = bt[i]; bm[i].nt = 12;
    return i;
}

static int SHIP_FUSE, SHIP_WING, SHIP_FIN, SHIP_CAN, SHIP_ENG;
static int EN_FUSE, EN_WING, EN_FIN;
static int AST_MAIN, AST_CHUNK;
static int BOLT_M, SHARD_M, STAR_M;

static void build_meshes(void) {
    nbox = 0;
    SHIP_FUSE = box(FX(26), FX(20), FX(85), 8);
    SHIP_WING = box(FX(58), FX(7),  FX(36), 8);
    SHIP_FIN  = box(FX(9),  FX(28), FX(26), 8);
    SHIP_CAN  = box(FX(15), FX(13), FX(28), 16);
    SHIP_ENG  = box(FX(13), FX(13), FX(9),  24);
    EN_FUSE   = box(FX(20), FX(16), FX(58), 32);
    EN_WING   = box(FX(44), FX(6),  FX(24), 32);
    EN_FIN    = box(FX(8),  FX(18), FX(16), 32);
    AST_MAIN  = box(FX(65), FX(55), FX(60), 48);
    AST_CHUNK = box(FX(34), FX(29), FX(31), 48);
    BOLT_M    = box(FX(6),  FX(6),  FX(42), 56);
    SHARD_M   = box(FX(9),  FX(9),  FX(9),  64);
    STAR_M    = box(FX(4),  FX(4),  FX(4),  72);
}

// A composite part: rotate its local offset by the object's own current orientation,
// then place the box there and give it the SAME orientation — that's the whole trick
// for a rigid multi-box body (see file header).
static int add_part(Inst *inst, int n, int meshIdx, V3 base, V3 local, int ax, int ay, int az, int32_t scale) {
    if (n >= MAX_INST) return n;
    int32_t x = local.x, y = local.y, z = local.z;
    g3d_rot(&x, &y, &z, ax, ay, az);
    x = mul(x, scale); y = mul(y, scale); z = mul(z, scale);
    inst[n].m = &bm[meshIdx];
    inst[n].pos = (V3){ base.x + x, base.y + y, base.z + z };
    inst[n].ax = ax; inst[n].ay = ay; inst[n].az = az;
    inst[n].scale = scale;
    return n + 1;
}

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int k = 60 + i * 28;
        g_pal[base + i] = 0xFF000000u
            | ((uint32_t)(((rgb >> 16) & 255) * k / 255) << 16)
            | ((uint32_t)(((rgb >> 8) & 255) * k / 255) << 8)
            | (uint32_t)((rgb & 255) * k / 255);
    }
}

// ---- game state -------------------------------------------------------------------
typedef struct {
    uint8_t alive, kind;              // 0 = asteroid (tumbles), 1 = fighter (weaves + banks)
    int32_t baseX, x, y, z;
    int32_t scale;                    // asteroids vary in size
    int16_t rx, ry, rz, rrx, rry, rrz; // asteroid tumble angle + per-instance rate
    int32_t wphase, wrate, wamp;      // fighter weave
} Enemy;

typedef struct { uint8_t alive; int32_t x, y, z; } Bolt;

typedef struct {
    uint8_t life;
    int32_t x, y, z, vx, vy, vz;
    int16_t rx, ry, rz, rrx, rry, rrz;
} Shard;

typedef struct { int32_t x, y, z; } Star;

static Enemy g_en[MAX_ENEMY];
static Bolt  g_bolt[MAX_BOLT];
static Shard g_shard[MAX_SHARD];
static Star  g_star[NUM_STARS];

static uint32_t g_frame;
static uint32_t g_seed;
static int32_t g_px, g_py;            // ship position: world x,y; z is always 0
static int32_t g_bank_cur, g_pitch_cur;
static int g_fire_cd, g_iframes, g_hurt;
static int32_t g_shake;
static int g_spawn_timer;
static int32_t g_score;
static int g_lives, g_over;
static uint32_t g_events;

static uint32_t rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

static void spawn_shards(int32_t x, int32_t y, int32_t z, int burst) {
    int n = burst;
    for (int k = 0; k < n; k++) {
        int idx = -1;
        for (int i = 0; i < MAX_SHARD; i++) if (!g_shard[i].life) { idx = i; break; }
        if (idx < 0) break;
        Shard *s = &g_shard[idx];
        uint32_t r1 = rnd(), r2 = rnd();
        int ang = (int)(r1 & 1023);
        int32_t speed = FX(8) + (int32_t)(r2 % FX(10));
        int32_t sx = g_sin[ang], sz = g_sin[(ang + 256) & 1023];
        int32_t sy = (int32_t)((int)((r1 >> 10) & 255) - 128);
        s->x = x; s->y = y; s->z = z;
        s->vx = (int32_t)(((int64_t)sx * speed) >> 15);
        s->vz = (int32_t)(((int64_t)sz * speed) >> 15);
        s->vy = (int32_t)(((int64_t)sy * speed) >> 8);
        s->rx = (int16_t)(r2 & 1023); s->ry = (int16_t)((r2 >> 10) & 1023); s->rz = (int16_t)((r1 >> 20) & 1023);
        s->rrx = (int16_t)(20 + (r2 % 40)); s->rry = (int16_t)(15 + ((r2 >> 5) % 35)); s->rrz = (int16_t)(10 + ((r2 >> 10) % 30));
        s->life = SHARD_LIFE;
    }
}

static void spawn_enemy(int wave) {
    for (int i = 0; i < MAX_ENEMY; i++) {
        if (g_en[i].alive) continue;
        Enemy *e = &g_en[i];
        uint32_t r1 = rnd(), r2 = rnd(), r3 = rnd(), r4 = rnd();
        int fighterChance = 25 + wave * 6; if (fighterChance > 70) fighterChance = 70;
        e->kind = (uint8_t)((r1 % 100) < (uint32_t)fighterChance ? 1 : 0);
        e->alive = 1;
        int32_t limX = (SHIP_LIM_X * 85) / 100, limY = (SHIP_LIM_Y * 85) / 100;
        e->baseX = (int32_t)(((int64_t)(r2 % 2001) - 1000) * limX / 1000);
        e->y      = (int32_t)(((int64_t)(r3 % 2001) - 1000) * limY / 1000);
        e->x = e->baseX;
        e->z = SPAWN_Z;
        if (e->kind == 0) {
            e->scale = FX(70) + (int32_t)(r4 % FX(80));
            e->rx = (int16_t)(r1 & 1023); e->ry = (int16_t)(r2 & 1023); e->rz = (int16_t)(r3 & 1023);
            e->rrx = (int16_t)(2 + ((r4 >> 3) % 7)); e->rry = (int16_t)(2 + ((r4 >> 7) % 7)); e->rrz = (int16_t)(2 + ((r4 >> 11) % 7));
            e->wamp = 0; e->wphase = 0; e->wrate = 0;
        } else {
            e->scale = U;
            e->wamp = FX(40) + (int32_t)(r4 % FX(60));
            e->wphase = (int32_t)((r4 >> 8) & 1023);
            e->wrate = 6 + (int32_t)((r4 >> 16) % 10);
            e->rx = e->ry = e->rz = e->rrx = e->rry = e->rrz = 0;
        }
        return;
    }
}

static void reset_game(void) {
    g_frame = 0;
    g_seed = 0x9E3779B9u;
    g_px = 0; g_py = 0;
    g_bank_cur = 0; g_pitch_cur = 0;
    g_fire_cd = 0; g_iframes = 0; g_hurt = 0;
    g_shake = 0;
    g_spawn_timer = 30;
    g_score = 0;
    g_lives = START_LIVES;
    g_over = 0;
    g_events = 0;
    for (int i = 0; i < MAX_ENEMY; i++) g_en[i].alive = 0;
    for (int i = 0; i < MAX_BOLT; i++) g_bolt[i].alive = 0;
    for (int i = 0; i < MAX_SHARD; i++) g_shard[i].life = 0;
    for (int i = 0; i < NUM_STARS; i++) {
        uint32_t r1 = rnd(), r2 = rnd(), r3 = rnd();
        g_star[i].x = (int32_t)((int64_t)(r1 % 2001) - 1000) * SHIP_LIM_X * 2 / 1000;
        g_star[i].y = (int32_t)((int64_t)(r2 % 2001) - 1000) * SHIP_LIM_Y * 2 / 1000;
        g_star[i].z = (int32_t)(r3 % (uint32_t)STAR_RANGE);
    }
}

static void init(void) {
    tables_init();
    build_meshes();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0A0E1E;   // deep space
    g_pal[1] = 0xFF7C82A6;   // HUD label
    g_pal[2] = 0xFFF2F4FA;   // HUD number
    g_pal[3] = 0xFF15171F;   // game-over band
    g_pal[4] = 0xFF4DF0FF;   // accent
    g_pal[5] = 0xFFE0505A;   // danger / hit flash
    ramp(8,  0xFF5FB4E8);    // hull
    ramp(16, 0xFFF2F4FA);    // canopy
    ramp(24, 0xFFFFD24D);    // engine glow
    ramp(32, 0xFFE0525C);    // hostile fighters
    ramp(48, 0xFF8A7A66);    // asteroid rock
    ramp(56, 0xFF7CFFB0);    // bolt
    ramp(64, 0xFFFFA23C);    // explosion debris
    ramp(72, 0xFFCFE0FF);    // starfield
    reset_game();
}

static void combined_input(const Input in[2], int32_t *ix, int32_t *iy, int *jump) {
    int x = in[0].x + in[1].x, y = in[0].y + in[1].y;
    if (x > 1) x = 1; if (x < -1) x = -1;
    if (y > 1) y = 1; if (y < -1) y = -1;
    *ix = x; *iy = y;
    *jump = in[0].jump || in[1].jump;
}

static void tick(const Input in[2]) {
    g_frame++;
    g_events = 0;
    int32_t ix, iy; int ijump;
    combined_input(in, &ix, &iy, &ijump);

    if (g_over) {
        if (ijump) reset_game();
        return;
    }

    // ---- steer ----
    g_px = clampi(g_px + ix * SHIP_SPEED, -SHIP_LIM_X, SHIP_LIM_X);
    g_py = clampi(g_py + iy * SHIP_SPEED, -SHIP_LIM_Y, SHIP_LIM_Y);
    int32_t bankTarget = -ix * BANK_MAX, pitchTarget = iy * PITCH_MAX;
    g_bank_cur += (bankTarget - g_bank_cur) / 4;
    g_pitch_cur += (pitchTarget - g_pitch_cur) / 4;

    // ---- fire ----
    if (g_fire_cd > 0) g_fire_cd--;
    if (ijump && g_fire_cd == 0) {
        for (int i = 0; i < MAX_BOLT; i++)
            if (!g_bolt[i].alive) { g_bolt[i].alive = 1; g_bolt[i].x = g_px; g_bolt[i].y = g_py; g_bolt[i].z = 0; break; }
        g_fire_cd = FIRE_CD;
        g_events |= EV_FIRE;
    }
    for (int i = 0; i < MAX_BOLT; i++) if (g_bolt[i].alive) {
        g_bolt[i].z += BOLT_SPEED;
        if (g_bolt[i].z > BOLT_MAXZ) g_bolt[i].alive = 0;
    }

    // ---- waves ----
    int wave = (int)(g_frame / WAVE_LEN);
    if (g_spawn_timer > 0) g_spawn_timer--;
    else {
        spawn_enemy(wave);
        int interval = SPAWN_BASE - wave * SPAWN_DECAY; if (interval < SPAWN_MIN) interval = SPAWN_MIN;
        g_spawn_timer = interval;
    }
    int32_t fwdSpeed = FWD_BASE + wave * FWD_WAVE_STEP; if (fwdSpeed > FWD_MAX) fwdSpeed = FWD_MAX;

    // ---- enemies: move, get shot, or reach the ship ----
    for (int i = 0; i < MAX_ENEMY; i++) {
        Enemy *e = &g_en[i]; if (!e->alive) continue;
        int32_t oldz = e->z;
        int32_t speed = fwdSpeed + (e->kind == 1 ? FIGHTER_EXTRA : 0);
        e->z -= speed;
        if (e->kind == 1) {
            e->wphase = (e->wphase + e->wrate) & 1023;
            e->x = e->baseX + mul(e->wamp, g_sin[e->wphase]);
        } else {
            e->x = e->baseX;
            e->rx = (e->rx + e->rrx) & 1023; e->ry = (e->ry + e->rry) & 1023; e->rz = (e->rz + e->rrz) & 1023;
        }

        for (int b = 0; b < MAX_BOLT; b++) {
            if (!g_bolt[b].alive) continue;
            int32_t dz = g_bolt[b].z - e->z; if (dz < 0) dz = -dz;
            if (dz > BOLT_HIT_TOL) continue;
            int32_t dx = g_bolt[b].x - e->x; if (dx < 0) dx = -dx;
            int32_t dy = g_bolt[b].y - e->y; if (dy < 0) dy = -dy;
            int32_t rad = (e->kind == 0 ? mul(ASTEROID_R, e->scale) : FIGHTER_R) + BOLT_R;
            if (dx < rad && dy < rad) {
                e->alive = 0; g_bolt[b].alive = 0;
                spawn_shards(e->x, e->y, e->z, e->kind == 1 ? 10 : 8);
                g_score += (e->kind == 1 ? 100 : 60) + wave * 5;
                g_shake = SHAKE_KILL;
                g_events |= EV_KILL;
                break;
            }
        }
        if (!e->alive) continue;

        if (oldz >= 0 && e->z < 0) {                  // crossed the ship's plane, unshot
            e->alive = 0;
            if (g_iframes == 0) {
                int32_t dx = g_px - e->x; if (dx < 0) dx = -dx;
                int32_t dy = g_py - e->y; if (dy < 0) dy = -dy;
                int32_t rad = e->kind == 0 ? mul(PLAYER_HIT_AST, e->scale) : PLAYER_HIT_FTR;
                if (dx < rad && dy < rad) {
                    g_lives--; g_hurt = HURT_FRAMES; g_iframes = IFRAMES; g_shake = SHAKE_HIT;
                    g_events |= EV_HIT;
                    if (g_lives <= 0) {
                        g_over = 1; g_shake = SHAKE_DEATH;
                        spawn_shards(g_px, g_py, 0, 16);
                        g_events |= EV_DEATH;
                    }
                }
            }
        }
    }
    if (g_iframes > 0) g_iframes--;
    if (g_hurt > 0) g_hurt--;

    // ---- shards ----
    for (int i = 0; i < MAX_SHARD; i++) if (g_shard[i].life) {
        Shard *s = &g_shard[i];
        s->x += s->vx; s->y += s->vy; s->z += s->vz;
        s->vx = mul(s->vx, FX(92)); s->vy = mul(s->vy, FX(92)); s->vz = mul(s->vz, FX(92));
        s->rx = (s->rx + s->rrx) & 1023; s->ry = (s->ry + s->rry) & 1023; s->rz = (s->rz + s->rrz) & 1023;
        s->life--;
    }

    // ---- starfield scroll ----
    for (int i = 0; i < NUM_STARS; i++) {
        g_star[i].z -= STAR_SPEED;
        if (g_star[i].z < -FX(300)) g_star[i].z += STAR_RANGE;
    }

    g_shake = mul(g_shake, SHAKE_DECAY);
}

static void audio(void) {
    if (g_events & EV_DEATH) synth_note(NCHAN - 1, 3, 38, 170);
    else if (g_events & EV_KILL) synth_note(NCHAN - 1, 3, 52, 150);
    else if (g_events & EV_HIT) synth_note(NCHAN - 1, 4, 45, 165);
    else if (g_events & EV_FIRE) synth_note(NCHAN - 1, 5, 74, 110);
}

// ---- HUD number rendering (digits(), copied from the house pattern) -------------
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) {
    char b[12]; digits(v, b);
    text_draw(x, y, sc, b, ci);
}

static Inst g_inst[MAX_INST];

static void draw(void) {
    // 🔴 tick() returns early once g_over is set, so g_hurt (and its flash) freezes at
    // whatever value it held on the killing blow — always true right after death. Gate
    // the flash on !g_over or the whole game-over screen stays stuck red.
    uint8_t bg = (!g_over && g_hurt > HURT_FRAMES - 5) ? 5 : 0;
    fb_clear(bg);

    int n = 0;
    V3 shipPos = { g_px, g_py, 0 };
    int sax = g_pitch_cur, say = g_bank_cur / YAW_DIV, saz = g_bank_cur;
    n = add_part(g_inst, n, SHIP_FUSE, shipPos, (V3){ 0, 0, 0 }, sax, say, saz, U);
    n = add_part(g_inst, n, SHIP_WING, shipPos, (V3){ FX(72), -FX(4), -FX(4) }, sax, say, saz, U);
    n = add_part(g_inst, n, SHIP_WING, shipPos, (V3){ -FX(72), -FX(4), -FX(4) }, sax, say, saz, U);
    n = add_part(g_inst, n, SHIP_FIN,  shipPos, (V3){ 0, FX(34), -FX(54) }, sax, say, saz, U);
    n = add_part(g_inst, n, SHIP_CAN,  shipPos, (V3){ 0, FX(19), FX(18) }, sax, say, saz, U);
    n = add_part(g_inst, n, SHIP_ENG,  shipPos, (V3){ 0, 0, -FX(93) }, sax, say, saz, U);

    for (int i = 0; i < MAX_ENEMY; i++) {
        Enemy *e = &g_en[i]; if (!e->alive) continue;
        V3 pos = { e->x, e->y, e->z };
        if (e->kind == 0) {
            n = add_part(g_inst, n, AST_MAIN,  pos, (V3){ 0, 0, 0 }, e->rx, e->ry, e->rz, e->scale);
            n = add_part(g_inst, n, AST_CHUNK, pos, (V3){ FX(36), FX(20), -FX(18) }, e->rx, e->ry, e->rz, e->scale);
        } else {
            int bankA = (int)(((int32_t)g_sin[(e->wphase + 256) & 1023] * BANK_MAX) >> 15);
            n = add_part(g_inst, n, EN_FUSE, pos, (V3){ 0, 0, 0 }, 0, 0, bankA, U);
            n = add_part(g_inst, n, EN_WING, pos, (V3){ FX(50), -FX(2), FX(2) }, 0, 0, bankA, U);
            n = add_part(g_inst, n, EN_WING, pos, (V3){ -FX(50), -FX(2), FX(2) }, 0, 0, bankA, U);
            n = add_part(g_inst, n, EN_FIN,  pos, (V3){ 0, FX(22), FX(34) }, 0, 0, bankA, U);
        }
    }

    for (int i = 0; i < MAX_BOLT; i++) if (g_bolt[i].alive)
        n = add_part(g_inst, n, BOLT_M, (V3){ g_bolt[i].x, g_bolt[i].y, g_bolt[i].z }, (V3){ 0, 0, 0 }, 0, 0, 0, U);

    for (int i = 0; i < MAX_SHARD; i++) if (g_shard[i].life) {
        Shard *s = &g_shard[i];
        int32_t frac = (int32_t)(((int64_t)s->life * U) / SHARD_LIFE);
        n = add_part(g_inst, n, SHARD_M, (V3){ s->x, s->y, s->z }, (V3){ 0, 0, 0 }, s->rx, s->ry, s->rz, frac);
    }

    for (int i = 0; i < NUM_STARS; i++)
        n = add_part(g_inst, n, STAR_M, (V3){ g_star[i].x, g_star[i].y, g_star[i].z }, (V3){ 0, 0, 0 }, 0, 0, 0, U);

    Cam cam;
    cam.pos.x = mul(g_px, CAM_FOLLOW) + mul(g_shake, g_sin[(g_frame * 53) & 1023]);
    cam.pos.y = CAM_UP + mul(g_py, CAM_FOLLOW) + mul(g_shake, g_sin[(g_frame * 71 + 300) & 1023]);
    cam.pos.z = -CAM_BACK;
    cam.ax = -(g_pitch_cur / 4); cam.ay = 0; cam.az = 0;
    g3d_scene(g_inst, n, &cam, 0, 0, 0);

    // ---- HUD ----
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "SCORE", 1);
    num(6 * sc + 34 * sc, hud_top(), sc, g_score, 2);
    text_draw(6 * sc, hud_top() + 10 * sc, sc, "LIVES", 1);
    num(6 * sc + 34 * sc, hud_top() + 10 * sc, sc, g_lives, g_lives > 1 ? (uint8_t)2 : (uint8_t)5);
    const char *wv = "WAVE";
    int wave = (int)(g_frame / WAVE_LEN) + 1;
    text_draw(g_fbw - 6 * sc - text_width(wv, sc) - 24 * sc, hud_top(), sc, wv, 1);
    num(g_fbw - 6 * sc - 18 * sc, hud_top(), sc, wave, 2);

    if (!g_over) return;

    int cx = g_fbw / 2, cy = g_fbh / 2;
    for (int y = cy - 30 * sc; y < cy + 34 * sc; y++)
        for (int x = 0; x < g_fbw; x++)
            if (y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = 3;
    text_draw(cx - text_width("GAME OVER", sc * 3) / 2, cy - 22 * sc, sc * 3, "GAME OVER", 5);
    char b[12]; digits(g_score, b);
    int w = text_width("SCORE", sc * 2) + 6 * sc * 2 + text_width(b, sc * 2);
    text_draw(cx - w / 2, cy + 3 * sc, sc * 2, "SCORE", 1);
    num(cx - w / 2 + text_width("SCORE", sc * 2) + 6 * sc * 2, cy + 3 * sc, sc * 2, g_score, 2);
    text_draw(cx - text_width("SPACE TO RESTART", sc) / 2, cy + 22 * sc, sc, "SPACE TO RESTART", 1);
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ULL;
#define MIX(v) h = (h ^ (uint64_t)(uint32_t)(v)) * 1099511628211ULL
    MIX(g_frame); MIX(g_score); MIX(g_lives); MIX(g_over); MIX(g_px); MIX(g_py);
    MIX(g_seed); MIX(g_spawn_timer); MIX(g_fire_cd); MIX(g_iframes); MIX(g_hurt); MIX(g_shake);
    for (int i = 0; i < MAX_ENEMY; i++) {
        MIX(g_en[i].alive);
        if (g_en[i].alive) { MIX(g_en[i].kind); MIX(g_en[i].x); MIX(g_en[i].y); MIX(g_en[i].z); }
    }
    for (int i = 0; i < MAX_BOLT; i++) {
        MIX(g_bolt[i].alive);
        if (g_bolt[i].alive) { MIX(g_bolt[i].x); MIX(g_bolt[i].y); MIX(g_bolt[i].z); }
    }
    for (int i = 0; i < MAX_SHARD; i++) MIX(g_shard[i].life);
#undef MIX
    return h;
}

const Game game_railstorm = { "railstorm", init, tick, audio, draw, checksum };
