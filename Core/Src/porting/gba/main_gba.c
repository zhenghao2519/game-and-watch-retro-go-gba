#include <odroid_system.h>

#include <string.h>
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "appid.h"
#include "bilinear.h"
#include "filesystem.h"

/* gpsp. The core's own headers pull in libretro types and register-name macros
 * that collide with CMSIS, so we declare the handful of entry points we use. */
#include "gba_idle_loop.h"
#include "gba_audio_filter.h"

extern void odroid_system_get_sram_path(char *path, size_t size, int slot);
extern uint32_t  idle_loop_target_pc; /* gpSP's; gba_frontend.c owns the storage */
extern uint32_t  idle_loop_cond;      /* 0 = always burn; 1 = only while the branch loops */
extern uint16_t *gba_screen_pixels; /* the core renders straight into this, RGB565 */
extern uint32_t  execute_cycles;    /* cycles the core wants to run before the next event */
extern uint32_t  skip_next_frame;   /* set and the PPU evaluates but does not draw */
extern uint8_t   bios_rom[16 * 1024];
extern const uint8_t open_gba_bios_rom[];

/* Use accessor functions from gba_frontend.c to reach gamepak_backup.
 * Both gba_frontend.o and gba_memory.o go through --redefine-syms together,
 * so their symbol resolution is guaranteed consistent. main_gba.o also goes
 * through redefine, but using a function call eliminates any possible
 * linker-level mismatch with the BSS array. */
extern uint8_t *gba_get_backup_ptr(void);
extern unsigned int gba_get_backup_size(void);

void     init_main(void);
void     init_memory(void);
void     init_sound(void);
void     init_gamepak_buffer(void);
void     reset_gba(void);
void     execute_arm(uint32_t cycles);
void     gba_set_xip_rom(uint8_t *base, uint32_t size);
void     gba_set_keys(uint32_t keys);
uint32_t load_gamepak(const void *info, const char *name, int rtc, int rumble, int serial);
/* load_gamepak's last three arguments. Spelled out here rather than included: gpSP's
 * headers collide with CMSIS (see the note at the top of this file).
 *
 * The two scales are NOT the same, which is the trap. rtc and rumble are a tri-state
 * where 0 means DISABLE and -1 means "ask the cart" (gba_memory.h:25-27). serial is a
 * MODE, where 0 means disabled and "auto" is 6 (serial.h:20-26) — so a -1 there is not
 * "no opinion", it is a serial mode that does not exist. */
#define FEAT_AUTODETECT       (-1)
#define FEAT_DISABLE            0
#define FEAT_ENABLE             1
#define SERIAL_MODE_DISABLED    0
#define SERIAL_MODE_AUTO        6
uint32_t sound_read_samples(int16_t *out, uint32_t frames);
uint32_t sound_fifo_rate_hz(void);
#if CHEAT_CODES == 1
/* gpsp's cheat engine: GameShark / CodeBreaker / Action Replay, 20 slots. */
int  cheat_parse(unsigned index, const char *code);
void cheat_clear(void);
#define GBA_MAX_CHEAT_SLOTS 20
#endif

#define GBA_WIDTH   240
#define GBA_HEIGHT  160
/* The GBA's real frame: 280,896 cycles of a 16.777216 MHz clock. That is 59.7275 fps,
 * and every rate below is derived from it rather than from a rounded 60 — a 0.45%
 * error is a sample buffer lapping itself every nine seconds. */
#define GBA_FRAME_CYCLES  280896.0f
#define GBA_CPU_HZ        16777216.0f
#define GBA_FPS     60          /* only for anything that must name a whole number */
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

/* The mixer runs at the rate the SAI is already set to, so nothing has to be
 * resampled on a budget that has no room for it (gpsp's own default is 65536Hz).
 * The device has one speaker, so the core's stereo pair is folded to mono on the
 * way out — half the samples to touch, and nothing is lost that could be heard. */
#define GBA_SAMPLE_RATE          48000

