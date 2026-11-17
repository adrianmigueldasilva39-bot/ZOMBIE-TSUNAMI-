/*
 * ZOMBIE TSUNAMI - homebrew PS3 (PSL1GHT)
 * Inspirado em Zombie Tsunami (Mobigame).
 *
 * Controles:
 *   ESQUERDA / DIREITA : conduz a horda
 *   START              : reinicia
 *
 * A horda cresce ao pegar humanos e portões (+N / xN) e diminui
 * ao bater em barreiras. A cada 1500 frames passa um tsunami de
 * zumbis que devora os humanos no caminho.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#define SCR_W      1280
#define SCR_H      720
#define ROAD_X     340
#define ROAD_W     600
#define LANES      3
#define LANE_W     (ROAD_W / LANES)
#define CROWD_Y    600
#define MAX_OBJ    128
#define MAX_ZOMBIES 2000
#define CB_SIZE    0x100000
#define HOST_SIZE  (32 * 1024 * 1024)

/* Índices de botão do pad (ordem padrão libps3 / io/pad.h) */
#define BT_START   3
#define BT_RIGHT   5
#define BT_LEFT    7

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

enum { OBJ_HUMAN, OBJ_GATE, OBJ_WALL };
enum { OP_ADD, OP_MUL };

typedef struct {
    int   type;
    int   active;
    int   op;
    int   value;
    float x, y, w, h;
} Obj;

/* ---------- Vídeo / RSX ---------- */
static gcmContextData *ctx;
static void *bufs[2];
static u32   offs[2];
static int   cur = 0;
static u32  *fb;

/* ---------- Estado do jogo ---------- */
static padData pad;
static Obj  objs[MAX_OBJ];
static int  zcount;
static int  score;
static int  frame;
static float px;            /* centro X da horda */
static float scroll;        /* velocidade de descida dos objetos */
static int  wave_on;
static float wave_y;

/* ---------- Utilitários ---------- */
static float clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

static int clampi(int v, int a, int b)
{
    return v < a ? a : (v > b ? b : v);
}

static inline void put(int x, int y, u32 c)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H)
        fb[y * SCR_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, u32 c)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            put(i, j, c);
}

static void fill_circle(int cx, int cy, int r, u32 c)
{
    int dx, dy;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                put(cx + dx, cy + dy, c);
}

/* ---------- Objetos ---------- */
static Obj *alloc_obj(void)
{
    int i;
    for (i = 0; i < MAX_OBJ; i++) {
        if (!objs[i].active) {
            memset(&objs[i], 0, sizeof(Obj));
            objs[i].active = 1;
            return &objs[i];
        }
    }
    return NULL;
}

static void spawn_human(void)
{
    Obj *o = alloc_obj();
    if (!o) return;
    o->type = OBJ_HUMAN;
    o->w = 18;
    o->h = 26;
    o->x = ROAD_X + 20 + rand() % (ROAD_W - 40);
    o->y = -40;
}

static void spawn_gates(void)
{
    Obj *a = alloc_obj();
    Obj *b = alloc_obj();

    if (a) {                       /* portão da esquerda: sempre soma */
        a->type = OBJ_GATE;
        a->op = OP_ADD;
        a->value = 3 + rand() % 8;
        a->x = ROAD_X;
        a->y = -60;
        a->w = ROAD_W / 2 - 4;
        a->h = 40;
    }
    if (b) {                       /* portão da direita: soma ou multiplica */
        b->type = OBJ_GATE;
        b->x = ROAD_X + ROAD_W / 2 + 4;
        b->y = -60;
        b->w = ROAD_W / 2 - 4;
        b->h = 40;
        if (rand() % 2) {
            b->op = OP_MUL;
            b->value = 2 + rand() % 2;
        } else {
            b->op = OP_ADD;
            b->value = 1 + rand() % 5;
        }
    }
}

