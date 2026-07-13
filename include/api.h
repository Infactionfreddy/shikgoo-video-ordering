#pragma once
#include "state.h"
#include "http.h"
#include "ws.h"

/* REST endpoints, werden in main.c von handle_conn() nach method+path gedispatcht.
 * jede funktion schreibt ihre antwort selbst auf den Conn */
void api_get_menu     (Conn *c);
void api_post_order   (Conn *c, const char *body);
void api_get_order    (Conn *c, const char *path);
void api_patch_order  (Conn *c, const char *path, const char *body);
void api_waiter_status(Conn *c, const char *method, const char *body); /* GET liest, POST setzt busy */
void api_get_queue    (Conn *c);
void api_call_waiter  (Conn *c, const char *body);
void api_get_calls    (Conn *c);
void api_resolve_call (Conn *c, const char *path);
