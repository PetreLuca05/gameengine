// Phone pairing in the menus (docs/PHONE.md "Phone flow"): the Phone card (main menu Player tab,
// the pause menu), the phone glyph (main menu footer, lobby player list) and the Connect portal, a
// centred modal on the screen stack (SCR_PHONE, src/ui/menus.c): QR code, URL, "Waiting for
// device…", Use in-game phone, Close. Layouts: assets/ui/screens/mainmenu_phone_{card,portal}.json.
#include "mm_internal.h"
#include "gameplay/phone/phone.h"
#include "gameplay/phone/phone_os.h"
#include "ui/menus.h"
#include "ui/screens/screens.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// A small phone: rounded outline, the screen lit when a device is connected, the home dot.
void mm_phone_glyph(GuiRect r, u32 col, bool lit) {
    float h = r.h, w = h * 0.58f;
    GuiRect b = { r.x + (r.w - w) * 0.5f, r.y, w, h };
    float t = fmaxf(1.0f, gui_s(1.5f));
    gd_round_outline(b, w * 0.22f, t, col);
    if (lit) gd_round(gui_rect(b.x + t * 2, b.y + t * 2.5f, b.w - t * 4, b.h - t * 6), w * 0.08f, gui_rgba_mul(col, 0.55f));
    gd_circle(b.x + b.w * 0.5f, b.y + b.h - t * 1.9f, t * 0.8f, col);
}

static u32 card_color(int card) {
    switch (card) {
    case PCARD_CONNECTED: return TH->ok;
    case PCARD_WAITING: return TH->info;
    case PCARD_INGAME: return TH->text_dim;
    default: return TH->text_faint;
    }
}

// "Not connected" / "Waiting…" / "Connected (iPhone · Safari)" / "In-game phone".
const char *mm_phone_status_text(char *b, size_t n) {
    PhonePairInfo p = phone_pair_info();
    switch (p.card) {
    case PCARD_CONNECTED: snprintf(b, n, p.device[0] ? "Connected (%s)" : "Connected", p.device); break;
    case PCARD_WAITING: snprintf(b, n, "Waiting\xE2\x80\xA6"); break;
    case PCARD_INGAME: snprintf(b, n, "In-game phone"); break;
    default: snprintf(b, n, "Not connected"); break;
    }
    return b;
}

// The footer / pause entry point: the glyph + the status; a click opens the portal.
float mm_phone_status_chip(GuiRect r) {
    PhonePairInfo p = phone_pair_info();
    char st[96];
    mm_phone_status_text(st, sizeof(st));
    GuiText t = { gui_s(TH->t_small), GW_MEDIUM, TH->text_dim, GUI_LEFT, 0, false, true, 0 };
    float gw = gd_line_height(t.size), tw = gd_text_width(&t, st), pad = gui_s(TH->gap * 0.75f);
    GuiRect hit = { r.x, r.y, gw + pad * 3 + tw, r.h };
    GuiId id = gui_id("mm_phone_chip");
    GuiInteract in = gui_interact(id, hit, GUI_FOCUSABLE);
    if (in.hot) gd_round(hit, gui_s(TH->radius), TH->hover);
    if (in.focused) gw_focus_ring(hit, gui_s(TH->radius), 1.0f);
    mm_phone_glyph(gui_rect(hit.x + pad, r.y + (r.h - gw) * 0.5f, gw, gw), card_color(p.card), p.connected);
    t.color = in.hot ? TH->text : TH->text_dim;
    gd_text(&t, hit.x + pad * 2 + gw, r.y + (r.h - gd_line_height(t.size)) * 0.5f, st);
    if (in.hovered) gui_tooltip(id, "Phone: use your real phone as the in-game phone (Connect portal)");
    if (in.clicked) menus_open_phone_portal();
    return hit.w;
}

// ------------------------------------------------------------------ the card

