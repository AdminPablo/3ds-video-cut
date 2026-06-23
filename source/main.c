#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <malloc.h>
#include <unistd.h>
#include <math.h>
#include <3ds/ndsp/ndsp.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

// =====================================================
// CONFIG
// =====================================================
#define DCIM_PATH          "sdmc:/DCIM/100NIN03/"
#define APP_ROOT_PATH      "sdmc:/3DSVideoCut"
#define APP_DATA_PATH      APP_ROOT_PATH "/"
#define CACHE_PATH         APP_DATA_PATH "temp/thumbnails/"
#define EXPORT_VIDEO_PATH  APP_DATA_PATH "export.avi"

#define MAX_VIDEOS  128
#define THUMB_SIZE  64
#define MAX_JPEG_FRAME_SIZE (2 * 1024 * 1024)
#define EXPORT_FPS  15
#define PREVIEW_FRAME_MS 66

// Z values. Depth is disabled, but keeping layers explicit makes draw order clear.
#define Z_BG  0.0f
#define Z_UI  0.5f

// Colors
#define CLR_TEXT_BLACK   C2D_Color32(40, 40, 40, 255)
#define CLR_TEXT_GRAY    C2D_Color32(100, 100, 100, 255)
#define CLR_WHITE        C2D_Color32(255, 255, 255, 255)
#define CLR_SEL_BORDER   C2D_Color32(0, 200, 100, 255)
#define CLR_ORANGE       C2D_Color32(255, 140, 0, 255)

// App states
typedef enum {
    STATE_MENU = 0,
    STATE_GALLERY = 1,
    STATE_PLAYER = 2,
    STATE_EXPORT = 3,
    STATE_LOADING = 4,
} AppState;

typedef struct {
    char filename[256];
    bool selected;
    C2D_Image thumbnail;
    bool thumbnail_loaded;
} VideoClip;

// =====================================================
// Loading Screen
// =====================================================
typedef struct {
    float alpha;
    float spinner_angle;
    bool active;
    char message[256];
    bool fading_out;
} LoadingScreen;

// =====================================================
// Spinner audio
// =====================================================
typedef struct {
    ndspWaveBuf waveBuf;
    s16* audioBuffer;
    bool playing;
    int channel;
} SpinnerSound;

// =====================================================
// Fade transition
// =====================================================
typedef struct {
    bool active;
    float alpha;
    bool fading_in; // true = black to transparent, false = transparent to black
} FadeTransition;

// =====================================================
// Globals
// =====================================================
static VideoClip g_videos[MAX_VIDEOS];
static int g_video_count = 0;

static AppState g_state = STATE_MENU;
static int g_menu_sel = 0;

static int g_selected_index = 0;
static int g_scroll_offset = 0;

static C3D_RenderTarget* top_screen;
static C3D_RenderTarget* bottom_screen;

static C2D_SpriteSheet spriteSheet_top = NULL;
static C2D_SpriteSheet spriteSheet_bottom = NULL;
static C2D_Image bg_top_half;
static C2D_Image bg_bottom_half;
static bool bg_loaded = false;

static LoadingScreen loading_screen = {0};
static SpinnerSound g_spinner_sound = {0};
static FadeTransition g_fade = {0};
static char g_status_message[128] = "";

static void draw_loading_frame(void);
static void hide_loading_dialog(void);
static void load_thumbnail_for_video(int index);

// =====================================================
// Fade transitions
// =====================================================
static void start_fade_in(void) {
    g_fade.active = true;
    g_fade.alpha = 1.0f;
    g_fade.fading_in = true;
}

static void update_fade(void) {
    if (!g_fade.active) return;
    
    if (g_fade.fading_in) {
        g_fade.alpha -= 0.05f;
        if (g_fade.alpha <= 0.0f) {
            g_fade.alpha = 0.0f;
            g_fade.active = false;
        }
    } else {
        g_fade.alpha += 0.05f;
        if (g_fade.alpha >= 1.0f) {
            g_fade.alpha = 1.0f;
            g_fade.active = false;
        }
    }
}

static void draw_fade(void) {
    if (!g_fade.active && g_fade.alpha <= 0.0f) return;
    
    u8 alpha = (u8)(g_fade.alpha * 255);
    C2D_DrawRectSolid(0, 0, 0.0001f, 320, 240, C2D_Color32(0, 0, 0, alpha));
}

