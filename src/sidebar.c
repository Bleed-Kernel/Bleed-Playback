#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <keyboard.h>
#include <playback.h>

#define ROW_PAD         4
#define DOUBLE_CLICK_MS 400
#define WHEEL_ROWS      3
#define BOX             9
#define INDENT          18
#define ICON_W          22

#define KEY_ENTER       0x1C
#define KEY_BACK        0x0E
#define KEY_ESCAPE      0x01

static int row_h(void) {
    return ui_font_height(&ui_font_text) + ROW_PAD;
}

static ui_rect_t list_rect(const sidebar_t *s) {
    ui_rect_t r = ui_rect_shrink(s->rect, 2);
    r.w -= UI_SCROLL_W;
    return r;
}

static ui_scrollbar_t *bar(sidebar_t *s) {
    ui_rect_t r  = ui_rect_shrink(s->rect, 2);
    int       n  = list_rect(s).h / row_h();
    s->scroll.rect    = (ui_rect_t){ r.x + r.w - UI_SCROLL_W, r.y, UI_SCROLL_W, r.h };
    s->scroll.total   = s->count;
    s->scroll.visible = n > 0 ? n : 1;
    return &s->scroll;
}

static int row_add(sidebar_t *s, int kind, int list, int track) {
    if (s->count == s->cap) {
        int    cap   = s->cap ? s->cap * 2 : 64;
        row_t *grown = realloc(s->rows, (size_t)cap * sizeof(row_t));
        if (!grown)
            return -1;
        s->rows = grown;
        s->cap  = cap;
    }
    s->rows[s->count++] = (row_t){ kind, list, track };
    return 0;
}

void sidebar_rebuild(sidebar_t *s) {
    row_t was = { -1, -1, -1 };
    if (s->selected >= 0 && s->selected < s->count)
        was = s->rows[s->selected];

    s->count = 0;
    for (int i = 0; i < library.count; i++) {
        row_add(s, ROW_LIST, i, -1);
        if (library.lists[i].open)
            for (int t = 0; t < library.lists[i].count; t++)
                row_add(s, ROW_TRACK, i, t);
    }

    // the same row if its still there
    int found = -1;
    for (int i = 0; i < s->count && found < 0; i++)
        if (s->rows[i].kind == was.kind && s->rows[i].list == was.list && s->rows[i].track == was.track)
            found = i;
    for (int i = 0; i < s->count && found < 0; i++)
        if (s->rows[i].kind == ROW_LIST && s->rows[i].list == was.list)
            found = i;
    if (found < 0)
        found = s->selected < s->count ? s->selected : s->count - 1;

    s->selected = found < 0 ? 0 : found;
    sidebar_show(s, s->selected);
}

void sidebar_show(sidebar_t *s, int i) {
    ui_scrollbar_t *b = bar(s);
    if (i < b->top)
        b->top = i;
    if (i >= b->top + b->visible)
        b->top = i - b->visible + 1;
    ui_scrollbar_to(b, b->top);
}

int sidebar_list(const sidebar_t *s) {
    if (s->selected < 0 || s->selected >= s->count)
        return -1;
    int list = s->rows[s->selected].list;
    return list > RECENT ? list : -1;
}

// drawing

static void expander(ui_canvas_t *c, int x, int y, int open) {
    ui_rect_t r = { x, y - BOX / 2, BOX, BOX };
    ui_fill(c, r, UI_COLOUR_BACK_L);
    ui_edge(c, r, UI_COLOUR_HILIGHT, UI_COLOUR_HILIGHT);
    ui_fill(c, (ui_rect_t){x + 2, y, BOX - 4, 1}, UI_COLOUR_TEXT);
    if (!open)
        ui_fill(c, (ui_rect_t){x + BOX / 2, y - BOX / 2 + 2, 1, BOX - 4}, UI_COLOUR_TEXT);
}

// a clock face for recently played
static void clock_icon(ui_canvas_t *c, int x, int y, uint32_t ink) {
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++)
            if (dx * dx + dy * dy >= 25 && dx * dx + dy * dy <= 38)
                ui_fill(c, (ui_rect_t){x + dx, y + dy, 1, 1}, ink);

    ui_fill(c, (ui_rect_t){x, y - 4, 1, 5}, ink);
    ui_fill(c, (ui_rect_t){x, y, 4, 1}, ink);
}

// lines of a list with a note on the end
static void playlist_icon(ui_canvas_t *c, int x, int y, uint32_t ink) {
    for (int i = 0; i < 3; i++)
        ui_fill(c, (ui_rect_t){x - 7, y - 5 + i * 4, i == 2 ? 6 : 9, 2}, ink);
    ui_fill(c, (ui_rect_t){x + 4, y - 3, 1, 7}, LCD_LIT);
    ui_fill(c, (ui_rect_t){x + 5, y - 3, 2, 1}, LCD_LIT);
    ui_fill(c, (ui_rect_t){x + 2, y + 2, 3, 3}, LCD_LIT);
}