static void spawn_wall_row(void)
{
    int gap = rand() % LANES;
    int l;
    for (l = 0; l < LANES; l++) {
        Obj *o;
        if (l == gap) continue;
        o = alloc_obj();
        if (!o) return;
        o->type = OBJ_WALL;
        o->x = ROAD_X + l * LANE_W + 10;
        o->w = LANE_W - 20;
        o->h = 40;
        o->y = -60;
    }
}

/* ---------- Horda ---------- */
static float crowd_hw(void)
{
    return 30.0f + sqrtf((float)zcount) * 3.0f;
}

static float crowd_hh(void)
{
    return 30.0f + sqrtf((float)zcount) * 2.0f;
}

static int hits_crowd(const Obj *o, float hw, float hh)
{
    return o->x < px + hw &&
           o->x + o->w > px - hw &&
           o->y < CROWD_Y + hh &&
           o->y + o->h > CROWD_Y - hh;
}

static void reset_game(void)
{
    memset(objs, 0, sizeof(objs));
    zcount = 10;
    score = 0;
    frame = 0;
    px = ROAD_X + ROAD_W / 2;
    scroll = 6.0f;
    wave_on = 0;
    wave_y = 0;
}

/* ---------- Lógica ---------- */
static void update(void)
{
    int i;
    float hw, hh;

    if (pad.button[BT_START]) {
        reset_game();
        return;
    }

    frame++;

    if (pad.button[BT_LEFT])  px -= 7.0f;
    if (pad.button[BT_RIGHT]) px += 7.0f;
    px = clampf(px, ROAD_X + 60, ROAD_X + ROAD_W - 60);

    scroll = 6.0f + fminf((float)frame / 1200.0f, 6.0f);

    /* Spawns */
    if (frame % 22 == 0)  spawn_human();
    if (frame % 260 == 0) spawn_gates();
    if (frame > 200 && frame % 330 == 0) spawn_wall_row();

    /* Movimento e colisões */
    hw = crowd_hw();
    hh = crowd_hh();

    for (i = 0; i < MAX_OBJ; i++) {
        Obj *o = &objs[i];
        if (!o->active) continue;

        o->y += scroll;
        if (o->y > SCR_H) {
            o->active = 0;
            continue;
        }

        if (!hits_crowd(o, hw, hh)) continue;

        if (o->type == OBJ_HUMAN) {
            zcount += 1;
            score += 10;
            o->active = 0;
        } else if (o->type == OBJ_GATE) {
            if (o->op == OP_ADD) zcount += o->value;
            else                 zcount *= o->value;
            if (zcount > MAX_ZOMBIES) zcount = MAX_ZOMBIES;
            o->active = 0;
        } else if (o->type == OBJ_WALL) {
            zcount -= zcount / 3 + 1;
            o->active = 0;
        }

        hw = crowd_hw();
        hh = crowd_hh();
    }

    /* Tsunami de zumbis */
    if (!wave_on && frame > 0 && frame % 1500 == 0) {
        wave_on = 1;
        wave_y = -20.0f;
    }
    if (wave_on) {
        wave_y += 14.0f;
        for (i = 0; i < MAX_OBJ; i++) {
            Obj *o = &objs[i];
            if (!o->active || o->type != OBJ_HUMAN) continue;
            if (o->y <= wave_y && o->y >= wave_y - 14.0f) {
                o->active = 0;
                score += 25;
                zcount += 1;
            }
        }
        if (wave_y > SCR_H) wave_on = 0;
    }

    zcount = clampi(zcount, 0, MAX_ZOMBIES);
    if (zcount <= 0) reset_game();
}

/* ---------- Desenho ---------- */
static void draw_object(const Obj *o)
{
    int x = (int)o->x, y = (int)o->y;
    int k, n;

    if (o->type == OBJ_HUMAN) {
        fill_rect(x, y, 18, 26, RGB(240, 120, 170));
        fill_circle(x + 9, y - 6, 7, RGB(250, 210, 170));
    } else if (o->type == OBJ_GATE) {
        u32 col = (o->op == OP_ADD) ? RGB(46, 204, 64) : RGB(176, 64, 255);
        fill_rect(x, y, (int)o->w, (int)o->h, col);
        n = o->value > 10 ? 10 : o->value;
        for (k = 0; k < n; k++)
            fill_rect(x + 8 + k * 16, y + 12, 10, 16, RGB(255, 255, 255));
    } else if (o->type == OBJ_WALL) {
        fill_rect(x, y, (int)o->w, (int)o->h, RGB(60, 50, 50));
        fill_rect(x, y + (int)o->h - 6, (int)o->w, 6, RGB(90, 70, 70));
    }
}