// =====================================================
// 2D setup
// =====================================================
static void setup_2d_no_depth(void) {
    C3D_DepthTest(false, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_DepthMap(true, 0.0f, 1.0f);
}

// =====================================================
// Audio functions
// =====================================================
static void init_spinner_sound(void) {
    ndspInit();
    
    g_spinner_sound.channel = 0;
    
    ndspChnReset(g_spinner_sound.channel);
    ndspChnSetInterp(g_spinner_sound.channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(g_spinner_sound.channel, 22050.0f);
    ndspChnSetFormat(g_spinner_sound.channel, NDSP_FORMAT_MONO_PCM16);
    
    int sample_rate = 22050;
    int duration_samples = sample_rate / 20;
    
    g_spinner_sound.audioBuffer = (s16*)linearAlloc(duration_samples * sizeof(s16));
    
    for (int i = 0; i < duration_samples; i++) {
        float t = (float)i / sample_rate;
        g_spinner_sound.audioBuffer[i] = (s16)(32767.0f * 0.3f * sinf(2.0f * 3.14159f * 440.0f * t));
    }
    
    memset(&g_spinner_sound.waveBuf, 0, sizeof(ndspWaveBuf));
    g_spinner_sound.waveBuf.data_vaddr = g_spinner_sound.audioBuffer;
    g_spinner_sound.waveBuf.nsamples = duration_samples;
    g_spinner_sound.waveBuf.looping = false;
    
    DSP_FlushDataCache(g_spinner_sound.audioBuffer, duration_samples * sizeof(s16));
}

static void play_spinner_sound(void) {
    if (!g_spinner_sound.audioBuffer) return;
    
    if (!ndspChnIsPlaying(g_spinner_sound.channel)) {
        ndspChnWaveBufAdd(g_spinner_sound.channel, &g_spinner_sound.waveBuf);
    }
}

static void cleanup_spinner_sound(void) {
    if (g_spinner_sound.audioBuffer) {
        ndspChnReset(g_spinner_sound.channel);
        linearFree(g_spinner_sound.audioBuffer);
        g_spinner_sound.audioBuffer = NULL;
    }
    ndspExit();
}

// =====================================================
// Loading Screen Functions
// =====================================================
static void show_loading(const char* message) {
    loading_screen.active = true;
    loading_screen.alpha = 0.0f;
    loading_screen.spinner_angle = 0.0f;
    loading_screen.fading_out = false;
    snprintf(loading_screen.message, sizeof(loading_screen.message), "%s", message);
}

static void show_loading_now(const char* message) {
    show_loading(message);
    loading_screen.alpha = 1.0f;
}

static void start_fade_out_loading(void) {
    loading_screen.fading_out = true;
}

static void update_loading(void) {
    if (!loading_screen.active) return;
    
    if (loading_screen.fading_out) {
        loading_screen.alpha -= 0.05f;
        if (loading_screen.alpha <= 0.0f) {
            loading_screen.alpha = 0.0f;
            loading_screen.active = false;
        }
    } else {
        if (loading_screen.alpha < 1.0f) {
            loading_screen.alpha += 0.05f;
            if (loading_screen.alpha > 1.0f) loading_screen.alpha = 1.0f;
        }
    }
    
    loading_screen.spinner_angle += 5.0f;
    if (loading_screen.spinner_angle >= 360.0f) {
        loading_screen.spinner_angle -= 360.0f;
        play_spinner_sound();
    }
}

static void draw_loading_dialog(void) {
    if (!loading_screen.active) return;
    if (loading_screen.alpha <= 0.0f) return;
    
    u8 alpha = (u8)(loading_screen.alpha * 200);
    u8 alpha_full = (u8)(loading_screen.alpha * 255);
    
    // Semi-transparent overlay over the full screen.
    C2D_DrawRectSolid(0, 0, 0.001f, 320, 240, C2D_Color32(0, 0, 0, alpha));
    
    // Centered dialog, not a full-screen panel.
    float dialog_w = 280;
    float dialog_h = 180;
    float dialog_x = (320 - dialog_w) / 2;
    float dialog_y = (240 - dialog_h) / 2;
    
    float corner_radius = 12.0f;
    u32 border_color = C2D_Color32(255, 200, 0, alpha_full);
    u32 bg_color = C2D_Color32(250, 250, 250, alpha_full);
    
    // Draw a rounded dialog background.
    C2D_DrawRectSolid(dialog_x + corner_radius, dialog_y, 0.0005f, 
                      dialog_w - 2*corner_radius, dialog_h, bg_color);
    C2D_DrawRectSolid(dialog_x, dialog_y + corner_radius, 0.0005f, 
                      corner_radius, dialog_h - 2*corner_radius, bg_color);
    C2D_DrawRectSolid(dialog_x + dialog_w - corner_radius, dialog_y + corner_radius, 0.0005f, 
                      corner_radius, dialog_h - 2*corner_radius, bg_color);
    
    // Approximate rounded corners.
    for (int i = 0; i < corner_radius; i++) {
        for (int j = 0; j < corner_radius; j++) {
            float dist = sqrtf(i*i + j*j);
            if (dist < corner_radius) {
                // Top-left corner
                C2D_DrawRectSolid(dialog_x + corner_radius - i, dialog_y + corner_radius - j, 
                                 0.0005f, 1, 1, bg_color);
                // Top-right corner
                C2D_DrawRectSolid(dialog_x + dialog_w - corner_radius + i - 1, dialog_y + corner_radius - j, 
                                 0.0005f, 1, 1, bg_color);
                // Bottom-left corner
                C2D_DrawRectSolid(dialog_x + corner_radius - i, dialog_y + dialog_h - corner_radius + j - 1, 
                                 0.0005f, 1, 1, bg_color);
                // Bottom-right corner
                C2D_DrawRectSolid(dialog_x + dialog_w - corner_radius + i - 1, dialog_y + dialog_h - corner_radius + j - 1, 
                                 0.0005f, 1, 1, bg_color);
            }
        }
    }
    
    // Draw yellow border.
    float border_width = 3.0f;
    
    // Straight borders
    C2D_DrawRectSolid(dialog_x + corner_radius, dialog_y, 0.0004f, 
                      dialog_w - 2*corner_radius, border_width, border_color);
    C2D_DrawRectSolid(dialog_x + corner_radius, dialog_y + dialog_h - border_width, 0.0004f, 
                      dialog_w - 2*corner_radius, border_width, border_color);
    C2D_DrawRectSolid(dialog_x, dialog_y + corner_radius, 0.0004f, 
                      border_width, dialog_h - 2*corner_radius, border_color);
    C2D_DrawRectSolid(dialog_x + dialog_w - border_width, dialog_y + corner_radius, 0.0004f, 
                      border_width, dialog_h - 2*corner_radius, border_color);
    
    // Corner borders
    for (int i = 0; i < corner_radius; i++) {
        for (int j = 0; j < corner_radius; j++) {
            float dist = sqrtf(i*i + j*j);
            if (dist >= corner_radius - border_width && dist < corner_radius) {
                C2D_DrawRectSolid(dialog_x + corner_radius - i, dialog_y + corner_radius - j, 
                                 0.0004f, 1, 1, border_color);
                C2D_DrawRectSolid(dialog_x + dialog_w - corner_radius + i - 1, dialog_y + corner_radius - j, 
                                 0.0004f, 1, 1, border_color);
                C2D_DrawRectSolid(dialog_x + corner_radius - i, dialog_y + dialog_h - corner_radius + j - 1, 
                                 0.0004f, 1, 1, border_color);
                C2D_DrawRectSolid(dialog_x + dialog_w - corner_radius + i - 1, dialog_y + dialog_h - corner_radius + j - 1, 
                                 0.0004f, 1, 1, border_color);
            }
        }
    }
    
    // Text
    C2D_TextBuf buf = C2D_TextBufNew(512);
    C2D_Text t;
    
    // Title
    C2D_TextParse(&t, buf, "Please wait...");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, dialog_x + 35, dialog_y + 30, 0.0003f, 
                 0.5f, 0.5f, C2D_Color32(80, 80, 80, alpha_full));
    
    // Centered custom message
    C2D_TextParse(&t, buf, loading_screen.message);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, dialog_x + 40, dialog_y + 65, 0.0003f, 
                 0.45f, 0.45f, C2D_Color32(100, 100, 100, alpha_full));
    
    // Warning
    C2D_TextParse(&t, buf, "Do not remove the SD card");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, dialog_x + 50, dialog_y + 90, 0.0003f, 
                 0.42f, 0.42f, C2D_Color32(120, 120, 120, alpha_full));
    
    C2D_TextParse(&t, buf, "or power off the console.");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, dialog_x + 55, dialog_y + 105, 0.0003f, 
                 0.42f, 0.42f, C2D_Color32(120, 120, 120, alpha_full));
    
    // Animated spinner centered in the dialog.
    float spinner_x = dialog_x + dialog_w / 2;
    float spinner_y = dialog_y + dialog_h - 40;
    float spinner_radius = 18.0f;
    
    for (int i = 0; i < 12; i++) {
        float angle = (loading_screen.spinner_angle + i * 30.0f) * 3.14159f / 180.0f;
        float dot_x = spinner_x + cosf(angle) * spinner_radius;
        float dot_y = spinner_y + sinf(angle) * spinner_radius;
        
        // Decreasing opacity creates the motion effect.
        u8 dot_alpha = (u8)(alpha_full * (1.0f - (float)i / 12.0f));
        
        // Dot size
        float dot_size = 5.0f;
        
        // Same yellow as the border.
        C2D_DrawRectSolid(dot_x - dot_size/2, dot_y - dot_size/2, 0.0002f, 
                         dot_size, dot_size, C2D_Color32(255, 200, 0, dot_alpha));
    }
    
    C2D_TextBufDelete(buf);
}

// =====================================================
// Helpers
// =====================================================
static size_t bounded_strlen(const char* text, size_t max_len) {
    size_t len = 0;
    if (!text) return 0;
    while (len < max_len && text[len] != '\0') len++;
    return len;
}

