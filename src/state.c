#include <pthread.h>
#include "state.h"

/* reihenfolge MUSS 1:1 zum STATUS enum in state.h passen, index ist der status-wert.
 * wird direkt als STATUS_STR[status] rausgeschrieben, verschieben = kaputt */
const char * const STATUS_STR[7] = {
    "pending", "accepted", "preparing", "ready", "delivered", "bill", "paid"
};

/* zwei locks mit absicht: g_lock für orders/calls/menu, ws_lock nur die
 * verbindungsliste. sonst würde broadcast den datenpfad blockieren */
pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ws_lock = PTHREAD_MUTEX_INITIALIZER;

/* keine db, alles im ram. persist läuft nur über die save/load-funktionen nach json */
Order    orders[MAX_ORDERS];      /* orders/calls/menu/waiter_busy: immer unter g_lock anfassen */
int      order_count   = 0;
int      next_order_id = 1;
MenuItem menu[MAX_MENU];
int      menu_count    = 0;
int      waiter_busy   = 0;
WsClient    ws_clients[MAX_WS];   /* ACHTUNG: das hier läuft unter ws_lock, nicht g_lock */
int         ws_count       = 0;
CallSession g_call_session = {0}; /* nur genau ein videocall gleichzeitig, mehr braucht's im lan nicht */
WaiterCall calls[MAX_CALLS];
int        call_count    = 0;
int        next_call_id  = 1;
