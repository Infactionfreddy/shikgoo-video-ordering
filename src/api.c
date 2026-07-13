/* http-json-api, dünne debug-schicht über logic.c (gut für curl-tests).
 * pro handler immer gleich: body parsen, logic aufrufen, json zurück.
 * logik + locking steckt in logic.c, hier bewusst nicht.
 * ws_broadcast erst nach send_resp, wenn andere clients was mitkriegen müssen. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "conn.h"
#include "api.h"
#include "logic.h"
#include "menu.h"

/* menü nur beim start geladen, kein hot-reload. also einfach snapshot raus */
void api_get_menu(Conn *c) {
    cJSON *arr = logic_menu_items();
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!body) { json_resp(c, 500, "{\"error\":\"Out of memory\"}"); return; }
    send_resp(c, 200, "application/json", body, -1);
    cJSON_free(body);
}

void api_post_order(Conn *c, const char *body_str) {
    cJSON *body = cJSON_Parse(body_str);
    if (!body) { json_resp(c, 400, "{\"error\":\"Invalid JSON\"}"); return; }

    cJSON *table_j = cJSON_GetObjectItemCaseSensitive(body, "table_number");
    int table = cJSON_IsNumber(table_j) ? (int)table_j->valuedouble : -1;

    cJSON *items_j = cJSON_GetObjectItemCaseSensitive(body, "items");
    int n = cJSON_IsArray(items_j) ? cJSON_GetArraySize(items_j) : 0;
    /* n hier clampen, vi[20] liegt aufm stack. sonst overflow beim kopieren unten */
    if (n < 1 || n > 20) {
        cJSON_Delete(body);
        json_resp(c, 400, "{\"error\":\"items must be a non-empty array (max 20)\"}");
        return;
    }

    LogicOrderItem vi[20];
    for (int i = 0; i < n; i++) {
        cJSON *elem  = cJSON_GetArrayItem(items_j, i);
        cJSON *mid_j = cJSON_GetObjectItemCaseSensitive(elem, "menu_item_id");
        cJSON *qty_j = cJSON_GetObjectItemCaseSensitive(elem, "quantity");
        /* fehlt oder falscher typ = -1, den rest validiert logic_place_order */
        vi[i].menu_item_id = cJSON_IsNumber(mid_j) ? (int)mid_j->valuedouble : -1;
        vi[i].quantity     = cJSON_IsNumber(qty_j) ? (int)qty_j->valuedouble : -1;
    }

    cJSON *cname_j = cJSON_GetObjectItemCaseSensitive(body, "customer_name");
    cJSON *notes_j = cJSON_GetObjectItemCaseSensitive(body, "notes");
    const char *cname = cJSON_IsString(cname_j) ? cname_j->valuestring : NULL;
    const char *notes = cJSON_IsString(notes_j) ? notes_j->valuestring : NULL;

    char evt[128] = "";
    int oid = logic_place_order(table, n, vi, cname, notes, evt, sizeof(evt));
    cJSON_Delete(body); /* achtung: cname/notes zeigen jetzt ins freed body, nicht mehr anfassen */

    if (oid == LOGIC_CAPACITY) { json_resp(c, 503, "{\"error\":\"Server at capacity\"}"); return; }
    if (oid < 0)               { json_resp(c, 400, "{\"error\":\"Invalid input\"}");      return; }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"order_id\":%d,\"status\":\"pending\"}", oid);
    send_resp(c, 201, "application/json", resp, -1);
    if (evt[0]) ws_broadcast(evt); /* kellner-dashboard über die neue order wecken */
}

/* id aus dem pfad: /api/orders/42 gibt 42 (letztes segment nachm slash) */
void api_get_order(Conn *c, const char *path) {
    const char *slash = strrchr(path, '/');
    int oid = slash ? atoi(slash + 1) : 0;
    if (oid <= 0) { json_resp(c, 400, "{\"error\":\"Invalid ID\"}"); return; }
    cJSON *obj = logic_order_detail(oid);
    if (!obj)  { json_resp(c, 404, "{\"error\":\"Order not found\"}"); return; }
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    send_resp(c, 200, "application/json", out, -1);
    cJSON_free(out);
}

