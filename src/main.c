#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <syscalls.h>
#include <keyboard.h>
#include <hpet.h>
#include <crime/crime.h>
#include <bleed_audio.h>
#include <playback.h>

#define WINDOW_W        640
#define WINDOW_H        480
#define PAD             8
#define LCD_H           84
#define SEEK_H          22
#define BUTTON_W        34
#define BUTTON_H        26
#define VOLUME_W        96
#define PATH_H          24
#define STATUS_H        22
#define STATUS_RIGHT_W  110
#define SIDE_W          220
#define SIDE_GAP        8
#define SIDE_BUTTON_W   68
#define KEY_DELETE      0x53

#define FRAME_MS        40
#define SEEK_STEP_MS    5000
#define RESTART_MS      3000
#define VOLUME_STEP     5
#define START_DIR       "/"

enum {
    BTN_PREV,
    BTN_PLAY,
    BTN_PAUSE,
    BTN_STOP,
    BTN_NEXT,
    BTN_UP,
    BTN_COUNT,
};

static const int btn_glyph[BTN_COUNT] = {
    GLYPH_PREV, GLYPH_PLAY, GLYPH_PAUSE, GLYPH_STOP, GLYPH_NEXT, GLYPH_FOLDER_UP,
};

enum {
    SIDE_NEW,
    SIDE_ADD,
    SIDE_REMOVE,
    SIDE_BUTTONS,
};

static const char *side_label[SIDE_BUTTONS] = { "New", "Add", "Remove" };

enum {
    FOCUS_FILES,
    FOCUS_SIDE,
};

static cs_window_t *win;
static mk_player_t *player;
static ui_filelist_t files;
static sidebar_t    side = { .editing = -1, .last_click = -1 };
static ui_button_t  side_buttons[SIDE_BUTTONS];
static int          focus;
static int          pending_remove = -1;
static lcd_t        lcd;
static char         caption[CS_TITLE_MAX] = "Playback";
static char         song_caption[CS_TITLE_MAX];
static slider_t     seek;
static slider_t     volume;
static ui_button_t  buttons[BTN_COUNT];

static ui_rect_t    lcd_r, path_r, status_r, speaker_r;
static uint32_t     mouse_was;
static int          quit;

// what plays next?
static char       (*queue)[UI_PATH_MAX];
static int          queue_count;
static int          queue_at = -1;
static char         status[160];

static int          hpet_fd = -2;

static uint64_t now_ms(void) {
    static uint64_t fallback;
    uint64_t        fs = 0;

    if (hpet_fd == -2)
        hpet_fd = _open("/dev/hpet", O_RDONLY);
    if (hpet_fd >= 0 && _ioctl(hpet_fd, HPET_IOCTL_GET_FEMTOSECONDS, &fs) == 0 && fs)
        return fs / femtosecondsPerMillisecond;
    return fallback += FRAME_MS;
}

static int media_file(const char *name) {
    return mk_supported(name) ? UI_FILE_AUDIO : UI_FILE_HIDE;
}

static int playing(void) {
    return player && mk_player_state(player) == MK_PLAYING;
}

