#include <SDL2/SDL.h>
#include <iostream>
#include <vector>
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

#if defined(__x86_64__) && (defined(__linux__) || defined(__unix__))
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

struct ActiveSFX {
    std::vector<int16_t> samples;
    size_t cursor = 0;
};

struct AudioMixer {
    float sfx_volume = 0.8f;
    float music_volume = 0.5f;

    Uint8* music_data = nullptr;
    Uint32 music_len = 0;
    Uint32 music_pos = 0;
    bool has_music = false;

    std::vector<ActiveSFX> sfx_channels;
    SDL_AudioDeviceID dev = 0;
};

static AudioMixer g_mixer;

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
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    want.userdata = &g_mixer;

    g_mixer.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);

    SDL_AudioSpec wav_spec;
    Uint8* wav_buf = nullptr;
    Uint32 wav_len = 0;
    if (SDL_LoadWAV("assets/music.wav", &wav_spec, &wav_buf, &wav_len)) {
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, wav_spec.format, wav_spec.channels, wav_spec.freq,
                              have.format, have.channels, have.freq) >= 0) {
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
        float freq = 650.0f + p * 900.0f;
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

enum class GameState { MENU, SETTINGS, PLAYING, GAMEOVER };

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

int main() {
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
    std::optional<std::pair<int, int>> next_turn;

    int score = 0;
    int high_score = 0;
    int initial_speed = 95;
    int tick_rate = initial_speed;

    auto reset_game = [&]() {
        snake.length = 4;
        snake.dx = 1; snake.dy = 0;
        visual_look_dx = 1; visual_look_dy = 0;
        next_turn = std::nullopt;
        snake.x[0] = 6; snake.y[0] = 10;
        snake.x[1] = 5; snake.y[1] = 10;
        snake.x[2] = 4; snake.y[2] = 10;
        snake.x[3] = 3; snake.y[3] = 10;
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
    GameState state = GameState::MENU;
    bool running = true;

    Button btn_play    { { 275, 240, 250, 50 }, "PLAY" };
    Button btn_settings{ { 275, 310, 250, 50 }, "SETTINGS" };
    Button btn_exit    { { 275, 380, 250, 50 }, "EXIT" };

    Button btn_speed   { { 250, 340, 300, 48 }, "SPEED: NORMAL" };
    Button btn_back    { { 250, 410, 300, 48 }, "BACK" };

    Button btn_restart { { 275, 290, 250, 50 }, "RESTART" };
    Button btn_to_menu { { 275, 360, 250, 50 }, "MAIN MENU" };

    Slider slider_sfx  { { 250, 210, 300, 14 }, "SOUND FX", 0.8f };
    Slider slider_music{ { 250, 280, 300, 14 }, "MUSIC", 0.5f };

    uint32_t last_tick = SDL_GetTicks();
    uint32_t last_frame_time = SDL_GetTicks();

    while (running) {
        uint32_t frame_start = SDL_GetTicks();
        float delta_time = (frame_start - last_frame_time) / 1000.0f;
        last_frame_time = frame_start;

        int mouse_x, mouse_y;
        Uint32 mouse_btn = SDL_GetMouseState(&mouse_x, &mouse_y);
        bool mouse_down = (mouse_btn & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

        if (state == GameState::MENU) {
            btn_play.update(mouse_x, mouse_y);
            btn_settings.update(mouse_x, mouse_y);
            btn_exit.update(mouse_x, mouse_y);
        } else if (state == GameState::SETTINGS) {
            slider_sfx.update(mouse_x, mouse_y, mouse_down);
            slider_music.update(mouse_x, mouse_y, mouse_down);

            SDL_LockAudioDevice(g_mixer.dev);
            g_mixer.sfx_volume = slider_sfx.value;
            g_mixer.music_volume = slider_music.value;
            SDL_UnlockAudioDevice(g_mixer.dev);

            btn_speed.update(mouse_x, mouse_y);
            btn_back.update(mouse_x, mouse_y);
        } else if (state == GameState::GAMEOVER) {
            btn_restart.update(mouse_x, mouse_y);
            btn_to_menu.update(mouse_x, mouse_y);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                if (state == GameState::MENU) {
                    if (btn_play.hovered) {
                        sound_play_click();
                        reset_game();
                        spawn_apple(apple, snake, rng);
                        state = GameState::PLAYING;
                    } else if (btn_settings.hovered) {
                        sound_play_click();
                        state = GameState::SETTINGS;
                    } else if (btn_exit.hovered) {
                        running = false;
                    }
                } else if (state == GameState::SETTINGS) {
                    if (btn_speed.hovered) {
                        if (initial_speed == 95) {
                            initial_speed = 70;
                            btn_speed.text = "SPEED: FAST";
                        } else if (initial_speed == 70) {
                            initial_speed = 48;
                            btn_speed.text = "SPEED: INSANE";
                        } else {
                            initial_speed = 95;
                            btn_speed.text = "SPEED: NORMAL";
                        }
                        sound_play_click();
                    } else if (btn_back.hovered) {
                        sound_play_click();
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
                        state = GameState::MENU;
                    }
                }
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    if (state == GameState::PLAYING) state = GameState::MENU;
                    else if (state == GameState::SETTINGS) state = GameState::MENU;
                    else if (state == GameState::MENU) running = false;
                }
                if (state == GameState::PLAYING) {
                    int cur_dx = next_turn ? next_turn->first : snake.dx;
                    int cur_dy = next_turn ? next_turn->second : snake.dy;
                    switch (event.key.keysym.sym) {
                        case SDLK_UP: case SDLK_w:
                            if (cur_dy == 0) { next_turn = {0, -1}; visual_look_dx = 0; visual_look_dy = -1; }
                            break;
                        case SDLK_DOWN: case SDLK_s:
                            if (cur_dy == 0) { next_turn = {0, 1}; visual_look_dx = 0; visual_look_dy = 1; }
                            break;
                        case SDLK_LEFT: case SDLK_a:
                            if (cur_dx == 0) { next_turn = {-1, 0}; visual_look_dx = -1; visual_look_dy = 0; }
                            break;
                        case SDLK_RIGHT: case SDLK_d:
                            if (cur_dx == 0) { next_turn = {1, 0}; visual_look_dx = 1; visual_look_dy = 0; }
                            break;
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

            if (next_turn) {
                snake.dx = next_turn->first;
                snake.dy = next_turn->second;
                next_turn = std::nullopt;
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
        // РЕНДЕРИНГ (ЧИСТАЯ ПРОЦЕДУРНАЯ ГРАФИКА)
        // =====================================================================
        SDL_SetRenderDrawColor(renderer, 12, 16, 22, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 20, 26, 35, 255);
        for (int x = 0; x < SCREEN_WIDTH; x += CELL_SIZE) SDL_RenderDrawLine(renderer, x, 0, x, SCREEN_HEIGHT);
        for (int y = 0; y < SCREEN_HEIGHT; y += CELL_SIZE) SDL_RenderDrawLine(renderer, 0, y, SCREEN_WIDTH, y);

        // Яблоко со стеблем
        SDL_Rect ab{ apple.x * CELL_SIZE + 4, apple.y * CELL_SIZE + 5, CELL_SIZE - 8, CELL_SIZE - 9 };
        SDL_SetRenderDrawColor(renderer, 230, 57, 70, 255);
        SDL_RenderFillRect(renderer, &ab);
        SDL_Rect as{ apple.x * CELL_SIZE + CELL_SIZE / 2 - 2, apple.y * CELL_SIZE + 2, 4, 4 };
        SDL_SetRenderDrawColor(renderer, 46, 196, 182, 255);
        SDL_RenderFillRect(renderer, &as);

        // Змейка
        for (int i = snake.length - 1; i >= 0; --i) {
            float rx = prev_x[i] + (snake.x[i] - prev_x[i]) * alpha;
            float ry = prev_y[i] + (snake.y[i] - prev_y[i]) * alpha;
            int px = (int)std::round(rx * CELL_SIZE);
            int py = (int)std::round(ry * CELL_SIZE);
            SDL_Rect seg_dst{ px + 2, py + 2, CELL_SIZE - 4, CELL_SIZE - 4 };

            if (i == 0) {
                // Голова
                SDL_SetRenderDrawColor(renderer, 43, 216, 87, 255);
                SDL_RenderFillRect(renderer, &seg_dst);

                // Глаза (0 ms Lag)
                int e1_x = px + 8, e1_y = py + 8, e2_x = px + 20, e2_y = py + 8;
                int p1_x = e1_x + 1, p1_y = e1_y + 1, p2_x = e2_x + 1, p2_y = e2_y + 1;
                if (visual_look_dx == 1)      { e1_x = px + 18; e1_y = py + 6;  e2_x = px + 18; e2_y = py + 20; p1_x = px + 22; p1_y = py + 8;  p2_x = px + 22; p2_y = py + 22; }
                else if (visual_look_dx == -1){ e1_x = px + 8;  e1_y = py + 6;  e2_x = px + 8;  e2_y = py + 20; p1_x = px + 7;  p1_y = py + 8;  p2_x = px + 7;  p2_y = py + 22; }
                else if (visual_look_dy == 1) { e1_x = px + 6;  e1_y = py + 18; e2_x = px + 20; e2_y = py + 18; p1_x = px + 8;  p1_y = py + 22; p2_x = px + 22; p2_y = py + 22; }

                SDL_SetRenderDrawColor(renderer, 245, 245, 245, 255);
                SDL_Rect eye1{ e1_x, e1_y, 6, 6 }, eye2{ e2_x, e2_y, 6, 6 };
                SDL_RenderFillRect(renderer, &eye1); SDL_RenderFillRect(renderer, &eye2);
                SDL_SetRenderDrawColor(renderer, 10, 10, 10, 255);
                SDL_Rect pup1{ p1_x, p1_y, 3, 3 }, pup2{ p2_x, p2_y, 3, 3 };
                SDL_RenderFillRect(renderer, &pup1); SDL_RenderFillRect(renderer, &pup2);

                // Мерцающий язык
                if ((now / 150) % 2 == 0) {
                    SDL_SetRenderDrawColor(renderer, 230, 57, 70, 255);
                    int tx = px + CELL_SIZE / 2;
                    int ty = py + CELL_SIZE / 2;
                    if (visual_look_dx == 1)       SDL_RenderDrawLine(renderer, px + CELL_SIZE - 2, ty, px + CELL_SIZE + 6, ty);
                    else if (visual_look_dx == -1) SDL_RenderDrawLine(renderer, px + 2, ty, px - 6, ty);
                    else if (visual_look_dy == -1) SDL_RenderDrawLine(renderer, tx, py + 2, tx, py - 6);
                    else if (visual_look_dy == 1)  SDL_RenderDrawLine(renderer, tx, py + CELL_SIZE - 2, tx, py + CELL_SIZE + 6);
                }
            } else {
                // Тело со сглаженным внутренним объемом
                SDL_Rect outer{ px + 3, py + 3, CELL_SIZE - 6, CELL_SIZE - 6 };
                SDL_SetRenderDrawColor(renderer, 27, 138, 56, 255);
                SDL_RenderFillRect(renderer, &outer);
                SDL_Rect inner{ px + 6, py + 6, CELL_SIZE - 12, CELL_SIZE - 12 };
                SDL_SetRenderDrawColor(renderer, 35, 165, 72, 255);
                SDL_RenderFillRect(renderer, &inner);
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
        }

        // Меню
        if (state == GameState::MENU) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 8, 12, 18, 225);
            SDL_Rect full{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            SDL_RenderFillRect(renderer, &full);

            std::string logo = "DISNAKECTOR";
            int logo_w = get_text_width(logo, 5);
            draw_text(renderer, logo, (SCREEN_WIDTH - logo_w) / 2, 110, 5, { 43, 216, 87, 255 });

            btn_play.draw(renderer);
            btn_settings.draw(renderer);
            btn_exit.draw(renderer);
        } else if (state == GameState::SETTINGS) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 8, 12, 18, 235);
            SDL_Rect full{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            SDL_RenderFillRect(renderer, &full);

            std::string title = "SETTINGS";
            int tw = get_text_width(title, 4);
            draw_text(renderer, title, (SCREEN_WIDTH - tw) / 2, 90, 4, { 255, 255, 255, 255 });

            slider_sfx.draw(renderer);
            slider_music.draw(renderer);
            btn_speed.draw(renderer);
            btn_back.draw(renderer);
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