void api_patch_order(Conn *c, const char *path, const char *body_str) {
    const char *slash = strrchr(path, '/');
    int oid = slash ? atoi(slash + 1) : 0;
    if (oid <= 0) { json_resp(c, 400, "{\"error\":\"Invalid ID\"}"); return; }

    cJSON *body = cJSON_Parse(body_str);
    cJSON *st_j = body ? cJSON_GetObjectItemCaseSensitive(body, "status") : NULL;
    /* status kommt als string ("ready" usw), status_idx macht den enum-index draus. -1 = unbekannt */
    int new_st  = cJSON_IsString(st_j) ? status_idx(st_j->valuestring) : -1;
    cJSON_Delete(body);
    if (new_st < 0) { json_resp(c, 400, "{\"error\":\"Invalid status\"}"); return; }

    char evt[128] = "";
    int result = logic_update_order(oid, new_st, evt, sizeof(evt));

    if (result == LOGIC_BACKWARD) { json_resp(c, 400, "{\"error\":\"Status must advance forward\"}"); return; }
    if (result == LOGIC_NOTFOUND) { json_resp(c, 404, "{\"error\":\"Order not found\"}");             return; }
    if (result < 0)               { json_resp(c, 400, "{\"error\":\"Invalid request\"}");             return; }

    /* frischen stand gleich mitschicken, spart dem client ein zweites GET */
    cJSON *obj = logic_order_detail(oid);
    if (obj) {
        char *out = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        send_resp(c, 200, "application/json", out, -1);
        cJSON_free(out);
    } else {
        json_resp(c, 200, "{\"ok\":true}"); /* war paid, logic hat die order schon gelöscht, kein detail mehr da */
    }
    if (evt[0]) ws_broadcast(evt);
}

/* GET liest nur waiter_busy, POST setzt es. lock hier direkt, weil's nur ein flag ist */
void api_waiter_status(Conn *c, const char *method, const char *body_str) {
    if (strcmp(method, "GET") == 0) {
        pthread_mutex_lock(&g_lock);
        int busy = waiter_busy;
        pthread_mutex_unlock(&g_lock);
        json_resp(c, 200, "{\"busy\":%s}", busy ? "true" : "false");
        return;
    }
    cJSON *body   = cJSON_Parse(body_str);
    cJSON *busy_j = body ? cJSON_GetObjectItemCaseSensitive(body, "busy") : NULL;
    if (!cJSON_IsBool(busy_j)) {
        cJSON_Delete(body);
        json_resp(c, 400, "{\"error\":\"busy field required\"}");
        return;
    }
    int busy = cJSON_IsTrue(busy_j);
    cJSON_Delete(body);

    char evt[64] = "";
    logic_set_availability(busy, evt, sizeof(evt));
    json_resp(c, 200, "{\"ok\":true}");
    if (evt[0]) ws_broadcast(evt);
}

void api_call_waiter(Conn *c, const char *body_str) {
    cJSON *body  = cJSON_Parse(body_str);
    cJSON *tbl_j = body ? cJSON_GetObjectItemCaseSensitive(body, "table_number") : NULL;
    int table    = cJSON_IsNumber(tbl_j) ? (int)tbl_j->valuedouble : -1;
    cJSON_Delete(body);

    int is_existing = 0;
    char evt[128] = "";
    int cid = logic_call_waiter(table, &is_existing, evt, sizeof(evt));

    if (cid == LOGIC_CAPACITY) { json_resp(c, 503, "{\"error\":\"Too many active calls\"}"); return; }
    if (cid < 0)               { json_resp(c, 400, "{\"error\":\"table_number must be 1-20\"}"); return; }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"call_id\":%d}", cid);
    /* 201 wenn wir grad neu angelegt haben, 200 wenn der call schon lief (idempotent, siehe logic) */
    send_resp(c, is_existing ? 200 : 201, "application/json", resp, -1);
    if (evt[0]) ws_broadcast(evt);
}

void api_get_calls(Conn *c) {
    cJSON *arr = logic_active_calls();
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    send_resp(c, 200, "application/json", body, -1);
    cJSON_free(body);
}

void api_resolve_call(Conn *c, const char *path) {
    const char *slash = strrchr(path, '/');
    int cid = slash ? atoi(slash + 1) : 0;

    char evt[128] = "";
    int result = logic_resolve_call(cid, evt, sizeof(evt));

    /* notfound und invalid beide als 404, dem client egal welcher genau */
    if (result == LOGIC_NOTFOUND || result == LOGIC_INVALID) {
        json_resp(c, 404, "{\"error\":\"Call not found\"}");
        return;
    }
    json_resp(c, 200, "{\"ok\":true}");
    if (evt[0]) ws_broadcast(evt);
}

void api_get_queue(Conn *c) {
    cJSON *arr = logic_queue_orders();
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    send_resp(c, 200, "application/json", body, -1);
    cJSON_free(body);
}
