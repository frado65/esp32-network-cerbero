#include "login_save.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_login_lock = portMUX_INITIALIZER_UNLOCKED;

static size_t physical_index(const wifi_login_store_t *store, size_t logical_index)
{
    return (store->head + logical_index) % LOGIN_SAVE_CAPACITY;
}

static void copy_login(wifi_login_t *destination, const wifi_login_t *source)
{
    memcpy(destination, source, sizeof(*destination));
    destination->ssid[WIFI_SSID_BUFFER_SIZE - 1U] = '\0';
    destination->password[WIFI_PASSWORD_BUFFER_SIZE - 1U] = '\0';
}

void login_save_init(wifi_login_store_t *store)
{
    if (store == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_login_lock);
    memset(store, 0, sizeof(*store));
    store->current_ixp = LOGIN_SAVE_NOT_FOUND;
    portEXIT_CRITICAL(&s_login_lock);
}

bool login_save_add(wifi_login_store_t *store, const wifi_login_t *login)
{
    if (store == NULL || login == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_login_lock);

    if (store->count < LOGIN_SAVE_CAPACITY) {
        copy_login(&store->entries[physical_index(store, store->count)], login);
        ++store->count;
        if (store->current_ixp == LOGIN_SAVE_NOT_FOUND) {
            store->current_ixp = 0;
        }
    } else {
        copy_login(&store->entries[store->head], login);
        store->head = (store->head + 1U) % LOGIN_SAVE_CAPACITY;
        if (store->current_ixp == 0) {
            store->current_ixp = LOGIN_SAVE_NOT_FOUND;
        } else if (store->current_ixp > 0) {
            --store->current_ixp;
        }
    }

    portEXIT_CRITICAL(&s_login_lock);
    return true;
}

size_t login_save_length(const wifi_login_store_t *store)
{
    if (store == NULL) {
        return 0U;
    }

    portENTER_CRITICAL(&s_login_lock);
    const size_t count = store->count;
    portEXIT_CRITICAL(&s_login_lock);

    return count;
}

int32_t login_save_find(const wifi_login_store_t *store, const char *ssid)
{
    if (store == NULL || ssid == NULL) {
        return LOGIN_SAVE_NOT_FOUND;
    }

    int32_t result = LOGIN_SAVE_NOT_FOUND;

    portENTER_CRITICAL(&s_login_lock);
    for (size_t ixp = 0; ixp < store->count; ++ixp) {
        if (strcmp(store->entries[physical_index(store, ixp)].ssid, ssid) == 0) {
            result = (int32_t)ixp;
            break;
        }
    }
    portEXIT_CRITICAL(&s_login_lock);

    return result;
}

bool login_save_get(const wifi_login_store_t *store, size_t ixp, wifi_login_t *out_login)
{
    if (store == NULL || out_login == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_login_lock);
    if (ixp >= store->count) {
        portEXIT_CRITICAL(&s_login_lock);
        return false;
    }

    *out_login = store->entries[physical_index(store, ixp)];
    portEXIT_CRITICAL(&s_login_lock);
    return true;
}

bool login_save_set_current(wifi_login_store_t *store, size_t ixp)
{
    if (store == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_login_lock);
    if (ixp >= store->count) {
        portEXIT_CRITICAL(&s_login_lock);
        return false;
    }

    store->current_ixp = (int32_t)ixp;
    portEXIT_CRITICAL(&s_login_lock);
    return true;
}

bool login_save_get_current(const wifi_login_store_t *store, wifi_login_t *out_login)
{
    if (store == NULL || out_login == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_login_lock);
    if (store->current_ixp < 0 || (size_t)store->current_ixp >= store->count) {
        portEXIT_CRITICAL(&s_login_lock);
        return false;
    }

    *out_login = store->entries[physical_index(store, (size_t)store->current_ixp)];
    portEXIT_CRITICAL(&s_login_lock);
    return true;
}

bool login_save_remove(wifi_login_store_t *store, size_t ixp)
{
    if (store == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_login_lock);
    if (ixp >= store->count) {
        portEXIT_CRITICAL(&s_login_lock);
        return false;
    }

    for (size_t current = ixp; current + 1U < store->count; ++current) {
        store->entries[physical_index(store, current)] =
            store->entries[physical_index(store, current + 1U)];
    }

    memset(&store->entries[physical_index(store, store->count - 1U)], 0, sizeof(wifi_login_t));
    --store->count;

    if (store->current_ixp == (int32_t)ixp) {
        store->current_ixp = LOGIN_SAVE_NOT_FOUND;
    } else if (store->current_ixp > (int32_t)ixp) {
        --store->current_ixp;
    }

    if (store->count == 0U) {
        store->head = 0U;
        store->current_ixp = LOGIN_SAVE_NOT_FOUND;
    }

    portEXIT_CRITICAL(&s_login_lock);
    return true;
}
