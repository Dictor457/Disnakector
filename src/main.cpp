#include <SDL2/SDL.h>
#include <iostream>
#include <vector>
#include <deque>
#include <random>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <string>
#include <optional>
#include <cstring>
#include "font.hpp"

#ifndef M_PI
constexpr float M_PI = 3.14159265358979323846f;
#endif

constexpr int CELL_SIZE     = 32;
constexpr int GRID_WIDTH    = 25;
constexpr int GRID_HEIGHT   = 20;
constexpr int SCREEN_WIDTH   = GRID_WIDTH * CELL_SIZE;  // 800 px
constexpr int SCREEN_HEIGHT  = GRID_HEIGHT * CELL_SIZE; // 640 px

struct Snake {
    int x[1024];
    int y[1024];
    int length;
    int dx;
    int dy;
};

#if defined(__x86_64__) #if defined(__x86_64__) && (defined(__linux__) || defined(__unix__))#if defined(__x86_64__) && (defined(__linux__) || defined(__unix__)) (defined(__linux__) || defined(__unix__)) && !defined(__ANDROID__)
extern "C" int snake_step_asm(Snake* s, int max_w, int max_h, int grow);
#define DISNAKECTOR_CORE_STEP snake_step_asm
#else
int snake_step_cpp(Snake* s, int max_w, int max_h, int grow) {
    if (!s || s->length < 1) return -1;
    if (grow) s->length++;
    int new_x = s->x[0] + s->dx;
    int new_y = s->y[0] + s->dy;
    if (new_x < 0 || new_x >= max_w || new_y < 0 || new_y >= max_h) return 1;
    for (int i = s->length - 1; i > 0; --i) {
        s->x[i] = s->x[i - 1];
        s->y[i] = s->y[i - 1];
    }
    s->x[0] = new_x;
    s->y[0] = new_y;
    for (int i = 1; i < s->length; ++i) {
        if (s->x[i] == new_x && s->y[i] == new_y) return 2;
    }
    return 0;
}
#define DISNAKECTOR_CORE_STEP snake_step_cpp
#endif

struct Apple { int x; int y; };
struct Particle { float x, y, vx, vy, life; uint8_t r, g, b; };

static SDL_Surface* create_procedural_icon() {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 32, 32, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return nullptr;
    auto* pixels = static_cast<uint32_t*>(surface->pixels);
    SDL_LockSurface(surface);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            uint32_t color = 0x00000000;
            if (x >= 4 && x <= 27 && y >= 4 && y <= 27) color = 0xFF1B8A38;
            if (x >= 8 && x <= 23 && y >= 8 && y <= 23) color = 0xFF2BD857;
            if ((x >= 8 && x <= 11 && y >= 8 && y <= 11) || (x >= 20 && x <= 23 && y >= 8 && y <= 11)) color = 0xFF0D1117;
            if (x >= 14 && x <= 17 && y >= 26 && y <= 31) color = 0xFFE63946;
            pixels[y * 32 + x] = color;
        }
    }
    SDL_UnlockSurface(surface);
    return surface;
}

void draw_filled_circle(SDL_Renderer* ren, int cx, int cy, int r, SDL_Color col) {
    if (r <= 0) return;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (int dy = -r; dy <= r; ++dy) {
        int dx = static_cast<int>(std::sqrt(r * r - dy * dy));
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// =============================================================================
// Аудиомикшер
// =============================================================================
struct ActiveSFX {
    std::vector<int16_t> samples;
    size_t cursor = 0;
};

struct AudioMixer {
    float sfx_volume = 0.8f;
    float music_volume = 0.0f; // По умолчанию на нуле до решения игрока

    Uint8* music_data = nullptr;
    Uint32 music_len = 0;
    Uint32 music_pos = 0;
    bool has_music = false;

    int current_track = 1;

    std::vector<ActiveSFX> sfx_channels;
    SDL_AudioDeviceID dev = 0;
    SDL_AudioSpec have_spec{};
};

static AudioMixer g_mixer;

void load_track_file(int track_num) {
    if (g_mixer.dev > 0) SDL_LockAudioDevice(g_mixer.dev);

    if (g_mixer.music_data) {
        std::free(g_mixer.music_data);
        g_mixer.music_data = nullptr;
    }
    g_mixer.music_len = 0;
    g_mixer.music_pos = 0;
    g_mixer.has_music = false;
    g_mixer.current_track = track_num;

    std::string path = "assets/track" + std::to_string(track_num) + ".wav";
    SDL_AudioSpec wav_spec;
    Uint8* wav_buf = nullptr;
    Uint32 wav_len = 0;

    if (SDL_LoadWAV(path.c_str(), &wav_spec, &wav_buf, &wav_len)) {
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, wav_spec.format, wav_spec.channels, wav_spec.freq,
                              g_mixer.have_spec.format, g_mixer.have_spec.channels, g_mixer.have_spec.freq) >= 0) {
            cvt.len = wav_len;
            cvt.buf = static_cast<Uint8*>(std::malloc(wav_len * cvt.len_mult));
            std::memcpy(cvt.buf, wav_buf, wav_len);
            SDL_ConvertAudio(&cvt);
            SDL_FreeWAV(wav_buf);

            g_mixer.music_data = cvt.buf;
            g_mixer.music_len = cvt.len_cvt;
            g_mixer.has_music = true;
        } else {
            SDL_FreeWAV(wav_buf);
        }
    }

    if (g_mixer.dev > 0) SDL_UnlockAudioDevice(g_mixer.dev);
}

