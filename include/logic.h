#pragma once
#include <stddef.h>
#include "state.h"
#include "cJSON.h"

/* fehlercodes, alle negativ um sie von positiven ids zu unterscheiden */
#define LOGIC_INVALID  (-1)
#define LOGIC_CAPACITY (-2) /* array voll (MAX_ORDERS / MAX_CALLS erreicht) */
#define LOGIC_NOTFOUND (-3)
#define LOGIC_BACKWARD (-4) /* status nur vorwaerts, rueckwaerts wird abgelehnt */

typedef struct { int menu_item_id; int quantity; } LogicOrderItem;

/* lesen, aufrufer muss danach cJSON_Delete() machen */
cJSON *logic_menu_items(void);
cJSON *logic_order_detail(int oid);
cJSON *logic_active_order_for_table(int table);
cJSON *logic_queue_orders(void);
cJSON *logic_active_calls(void);

/* schreiben. bei erfolg wird evt_out mit dem ws_broadcast-payload gefuellt,
 * damit der aufrufer das event nach dem unlock senden kann */
int  logic_place_order(int table, int n, const LogicOrderItem *items,
                       const char *cname, const char *notes,
                       char *evt_out, size_t evt_cap);
int  logic_update_order(int oid, int new_st, char *evt_out, size_t evt_cap);
int  logic_call_waiter(int table, int *is_existing, char *evt_out, size_t evt_cap); /* is_existing=1 wenn tisch schon offenen ruf hat */
int  logic_resolve_call(int cid, char *evt_out, size_t evt_cap);
void logic_set_availability(int busy, char *evt_out, size_t evt_cap);