static void layout(void) {
    int w = win->width, h = win->height;
    int inner = w - PAD * 2;

    lcd_r     = (ui_rect_t){ PAD, PAD, inner, LCD_H };
    seek.rect = (ui_rect_t){ PAD, lcd_r.y + LCD_H + 6, inner, SEEK_H };

    int by = seek.rect.y + SEEK_H + 4;
    for (int i = 0; i < BTN_UP; i++)
        buttons[i].rect = (ui_rect_t){ PAD + i * (BUTTON_W + 2), by, BUTTON_W, BUTTON_H };

    volume.rect = (ui_rect_t){ w - PAD - VOLUME_W, by + (BUTTON_H - SEEK_H) / 2, VOLUME_W, SEEK_H };
    speaker_r   = (ui_rect_t){ volume.rect.x - 20, by, 18, BUTTON_H };

    // the sidebar down the left with its buttons on top, the folder on the right
    int py = by + BUTTON_H + 14;
    int fx = PAD + SIDE_W + SIDE_GAP;
    int fw = w - PAD - fx;
    for (int i = 0; i < SIDE_BUTTONS; i++)
        side_buttons[i].rect = (ui_rect_t){ PAD + i * (SIDE_BUTTON_W + 8), py, SIDE_BUTTON_W, PATH_H };
    buttons[BTN_UP].rect = (ui_rect_t){ w - PAD - 28, py, 28, PATH_H };
    path_r       = (ui_rect_t){ fx, py, fw - 32, PATH_H };
    status_r     = (ui_rect_t){ 0, h - STATUS_H, w, STATUS_H };

    int ly = py + PATH_H + 4;
    int lh = status_r.y - 6 - ly;
    files.rect = (ui_rect_t){ fx, ly, fw, lh > 0 ? lh : 0 };
    side.rect  = (ui_rect_t){ PAD, ly, SIDE_W, lh > 0 ? lh : 0 };
    ui_filelist_show(&files, files.selected);
    sidebar_show(&side, side.selected);
}

// drawing

static ui_canvas_t canvas(void) {
    return ui_canvas(win->pixels, win->width, win->height, win->pitch);
}

static void commit(ui_rect_t r) {
    cs_window_commit(win, r.x, r.y, r.w, r.h);
}

static void caption_update(void) {
    const char *want = lcd.state == MK_PLAYING || lcd.state == MK_PAUSED ? song_caption : "Playback";
    if (strcmp(want, caption) == 0)
        return;

    snprintf(caption, sizeof(caption), "%s", want);
    cs_window_title(win, caption);
}

static void lcd_update(void) {
    if (!player)
        return;

    lcd.state  = mk_player_state(player);
    lcd.len_ms = mk_player_length_ms(player);
    if (!seek.dragging)
        lcd.pos_ms = mk_player_pos_ms(player);
    if (lcd.state == MK_ENDED)
        lcd.state = MK_STOPPED;

    if (playing())
        mk_player_spectrum(player, lcd.bands);
    else
        memset(lcd.bands, 0, sizeof(lcd.bands));

    caption_update();
}

static void draw_lcd(ui_canvas_t *c) {
    lcd_draw(c, lcd_r, &lcd);
}

static void draw_seek(ui_canvas_t *c) {
    if (!seek.dragging)
        seek.value = (int)lcd.pos_ms;
    seek.max = (int)lcd.len_ms;
    slider_draw(c, &seek);
}

static void draw_button(ui_canvas_t *c, int i) {
    glyph_button_draw(c, &buttons[i], btn_glyph[i]);
}

