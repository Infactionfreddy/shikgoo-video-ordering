#pragma once
#include "conn.h"
#include <pthread.h>

/* harte limits, die arrays unten sind alle static in der groesse */
#define PORT         8443  /* https, nicht 443 um root zu vermeiden */
#define MAX_ORDERS   1000
#define MAX_MENU     100
#define MAX_WS       64    /* max gleichzeitige websocket-verbindungen */
#define MAX_CALLS    200
#define RECV_BUF     8192  /* ein request muss reinpassen, reicht dicke fuers lan */
#define FRONTEND_DIR "frontend"  /* static files, relativ zum cwd */

typedef enum { ST_PENDING, ST_ACCEPTED, ST_PREPARING, ST_READY, ST_DELIVERED, ST_BILL, ST_PAID } Status; /* order-lifecycle, geht nur vorwaerts (siehe LOGIC_BACKWARD) */

extern const char * const STATUS_STR[7]; /* gleiche reihenfolge wie das enum, index = status */

typedef struct {
    int  id, price;         /* preis in cent */
    char name[64], category[32], description[128];
} MenuItem;

typedef struct {
    int    id, table_number, item_count; /* item_count = belegte einträge in items[] */
    char   customer_name[64], notes[256];
    Status status;
    long   timestamp;
    struct { int menu_item_id, quantity; } items[20]; /* max 20 items pro bestellung */
} Order;

typedef struct {
    int  id, table_number;
    long timestamp;
    int  resolved;      /* 1 = kellner hat den ruf abgehakt */
} WaiterCall;

typedef struct { char *d; size_t len, cap; } Buf; /* dyn string-buffer, cap = alloziert, len = benutzt */

typedef struct {
    Conn             conn;         /* muss erstes feld sein, wird in ws_add() als Conn* gecastet */
    int              table_number; /* 0 = kellner-dashboard, 1-20 = kunden-tisch */
    int              is_waiter;
    pthread_mutex_t  send_lock;    /* kein paralleles SSL_write aus mehreren threads */
} WsClient;

typedef struct {
    int  active;       /* 1 = call laeuft grad */
    int  pending;      /* ring unterwegs, blockt neue rings bis accept/reject */
    int  caller_table;
    int  waiter_fd;    /* die 2 sockets im call, zum gezielten relayen */
    int  customer_fd;
    long started_at;   /* unix ts, fuer logs */
} CallSession;
