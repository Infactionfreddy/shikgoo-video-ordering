#pragma once
#include "conn.h"
#include "state.h"

/* ws_add gibt -1 wenn schon MAX_WS clients dran sind */
int  ws_add      (Conn *c, int table_number, int is_waiter);
void ws_remove   (Conn *c);
void ws_send_to  (Conn *c, const char *msg); /* an genau einen client (z.b. call-signaling) */
void ws_broadcast(const char *msg);
int  ws_handshake(Conn *c, const char *key); /* schickt die 101-antwort auf den Sec-WebSocket-Key */
void ws_loop     (Conn *c); /* blockiert bis verbindung getrennt wird */
int  ws_find_waiter(void);           /* kellner-fd oder -1, ws_lock muss gehalten sein */
int  ws_find_table (int table_number); /* tisch-fd oder -1, dito ws_lock */
