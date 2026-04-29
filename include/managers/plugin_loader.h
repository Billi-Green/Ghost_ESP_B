#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include "managers/plugin_api.h"
#include "managers/plugin_manager.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const plugin_app_manifest_t *manifest;
    const ghostesp_app_t *app;
    void *handle;
    bool running;
    bool unsafe_allowed;
} plugin_loaded_app_t;

esp_err_t plugin_loader_load(const char *id, plugin_loaded_app_t **out_app);
esp_err_t plugin_loader_start(plugin_loaded_app_t *loaded);
esp_err_t plugin_loader_stop(plugin_loaded_app_t *loaded);
esp_err_t plugin_loader_unload(plugin_loaded_app_t *loaded);
plugin_loaded_app_t *plugin_loader_current(void);
const char *plugin_loader_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
