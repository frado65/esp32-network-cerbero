#pragma once
// event_log.h

typedef struct {
    time_t timestamp;        // 
    uint8_t  error_mask;     // Bitfield (es: bit0=OK, bit1=timeout, bit2=LAN, bit3=LAN, bit4=WAN, bit5=DNS) // mettere spiegazione.
} DiagnosisEntry;

