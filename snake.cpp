// ============================================================================
//  CYBER SNAKE : NEON GARDEN
//  2D cyberpunk-styled Snake for Linux (SDL2).  No external asset files:
//  every graphic, font and sound is generated in code.
//
//  Controls : W A S D (or arrow keys)   P / SPACE = pause   ESC = menu
//             F11 = fullscreen          ENTER = confirm
// ============================================================================
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

// ------------------------------------------------------------------ constants
static const int   W = 960, H = 720;          // logical window size
static const int   CELL = 28, COLS = 32, ROWS = 22;
static const int   FX = 32, FY = 72;          // playfield origin
static const float PI = 3.14159265f;

struct Col { Uint8 r, g, b; };
struct V2  { float x, y; };
struct P   { int x, y; bool operator==(const P& o) const { return x == o.x && y == o.y; } };

static Col mix(Col a, Col b, float t) {
    t = std::max(0.f, std::min(1.f, t));
    return Col{(Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t), (Uint8)(a.b + (b.b - a.b) * t)};
}

// ------------------------------------------------------------------ globals
static SDL_Window*   win = nullptr;
static SDL_Renderer* R = nullptr;
static SDL_Texture *texCircle = nullptr, *texGlow = nullptr, *texBg = nullptr, *texScan = nullptr, *texVig = nullptr;

static uint32_t rngState = 0x1234ABCDu;
static uint32_t rnd() { rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5; return rngState; }
static int   rndi(int n) { return (int)(rnd() % (uint32_t)n); }
static float rndf() { return (rnd() & 0xFFFFFF) / 16777216.f; }

enum State { MENU, SETTINGS, PLAYING, PAUSED, GAMEOVER };
static State state = MENU;
static double T = 0;   // seconds since start

static const int         STEP_MS[4]    = {180, 120, 85, 55};
static const char*       SPEED_NAME[4] = {"SLOW", "NORMAL", "FAST", "INSANE"};
static const Col         SKIN[3]       = {{235, 240, 255}, {60, 255, 90}, {40, 150, 255}};
static const char*       SKIN_NAME[3]  = {"WHITE", "GREEN", "BLUE"};
static struct { int speed = 1; int color = 1; bool sound = true; } cfg;
static int menuSel = 0, setSel = 0;

// ------------------------------------------------------------------ audio
struct Voice { bool on; float f, slide, vol, ph; int wave, pos, len; };
static Voice   voices[8] = {};
static SDL_AudioDeviceID adev = 0;
static volatile int gSfx = 1, gMusic = 1;
static int gRate = 44100;

static const int BASS_PAT[16] = {0, 99, 0, 99, 0, 99, 12, 99, 3, 99, 3, 99, 7, 99, 5, 99};
static const int ARP_PAT[16]  = {0, 3, 7, 12, 7, 3, 0, 3, 5, 8, 12, 17, 12, 8, 5, 8};
static const int ROOTS[4]     = {0, -4, -2, -5};

static void audioCb(void*, Uint8* stream, int len) {
    static int mStep = 0, mCount = 0;
    static float bassF = 0, bassEnv = 0, bassPh = 0, arpF = 220, arpEnv = 0, arpPh = 0;
    static uint32_t nz = 22222;
    float* out = (float*)stream;
    int n = len / (int)sizeof(float);
    for (int i = 0; i < n; i++) {
        float s = 0;
        if (gMusic) {
            if (mCount <= 0) {
                mCount = (int)(gRate * 60.f / (112.f * 4.f));
                int bar = (mStep / 16) % 4, st = mStep % 16, root = ROOTS[bar];
                int b = BASS_PAT[st];
                bassF = (b == 99) ? 0.f : 55.f * powf(2.f, (root + b) / 12.f);
                bassEnv = (b == 99) ? 0.f : 1.f;
                arpF = 220.f * powf(2.f, (root + ARP_PAT[st]) / 12.f);
                arpEnv = 1.f;
                mStep++;
            }
            mCount--;
            if (bassF > 0) {
                bassPh += bassF / gRate; if (bassPh >= 1) bassPh -= 1;
                s += (bassPh * 2 - 1) * bassEnv * 0.09f;
            }
            bassEnv *= 0.99985f;
            arpPh += arpF / gRate; if (arpPh >= 1) arpPh -= 1;
            s += (arpPh < 0.25f ? 1.f : -1.f) * arpEnv * 0.025f;
            arpEnv *= 0.9992f;
        }
        for (auto& v : voices) {
            if (!v.on) continue;
            v.ph += v.f / gRate; if (v.ph >= 1) v.ph -= 1;
            float w;
            switch (v.wave) {
                case 0:  w = v.ph < 0.5f ? 1.f : -1.f; break;
                case 1:  w = v.ph * 2 - 1; break;
                case 2:  w = sinf(v.ph * 2 * PI); break;
                default: nz ^= nz << 13; nz ^= nz >> 17; nz ^= nz << 5; w = ((nz & 0xFFFF) / 32768.f) - 1.f; break;
            }
            s += w * v.vol * (1.f - (float)v.pos / v.len);
            v.f += v.slide / gRate; if (v.f < 20) v.f = 20;
            if (++v.pos >= v.len) v.on = false;
        }
        out[i] = std::max(-1.f, std::min(1.f, s * 0.8f));
    }
}
static void playTone(int wave, float f, float slide, float vol, float sec) {
    if (!adev || !gSfx) return;
    SDL_LockAudioDevice(adev);
    for (auto& v : voices) if (!v.on) { v = Voice{true, f, slide, vol, 0.f, wave, 0, std::max(1, (int)(sec * gRate))}; break; }
    SDL_UnlockAudioDevice(adev);
}
static void sfxMove()   { playTone(0, 660, 0, 0.10f, 0.04f); }
static void sfxSelect() { playTone(0, 880, 900, 0.12f, 0.10f); }
static void sfxEat()    { playTone(0, 500, 2600, 0.14f, 0.12f); playTone(2, 1200, 0, 0.10f, 0.15f); }
static void sfxDie()    { playTone(1, 320, -600, 0.20f, 0.70f); playTone(3, 0, 0, 0.18f, 0.50f); }
static void applySound() { gSfx = gMusic = cfg.sound ? 1 : 0; }