/* Samples per frame — and NOT 48000/60.
 *
 * A GBA frame is 280,896 cycles of a 16.777216 MHz clock: 16.7427 ms, which is
 * 59.7275 fps, not 60. At 48 kHz that is 803.65 samples a frame, and gpSP produces
 * exactly that many. Asking for 800 left 3.65 of them behind every frame, and gpSP's
 * ring holds 2048 — so it filled and lapped itself about every nine seconds, after
 * which sound_read_samples() returned a nonsense count and the code below filled the
 * rest of the buffer with ZEROES. A waveform yanked to zero and back is a click. That
 * was the crackle, and it was never the speaker.
 *
 * 804 is a hair more than is produced, which is the safe side to err on: the ring
 * drains to its floor and stays there instead of lapping, and the shortfall is about
 * a third of a sample per frame — held, not zeroed, below. */
#define GBA_AUDIO_FRAMES         804
static int16_t gba_audio_stereo[GBA_AUDIO_FRAMES * 2];

/* The GBA framebuffer. The LCD's own buffers live outside RAM_EMU, but the core
 * renders a 240x160 image that then has to be scaled, so it needs a source.
 *
 * It comes from AHB SRAM, not from the overlay: 75 KB of a 724 KB pool that this
 * core has already very nearly spent, against 120 KB of AHB that nothing else is
 * using while a game runs. The scaler reads it once per frame and the DMA never
 * touches it, so the slower bus costs nothing that shows. */
#define GBA_FRAMEBUFFER_BYTES  (GBA_WIDTH * GBA_HEIGHT * sizeof(uint16_t))
static uint16_t *gba_framebuffer;

