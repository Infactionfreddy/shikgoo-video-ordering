#include <string.h>
#include <time.h>
#include <stdio.h>
#include "logic.h"
#include "menu.h"
#include "store.h"

/* single source of truth für orders/calls/menu. api.c und ws.c sind nur wrapper,
 * die eigentliche arbeit (locking + persist) sitzt hier an einer stelle. */

/* lesepfad: snapshot unterm g_lock bauen, nix mutieren.
 * rückgabe ist frisches cJSON, caller macht cJSON_Delete */

/* lock hier eigentlich unnötig (menü kommt nur beim start), aber falls mal
 * hot-reload dazukommt ist der lesepfad schon safe */
cJSON *logic_menu_items(void) {
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < menu_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id",          menu[i].id);
        cJSON_AddStringToObject(item, "name",        menu[i].name);
        cJSON_AddNumberToObject(item, "price",       menu[i].price);
        cJSON_AddStringToObject(item, "category",    menu[i].category);
        cJSON_AddStringToObject(item, "description", menu[i].description);
        cJSON_AddStringToObject(item, "image_url",   ""); /* keine bild-url vom backend, frontend hat eigene assets */
        cJSON_AddItemToArray(arr, item);
    }
    pthread_mutex_unlock(&g_lock);
    return arr;
}

/* eine order komplett mit items. NULL wenn oid nicht existiert */
cJSON *logic_order_detail(int oid) {
    cJSON *obj = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < order_count; i++) {
        if (orders[i].id != oid) continue;
        obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id",            orders[i].id);
        cJSON_AddNumberToObject(obj, "table_number",  orders[i].table_number);
        cJSON_AddStringToObject(obj, "status",        STATUS_STR[orders[i].status]);
        cJSON_AddNumberToObject(obj, "timestamp",     orders[i].timestamp);
        cJSON_AddNumberToObject(obj, "item_count",    orders[i].item_count);
        cJSON_AddStringToObject(obj, "customer_name", orders[i].customer_name);
        cJSON_AddStringToObject(obj, "notes",         orders[i].notes);
        cJSON *items = cJSON_AddArrayToObject(obj, "items");
        for (int j = 0; j < orders[i].item_count; j++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddNumberToObject(it, "menu_item_id", orders[i].items[j].menu_item_id);
            cJSON_AddNumberToObject(it, "quantity",     orders[i].items[j].quantity);
            cJSON_AddItemToArray(items, it);
        }
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return obj;
}

/* aktive order fürn tisch, von hinten damit man die neueste offene erwischt.
 * NULL wenn grad nix offen */
cJSON *logic_active_order_for_table(int table) {
    cJSON *obj = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = order_count - 1; i >= 0; i--) {
        if (orders[i].table_number != table) continue;
        if (orders[i].status == ST_PAID) continue;
        obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id",            orders[i].id);
        cJSON_AddNumberToObject(obj, "table_number",  orders[i].table_number);
        cJSON_AddStringToObject(obj, "status",        STATUS_STR[orders[i].status]);
        cJSON_AddNumberToObject(obj, "timestamp",     orders[i].timestamp);
        cJSON_AddNumberToObject(obj, "item_count",    orders[i].item_count);
        cJSON_AddStringToObject(obj, "customer_name", orders[i].customer_name);
        cJSON_AddStringToObject(obj, "notes",         orders[i].notes);
        cJSON *items = cJSON_AddArrayToObject(obj, "items");
        for (int j = 0; j < orders[i].item_count; j++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddNumberToObject(it, "menu_item_id", orders[i].items[j].menu_item_id);
            cJSON_AddNumberToObject(it, "quantity",     orders[i].items[j].quantity);
            cJSON_AddItemToArray(items, it);
        }
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return obj;
}

/* kellner-dashboard, alles außer paid. paid fliegt eh schon in logic_update_order
 * aus dem array, kann hier also nicht mehr auftauchen */
cJSON *logic_queue_orders(void) {
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < order_count; i++) {
        if (orders[i].status == ST_PAID) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id",            orders[i].id);
        cJSON_AddNumberToObject(item, "table_number",  orders[i].table_number);
        cJSON_AddStringToObject(item, "status",        STATUS_STR[orders[i].status]);
        cJSON_AddNumberToObject(item, "timestamp",     orders[i].timestamp);
        cJSON_AddNumberToObject(item, "item_count",    orders[i].item_count);
        cJSON_AddStringToObject(item, "customer_name", orders[i].customer_name);
        cJSON_AddItemToArray(arr, item);
    }
    pthread_mutex_unlock(&g_lock);
    return arr;
}

/* nur offene calls. resolved bleibt im array (history), klingelt aber nicht mehr */
cJSON *logic_active_calls(void) {
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < call_count; i++) {
        if (calls[i].resolved) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id",           calls[i].id);
        cJSON_AddNumberToObject(item, "table_number", calls[i].table_number);
        cJSON_AddNumberToObject(item, "timestamp",    calls[i].timestamp);
        cJSON_AddItemToArray(arr, item);
    }
    pthread_mutex_unlock(&g_lock);
    return arr;
}

/* schreibpfad: validieren ohne lock, dann kurz unterm lock eintragen.
 * save immer erst nach dem unlock, sonst hält datei-io den g_lock fest */

