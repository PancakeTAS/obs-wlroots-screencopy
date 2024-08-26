#include <obs/obs-module.h>
#include <obs/obs-properties.h>

OBS_DECLARE_MODULE()

// TODO: implement source_create, source_update, source_destroy, source_render

static void* source_create(obs_data_t* settings, obs_source_t* source) {
    return NULL;
}

static void source_update(void* _, obs_data_t* settings) {

}

static void source_destroy(void* _) {

}

static void source_render(void* _, gs_effect_t* effect) {

}

// source definition

static const char* source_get_name(void* _) { return "Screencopy Source"; }
static uint32_t source_get_width(void* _) { return 2560; } // FIXME: don't hardcode width and height
static uint32_t source_get_height(void* _) { return 1440; }
static obs_properties_t* source_get_properties(void* _) { return obs_properties_create(); } // FIXME: add properties and defaults
static void source_get_defaults(obs_data_t* _) {}
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

// module definition

bool obs_module_load() {
    obs_register_source(&source_info);
    return true;
}