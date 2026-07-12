#pragma once
// #include "event_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DIAG_BIT_OK       (1 << 0)
#define DIAG_BIT_TIMEOUT  (1 << 1)
#define DIAG_BIT_LAN_IP   (1 << 2)
#define DIAG_BIT_LAN_PING (1 << 3)
#define DIAG_BIT_WAN      (1 << 4)
#define DIAG_BIT_DNS      (1 << 5)

/**
 * timestamp: timestamp dell'evento.
 * error_mask: flag del tipo di evento:
 * - 100000 -> (bit0) partenza OK
 * - 110000 -> (bit1) tempo trascorso dall'ultimo ciclo > 10 sec (o valore impostato).
 * - 101000 -> (bit2) Problemi LAN1, recupero IP fallito.
 * - 100100 -> (bit3) Problemi LAN2, ping verso router timeout.
 * - 100010 -> (bit2) Problemi WAN, ping verso IP definito in internet timeout (es: 8.8.8.8).
 * - 100001 -> (bit5) Problemi DNS, ping verso indirizzo web / URL definito in internet timeout (es: google.com).
 */
typedef struct {
    time_t timestamp;        // long int
    uint8_t  error_mask;     // Bitfield (es: bit0=OK, bit1=timeout, bit2=LAN, bit3=LAN, bit4=WAN, bit5=DNS) // mettere spiegazione.
} DiagnosisEntry;


bool diag_append(DiagnosisEntry diagnosisEntry);

// Estrae l'i-esimo elemento dal più vecchio (0) al più recente (count-1)
bool diag_extract(uint16_t index, DiagnosisEntry *out_entry);

void diag_clear(void);

uint16_t diag_get_count(void);