void audio_callback(void* userdata, Uint8* stream, int len) {
    auto* mixer = static_cast<AudioMixer*>(userdata);
    std::memset(stream, 0, len);

    if (mixer->has_music && mixer->music_len > 0 && mixer->music_volume > 0.001f) {
        int bytes_left = len;
        Uint8* out_ptr = stream;
        while (bytes_left > 0) {
            Uint32 available = mixer->music_len - mixer->music_pos;
            Uint32 chunk = std::min((Uint32)bytes_left, available);
            SDL_MixAudioFormat(out_ptr, mixer->music_data + mixer->music_pos,
                               AUDIO_S16SYS, chunk,
                               static_cast<int>(mixer->music_volume * SDL_MIX_MAXVOLUME));
            mixer->music_pos += chunk;
            if (mixer->music_pos >= mixer->music_len) mixer->music_pos = 0;
            out_ptr += chunk;
            bytes_left -= chunk;
        }
    }

    if (mixer->sfx_volume > 0.001f) {
        int samples_to_mix = len / (int)sizeof(int16_t);
        for (auto& sfx : mixer->sfx_channels) {
            if (sfx.cursor >= sfx.samples.size()) continue;
            int remaining = static_cast<int>(sfx.samples.size() - sfx.cursor);
            int count = std::min(samples_to_mix, remaining);
            int bytes_chunk = count * (int)sizeof(int16_t);
            SDL_MixAudioFormat(stream, reinterpret_cast<const Uint8*>(sfx.samples.data() + sfx.cursor),
                               AUDIO_S16SYS, bytes_chunk,
                               static_cast<int>(mixer->sfx_volume * SDL_MIX_MAXVOLUME));
            sfx.cursor += count;
        }
        mixer->sfx_channels.erase(
            std::remove_if(mixer->sfx_channels.begin(), mixer->sfx_channels.end(),
                           [](const ActiveSFX& s) { return s.cursor >= s.samples.size(); }),
            mixer->sfx_channels.end()
        );
    }
}

void init_audio() {
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    want.userdata = &g_mixer;

    g_mixer.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &g_mixer.have_spec, 0);
    load_track_file(1);
    if (g_mixer.dev > 0) SDL_PauseAudioDevice(g_mixer.dev, 0);
}

void play_sfx_buffer(const std::vector<int16_t>& stereo_samples) {
    if (g_mixer.dev == 0 || g_mixer.sfx_volume <= 0.001f) return;
    SDL_LockAudioDevice(g_mixer.dev);
    g_mixer.sfx_channels.push_back({ stereo_samples, 0 });
    SDL_UnlockAudioDevice(g_mixer.dev);
}

void sound_play_eat() {
    const int sr = 44100;
    const int dur_ms = 50;
    const int total_frames = sr * dur_ms / 1000;
    std::vector<int16_t> samples(total_frames * 2);
    for (int i = 0; i < total_frames; ++i) {
        float p = (float)i / total_frames;
        float freq = 680.0f + p * 920.0f;
        float s = std::sin(2.0f * M_PI * freq * ((float)i / sr));
        int16_t val = static_cast<int16_t>(s * (1.0f - p) * 9000.0f);
        samples[i * 2] = val;
        samples[i * 2 + 1] = val;
    }
    play_sfx_buffer(samples);
}

void sound_play_click() {
    const int sr = 44100;
    const int dur_ms = 18;
    const int total_frames = sr * dur_ms / 1000;
    std::vector<int16_t> samples(total_frames * 2);
    for (int i = 0; i < total_frames; ++i) {
        float s = std::sin(2.0f * M_PI * 1300.0f * ((float)i / sr));
        int16_t val = static_cast<int16_t>(s * (1.0f - (float)i / total_frames) * 6000.0f);
        samples[i * 2] = val;
        samples[i * 2 + 1] = val;
    }
    play_sfx_buffer(samples);
}

void sound_play_crash() {
    const int sr = 44100;
    const int dur_ms = 220;
    const int total_frames = sr * dur_ms / 1000;
    std::vector<int16_t> samples(total_frames * 2);
    for (int i = 0; i < total_frames; ++i) {
        float decay = 1.0f - (float)i / total_frames;
        int noise = (rand() % 16000) - 8000;
        float saw = ((std::fmod(i, 300.0f) / 150.0f) - 1.0f) * 7000.0f;
        int16_t val = static_cast<int16_t>((noise * 0.4f + saw * 0.6f) * decay);
        samples[i * 2] = val;
        samples[i * 2 + 1] = val;
    }
    play_sfx_buffer(samples);
}

// =============================================================================
// UI Элементы
// =============================================================================
struct Slider {
    SDL_Rect track;
    std::string label;
    float value;
    bool dragging = false;

    void update(int mx, int my, bool mouse_down) {
        if (mouse_down) {
            if (dragging || (mx >= track.x - 12 && mx <= track.x + track.w + 12 &&
                             my >= track.y - 12 && my <= track.y + track.h + 12)) {
                dragging = true;
                value = std::clamp(static_cast<float>(mx - track.x) / track.w, 0.0f, 1.0f);
            }
        } else {
            dragging = false;
        }
    }

    void draw(SDL_Renderer* ren) const {
        int pct = static_cast<int>(std::round(value * 100.0f));
        std::string txt = label + ": " + std::to_string(pct) + "%";
        draw_text(ren, txt, track.x, track.y - 24, 2, { 220, 230, 245, 255 });

        SDL_SetRenderDrawColor(ren, 20, 26, 36, 255);
        SDL_RenderFillRect(ren, &track);
        SDL_SetRenderDrawColor(ren, 45, 58, 75, 255);
        SDL_RenderDrawRect(ren, &track);

        int fill_w = static_cast<int>(value * track.w);
        if (fill_w > 0) {
            SDL_Rect fill{ track.x, track.y, fill_w, track.h };
            SDL_SetRenderDrawColor(ren, 43, 216, 87, 255);
            SDL_RenderFillRect(ren, &fill);
        }

        SDL_Rect thumb{ track.x + fill_w - 6, track.y - 5, 12, track.h + 10 };
        SDL_SetRenderDrawColor(ren, 245, 245, 245, 255);
        SDL_RenderFillRect(ren, &thumb);
        SDL_SetRenderDrawColor(ren, dragging ? 43 : 20, dragging ? 216 : 26, dragging ? 87 : 35, 255);
        SDL_RenderDrawRect(ren, &thumb);
    }
};

struct Button {
    SDL_Rect rect;
    std::string text;
    bool hovered = false;

    void update(int mx, int my) {
        hovered = (mx >= rect.x && mx <= rect.x + rect.w && my >= rect.y && my <= rect.y + rect.h);
    }