void mainmenu_phone_card(GuiRect r) {
    MainMenu *M = mm();
    PhonePairInfo p = phone_pair_info();
    mm_panel(r);
    mm_layout_solve(&M->phone_card, gui_rect_inset(r, gui_s(TH->pad)));
    MmLayout *L = &M->phone_card;
    GuiRect head = mm_node(L, "head"), glyph = mm_node(L, "glyph"), title = mm_node(L, "title");
    (void)head;
    float g = fminf(glyph.w, glyph.h);
    mm_phone_glyph(gui_rect(glyph.x + (glyph.w - g) * 0.5f, glyph.y + (glyph.h - g) * 0.5f, g, g), card_color(p.card), p.connected);
    gw_label(title, "Phone", gui_s(TH->t_body + 1), GW_SEMIBOLD, TH->text, GUI_LEFT);
    // Status: a dot + the state.
    GuiRect sr = mm_node(L, "status");
    char st[96];
    mm_phone_status_text(st, sizeof(st));
    float dr = gui_s(TH->radius) + 1;
    gd_circle(sr.x + dr, sr.y + sr.h * 0.5f, dr, card_color(p.card));
    GuiText stt = { gui_s(TH->t_body - 1), GW_MEDIUM, p.card == PCARD_CONNECTED ? TH->text : TH->text_dim, GUI_LEFT, 0, false, true, sr.w - dr * 4 };
    gd_text(&stt, sr.x + dr * 3, sr.y + (sr.h - gd_line_height(stt.size)) * 0.5f, st);
    // What it means / what to do.
    char d[200];
    switch (p.card) {
    case PCARD_CONNECTED: snprintf(d, sizeof(d), "Your phone is the in-game phone: tilt it to aim, Camera and Gallery unlock in a match."); break;
    case PCARD_WAITING: snprintf(d, sizeof(d), "Scan the QR code with your phone (same Wi-Fi as this PC)."); break;
    case PCARD_INGAME: snprintf(d, sizeof(d), "This session uses the in-game phone (HUD panel)."); break;
    default:
        if (p.stored && p.device[0]) snprintf(d, sizeof(d), "Open the page on %s to reconnect, or connect a device.", p.device);
        else snprintf(d, sizeof(d), "Use your real phone as the in-game phone: scan a QR code, no app needed.");
        break;
    }
    if (p.card != PCARD_CONNECTED && p.error[0] && strcmp(p.error, "Phone disconnected")) snprintf(d, sizeof(d), "%s", p.error);
    GuiText dt = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT, 0, false, true, 0 };
    GuiRect dr_ = mm_node(L, "detail");
    gd_clip_push(dr_);
    gd_text_wrap(&dt, dr_.x, dr_.y, dr_.w, d, true);
    gd_clip_pop();
    // Actions.
    GuiRect a = mm_node(L, "primary"), b = mm_node(L, "secondary");
    switch (p.card) {
    case PCARD_CONNECTED:
        if (gw_button(gui_id("ph_disconnect"), a, "Disconnect", GB_SECONDARY, true)) phone_disconnect();
        if (gw_button(gui_id("ph_forget"), b, "Forget device", GB_GHOST, true)) phone_forget();
        break;
    case PCARD_WAITING:
        if (gw_button(gui_id("ph_open"), a, "Show QR code", GB_PRIMARY, true)) menus_open_phone_portal();
        if (gw_button(gui_id("ph_decline"), b, "Use in-game phone", GB_GHOST, true)) phone_decline();
        break;
    case PCARD_INGAME:
        if (gw_button(gui_id("ph_connect"), a, "Connect a device", GB_SECONDARY, true)) menus_open_phone_portal();
        if (p.stored && gw_button(gui_id("ph_forget"), b, "Forget device", GB_GHOST, true)) phone_forget();
        break;
    default:
        if (gw_button(gui_id("ph_connect"), a, "Connect", GB_PRIMARY, true)) menus_open_phone_portal();
        if (p.stored) { if (gw_button(gui_id("ph_forget"), b, "Forget device", GB_GHOST, true)) phone_forget(); }
        else if (gw_button(gui_id("ph_decline"), b, "Use in-game phone", GB_GHOST, true)) phone_decline();
        break;
    }
}

// ------------------------------------------------------------------ the Connect portal (SCR_PHONE)

static void spinner(float cx, float cy, float rad, u32 col) {
    float t = (float)gui_time();
    gd_ring(cx, cy, rad, fmaxf(1.5f, gui_s(2.5f)), 1.0f, gui_rgba_mul(col, 0.25f));
    // A rotating arc: two short arcs read as motion on the clockwise-from-top ring.
    float a = fmodf(t * 1.4f, 1.0f);
    float x = cx + sinf(a * 6.2831853f) * rad, y = cy - cosf(a * 6.2831853f) * rad;
    gd_circle(x, y, fmaxf(2.0f, gui_s(3.0f)), col);
}