// ------------------------------------------------------------------ textures
template <class F>
static SDL_Texture* makeTexture(int w, int h, F f, SDL_BlendMode bm) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    Uint32* px = (Uint32*)s->pixels;
    int pitch = s->pitch / 4;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[y * pitch + x] = f(x, y);
    SDL_Texture* t = SDL_CreateTextureFromSurface(R, s);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(t, bm);
    return t;
}
static Uint32 argb(int a, int r, int g, int b) { return ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b; }

// ------------------------------------------------------------------ primitives
static void fillRect(float x, float y, float w, float h, Col c, int a = 255) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, (Uint8)a);
    SDL_FRect r{x, y, w, h};
    SDL_RenderFillRectF(R, &r);
}
static void circle(float cx, float cy, float rad, Col c, int a = 255) {
    SDL_SetTextureColorMod(texCircle, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(texCircle, (Uint8)a);
    SDL_FRect d{cx - rad, cy - rad, rad * 2, rad * 2};
    SDL_RenderCopyF(R, texCircle, nullptr, &d);
}
static void glow(float cx, float cy, float rad, Col c, int a) {
    SDL_SetTextureColorMod(texGlow, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(texGlow, (Uint8)std::min(255, a));
    SDL_FRect d{cx - rad, cy - rad, rad * 2, rad * 2};
    SDL_RenderCopyF(R, texGlow, nullptr, &d);
}
static void line(float x1, float y1, float x2, float y2, Col c, int a = 255) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, (Uint8)a);
    SDL_RenderDrawLineF(R, x1, y1, x2, y2);
}

// ------------------------------------------------------------------ 5x7 font
struct Glyph { char c; uint8_t r[7]; };
static const Glyph FONT[] = {
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {':', {0b00000, 0b00100, 0b00000, 0b00000, 0b00000, 0b00100, 0b00000}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b00100, 0b00100, 0b01000}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {'?', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
    {'<', {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010}},
    {'>', {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000}},
    {'/', {0b00001, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b10000}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
};
static const uint8_t* glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (const auto& g : FONT) if (g.c == c) return g.r;
    return nullptr;
}
static int textW(const std::string& s, int sc) { return s.empty() ? 0 : (int)s.size() * 6 * sc - sc; }
static void textRaw(const std::string& s, float x, float y, int sc, Col c, int a) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, (Uint8)a);
    for (char ch : s) {
        const uint8_t* g = glyph(ch);
        if (g) for (int row = 0; row < 7; row++) {
            int col = 0;
            while (col < 5) {   // merge horizontal runs into one rect
                if (g[row] & (1 << (4 - col))) {
                    int run = 1;
                    while (col + run < 5 && (g[row] & (1 << (4 - col - run)))) run++;
                    SDL_FRect r{x + col * sc, y + row * sc, (float)(run * sc), (float)sc};
                    SDL_RenderFillRectF(R, &r);
                    col += run;
                } else col++;
            }
        }
        x += 6 * sc;
    }
}
// neon text: soft halo + bright core.  (x is the centre when center == true)
static void neonText(const std::string& s, float x, float y, int sc, Col c, bool center = false, int halo = 40) {
    if (center) x -= textW(s, sc) / 2.f;
    int o = std::max(1, sc / 3);
    if (halo > 0) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        if (dx || dy) textRaw(s, x + dx * o, y + dy * o, sc, c, halo);
    textRaw(s, x, y, sc, mix(c, Col{255, 255, 255}, 0.35f), 255);
}
static void glitchText(const std::string& s, float cx, float y, int sc, Col a, Col b) {
    float x = cx - textW(s, sc) / 2.f;
    float o = sc * 0.28f + sinf((float)T * 2.f) * sc * 0.06f;
    textRaw(s, x - o, y, sc, a, 200);
    textRaw(s, x + o, y, sc, b, 200);
    textRaw(s, x, y, sc, Col{245, 250, 255}, 255);
}

// ------------------------------------------------------------------ fruit
enum { APPLE, CHERRY, BANANA, GRAPE, ORANGE, NFRUIT };
static const int FRUIT_PTS[NFRUIT]  = {10, 20, 15, 25, 10};
static const Col FRUIT_GLOW[NFRUIT] = {{255, 50, 70}, {255, 40, 90}, {255, 225, 60}, {170, 70, 255}, {255, 150, 30}};