static odroid_video_frame_t video_frame = {GBA_WIDTH, GBA_HEIGHT, GBA_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

static void blit_emulator(void);

/* ------------------------------------------------------------------- diag ---
 * Where the frame actually goes.
 *
 * The overlay says FPS 40 and CPU 69% — enough to know we are waiting on the audio
 * tick, not enough to know WHAT is late. Emulating the ARM7 and drawing the screen
 * are the only two candidates and they want opposite fixes: the interpreter wants
 * clock and ITCM, the renderer wants to be out of flash. Guessing which cost two
 * builds already. So measure both, and put the answer in the pause menu.
 *
 * Averaged over a second so a single heavy frame does not read as a trend. */
typedef struct {
    uint32_t cycles;   /* accumulated since the last publish */
    uint32_t frames;
    uint32_t us;       /* published: microseconds per frame */
} gba_diag_t;

/* execute_arm() is TWO things. gpSP renders the picture from inside it — the PPU runs
 * per scanline, as the ARM7 executes — so "Emulate" is the interpreter AND the
 * renderer added together, and the two want opposite fixes.
 *
 * A skipped frame is what tells them apart: skip_next_frame makes the PPU evaluate the
 * scanline but not draw it. So time execute_arm() separately on frames that drew and
 * frames that did not, and the difference IS the renderer.
 *
 *   Emu+ppu   execute_arm() on a frame that rendered
 *   Emu only  execute_arm() on a frame that skipped rendering
 *   PPU       the difference — what drawing the picture actually costs
 */
static gba_diag_t diag_emu_draw;
static gba_diag_t diag_emu_skip;
static gba_diag_t diag_scale;
static gba_diag_t diag_overlay;
static gba_diag_t diag_wait;
static uint32_t   diag_last_tick;
static char       diag_emu_draw_str[16] = "-";
static char       diag_emu_skip_str[16] = "-";
static char       diag_ppu_str[16]      = "-";
static char       diag_scale_str[16]    = "-";
static char       diag_overlay_str[16]  = "-";
static char       diag_wait_str[16]     = "-";
/* Which of blit_emulator()'s branches actually ran. Scaling and filtering are user
 * settings, and SOFT does not go anywhere near the nearest-neighbour scaler — it
 * clears the whole 153 KB buffer and then runs a bilinear resample. */
static char       diag_path_str[16]    = "-";

static inline void gba_diag_add(gba_diag_t *d, uint32_t cycles)
{
    d->cycles += cycles;
    d->frames++;
}

static void gba_diag_format(gba_diag_t *d, char *out, size_t out_len)
{
    if (d->frames == 0) {
        snprintf(out, out_len, "-");
    } else {
        /* SystemCoreClock is whatever the overclock left us at, so this stays true
         * across the OC levels rather than assuming 280MHz. */
        uint32_t per_frame = d->cycles / d->frames;
        d->us = (uint32_t)((uint64_t)per_frame * 1000000u / SystemCoreClock);
        snprintf(out, out_len, "%lu.%02lu ms",
                 (unsigned long)(d->us / 1000), (unsigned long)((d->us % 1000) / 10));
    }
    d->cycles = 0;
    d->frames = 0;
}

static void gba_diag_publish(void)
{
    uint32_t now = HAL_GetTick();
    if (now - diag_last_tick < 1000)
        return;
    diag_last_tick = now;

    gba_diag_format(&diag_emu_draw, diag_emu_draw_str, sizeof(diag_emu_draw_str));
    gba_diag_format(&diag_emu_skip, diag_emu_skip_str, sizeof(diag_emu_skip_str));
    gba_diag_format(&diag_scale, diag_scale_str, sizeof(diag_scale_str));
    gba_diag_format(&diag_overlay, diag_overlay_str, sizeof(diag_overlay_str));
    gba_diag_format(&diag_wait, diag_wait_str, sizeof(diag_wait_str));

    /* What the picture costs: the same emulation, once with the PPU drawing and once
     * without. Only meaningful when frameskip is actually skipping something. */
    if (diag_emu_draw.us > diag_emu_skip.us && diag_emu_skip.us > 0) {
        uint32_t ppu = diag_emu_draw.us - diag_emu_skip.us;
        snprintf(diag_ppu_str, sizeof(diag_ppu_str), "%lu.%02lu ms",
                 (unsigned long)(ppu / 1000), (unsigned long)((ppu % 1000) / 10));
    } else {
        snprintf(diag_ppu_str, sizeof(diag_ppu_str), "no skips");
    }
}

/* ------------------------------------------------------------------ fatal --- */
/* Say which failure it was, and stay on screen while it is read. */
static void __attribute__((noreturn)) gba_fatal(const char *line_1, const char *line_2)
{
    printf("gba: FATAL %s / %s\n", line_1, line_2 ? line_2 : "");
    lcd_backlight_set(180);

    uint16_t *dest = lcd_get_active_buffer();
    memset(dest, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    odroid_overlay_draw_text_line(16, 100, LCD_WIDTH - 32, line_1, 0xFFFF, 0x0000);
    if (line_2)
        odroid_overlay_draw_text_line(16, 120, LCD_WIDTH - 32, line_2, 0xFFFF, 0x0000);
    lcd_swap();

    while (true) {
        wdog_refresh();
        HAL_Delay(20);
    }
}

/* -------------------------------------------------------------------- XIP ---
 * In the flash-only build the renderer, M4A HLE code and BIOS image are linked
 * directly into the ext flash overlay section at their final addresses by the
 * linker script. No runtime caching or relocation is needed — they are memory-
 * mapped from the moment the MCU boots.
 *
 * The sentinel mechanism is therefore a no-op here: the linker resolves all
 * references at link time. gba_cache_xip_to_flash() becomes unnecessary.
 */

/* The open-source BIOS is linked into the overlay via gba_bios.S.
 * On the flash-only build there is no filesystem to check for an
 * official BIOS, so we always use the bundled one. */
static void gba_load_bios(void)
{
    memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
    printf("gba: using bundled open BIOS\n");
}

/* ------------------------------------------------------------------- SRAM --- */
/* The cart's own save — the one the game writes when you save in-game.
 * On filesystem_wip, saves go through the fs_open/fs_write filesystem layer. */
static void gba_show_save_indicator(uint16_t color)
{
    uint16_t *dest = lcd_get_active_buffer();
    for (int i = 0; i < LCD_WIDTH * 2; i++)
        dest[i] = color;
    lcd_swap();
}

static void gba_SramSave(const char *sramPath)
{
    /* Check if gamepak_backup has any real data anywhere (not all 0xFF) */
    int has_data = 0;
    uint8_t *bp = gba_get_backup_ptr();
    unsigned int bsz = gba_get_backup_size();
    for (unsigned int i = 0; i < bsz; i += 64) {
        if (bp[i] != 0xFF) { has_data = 1; break; }
    }

    fs_file_t *file = fs_open(sramPath, FS_WRITE, FS_RAW);
    if (file) {
        fs_write(file, gba_get_backup_ptr(), gba_get_backup_size());
        fs_close(file);
        if (has_data)
            gba_show_save_indicator(0x07E0); /* green = save OK with data */
        else
            gba_show_save_indicator(0xF81F); /* magenta = save OK but backup is all 0xFF */
    } else {
        gba_show_save_indicator(0xF800); /* red = save FAILED */
    }
}

static void gba_SramLoad(const char *sramPath)
{
    fs_file_t *file = fs_open(sramPath, FS_READ, FS_RAW);
    if (file) {
        fs_read(file, gba_get_backup_ptr(), gba_get_backup_size());
        fs_close(file);
        /* Check if loaded data is non-empty (not all 0xFF) */
        int has_data = 0;
        for (int i = 0; i < 256; i++) {
            if (gba_get_backup_ptr()[i] != 0xFF) { has_data = 1; break; }
        }
        if (has_data)
            gba_show_save_indicator(0x001F); /* blue = load OK, has real data */
        else
            gba_show_save_indicator(0x07FF); /* cyan = load OK but data is all 0xFF */
    } else {
        gba_show_save_indicator(0xFFE0); /* yellow = no save file */
    }
}

/* -------------------------------------------------------------- savestate --- */
static bool gba_SaveState(char *savePathName, char *sramPathName, int slot)
{
    (void)savePathName;
    (void)slot;
    gba_SramSave(sramPathName);
    return true;
}

static bool gba_LoadState(char *savePathName, char *sramPathName, int slot)
{
    (void)savePathName;
    (void)slot;
    gba_SramLoad(sramPathName);
    return true;
}

/* ------------------------------------------------------------------ audio --- */
static void gba_pcm_submit(void)
{
    uint32_t got = sound_read_samples(gba_audio_stereo, GBA_AUDIO_FRAMES);

    if (common_emu_sound_loop_is_muted())
        return;

    int32_t   factor = common_emu_sound_get_volume();
    int16_t  *out    = audio_get_active_buffer();
    uint16_t  len    = audio_get_buffer_length();

    if (len > GBA_AUDIO_FRAMES)
        len = GBA_AUDIO_FRAMES;

    /* Hold the last sample when the core comes up short; do not slam to zero.
     *
     * A shortfall is normal here — we ask for a hair more than a frame produces, on
     * purpose (see GBA_AUDIO_FRAMES) — so this runs a fraction of a sample per frame.
     * Repeating a sample for 20 microseconds is inaudible. Dropping the waveform to
     * zero and back is a click, and doing it every frame is a crackle. */
    static int16_t last_mono = 0;

    for (uint16_t i = 0; i < len; i++) {
        /* One speaker: fold the pair rather than throw a channel away. Anything
         * panned hard to the side would otherwise vanish. */
        if (i < got) {
            last_mono = (int16_t)(((int32_t)gba_audio_stereo[i * 2] +
                                   gba_audio_stereo[i * 2 + 1]) / 2);
        }
        out[i] = last_mono;
    }

    /* The analog rolloff the real console has and our clean DAC does not —
     * cutoff follows the rate this cart is clocking its FIFOs at, so the
     * resampling images go and the music stays. See gba_audio_filter.h. */
    gba_lpf_configure(sound_fifo_rate_hz());
    gba_lpf_apply(out, len);

    for (uint16_t i = 0; i < len; i++)
        out[i] = (int16_t)(((int32_t)out[i] * factor) >> 8);

}

/* ------------------------------------------------------------------ video --- */
/* The nearest-neighbour source column for every destination column. It is the same
 * for all 213 rows, so it is computed once per scaling mode instead of doing a
 * multiply and a shift for each of ~68,000 pixels, every frame. */
static uint16_t nn_xmap[LCD_WIDTH];
static int32_t  nn_xmap_width = -1;

__attribute__((optimize("unroll-loops")))
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    int w1 = video_frame.width;
    int h1 = video_frame.height;
    int w2 = dest_width;
    int h2 = dest_height;

    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (LCD_WIDTH - dest_width) / 2;
    int wpad = (LCD_HEIGHT - dest_height) / 2;

    if (nn_xmap_width != dest_width) {
        int x_ratio = (int)((w1 << 16) / w2) + 1;
        for (int j = 0; j < w2; j++)
            nn_xmap[j] = (uint16_t)((j * x_ratio) >> 16);
        nn_xmap_width = dest_width;
    }

    uint16_t *screen_buf = (uint16_t *)video_frame.buffer;
    uint16_t *dest = lcd_get_active_buffer();

    /* Write every pixel of the buffer, borders included. A separate clear is a
     * ~150KB memset that can overtake the beam and desync the vblank swap. */
    for (int i = 0; i < wpad; i++)
        memset(dest + i * LCD_WIDTH, 0, LCD_WIDTH * sizeof(uint16_t));
    for (int i = wpad + h2; i < LCD_HEIGHT; i++)
        memset(dest + i * LCD_WIDTH, 0, LCD_WIDTH * sizeof(uint16_t));

    /* Two pixels per store.
     *
     * The LCD pool is uncached AND unbuffered (see ._ram_uc in the linker script), so
     * every write to it is a bus round trip the CPU stalls on until it completes. FIT
     * writes 320x213 pixels a frame; one 16-bit store each is 68,160 round trips, and
     * that — not the scaling arithmetic — was 9.1 ms of a 16.67 ms frame. Packing two
     * pixels into one 32-bit store halves the trips.
     *
     * 32-bit, not 64: a Cortex-M7 traps an unaligned STRD, and Super Metroid already
     * died once on exactly that (see the root CLAUDE.md). Rows are 640 bytes, so a row
     * start is always 4-byte aligned, and hpad is even for every scaling mode here —
     * but a 64-bit store would need 8, which is not guaranteed. */
    for (int i = 0; i < h2; i++) {
        uint16_t *row = dest + (i + wpad) * LCD_WIDTH;
        int y2 = ((i * y_ratio) >> 16);
        const uint16_t *src_row = screen_buf + (y2 * w1);

        for (int j = 0; j < hpad; j++)
            row[j] = 0;

        uint32_t *out32 = (uint32_t *)(row + hpad);
        int pairs = w2 >> 1;
        for (int j = 0; j < pairs; j++) {
            uint32_t lo = src_row[nn_xmap[j * 2]];
            uint32_t hi = src_row[nn_xmap[j * 2 + 1]];
            out32[j] = lo | (hi << 16);
        }
        if (w2 & 1)
            row[hpad + w2 - 1] = src_row[nn_xmap[w2 - 1]];

        for (int j = hpad + w2; j < LCD_WIDTH; j++)
            row[j] = 0;
    }
}

static void screen_blit_bilinear(int32_t dest_width)
{
    int hpad = (LCD_WIDTH - dest_width) / 2;
    uint16_t *dest = lcd_get_active_buffer();

    image_t dst_img = {dest_width, LCD_HEIGHT, 2, ((uint8_t *)dest) + hpad * 2};
    image_t src_img = {video_frame.width, video_frame.height, 2, video_frame.buffer};

    if (hpad > 0)
        memset(dest, 0x00, hpad * 2);

    imlib_draw_image(&dst_img, &src_img, 0, 0, LCD_WIDTH,
                     ((float)dest_width) / ((float)video_frame.width),
                     ((float)LCD_HEIGHT) / ((float)video_frame.height),
                     NULL, -1, 255, NULL, NULL, IMAGE_HINT_BILINEAR, NULL, NULL);
}

static void blit_emulator(void)
{
    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    /* Which branch ran. SOFT does not touch the nearest-neighbour scaler at all: it
     * clears all 153 KB of the buffer and then bilinear-resamples into it. */
    snprintf(diag_path_str, sizeof(diag_path_str), "%s/%s",
             scaling == ODROID_DISPLAY_SCALING_OFF  ? "off"
           : scaling == ODROID_DISPLAY_SCALING_FIT  ? "fit"
           : scaling == ODROID_DISPLAY_SCALING_FULL ? "full" : "cust",
             filtering == ODROID_DISPLAY_FILTER_SOFT ? "soft" : "hard");

    static odroid_display_scaling_t last_scaling = -1;
    if (scaling != last_scaling) {
        lcd_clear_buffers();
        last_scaling = scaling;
    }

    /* 240x160 is 3:2. Filling the 240px height would need 360px of width, which
     * the 320px panel does not have — so FIT fills the width instead and letter-
     * boxes: 320x213. */
    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        screen_blit_nn(GBA_WIDTH, GBA_HEIGHT);   /* native, centred */
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT) {
            lcd_clear_active_buffer();           /* bilinear does not fill the borders */
            screen_blit_bilinear(LCD_WIDTH);
        } else {
            screen_blit_nn(LCD_WIDTH, 213);
        }
        break;
    case ODROID_DISPLAY_SCALING_FULL:
    case ODROID_DISPLAY_SCALING_CUSTOM:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT)
            screen_blit_bilinear(LCD_WIDTH);
        else
            screen_blit_nn(LCD_WIDTH, LCD_HEIGHT);
        break;
    default:
        screen_blit_nn(LCD_WIDTH, 213);
        break;
    }
}