    void draw(SDL_Renderer* ren) const {
        if (hovered) {
            SDL_SetRenderDrawColor(ren, 35, 45, 60, 255);
            SDL_RenderFillRect(ren, &rect);
            SDL_SetRenderDrawColor(ren, 43, 216, 87, 255);
        } else {
            SDL_SetRenderDrawColor(ren, 20, 26, 35, 230);
            SDL_RenderFillRect(ren, &rect);
            SDL_SetRenderDrawColor(ren, 50, 65, 85, 255);
        }
        for (int b = 0; b < 2; ++b) {
            SDL_Rect border{ rect.x - b, rect.y - b, rect.w + b * 2, rect.h + b * 2 };
            SDL_RenderDrawRect(ren, &border);
        }
        int text_w = get_text_width(text, 2);
        int tx = rect.x + (rect.w - text_w) / 2;
        int ty = rect.y + (rect.h - 16) / 2;
        SDL_Color col = hovered ? SDL_Color{ 255, 255, 255, 255 } : SDL_Color{ 180, 200, 220, 255 };
        draw_text(ren, text, tx, ty, 2, col, true);
    }
};

// =============================================================================
// Лехман в полный рост
// =============================================================================
void draw_lekhman_fullbody(SDL_Renderer* ren, int x, int y, uint32_t now) {
    float pec_bounce = std::sin(now * 0.007f);
    int pec_y_off = static_cast<int>(pec_bounce * 4.5f);

    SDL_SetRenderDrawColor(ren, 25, 20, 18, 255);
    SDL_Rect hair_back{ x + 16, y - 2, 54, 38 };
    SDL_RenderFillRect(ren, &hair_back);

    SDL_SetRenderDrawColor(ren, 230, 172, 132, 255);
    SDL_Rect face{ x + 24, y + 10, 38, 28 };
    SDL_RenderFillRect(ren, &face);

    SDL_SetRenderDrawColor(ren, 200, 140, 100, 255);
    SDL_RenderDrawLine(ren, x + 24, y + 22, x + 27, y + 32);
    SDL_RenderDrawLine(ren, x + 61, y + 22, x + 58, y + 32);

    SDL_SetRenderDrawColor(ren, 140, 50, 45, 255);
    SDL_RenderDrawLine(ren, x + 38, y + 32, x + 49, y + 31);

    bool winking = ((now % 2600) < 380);

    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_Rect eye_l{ x + 30, y + 20, 8, 5 };
    SDL_RenderFillRect(ren, &eye_l);
    SDL_SetRenderDrawColor(ren, 20, 25, 35, 255);
    SDL_Rect pupil_l{ x + 33, y + 20, 4, 5 };
    SDL_RenderFillRect(ren, &pupil_l);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderDrawPoint(ren, x + 34, y + 21);

    if (winking) {
        SDL_SetRenderDrawColor(ren, 25, 25, 30, 255);
        SDL_RenderDrawLine(ren, x + 48, y + 23, x + 56, y + 21);
        SDL_RenderDrawLine(ren, x + 48, y + 24, x + 56, y + 22);
    } else {
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_Rect eye_r{ x + 48, y + 20, 8, 5 };
        SDL_RenderFillRect(ren, &eye_r);
        SDL_SetRenderDrawColor(ren, 20, 25, 35, 255);
        SDL_Rect pupil_r{ x + 50, y + 20, 4, 5 };
        SDL_RenderFillRect(ren, &pupil_r);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderDrawPoint(ren, x + 51, y + 21);
    }

    int curls[6][3] = { {20, 10, 10}, {28, 14, 9}, {36, 16, 11}, {45, 15, 9}, {53, 14, 10}, {61, 9, 8} };
    for (int i = 0; i < 6; ++i) {
        SDL_Rect curl{ x + curls[i][0], y + curls[i][1], curls[i][2], 7 };
        SDL_SetRenderDrawColor(ren, 35, 28, 25, 255);
        SDL_RenderFillRect(ren, &curl);
        SDL_SetRenderDrawColor(ren, 55, 45, 40, 255);
        SDL_RenderDrawPoint(ren, x + curls[i][0] + 2, y + curls[i][1] + 2);
    }

    SDL_SetRenderDrawColor(ren, 220, 160, 120, 255);
    SDL_Rect neck{ x + 35, y + 36, 16, 10 };
    SDL_RenderFillRect(ren, &neck);

    SDL_Rect delts{ x + 8, y + 44, 70, 16 };
    SDL_RenderFillRect(ren, &delts);

    SDL_SetRenderDrawColor(ren, 240, 180, 138, 255);
    SDL_Rect pec_l{ x + 16, y + 48 + pec_y_off, 26, 20 };
    SDL_Rect pec_r{ x + 44, y + 48 + pec_y_off, 26, 20 };
    SDL_RenderFillRect(ren, &pec_l);
    SDL_RenderFillRect(ren, &pec_r);

    SDL_SetRenderDrawColor(ren, 175, 110, 75, 255);
    SDL_RenderDrawLine(ren, x + 16, y + 68 + pec_y_off, x + 41, y + 68 + pec_y_off);
    SDL_RenderDrawLine(ren, x + 45, y + 68 + pec_y_off, x + 70, y + 68 + pec_y_off);
    SDL_RenderDrawLine(ren, x + 43, y + 48 + pec_y_off, x + 43, y + 72 + pec_y_off);

    SDL_SetRenderDrawColor(ren, 225, 165, 125, 255);
    SDL_Rect abs_core{ x + 24, y + 72, 38, 30 };
    SDL_RenderFillRect(ren, &abs_core);

    SDL_SetRenderDrawColor(ren, 185, 120, 85, 255);
    SDL_RenderDrawLine(ren, x + 43, y + 72, x + 43, y + 102);
    SDL_RenderDrawLine(ren, x + 28, y + 80, x + 58, y + 80);
    SDL_RenderDrawLine(ren, x + 30, y + 90, x + 56, y + 90);
    SDL_RenderDrawLine(ren, x + 32, y + 99, x + 54, y + 99);

    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_Rect ck_band{ x + 20, y + 102, 46, 7 };
    SDL_RenderFillRect(ren, &ck_band);

    SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
    SDL_Rect ck_logo{ x + 38, y + 104, 10, 3 };
    SDL_RenderFillRect(ren, &ck_logo);

    SDL_SetRenderDrawColor(ren, 25, 27, 34, 255);
    SDL_Rect ck_boxers{ x + 20, y + 109, 46, 22 };
    SDL_RenderFillRect(ren, &ck_boxers);

    SDL_SetRenderDrawColor(ren, 220, 160, 120, 255);
    SDL_Rect leg_l{ x + 20, y + 131, 18, 38 };
    SDL_Rect leg_r{ x + 48, y + 131, 18, 38 };
    SDL_RenderFillRect(ren, &leg_l);
    SDL_RenderFillRect(ren, &leg_r);

    SDL_SetRenderDrawColor(ren, 180, 115, 80, 255);
    SDL_RenderDrawLine(ren, x + 28, y + 135, x + 28, y + 155);
    SDL_RenderDrawLine(ren, x + 58, y + 135, x + 58, y + 155);
}