static void drawFruit(int type, float cx, float cy, float t) {
    cy += sinf(t * 4.f) * 1.6f;
    float pulse = 0.85f + 0.15f * sinf(t * 6.f);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    glow(cx, cy, 30 * pulse, FRUIT_GLOW[type], 110);
    const Col leaf{70, 230, 80};
    switch (type) {
        case APPLE:
            circle(cx, cy + 1.5f, 11.5f, Col{110, 10, 25});
            circle(cx, cy + 1.5f, 10.f, Col{235, 45, 60});
            circle(cx - 3.5f, cy - 1.5f, 2.8f, Col{255, 255, 255}, 170);
            fillRect(cx - 1, cy - 12, 2, 5, Col{120, 70, 30});
            circle(cx + 4, cy - 9, 3.4f, leaf);
            break;
        case CHERRY: {
            line(cx - 5.5f, cy - 1, cx + 1, cy - 11, Col{90, 200, 70});
            line(cx - 4.5f, cy - 1, cx + 2, cy - 11, Col{90, 200, 70});
            line(cx + 5.5f, cy, cx + 1, cy - 11, Col{90, 200, 70});
            line(cx + 6.5f, cy, cx + 2, cy - 11, Col{90, 200, 70});
            circle(cx + 4, cy - 11, 3.f, leaf);
            float xs[2] = {cx - 5.5f, cx + 5.5f}, ys[2] = {cy + 5, cy + 6};
            for (int i = 0; i < 2; i++) {
                circle(xs[i], ys[i], 7.5f, Col{100, 0, 30});
                circle(xs[i], ys[i], 6.f, Col{230, 30, 70});
                circle(xs[i] - 2, ys[i] - 2, 1.6f, Col{255, 255, 255}, 170);
            }
            break;
        }
        case BANANA:
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < 8; i++) {
                    float a = PI * (0.12f + 0.76f * i / 7.f);
                    float px = cx + cosf(a) * 11.f, py = cy - 7 + sinf(a) * 13.f;
                    float rr = 4.8f - fabsf(i - 3.5f) * 0.35f;
                    if (pass == 0) circle(px, py, rr + 1.5f, Col{150, 110, 10});
                    else circle(px, py, rr, (i == 0 || i == 7) ? Col{140, 90, 30} : Col{255, 225, 50});
                }
            break;
        case GRAPE: {
            fillRect(cx - 1, cy - 12, 2, 6, Col{110, 70, 30});
            circle(cx + 4, cy - 10, 3.2f, leaf);
            const float gx[6] = {-5, 0, 5, -2.5f, 2.5f, 0}, gy[6] = {-3, -4, -3, 2, 2, 7};
            for (int i = 0; i < 6; i++) circle(cx + gx[i], cy + gy[i], 5.6f, Col{60, 10, 110});
            for (int i = 0; i < 6; i++) {
                circle(cx + gx[i], cy + gy[i], 4.4f, Col{165, 70, 240});
                circle(cx + gx[i] - 1.2f, cy + gy[i] - 1.5f, 1.2f, Col{255, 255, 255}, 120);
            }
            break;
        }
        case ORANGE:
            circle(cx, cy + 1, 11.5f, Col{150, 70, 0});
            circle(cx, cy + 1, 10.f, Col{255, 150, 30});
            circle(cx - 3.5f, cy - 2, 2.8f, Col{255, 255, 255}, 150);
            circle(cx, cy - 8.5f, 1.2f, Col{140, 80, 10});
            circle(cx + 4, cy - 10, 3.f, leaf);
            break;
    }
}

// ------------------------------------------------------------------ snake drawing
static void connect(V2 a, V2 b, float rad, Col c) {
    if (fabsf(a.x - b.x) > fabsf(a.y - b.y)) fillRect(fminf(a.x, b.x), a.y - rad, fabsf(a.x - b.x), rad * 2, c);
    else                                     fillRect(a.x - rad, fminf(a.y, b.y), rad * 2, fabsf(a.y - b.y), c);
}
// p[0] is the head.  fwd = facing direction.  flash = 0..1 red tint (death)
static void drawSnake(const std::vector<V2>& p, V2 fwd, float r, Col base, float flash, bool detail) {
    int n = (int)p.size();
    if (!n) return;
    Col dark = mix(base, Col{0, 0, 0}, 0.6f), ol{3, 12, 8}, red{255, 40, 60};
    auto colAt = [&](int i) {
        float t = n > 1 ? (float)i / (n - 1) : 0.f;
        Col c = mix(base, dark, t * 0.6f);
        return flash > 0 ? mix(c, red, flash) : c;
    };
    auto rad = [&](int i) { return i == 0 ? r * 1.18f : r * (1.f - 0.12f * (n > 1 ? (float)i / (n - 1) : 0.f)); };
    auto linked = [&](int i) {
        float dx = p[i].x - p[i + 1].x, dy = p[i].y - p[i + 1].y, d = sqrtf(dx * dx + dy * dy);
        return d > r * 1.6f && d < CELL * 1.2f;
    };
    // neon glow under everything
    Col gc = flash > 0 ? mix(base, red, flash) : base;
    for (int i = n - 1; i >= 0; i--) glow(p[i].x, p[i].y, rad(i) * 2.6f, gc, 38);
    // dark outline
    for (int i = n - 1; i >= 0; i--) {
        circle(p[i].x, p[i].y, rad(i) + 2.2f, ol);
        if (i + 1 < n && linked(i)) connect(p[i], p[i + 1], std::min(rad(i), rad(i + 1)) + 2.2f, ol);
    }
    V2 f = fwd, pp{-f.y, f.x};
    float hr = rad(0);
    if (detail && (SDL_GetTicks() / 350) % 4 == 0) {   // flicking tongue
        V2 s0{p[0].x + f.x * hr, p[0].y + f.y * hr}, m{s0.x + f.x * 8, s0.y + f.y * 8};
        Col tc{255, 40, 110};
        line(s0.x, s0.y, m.x, m.y, tc);
        line(m.x, m.y, m.x + f.x * 5 + pp.x * 4, m.y + f.y * 5 + pp.y * 4, tc);
        line(m.x, m.y, m.x + f.x * 5 - pp.x * 4, m.y + f.y * 5 - pp.y * 4, tc);
    }
    // body colour
    for (int i = n - 1; i >= 0; i--) {
        Col c = colAt(i);
        circle(p[i].x, p[i].y, rad(i), c);
        if (i + 1 < n && linked(i)) connect(p[i], p[i + 1], std::min(rad(i), rad(i + 1)), mix(c, colAt(i + 1), 0.5f));
    }
    // glowing core + shine
    for (int i = n - 1; i >= 0; i--) {
        if (i % 2 == 0) circle(p[i].x, p[i].y, rad(i) * 0.30f, mix(colAt(i), Col{255, 255, 255}, 0.6f), 170);
        circle(p[i].x - rad(i) * 0.3f, p[i].y - rad(i) * 0.35f, rad(i) * 0.25f, Col{255, 255, 255}, 55);
    }
    if (detail) {   // eyes
        for (int s = -1; s <= 1; s += 2) {
            float ex = p[0].x + f.x * hr * 0.35f + pp.x * hr * 0.5f * s;
            float ey = p[0].y + f.y * hr * 0.35f + pp.y * hr * 0.5f * s;
            circle(ex, ey, hr * 0.34f, Col{255, 255, 255});
            circle(ex + f.x * 1.6f, ey + f.y * 1.6f, hr * 0.17f, Col{30, 0, 50});
        }
    }
}