static void blit(void)
{
    /* The wait for the LCD's previous swap to finish. Idle time, not work — and the
     * one number here that going FASTER makes bigger, because the slack has to land
     * somewhere. Timed on its own so it stops being mistaken for the renderer. */
    common_emu_clear_dwt_cycles();
    lcd_sleep_while_swap_pending();
    gba_diag_add(&diag_wait, common_emu_get_dwt_cycles());

    common_emu_clear_dwt_cycles();
    blit_emulator();
    gba_diag_add(&diag_scale, common_emu_get_dwt_cycles());

    common_emu_clear_dwt_cycles();
    common_ingame_overlay();
    gba_diag_add(&diag_overlay, common_emu_get_dwt_cycles());
}

/* ------------------------------------------------------------------ input --- */
/* KEYINPUT bit order, and what gpsp's gba_set_keys() expects: a set bit is held. */
#define GBA_KEY_A      0x0001
#define GBA_KEY_B      0x0002
#define GBA_KEY_SELECT 0x0004
#define GBA_KEY_START  0x0008
#define GBA_KEY_RIGHT  0x0010
#define GBA_KEY_LEFT   0x0020
#define GBA_KEY_UP     0x0040
#define GBA_KEY_DOWN   0x0080
#define GBA_KEY_R      0x0100
#define GBA_KEY_L      0x0200