static void draw_path(ui_canvas_t *c) {
    char text[PB_PATH_MAX];
    ui_text_ascii(text, sizeof(text), files.path);

    ui_panel(c, path_r, UI_SUNKEN);

    ui_rect_t inside = ui_rect_shrink(path_r, 4);
    const char *shown = text;
    while (*shown && ui_text_width(&ui_font_text, shown) > inside.w)
        shown++;

    ui_rect_t old = c->clip;
    ui_clip(c, ui_rect_clip(inside, old));
    ui_text_in(c, &ui_font_text, inside, shown, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    c->clip = old;
}

static void draw_status(ui_canvas_t *c) {
    ui_fill(c, status_r, UI_COLOUR_FACE);
    ui_fill(c, (ui_rect_t){status_r.x, status_r.y, status_r.w, 1}, UI_COLOUR_HILIGHT);

    ui_rect_t left  = { 2, status_r.y + 3, status_r.w - STATUS_RIGHT_W - 6, STATUS_H - 5 };
    ui_rect_t right = { status_r.w - STATUS_RIGHT_W - 2, status_r.y + 3, STATUS_RIGHT_W, STATUS_H - 5 };
    char      vol[32];
    snprintf(vol, sizeof(vol), "Volume %d%%", volume.value);

    ui_edge(c, left, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);
    ui_edge(c, right, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);

    ui_rect_t old = c->clip;
    ui_clip(c, ui_rect_clip(ui_rect_shrink(left, 1), old));
    ui_text_in(c, &ui_font_text, (ui_rect_t){left.x + 5, left.y, left.w - 10, left.h}, status,
               UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    c->clip = old;
    ui_text_in(c, &ui_font_text, right, vol, UI_COLOUR_TEXT, UI_ALIGN_CENTRE);
}

static void draw_all(void) {
    ui_canvas_t c = canvas();

    ui_fill(&c, c.clip, UI_COLOUR_FACE);
    draw_lcd(&c);
    draw_seek(&c);
    for (int i = 0; i < BTN_COUNT; i++)
        draw_button(&c, i);

    glyph_draw(&c, GLYPH_SPEAKER, speaker_r.x + speaker_r.w / 2, speaker_r.y + speaker_r.h / 2, UI_COLOUR_TEXT);
    slider_draw(&c, &volume);

    ui_separator(&c, PAD, buttons[BTN_UP].rect.y - 8, win->width - PAD * 2);
    for (int i = 0; i < SIDE_BUTTONS; i++)
        ui_button_draw(&c, &side_buttons[i]);
    sidebar_draw(&c, &side);
    draw_path(&c);
    ui_filelist_draw(&c, &files);
    draw_status(&c);

    cs_window_commit(win, 0, 0, win->width, win->height);
}

// the bits that change while a song plays
static void draw_frame(void) {
    ui_canvas_t c = canvas();

    draw_lcd(&c);
    commit(lcd_r);
    draw_seek(&c);
    commit(seek.rect);
}

static void redraw_status(void) {
    ui_canvas_t c = canvas();
    draw_status(&c);
    commit(status_r);
}

static void redraw_files(void) {
    ui_canvas_t c = canvas();
    draw_path(&c);
    commit(path_r);
    ui_filelist_draw(&c, &files);
    commit(files.rect);
}

static void redraw_side(void) {
    ui_canvas_t c = canvas();
    sidebar_draw(&c, &side);
    commit(side.rect);
}

// the folder showing is remembered along with everything else
static void save(void) {
    strcpy(library.folder, files.path);
    library.volume = volume.value;
    library_save();
}

static void status_say(const char *text) {
    snprintf(status, sizeof(status), "%s", text);
    redraw_status();
}

static void status_set(void) {
    const char *what = "Stopped";
    if (!player)
        what = "Couldnt get the sound card, nothing can play";
    else if (mk_player_state(player) == MK_PLAYING)
        what = "Playing";
    else if (mk_player_state(player) == MK_PAUSED)
        what = "Paused";

    if (player && queue_at >= 0 && queue_count > 1)
        snprintf(status, sizeof(status), "%s - track %d of %d", what, queue_at + 1, queue_count);
    else
        snprintf(status, sizeof(status), "%s", what);
}

// playing

static void title_from(const mk_info_t *info, const char *name) {
    char raw[PB_NAME_MAX];

    if (info->title[0] && info->artist[0])
        snprintf(raw, sizeof(raw), "%s - %s", info->artist, info->title);
    else if (info->title[0])
        snprintf(raw, sizeof(raw), "%s", info->title);
    else {
        snprintf(raw, sizeof(raw), "%s", name);
        char *dot = strrchr(raw, '.');
        if (dot && dot != raw)
            *dot = '\0';
    }
    ui_text_ascii(lcd.title, sizeof(lcd.title), raw);
    lcd.scroll = 0;

    // the window gets song first, without tags its just the file name
    if (info->title[0] && info->artist[0])
        snprintf(raw, sizeof(raw), "Playback %s - %s", info->title, info->artist);
    else if (info->title[0])
        snprintf(raw, sizeof(raw), "Playback %s", info->title);
    else
        snprintf(raw, sizeof(raw), "Playback %s", lcd.title);
    ui_text_ascii(song_caption, sizeof(song_caption), raw);
}

static int play_at(int i) {
    if (!player || i < 0 || i >= queue_count)
        return -1;

    const char *path = queue[i];
    if (mk_player_load(player, path) < 0) {
        char name[UI_NAME_MAX], text[UI_NAME_MAX + 16];
        ui_text_ascii(name, sizeof(name), path_name(path));
        snprintf(text, sizeof(text), "Couldnt play %s", name);
        status_say(text);
        return -1;
    }

    queue_at = i;
    title_from(mk_player_info(player), path_name(path));
    strcpy(files.marked, path);
    strcpy(side.playing, path);
    mk_player_play(player);

    library_played(path);
    sidebar_rebuild(&side);
    save();

    status_set();
    lcd_update();
    draw_all();
    return 0;
}

static int queue_room(int n) {
    free(queue);
    queue       = malloc((size_t)(n ? n : 1) * UI_PATH_MAX);
    queue_count = 0;
    queue_at    = -1;
    return queue ? 0 : -1;
}

// the folders songs become what plays next, starting from entry i
static void play_from_files(int i) {
    if (queue_room(files.count) < 0)
        return;

    int start = 0;
    for (int k = 0; k < files.count; k++) {
        if (files.files[k].icon == UI_FILE_FOLDER)
            continue;
        if (k == i)
            start = queue_count;
        ui_filelist_path(&files, k, queue[queue_count++], UI_PATH_MAX);
    }
    play_at(start);
}

static void play_from_list(int list, int track) {
    songlist_t *l = &library.lists[list];
    if (queue_room(l->count) < 0)
        return;

    memcpy(queue, l->tracks, (size_t)l->count * UI_PATH_MAX);
    queue_count = l->count;
    play_at(track);
}

static void files_result(int r) {
    if (r & UI_OPENED)
        play_from_files(files.selected);
    else if (r & UI_CHANGED)
        redraw_files();
}

static void side_result(int r) {
    static int was_editing = -1;

    // a name was just typed in, the status bar can stop asking for one
    if (was_editing >= 0 && side.editing < 0) {
        status_set();
        redraw_status();
    }
    was_editing = side.editing;

    if (r & (UI_CHANGED | SIDE_PLAY))
        pending_remove = -1;
    if (r & SIDE_PLAY) {
        const row_t *row = &side.rows[side.selected];
        play_from_list(row->list, row->track);
    } else if (r & UI_CHANGED) {
        redraw_side();
    }
}

// playlists

static void playlist_new(void) {
    char name[LIST_NAME_MAX];
    snprintf(name, sizeof(name), "Playlist %d", library.count);

    if (!library_add_list(name)) {
        status_say("No memory for another playlist");
        return;
    }
    sidebar_rebuild(&side);
    sidebar_rename(&side, library.count - 1);
    focus = FOCUS_SIDE;
    save();
    redraw_side();
    status_say("Type a name for the playlist and press enter");
}

static int add_path(songlist_t *l, const char *path) {
    return songlist_add(l, path) == 0;
}

// the song picked on the right, or every song in the folder picked on the right
static void playlist_add(void) {
    int target = sidebar_list(&side);
    if (target < 0) {
        status_say("Pick a playlist on the left to add to");
        return;
    }
    if (files.selected < 0 || files.selected >= files.count)
        return;

    songlist_t *l     = &library.lists[target];
    int         added = 0;
    char        path[UI_PATH_MAX];
    ui_filelist_path(&files, files.selected, path, sizeof(path));

    if (files.files[files.selected].icon != UI_FILE_FOLDER) {
        added = add_path(l, path);
    } else if (strcmp(files.files[files.selected].name, "..") != 0) {
        ui_filelist_t inside = { .filter = media_file };
        if (ui_filelist_open(&inside, path) == 0) {
            for (int i = 0; i < inside.count; i++) {
                if (inside.files[i].icon == UI_FILE_FOLDER)
                    continue;
                char song[UI_PATH_MAX];
                ui_filelist_path(&inside, i, song, sizeof(song));
                added += add_path(l, song);
            }
        }
        ui_filelist_free(&inside);
    }

    char name[LIST_NAME_MAX], text[LIST_NAME_MAX + 48];
    ui_text_ascii(name, sizeof(name), l->name);
    if (added) {
        l->open = 1;
        sidebar_rebuild(&side);
        save();
        redraw_side();
        snprintf(text, sizeof(text), "Added %d song%s to %s", added, added == 1 ? "" : "s", name);
    } else {
        snprintf(text, sizeof(text), "Nothing new to add to %s", name);
    }
    status_say(text);
}

// a track goes straight away, a whole list only on the second press
static void playlist_remove(void) {
    if (side.selected < 0 || side.selected >= side.count)
        return;

    const row_t *row = &side.rows[side.selected];
    songlist_t  *l   = &library.lists[row->list];
    char         name[LIST_NAME_MAX], text[LIST_NAME_MAX + 48];
    ui_text_ascii(name, sizeof(name), l->name);

    if (row->kind == ROW_TRACK) {
        songlist_remove(l, row->track);
    } else if (pending_remove != row->list) {
        if (row->list == RECENT && !l->count)
            return;
        pending_remove = row->list;
        if (row->list == RECENT)
            snprintf(text, sizeof(text), "Press Remove again to clear %s", name);
        else
            snprintf(text, sizeof(text), "Press Remove again to delete %s", name);
        status_say(text);
        return;
    } else if (row->list == RECENT) {
        l->count = 0;
        status_say("Cleared recently played");
    } else {
        library_remove_list(row->list);
        snprintf(text, sizeof(text), "Deleted %s", name);
        status_say(text);
    }

    pending_remove = -1;
    sidebar_rebuild(&side);
    save();
    redraw_side();
}

static void do_play(void) {
    if (!player)
        return;
    if (mk_player_info(player)) {
        mk_player_play(player);
    } else {
        int i = files.selected;
        if (i < 0 || i >= files.count || files.files[i].icon == UI_FILE_FOLDER)
            for (i = 0; i < files.count && files.files[i].icon == UI_FILE_FOLDER; i++)
                ;
        if (i < files.count)
            play_from_files(i);
    }
}

static void do_pause(void) {
    if (!player)
        return;
    if (mk_player_state(player) == MK_PAUSED)
        mk_player_play(player);
    else
        mk_player_pause(player);
}

static void do_prev(void) {
    if (!player)
        return;
    if (mk_player_pos_ms(player) > RESTART_MS || queue_at <= 0)
        mk_player_seek_ms(player, 0);
    else
        play_at(queue_at - 1);
}

static void do_next(void) {
    if (player && queue_at + 1 < queue_count)
        play_at(queue_at + 1);
}

static void seek_by(int64_t ms) {
    if (!player || !mk_player_info(player))
        return;

    int64_t to = (int64_t)mk_player_pos_ms(player) + ms;
    if (to < 0)
        to = 0;
    mk_player_seek_ms(player, (uint64_t)to);
}

static void volume_set(int v) {
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    volume.value = v;
    if (player)
        mk_player_volume(player, v);

    ui_canvas_t c = canvas();
    slider_draw(&c, &volume);
    commit(volume.rect);
    redraw_status();
}

static void volume_keep(int v) {
    volume_set(v);
    save();
}

// events

static void button_clicked(int i) {
    switch (i) {
    case BTN_PREV:  do_prev(); break;
    case BTN_PLAY:  do_play(); break;
    case BTN_PAUSE: do_pause(); break;
    case BTN_STOP:  if (player) mk_player_stop(player); break;
    case BTN_NEXT:  do_next(); break;
    case BTN_UP:
        ui_filelist_up(&files);
        redraw_files();
        break;
    }
}

static void on_mouse(const cs_event_t *ev) {
    int left     = (ev->buttons & CS_BUTTON_LEFT) != 0;
    int was_left = (mouse_was & CS_BUTTON_LEFT) != 0;
    mouse_was = ev->buttons;

    ui_canvas_t c = canvas();

    int r = slider_mouse(&seek, ev->x, ev->y, left, was_left);
    if (r & SLIDER_MOVED)
        lcd.pos_ms = (uint64_t)seek.value;
    if ((r & SLIDER_DROPPED) && player)
        mk_player_seek_ms(player, (uint64_t)seek.value);
    if (r & UI_CHANGED) {
        draw_frame();
        return;
    }

    r = slider_mouse(&volume, ev->x, ev->y, left, was_left);
    if (r & SLIDER_MOVED)
        volume_set(volume.value);
    if (r & SLIDER_DROPPED)
        volume_keep(volume.value);
    if (r & (SLIDER_MOVED | SLIDER_DROPPED))
        return;
    if (volume.dragging)
        return;

    for (int i = 0; i < BTN_COUNT; i++) {
        r = ui_button_mouse(&buttons[i], ev->x, ev->y, left);
        if (r & UI_CHANGED) {
            draw_button(&c, i);
            commit(buttons[i].rect);
        }
        if (r & UI_CLICKED) {
            button_clicked(i);
            status_set();
            redraw_status();
        }
    }

    for (int i = 0; i < SIDE_BUTTONS; i++) {
        r = ui_button_mouse(&side_buttons[i], ev->x, ev->y, left);
        if (r & UI_CHANGED) {
            ui_button_draw(&c, &side_buttons[i]);
            commit(side_buttons[i].rect);
        }
        if (!(r & UI_CLICKED))
            continue;
        if (i == SIDE_NEW)    playlist_new();
        if (i == SIDE_ADD)    playlist_add();
        if (i == SIDE_REMOVE) playlist_remove();
    }

    if (left && !was_left && lcd_clock_hit(lcd_r, ev->x, ev->y)) {
        lcd.remaining = !lcd.remaining;
        draw_frame();
    }

    // the keys go to whichever side was clicked last
    if (left && !was_left) {
        if (ui_rect_contains(side.rect, ev->x, ev->y))
            focus = FOCUS_SIDE;
        else if (ui_rect_contains(files.rect, ev->x, ev->y) || ui_rect_contains(path_r, ev->x, ev->y))
            focus = FOCUS_FILES;
    }

    side_result(sidebar_mouse(&side, ev->x, ev->y, left, now_ms()));
    files_result(ui_filelist_mouse(&files, ev->x, ev->y, left, now_ms()));
}

static void on_key(const cs_event_t *ev) {
    if (ev->key_action != KEY_DOWN)
        return;

    keyboard_event_t key = { .keycode = ev->keycode, .action = ev->key_action, .keymod = ev->keymod };
    char ch = tty_key_to_ascii(&key);
    if (ch >= 'A' && ch <= 'Z')
        ch += 'a' - 'A';

    // naming a playlist takes every key
    if (side.editing >= 0) {
        side_result(sidebar_key(&side, ev->keycode, ev->keymod));
        return;
    }

    if (focus == FOCUS_SIDE) {
        if (ev->keycode == KEY_DELETE) {
            playlist_remove();
            return;
        }
        int r = sidebar_key(&side, ev->keycode, ev->keymod);
        if (r) {
            side_result(r);
            return;
        }
    }

    switch (ev->keycode) {
    case ArrowLeft:  seek_by(-SEEK_STEP_MS); return;
    case ArrowRight: seek_by(SEEK_STEP_MS); return;
    }
    if (focus == FOCUS_FILES) {
        int r = ui_filelist_key(&files, ev->keycode);
        if (r) {
            files_result(r);
            return;
        }
    }

    switch (ch) {
    case ' ':
        if (playing())
            mk_player_pause(player);
        else
            do_play();
        break;
    case 's':  if (player) mk_player_stop(player); break;
    case 'n':  do_next(); break;
    case 'p':  do_prev(); break;
    case '+':
    case '=':  volume_keep(volume.value + VOLUME_STEP); return;
    case '-':  volume_keep(volume.value - VOLUME_STEP); return;
    default:   return;
    }
    status_set();
    redraw_status();
}

static void on_event(const cs_event_t *ev) {
    switch (ev->type) {
    case CS_EV_CLOSE_REQUEST:
        quit = 1;
        break;
    case CS_EV_RESIZE:
        layout();
        draw_all();
        break;
    case CS_EV_MOUSE:
        on_mouse(ev);
        break;
    case CS_EV_WHEEL:
        if (ui_rect_contains(volume.rect, ev->x, ev->y) || ui_rect_contains(speaker_r, ev->x, ev->y))
            volume_keep(volume.value + ev->wheel * VOLUME_STEP);
        else if (ui_rect_contains(side.rect, ev->x, ev->y))
            side_result(sidebar_wheel(&side, ev->wheel));
        else
            files_result(ui_filelist_wheel(&files, ev->wheel));
        break;
    case CS_EV_KEY:
        on_key(ev);
        break;
    }
}

// a song ran out, on to the next one or stop at the end of the folder
static void check_ended(void) {
    if (!player || mk_player_state(player) != MK_ENDED)
        return;

    if (queue_at + 1 < queue_count && play_at(queue_at + 1) == 0)
        return;

    mk_player_stop(player);
    status_set();
    redraw_status();
}

// handed a folder its opened, handed a song its folder is opened and the song played
static void start_with(const char *arg) {
    if (!arg) {
        if (!library.folder[0] || ui_filelist_open(&files, library.folder) < 0)
            ui_filelist_open(&files, START_DIR);
        return;
    }
    if (ui_filelist_open(&files, arg) == 0)
        return;

    char dir[UI_PATH_MAX];
    strncpy(dir, arg, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char       *slash = strrchr(dir, '/');
    const char *name  = slash ? arg + (slash - dir) + 1 : arg;
    if (slash == dir)
        dir[1] = '\0';
    else if (slash)
        *slash = '\0';
    else
        strcpy(dir, ".");

    if (ui_filelist_open(&files, dir) < 0) {
        ui_filelist_open(&files, START_DIR);
        return;
    }
    for (int i = 0; i < files.count; i++) {
        if (files.files[i].icon != UI_FILE_FOLDER && !strcmp(files.files[i].name, name)) {
            files.selected = i;
            play_from_files(i);
            return;
        }
    }
}

int main(int argc, char **argv) {
    if (cs_connect() < 0) {
        printf("playback: needs the desktop running\n");
        return 1;
    }
    if (ui_init() < 0) {
        printf("playback: no font ending in " UI_FONT_REGULAR " in " UI_FONT_DIR "\n");
        return 1;
    }

    player = mk_player_new();
    library_load();

    win = cs_window_open(WINDOW_W, WINDOW_H, "Playback");
    if (!win) {
        printf("playback: the desktop wouldnt give us a window\n");
        return 1;
    }

    files.filter = media_file;
    volume.max   = 100;
    volume.value = library.volume;
    if (player)
        mk_player_volume(player, volume.value);
    for (int i = 0; i < BTN_COUNT; i++)
        buttons[i].label = "";
    for (int i = 0; i < SIDE_BUTTONS; i++)
        side_buttons[i].label = side_label[i];
    strcpy(lcd.title, "");

    sidebar_rebuild(&side);
    layout();
    start_with(argc > 1 ? argv[1] : NULL);
    layout();
    status_set();
    draw_all();

    uint64_t last_frame = now_ms();
    int      moving     = 0;
    cs_event_t ev;

    while (!quit) {
        int wait = player ? mk_player_pump(player) : -1;
        check_ended();

        int timeout = wait;
        if (playing() || moving) {
            int64_t left = (int64_t)(last_frame + FRAME_MS) - (int64_t)now_ms();
            int     due  = left > 0 ? (int)left : 0;
            if (timeout < 0 || due < timeout)
                timeout = due;
        }

        if (cs_event_wait(&ev, timeout) > 0)
            on_event(&ev);

        uint64_t now = now_ms();
        if (now - last_frame >= FRAME_MS) {
            int state = lcd.state;
            lcd_update();
            moving = lcd_tick(&lcd);
            if (moving || state != lcd.state || playing())
                draw_frame();
            if (state != lcd.state) {
                status_set();
                redraw_status();
            }
            last_frame = now;
        }
    }

    save();
    mk_player_free(player);
    ui_filelist_free(&files);
    au_close();
    cs_window_close(win);
    return 0;
}