// ------------------------------------------------------------------ particles / floating text
struct Part { float x, y, vx, vy, life, maxLife; Col c; float sz; };
static std::vector<Part> parts;
struct FText { float x, y, life; std::string s; Col c; };
static std::vector<FText> ftexts;
static void burst(float x, float y, Col c, int n, float speed) {
    for (int i = 0; i < n; i++) {
        float a = rndf() * 2 * PI, sp = speed * (0.3f + rndf());
        float l = 0.5f + rndf() * 0.5f;
        parts.push_back({x, y, cosf(a) * sp, sinf(a) * sp, l, l, c, 1.5f + rndf() * 2.f});
    }
}
static void updateFx(float dt) {
    for (auto& p : parts) { p.x += p.vx * dt; p.y += p.vy * dt; p.vx *= 0.96f; p.vy *= 0.96f; p.life -= dt; }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Part& p) { return p.life <= 0; }), parts.end());
    for (auto& t : ftexts) { t.y -= 34 * dt; t.life -= dt; }
    ftexts.erase(std::remove_if(ftexts.begin(), ftexts.end(), [](const FText& t) { return t.life <= 0; }), ftexts.end());
}
static void drawFx() {
    for (auto& p : parts) {
        float k = p.life / p.maxLife;
        glow(p.x, p.y, p.sz * 4.f, p.c, (int)(200 * k));
        circle(p.x, p.y, p.sz * k, Col{255, 255, 255}, (int)(255 * k));
    }
    for (auto& t : ftexts) {
        int a = (int)(255 * std::min(1.f, t.life * 2.f));
        textRaw(t.s, t.x - textW(t.s, 2) / 2.f, t.y, 2, mix(t.c, Col{255, 255, 255}, 0.4f), a);
    }
}

// ------------------------------------------------------------------ game state
static std::deque<P> snake;
static std::vector<uint8_t> occ;
static P dir{1, 0};
static std::deque<P> dirQ;
static P fruit{0, 0};
static int fruitType = 0;
static bool hasFruit = false;
static int score = 0, best = 0;
static double acc = 0, goT = 0;
static float shake = 0;
static std::string deathMsg1, deathMsg2;
static P biteCell{0, 0};
#ifdef HARNESS
static int speedOverride = 0;
#endif

static inline int idx(P p) { return p.y * COLS + p.x; }
static V2 cc(P p) {
    float ox = shake > 0 ? (rndf() - 0.5f) * shake * 10.f : 0, oy = shake > 0 ? (rndf() - 0.5f) * shake * 10.f : 0;
    return V2{FX + p.x * CELL + CELL / 2.f + ox, FY + p.y * CELL + CELL / 2.f + oy};
}

static void spawnFruit() {
    std::vector<int> freeCells;
    for (int i = 0; i < COLS * ROWS; i++) if (!occ[i]) freeCells.push_back(i);
    if (freeCells.empty()) { hasFruit = false; return; }
    int c = freeCells[rndi((int)freeCells.size())];
    fruit = P{c % COLS, c / COLS};
    fruitType = rndi(NFRUIT);
    hasFruit = true;
}
static void startGame() {
    snake.clear(); dirQ.clear(); parts.clear(); ftexts.clear();
    occ.assign(COLS * ROWS, 0);
    int hx = COLS / 2, hy = ROWS / 2;
    for (int i = 0; i < 4; i++) { P c{hx - i, hy}; snake.push_back(c); occ[idx(c)] = 1; }
    dir = P{1, 0};
    score = 0; acc = 0; goT = 0; shake = 0;
    spawnFruit();
    state = PLAYING;
}
static void die(const char* l1, const char* l2, P at) {
    deathMsg1 = l1; deathMsg2 = l2; biteCell = at;
    best = std::max(best, score);
    state = GAMEOVER; goT = 0; shake = 1.f;
    V2 c = cc(at);
    burst(c.x, c.y, Col{255, 60, 60}, 40, 260);
    burst(c.x, c.y, Col{255, 220, 0}, 20, 180);
    sfxDie();
}
static void queueDir(P d) {
    P last = dirQ.empty() ? dir : dirQ.back();
    if ((d.x == last.x && d.y == last.y) || (d.x == -last.x && d.y == -last.y)) return;  // ignore same / reverse
    if (dirQ.size() < 2) dirQ.push_back(d);
}
static void step() {
    if (!dirQ.empty()) { dir = dirQ.front(); dirQ.pop_front(); }
    P h = snake.front();
    P n{(h.x + dir.x + COLS) % COLS, (h.y + dir.y + ROWS) % ROWS};   // walls wrap around
    if ((int)snake.size() >= COLS * ROWS) {                            // board completely full
        die("MAXIMUM SIZE REACHED!", "NO ROOM LEFT - THE SNAKE BIT ITSELF", n);
        return;
    }
    bool grow = hasFruit && n == fruit;
    P tail = snake.back();
    if (occ[idx(n)] && !(!grow && n == tail)) {                        // the ONLY way to lose
        die("OUCH!", "THE SNAKE BIT ITSELF", n);
        return;
    }
    if (!grow) { occ[idx(tail)] = 0; snake.pop_back(); }
    snake.push_front(n);
    occ[idx(n)] = 1;
    if (grow) {
        int pts = FRUIT_PTS[fruitType];
        score += pts; best = std::max(best, score);
        V2 c = cc(n);
        burst(c.x, c.y, FRUIT_GLOW[fruitType], 26, 200);
        ftexts.push_back({c.x, c.y - 10, 0.9f, "+" + std::to_string(pts), FRUIT_GLOW[fruitType]});
        sfxEat();
        spawnFruit();
    }
}

