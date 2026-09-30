/* nPlayer 3.13.0 bridge: one external renderer per library. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ass/ass.h"
#define NPA_EXPORT __attribute__((visibility("default")))
typedef struct Library Library;
typedef struct Renderer Renderer;
typedef struct Track Track;
struct Track {
    ASS_Track *track;
    ASS_Library *library;
    ASS_Renderer *renderer;
    Library *owner;
    int fonts_loaded;
    Track *next, *registered_next;
};
struct Renderer {
    Library *owner;
    ASS_Track *last_track;
    int width, height, configured, update;
    char *font, *family, *config;
};
struct Library {
    char *directory;
    int extract, closing;
    void (*message_cb)(int, const char *, va_list, void *);
    void *message_data;
    Renderer *renderer;
    Track *tracks;
    pthread_mutex_t mutex;
};
static Track *registered_tracks;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static void fail(const char *message)
{
    fprintf(stderr, "LibASSBridge: %s\n", message);
    abort();
}
static char *copy_string(const char *s)
{
    char *copy = s ? strdup(s) : NULL;
    if (s && !copy) fail("cannot copy configuration");
    return copy;
}
static Track *find_registered_track(ASS_Track *track)
{
    pthread_mutex_lock(&registry_mutex);
    Track *found = NULL;
    for (Track *t = registered_tracks; t; t = t->registered_next)
        if (t->track == track) { found = t; break; }
    pthread_mutex_unlock(&registry_mutex);
    if (!found) fail("unknown track");
    /* The caller owns this track; concurrent use/free of that same track is invalid. */
    return found;
}
static Track *find_owned_track(Library *l, ASS_Track *track)
{
    for (Track *t = l->tracks; t; t = t->next)
        if (t->track == track) return t;
    fail("renderer and track owners differ");
    return NULL;
}
static void configure_track(Track *t, Renderer *r)
{
    ass_set_frame_size(t->renderer, r->width, r->height);
    ass_set_fonts(t->renderer, r->font, r->family,
                  ASS_FONTPROVIDER_FONTCONFIG, r->config, r->update);
    if (!t->fonts_loaded) {
        /* Future rebuilds reuse this track's snapshot; inline fonts stay intact. */
        ass_set_fonts_dir(t->library, NULL);
        t->fonts_loaded = 1;
    }
}
NPA_EXPORT ASS_Library *npa_ass_library_init(void)
{
    Library *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    if (pthread_mutex_init(&l->mutex, NULL)) { free(l); return NULL; }
    return (ASS_Library *)l;
}
static void destroy_library_record(Library *l)
{
    pthread_mutex_destroy(&l->mutex);
    free(l->directory);
    free(l);
}
NPA_EXPORT void npa_ass_library_done(ASS_Library *library)
{
    Library *l = (Library *)library;
    pthread_mutex_lock(&l->mutex);
    if (l->renderer) fail("library released with live external renderer");
    l->closing = 1;
    int destroy = !l->tracks;
    pthread_mutex_unlock(&l->mutex);
    if (destroy) destroy_library_record(l);
}
NPA_EXPORT void npa_ass_set_fonts_dir(ASS_Library *library, const char *dir)
{
    Library *l = (Library *)library;
    char *s = copy_string(dir);
    pthread_mutex_lock(&l->mutex);
    free(l->directory);
    l->directory = s;
    pthread_mutex_unlock(&l->mutex);
}
NPA_EXPORT void npa_ass_set_extract_fonts(ASS_Library *library, int extract)
{
    Library *l = (Library *)library;
    pthread_mutex_lock(&l->mutex);
    l->extract = extract;
    pthread_mutex_unlock(&l->mutex);
}
NPA_EXPORT void npa_ass_set_message_cb(ASS_Library *library,
    void (*cb)(int, const char *, va_list, void *), void *data)
{
    Library *l = (Library *)library;
    pthread_mutex_lock(&l->mutex);
    l->message_cb = cb;
    l->message_data = data;
    pthread_mutex_unlock(&l->mutex);
}
NPA_EXPORT ASS_Renderer *npa_ass_renderer_init(ASS_Library *library)
{
    Library *l = (Library *)library;
    pthread_mutex_lock(&l->mutex);
    if (l->renderer) fail("only one external renderer per library is supported");
    Renderer *r = calloc(1, sizeof(*r));
    if (!r) { pthread_mutex_unlock(&l->mutex); return NULL; }
    r->owner = l;
    l->renderer = r;
    pthread_mutex_unlock(&l->mutex);
    return (ASS_Renderer *)r;
}
NPA_EXPORT void npa_ass_renderer_done(ASS_Renderer *renderer)
{
    Renderer *r = (Renderer *)renderer;
    pthread_mutex_lock(&r->owner->mutex);
    r->owner->renderer = NULL;
    pthread_mutex_unlock(&r->owner->mutex);
    free(r->font); free(r->family); free(r->config); free(r);
}
NPA_EXPORT void npa_ass_set_frame_size(ASS_Renderer *renderer, int w, int h)
{
    Renderer *r = (Renderer *)renderer;
    pthread_mutex_lock(&r->owner->mutex);
    r->width = w; r->height = h;
    for (Track *t = r->owner->tracks; t; t = t->next)
        ass_set_frame_size(t->renderer, w, h);
    pthread_mutex_unlock(&r->owner->mutex);
}
NPA_EXPORT void npa_ass_set_fonts(ASS_Renderer *renderer, const char *font,
    const char *family, int provider, const char *config, int update)
{
    (void)provider;
    Renderer *r = (Renderer *)renderer;
    char *f = copy_string(font), *a = copy_string(family), *c = copy_string(config);
    pthread_mutex_lock(&r->owner->mutex);
    free(r->font); free(r->family); free(r->config);
    r->font = f; r->family = a; r->config = c; r->update = update;
    r->configured = 1;
    for (Track *t = r->owner->tracks; t; t = t->next)
        configure_track(t, r);
    pthread_mutex_unlock(&r->owner->mutex);
}
NPA_EXPORT ASS_Track *npa_ass_new_track(ASS_Library *library)
{
    Library *l = (Library *)library;
    Track *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    pthread_mutex_lock(&l->mutex);
    if (l->closing) fail("new track after library close");
    t->owner = l;
    t->library = ass_library_init();
    if (!t->library) { pthread_mutex_unlock(&l->mutex); free(t); return NULL; }
    ass_set_fonts_dir(t->library, l->directory);
    ass_set_extract_fonts(t->library, l->extract);
    if (l->message_cb) ass_set_message_cb(t->library, l->message_cb, l->message_data);
    t->renderer = ass_renderer_init(t->library);
    t->track = ass_new_track(t->library);
    if (!t->renderer || !t->track) {
        if (t->track) ass_free_track(t->track);
        if (t->renderer) ass_renderer_done(t->renderer);
        ass_library_done(t->library); pthread_mutex_unlock(&l->mutex); free(t); return NULL;
    }
    t->next = l->tracks;
    l->tracks = t;
    pthread_mutex_lock(&registry_mutex);
    t->registered_next = registered_tracks;
    registered_tracks = t;
    pthread_mutex_unlock(&registry_mutex);
    pthread_mutex_unlock(&l->mutex);
    return t->track;
}
NPA_EXPORT ASS_Image *npa_ass_render_frame(ASS_Renderer *renderer,
    ASS_Track *track, long long now, int *change)
{
    Renderer *r = (Renderer *)renderer;
    pthread_mutex_lock(&r->owner->mutex);
    Track *t = find_owned_track(r->owner, track);
    if (!t->fonts_loaded) {
        if (!r->configured) fail("render before fonts are configured");
        configure_track(t, r);
    }
    ASS_Image *images = ass_render_frame(t->renderer, track, now, change);
    if (r->last_track != track && change) *change = 2;
    r->last_track = track;
    pthread_mutex_unlock(&r->owner->mutex);
    return images;
}
NPA_EXPORT void npa_ass_process_codec_private(ASS_Track *t, const char *data, int size)
{ Track *record = find_registered_track(t); pthread_mutex_lock(&record->owner->mutex); ass_process_codec_private(t, data, size); pthread_mutex_unlock(&record->owner->mutex); }
NPA_EXPORT void npa_ass_process_data(ASS_Track *t, const char *data, int size)
{ Track *record = find_registered_track(t); pthread_mutex_lock(&record->owner->mutex); ass_process_data(t, data, size); pthread_mutex_unlock(&record->owner->mutex); }
NPA_EXPORT void npa_ass_flush_events(ASS_Track *t)
{ Track *record = find_registered_track(t); pthread_mutex_lock(&record->owner->mutex); ass_flush_events(t); pthread_mutex_unlock(&record->owner->mutex); }
NPA_EXPORT void npa_ass_free_track(ASS_Track *track)
{
    Track *t = find_registered_track(track);
    Library *owner = t->owner;
    pthread_mutex_lock(&owner->mutex);
    if (owner->renderer && owner->renderer->last_track == track) owner->renderer->last_track = NULL;
    pthread_mutex_lock(&registry_mutex);
    Track **entry = &registered_tracks;
    while (*entry != t) entry = &(*entry)->registered_next;
    *entry = t->registered_next;
    pthread_mutex_unlock(&registry_mutex);
    Track **p = &t->owner->tracks;
    while (*p != t) p = &(*p)->next;
    *p = t->next;
    ass_free_track(track);
    ass_renderer_done(t->renderer);
    ass_library_done(t->library);
    int destroy = owner->closing && !owner->tracks;
    pthread_mutex_unlock(&owner->mutex);
    free(t);
    if (destroy) destroy_library_record(owner);
}