void draw_speech_bubble(SDL_Renderer* ren, int bx, int by, int bw, int bh, const std::string& text) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 12, 16, 24, 245);
    SDL_Rect bubble{ bx, by, bw, bh };
    SDL_RenderFillRect(ren, &bubble);

    SDL_SetRenderDrawColor(ren, 43, 216, 87, 240);
    SDL_RenderDrawRect(ren, &bubble);

    SDL_RenderDrawLine(ren, bx, by + bh / 2 - 4, bx - 14, by + bh / 2 + 6);
    SDL_RenderDrawLine(ren, bx, by + bh / 2 + 10, bx - 14, by + bh / 2 + 6);

    draw_text(ren, text, bx + 16, by + (bh - 16) / 2, 2, { 255, 255, 255, 255 }, true);
}

// =============================================================================
// Чистый, стильный логотип DISNAKECTOR
// =============================================================================
void draw_clean_pro_logo(SDL_Renderer* ren, int center_x, int top_y, uint32_t now) {
    std::string title = "DISNAKECTOR";
    int text_w = get_text_width(title, 4);

    int plate_w = text_w + 50;
    int plate_h = 56;
    int plate_x = center_x - plate_w / 2;
    int plate_y = top_y;

    // Плавное неоновое дыхание контура
    float pulse = (std::sin(now * 0.005f) + 1.0f) * 0.5f;
    uint8_t border_glow = static_cast<uint8_t>(170 + pulse * 85);

    // Подложка
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 14, 20, 30, 240);
    SDL_Rect plate{ plate_x, plate_y, plate_w, plate_h };
    SDL_RenderFillRect(ren, &plate);

    // Пульсирующая неоновая рамка
    SDL_SetRenderDrawColor(ren, 0, border_glow, 255, 255);
    SDL_RenderDrawRect(ren, &plate);

    // Боковые энерго-стрелы
    int arrow_len = 30;
    SDL_SetRenderDrawColor(ren, 43, 216, 87, 255);
    SDL_RenderDrawLine(ren, plate_x - 10, plate_y + plate_h / 2, plate_x - 10 - arrow_len, plate_y + plate_h / 2);
    SDL_RenderDrawLine(ren, plate_x + plate_w + 10, plate_y + plate_h / 2, plate_x + plate_w + 10 + arrow_len, plate_y + plate_h / 2);

    // 3D-текст логотипа
    int tx = center_x - text_w / 2;
    draw_text(ren, title, tx + 3, plate_y + 14, 4, { 5, 10, 16, 255 }, false); // Тень
    draw_text(ren, title, tx, plate_y + 11, 4, { 43, 225, 95, 255 }, false);   // Яркий изумруд
}

// =============================================================================
// Сенсорные стрелочки управления (Touch D-Pad)
// =============================================================================
struct TouchButton {
    SDL_Rect rect;
    std::string label;
    int dx, dy;
    bool pressed = false;
};

static std::vector<TouchButton> g_touch_pad = {
    { { SCREEN_WIDTH - 120, SCREEN_HEIGHT - 170, 50, 50 }, "^", 0, -1, false },
    { { SCREEN_WIDTH - 120, SCREEN_HEIGHT - 60,  50, 50 }, "v", 0,  1, false },
    { { SCREEN_WIDTH - 175, SCREEN_HEIGHT - 115, 50, 50 }, "<", -1, 0, false },
    { { SCREEN_WIDTH - 65,  SCREEN_HEIGHT - 115, 50, 50 }, ">",  1, 0, false }
};

void draw_touch_dpad(SDL_Renderer* ren) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (const auto& btn : g_touch_pad) {
        if (btn.pressed) {
            SDL_SetRenderDrawColor(ren, 43, 216, 87, 180);
            SDL_RenderFillRect(ren, &btn.rect);
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        } else {
            SDL_SetRenderDrawColor(ren, 20, 30, 45, 130);
            SDL_RenderFillRect(ren, &btn.rect);
            SDL_SetRenderDrawColor(ren, 0, 220, 255, 180);
        }
        SDL_RenderDrawRect(ren, &btn.rect);
        int tw = get_text_width(btn.label, 2);
        draw_text(ren, btn.label, btn.rect.x + (btn.rect.w - tw) / 2, btn.rect.y + 16, 2, { 255, 255, 255, 240 }, false);
    }
}

enum class GameState { AUDIO_PROMPT, MENU, SETTINGS, PLAYING, GAMEOVER, EXIT_CONFIRM };