// ------------------------------------------------------------------ background
static void buildBg() {
    if (texBg) SDL_DestroyTexture(texBg);
    texBg = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W, H);
    SDL_SetRenderTarget(R, texBg);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    uint32_t saved = rngState; rngState = 0xC0FFEE11u;
    // night-city gradient
    for (int y = 0; y < H; y++) {
        float t = (float)y / H;
        SDL_SetRenderDrawColor(R, (Uint8)(18 - 14 * t), (Uint8)(8 + 14 * t), (Uint8)(40 - 14 * t), 255);
        SDL_RenderDrawLine(R, 0, y, W, y);
    }
    // circuit traces
    for (int k = 0; k < 80; k++) {
        int x = rndi(W), y = rndi(H);
        Col c = (rnd() & 1) ? Col{255, 40, 200} : Col{0, 220, 255};
        int a = 40 + rndi(50);
        for (int s = 0; s < 4; s++) {
            int len = 20 + rndi(90), mode = rndi(3), sg = (rnd() & 1) ? 1 : -1, nx = x, ny = y;
            if (mode == 0) nx += sg * len; else if (mode == 1) ny += sg * len; else { nx += sg * len / 2; ny += len / 2; }
            line((float)x, (float)y, (float)nx, (float)ny, c, a);
            x = nx; y = ny;
        }
        circle((float)x, (float)y, 3, c, a + 60);
    }
    // skyline behind the HUD
    for (int x = 0; x < W;) {
        int bw = 30 + rndi(40), bh = 12 + rndi(34), top = FY - 12 - bh;
        fillRect((float)x, (float)top, (float)bw, (float)bh + 12, Col{12, 9, 30});
        if (rnd() % 3 == 0) line((float)x, (float)top, (float)(x + bw), (float)top, (rnd() & 1) ? Col{255, 40, 200} : Col{0, 220, 255}, 140);
        for (int wy = top + 4; wy < FY - 14; wy += 6) for (int wx = x + 4; wx < x + bw - 3; wx += 6)
            if (rnd() % 3 == 0) {
                Col wc = (rnd() % 3 == 0) ? Col{255, 230, 90} : ((rnd() & 1) ? Col{0, 220, 255} : Col{255, 60, 200});
                fillRect((float)wx, (float)wy, 3, 3, wc, 150);
            }
        x += bw + rndi(6);
    }
    // garden: mown lawn stripes, grass blades, flowers
    fillRect(FX - 16, FY - 16, COLS * CELL + 32, ROWS * CELL + 32, Col{0, 0, 0}, 130);
    for (int cy = 0; cy < ROWS; cy++) for (int cx = 0; cx < COLS; cx++) {
        Col b = ((cx + cy) & 1) ? Col{22, 92, 38} : Col{28, 108, 46};
        int j = rndi(7) - 3;
        fillRect((float)(FX + cx * CELL), (float)(FY + cy * CELL), CELL, CELL, Col{(Uint8)(b.r + j), (Uint8)(b.g + j * 2), (Uint8)(b.b + j)});
        for (int k = 0; k < 5; k++) {
            float bx = (float)(FX + cx * CELL + rndi(CELL)), by = (float)(FY + cy * CELL + 5 + rndi(CELL - 6));
            float len = (float)(3 + rndi(5)), tilt = (float)(rndi(5) - 2);
            line(bx, by, bx + tilt, by - len, Col{(Uint8)(30 + rndi(30)), (Uint8)(125 + rndi(50)), (Uint8)(40 + rndi(30))}, 120);
        }
    }
    for (int k = 0; k < 75; k++) {
        float fx = (float)(FX + 6 + rndi(COLS * CELL - 12)), fy = (float)(FY + 6 + rndi(ROWS * CELL - 12));
        Col fc[4] = {{255, 120, 220}, {120, 240, 255}, {255, 240, 120}, {255, 255, 255}};
        Col c = fc[rndi(4)];
        for (int q = 0; q < 4; q++) circle(fx + (q < 2 ? (q ? 2.6f : -2.6f) : 0), fy + (q >= 2 ? (q == 3 ? 2.6f : -2.6f) : 0), 1.7f, c, 175);
        circle(fx, fy, 1.3f, Col{255, 230, 80}, 200);
    }
    for (int i = 0; i <= COLS; i++) line((float)(FX + i * CELL), (float)FY, (float)(FX + i * CELL), (float)(FY + ROWS * CELL), Col{0, 255, 220}, 30);
    for (int j = 0; j <= ROWS; j++) line((float)FX, (float)(FY + j * CELL), (float)(FX + COLS * CELL), (float)(FY + j * CELL), Col{0, 255, 220}, 30);
    SDL_SetRenderTarget(R, nullptr);
    rngState = saved;
}

