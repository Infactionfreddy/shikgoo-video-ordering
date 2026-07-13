#pragma once
#include "state.h"
#include "menu.h"

/* write-through: nach jeder mutation save aufrufen, sonst ist der stand nach restart weg.
 * load nur einmal beim start */
int  load_orders(void); /* anzahl wiederhergestellter orders */
void save_orders(void);
int  load_calls(void);
void save_calls(void);
