#include <stdio.h>
#include <string.h>
#include <playback.h>

#define PAD         8
#define DIGIT_W     14
#define DIGIT_H     26
#define SEG         3
#define DIGIT_GAP   4
#define COLON_W     6
#define CLOCK_X     26

#define BAR_GAP     2
#define BARS_H      40
#define BAR_FALL    20 
#define PEAK_FALL   5

#define TITLE_Y     44
#define MARQUEE_GAP "     ***     "

enum { SA = 1, SB = 2, SC = 4, SD = 8, SE = 16, SF = 32, SG = 64 };

static const uint8_t digit_segs[10] = {
    SA | SB | SC | SD | SE | SF,
    SB | SC,
    SA | SB | SG | SE | SD,
    SA | SB | SG | SC | SD,
    SF | SG | SB | SC,
    SA | SF | SG | SC | SD,
    SA | SF | SG | SE | SD | SC,
    SA | SB | SC,
    SA | SB | SC | SD | SE | SF | SG,
    SA | SB | SC | SD | SF | SG,
};

static void segments(ui_canvas_t *c, int x, int y, uint8_t lit) {
    int half = DIGIT_H / 2;
    ui_rect_t seg[7] = {
        { x + SEG, y, DIGIT_W - SEG * 2, SEG },
        { x + DIGIT_W - SEG, y + SEG, SEG, half - SEG - 1 },
        { x + DIGIT_W - SEG, y + half + 1, SEG, half - SEG - 1 },
        { x + SEG, y + DIGIT_H - SEG, DIGIT_W - SEG * 2, SEG },
        { x, y + half + 1, SEG, half - SEG - 1 },
        { x, y + SEG, SEG, half - SEG - 1 },
        { x + SEG, y + half - SEG / 2, DIGIT_W - SEG * 2, SEG },
    };
    for (int i = 0; i < 7; i++)
        ui_fill(c, seg[i], (lit >> i) & 1 ? LCD_LIT : LCD_DIM);
}

static int clock_w(void) {
    return DIGIT_W * 5 + DIGIT_GAP * 4 + COLON_W;
}

static void clock_draw(ui_canvas_t *c, int x, int y, uint64_t ms, int minus) {
    uint64_t s   = ms / 1000;
    uint64_t min = s / 60;
    if (min > 99)
        min = 99;

    // a lone minus sign before the first digit when it counts down
    ui_fill(c, (ui_rect_t){x - 10, y + DIGIT_H / 2 - SEG / 2, 7, SEG}, minus ? LCD_LIT : LCD_BACK);

    segments(c, x, y, digit_segs[min / 10]);
    x += DIGIT_W + DIGIT_GAP;
    segments(c, x, y, digit_segs[min % 10]);
    x += DIGIT_W + DIGIT_GAP;

    ui_fill(c, (ui_rect_t){x + 1, y + 7, SEG, SEG}, LCD_LIT);
    ui_fill(c, (ui_rect_t){x + 1, y + DIGIT_H - 10, SEG, SEG}, LCD_LIT);
    x += COLON_W;

    segments(c, x, y, digit_segs[(s % 60) / 10]);
    x += DIGIT_W + DIGIT_GAP;
    segments(c, x, y, digit_segs[s % 10]);
}

// half of whats already there and half colour, drawn last so the text shows through
static void fill_half(ui_canvas_t *c, ui_rect_t r, uint32_t colour) {
    r = ui_rect_clip(r, c->clip);

    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = c->pixels + (size_t)y * c->pitch + r.x;
        for (int x = 0; x < r.w; x++)
            row[x] = ((row[x] >> 1) & 0x7F7F7F) + ((colour >> 1) & 0x7F7F7F);
    }
}