static void gba_input_read(odroid_gamepad_state_t *joystick)
{
    uint32_t keys = 0;
    if (joystick->values[ODROID_INPUT_UP])     keys |= GBA_KEY_UP;
    if (joystick->values[ODROID_INPUT_DOWN])   keys |= GBA_KEY_DOWN;
    if (joystick->values[ODROID_INPUT_LEFT])   keys |= GBA_KEY_LEFT;
    if (joystick->values[ODROID_INPUT_RIGHT])  keys |= GBA_KEY_RIGHT;
    if (joystick->values[ODROID_INPUT_A])      keys |= GBA_KEY_A;
    if (joystick->values[ODROID_INPUT_B])      keys |= GBA_KEY_B;
    if (joystick->values[ODROID_INPUT_START])  keys |= GBA_KEY_START;
    if (joystick->values[ODROID_INPUT_SELECT]) keys |= GBA_KEY_SELECT;
    /* The unit has no shoulder buttons. Y/X stand in for L/R — every GBA game
     * that uses them uses them, and there is nowhere else to put them. */
    if (joystick->values[ODROID_INPUT_X])      keys |= GBA_KEY_R;
    if (joystick->values[ODROID_INPUT_Y])      keys |= GBA_KEY_L;

    gba_set_keys(keys);
}

