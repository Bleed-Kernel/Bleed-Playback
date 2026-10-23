#include <string.h>
#include <playback.h>

#define THUMB_W     11
#define THUMB_H_MAX 21
#define GROOVE_H    4

static void row(ui_canvas_t *c, int x, int y, int w, uint32_t colour) {
    ui_fill(c, (ui_rect_t){x, y, w, 1}, colour);
}

static void box(ui_canvas_t *c, int x, int y, int w, int h, uint32_t colour) {
    ui_fill(c, (ui_rect_t){x, y, w, h}, colour);
}

static void triangle(ui_canvas_t *c, int x, int y, int half, int dir, uint32_t colour) {
    for (int i = -half; i <= half; i++) {
        int w = half - (i < 0 ? -i : i) + 1;
        row(c, dir > 0 ? x : x - w + 1, y + i, w, colour);
    }
}

static void triangle_v(ui_canvas_t *c, int x, int y, int half, int dir, uint32_t colour) {
    for (int i = 0; i <= half; i++)
        row(c, x - i, dir < 0 ? y + i : y - i, i * 2 + 1, colour);
}

static void folder(ui_canvas_t *c, int x, int y) {
    box(c, x, y, 6, 2, COLOUR_FOLDER);
    box(c, x, y + 2, 14, 9, COLOUR_FOLDER);
    row(c, x + 1, y + 3, 12, 0xF8E0A0);
    row(c, x, y + 10, 14, 0x9A7A28);
}

void glyph_draw(ui_canvas_t *c, int glyph, int x, int y, uint32_t colour) {
    switch (glyph) {
    case GLYPH_PLAY:
        triangle(c, x - 3, y, 5, 1, colour);
        break;
    case GLYPH_PAUSE:
        box(c, x - 4, y - 5, 3, 11, colour);
        box(c, x + 1, y - 5, 3, 11, colour);
        break;
    case GLYPH_STOP:
        box(c, x - 4, y - 4, 9, 9, colour);
        break;
    case GLYPH_PREV:
        box(c, x - 6, y - 4, 2, 9, colour);
        triangle(c, x, y, 4, -1, colour);
        triangle(c, x + 5, y, 4, -1, colour);
        break;
    case GLYPH_NEXT:
        triangle(c, x - 5, y, 4, 1, colour);
        triangle(c, x, y, 4, 1, colour);
        box(c, x + 5, y - 4, 2, 9, colour);
        break;
    case GLYPH_FOLDER_UP:
        folder(c, x - 7, y - 5);
        triangle_v(c, x, y - 2, 3, -1, UI_COLOUR_FACE);
        box(c, x - 1, y + 2, 3, 3, UI_COLOUR_FACE);
        break;
    case GLYPH_SPEAKER:
        box(c, x - 6, y - 2, 3, 5, colour);
        for (int i = 0; i < 4; i++)
            box(c, x - 3 + i, y - 2 - i, 1, 5 + i * 2, colour);
        box(c, x + 3, y - 1, 1, 3, colour);
        box(c, x + 5, y - 3, 1, 7, colour);
        break;
    }
}

void glyph_button_draw(ui_canvas_t *c, const ui_button_t *b, int glyph) {
    ui_button_t blank = *b;
    blank.label = "";
    ui_button_draw(c, &blank);

    int down = b->held && b->hover;
    glyph_draw(c, glyph, b->rect.x + b->rect.w / 2 + down, b->rect.y + b->rect.h / 2 + down, UI_COLOUR_BUTTON_X);
}

void field_draw(ui_canvas_t *c, ui_rect_t r, uint32_t back) {
    ui_fill(c, r, back);
    ui_edge(c, r, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);
    ui_edge(c, ui_rect_shrink(r, 1), UI_COLOUR_DKSHADOW, UI_COLOUR_LIGHT);
}

// slider

static int thumb_h(const slider_t *s) {
    return s->rect.h < THUMB_H_MAX ? s->rect.h : THUMB_H_MAX;
}

static int thumb_x(const slider_t *s) {
    if (s->max <= 0)
        return s->rect.x;
    return s->rect.x + (int)((int64_t)(s->rect.w - THUMB_W) * s->value / s->max);
}

void slider_draw(ui_canvas_t *c, const slider_t *s) {
    ui_rect_t r  = s->rect;
    int       tx = thumb_x(s);
    int       th = thumb_h(s);

    ui_fill(c, r, UI_COLOUR_FACE);

    // the groove runs a thumbs half width in from each end
    ui_rect_t groove = { r.x + THUMB_W / 2, r.y + (r.h - GROOVE_H) / 2, r.w - THUMB_W + 1, GROOVE_H };
    ui_fill(c, groove, UI_COLOUR_DKSHADOW);
    if (s->max > 0 && tx > groove.x)
        ui_gradient(c, (ui_rect_t){groove.x, groove.y, tx - groove.x + THUMB_W / 2, GROOVE_H},
                    UI_COLOUR_CAPTION_L, UI_COLOUR_CAPTION_R, 0);
    ui_edge(c, groove, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);

    if (s->max <= 0)
        return;

    ui_rect_t thumb = { tx, r.y + (r.h - th) / 2, THUMB_W, th };
    ui_fill(c, thumb, UI_COLOUR_BUTTON);
    ui_edge(c, thumb, UI_COLOUR_BUTTON_HI, UI_COLOUR_BUTTON_LO);
    ui_edge(c, ui_rect_shrink(thumb, 1), UI_COLOUR_BUTTON_IN_HI, UI_COLOUR_BUTTON_IN_LO);
    row(c, thumb.x + 3, thumb.y + th / 2, THUMB_W - 6, UI_COLOUR_BUTTON_LO);
}

static int value_at(const slider_t *s, int x) {
    int span = s->rect.w - THUMB_W;
    if (span <= 0)
        return 0;

    int at = x - s->rect.x;
    if (at < 0)    at = 0;
    if (at > span) at = span;
    return (int)((int64_t)at * s->max / span);
}

int slider_mouse(slider_t *s, int x, int y, int left, int was_left) {
    if (s->max <= 0)
        return 0;

    int result = 0;
    if (left && !was_left && ui_rect_contains(s->rect, x, y)) {
        int tx = thumb_x(s);

        s->grab = (x >= tx && x < tx + THUMB_W) ? x - tx : THUMB_W / 2;
        s->dragging = 1;
    }
    if (!s->dragging)
        return 0;

    int v = value_at(s, x - s->grab);
    if (v != s->value) {
        s->value = v;
        result |= UI_CHANGED | SLIDER_MOVED;
    }
    if (!left) {
        s->dragging = 0;
        result |= UI_CHANGED | SLIDER_DROPPED;
    }
    return result;
}