bool mainmenu_phone_portal_draw(bool top) {
    MainMenu *M = mm();
    PhonePairInfo p = phone_pair_info();
    if (!p.portal) return true;   // declined / closed by a command
    // A device that connected while the portal was open: "Connected" for a moment, then it closes.
    if (p.portal_connected > 1.6f) return true;
    float a = gui_ease_out(gui_anim_t(gui_idi("screen", SCR_PHONE)) / TH->modal_in);
    gd_rect(gui_rect(0, 0, gui_w(), gui_h()), gui_rgba_mul(TH->scrim, a));
    MmLayout *L = &M->phone_portal;
    mm_layout_solve(L, gui_rect(0, 0, gui_w(), gui_h()));
    GuiRect dlg = mm_node(L, "dialog");
    float dy = (1.0f - a) * gui_s(12);
    for (int i = 0; i < L->count; i++) L->nodes[i].rect.y += dy;
    dlg.y += dy;
    gd_alpha_push(a);
    gui_push_input(top);
    mm_panel(dlg);
    if (top) gui_focus_scope(dlg);
    // Title.
    GuiRect tr = mm_node(L, "title");
    GuiText tt = { gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT, 0, false, false, tr.w };
    gd_text(&tt, tr.x, tr.y + (tr.h - gd_line_height(tt.size)) * 0.5f, p.connected ? "Phone connected" : "Connect your phone");
    // QR code (white quiet zone), or why there is none.
    GuiRect q = mm_node(L, "qr");
    float side = fminf(q.w, q.h);
    GuiRect qs = { q.x + (q.w - side) * 0.5f, q.y + (q.h - side) * 0.5f, side, side };
    if (p.connected) {
        gd_round(qs, gui_s(TH->radius_panel), TH->slot_bg);
        float cr = side * 0.18f;
        gd_circle(qs.x + side * 0.5f, qs.y + side * 0.5f, cr, TH->ok);
        mm_phone_glyph(gui_rect(qs.x + side * 0.5f - cr * 0.6f, qs.y + side * 0.5f - cr * 0.6f, cr * 1.2f, cr * 1.2f), UI_RGBA(255, 255, 255, 255), true);
    } else if (p.code && p.qr) {
        gd_round(qs, gui_s(TH->radius_panel), UI_RGBA(255, 255, 255, 255));
        float m = side * 0.04f;
        gd_image(gui_rect(qs.x + m, qs.y + m, side - m * 2, side - m * 2), p.qr, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
    } else {
        gd_round(qs, gui_s(TH->radius_panel), TH->slot_bg);
        GuiText et = { gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_CENTER, 0, false, true, side * 0.8f };
        const char *why = p.error[0] ? p.error : "Starting the phone server\xE2\x80\xA6";
        gd_text_wrap(&et, qs.x + side * 0.1f, qs.y + side * 0.45f, side * 0.8f, why, true);
    }
    // Steps, the URL, the status.
    GuiRect info = mm_node(L, "steps");
    GuiText it = { gui_s(TH->t_body - 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, true, 0 };
    const char *steps = p.connected ? "Your phone is now the in-game phone. In a match, take it out (X) and tilt your real phone to aim it."
                                    : "1. Join the same Wi-Fi as this PC.\n2. Scan the code with the phone's camera.\n3. The first time, accept the certificate warning (the game makes its own).";
    gd_clip_push(info);
    gd_text_wrap(&it, info.x, info.y, info.w, steps, true);
    gd_clip_pop();
    GuiRect ur = mm_node(L, "url");
    if (!p.connected && p.code) {
        GuiText ut = { gui_s(TH->t_body - 1), GW_MEDIUM, TH->accent, GUI_LEFT, 0, false, true, ur.w };
        char host[200];
        // The address without the one-time code reads better; the code is in the QR (and the console).
        snprintf(host, sizeof(host), "%.*s", (int)(strchr(p.url, '?') ? strchr(p.url, '?') - p.url : (long)strlen(p.url)), p.url);
        gd_text(&ut, ur.x, ur.y, host);
        char ex[64];
        int s = (int)p.token_left;
        snprintf(ex, sizeof(ex), "Code expires in %d:%02d  \xC2\xB7  a new one follows", s / 60, s % 60);
        GuiText xt = { gui_s(TH->t_small), GW_REGULAR, p.token_left < 20 ? TH->warning : TH->text_faint, GUI_LEFT, 0, false, true, ur.w };
        gd_text(&xt, ur.x, ur.y + gd_line_height(ut.size) + gui_s(2), ex);
    }
    GuiRect sr = mm_node(L, "status");
    GuiText st = { gui_s(TH->t_body), GW_MEDIUM, TH->text, GUI_LEFT, 0, false, true, sr.w - sr.h * 1.2f };
    float cy = sr.y + sr.h * 0.5f, rad = sr.h * 0.28f;
    char line[120];
    if (p.connected) {
        gd_circle(sr.x + rad, cy, rad, TH->ok);
        snprintf(line, sizeof(line), p.device[0] ? "Connected (%s)" : "Connected", p.device);
    } else if (p.code) {
        spinner(sr.x + rad, cy, rad, TH->info);
        snprintf(line, sizeof(line), "Waiting for device\xE2\x80\xA6");
    } else {
        gd_circle(sr.x + rad, cy, rad, TH->warning);
        snprintf(line, sizeof(line), "No pairing code");
    }
    gd_text(&st, sr.x + rad * 2 + gui_s(TH->gap), cy - gd_line_height(st.size) * 0.5f, line);
    // Buttons.
    bool close = false;
    GuiRect b1 = mm_node(L, "decline"), b2 = mm_node(L, "close");
    if (p.connected) {
        if (gw_button(gui_id("pp_disconnect"), b1, "Disconnect", GB_SECONDARY, true)) { phone_disconnect(); close = true; }
    } else if (gw_button(gui_id("pp_decline"), b1, "Use in-game phone", GB_SECONDARY, true)) {
        phone_decline();
        close = true;
    }
    if (gw_button(gui_id("pp_close"), b2, "Close", GB_PRIMARY, true)) close = true;
    if (top && !gui_focus()) gui_set_focus(gui_id("pp_close"));
    gui_pop_input();
    gd_alpha_pop();
    return close;
}
