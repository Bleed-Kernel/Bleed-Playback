#pragma once

#include <stdint.h>
#include <stddef.h>
#include <bleed_uikit.h>
#include <bleed_mediakit.h>

#define PB_PATH_MAX     512
#define PB_NAME_MAX     256

// the lcd, red on black like an old car stereo
#define LCD_BACK        0x0B0B0D
#define LCD_LIT         0xF0303C
#define LCD_DIM         0x2A1013
#define LCD_TEXT        0xE8505A
#define LCD_FAINT       0xA8444E
#define LCD_BAR_LO      0x8E0D17
#define LCD_BAR_HI      0xFF6A70
#define LCD_PEAK        0xFFD6D8

#define COLOUR_FOLDER   0xE0B850

// widgets.c

enum {
    GLYPH_PLAY,
    GLYPH_PAUSE,
    GLYPH_STOP,
    GLYPH_PREV,
    GLYPH_NEXT,
    GLYPH_FOLDER_UP,
    GLYPH_SPEAKER,
};

void glyph_draw(ui_canvas_t *c, int glyph, int x, int y, uint32_t colour);
void glyph_button_draw(ui_canvas_t *c, const ui_button_t *b, int glyph);

typedef struct {
    ui_rect_t rect;
    int       value;
    int       max;
    int       dragging;
    int       grab;
} slider_t;

#define SLIDER_MOVED    (1 << 2)
#define SLIDER_DROPPED  (1 << 3)

void slider_draw(ui_canvas_t *c, const slider_t *s);
int  slider_mouse(slider_t *s, int x, int y, int left, int was_left);

void field_draw(ui_canvas_t *c, ui_rect_t r, uint32_t back);

// lcd.c

typedef struct {
    int         state;          // MK_*
    uint64_t    pos_ms;
    uint64_t    len_ms;
    int         remaining;      // count down rather than up
    char        title[PB_NAME_MAX];
    uint8_t     bands[MK_SPECTRUM];

    // the lcds own, it keeps them between frames
    int         bars[MK_SPECTRUM];
    int         peaks[MK_SPECTRUM];
    int         scroll;
} lcd_t;

int  lcd_tick(lcd_t *lcd);
void lcd_draw(ui_canvas_t *c, ui_rect_t r, lcd_t *lcd);

int  lcd_clock_hit(ui_rect_t r, int x, int y);

// library.c

#define CONFIG_DIR      "/initrd/player"
#define CONFIG_PATH     CONFIG_DIR "/config.ini"
#define LIST_NAME_MAX   64
#define RECENT_MAX      25

typedef struct {
    char   name[LIST_NAME_MAX];
    char (*tracks)[UI_PATH_MAX];
    int    count;
    int    cap;
    int    open;
} songlist_t;

typedef struct {
    songlist_t *lists;
    int         count;
    int         cap;
    int         volume;
    char        folder[UI_PATH_MAX];
} library_t;

#define RECENT  0

extern library_t library;

void library_load(void);
void library_save(void);

songlist_t *library_add_list(const char *name);
void        library_remove_list(int i);

// to the front of recently played, once
void library_played(const char *path);

int  songlist_add(songlist_t *s, const char *path);
void songlist_remove(songlist_t *s, int i);

// the name on the end of a path
const char *path_name(const char *path);

// sidebar.c

enum {
    ROW_LIST,
    ROW_TRACK,
};

typedef struct {
    int kind;                   // ROW_*
    int list;
    int track;
} row_t;

typedef struct {
    ui_rect_t      rect;
    char           playing[UI_PATH_MAX];

    row_t         *rows;
    int            count;
    int            cap;
    int            selected;
    ui_scrollbar_t scroll;
    uint8_t        was_left;
    int            last_click;
    uint64_t       last_click_ms;

    int            editing;
    char           edit[LIST_NAME_MAX];
    int            edit_fresh;
} sidebar_t;

#define SIDE_PLAY   (1 << 4)

// build the rows again after the library changed, the selection stays on the same thing if it can
void sidebar_rebuild(sidebar_t *s);
void sidebar_draw(ui_canvas_t *c, sidebar_t *s);
int  sidebar_mouse(sidebar_t *s, int x, int y, int left, uint64_t now_ms);
int  sidebar_wheel(sidebar_t *s, int notches);
int  sidebar_key(sidebar_t *s, uint16_t keycode, uint8_t keymod);
void sidebar_show(sidebar_t *s, int i);

// the playlist the selection is in or on, -1 for none or recently played
int  sidebar_list(const sidebar_t *s);

// start naming list i
void sidebar_rename(sidebar_t *s, int list);