static void copy_string(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    size_t len = bounded_strlen(src, dst_size - 1);
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

static void set_status_message(const char* message) {
    copy_string(g_status_message, sizeof(g_status_message), message);
}

static bool join_path(char* out, size_t out_size, const char* dir, const char* name) {
    if (!out || !dir || !name || out_size == 0) return false;

    size_t dir_len = strlen(dir);
    size_t name_len = bounded_strlen(name, 255);
    if (dir_len + name_len + 1 > out_size) {
        out[0] = '\0';
        return false;
    }

    memcpy(out, dir, dir_len);
    memcpy(out + dir_len, name, name_len);
    out[dir_len + name_len] = '\0';
    return true;
}

static bool get_video_path(const char* filename, char* out_path, size_t out_size) {
    return join_path(out_path, out_size, DCIM_PATH, filename);
}

static bool get_file_size(FILE* file, long* out_size) {
    if (!file || !out_size) return false;

    long original = ftell(file);
    if (original < 0) original = 0;

    if (fseek(file, 0, SEEK_END) != 0) return false;
    long size = ftell(file);
    if (size < 0) return false;

    if (fseek(file, original, SEEK_SET) != 0) return false;
    *out_size = size;
    return true;
}

static void ensure_cache_folder(void) {
    mkdir(APP_ROOT_PATH, 0777);
    mkdir(APP_DATA_PATH "temp", 0777);
    mkdir(CACHE_PATH, 0777);
}

static void load_backgrounds(void) {
    spriteSheet_top = C2D_SpriteSheetLoad("romfs:/gfx/bg_top.t3x");
    spriteSheet_bottom = C2D_SpriteSheetLoad("romfs:/gfx/bg_bottom.t3x");

    if (!spriteSheet_top || !spriteSheet_bottom) return;

    bg_top_half = C2D_SpriteSheetGetImage(spriteSheet_top, 0);
    bg_bottom_half = C2D_SpriteSheetGetImage(spriteSheet_bottom, 0);

    if (bg_top_half.tex && bg_bottom_half.tex) bg_loaded = true;
}

static bool get_thumbnail_path(const char* video_name, char* out_path, size_t size) {
    char base[256];
    copy_string(base, sizeof(base), video_name);

    char* dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    size_t cache_len = strlen(CACHE_PATH);
    size_t base_len = strlen(base);
    size_t ext_len = strlen(".jpg");

    if (cache_len + base_len + ext_len + 1 > size) {
        if (size > 0) out_path[0] = '\0';
        return false;
    }

    memcpy(out_path, CACHE_PATH, cache_len);
    memcpy(out_path + cache_len, base, base_len);
    memcpy(out_path + cache_len + base_len, ".jpg", ext_len);
    out_path[cache_len + base_len + ext_len] = '\0';
    return true;
}

static bool append_jpeg_byte(u8** buffer, size_t* len, size_t* capacity, u8 value) {
    if (*len >= MAX_JPEG_FRAME_SIZE) return false;

    if (*len >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? (64 * 1024) : (*capacity * 2);
        if (new_capacity > MAX_JPEG_FRAME_SIZE) new_capacity = MAX_JPEG_FRAME_SIZE;
        if (new_capacity <= *capacity) return false;

        u8* new_buffer = (u8*)realloc(*buffer, new_capacity);
        if (!new_buffer) return false;

        *buffer = new_buffer;
        *capacity = new_capacity;
    }

    (*buffer)[(*len)++] = value;
    return true;
}

static bool copy_jpeg_from_memory(const u8* data, size_t size, u8** out_jpg, size_t* out_len) {
    if (!data || !out_jpg || !out_len || size < 4 || size > MAX_JPEG_FRAME_SIZE) return false;

    size_t soi = (size_t)-1;
    for (size_t i = 0; i + 1 < size; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) {
            soi = i;
            break;
        }
    }

    if (soi == (size_t)-1) return false;

    size_t eoi = (size_t)-1;
    for (size_t i = soi + 2; i + 1 < size; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9) {
            eoi = i + 2;
            break;
        }
    }

    if (eoi == (size_t)-1 || eoi <= soi) return false;

    size_t jpg_len = eoi - soi;
    u8* jpg = (u8*)malloc(jpg_len);
    if (!jpg) return false;

    memcpy(jpg, data + soi, jpg_len);
    *out_jpg = jpg;
    *out_len = jpg_len;
    return true;
}

static bool is_stream_chunk_id(const u8 id[4]) {
    if (id[0] < '0' || id[0] > '9') return false;
    if (id[1] < '0' || id[1] > '9') return false;

    if (id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) return true;
    if (id[2] == 'w' && id[3] == 'b') return true;

    return false;
}

static bool is_primary_video_chunk_id(const u8 id[4]) {
    return id[0] == '0' && id[1] == '0' && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b');
}

static u32 read_u32_le(const u8 bytes[4]) {
    return ((u32)bytes[0]) |
           ((u32)bytes[1] << 8) |
           ((u32)bytes[2] << 16) |
           ((u32)bytes[3] << 24);
}

static bool read_primary_video_jpeg_chunk_from_file(FILE* file, long* pos, long file_size, u8** out_jpg, size_t* out_len) {
    if (!file || !pos || !out_jpg || !out_len || file_size <= 0) return false;

    long scan = *pos;
    if (scan < 0 || scan >= file_size) scan = 0;

    while (scan + 8 < file_size) {
        u8 id[4];
        u8 size_bytes[4];

        if (fseek(file, scan, SEEK_SET) != 0) return false;
        if (fread(id, 1, sizeof(id), file) != sizeof(id)) break;
        if (fread(size_bytes, 1, sizeof(size_bytes), file) != sizeof(size_bytes)) break;

        u32 chunk_size = read_u32_le(size_bytes);
        long data_pos = scan + 8;
        long next_pos = data_pos + (long)chunk_size + (chunk_size & 1);

        if (is_stream_chunk_id(id) && chunk_size > 0 && next_pos <= file_size) {
            if (is_primary_video_chunk_id(id) && chunk_size <= MAX_JPEG_FRAME_SIZE) {
                u8* chunk = (u8*)malloc(chunk_size);
                if (!chunk) return false;

                if (fread(chunk, 1, chunk_size, file) != chunk_size) {
                    free(chunk);
                    return false;
                }

                bool ok = copy_jpeg_from_memory(chunk, chunk_size, out_jpg, out_len);
                free(chunk);
                *pos = next_pos;
                if (ok) return true;
            }

            scan = next_pos;
        } else {
            scan++;
        }
    }

    *pos = file_size;
    return false;
}

static bool read_next_jpeg_from_file(FILE* file, long* pos, long file_size, u8** out_jpg, size_t* out_len) {
    if (!file || !pos || !out_jpg || !out_len || file_size <= 0) return false;

    if (*pos < 0 || *pos >= file_size) *pos = 0;
    if (fseek(file, *pos, SEEK_SET) != 0) return false;

    bool in_jpeg = false;
    int prev = -1;
    int c;

    u8* buffer = NULL;
    size_t len = 0;
    size_t capacity = 0;

    while ((c = fgetc(file)) != EOF) {
        long after = ftell(file);

        if (!in_jpeg) {
            if (prev == 0xFF && c == 0xD8) {
                in_jpeg = true;
                if (!append_jpeg_byte(&buffer, &len, &capacity, 0xFF) ||
                    !append_jpeg_byte(&buffer, &len, &capacity, 0xD8)) {
                    free(buffer);
                    if (after >= 0) *pos = after;
                    return false;
                }
            }
        } else {
            if (!append_jpeg_byte(&buffer, &len, &capacity, (u8)c)) {
                free(buffer);
                if (after >= 0) *pos = after;
                return false;
            }

            if (prev == 0xFF && c == 0xD9) {
                if (after >= 0) *pos = after;
                *out_jpg = buffer;
                *out_len = len;
                return true;
            }
        }

        prev = c;
    }

    free(buffer);
    *pos = file_size;
    return false;
}

