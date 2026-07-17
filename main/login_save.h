#ifndef LOGIN_SAVE_H
#define LOGIN_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGIN_SAVE_CAPACITY       10U
#define WIFI_SSID_BUFFER_SIZE     32U
#define WIFI_PASSWORD_BUFFER_SIZE 64U
#define LOGIN_SAVE_NOT_FOUND      (-1)

typedef struct {
    char ssid[WIFI_SSID_BUFFER_SIZE];
    char password[WIFI_PASSWORD_BUFFER_SIZE];
} wifi_login_t;

typedef struct {
    wifi_login_t entries[LOGIN_SAVE_CAPACITY];
    size_t head;
    size_t count;
    int32_t current_ixp;
} wifi_login_store_t;

/** Initializes an empty login store with no active login. */
void login_save_init(wifi_login_store_t *store);

/**
 * Adds a login as the newest element.
 *
 * When the buffer is full, the oldest element is overwritten.
 * The strings stored in the buffer are always NUL-terminated.
 *
 * @return true when the login was added, false if login is NULL.
 */
bool login_save_add(wifi_login_store_t *store, const wifi_login_t *login);

/** Returns the number of logins currently stored. */
size_t login_save_length(const wifi_login_store_t *store);

/**
 * Searches for an SSID, starting from the oldest element.
 *
 * @return its logical index, or LOGIN_SAVE_NOT_FOUND when it is absent or
 *         ssid is NULL.
 */
int32_t login_save_find(const wifi_login_store_t *store, const char *ssid);

/**
 * Copies the login at logical index ixp into out_login.
 * Index zero always identifies the oldest element.
 *
 * @return true on success, false for an invalid index or NULL output pointer.
 */
bool login_save_get(const wifi_login_store_t *store, size_t ixp, wifi_login_t *out_login);

/** Selects the login at logical index ixp as the active Wi-Fi login. */
bool login_save_set_current(wifi_login_store_t *store, size_t ixp);

/** Copies the currently selected login into out_login. */
bool login_save_get_current(const wifi_login_store_t *store, wifi_login_t *out_login);

/**
 * Removes the login at logical index ixp and compacts subsequent elements.
 *
 * @return true on success, false if ixp is outside the current queue.
 */
bool login_save_remove(wifi_login_store_t *store, size_t ixp);

#endif // LOGIN_SAVE_H
