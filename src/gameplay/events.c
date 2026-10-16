#include "events.h"
#include "world/world.h"
#include "core/log.h"
#include <stdio.h>
#include <stdarg.h>

void ev_push(World *w, const GameEvent *ev) {
    if (!w->authority) return;
    if (w->event_count >= MAX_PENDING_EVENTS) { log_warn("events: queue full, dropping type %d", ev->type); return; }
    w->events[w->event_count++] = *ev;
}

void ev_hint(World *w, int player, const char *fmt, ...) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_HINT;
    ev.target = player < 0 ? EVENT_ALL : (u8)player;
    ev.except = EVENT_ALL;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);
    ev_push(w, &ev);
}

void ev_sound(World *w, const char *name, vec3 pos, float volume, int except) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SOUND;
    ev.target = EVENT_ALL;
    ev.except = except < 0 ? EVENT_ALL : (u8)except;
    ev.id = dg_hash_str(name);
    ev.pos = pos;
    ev.value = volume;
    dg_strcpy(ev.text, sizeof(ev.text), name);
    ev_push(w, &ev);
}