// along the bottom of the lcd, the bars split the width between them
static void bars_draw(ui_canvas_t *c, ui_rect_t area, const lcd_t *lcd) {
    int bar_w = (area.w - BAR_GAP * (MK_SPECTRUM - 1)) / MK_SPECTRUM;
    if (bar_w < 1)
        return;

    int x      = area.x + (area.w - (bar_w + BAR_GAP) * MK_SPECTRUM + BAR_GAP) / 2;
    int bottom = area.y + area.h - 1;

    for (int i = 0; i < MK_SPECTRUM; i++) {
        int bx = x + i * (bar_w + BAR_GAP);
        int h  = lcd->bars[i] * area.h / 255;
        int ph = lcd->peaks[i] * area.h / 255;

        for (int cy = 0; cy < h; cy += 2) {
            uint32_t colour = cy > area.h * 2 / 3 ? LCD_BAR_HI : cy > area.h / 3 ? LCD_LIT : LCD_BAR_LO;
            fill_half(c, (ui_rect_t){bx, bottom - cy, bar_w, 1}, colour);
        }
        if (ph > 1)
            fill_half(c, (ui_rect_t){bx, bottom - ph, bar_w, 1}, LCD_PEAK);
    }
}

static void title_draw(ui_canvas_t *c, ui_rect_t area, const lcd_t *lcd) {
    const ui_font_t *f  = &ui_font_text;
    int              tw = ui_text_width(f, lcd->title);
    ui_rect_t        old = c->clip;

    ui_clip(c, ui_rect_clip(area, old));
    if (tw <= area.w) {
        ui_text(c, f, area.x, area.y, lcd->title, LCD_TEXT);
    } else {
        // round and round, with a gap so the end doesnt run into the start
        int loop = tw + ui_text_width(f, MARQUEE_GAP);
        int x    = area.x - lcd->scroll % loop;
        ui_text(c, f, x, area.y, lcd->title, LCD_TEXT);
        ui_text(c, f, x + tw, area.y, MARQUEE_GAP, LCD_FAINT);
        ui_text(c, f, x + loop, area.y, lcd->title, LCD_TEXT);
    }
    c->clip = old;
}

void lcd_draw(ui_canvas_t *c, ui_rect_t r, lcd_t *lcd) {
    field_draw(c, r, LCD_BACK);

    ui_rect_t old = c->clip;
    ui_clip(c, ui_rect_clip(ui_rect_shrink(r, 2), old));

    int x = r.x + PAD, y = r.y + PAD;
    int glyph = lcd->state == MK_PLAYING ? GLYPH_PLAY : lcd->state == MK_PAUSED ? GLYPH_PAUSE : GLYPH_STOP;
    glyph_draw(c, glyph, x + 4, y + DIGIT_H / 2, LCD_LIT);

    uint64_t shown = lcd->pos_ms;
    if (lcd->remaining)
        shown = lcd->len_ms > lcd->pos_ms ? lcd->len_ms - lcd->pos_ms : 0;
    clock_draw(c, r.x + CLOCK_X + DIGIT_GAP + 10, y, shown, lcd->remaining);

    title_draw(c, (ui_rect_t){x, r.y + TITLE_Y, r.w - PAD * 2, ui_font_height(&ui_font_text)}, lcd);

    bars_draw(c, (ui_rect_t){r.x + 4, r.y + r.h - 3 - BARS_H, r.w - 8, BARS_H}, lcd);

    c->clip = old;
}

int lcd_clock_hit(ui_rect_t r, int x, int y) {
    ui_rect_t clock = { r.x + CLOCK_X, r.y + PAD, clock_w() + 14, DIGIT_H };
    return ui_rect_contains(clock, x, y);
}

int lcd_tick(lcd_t *lcd) {
    int moving = 0;

    for (int i = 0; i < MK_SPECTRUM; i++) {
        int v = lcd->bars[i] - BAR_FALL;
        if (lcd->bands[i] > v)
            v = lcd->bands[i];
        lcd->bars[i] = v < 0 ? 0 : v;

        if (lcd->bars[i] >= lcd->peaks[i])
            lcd->peaks[i] = lcd->bars[i];
        else if ((lcd->peaks[i] -= PEAK_FALL) < 0)
            lcd->peaks[i] = 0;
        if (lcd->bars[i] || lcd->peaks[i])
            moving = 1;
    }

    if (lcd->state == MK_PLAYING) {
        lcd->scroll++;
        moving = 1;
    }
    return moving;
}
