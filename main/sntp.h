#pragma once

#include <time.h>

/* Avvia il client SNTP e configura il fuso orario italiano. */
void initialize_sntp(const char *tag);

/* Aggiorna p_now con l'ora corrente e la scrive nel log. */
void print_time(const char *tag, time_t *p_now);
