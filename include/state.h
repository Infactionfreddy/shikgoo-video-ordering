#pragma once
#include <pthread.h>
#include "types.h"
#include "conn.h"

/* g_lock:  orders, calls, kellner-status, call_session
 * ws_lock: die ws-client-liste
 * nie beide zusammen halten, sonst deadlock */
extern pthread_mutex_t g_lock;
extern pthread_mutex_t ws_lock;

/* alles im ram, die json-files sind nur fuer restarts */
extern Order    orders[MAX_ORDERS];
extern int      order_count;
extern int      next_order_id;   /* läuft nur hoch, ids werden nie recycelt */
extern MenuItem menu[MAX_MENU];
extern int      menu_count;
extern int      waiter_busy;     /* 1 = kellner ist beschaeftigt */
extern WsClient    ws_clients[MAX_WS];
extern int         ws_count;
extern CallSession g_call_session; /* nur ein gleichzeitiger videocall */
extern WaiterCall calls[MAX_CALLS];
extern int        call_count;
extern int        next_call_id;  /* wie next_order_id, monoton */