/* ------------------------------------------------------------------- main --- */
void app_main_gba(uint8_t load_state, uint8_t start_paused, uint8_t save_slot)
{
    odroid_gamepad_state_t joystick;
    /* Read-only (enabled = -1): the frame budget is 16.67ms, and these two say who
     * is spending it. If Emulate dominates, the answer is clock and the interpreter.
     * If Draw does, the answer is the renderer and where its code lives. */
    odroid_dialog_choice_t options[] = {
        {0, "Emu+ppu",  diag_emu_draw_str, -1, NULL},
        {0, "Emu only", diag_emu_skip_str, -1, NULL},
        {0, " = PPU",   diag_ppu_str,      -1, NULL},
        {0, "Scale",    diag_scale_str,    -1, NULL},
        {0, "Ovl",      diag_overlay_str,  -1, NULL},
        {0, "LCD wait",  diag_wait_str,   -1, NULL},
        {0, "Path",     diag_path_str,     -1, NULL},
        ODROID_DIALOG_CHOICE_LAST
    };

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    /* 1674, not 1667. A GBA frame is 16.7427 ms — 59.7275 fps, not 60 — and the pacing
     * loop's idea of a frame has to be the same one the audio DMA enforces, or the two
     * fight and the loser is a dropped frame. The LCD is told 60 because its refresh
     * rate is a hardware setting with no 59.7 to choose. */
    common_emu_state.frame_time_10us = (uint16_t)(100000.0f * GBA_FRAME_CYCLES / GBA_CPU_HZ + 0.5f);
    lcd_set_refresh_rate(60);

    gba_framebuffer = ahb_malloc(GBA_FRAMEBUFFER_BYTES);
    if (gba_framebuffer == NULL)
        gba_fatal("Out of AHB SRAM", "The 75KB framebuffer could not be allocated");
    memset(gba_framebuffer, 0, GBA_FRAMEBUFFER_BYTES);

    video_frame.buffer = gba_framebuffer;
    gba_screen_pixels = gba_framebuffer;

    odroid_system_init(APPID_GBA, GBA_SAMPLE_RATE);
    odroid_system_emu_init(&gba_LoadState, &gba_SaveState, NULL);

    /* Native 240x160 is a small island on a 320x240 panel; FIT is the sane
     * first-run default. Any choice the user makes afterwards is theirs. */
    if (odroid_display_get_scaling_mode() == ODROID_DISPLAY_SCALING_OFF)
        odroid_display_set_scaling_mode(ODROID_DISPLAY_SCALING_FIT);

    audio_start_playing(GBA_AUDIO_FRAMES);
    gba_lpf_reset();

    /* In the flash-only build, the renderer and BIOS are already linked at their
     * final addresses in ext flash. No XIP caching or sentinel patching needed. */

    init_main();
    init_memory();
    init_sound();

    gba_load_bios();
    memset(gba_get_backup_ptr(), 0xFF, gba_get_backup_size());

    /* The ROM lives in external flash, baked in at compile time by parse_roms.py.
     * It is memory-mapped via QSPI — no decompression, no copy, no SD needed.
     * ACTIVE_FILE->address points directly into the EXTFLASH address space. */
    uint32_t rom_size = ACTIVE_FILE->size;
    uint8_t *rom = (uint8_t *)ACTIVE_FILE->address;
    if (rom == NULL || rom_size == 0)
        gba_fatal("No GBA ROM found", "Check that a .gba ROM was included in the build");
    gba_set_xip_rom(rom, rom_size);
    init_gamepak_buffer();

    /* force_rtc, force_rumble, force_serial — and only the first one changes.
     *
     * RTC: -1, not 0. Zero is not "no opinion" here, it is FEAT_DISABLE — an override
     * that says turn the clock OFF. gpSP recognised Ruby, set rtc_enabled from the
     * cart, and then the 0 we passed switched it straight back off. The game reported
     * that its internal battery had run dry, which for a cart with no working clock is
     * exactly true. Ruby, Sapphire and Emerald all keep time: berries grow, tides turn.
     * Let the cart decide.
     *
     * Rumble: 0 stays. There is no motor in a Game & Watch, so emulating the pak is
     * work with nowhere to land.
     *
     * Serial: 0 stays, and note it is NOT the same tri-state — it is a serial MODE
     * (serial.h:20-26), where 0 is SERIAL_MODE_DISABLED and "auto" is 6. There is no
     * link port either, and leaving it disabled also keeps gba_over.h from turning on
     * Pokemon's serial emulation, which would be per-frame work for a cable that does
     * not exist. */
    if (load_gamepak(NULL, ACTIVE_FILE->name,
                     FEAT_AUTODETECT,        /* rtc: ask the cart          */
                     FEAT_DISABLE,           /* rumble: no motor           */
                     SERIAL_MODE_DISABLED) != 0)   /* serial: no link port */
        gba_fatal("Not a Game Boy Advance ROM", "The header did not check out");

    /* gba_over.h sets flash_bank_cnt=128KB for games like Pokemon, but
     * detect_backup_subcircuit() may still set backup_type_reset=EEPROM if it
     * finds an EEPROM_V string in the ROM. Force consistency: if the override
     * table requested 128KB flash, honour that over the signature scan. */
    /* Force Flash 128KB unconditionally for now — will scope this to
     * specific games once save is confirmed working. */
    extern void gba_force_flash128_backup(void);
    gba_force_flash128_backup();

    /* After load_gamepak, on purpose: it is what sets idle_loop_target_pc from
     * gpSP's own gba_over.h, and ours has to win. A game with no busy-wait PC
     * spins through the whole 280,896-cycle frame instead of doing ~75,000 cycles
     * of work and stopping — so this is not a tuning knob, it is the difference
     * between full speed and no chance of it. See gba_idle_loop.c. */
    uint32_t idle_pc = gba_idle_loop_lookup((const char *)&rom[0xAC]);
    if (idle_pc != 0) {
        idle_loop_target_pc = idle_pc;
        printf("gba: idle loop at 0x%08lX\n", (unsigned long)idle_pc);
    }

    /* The OTHER kind of wait: a raster poll — `ldrh rN,[VCOUNT]; cmp; bne` —
     * that the classic always-burn skip must not touch, because these games'
     * delay code CALLS the poll in a counted burst and on hardware ~120 calls
     * fit inside the matching scanline; burn every arrival and a six-frame
     * intro becomes seven hundred (proven: Super Robot Taisen D froze). So the
     * target is the poll's closing branch and the slice burns only while the
     * branch will loop (IDLE_COND_WHEN_NE; the check costs nothing off-match).
     *
     * Hand-curated, one entry per game PROVEN on the host A/B rig
     * (tools/gba_m4a/prove_main.c, IDLE_PC= + IDLE_COND=ne): screens 99.8%
     * identical at a two-frame shift, interpreted instructions -15..-17%.
     * Only for carts with NO entry in the generated idle table — the two
     * waits would otherwise fight over one target slot. */
    static const struct { char code[5]; uint32_t branch_pc; } vcount_polls[] = {
        { "A6SJ", 0x8932178 },   /* Super Robot Taisen D  (-15.2%) */
        { "ATIJ", 0x858f088 },   /* Tennis no Ouji-sama Genius Boys Academy (-16.8%) */
    };
    if (idle_pc == 0) {
        for (size_t i = 0; i < sizeof(vcount_polls) / sizeof(vcount_polls[0]); i++) {
            if (memcmp(vcount_polls[i].code, &rom[0xAC], 4) == 0) {
                idle_loop_target_pc = vcount_polls[i].branch_pc;
                idle_loop_cond = 1;   /* IDLE_COND_WHEN_NE */
                printf("gba: vcount poll at 0x%08lX (cond NE)\n",
                       (unsigned long)idle_loop_target_pc);
                break;
            }
        }
    }

    reset_gba();

#if CHEAT_CODES == 1
    cheat_clear();
    unsigned slot = 0;
    for (int i = 0; i < ACTIVE_FILE->cheat_count && slot < GBA_MAX_CHEAT_SLOTS; i++) {
        if (odroid_settings_ActiveGameGenieCodes_is_enabled(ACTIVE_FILE->id, i))
            cheat_parse(slot++, ACTIVE_FILE->cheat_codes[i]);
    }
#endif

    /* Load SRAM save AFTER all core initialization is complete.
     * Always attempt to load — either via the system load_state path
     * (resume from sleep) or our direct path (fresh game start).
     * This must be the very last thing before the frame loop so that
     * nothing else can reset gamepak_backup or flash controller state. */
    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        char sramPath[FS_MAX_PATH_SIZE];
        odroid_system_get_sram_path(sramPath, sizeof(sramPath), 0);
        gba_SramLoad(sramPath);
        lcd_clear_buffers();
    }

    common_emu_enable_dwt_cycles();

    while (true) {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();
        /* Always render — gpSP must produce every frame for correct timing.
         * Display update is gated by whether the LCD is ready (non-blocking). */
        skip_next_frame = 0;

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        gba_input_read(&joystick);

        common_emu_clear_dwt_cycles();
        execute_arm(execute_cycles);
        gba_diag_add(drawFrame ? &diag_emu_draw : &diag_emu_skip,
                     common_emu_get_dwt_cycles());

        /* Blit only when LCD has finished the previous swap — if still
         * pending, skip this display update (frame drop) but keep emulating.
         * This avoids blocking on lcd_sleep_while_swap_pending() inside blit()
         * and gives the CPU back to the emulator sooner. */
        if (!lcd_is_swap_pending()) {
            blit();
            lcd_swap();
        }
        gba_diag_publish();

        gba_pcm_submit();

        common_emu_sound_sync(false);
    }
}