// ------------------------------------------------------------------ screens
static void panel(float x, float y, float w, float h, Col edge) {
    fillRect(x, y, w, h, Col{4, 6, 16}, 205);
    fillRect(x, y, w, 2, edge); fillRect(x, y + h - 2, w, 2, edge);
    fillRect(x, y, 2, h, edge); fillRect(x + w - 2, y, 2, h, edge);
    glow(x, y, 30, edge, 90); glow(x + w, y, 30, edge, 90); glow(x, y + h, 30, edge, 90); glow(x + w, y + h, 30, edge, 90);
}
static void drawDemoSnake(float y) {
    float hx = fmodf((float)T * 95.f, W + 420.f) - 60.f;
    std::vector<V2> pts;
    for (int i = 0; i < 20; i++) { float x = hx - i * 13.f; pts.push_back(V2{x, y + sinf(x * 0.02f) * 26.f}); }
    V2 f{1.f, cosf(hx * 0.02f) * 0.52f};
    float l = sqrtf(f.x * f.x + f.y * f.y); f.x /= l; f.y /= l;
    drawSnake(pts, f, 11.f, SKIN[cfg.color], 0, true);
}
static void drawMenu() {
    fillRect(0, 0, W, H, Col{0, 0, 0}, 150);
    neonText("GREETINGS, PLAYER!", W / 2.f, 62, 4, Col{255, 225, 40}, true);
    neonText("WELCOME TO THE NEON GARDEN", W / 2.f, 108, 2, Col{0, 230, 255}, true, 30);
    glitchText("SNAKE", W / 2.f, 160, 16, Col{255, 30, 200}, Col{0, 230, 255});
    glow(W / 2.f, 215, 330, Col{255, 30, 200}, 22);
    const char* items[3] = {"PLAY", "SETTINGS", "QUIT"};
    for (int i = 0; i < 3; i++) {
        float y = 330.f + i * 78.f;
        bool sel = (i == menuSel);
        if (sel) {
            fillRect(W / 2.f - 270, y - 12, 540, 62, Col{255, 30, 200}, 38);
            fillRect(W / 2.f - 270, y - 12, 540, 2, Col{0, 230, 255}, 200);
            fillRect(W / 2.f - 270, y + 48, 540, 2, Col{0, 230, 255}, 200);
            float o = sinf((float)T * 8.f) * 5.f;
            neonText(">", W / 2.f - 200 - o, y, 5, Col{255, 225, 40});
            neonText("<", W / 2.f + 200 - 25 + o, y, 5, Col{255, 225, 40});
        }
        neonText(items[i], W / 2.f, y, 5, sel ? Col{0, 255, 230} : Col{140, 160, 185}, true, sel ? 50 : 0);
    }
    drawDemoSnake(612);
    textRaw("W S  SELECT     ENTER  CONFIRM     F11  FULLSCREEN", W / 2.f - textW("W S  SELECT     ENTER  CONFIRM     F11  FULLSCREEN", 2) / 2.f, 688, 2, Col{110, 190, 180}, 255);
}
static void drawSettings() {
    fillRect(0, 0, W, H, Col{0, 0, 0}, 150);
    glitchText("SETTINGS", W / 2.f, 70, 9, Col{255, 30, 200}, Col{0, 230, 255});
    const char* labels[4] = {"SNAKE SPEED", "SNAKE COLOR", "SOUND", "BACK"};
    std::string vals[4] = {SPEED_NAME[cfg.speed], SKIN_NAME[cfg.color], cfg.sound ? "ON" : "OFF", ""};
    for (int i = 0; i < 4; i++) {
        float y = 210.f + i * 86.f;
        bool sel = (i == setSel);
        if (sel) {
            fillRect(110, y - 14, W - 220, 64, Col{255, 30, 200}, 38);
            fillRect(110, y - 14, W - 220, 2, Col{0, 230, 255}, 200);
            fillRect(110, y + 48, W - 220, 2, Col{0, 230, 255}, 200);
        }
        Col lc = sel ? Col{0, 255, 230} : Col{140, 160, 185};
        if (i < 3) {
            neonText(labels[i], 150, y, 4, lc, false, sel ? 50 : 0);
            Col vc = (i == 1) ? SKIN[cfg.color] : (sel ? Col{255, 225, 40} : Col{200, 210, 225});
            if (i == 2) vc = cfg.sound ? Col{60, 255, 120} : Col{255, 70, 90};
            std::string v = sel ? "< " + vals[i] + " >" : vals[i];
            neonText(v, 720, y, 4, vc, true, sel ? 50 : 0);
        } else neonText(labels[i], W / 2.f, y, 4, lc, true, sel ? 50 : 0);
    }
    drawDemoSnake(628);
    std::string hint = "W S  SELECT     A D  CHANGE     ENTER  OK     ESC  BACK";
    textRaw(hint, W / 2.f - textW(hint, 2) / 2.f, 690, 2, Col{110, 190, 180}, 255);
}
static void drawBorder() {
    Col y{255, 221, 0};
    float pulse = 0.75f + 0.25f * sinf((float)T * 3.f);
    float fw = COLS * CELL, fh = ROWS * CELL;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_ADD);
    for (int k = 1; k <= 6; k++) {   // outer neon bloom
        float e = 6 + k * 2.5f;
        int a = (int)(34 * pulse / k * 2.f);
        SDL_SetRenderDrawColor(R, y.r, y.g, y.b, (Uint8)a);
        SDL_FRect t{FX - e, FY - e, fw + 2 * e, 2.5f}, b{FX - e, FY + fh + e - 2.5f, fw + 2 * e, 2.5f};
        SDL_FRect l{FX - e, FY - e, 2.5f, fh + 2 * e}, r{FX + fw + e - 2.5f, FY - e, 2.5f, fh + 2 * e};
        SDL_RenderFillRectF(R, &t); SDL_RenderFillRectF(R, &b); SDL_RenderFillRectF(R, &l); SDL_RenderFillRectF(R, &r);
    }
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    fillRect(FX - 6, FY - 6, fw + 12, 6, y);        // solid yellow frame
    fillRect(FX - 6, FY + fh, fw + 12, 6, y);
    fillRect(FX - 6, FY, 6, fh, y);
    fillRect(FX + fw, FY, 6, fh, y);
    Col hi{255, 248, 170};
    fillRect(FX - 6, FY - 6, fw + 12, 1.5f, hi, 200);   // highlight edge
    fillRect(FX - 6, FY + fh + 4.5f, fw + 12, 1.5f, hi, 120);
    glow(FX - 3, FY - 3, 26, y, 130); glow(FX + fw + 3, FY - 3, 26, y, 130);
    glow(FX - 3, FY + fh + 3, 26, y, 130); glow(FX + fw + 3, FY + fh + 3, 26, y, 130);
}
static void drawHud() {
    neonText("CYBER SNAKE", 32, 10, 4, Col{0, 240, 255});
    neonText("NEON GARDEN", 34, 42, 2, Col{255, 60, 200}, false, 30);
    char buf[64];
    snprintf(buf, sizeof buf, "SCORE %05d", score);
    neonText(buf, W / 2.f, 12, 4, Col{255, 230, 60}, true);
    snprintf(buf, sizeof buf, "SPEED %s", SPEED_NAME[cfg.speed]);
    neonText(buf, W / 2.f, 44, 2, Col{0, 230, 255}, true, 25);
    snprintf(buf, sizeof buf, "LENGTH %d", (int)snake.size());
    neonText(buf, (float)(W - 32 - textW(buf, 2)), 14, 2, Col{60, 255, 120});
    snprintf(buf, sizeof buf, "BEST %d", best);
    neonText(buf, (float)(W - 32 - textW(buf, 2)), 38, 2, Col{255, 60, 200}, false, 30);
    std::string hint = "W A S D  MOVE     P  PAUSE     ESC  MENU     F11  FULLSCREEN";
    textRaw(hint, W / 2.f - textW(hint, 2) / 2.f, 697, 2, Col{110, 190, 180}, 255);
}
static void drawGame() {
    if (hasFruit && state != GAMEOVER) { V2 c = cc(fruit); drawFruit(fruitType, c.x, c.y, (float)T); }
    std::vector<V2> pts;
    pts.reserve(snake.size());
    for (auto& c : snake) pts.push_back(cc(c));
    float flash = 0;
    if (state == GAMEOVER) flash = goT < 1.2 ? (sinf((float)goT * 20.f) > 0 ? 0.8f : 0.15f) : 0.5f;
    drawSnake(pts, V2{(float)dir.x, (float)dir.y}, CELL * 0.42f, SKIN[cfg.color], flash, true);
    drawFx();
    drawBorder();
    drawHud();
    if (state == PAUSED) {
        fillRect(FX, FY, COLS * CELL, ROWS * CELL, Col{0, 0, 0}, 150);
        glitchText("PAUSED", W / 2.f, 280, 10, Col{255, 30, 200}, Col{0, 230, 255});
        neonText("P  RESUME     ESC  MENU", W / 2.f, 390, 3, Col{0, 255, 230}, true, 30);
    }
    if (state == GAMEOVER && goT > 0.9) {
        float a = (float)std::min(1.0, (goT - 0.9) * 3.0);
        (void)a;
        panel(W / 2.f - 300, 200, 600, 330, Col{255, 40, 90});
        glitchText("GAME OVER", W / 2.f, 225, 9, Col{255, 30, 200}, Col{255, 220, 0});
        neonText(deathMsg1, W / 2.f, 315, 2, Col{255, 225, 40}, true, 30);
        neonText(deathMsg2, W / 2.f, 340, 2, Col{255, 120, 140}, true, 30);
        char buf[64];
        snprintf(buf, sizeof buf, "SCORE %d", score);
        neonText(buf, W / 2.f, 382, 4, Col{0, 255, 230}, true);
        snprintf(buf, sizeof buf, "LENGTH %d     BEST %d", (int)snake.size(), best);
        neonText(buf, W / 2.f, 430, 2, Col{60, 255, 120}, true, 30);
        if (fmodf((float)T, 1.0f) < 0.7f) neonText("ENTER  PLAY AGAIN     ESC  MENU", W / 2.f, 480, 2, Col{255, 255, 255}, true, 20);
    }
}
static void render() {
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
    SDL_RenderClear(R);
    SDL_RenderCopy(R, texBg, nullptr, nullptr);
    switch (state) {
        case MENU:     drawMenu(); break;
        case SETTINGS: drawSettings(); break;
        default:       drawGame(); break;
    }
    // CRT scanlines, moving scan bar, vignette
    SDL_RenderCopy(R, texScan, nullptr, nullptr);
    float by = fmodf((float)T * 70.f, H + 120.f) - 60.f;
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_ADD);
    fillRect(0, by, W, 44, Col{40, 255, 220}, 7);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_RenderCopy(R, texVig, nullptr, nullptr);
}