static bool read_next_camera_jpeg_frame(FILE* file, long* pos, long file_size, u8** out_jpg, size_t* out_len) {
    if (!file || !pos || !out_jpg || !out_len || file_size <= 0) return false;

    long start_pos = *pos;
    if (read_primary_video_jpeg_chunk_from_file(file, pos, file_size, out_jpg, out_len)) {
        return true;
    }

    if (start_pos <= 0) {
        *pos = 0;
        return read_next_jpeg_from_file(file, pos, file_size, out_jpg, out_len);
    }

    return false;
}

// =====================================================
// Texture tiling (RGB8) for 3DS
// =====================================================
static void tile_texture(u8* dst, const u8* src, int width, int height) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int dstPos = ((((y >> 3) * (width >> 3) + (x >> 3)) << 6) +
                         ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                          ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))) * 3;

            int srcPos = (y * width + x) * 3;

            dst[dstPos + 0] = src[srcPos + 2];
            dst[dstPos + 1] = src[srcPos + 1];
            dst[dstPos + 2] = src[srcPos + 0];
        }
    }
}

// =====================================================
// Load JPEG -> C2D_Image
// =====================================================
static C2D_Image load_thumbnail_from_jpeg(const char* jpg_path) {
    FILE* file = fopen(jpg_path, "rb");
    if (!file) return (C2D_Image){NULL, NULL};

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > MAX_JPEG_FRAME_SIZE) {
        fclose(file);
        return (C2D_Image){NULL, NULL};
    }

    unsigned char* fileData = (unsigned char*)malloc(fileSize);
    if (!fileData) {
        fclose(file);
        return (C2D_Image){NULL, NULL};
    }

    fread(fileData, 1, fileSize, file);
    fclose(file);

    int width, height, channels;
    unsigned char* imgData = stbi_load_from_memory(fileData, (int)fileSize, &width, &height, &channels, 3);
    free(fileData);
    if (!imgData) return (C2D_Image){NULL, NULL};

    unsigned char* resized = (unsigned char*)malloc(THUMB_SIZE * THUMB_SIZE * 3);
    if (!resized) {
        stbi_image_free(imgData);
        return (C2D_Image){NULL, NULL};
    }

    float sx = (float)width / THUMB_SIZE;
    float sy = (float)height / THUMB_SIZE;

    for (int y = 0; y < THUMB_SIZE; y++) {
        for (int x = 0; x < THUMB_SIZE; x++) {
            int src_x = (int)(x * sx);
            int src_y = (int)(y * sy);

            if (src_x < 0) src_x = 0;
            if (src_y < 0) src_y = 0;
            if (src_x >= width) src_x = width - 1;
            if (src_y >= height) src_y = height - 1;

            int src_idx = (src_y * width + src_x) * 3;
            int dst_idx = (y * THUMB_SIZE + x) * 3;

            resized[dst_idx + 0] = imgData[src_idx + 0];
            resized[dst_idx + 1] = imgData[src_idx + 1];
            resized[dst_idx + 2] = imgData[src_idx + 2];
        }
    }
    stbi_image_free(imgData);

    C3D_Tex* tex = (C3D_Tex*)linearAlloc(sizeof(C3D_Tex));
    if (!tex) {
        free(resized);
        return (C2D_Image){NULL, NULL};
    }

    if (!C3D_TexInit(tex, THUMB_SIZE, THUMB_SIZE, GPU_RGB8)) {
        linearFree(tex);
        free(resized);
        return (C2D_Image){NULL, NULL};
    }

    tile_texture((u8*)tex->data, resized, THUMB_SIZE, THUMB_SIZE);
    free(resized);

    C3D_TexFlush(tex);
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);

    Tex3DS_SubTexture* subtex = (Tex3DS_SubTexture*)linearAlloc(sizeof(Tex3DS_SubTexture));
    if (!subtex) {
        C3D_TexDelete(tex);
        linearFree(tex);
        return (C2D_Image){NULL, NULL};
    }

    subtex->width = THUMB_SIZE;
    subtex->height = THUMB_SIZE;
    subtex->left = 0.0f;
    subtex->top = 1.0f;
    subtex->right = 1.0f;
    subtex->bottom = 0.0f;

    return (C2D_Image){tex, subtex};
}

// =====================================================
// Extract JPEG from AVI
// =====================================================
static bool extract_first_jpeg_from_avi(const char* avi_path, const char* jpg_path) {
    FILE* f = fopen(avi_path, "rb");
    if (!f) return false;

    long size = 0;
    if (!get_file_size(f, &size) || size <= 0) {
        fclose(f);
        return false;
    }

    long pos = 0;
    u8* jpg = NULL;
    size_t jpg_len = 0;
    if (!read_next_camera_jpeg_frame(f, &pos, size, &jpg, &jpg_len)) {
        fclose(f);
        return false;
    }
    fclose(f);

    FILE* out = fopen(jpg_path, "wb");
    if (!out) {
        free(jpg);
        return false;
    }

    bool ok = fwrite(jpg, 1, jpg_len, out) == jpg_len;
    fclose(out);
    free(jpg);
    return ok;
}

// =====================================================
// Scan videos
// =====================================================
static void scan_videos(void) {
    g_video_count = 0;

    DIR* dir = opendir(DCIM_PATH);
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && g_video_count < MAX_VIDEOS) {
        const char* n = ent->d_name;
        if (strstr(n, ".AVI") || strstr(n, ".avi")) {
            copy_string(g_videos[g_video_count].filename, sizeof(g_videos[g_video_count].filename), n);
            g_videos[g_video_count].selected = false;
            g_videos[g_video_count].thumbnail_loaded = false;
            g_video_count++;
        }
    }
    closedir(dir);
}

// =====================================================
// Generate thumbnails
// =====================================================
static void generate_all_thumbnails(void) {
    if (g_video_count == 0) return;

    show_loading_now("Preparing video thumbnails...");

    for (int i = 0; i < g_video_count; i++) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Loading thumbnail %d/%d...", i + 1, g_video_count);
        copy_string(loading_screen.message, sizeof(loading_screen.message), msg);

        draw_loading_frame();
        load_thumbnail_for_video(i);
    }
    
    hide_loading_dialog();
    
    start_fade_in();
}

static void load_thumbnail_for_video(int index) {
    if (index < 0 || index >= g_video_count) return;
    if (g_videos[index].thumbnail_loaded) return;

    char thumb_path[512];
    if (!get_thumbnail_path(g_videos[index].filename, thumb_path, sizeof(thumb_path))) return;

    C2D_Image img = load_thumbnail_from_jpeg(thumb_path);

    if (img.tex == NULL) {
        char video_path[512];
        if (!get_video_path(g_videos[index].filename, video_path, sizeof(video_path))) return;

        if (extract_first_jpeg_from_avi(video_path, thumb_path)) {
            img = load_thumbnail_from_jpeg(thumb_path);
        }
    }

    if (img.tex != NULL) {
        g_videos[index].thumbnail = img;
        g_videos[index].thumbnail_loaded = true;
    }
}

// =====================================================
// UI helpers
// =====================================================
static void draw_button(float x, float y, float w, float h, bool selected) {
    u32 border = selected ? CLR_ORANGE : C2D_Color32(60, 60, 60, 255);
    u32 fill   = C2D_Color32(245, 245, 245, 255);

    C2D_DrawRectSolid(x-2, y-2, Z_UI, w+4, h+4, border);
    C2D_DrawRectSolid(x, y, Z_UI, w, h, fill);
}

