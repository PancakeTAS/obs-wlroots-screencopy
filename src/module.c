#include <obs/obs-module.h>
#include <obs/obs-properties.h>
#include <obs/util/bmem.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-util.h>

OBS_DECLARE_MODULE()

static void noop() {}

// TODO: implement source_create, source_update, source_destroy, source_render

typedef struct {
    struct wl_output* output;
    char* name;
    char* description; // (optional!)

    struct wl_list link;
} wl_output_info;

typedef struct {
    struct wl_display* wl;
    struct wl_list outputs;
} source_data;


// wayland output

static void wl_output_name(void* _, struct wl_output* output, const char* name) {
    wl_output_info* info = (wl_output_info*) _;
    info->name = strdup(name);
}

static void wl_output_description(void* _, struct wl_output* output, const char* description) {
    wl_output_info* info = (wl_output_info*) _;
    info->description = strdup(description);
}

static struct wl_output_listener output_listener = {
    .geometry = noop,
    .mode = noop,
    .done = noop,
    .scale = noop,
    .name = wl_output_name,
    .description = wl_output_description
};

// wayland registry

static void wl_registry_global(void* _, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    source_data* data = (source_data*) _;

    if (strcmp(interface, wl_output_interface.name) == 0) {
        wl_output_info* output = bzalloc(sizeof(wl_output_info));
        output->output = wl_registry_bind(registry, name, &wl_output_interface, version);
        wl_output_add_listener(output->output, &output_listener, output);
        wl_list_insert(&data->outputs, &output->link);
    }

}

static struct wl_registry_listener listener = {
    .global = wl_registry_global,
    .global_remove = noop
};

// obs source

static void* source_create(obs_data_t* settings, obs_source_t* source) {
    source_data* data = bzalloc(sizeof(source_data));

    // connect to compositor
    data->wl = wl_display_connect(NULL);
    if (data->wl == NULL) {
        blog(LOG_ERROR, "Failed to connect to Wayland display");
        return NULL;
    }

    // initialize data struct
    wl_list_init(&data->outputs);

    // fetch registry
    struct wl_registry* registry = wl_display_get_registry(data->wl);
    wl_registry_add_listener(registry, &listener, data);
    wl_display_roundtrip(data->wl);

    // fetch outputs (note: listeners are registered during binding)
    wl_display_roundtrip(data->wl);

    // dev: print all outputs
    wl_output_info* output;
    wl_list_for_each(output, &data->outputs, link) {
        blog(LOG_INFO, "Found %s with description %s", output->name, output->description);
    }

    return data;
}

static void source_update(void* _, obs_data_t* settings) {

}

static void source_destroy(void* _) {
    source_data* data = (source_data*) _;

    // destroy all outputs
    wl_output_info* output, *safe_output;
    wl_list_for_each_safe(output, safe_output, &data->outputs, link) {
        wl_list_remove(&output->link);
        free(output->name);
        free(output->description);
        wl_output_destroy(output->output);
        bfree(output);
    }

    // disconnect from compositor
    wl_display_disconnect(data->wl);

    bfree(data);
}

static void source_render(void* _, gs_effect_t* effect) {

}

static obs_properties_t* source_get_properties(void* _) {
    source_data* data = (source_data*) _;
    obs_properties_t* properties = obs_properties_create();

    // add output list property
    obs_property_t* output = obs_properties_add_list(properties, "output", "Output", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    wl_output_info* info;
    wl_list_for_each(info, &data->outputs, link) {
        obs_property_list_add_string(output, info->name, info->name);
    }

    return properties;
}

static void source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "output", "");
}

// obs source definition

static const char* source_get_name(void* _) { return "Screencopy Source"; }
static uint32_t source_get_width(void* _) { return 2560; } // FIXME: don't hardcode width and height
static uint32_t source_get_height(void* _) { return 1440; }
static struct obs_source_info source_info = {
    .id = "screencopy-source",
    .version = 1,
    .get_name = source_get_name,

    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
    .icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,

    .create = source_create,
    .update = source_update,
    .destroy = source_destroy,
    .video_render = source_render,

    .get_properties = source_get_properties,
    .get_defaults = source_get_defaults,

    .get_width = source_get_width,
    .get_height = source_get_height
};

// obs module

bool obs_module_load() {
    obs_register_source(&source_info);
    return true;
}