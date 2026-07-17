// event_log.c
#include "event_log.h"
#include <stdint.h>

#define DIAG_MAX_ENTRIES 64  // Scegli una potenza di 2 se vuoi ottimizzare

/* Registro circolare residente in RAM: head è la prossima cella da scrivere,
 * count è il numero di record validi (mai superiore a DIAG_MAX_ENTRIES). */
static DiagnosisEntry g_diag_buffer[DIAG_MAX_ENTRIES];
static uint16_t g_diag_head = 0;
static uint16_t g_diag_count = 0;

bool diag_append(DiagnosisEntry diagnosisEntry) {
    /* Il valore di ritorno informa il chiamante che l'inserimento ha causato
     * la perdita del record più vecchio. */
    bool _overwriting = false;

    // Se il buffer è già pieno, significa che stiamo per sovrascrivere 
    // l'elemento più vecchio (che non è ancora stato estratto).
    if (g_diag_count == DIAG_MAX_ENTRIES) {
        _overwriting = true;
    }

    g_diag_buffer[g_diag_head].timestamp = diagnosisEntry.timestamp;
    g_diag_buffer[g_diag_head].error_mask = diagnosisEntry.error_mask;

    // Avanza l'indice
    g_diag_head = (g_diag_head + 1) % DIAG_MAX_ENTRIES;

    // Se non siamo al massimo, incrementiamo il contatore.
    // Se eravamo al massimo, il contatore resta fisso al valore MAX.
    if (g_diag_count < DIAG_MAX_ENTRIES) {
        g_diag_count++;
    }

    return _overwriting;
}

// Estrae l'i-esimo elemento dal più vecchio (0) al più recente (count-1)
bool diag_extract(uint16_t index, DiagnosisEntry *out_entry) {
    /* Un indice fuori dall'intervallo non identifica alcun record valido. */
    if (index >= g_diag_count) return false;

    // Calcola la posizione reale partendo dal più vecchio
    // Il più vecchio è (head - count + size) % size
    uint16_t _oldest_idx = (g_diag_head + DIAG_MAX_ENTRIES - g_diag_count) % DIAG_MAX_ENTRIES;
    uint16_t _target_idx = (_oldest_idx + index) % DIAG_MAX_ENTRIES;

    *out_entry = g_diag_buffer[_target_idx];
    return true;
}

void diag_clear(void) {
    // Basta azzerare la testa e il contatore. I vecchi dati rimasti
    // in memoria verranno sovrascritti al prossimo diag_append.
    g_diag_head = 0;
    g_diag_count = 0;
}

uint16_t diag_get_count(void) {
    // Espone il contatore senza rivelare al chiamante la disposizione circolare.
    return g_diag_count;
}
