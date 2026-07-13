#pragma once
#include "state.h"

int load_menu(void);           /* laedt menu.json, gibt item-anzahl zurueck */
int menu_exists(int id);       /* menu_item_id gueltig? gegen fake-bestellungen */
int status_idx(const char *s); /* status-string zu enum-index, -1 bei unbekannt */
