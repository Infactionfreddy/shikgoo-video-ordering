/* menu.json einmal beim start. kein hot-reload, aenderungen erst nach neustart */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "menu.h"

int load_menu(void) {
    FILE *f = fopen("menu.json", "r");
    if (!f) return -1; /* ohne menu.json kein sinn, start soll hart failen */
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
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return -1; }

    cJSON *elem;
    cJSON_ArrayForEach(elem, root) {
        if (menu_count >= MAX_MENU) break; /* wenn voll, rest ignorieren */
        cJSON *id_j   = cJSON_GetObjectItemCaseSensitive(elem, "id");
        int id = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : 0;
        if (id <= 0) continue;

        MenuItem *m = &menu[menu_count];
        m->id    = id;
        cJSON *price_j = cJSON_GetObjectItemCaseSensitive(elem, "price");
        cJSON *name_j  = cJSON_GetObjectItemCaseSensitive(elem, "name");
        cJSON *cat_j   = cJSON_GetObjectItemCaseSensitive(elem, "category");
        cJSON *desc_j  = cJSON_GetObjectItemCaseSensitive(elem, "description");
        /* preis als int (cent), kein float wegen rundung beim summieren */
        m->price = cJSON_IsNumber(price_j) ? (int)price_j->valuedouble : 0;
        if (cJSON_IsString(name_j))  strncpy(m->name,        name_j->valuestring,  sizeof(m->name)        - 1);
        if (cJSON_IsString(cat_j))   strncpy(m->category,    cat_j->valuestring,   sizeof(m->category)    - 1);
        if (cJSON_IsString(desc_j))  strncpy(m->description, desc_j->valuestring,  sizeof(m->description) - 1);
        menu_count++;
    }
    cJSON_Delete(root);
    return menu_count;
}

/* lineare suche reicht, menu ist klein */
int menu_exists(int id) {
    for (int i = 0; i < menu_count; i++)
        if (menu[i].id == id) return 1;
    return 0;
}

/* status-string zu enum-index, -1 bei unbekannt */
int status_idx(const char *s) {
    /* 7 = anzahl STATUS_STR, muss mit dem Status-enum sync sein */
    for (int i = 0; i < 7; i++)
        if (strcmp(STATUS_STR[i], s) == 0) return i;
    return -1;
}