/* neue order. evt_out kriegt das new_order-json fürs broadcast zurück */
int logic_place_order(int table, int n, const LogicOrderItem *items,
                      const char *cname, const char *notes,
                      char *evt_out, size_t evt_cap) {
    /* vorab checken, noch ohne lock. max 20 tische, max 20 items */
    if (table < 1 || table > 20 || n < 1 || n > 20) return LOGIC_INVALID;
    for (int i = 0; i < n; i++) {
        if (items[i].menu_item_id <= 0 || items[i].quantity < 1 ||
            items[i].quantity > 99 || !menu_exists(items[i].menu_item_id))
            return LOGIC_INVALID;
    }

    pthread_mutex_lock(&g_lock);
    if (order_count >= MAX_ORDERS) {
        pthread_mutex_unlock(&g_lock); /* voll, unlock vor return nicht vergessen */
        return LOGIC_CAPACITY;
    }
    Order *o = &orders[order_count];
    memset(o, 0, sizeof(*o));
    o->id           = next_order_id++;
    o->table_number = table;
    o->status       = ST_PENDING;
    o->timestamp    = (long)time(NULL);
    o->item_count   = n;
    for (int i = 0; i < n; i++) {
        o->items[i].menu_item_id = items[i].menu_item_id;
        o->items[i].quantity     = items[i].quantity;
    }
    /* size-1: struct ist eh genullt, terminator bleibt also stehen */
    if (cname) strncpy(o->customer_name, cname, sizeof(o->customer_name) - 1);
    if (notes) strncpy(o->notes,         notes, sizeof(o->notes)         - 1);
    order_count++;
    int oid = o->id; /* id noch unterm lock greifen, o kann danach weg sein */
    pthread_mutex_unlock(&g_lock);

    save_orders();
    if (evt_out && evt_cap)
        snprintf(evt_out, evt_cap,
                 "{\"type\":\"new_order\",\"order_id\":%d,\"table_number\":%d,\"status\":\"pending\"}",
                 oid, table);
    return oid;
}

/* status nur vorwärts (pending ... paid), rückwärts gibt LOGIC_BACKWARD.
 * paid ist endstation, order fliegt dann direkt raus */
int logic_update_order(int oid, int new_st, char *evt_out, size_t evt_cap) {
    if (oid <= 0 || new_st < 0) return LOGIC_INVALID;

    int result = LOGIC_NOTFOUND;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < order_count; i++) {
        if (orders[i].id != oid) continue;
        if (new_st <= (int)orders[i].status) {
            pthread_mutex_unlock(&g_lock); /* rückwärts oder gleich = no-op */
            return LOGIC_BACKWARD;
        }
        orders[i].status = (Status)new_st;
        if (evt_out && evt_cap)
            snprintf(evt_out, evt_cap,
                     "{\"type\":\"order_update\",\"order_id\":%d,\"status\":\"%s\"}",
                     oid, STATUS_STR[new_st]);
        result = new_st;
        if ((Status)new_st == ST_PAID) {
            /* bezahlt = weg. loch mit memmove zuschieben, array bleibt dicht.
             * len ist (count-i-1), beim letzten element also 0 und kein copy */
            memmove(&orders[i], &orders[i + 1],
                    sizeof(Order) * (size_t)(order_count - i - 1));
            order_count--;
        }
        break;
    }
    pthread_mutex_unlock(&g_lock);

    if (result >= 0) save_orders();
    return result;
}

/* idempotent: klickt ein tisch zweimal, kriegt er denselben call zurück statt einem zweiten.
 * is_existing sagt dem caller welcher fall */
int logic_call_waiter(int table, int *is_existing, char *evt_out, size_t evt_cap) {
    if (table < 1 || table > 20) return LOGIC_INVALID;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < call_count; i++) {
        if (!calls[i].resolved && calls[i].table_number == table) {
            int existing = calls[i].id;
            pthread_mutex_unlock(&g_lock);
            if (is_existing) *is_existing = 1;
            return existing; /* tisch hat schon einen offenen call, kein neuer, kein event */
        }
    }
    if (is_existing) *is_existing = 0;
    if (call_count >= MAX_CALLS) {
        pthread_mutex_unlock(&g_lock);
        return LOGIC_CAPACITY;
    }
    WaiterCall *c = &calls[call_count];
    memset(c, 0, sizeof(*c));
    c->id           = next_call_id++;
    c->table_number = table;
    c->timestamp    = (long)time(NULL);
    c->resolved     = 0;
    int cid         = c->id;
    call_count++;
    pthread_mutex_unlock(&g_lock);

    save_calls();
    if (evt_out && evt_cap)
        snprintf(evt_out, evt_cap,
                 "{\"type\":\"call_waiter\",\"call_id\":%d,\"table_number\":%d}", cid, table);
    return cid;
}

/* call abhaken: nur resolved setzen, eintrag bleibt fürs log.
 * return ist die table_number (>=0), damit der caller weiß welcher tisch */
int logic_resolve_call(int cid, char *evt_out, size_t evt_cap) {
    if (cid <= 0) return LOGIC_INVALID;

    int table = LOGIC_NOTFOUND;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < call_count; i++) {
        if (calls[i].id == cid && !calls[i].resolved) {
            calls[i].resolved = 1;
            table = calls[i].table_number;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (table < 0) return LOGIC_NOTFOUND; /* cid gabs nicht oder war schon resolved */
    save_calls();
    if (evt_out && evt_cap)
        snprintf(evt_out, evt_cap,
                 "{\"type\":\"call_resolved\",\"call_id\":%d,\"table_number\":%d}", cid, table);
    return table;
}

/* nur flag + event, kein save. waiter_busy ist laufzeit-state und darf beim
 * neustart ruhig wieder 0 sein */
void logic_set_availability(int busy, char *evt_out, size_t evt_cap) {
    pthread_mutex_lock(&g_lock);
    waiter_busy = busy;
    pthread_mutex_unlock(&g_lock);
    if (evt_out && evt_cap)
        snprintf(evt_out, evt_cap,
                 "{\"type\":\"waiter_status\",\"busy\":%s}", busy ? "true" : "false");
}
