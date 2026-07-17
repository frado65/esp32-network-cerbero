#ifndef NETWORK_CHECK_H
#define NETWORK_CHECK_H

#include <stdbool.h>

#include "lwip/ip_addr.h"

/* Esegue un singolo ping ICMP e attende l'esito con timeout. */
bool perform_ping(const ip_addr_t *target_ip);

/* Legge dalla netif Station l'indirizzo IPv4 del gateway assegnato via DHCP. */
bool get_router_ip(ip_addr_t *target_ip);

/* Risolve un hostname IPv4 e verifica con un ping che sia raggiungibile. */
bool perform_dns_check(const char *target_host);

#endif // NETWORK_CHECK_H
