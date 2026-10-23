#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <syscalls.h>
#include <playback.h>

#define CONFIG_MAX      (256 * 1024)
#define VOLUME_DEFAULT  80

library_t library;

const char *path_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static songlist_t *list_new(const char *name) {
    if (library.count == library.cap) {
        int         cap   = library.cap ? library.cap * 2 : 8;
        songlist_t *grown = realloc(library.lists, (size_t)cap * sizeof(songlist_t));
        if (!grown)
            return NULL;
        library.lists = grown;
        library.cap   = cap;
    }

    songlist_t *s = &library.lists[library.count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, LIST_NAME_MAX - 1);
    return s;
}

songlist_t *library_add_list(const char *name) {
    return list_new(name);
}

void library_remove_list(int i) {
    if (i <= RECENT || i >= library.count)
        return;

    free(library.lists[i].tracks);
    memmove(&library.lists[i], &library.lists[i + 1], (size_t)(library.count - i - 1) * sizeof(songlist_t));
    library.count--;
}

static int track_room(songlist_t *s) {
    if (s->count < s->cap)
        return 0;

    int  cap = s->cap ? s->cap * 2 : 16;
    void *grown = realloc(s->tracks, (size_t)cap * UI_PATH_MAX);
    if (!grown)
        return -1;
    s->tracks = grown;
    s->cap    = cap;
    return 0;
}

int songlist_add(songlist_t *s, const char *path) {
    if (!path[0] || strlen(path) >= UI_PATH_MAX)
        return -1;
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->tracks[i], path))
            return -1;
    if (track_room(s) < 0)
        return -1;

    strcpy(s->tracks[s->count++], path);
    return 0;
}

void songlist_remove(songlist_t *s, int i) {
    if (i < 0 || i >= s->count)
        return;
    memmove(s->tracks[i], s->tracks[i + 1], (size_t)(s->count - i - 1) * UI_PATH_MAX);
    s->count--;
}

void library_played(const char *path) {
    songlist_t *r = &library.lists[RECENT];

    for (int i = 0; i < r->count; i++) {
        if (!strcmp(r->tracks[i], path)) {
            songlist_remove(r, i);
            break;
        }
    }
    if (r->count >= RECENT_MAX)
        r->count = RECENT_MAX - 1;
    if (track_room(r) < 0 || strlen(path) >= UI_PATH_MAX)
        return;

    memmove(r->tracks[1], r->tracks[0], (size_t)r->count * UI_PATH_MAX);
    strcpy(r->tracks[0], path);
    r->count++;
}

// the file

static char *config_read(void) {
    int fd = _open(CONFIG_PATH, O_RDONLY);
    if (fd < 0)
        return NULL;

    char *text = malloc(CONFIG_MAX + 1);
    long  len  = 0;
    if (text) {
        long n;
        while (len < CONFIG_MAX && (n = _read(fd, text + len, (unsigned long)(CONFIG_MAX - len))) > 0)
            len += n;
        text[len] = '\0';
    }
    _close(fd);
    return text;
}

// one key=value or path line, where its going depends on the last [section]
static void config_line(char *line, songlist_t **in) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' '))
        line[--len] = '\0';
    if (!len || line[0] == '#')
        return;

    if (!strcmp(line, "[recent]")) {
        *in = &library.lists[RECENT];
        return;
    }
    if (!strcmp(line, "[playlist]")) {
        *in = list_new("Playlist");
        return;
    }

    if (line[0] == '/') {
        if (*in)
            songlist_add(*in, line);
        return;
    }

    char *eq = strchr(line, '=');
    if (!eq)
        return;
    *eq = '\0';
    const char *value = eq + 1;

    if (*in) {
        if (!strcmp(line, "name") && *in != &library.lists[RECENT])
            strncpy((*in)->name, value, LIST_NAME_MAX - 1);
        else if (!strcmp(line, "open"))
            (*in)->open = atoi(value) != 0;
    } else if (!strcmp(line, "volume")) {
        library.volume = atoi(value);
    } else if (!strcmp(line, "folder")) {
        strncpy(library.folder, value, UI_PATH_MAX - 1);
    }
}

void library_load(void) {
    library.count  = 0;
    library.volume = VOLUME_DEFAULT;
    list_new("Recently Played");

    char *text = config_read();
    if (!text)
        return;

    songlist_t *in = NULL;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        config_line(line, &in);
        line = next;
    }
    free(text);

    if (library.volume < 0 || library.volume > 100)
        library.volume = VOLUME_DEFAULT;
}

void library_save(void) {
    _mkdir(CONFIG_DIR, 0755);
    int fd = _open(CONFIG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;

    dprintf(fd, "# playback remembers things here, it rewrites the whole file\n");
    dprintf(fd, "volume=%d\n", library.volume);
    if (library.folder[0])
        dprintf(fd, "folder=%s\n", library.folder);

    for (int i = 0; i < library.count; i++) {
        songlist_t *s = &library.lists[i];
        if (i == RECENT)
            dprintf(fd, "\n[recent]\n");
        else
            dprintf(fd, "\n[playlist]\nname=%s\n", s->name);
        if (s->open)
            dprintf(fd, "open=1\n");
        for (int t = 0; t < s->count; t++)
            dprintf(fd, "%s\n", s->tracks[t]);
    }
    _close(fd);
}