static void play_arrow(ui_canvas_t *c, int x, int y, uint32_t ink) {
    for (int i = -5; i <= 5; i++)
        ui_fill(c, (ui_rect_t){x - 3, y + i, 6 - (i < 0 ? -i : i), 1}, ink);
}

static void list_row(ui_canvas_t *c, sidebar_t *s, const row_t *row, ui_rect_t r, int chosen) {
    songlist_t *l   = &library.lists[row->list];
    int         cy  = r.y + r.h / 2;
    uint32_t    ink = chosen ? UI_COLOUR_TEXT : UI_COLOUR_BACK_TEXT;
    char        text[LIST_NAME_MAX + 16];

    if (l->count)
        expander(c, r.x + 3, cy, l->open);

    int ix = r.x + 4 + BOX + ICON_W / 2;
    if (row->list == RECENT)
        clock_icon(c, ix, cy, ink);
    else
        playlist_icon(c, ix, cy, ink);

    ui_rect_t label = { r.x + 6 + BOX + ICON_W, r.y, r.w - BOX - ICON_W - 8, r.h };

    if (s->editing == row->list) {
        ui_rect_t box = { label.x - 3, r.y + 1, label.w, r.h - 2 };
        ui_fill(c, box, UI_COLOUR_DKSHADOW);
        ui_edge(c, box, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);

        ui_text_ascii(text, sizeof(text), s->edit);
        ui_rect_t inside = { box.x + 3, box.y, box.w - 6, box.h };
        int       tw     = ui_text_width(&ui_font_text, text);
        if (s->edit_fresh && tw)
            ui_fill(c, (ui_rect_t){inside.x - 1, box.y + 2, tw + 2, box.h - 4}, UI_COLOUR_CAPTION_R);
        ui_text_in(c, &ui_font_text, inside, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
        int caret = inside.x + tw + 1;
        ui_fill(c, (ui_rect_t){caret, box.y + 3, 1, box.h - 6}, UI_COLOUR_TEXT);
        return;
    }

    ui_text_ascii(text, LIST_NAME_MAX, l->name);
    ui_text_in(c, &ui_font_text, label, text, UI_COLOUR_TEXT, UI_ALIGN_LEFT);

    char count[16];
    snprintf(count, sizeof(count), "%d", l->count);
    ui_text_in(c, &ui_font_light, label, count, chosen ? UI_COLOUR_TEXT : UI_COLOUR_BACK_TEXT, UI_ALIGN_RIGHT);
}

static void track_row(ui_canvas_t *c, sidebar_t *s, const row_t *row, ui_rect_t r, int chosen) {
    const char *path    = library.lists[row->list].tracks[row->track];
    int         playing = s->playing[0] && !strcmp(path, s->playing);
    int         cy      = r.y + r.h / 2;
    int         ix      = r.x + INDENT + 4 + BOX + ICON_W / 2;
    char        text[UI_NAME_MAX];

    if (playing)
        play_arrow(c, ix, cy, chosen ? UI_COLOUR_TEXT : LCD_TEXT);
    else
        ui_file_icon(c, UI_FILE_AUDIO, ix, cy, chosen ? UI_COLOUR_TEXT : UI_COLOUR_BACK_TEXT);

    ui_text_ascii(text, sizeof(text), path_name(path));
    ui_rect_t label = { r.x + INDENT + 6 + BOX + ICON_W, r.y, r.w - INDENT - BOX - ICON_W - 8, r.h };
    ui_text_in(c, &ui_font_text, label, text, chosen || !playing ? UI_COLOUR_TEXT : LCD_TEXT, UI_ALIGN_LEFT);
}

void sidebar_draw(ui_canvas_t *c, sidebar_t *s) {
    ui_panel(c, s->rect, UI_SUNKEN);

    ui_rect_t lr  = list_rect(s);
    ui_rect_t old = c->clip;
    int       rh  = row_h();

    ui_clip(c, ui_rect_clip(lr, old));
    for (int i = bar(s)->top, y = lr.y; i < s->count && y < lr.y + lr.h; i++, y += rh) {
        ui_rect_t r      = { lr.x, y, lr.w, rh };
        int       chosen = i == s->selected;

        if (chosen && s->editing < 0)
            ui_fill(c, r, UI_COLOUR_CAPTION_R);
        if (s->rows[i].kind == ROW_LIST)
            list_row(c, s, &s->rows[i], r, chosen && s->editing < 0);
        else
            track_row(c, s, &s->rows[i], r, chosen);
    }
    c->clip = old;

    ui_scrollbar_draw(c, bar(s));
}

// input

static void toggle(sidebar_t *s, int i) {
    songlist_t *l = &library.lists[s->rows[i].list];
    if (!l->count)
        return;
    l->open = !l->open;
    s->selected = i;
    sidebar_rebuild(s);
    library_save();
}

static void edit_end(sidebar_t *s, int keep) {
    if (s->editing < 0)
        return;
    if (keep && s->edit[0]) {
        strcpy(library.lists[s->editing].name, s->edit);
        library_save();
    }
    s->editing = -1;
}

void sidebar_rename(sidebar_t *s, int list) {
    if (list <= RECENT || list >= library.count)
        return;
    s->editing    = list;
    s->edit_fresh = 1;
    strcpy(s->edit, library.lists[list].name);

    for (int i = 0; i < s->count; i++) {
        if (s->rows[i].kind == ROW_LIST && s->rows[i].list == list) {
            s->selected = i;
            sidebar_show(s, i);
        }
    }
}

// enter or a double click on a row
static int open_row(sidebar_t *s, int i) {
    if (s->rows[i].kind == ROW_TRACK)
        return SIDE_PLAY | UI_CHANGED;
    toggle(s, i);
    return UI_CHANGED;
}

int sidebar_mouse(sidebar_t *s, int x, int y, int left, uint64_t now_ms) {
    int was_left = s->was_left;
    s->was_left = (uint8_t)left;

    int r = ui_scrollbar_mouse(bar(s), x, y, left);
    if (r & UI_TAKEN)
        return r & UI_CHANGED;
    if (!left || was_left)
        return 0;

    ui_rect_t lr = list_rect(s);
    if (!ui_rect_contains(lr, x, y)) {
        int was = s->editing >= 0;
        edit_end(s, 1);
        return was ? UI_CHANGED : 0;
    }

    int i = s->scroll.top + (y - lr.y) / row_h();
    if (i >= s->count)
        return 0;
    if (s->editing >= 0 && !(s->rows[i].kind == ROW_LIST && s->rows[i].list == s->editing))
        edit_end(s, 1);
    if (s->editing >= 0)
        return 0;

    if (s->rows[i].kind == ROW_LIST && x < lr.x + 4 + BOX) {
        toggle(s, i);
        return UI_CHANGED;
    }

    int twice = i == s->last_click && now_ms - s->last_click_ms < DOUBLE_CLICK_MS;
    s->selected      = i;
    s->last_click    = twice ? -1 : i;
    s->last_click_ms = now_ms;
    return twice ? open_row(s, i) : UI_CHANGED;
}

int sidebar_wheel(sidebar_t *s, int notches) {
    ui_scrollbar_t *b   = bar(s);
    int             old = b->top;
    ui_scrollbar_to(b, b->top - notches * WHEEL_ROWS);
    return b->top != old ? UI_CHANGED : 0;
}

static int edit_key(sidebar_t *s, uint16_t keycode, uint8_t keymod) {
    keyboard_event_t key = { .keycode = keycode, .action = KEY_DOWN, .keymod = keymod };
    char             ch  = tty_key_to_ascii(&key);
    // shift and the like come through on their own and dont count as typing
    if (keycode != KEY_ENTER && keycode != KEY_ESCAPE && keycode != KEY_BACK && (ch < ' ' || ch >= 127))
        return 0;

    int fresh = s->edit_fresh;
    s->edit_fresh = 0;

    // the first key after it starts replaces the old name
    if (fresh && (keycode == KEY_BACK || (ch >= ' ' && ch < 127)))
        s->edit[0] = '\0';
    size_t len = strlen(s->edit);

    if (keycode == KEY_ENTER) {
        edit_end(s, 1);
    } else if (keycode == KEY_ESCAPE) {
        edit_end(s, 0);
    } else if (keycode == KEY_BACK) {
        if (len && !fresh)
            s->edit[len - 1] = '\0';
    } else if (ch >= ' ' && ch < 127 && len + 1 < LIST_NAME_MAX) {
        s->edit[len]     = ch;
        s->edit[len + 1] = '\0';
    }
    return UI_CHANGED;
}

int sidebar_key(sidebar_t *s, uint16_t keycode, uint8_t keymod) {
    if (s->editing >= 0)
        return edit_key(s, keycode, keymod);
    if (!s->count)
        return 0;

    int          i   = s->selected;
    int          n   = bar(s)->visible;
    const row_t *row = &s->rows[i];

    switch (keycode) {
    case ArrowUp:   i--; break;
    case ArrowDown: i++; break;
    case PageUp:    i -= n - 1; break;
    case PageDown:  i += n - 1; break;
    case Home:      i = 0; break;
    case End:       i = s->count - 1; break;
    case KEY_ENTER: return open_row(s, i);
    case F2:
        sidebar_rename(s, row->list);
        return UI_CHANGED;
    case ArrowRight:
        if (row->kind == ROW_LIST && !library.lists[row->list].open)
            toggle(s, i);
        return UI_CHANGED;
    case ArrowLeft:
        if (row->kind == ROW_TRACK) {
            for (; i > 0 && s->rows[i].kind == ROW_TRACK; i--)
                ;
        } else if (library.lists[row->list].open) {
            toggle(s, i);
            return UI_CHANGED;
        }
        break;
    default:
        return 0;
    }

    if (i >= s->count) i = s->count - 1;
    if (i < 0)         i = 0;
    s->selected = i;
    sidebar_show(s, i);
    return UI_CHANGED;
}