static bool is_point_in_rect(float px, float py, float rx, float ry, float rw, float rh) {
    return px >= rx && px <= rx + rw && py >= ry && py <= ry + rh;
}

static void draw_bg_top(void) {
    if (bg_loaded) {
        C2D_DrawImageAt(bg_top_half, 0, 0, Z_BG, NULL, 1.0f, 1.0f);
        C2D_DrawImageAt(bg_top_half, 200, 0, Z_BG, NULL, -1.0f, 1.0f);
    } else {
        C2D_TargetClear(top_screen, C2D_Color32(230, 230, 250, 255));
    }
}

static void draw_bg_bottom(void) {
    if (bg_loaded) {
        C2D_DrawImageAt(bg_bottom_half, 0, 0, Z_BG, NULL, 1.0f, 1.0f);
        C2D_DrawImageAt(bg_bottom_half, 160, 0, Z_BG, NULL, -1.0f, 1.0f);
    } else {
        C2D_TargetClear(bottom_screen, C2D_Color32(240, 240, 240, 255));
    }
}

static void draw_loading_frame(void) {
    update_loading();

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_SceneBegin(bottom_screen);

    draw_bg_bottom();
    draw_loading_dialog();

    C3D_FrameEnd(0);
}

static void hide_loading_dialog(void) {
    start_fade_out_loading();

    while (loading_screen.active) {
        draw_loading_frame();
    }
}

// =====================================================
// PLAYER
// =====================================================
typedef struct {
    FILE* file;
    long file_size;
    long pos;
    int frame_no;

    C2D_Image frame_img;
    bool frame_loaded;
    int frame_w;
    int frame_h;
} MjpegPlayer;

static void player_free_frame(MjpegPlayer* p) {
    if (p->frame_loaded && p->frame_img.tex) {
        C3D_TexDelete(p->frame_img.tex);
        linearFree((void*)p->frame_img.tex);
        if (p->frame_img.subtex) linearFree((void*)p->frame_img.subtex);
    }
    p->frame_loaded = false;
    p->frame_img = (C2D_Image){NULL, NULL};
}

static void player_close(MjpegPlayer* p) {
    player_free_frame(p);
    if (p->file) fclose(p->file);
    memset(p, 0, sizeof(*p));
}

static bool player_open(MjpegPlayer* p, const char* path) {
    memset(p, 0, sizeof(*p));

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    long sz = 0;
    if (!get_file_size(f, &sz) || sz <= 0) {
        fclose(f);
        return false;
    }

    p->file = f;
    p->file_size = sz;
    p->pos = 0;
    p->frame_no = 0;
    return true;
}

static bool player_next_jpeg(MjpegPlayer* p, u8** jpg, size_t* jpg_len) {
    if (!p->file) return false;
    return read_next_camera_jpeg_frame(p->file, &p->pos, p->file_size, jpg, jpg_len);
}

static C2D_Image tex_from_rgb24_scaled(const unsigned char* rgb, int w, int h, int out_w, int out_h) {
    unsigned char* resized = (unsigned char*)malloc(out_w * out_h * 3);
    if (!resized) return (C2D_Image){NULL,NULL};

    float sx = (float)w / out_w;
    float sy = (float)h / out_h;

    for (int y=0; y<out_h; y++) {
        for (int x=0; x<out_w; x++) {
            int src_x = (int)(x * sx);
            int src_y = (int)(y * sy);
            if (src_x < 0) src_x = 0;
            if (src_y < 0) src_y = 0;
            if (src_x >= w) src_x = w-1;
            if (src_y >= h) src_y = h-1;

            int s = (src_y*w + src_x)*3;
            int d = (y*out_w + x)*3;
            resized[d+0] = rgb[s+0];
            resized[d+1] = rgb[s+1];
            resized[d+2] = rgb[s+2];
        }
    }

    C3D_Tex* tex = (C3D_Tex*)linearAlloc(sizeof(C3D_Tex));
    if (!tex) { free(resized); return (C2D_Image){NULL,NULL}; }
    if (!C3D_TexInit(tex, out_w, out_h, GPU_RGB8)) {
        linearFree(tex); free(resized);
        return (C2D_Image){NULL,NULL};
    }

    tile_texture((u8*)tex->data, resized, out_w, out_h);
    free(resized);

    C3D_TexFlush(tex);
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);

    Tex3DS_SubTexture* sub = (Tex3DS_SubTexture*)linearAlloc(sizeof(Tex3DS_SubTexture));
    if (!sub) {
        C3D_TexDelete(tex); linearFree(tex);
        return (C2D_Image){NULL,NULL};
    }

    sub->width = out_w;
    sub->height = out_h;
    sub->left = 0.0f;
    sub->top = 1.0f;
    sub->right = 1.0f;
    sub->bottom = 0.0f;

    return (C2D_Image){tex, sub};
}

static bool player_decode_next_frame(MjpegPlayer* p) {
    for (int attempt = 0; attempt < 8; attempt++) {
        u8* jpg = NULL;
        size_t jpg_len = 0;
        if (!player_next_jpeg(p, &jpg, &jpg_len)) {
            if (p->pos == 0) return false;
            p->pos = 0;
            continue;
        }

        int w,h,ch;
        unsigned char* rgb = stbi_load_from_memory(jpg, (int)jpg_len, &w, &h, &ch, 3);
        free(jpg);
        if (!rgb) continue;

        player_free_frame(p);

        int out_w = 320;
        int out_h = 180;
        p->frame_img = tex_from_rgb24_scaled(rgb, w, h, out_w, out_h);
        stbi_image_free(rgb);

        if (!p->frame_img.tex) return false;

        p->frame_loaded = true;
        p->frame_w = out_w;
        p->frame_h = out_h;
        p->frame_no++;
        return true;
    }

    return false;
}

// =====================================================
// Screens
// =====================================================
static void draw_menu_screen(void) {
    C2D_SceneBegin(top_screen);
    draw_bg_top();

    C2D_TextBuf buf = C2D_TextBufNew(256);
    C2D_Text t;

    C2D_TextParse(&t, buf, "3DS Video Cut");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, 110, 30, Z_UI, 1.0f, 1.0f, CLR_TEXT_BLACK);

    C2D_TextParse(&t, buf, "Preview, select, and export 3DS camera videos");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, 35, 90, Z_UI, 0.5f, 0.5f, CLR_TEXT_GRAY);

    C2D_TextBufDelete(buf);

    C2D_SceneBegin(bottom_screen);
    draw_bg_bottom();

    buf = C2D_TextBufNew(256);

    draw_button(20, 80, 130, 65, g_menu_sel == 0);
    C2D_TextParse(&t, buf, "Videos");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, 60, 103, Z_UI, 0.7f, 0.7f, CLR_TEXT_BLACK);

    draw_button(170, 80, 130, 65, g_menu_sel == 1);
    C2D_TextParse(&t, buf, "Exit");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, 215, 103, Z_UI, 0.7f, 0.7f, CLR_TEXT_BLACK);

    C2D_TextBufDelete(buf);
    
    draw_fade();
}

