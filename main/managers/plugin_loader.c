#include "managers/plugin_loader.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_ENABLE_NATIVE_SD_APPS
#include "esp_dlfcn.h"
#endif

static const char *TAG = "PluginLoader";
static plugin_loaded_app_t s_loaded;
static char s_last_error[160];

static esp_err_t fail_err(esp_err_t err, const char *message) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", message ? message : "plugin loader error");
    ESP_LOGE(TAG, "%s", s_last_error);
    return err;
}

plugin_loaded_app_t *plugin_loader_current(void) {
    return s_loaded.manifest ? &s_loaded : NULL;
}

esp_err_t plugin_loader_load(const char *id, plugin_loaded_app_t **out_app) {
    if (out_app) *out_app = NULL;
    if (!id) return fail_err(ESP_ERR_INVALID_ARG, "missing app id");

    if (s_loaded.manifest) {
        plugin_loader_unload(&s_loaded);
    }

    const plugin_app_manifest_t *manifest = plugin_manager_find(id);
    if (!manifest) return fail_err(ESP_ERR_NOT_FOUND, "app not found");
    if (!plugin_manager_target_supported()) return fail_err(ESP_ERR_NOT_SUPPORTED, "native SD apps disabled or unsupported target");
#if CONFIG_NATIVE_SD_APPS_REQUIRE_TARGET_MATCH
    if (!plugin_manager_target_matches(manifest)) return fail_err(ESP_ERR_NOT_SUPPORTED, "app target does not match firmware target");
#endif
    if (manifest->unsafe) {
#if !CONFIG_NATIVE_SD_APPS_UNSAFE_MODE
        return fail_err(ESP_ERR_INVALID_STATE, "app requests unsafe mode but firmware disabled it");
#endif
    }

#if CONFIG_ENABLE_NATIVE_SD_APPS
    void *handle = dlopen(manifest->entry_path, RTLD_NOW);
    if (!handle) {
        const char *err = dlerror();
        snprintf(s_last_error, sizeof(s_last_error), "dlopen failed: %s", err ? err : "unknown");
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }

    ghostesp_app_init_fn init_fn = (ghostesp_app_init_fn)dlsym(handle, "ghostesp_app_init");
    if (!init_fn) {
        const char *err = dlerror();
        snprintf(s_last_error, sizeof(s_last_error), "dlsym failed: %s", err ? err : "ghostesp_app_init missing");
        dlclose(handle);
        ESP_LOGE(TAG, "%s", s_last_error);
        return ESP_FAIL;
    }

    bool unsafe_allowed = manifest->unsafe;
    const ghostesp_app_t *app = init_fn(plugin_api_get(unsafe_allowed));
    if (!app || app->api_version != GHOSTESP_APP_API_VERSION) {
        dlclose(handle);
        return fail_err(ESP_ERR_INVALID_RESPONSE, "app init failed or returned incompatible API version");
    }

    memset(&s_loaded, 0, sizeof(s_loaded));
    s_loaded.manifest = manifest;
    s_loaded.app = app;
    s_loaded.handle = handle;
    s_loaded.unsafe_allowed = unsafe_allowed;
    if (out_app) *out_app = &s_loaded;
    s_last_error[0] = '\0';
    ESP_LOGI(TAG, "Loaded SD app %s", manifest->id);
    return ESP_OK;
#else
    return fail_err(ESP_ERR_NOT_SUPPORTED, "native SD apps are not compiled in");
#endif
}

esp_err_t plugin_loader_start(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->app) return ESP_ERR_INVALID_ARG;
    if (!loaded->running && loaded->app->on_start) loaded->app->on_start();
    loaded->running = true;
    return ESP_OK;
}

esp_err_t plugin_loader_stop(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->app) return ESP_ERR_INVALID_ARG;
    if (loaded->running && loaded->app->on_stop) loaded->app->on_stop();
    loaded->running = false;
    return ESP_OK;
}

esp_err_t plugin_loader_unload(plugin_loaded_app_t *loaded) {
    if (!loaded || !loaded->manifest) return ESP_OK;
    plugin_loader_stop(loaded);
#if CONFIG_ENABLE_NATIVE_SD_APPS
    if (loaded->handle) dlclose(loaded->handle);
#endif
    memset(loaded, 0, sizeof(*loaded));
    return ESP_OK;
}

const char *plugin_loader_last_error(void) {
    return s_last_error;
}