static void spawn_apple(Apple& apple, const Snake& snake, std::mt19937& rng) {
    std::uniform_int_distribution<int> dx(0, GRID_WIDTH - 1);
    std::uniform_int_distribution<int> dy(2, GRID_HEIGHT - 1);
    while (true) {
        apple.x = dx(rng);
        apple.y = dy(rng);
        bool col = false;
        for (int i = 0; i < snake.length; ++i) {
            if (snake.x[i] == apple.x && snake.y[i] == apple.y) { col = true; break; }
        }
        if (!col) break;
    }
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::cerr << "[FATAL] SDL_Init: " << SDL_GetError() << "\n";
        return 1;
    }

    init_audio();

    SDL_Window* window = SDL_CreateWindow(
        "Disnakector",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );

    SDL_Surface* icon_surf = create_procedural_icon();
    if (icon_surf) {
        SDL_SetWindowIcon(window, icon_surf);
        SDL_FreeSurface(icon_surf);
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    std::random_device rd;
    std::mt19937 rng(rd());

    Snake snake{};
    int prev_x[1024];
    int prev_y[1024];

    int visual_look_dx = 1;
    int visual_look_dy = 0;
    std::deque<std::pair<int, int>> safe_input_queue;

    int score = 0;
    int high_score = 0;
    int initial_speed = 95;
    int tick_rate = initial_speed;

    #if defined(__ANDROID__)
    bool touch_controls_enabled = true;
#else
    #if defined(__ANDROID__)
    bool touch_controls_enabled = true;
#else
    #if defined(__ANDROID__)
    bool touch_controls_enabled = true;
#else
    bool touch_controls_enabled = false;
#endif
#endif
#endif

    auto reset_game = [&]() {
        snake.length = 5;
        snake.dx = 1; snake.dy = 0;
        visual_look_dx = 1; visual_look_dy = 0;
        safe_input_queue.clear();
        snake.x[0] = 7; snake.y[0] = 10;
        snake.x[1] = 6; snake.y[1] = 10;
        snake.x[2] = 5; snake.y[2] = 10;
        snake.x[3] = 4; snake.y[3] = 10;
        snake.x[4] = 3; snake.y[4] = 10;
        for (int i = 0; i < snake.length; ++i) {
            prev_x[i] = snake.x[i];
            prev_y[i] = snake.y[i];
        }
        score = 0;
        tick_rate = initial_speed;
    };

    reset_game();
    Apple apple{};
    spawn_apple(apple, snake, rng);

    std::vector<Particle> particles;
    GameState state = GameState::AUDIO_PROMPT; // Вежливый старт с вопросом о музыке
    bool running = true;

    // Кнопки стартового окна музыки
    Button btn_music_yes { { 260, 270, 120, 50 }, "ДА" };
    Button btn_music_no  { { 420, 270, 120, 50 }, "НЕТ" };

    Button btn_play    { { 275, 185, 250, 48 }, "PLAY" };
    Button btn_settings{ { 275, 245, 250, 48 }, "SETTINGS" };
    Button btn_exit    { { 275, 305, 250, 48 }, "EXIT" };

    Button btn_track   { { 250, 245, 300, 44 }, "TRACK: 1 / 4" };
    Button btn_speed   { { 250, 298, 300, 44 }, "SPEED: NORMAL" };
    Button btn_touch   { { 250, 350, 300, 44 }, "TOUCH UI: OFF" };
    Button btn_back    { { 250, 402, 300, 44 }, "BACK" };

    Button btn_restart { { 275, 280, 250, 50 }, "RESTART" };
    Button btn_to_menu { { 275, 350, 250, 50 }, "MAIN MENU" };

    Button btn_confirm_yes{ { 260, 280, 120, 50 }, "ДА" };
    Button btn_confirm_no { { 420, 280, 120, 50 }, "НЕТ" };

    Slider slider_sfx  { { 250, 145, 300, 14 }, "SOUND FX", 0.8f };
    Slider slider_music{ { 250, 195, 300, 14 }, "MUSIC", 0.0f };

    std::string mascot_phrase = "С МУЗОНОМ ИЛИ В ТИШИНЕ?";

    uint32_t last_tick = SDL_GetTicks();
    uint32_t last_frame_time = SDL_GetTicks();

    while (running) {
        uint32_t frame_start = SDL_GetTicks();
        float delta_time = (frame_start - last_frame_time) / 1000.0f;
        last_frame_time = frame_start;

        int mouse_x, mouse_y;
        Uint32 mouse_btn = SDL_GetMouseState(&mouse_x, &mouse_y);
        bool mouse_down = (mouse_btn & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

        if (state == GameState::AUDIO_PROMPT) {
            btn_music_yes.update(mouse_x, mouse_y);
            btn_music_no.update(mouse_x, mouse_y);
        } else if (state == GameState::MENU) {
            btn_play.update(mouse_x, mouse_y);
            btn_settings.update(mouse_x, mouse_y);
            btn_exit.update(mouse_x, mouse_y);
        } else if (state == GameState::SETTINGS) {
            slider_sfx.update(mouse_x, mouse_y, mouse_down);
            slider_music.update(mouse_x, mouse_y, mouse_down);

            if (slider_music.dragging) {
                mascot_phrase = "ЧЕ НОРМАЛЬНЫЙ МУЗОН?";
            }

            SDL_LockAudioDevice(g_mixer.dev);
            g_mixer.sfx_volume = slider_sfx.value;
            g_mixer.music_volume = slider_music.value;
            SDL_UnlockAudioDevice(g_mixer.dev);

            btn_track.update(mouse_x, mouse_y);
            btn_speed.update(mouse_x, mouse_y);
            btn_touch.update(mouse_x, mouse_y);
            btn_back.update(mouse_x, mouse_y);
        } else if (state == GameState::EXIT_CONFIRM) {
            btn_confirm_yes.update(mouse_x, mouse_y);
            btn_confirm_no.update(mouse_x, mouse_y);
        } else if (state == GameState::GAMEOVER) {
            btn_restart.update(mouse_x, mouse_y);
            btn_to_menu.update(mouse_x, mouse_y);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                if (state == GameState::AUDIO_PROMPT) {
                    if (btn_music_yes.hovered) {
                        sound_play_click();
                        SDL_LockAudioDevice(g_mixer.dev);
                        g_mixer.music_volume = 0.5f;
                        slider_music.value = 0.5f;
                        SDL_UnlockAudioDevice(g_mixer.dev);
                        state = GameState::MENU;
                        mascot_phrase = "САЛАМ! Я ЛЕХМАН)";
                    } else if (btn_music_no.hovered) {
                        sound_play_click();
                        SDL_LockAudioDevice(g_mixer.dev);
                        g_mixer.music_volume = 0.0f;
                        slider_music.value = 0.0f;
                        SDL_UnlockAudioDevice(g_mixer.dev);
                        state = GameState::MENU;
                        mascot_phrase = "САЛАМ! Я ЛЕХМАН)";
                    }
                } else if (state == GameState::PLAYING && touch_controls_enabled) {
                    for (auto& btn : g_touch_pad) {
                        if (mouse_x >= btn.rect.x && mouse_x <= btn.rect.x + btn.rect.w &&
                            mouse_y >= btn.rect.y && mouse_y <= btn.rect.y + btn.rect.h) {
                            btn.pressed = true;
                            auto last_dir = safe_input_queue.empty() ? std::make_pair(snake.dx, snake.dy) : safe_input_queue.back();
                            if (btn.dx != -last_dir.first || btn.dy != -last_dir.second) {
                                if (safe_input_queue.size() < 2) {
                                    safe_input_queue.push_back({btn.dx, btn.dy});
                                    visual_look_dx = btn.dx; visual_look_dy = btn.dy;
                                }
                            }
                        }
                    }
                } else if (state == GameState::MENU) {
                    if (btn_play.hovered) {
                        sound_play_click();
                        reset_game();
                        spawn_apple(apple, snake, rng);
                        state = GameState::PLAYING;
                    } else if (btn_settings.hovered) {
                        sound_play_click();
                        state = GameState::SETTINGS;
                    } else if (btn_exit.hovered) {
                        sound_play_click();
                        state = GameState::EXIT_CONFIRM;
                        mascot_phrase = "СЕРЬЕЗНО ЛИВАЕШЬ?(";
                    }
                } else if (state == GameState::EXIT_CONFIRM) {
                    if (btn_confirm_yes.hovered) {
                        running = false;
                    } else if (btn_confirm_no.hovered) {
                        sound_play_click();
                        state = GameState::MENU;
                        mascot_phrase = "САЛАМ! Я ЛЕХМАН)";
                    }
                } else if (state == GameState::SETTINGS) {
                    if (btn_track.hovered) {
                        sound_play_click();
                        int next_track = (g_mixer.current_track % 4) + 1;
                        load_track_file(next_track);
                        btn_track.text = "TRACK: " + std::to_string(next_track) + " / 4";
                        mascot_phrase = "ЧЕ НОРМАЛЬНЫЙ МУЗОН?";
                    } else if (btn_speed.hovered) {
                        sound_play_click();
                        if (initial_speed == 95) {
                            initial_speed = 70;
                            btn_speed.text = "SPEED: FAST";
                            mascot_phrase = "НИХУЯ СЕБЕ FAST?";
                        } else if (initial_speed == 70) {
                            initial_speed = 48;
                            btn_speed.text = "SPEED: INSANE";
                            mascot_phrase = "INSANE? ЕБА ЧЕ ЗА МОНСТР";
                        } else {
                            initial_speed = 95;
                            btn_speed.text = "SPEED: NORMAL";
                            mascot_phrase = "НОРМАЛЬНАЯ СЛОЖНОСТЬ - ЗАЕБИСЬ";
                        }
                    } else if (btn_touch.hovered) {
                        sound_play_click();
                        touch_controls_enabled = !touch_controls_enabled;
                        btn_touch.text = touch_controls_enabled ? "TOUCH UI: ON" : "TOUCH UI: OFF";
                    } else if (btn_back.hovered) {
                        sound_play_click();
                        mascot_phrase = "САЛАМ! Я ЛЕХМАН)";
                        state = GameState::MENU;
                    }
                } else if (state == GameState::GAMEOVER) {
                    if (btn_restart.hovered) {
                        sound_play_click();
                        reset_game();
                        spawn_apple(apple, snake, rng);
                        state = GameState::PLAYING;
                    } else if (btn_to_menu.hovered) {
                        sound_play_click();
                        mascot_phrase = "САЛАМ! Я ЛЕХМАН)";
                        state = GameState::MENU;
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                for (auto& btn : g_touch_pad) btn.pressed = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    if (state == GameState::PLAYING) state = GameState::MENU;
                    else if (state == GameState::SETTINGS) state = GameState::MENU;
                    else if (state == GameState::EXIT_CONFIRM) state = GameState::MENU;
                    else if (state == GameState::AUDIO_PROMPT) {
                        state = GameState::MENU;
                        mascot_phrase = "САЛАМ! Я ЛЕХМАН)";
                    } else if (state == GameState::MENU) {
                        state = GameState::EXIT_CONFIRM;
                        mascot_phrase = "СЕРЬЕЗНО ЛИВАЕШЬ?(";
                    }
                }
                if (state == GameState::PLAYING) {
                    int req_dx = 0, req_dy = 0;
                    switch (event.key.keysym.sym) {
                        case SDLK_UP: case SDLK_w:    req_dx = 0; req_dy = -1; break;
                        case SDLK_DOWN: case SDLK_s:  req_dx = 0; req_dy = 1; break;
                        case SDLK_LEFT: case SDLK_a:  req_dx = -1; req_dy = 0; break;
                        case SDLK_RIGHT: case SDLK_d: req_dx = 1; req_dy = 0; break;
                    }

                    if (req_dx != 0 || req_dy != 0) {
                        auto last_dir = safe_input_queue.empty() ? std::make_pair(snake.dx, snake.dy) : safe_input_queue.back();
                        if (req_dx != -last_dir.first || req_dy != -last_dir.second) {
                            if (req_dx != last_dir.first || req_dy != last_dir.second) {
                                if (safe_input_queue.size() < 2) {
                                    safe_input_queue.push_back({req_dx, req_dy});
                                    visual_look_dx = req_dx; visual_look_dy = req_dy;
                                }
                            }
                        }
                    }
                } else if (state == GameState::GAMEOVER && (event.key.keysym.sym == SDLK_SPACE || event.key.keysym.sym == SDLK_r)) {
                    reset_game();
                    spawn_apple(apple, snake, rng);
                    state = GameState::PLAYING;
                }
            }
        }

        uint32_t now = SDL_GetTicks();
        if (state == GameState::PLAYING && (now - last_tick >= static_cast<uint32_t>(tick_rate))) {
            last_tick = now;

            for (int i = 0; i < snake.length; ++i) {
                prev_x[i] = snake.x[i];
                prev_y[i] = snake.y[i];
            }

            if (!safe_input_queue.empty()) {
                snake.dx = safe_input_queue.front().first;
                snake.dy = safe_input_queue.front().second;
                safe_input_queue.pop_front();
            }
            visual_look_dx = snake.dx;
            visual_look_dy = snake.dy;

            int next_x = snake.x[0] + snake.dx;
            int next_y = snake.y[0] + snake.dy;
            int will_grow = (next_x == apple.x && next_y == apple.y) ? 1 : 0;

            if (will_grow) {
                prev_x[snake.length] = prev_x[snake.length - 1];
                prev_y[snake.length] = prev_y[snake.length - 1];
            }

            int status = DISNAKECTOR_CORE_STEP(&snake, GRID_WIDTH, GRID_HEIGHT, will_grow);
            if (status != 0) {
                state = GameState::GAMEOVER;
                sound_play_crash();
            } else if (will_grow) {
                score++;
                if (score > high_score) high_score = score;
                tick_rate = std::max(35, initial_speed - score * 2);
                sound_play_eat();
                for (int i = 0; i < 20; ++i) {
                    float a = (rand() % 360) * (M_PI / 180.0f);
                    float sp = 1.5f + (rand() % 40) / 10.0f;
                    particles.push_back({
                        apple.x * CELL_SIZE + 16.0f, apple.y * CELL_SIZE + 16.0f,
                        std::cos(a) * sp, std::sin(a) * sp, 1.0f,
                        245, (uint8_t)(60 + rand() % 100), 75
                    });
                }
                spawn_apple(apple, snake, rng);
            }
        }

        float alpha = (state == GameState::PLAYING) ? std::clamp((float)(now - last_tick) / tick_rate, 0.0f, 1.0f) : 1.0f;

        for (auto& p : particles) {
            p.x += p.vx; p.y += p.vy; p.life -= delta_time * 2.8f;
        }
        particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0.0f; }), particles.end());

        // =====================================================================
        // РЕНДЕРИНГ
        // =====================================================================
        SDL_SetRenderDrawColor(renderer, 10, 14, 20, 255);
        SDL_RenderClear(renderer);

        // Кибер-сетка
        SDL_SetRenderDrawColor(renderer, 18, 24, 34, 255);
        for (int x = 0; x < SCREEN_WIDTH; x += CELL_SIZE) SDL_RenderDrawLine(renderer, x, 0, x, SCREEN_HEIGHT);
        for (int y = 0; y < SCREEN_HEIGHT; y += CELL_SIZE) SDL_RenderDrawLine(renderer, 0, y, SCREEN_WIDTH, y);

        // Глянцевое яблоко
        int ax = apple.x * CELL_SIZE;
        int ay = apple.y * CELL_SIZE;
        SDL_Rect apple_base{ ax + 4, ay + 6, CELL_SIZE - 8, CELL_SIZE - 10 };
        SDL_SetRenderDrawColor(renderer, 230, 45, 60, 255);
        SDL_RenderFillRect(renderer, &apple_base);

        SDL_Rect stem{ ax + CELL_SIZE / 2 - 2, ay + 2, 3, 5 };
        SDL_SetRenderDrawColor(renderer, 120, 70, 35, 255);
        SDL_RenderFillRect(renderer, &stem);

        SDL_Rect leaf{ ax + CELL_SIZE / 2 + 1, ay + 2, 5, 3 };
        SDL_SetRenderDrawColor(renderer, 43, 216, 87, 255);
        SDL_RenderFillRect(renderer, &leaf);

        SDL_SetRenderDrawColor(renderer, 255, 200, 210, 255);
        SDL_RenderDrawPoint(renderer, ax + 8, ay + 9);
        SDL_RenderDrawPoint(renderer, ax + 9, ay + 9);

        // =====================================================================
        // Сглаженное анатомическое тело змейки
        // =====================================================================
        std::vector<std::pair<float, float>> seg_centers(snake.length);
        for (int i = 0; i < snake.length; ++i) {
            float rx = prev_x[i] + (snake.x[i] - prev_x[i]) * alpha;
            float ry = prev_y[i] + (snake.y[i] - prev_y[i]) * alpha;
            seg_centers[i] = { rx * CELL_SIZE + CELL_SIZE / 2.0f, ry * CELL_SIZE + CELL_SIZE / 2.0f };
        }

        for (int i = snake.length - 1; i > 0; --i) {
            float progress = static_cast<float>(i) / snake.length;
            int radius = static_cast<int>(13.5f - progress * 4.5f);

            auto p1 = seg_centers[i];
            auto p2 = seg_centers[i - 1];

            for (int step = 0; step <= 5; ++step) {
                float t = step / 5.0f;
                int ix = static_cast<int>(p1.first + (p2.first - p1.first) * t);
                int iy = static_cast<int>(p1.second + (p2.second - p1.second) * t);

                draw_filled_circle(renderer, ix, iy, radius, { 25, 135, 60, 255 });
                draw_filled_circle(renderer, ix, iy, radius - 2, { 38, 175, 80, 255 });
                draw_filled_circle(renderer, ix, iy, std::max(2, radius - 6), { 65, 230, 115, 255 });
            }
        }

        // Голова змейки
        int hx = static_cast<int>(seg_centers[0].first);
        int hy = static_cast<int>(seg_centers[0].second);
        int head_r = 14;

        draw_filled_circle(renderer, hx, hy, head_r, { 30, 150, 70, 255 });
        draw_filled_circle(renderer, hx, hy, head_r - 2, { 45, 195, 90, 255 });

        int e1_x = hx, e1_y = hy, e2_x = hx, e2_y = hy;
        if (visual_look_dx == 1) {
            e1_x = hx + 4; e1_y = hy - 6; e2_x = hx + 4; e2_y = hy + 6;
        } else if (visual_look_dx == -1) {
            e1_x = hx - 4; e1_y = hy - 6; e2_x = hx - 4; e2_y = hy + 6;
        } else if (visual_look_dy == 1) {
            e1_x = hx - 6; e1_y = hy + 4; e2_x = hx + 6; e2_y = hy + 4;
        } else {
            e1_x = hx - 6; e1_y = hy - 4; e2_x = hx + 6; e2_y = hy - 4;
        }

        draw_filled_circle(renderer, e1_x, e1_y, 4, { 255, 205, 30, 255 });
        draw_filled_circle(renderer, e2_x, e2_y, 4, { 255, 205, 30, 255 });

        SDL_SetRenderDrawColor(renderer, 10, 15, 20, 255);
        SDL_RenderDrawLine(renderer, e1_x, e1_y - 2, e1_x, e1_y + 2);
        SDL_RenderDrawLine(renderer, e2_x, e2_y - 2, e2_x, e2_y + 2);

        if ((now / 150) % 2 == 0) {
            SDL_SetRenderDrawColor(renderer, 235, 45, 60, 255);
            int tx = hx + visual_look_dx * 13;
            int ty = hy + visual_look_dy * 13;
            if (visual_look_dx == 1) {
                SDL_RenderDrawLine(renderer, tx, ty, tx + 6, ty);
                SDL_RenderDrawLine(renderer, tx + 6, ty, tx + 9, ty - 2);
                SDL_RenderDrawLine(renderer, tx + 6, ty, tx + 9, ty + 2);
            } else if (visual_look_dx == -1) {
                SDL_RenderDrawLine(renderer, tx, ty, tx - 6, ty);
                SDL_RenderDrawLine(renderer, tx - 6, ty, tx - 9, ty - 2);
                SDL_RenderDrawLine(renderer, tx - 6, ty, tx - 9, ty + 2);
            } else if (visual_look_dy == 1) {
                SDL_RenderDrawLine(renderer, tx, ty, tx, ty + 6);
                SDL_RenderDrawLine(renderer, tx, ty + 6, tx - 2, ty + 9);
                SDL_RenderDrawLine(renderer, tx, ty + 6, tx + 2, ty + 9);
            } else if (visual_look_dy == -1) {
                SDL_RenderDrawLine(renderer, tx, ty, tx, ty - 6);
                SDL_RenderDrawLine(renderer, tx, ty - 6, tx - 2, ty - 9);
                SDL_RenderDrawLine(renderer, tx, ty - 6, tx + 2, ty - 9);
            }
        }

        // Частицы
        for (const auto& p : particles) {
            SDL_SetRenderDrawColor(renderer, p.r, p.g, p.b, (uint8_t)(p.life * 255.0f));
            SDL_Rect pr{ (int)p.x, (int)p.y, 3, 3 };
            SDL_RenderFillRect(renderer, &pr);
        }

        // HUD
        if (state == GameState::PLAYING || state == GameState::GAMEOVER) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 10, 14, 20, 200);
            SDL_Rect hud_plate{ 8, 8, 300, 36 };
            SDL_RenderFillRect(renderer, &hud_plate);
            SDL_SetRenderDrawColor(renderer, 43, 216, 87, 180);
            SDL_RenderDrawRect(renderer, &hud_plate);

            std::string score_str = "APPLES: " + std::to_string(score);
            std::string record_str = "BEST: " + std::to_string(high_score);
            draw_text(renderer, score_str, 18, 18, 2, { 43, 216, 87, 255 });
            draw_text(renderer, record_str, 175, 18, 2, { 255, 209, 102, 255 });

            if (touch_controls_enabled) {
                draw_touch_dpad(renderer);
            }
        }

        // =====================================================================
        // Стартовый диалог выбора звука
        // =====================================================================
        if (state == GameState::AUDIO_PROMPT) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 6, 8, 14, 245);
            SDL_Rect full{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            SDL_RenderFillRect(renderer, &full);

            std::string prompt = "ВКЛЮЧИТЬ МУЗЫКУ?";
            int pw = get_text_width(prompt, 3);
            draw_text(renderer, prompt, (SCREEN_WIDTH - pw) / 2, 190, 3, { 255, 255, 255, 255 });

            btn_music_yes.draw(renderer);
            btn_music_no.draw(renderer);

            draw_lekhman_fullbody(renderer, 30, 420, now);
            draw_speech_bubble(renderer, 130, 485, 635, 60, mascot_phrase);

        } else if (state == GameState::MENU) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 8, 12, 18, 225);
            SDL_Rect full{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            SDL_RenderFillRect(renderer, &full);

            // Чистый логотип
            draw_clean_pro_logo(renderer, SCREEN_WIDTH / 2, 75, now);

            btn_play.draw(renderer);
            btn_settings.draw(renderer);
            btn_exit.draw(renderer);

            draw_lekhman_fullbody(renderer, 30, 420, now);
            draw_speech_bubble(renderer, 130, 485, 635, 60, mascot_phrase);

        } else if (state == GameState::EXIT_CONFIRM) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 6, 8, 14, 235);
            SDL_Rect full{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            SDL_RenderFillRect(renderer, &full);

            std::string prompt = "ТЫ УВЕРЕН?";
            int pw = get_text_width(prompt, 4);
            draw_text(renderer, prompt, (SCREEN_WIDTH - pw) / 2, 190, 4, { 255, 255, 255, 255 });

            btn_confirm_yes.draw(renderer);
            btn_confirm_no.draw(renderer);

            draw_lekhman_fullbody(renderer, 30, 420, now);
            draw_speech_bubble(renderer, 130, 485, 635, 60, mascot_phrase);

        } else if (state == GameState::SETTINGS) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 8, 12, 18, 235);
            SDL_Rect full{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            SDL_RenderFillRect(renderer, &full);

            std::string title = "SETTINGS";
            int tw = get_text_width(title, 4);
            draw_text(renderer, title, (SCREEN_WIDTH - tw) / 2, 60, 4, { 255, 255, 255, 255 });

            slider_sfx.draw(renderer);
            slider_music.draw(renderer);
            btn_track.draw(renderer);
            btn_speed.draw(renderer);
            btn_touch.draw(renderer);
            btn_back.draw(renderer);

            draw_lekhman_fullbody(renderer, 30, 420, now);
            draw_speech_bubble(renderer, 130, 485, 635, 60, mascot_phrase);

        } else if (state == GameState::GAMEOVER) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 100, 10, 18, 185);
            SDL_Rect full{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            SDL_RenderFillRect(renderer, &full);

            std::string gov = "GAME OVER";
            int gw = get_text_width(gov, 5);
            draw_text(renderer, gov, (SCREEN_WIDTH - gw) / 2, 120, 5, { 255, 75, 75, 255 });

            std::string final_score = "FINAL APPLES: " + std::to_string(score);
            int fw = get_text_width(final_score, 2);
            draw_text(renderer, final_score, (SCREEN_WIDTH - fw) / 2, 200, 2, { 255, 255, 255, 255 });

            btn_restart.draw(renderer);
            btn_to_menu.draw(renderer);
        }

        SDL_RenderPresent(renderer);
    }

    if (g_mixer.dev > 0) SDL_CloseAudioDevice(g_mixer.dev);
    if (g_mixer.music_data) std::free(g_mixer.music_data);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