static void draw_gallery_screen(void) {
    C2D_SceneBegin(top_screen);
    draw_bg_top();

    C2D_TextBuf buf = C2D_TextBufNew(512);
    C2D_Text t;

    if (g_video_count > 0) {
        C2D_TextParse(&t, buf, g_videos[g_selected_index].filename);
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, 20, 20, Z_UI, 0.55f, 0.55f, CLR_TEXT_BLACK);

        if (g_videos[g_selected_index].thumbnail_loaded) {
            C2D_DrawImageAt(g_videos[g_selected_index].thumbnail, 20, 55, Z_UI, NULL, 2.0f, 2.0f);
        }

        int sel = 0;
        for (int i=0;i<g_video_count;i++) if (g_videos[i].selected) sel++;

        char status[96];
        snprintf(status, sizeof(status), "Selected: %d/%d", sel, g_video_count);
        C2D_TextParse(&t, buf, status);
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, 20, 200, Z_UI, 0.5f, 0.5f, CLR_TEXT_GRAY);

        if (g_status_message[0] != '\0') {
            C2D_TextParse(&t, buf, g_status_message);
            C2D_TextOptimize(&t);
            C2D_DrawText(&t, C2D_WithColor, 170, 170, Z_UI, 0.42f, 0.42f, CLR_ORANGE);
        }

        C2D_TextParse(&t, buf, "[A] Select  [Y] Preview  [X] Export AVI");
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, 20, 220, Z_UI, 0.45f, 0.45f, CLR_TEXT_BLACK);
    } else {
        C2D_TextParse(&t, buf, "No videos found in DCIM/100NIN03");
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, 10, 110, Z_UI, 0.55f, 0.55f, CLR_TEXT_BLACK);
    }

    C2D_TextBufDelete(buf);

    C2D_SceneBegin(bottom_screen);
    draw_bg_bottom();

    buf = C2D_TextBufNew(256);

    for (int i=0;i<g_video_count;i++) {
        int col = i % 4;
        int row = i / 4;
        int x = col * 80 + 10;
        int y = row * 80 + 10 - g_scroll_offset;

        if (y > 240 || y < -70) continue;

        u32 border = (i == g_selected_index) ? CLR_ORANGE :
                     (g_videos[i].selected ? CLR_SEL_BORDER : CLR_TEXT_GRAY);

        C2D_DrawRectSolid(x-2, y-2, Z_UI, 68, 68, border);
        C2D_DrawRectSolid(x, y, Z_UI, 64, 64, CLR_WHITE);

        if (g_videos[i].thumbnail_loaded) {
            C2D_DrawImageAt(g_videos[i].thumbnail, x, y, Z_UI, NULL, 1.0f, 1.0f);
        }

        if (g_videos[i].selected) {
            C2D_DrawRectSolid(x+46, y+46, Z_UI, 14, 14, CLR_SEL_BORDER);
        }
    }

    C2D_TextParse(&t, buf, "[B] Back   DPad: move");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, 10, 220, Z_UI, 0.45f, 0.45f, CLR_TEXT_BLACK);

    C2D_TextBufDelete(buf);
}

// =====================================================
// Export
// =====================================================
typedef struct {
    u32 offset;
    u32 size;
} AviIndexEntry;

typedef struct {
    AviIndexEntry* entries;
    int count;
    int capacity;
    u32 max_frame_size;
} AviIndex;

typedef struct {
    long riff_size_pos;
    long avih_total_frames_pos;
    long avih_suggested_buffer_pos;
    long strh_length_pos;
    long strh_suggested_buffer_pos;
    long movi_size_pos;
    long movi_data_start;
} AviPatchOffsets;

static bool write_fourcc(FILE* file, const char* text) {
    return fwrite(text, 1, 4, file) == 4;
}

static bool write_u16(FILE* file, u16 value) {
    u8 bytes[2];
    bytes[0] = (u8)(value & 0xFF);
    bytes[1] = (u8)((value >> 8) & 0xFF);
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static bool write_u32(FILE* file, u32 value) {
    u8 bytes[4];
    bytes[0] = (u8)(value & 0xFF);
    bytes[1] = (u8)((value >> 8) & 0xFF);
    bytes[2] = (u8)((value >> 16) & 0xFF);
    bytes[3] = (u8)((value >> 24) & 0xFF);
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static bool write_s32(FILE* file, s32 value) {
    return write_u32(file, (u32)value);
}

static bool patch_u32_at(FILE* file, long offset, u32 value) {
    long current = ftell(file);
    if (current < 0) return false;
    if (fseek(file, offset, SEEK_SET) != 0) return false;
    if (!write_u32(file, value)) return false;
    return fseek(file, current, SEEK_SET) == 0;
}

static bool avi_index_add(AviIndex* index, u32 offset, u32 size) {
    if (index->count >= index->capacity) {
        int new_capacity = (index->capacity == 0) ? 256 : index->capacity * 2;
        AviIndexEntry* new_entries = (AviIndexEntry*)realloc(index->entries, new_capacity * sizeof(AviIndexEntry));
        if (!new_entries) return false;

        index->entries = new_entries;
        index->capacity = new_capacity;
    }

    index->entries[index->count].offset = offset;
    index->entries[index->count].size = size;
    index->count++;
    if (size > index->max_frame_size) index->max_frame_size = size;
    return true;
}

static bool write_avi_header(FILE* out, int width, int height, AviPatchOffsets* patches) {
    memset(patches, 0, sizeof(*patches));

    if (!write_fourcc(out, "RIFF")) return false;
    patches->riff_size_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    if (!write_fourcc(out, "AVI ")) return false;

    if (!write_fourcc(out, "LIST")) return false;
    long hdrl_size_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    if (!write_fourcc(out, "hdrl")) return false;

    if (!write_fourcc(out, "avih")) return false;
    if (!write_u32(out, 56)) return false;
    if (!write_u32(out, 1000000 / EXPORT_FPS)) return false;
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, 0x10)) return false;
    patches->avih_total_frames_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, 1)) return false;
    patches->avih_suggested_buffer_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, (u32)width)) return false;
    if (!write_u32(out, (u32)height)) return false;
    for (int i = 0; i < 4; i++) {
        if (!write_u32(out, 0)) return false;
    }

    if (!write_fourcc(out, "LIST")) return false;
    long strl_size_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    if (!write_fourcc(out, "strl")) return false;

    if (!write_fourcc(out, "strh")) return false;
    if (!write_u32(out, 56)) return false;
    if (!write_fourcc(out, "vids")) return false;
    if (!write_fourcc(out, "MJPG")) return false;
    if (!write_u32(out, 0)) return false;
    if (!write_u16(out, 0)) return false;
    if (!write_u16(out, 0)) return false;
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, 1)) return false;
    if (!write_u32(out, EXPORT_FPS)) return false;
    if (!write_u32(out, 0)) return false;
    patches->strh_length_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    patches->strh_suggested_buffer_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, 0xFFFFFFFF)) return false;
    if (!write_u32(out, 0)) return false;
    if (!write_u16(out, 0)) return false;
    if (!write_u16(out, 0)) return false;
    if (!write_u16(out, (u16)width)) return false;
    if (!write_u16(out, (u16)height)) return false;

    if (!write_fourcc(out, "strf")) return false;
    if (!write_u32(out, 40)) return false;
    if (!write_u32(out, 40)) return false;
    if (!write_s32(out, width)) return false;
    if (!write_s32(out, height)) return false;
    if (!write_u16(out, 1)) return false;
    if (!write_u16(out, 24)) return false;
    if (!write_fourcc(out, "MJPG")) return false;
    if (!write_u32(out, (u32)(width * height * 3))) return false;
    if (!write_s32(out, 0)) return false;
    if (!write_s32(out, 0)) return false;
    if (!write_u32(out, 0)) return false;
    if (!write_u32(out, 0)) return false;

    long strl_end = ftell(out);
    if (!patch_u32_at(out, strl_size_pos, (u32)(strl_end - (strl_size_pos + 4)))) return false;

    long hdrl_end = ftell(out);
    if (!patch_u32_at(out, hdrl_size_pos, (u32)(hdrl_end - (hdrl_size_pos + 4)))) return false;

    if (!write_fourcc(out, "LIST")) return false;
    patches->movi_size_pos = ftell(out);
    if (!write_u32(out, 0)) return false;
    if (!write_fourcc(out, "movi")) return false;
    patches->movi_data_start = ftell(out);

    return true;
}