// ------------------------------------------------------------------ input
static bool quitReq = false;
static void handleKey(SDL_Keycode k) {
    if (k == SDLK_F11) {
        bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    bool up = k == SDLK_w || k == SDLK_UP, down = k == SDLK_s || k == SDLK_DOWN;
    bool left = k == SDLK_a || k == SDLK_LEFT, right = k == SDLK_d || k == SDLK_RIGHT;
    bool ok = k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE, back = k == SDLK_ESCAPE;
    switch (state) {
        case MENU:
            if (up)   { menuSel = (menuSel + 2) % 3; sfxMove(); }
            if (down) { menuSel = (menuSel + 1) % 3; sfxMove(); }
            if (ok) {
                sfxSelect();
                if (menuSel == 0) startGame();
                else if (menuSel == 1) { state = SETTINGS; setSel = 0; }
                else quitReq = true;
            }
            break;
        case SETTINGS: {
            if (up)   { setSel = (setSel + 3) % 4; sfxMove(); }
            if (down) { setSel = (setSel + 1) % 4; sfxMove(); }
            int d = right ? 1 : (left ? -1 : 0);
            if (ok && setSel < 3) d = 1;
            if (d) {
                if (setSel == 0) cfg.speed = (cfg.speed + d + 4) % 4;
                if (setSel == 1) cfg.color = (cfg.color + d + 3) % 3;
                if (setSel == 2) { cfg.sound = !cfg.sound; applySound(); }
                sfxSelect();
            } else if ((ok && setSel == 3) || back) { state = MENU; sfxSelect(); }
            break;
        }
        case PLAYING:
            if (up)    queueDir(P{0, -1});
            if (down)  queueDir(P{0, 1});
            if (left)  queueDir(P{-1, 0});
            if (right) queueDir(P{1, 0});
            if (k == SDLK_p || k == SDLK_SPACE) { state = PAUSED; sfxMove(); }
            if (back) { state = MENU; sfxSelect(); }
            break;
        case PAUSED:
            if (k == SDLK_p || ok) { state = PLAYING; sfxMove(); }
            if (back) { state = MENU; sfxSelect(); }
            break;
        case GAMEOVER:
            if (goT > 0.8) {
                if (ok || k == SDLK_r) { sfxSelect(); startGame(); }
                if (back) { state = MENU; sfxSelect(); }
            }
            break;
    }
}

#ifdef HARNESS
static int frameNo = 0;
static void shot(const char* name) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(R, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, name);
    SDL_FreeSurface(s);
}
static void harnessBefore() {   // runs before render
    frameNo++;
    if (frameNo == 6)  { state = SETTINGS; setSel = 1; cfg.color = 2; }
    if (frameNo == 14) { cfg.color = 1; startGame(); speedOverride = 25; fruit = P{snake.front().x + 3, snake.front().y}; hasFruit = true; fruitType = 2;
        for (int i = 0; i < 9; i++) { P c{snake.back().x - 1, snake.back().y}; snake.push_back(c); occ[idx(c)] = 1; } }
    if (frameNo > 14 && frameNo < 90 && frameNo % 6 == 0 && state == PLAYING) printf("f%d head=(%d,%d) len=%d score=%d\n", frameNo, snake.front().x, snake.front().y, (int)snake.size(), score);
    if (frameNo == 90) queueDir(P{0, -1});
    if (frameNo == 92) queueDir(P{-1, 0});
    if (frameNo == 94) queueDir(P{0, 1});
}
static void harnessAfter() {   // runs after render, before present
    if (frameNo == 4)  shot("/home/claude/shot_menu.bmp");
    if (frameNo == 12) shot("/home/claude/shot_settings.bmp");
    if (frameNo == 60) { shot("/home/claude/shot_game.bmp"); }
    if (frameNo == 120) { printf("state=%d (2=PLAYING,4=GAMEOVER)\n", (int)state); shot("/home/claude/shot_dead1.bmp"); }
    if (frameNo == 200) { printf("state=%d msg=%s / %s\n", (int)state, deathMsg1.c_str(), deathMsg2.c_str()); shot("/home/claude/shot_dead2.bmp"); }
    if (frameNo == 201) {   // full-board test
        startGame(); snake.clear(); occ.assign(COLS * ROWS, 1);
        for (int y = 0; y < ROWS; y++) for (int i = 0; i < COLS; i++) { int x = (y & 1) ? i : COLS - 1 - i; (void)x; }
        for (int y = ROWS - 1; y >= 0; y--) for (int i = 0; i < COLS; i++) { int x = ((ROWS - 1 - y) & 1) ? COLS - 1 - i : i; snake.push_front(P{x, y}); }
        hasFruit = false; state = PLAYING;
        printf("full snake len=%d cells=%d\n", (int)snake.size(), COLS * ROWS);
        step();
        printf("after step: state=%d msg=%s\n", (int)state, deathMsg1.c_str());
    }
    if (frameNo == 215) { shot("/home/claude/shot_full.bmp"); quitReq = true; }
}
#endif

