#ifndef SYSTEM_UTILS_H
#define SYSTEM_UTILS_H

#ifdef __cplusplus
/* Mantiene il nome C della funzione se l'header viene incluso da codice C++. */
extern "C" {
#endif

/**
 * @brief Initialize system utilities.
 *
 * Punto di estensione per inizializzazioni comuni a più applicazioni del
 * template; attualmente registra soltanto l'avvenuta inizializzazione.
 */
void system_utils_init(void);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_UTILS_H