static bool avi_write_frame(FILE* out, const AviPatchOffsets* patches, AviIndex* index, const u8* jpg, size_t jpg_len) {
    if (!out || !patches || !index || !jpg || jpg_len == 0 || jpg_len > 0xFFFFFFFF) return false;

    long chunk_pos = ftell(out);
    if (chunk_pos < 0) return false;

    if (!write_fourcc(out, "00dc")) return false;
    if (!write_u32(out, (u32)jpg_len)) return false;
    if (fwrite(jpg, 1, jpg_len, out) != jpg_len) return false;
    if ((jpg_len & 1) && fputc(0, out) == EOF) return false;

    u32 offset = (u32)(chunk_pos - patches->movi_data_start);
    return avi_index_add(index, offset, (u32)jpg_len);
}

static bool avi_finalize(FILE* out, const AviPatchOffsets* patches, const AviIndex* index) {
    long idx1_start = ftell(out);
    if (idx1_start < 0) return false;

    if (!write_fourcc(out, "idx1")) return false;
    if (!write_u32(out, (u32)(index->count * 16))) return false;

    for (int i = 0; i < index->count; i++) {
        if (!write_fourcc(out, "00dc")) return false;
        if (!write_u32(out, 0x10)) return false;
        if (!write_u32(out, index->entries[i].offset)) return false;
        if (!write_u32(out, index->entries[i].size)) return false;
    }

    long file_end = ftell(out);
    if (file_end < 0) return false;

    if (!patch_u32_at(out, patches->riff_size_pos, (u32)(file_end - 8))) return false;
    if (!patch_u32_at(out, patches->avih_total_frames_pos, (u32)index->count)) return false;
    if (!patch_u32_at(out, patches->avih_suggested_buffer_pos, index->max_frame_size)) return false;
    if (!patch_u32_at(out, patches->strh_length_pos, (u32)index->count)) return false;
    if (!patch_u32_at(out, patches->strh_suggested_buffer_pos, index->max_frame_size)) return false;
    if (!patch_u32_at(out, patches->movi_size_pos, (u32)(idx1_start - (patches->movi_size_pos + 4)))) return false;

    return true;
}

static int count_selected_videos(void) {
    int count = 0;
    for (int i = 0; i < g_video_count; i++) {
        if (g_videos[i].selected) count++;
    }
    return count;
}

static bool should_export_video(int index, int selected_count) {
    if (selected_count > 0) return g_videos[index].selected;
    return index == g_selected_index;
}

static bool find_first_export_frame_info(int selected_count, int* out_width, int* out_height) {
    for (int i = 0; i < g_video_count; i++) {
        if (!should_export_video(i, selected_count)) continue;

        char path[512];
        if (!get_video_path(g_videos[i].filename, path, sizeof(path))) continue;

        FILE* in = fopen(path, "rb");
        if (!in) continue;

        long file_size = 0;
        if (!get_file_size(in, &file_size) || file_size <= 0) {
            fclose(in);
            continue;
        }

        long pos = 0;
        u8* jpg = NULL;
        size_t jpg_len = 0;
        bool found = read_next_camera_jpeg_frame(in, &pos, file_size, &jpg, &jpg_len);
        fclose(in);

        if (!found) continue;

        int comp = 0;
        int width = 0;
        int height = 0;
        bool ok = stbi_info_from_memory(jpg, (int)jpg_len, &width, &height, &comp) != 0;
        free(jpg);

        if (ok && width > 0 && height > 0) {
            *out_width = width;
            *out_height = height;
            return true;
        }
    }

    return false;
}

static int export_clip_frames(int index, int clip_number, int clip_total, FILE* out, const AviPatchOffsets* patches, AviIndex* avi_index) {
    char path[512];
    if (!get_video_path(g_videos[index].filename, path, sizeof(path))) return -1;

    FILE* in = fopen(path, "rb");
    if (!in) return -1;

    long file_size = 0;
    if (!get_file_size(in, &file_size) || file_size <= 0) {
        fclose(in);
        return -1;
    }

    long pos = 0;
    int frames = 0;

    char msg[256];
    snprintf(msg, sizeof(msg), "Exporting clip %d/%d...", clip_number, clip_total);
    copy_string(loading_screen.message, sizeof(loading_screen.message), msg);
    draw_loading_frame();

    while (true) {
        u8* jpg = NULL;
        size_t jpg_len = 0;
        if (!read_next_camera_jpeg_frame(in, &pos, file_size, &jpg, &jpg_len)) break;

        bool ok = avi_write_frame(out, patches, avi_index, jpg, jpg_len);
        free(jpg);

        if (!ok) {
            fclose(in);
            return -1;
        }

        frames++;

        if ((frames % 10) == 0) {
            snprintf(msg, sizeof(msg), "Exporting clip %d/%d... %d frames", clip_number, clip_total, frames);
            copy_string(loading_screen.message, sizeof(loading_screen.message), msg);
            draw_loading_frame();
        }
    }

    fclose(in);
    return frames;
}