static void draw_crowd(void)
{
    int i;
    int n = zcount > 200 ? 200 : zcount;

    for (i = 0; i < n; i++) {
        float a = i * 2.39996f;                    /* espiral dourada */
        float r = 6.0f * sqrtf((float)i + 1.0f);
        int cx = (int)(px + cosf(a) * r);
        int cy = (int)(CROWD_Y + sinf(a) * r * 0.7f);
        fill_circle(cx, cy, 9, (i % 3) ? RGB(70, 180, 70) : RGB(40, 120, 40));
    }
}

static void render(void)
{
    int i, y, off;

    /* Grama */
    fill_rect(0, 0, SCR_W, SCR_H, RGB(90, 160, 70));

    /* Estrada */
    fill_rect(ROAD_X, 0, ROAD_W, SCR_H, RGB(72, 72, 84));

    /* Faixas descendo */
    off = ((int)(frame * scroll)) % 60;
    for (y = off - 60; y < SCR_H; y += 60) {
        fill_rect(ROAD_X + LANE_W - 3, y, 6, 30, RGB(230, 230, 230));
        fill_rect(ROAD_X + 2 * LANE_W - 3, y, 6, 30, RGB(230, 230, 230));
    }

    /* Objetos */
    for (i = 0; i < MAX_OBJ; i++)
        if (objs[i].active)
            draw_object(&objs[i]);

    /* Tsunami */
    if (wave_on) {
        fill_rect(ROAD_X, (int)wave_y - 7, ROAD_W, 14, RGB(40, 120, 60));
        fill_rect(ROAD_X, (int)wave_y - 2, ROAD_W, 4, RGB(200, 255, 220));
    }

    /* Horda */
    draw_crowd();

    /* HUD: pontos (amarelo) e tamanho da horda (verde) */
    fill_rect(20, 20, (int)fminf((float)score, 1200.0f), 14, RGB(255, 220, 0));
    fill_rect(20, 40, (int)fminf((float)zcount, 1200.0f), 10, RGB(60, 200, 60));
}

/* ---------- Vídeo: inicialização e flip ---------- */
static void video_init(void)
{
    videoState state;
    videoConfiguration vc;
    void *host;
    int i;

    videoGetState(0, 0, &state);

    memset(&vc, 0, sizeof(vc));
    vc.resolution = VIDEO_RESOLUTION_720;
    vc.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch      = SCR_W * 4;
    vc.aspect     = VIDEO_ASPECT_AUTO;
    videoConfigure(0, &vc, NULL, 0);
    videoGetState(0, 0, &state);

    host = memalign(1024 * 1024, HOST_SIZE);
    rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);

    for (i = 0; i < 2; i++) {
        bufs[i] = rsxMemalign(64, SCR_W * SCR_H * 4);
        rsxAddressToOffset(bufs[i], &offs[i]);
        gcmSetDisplayBuffer(i, offs[i], SCR_W * 4, SCR_W, SCR_H);
    }
    fb = bufs[cur];
}

static void flip(void)
{
    gcmSetFlip(ctx, cur);
    rsxFlushBuffer(ctx);
    gcmSetWaitFlip(ctx);       /* sincroniza com o vsync */
    cur ^= 1;
    fb = bufs[cur];
}

/* ---------- Main ---------- */
int main(void)
{
    video_init();
    ioPadInit(7);
    reset_game();

    while (1) {
        ioPadGetData(0, &pad);
        update();
        render();
        flip();
    }

    return 0;
}