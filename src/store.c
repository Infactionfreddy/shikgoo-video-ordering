/* write-through: jede mutation gleich auf disk.
 * kein wal/journal, ein crash kann offene orders verlieren, ist ok */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "store.h"

#define ORDERS_FILE "orders.json"

/* bezahlte sind in logic.c schon aus dem array, hier nicht nochmal filtern */
void save_orders(void) {
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < order_count; i++) {
        const Order *o = &orders[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id",            o->id);
        cJSON_AddNumberToObject(obj, "table_number",  o->table_number);
        cJSON_AddStringToObject(obj, "status",        STATUS_STR[o->status]);
        cJSON_AddNumberToObject(obj, "timestamp",     o->timestamp);
        cJSON_AddNumberToObject(obj, "item_count",    o->item_count);
        cJSON_AddStringToObject(obj, "customer_name", o->customer_name);
        cJSON_AddStringToObject(obj, "notes",         o->notes);
        cJSON *items = cJSON_AddArrayToObject(obj, "items");
        for (int j = 0; j < o->item_count; j++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddNumberToObject(it, "menu_item_id", o->items[j].menu_item_id);
            cJSON_AddNumberToObject(it, "quantity",     o->items[j].quantity);
            cJSON_AddItemToArray(items, it);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    pthread_mutex_unlock(&g_lock);
    /* lock nur ums in-ram kopieren, arr ist danach ne eigene kopie.
     * der write laeuft bewusst ohne lock, disk-io ist lahm und soll keinen request blocken */
    char *body = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (body) {
        FILE *f = fopen(ORDERS_FILE, "w");
        if (f) { fputs(body, f); fclose(f); }
        cJSON_free(body);
    }
}

/* nur beim start. gibt 0 wenn die datei noch nicht da ist (frischer start) */
int load_orders(void) {
    FILE *f = fopen(ORDERS_FILE, "r");
    if (!f) return 0;
    /* ganze datei in einen buffer (ftell size). gleiche masche in load_calls und load_menu */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return 0; }

    cJSON *elem;
    cJSON_ArrayForEach(elem, root) {
        if (order_count >= MAX_ORDERS) break;
        cJSON *id_j = cJSON_GetObjectItemCaseSensitive(elem, "id");
        int id = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : 0;
        if (id <= 0) continue; /* kaputte eintraege skippen statt abbrechen */

        Order *o = &orders[order_count];
        memset(o, 0, sizeof(*o));
        o->id = id;

        cJSON *table_j = cJSON_GetObjectItemCaseSensitive(elem, "table_number");
        cJSON *count_j = cJSON_GetObjectItemCaseSensitive(elem, "item_count");
        cJSON *ts_j    = cJSON_GetObjectItemCaseSensitive(elem, "timestamp");
        cJSON *st_j    = cJSON_GetObjectItemCaseSensitive(elem, "status");
        cJSON *cname_j = cJSON_GetObjectItemCaseSensitive(elem, "customer_name");
        cJSON *notes_j = cJSON_GetObjectItemCaseSensitive(elem, "notes");
        cJSON *items_j = cJSON_GetObjectItemCaseSensitive(elem, "items");

        /* defensiv typ-checken, die json auf disk koennte von hand editiert oder kaputt sein */
        o->table_number = cJSON_IsNumber(table_j) ? (int)table_j->valuedouble   : 0;
        o->item_count   = cJSON_IsNumber(count_j) ? (int)count_j->valuedouble   : 0;
        o->timestamp    = cJSON_IsNumber(ts_j)    ? (long)ts_j->valuedouble     : 0;

        if (cJSON_IsString(st_j)) {
            int st = status_idx(st_j->valuestring);
            o->status = st >= 0 ? (Status)st : ST_PENDING; /* unbekannter status -> pending */
        }
        if (cJSON_IsString(cname_j))
            strncpy(o->customer_name, cname_j->valuestring, sizeof(o->customer_name) - 1);
        if (cJSON_IsString(notes_j))
            strncpy(o->notes, notes_j->valuestring, sizeof(o->notes) - 1);

        if (cJSON_IsArray(items_j)) {
            int n = cJSON_GetArraySize(items_j);
            if (n > 20) n = 20; /* items[] ist fix 20 gross, rest abschneiden sonst overflow */
            for (int i = 0; i < n; i++) {
                cJSON *it    = cJSON_GetArrayItem(items_j, i);
                cJSON *mid_j = cJSON_GetObjectItemCaseSensitive(it, "menu_item_id");
                cJSON *qty_j = cJSON_GetObjectItemCaseSensitive(it, "quantity");
                o->items[i].menu_item_id = cJSON_IsNumber(mid_j) ? (int)mid_j->valuedouble : 0;
                o->items[i].quantity     = cJSON_IsNumber(qty_j) ? (int)qty_j->valuedouble : 0;
            }
        }

        /* next_order_id nachziehen, sonst id-kollisionen nach nem restart */
        if (o->id >= next_order_id) next_order_id = o->id + 1;
        order_count++;
    }
    cJSON_Delete(root);
    return order_count;
}

#define CALLS_FILE "calls.json"

/* resolved calls nicht mitspeichern, nach restart eh irrelevant */
void save_calls(void) {
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < call_count; i++) {
        if (calls[i].resolved) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id",           calls[i].id);
        cJSON_AddNumberToObject(obj, "table_number", calls[i].table_number);
        cJSON_AddNumberToObject(obj, "timestamp",    calls[i].timestamp);
        cJSON_AddItemToArray(arr, obj);
    }
    pthread_mutex_unlock(&g_lock);
    char *body = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (body) {
        FILE *f = fopen(CALLS_FILE, "w");
        if (f) { fputs(body, f); fclose(f); }
        cJSON_free(body);
    }
}

/* nur beim start. die datei hat eh nur offene calls drin */
int load_calls(void) {
    FILE *f = fopen(CALLS_FILE, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return 0; }

    cJSON *elem;
    cJSON_ArrayForEach(elem, root) {
        if (call_count >= MAX_CALLS) break;
        cJSON *id_j = cJSON_GetObjectItemCaseSensitive(elem, "id");
        int id = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : 0;
        if (id <= 0) continue;

        WaiterCall *c = &calls[call_count];
        memset(c, 0, sizeof(*c));
        c->id = id;
        cJSON *table_j = cJSON_GetObjectItemCaseSensitive(elem, "table_number");
        cJSON *ts_j    = cJSON_GetObjectItemCaseSensitive(elem, "timestamp");
        c->table_number = cJSON_IsNumber(table_j) ? (int)table_j->valuedouble : 0;
        c->timestamp    = cJSON_IsNumber(ts_j)    ? (long)ts_j->valuedouble   : 0;
        c->resolved     = 0; /* datei hat nur offene, also immer 0 */
        if (c->id >= next_call_id) next_call_id = c->id + 1;
        call_count++;
    }
    cJSON_Delete(root);
    return call_count;
}