static bool export_selected_videos(char* status, size_t status_size) {
    if (status_size > 0) status[0] = '\0';

    if (g_video_count == 0) {
        copy_string(status, status_size, "Export failed: no videos found");
        return false;
    }

    int selected_count = count_selected_videos();
    int clip_total = (selected_count > 0) ? selected_count : 1;
    int width = 0;
    int height = 0;

    show_loading_now("Preparing AVI export...");
    draw_loading_frame();

    if (!find_first_export_frame_info(selected_count, &width, &height)) {
        hide_loading_dialog();
        copy_string(status, status_size, "Export failed: no MJPEG frames");
        return false;
    }

    mkdir(APP_ROOT_PATH, 0777);

    FILE* out = fopen(EXPORT_VIDEO_PATH, "wb");
    if (!out) {
        hide_loading_dialog();
        copy_string(status, status_size, "Export failed: could not create file");
        return false;
    }

    AviPatchOffsets patches;
    AviIndex avi_index = {0};
    bool ok = write_avi_header(out, width, height, &patches);

    if (ok) {
        int clip_number = 0;
        for (int i = 0; i < g_video_count; i++) {
            if (!should_export_video(i, selected_count)) continue;

            clip_number++;
            int frames = export_clip_frames(i, clip_number, clip_total, out, &patches, &avi_index);
            if (frames < 0) {
                ok = false;
                break;
            }
        }
    }

    if (ok && avi_index.count > 0) {
        copy_string(loading_screen.message, sizeof(loading_screen.message), "Finalizing AVI export...");
        draw_loading_frame();
        ok = avi_finalize(out, &patches, &avi_index);
    } else {
        ok = false;
    }

    int exported_frames = avi_index.count;
    free(avi_index.entries);
    fclose(out);
    hide_loading_dialog();

    if (!ok) {
        remove(EXPORT_VIDEO_PATH);
        copy_string(status, status_size, "Export failed while writing AVI");
        return false;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Exported export.avi (%d frames)", exported_frames);
    copy_string(status, status_size, msg);
    return true;
}

// =====================================================
// Main
// =====================================================
int main(void) {
    gfxInitDefault();
    romfsInit();

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    setup_2d_no_depth();

    top_screen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom_screen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    init_spinner_sound();

    load_backgrounds();
    ensure_cache_folder();
    scan_videos();

    generate_all_thumbnails();

    if (g_video_count > 0) load_thumbnail_for_video(0);

    MjpegPlayer player;
    memset(&player, 0, sizeof(player));
    bool player_playing = false;
    u64 last_tick = osGetTime();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        touchPosition touch;

        if (kDown & KEY_START) break;

        // Touch input for the menu.
        if (kDown & KEY_TOUCH) {
            hidTouchRead(&touch);
        }

        if (g_state == STATE_MENU) {
            if (kDown & KEY_LEFT) g_menu_sel = 0;
            if (kDown & KEY_RIGHT) g_menu_sel = 1;

            // Touch on menu buttons.
            if (kDown & KEY_TOUCH) {
                if (is_point_in_rect(touch.px, touch.py, 20, 80, 130, 65)) {
                    g_menu_sel = 0;
                    g_state = STATE_GALLERY;
                } else if (is_point_in_rect(touch.px, touch.py, 170, 80, 130, 65)) {
                    break;
                }
            }

            if (kDown & KEY_A) {
                if (g_menu_sel == 0) g_state = STATE_GALLERY;
                else break;
            }
        }
        else if (g_state == STATE_GALLERY) {
            if (kDown & KEY_B) g_state = STATE_MENU;

            if (g_video_count > 0) {
                // Touch on thumbnails.
                if (kDown & KEY_TOUCH) {
                    for (int i=0;i<g_video_count;i++) {
                        int col = i % 4;
                        int row = i / 4;
                        int x = col * 80 + 10;
                        int y = row * 80 + 10 - g_scroll_offset;
                        
                        if (is_point_in_rect(touch.px, touch.py, x, y, 64, 64)) {
                            g_selected_index = i;
                            g_videos[i].selected = !g_videos[i].selected;
                            load_thumbnail_for_video(i);
                            break;
                        }
                    }
                }

                if (kDown & KEY_RIGHT) { g_selected_index++; if (g_selected_index >= g_video_count) g_selected_index = 0; load_thumbnail_for_video(g_selected_index); }
                if (kDown & KEY_LEFT)  { g_selected_index--; if (g_selected_index < 0) g_selected_index = g_video_count - 1; load_thumbnail_for_video(g_selected_index); }
                if (kDown & KEY_DOWN)  { g_selected_index += 4; if (g_selected_index >= g_video_count) g_selected_index = g_video_count - 1; load_thumbnail_for_video(g_selected_index); }
                if (kDown & KEY_UP)    { g_selected_index -= 4; if (g_selected_index < 0) g_selected_index = 0; load_thumbnail_for_video(g_selected_index); }

                if (kDown & KEY_A) {
                    g_videos[g_selected_index].selected = !g_videos[g_selected_index].selected;
                }

                if (kDown & KEY_Y) {
                    char path[512];
                    if (!get_video_path(g_videos[g_selected_index].filename, path, sizeof(path))) {
                        set_status_message("Preview failed: path is too long");
                        continue;
                    }

                    player_close(&player);
                    if (player_open(&player, path)) {
                        if (player_decode_next_frame(&player)) {
                            player_playing = true;
                            last_tick = osGetTime();
                            set_status_message("");
                            g_state = STATE_PLAYER;
                        } else {
                            player_close(&player);
                            set_status_message("Preview failed: no MJPEG frames");
                        }
                    } else {
                        set_status_message("Preview failed: could not open file");
                    }
                }

                if (kDown & KEY_X) {
                    char export_status[128];
                    set_status_message("Exporting AVI...");
                    export_selected_videos(export_status, sizeof(export_status));
                    set_status_message(export_status);
                }

                int row = g_selected_index / 4;
                int target = row * 80 - 40;
                if (target < 0) target = 0;
                if (g_scroll_offset < target) g_scroll_offset += 8;
                if (g_scroll_offset > target) g_scroll_offset -= 8;
            }
        }
        else if (g_state == STATE_PLAYER) {
            if (kDown & KEY_B) {
                player_playing = false;
                player_close(&player);
                g_state = STATE_GALLERY;
            }

            if (kDown & KEY_A) player_playing = !player_playing;
            if (kDown & KEY_RIGHT) player_decode_next_frame(&player);

            u64 now = osGetTime();
            if (player_playing && (now - last_tick) > PREVIEW_FRAME_MS) {
                last_tick = now;
                player_decode_next_frame(&player);
            }
        }

        update_fade();

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        if (g_state == STATE_MENU) {
            draw_menu_screen();
        } else if (g_state == STATE_GALLERY) {
            draw_gallery_screen();
        } else if (g_state == STATE_PLAYER) {
            C2D_SceneBegin(top_screen);
            draw_bg_top();

            C2D_TextBuf buf = C2D_TextBufNew(512);
            C2D_Text t;

            C2D_TextParse(&t, buf, "MJPEG Preview");
            C2D_TextOptimize(&t);
            C2D_DrawText(&t, C2D_WithColor, 20, 20, Z_UI, 0.6f, 0.6f, CLR_TEXT_BLACK);

            if (player.frame_loaded) {
                float x = (400 - player.frame_w) * 0.5f;
                float y = 50.0f;
                C2D_DrawImageAt(player.frame_img, x, y, Z_UI, NULL, 1.0f, 1.0f);
            } else {
                C2D_TextParse(&t, buf, "No preview frame decoded.");
                C2D_TextOptimize(&t);
                C2D_DrawText(&t, C2D_WithColor, 90, 110, Z_UI, 0.55f, 0.55f, CLR_TEXT_GRAY);
            }

            char info[128];
            snprintf(info, sizeof(info), "Frame: %d   [A]Play/Pause [->]Step [B]Back", player.frame_no);
            C2D_TextParse(&t, buf, info);
            C2D_TextOptimize(&t);
            C2D_DrawText(&t, C2D_WithColor, 20, 220, Z_UI, 0.45f, 0.45f, CLR_TEXT_GRAY);

            C2D_TextBufDelete(buf);

            C2D_SceneBegin(bottom_screen);
            draw_bg_bottom();
        }

        C3D_FrameEnd(0);
    }

    player_close(&player);

    for (int i=0;i<g_video_count;i++) {
        if (g_videos[i].thumbnail_loaded && g_videos[i].thumbnail.tex) {
            C3D_TexDelete(g_videos[i].thumbnail.tex);
            linearFree((void*)g_videos[i].thumbnail.tex);
            if (g_videos[i].thumbnail.subtex) linearFree((void*)g_videos[i].thumbnail.subtex);
        }
    }

    if (spriteSheet_top) C2D_SpriteSheetFree(spriteSheet_top);
    if (spriteSheet_bottom) C2D_SpriteSheetFree(spriteSheet_bottom);

    cleanup_spinner_sound();

    C2D_Fini();
    C3D_Fini();
    romfsExit();
    gfxExit();
    return 0;
}