// ------------------------------------------------------------------ main
int main(int, char**) {
    if (!SDL_getenv("SDL_VIDEODRIVER")) SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11,wayland");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "Could not start video: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("CYBER SNAKE - Neon Garden", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "Window error: %s\n", SDL_GetError()); return 1; }
    R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) R = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!R) { fprintf(stderr, "Renderer error: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(R, W, H);
    rngState ^= (uint32_t)SDL_GetTicks() * 2654435761u; if (!rngState) rngState = 1;

    texCircle = makeTexture(64, 64, [](int x, int y) {
        float d = sqrtf((x - 31.5f) * (x - 31.5f) + (y - 31.5f) * (y - 31.5f));
        return argb((int)(255 * std::max(0.f, std::min(1.f, 32.f - d))), 255, 255, 255);
    }, SDL_BLENDMODE_BLEND);
    texGlow = makeTexture(128, 128, [](int x, int y) {
        float d = sqrtf((x - 63.5f) * (x - 63.5f) + (y - 63.5f) * (y - 63.5f)) / 64.f;
        float a = std::max(0.f, 1.f - d); a *= a;
        return argb((int)(255 * a), 255, 255, 255);
    }, SDL_BLENDMODE_ADD);
    texScan = makeTexture(W, H, [](int, int y) { return argb(y % 3 == 0 ? 46 : 0, 0, 0, 0); }, SDL_BLENDMODE_BLEND);
    texVig = makeTexture(W, H, [](int x, int y) {
        float nx = (x - W / 2.f) / (W / 2.f), ny = (y - H / 2.f) / (H / 2.f);
        float d = sqrtf(nx * nx + ny * ny), t = std::max(0.f, std::min(1.f, (d - 0.6f) / 0.9f));
        return argb((int)(170 * powf(t, 1.6f)), 0, 0, 0);
    }, SDL_BLENDMODE_BLEND);
    buildBg();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want{}, have{};
        want.freq = 44100; want.format = AUDIO_F32SYS; want.channels = 1; want.samples = 1024; want.callback = audioCb;
        adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (adev) { gRate = have.freq; SDL_PauseAudioDevice(adev, 0); }
    }
    applySound();
    occ.assign(COLS * ROWS, 0);

    Uint32 last = SDL_GetTicks();
    while (!quitReq) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quitReq = true;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) handleKey(e.key.keysym.sym);
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && state == PLAYING) state = PAUSED;
            else if (e.type == SDL_RENDER_TARGETS_RESET || e.type == SDL_RENDER_DEVICE_RESET) buildBg();
        }
        Uint32 now = SDL_GetTicks();
        float dt = std::min(0.1f, (now - last) / 1000.f);
        last = now;
        T += dt;
#ifdef HARNESS
        harnessBefore();
#endif
        if (state == PLAYING) {
            acc += dt * 1000.0;
            int ms = STEP_MS[cfg.speed];
#ifdef HARNESS
            if (speedOverride) ms = speedOverride;
#endif
            while (acc >= ms && state == PLAYING) { step(); acc -= ms; }
        }
        if (state == GAMEOVER) goT += dt;
        if (shake > 0) shake = std::max(0.f, shake - dt * 2.f);
        if (state != PAUSED) updateFx(dt);
        render();
#ifdef HARNESS
        harnessAfter();
#endif
        SDL_RenderPresent(R);
        Uint32 el = SDL_GetTicks() - now;
        if (el < 15) SDL_Delay(16 - el);
    }
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyRenderer(R);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
