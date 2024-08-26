#include <fcntl.h>
#include <obs/obs-module.h>
#include <obs/obs-properties.h>
#include <obs/util/bmem.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <pthread.h>
#include <gbm.h>

#include "wlr-protocols/wlr-screencopy-unstable-v1.h"

OBS_DECLARE_MODULE()

static void noop() {}

typedef struct {
    struct wl_output* output;
    char* name;
    char* description; // (optional!)

    struct wl_list link;
} wl_output_info;

typedef struct {
    int gbm_fd;
    struct gbm_device* gbm;
    struct wl_display* wl;
    struct wl_list outputs;
    struct zwlr_screencopy_manager_v1* screencopy_manager;

    pthread_t capture_thread;
    pthread_mutex_t capture_mutex;
    volatile bool capture_running;
    volatile bool capture_stopsignal;
    struct wl_output* capture_output;

    uint32_t screencopy_frame_format;
    uint32_t screencopy_frame_width;
    uint32_t screencopy_frame_height;
    volatile bool screencopy_frame_failed;
} source_data;

// screencopy frame

static void screencopy_frame_linux_dmabuf(void* _, struct zwlr_screencopy_frame_v1* frame, uint32_t format, uint32_t width, uint32_t height) {
    source_data* data = (source_data*) _;
    data->screencopy_frame_format = format;
    data->screencopy_frame_width = width;
    data->screencopy_frame_height = height;
}

static void screencopy_frame_ready(void* _, struct zwlr_screencopy_frame_v1* frame, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    source_data* data = (source_data*) _;
    data->screencopy_frame_failed = false;
}

static void screencopy_frame_failed(void* _, struct zwlr_screencopy_frame_v1* frame) {
    source_data* data = (source_data*) _;
    data->screencopy_frame_failed = true;
}

static struct zwlr_screencopy_frame_v1_listener screencopy_frame_listener = {
    .buffer = noop,
    .flags = noop,
    .ready = screencopy_frame_ready,
    .failed = screencopy_frame_failed,
    .damage = noop,
    .linux_dmabuf = screencopy_frame_linux_dmabuf,
    .buffer_done = noop
};

// capture thread

static void* capture_thread(void* _) {
    source_data* data = (source_data*) _;

    // loop capture
    struct gbm_bo* gbm_bo = NULL;
    uint32_t gbm_bo_width, gbm_bo_height, gbm_bo_format;
    while (!data->capture_stopsignal) {
        if (!data->capture_running) {
            usleep(1000); // 1ms
            continue;
        }

        pthread_mutex_lock(&data->capture_mutex);

        // request output capture
        struct zwlr_screencopy_frame_v1* screencopy_frame = zwlr_screencopy_manager_v1_capture_output(data->screencopy_manager, 0, data->capture_output);
        zwlr_screencopy_frame_v1_add_listener(screencopy_frame, &screencopy_frame_listener, data);
        wl_display_roundtrip(data->wl);
        if (data->screencopy_frame_failed) {
            blog(LOG_ERROR, "Failed to capture output");
            pthread_mutex_unlock(&data->capture_mutex);
            continue; // FIXME: HOW DO I HANDLE THIS??
        }

        // create dma-buf
        if (gbm_bo == NULL) {
            gbm_bo_width = data->screencopy_frame_width;
            gbm_bo_height = data->screencopy_frame_height;
            gbm_bo_format = data->screencopy_frame_format;
            gbm_bo = gbm_bo_create(data->gbm, gbm_bo_width, gbm_bo_height, gbm_bo_format, GBM_BO_USE_RENDERING);
        } else if (gbm_bo_width != data->screencopy_frame_width || gbm_bo_height != data->screencopy_frame_height || gbm_bo_format != data->screencopy_frame_format) {
            gbm_bo_destroy(gbm_bo);
            gbm_bo_width = data->screencopy_frame_width;
            gbm_bo_height = data->screencopy_frame_height;
            gbm_bo_format = data->screencopy_frame_format;
            gbm_bo = gbm_bo_create(data->gbm, gbm_bo_width, gbm_bo_height, gbm_bo_format, GBM_BO_USE_RENDERING);
        }

        pthread_mutex_unlock(&data->capture_mutex);
    }

    // destroy dma-buf
    if (gbm_bo != NULL) {
        gbm_bo_destroy(gbm_bo);
    }

    return NULL;
}

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
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        data->screencopy_manager = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, version);
    }

}

static struct wl_registry_listener listener = {
    .global = wl_registry_global,
    .global_remove = noop
};

// obs source

static void source_update(void* _, obs_data_t* settings);
static void* source_create(obs_data_t* settings, obs_source_t* source) {
    source_data* data = bzalloc(sizeof(source_data));

    // create gbm device
    data->gbm_fd = open("/dev/dri/renderD128", O_RDWR);
    data->gbm = gbm_create_device(data->gbm_fd);
    if (data->gbm == NULL) {
        blog(LOG_ERROR, "Failed to create GBM device");
        return NULL;
    }

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
    if (data->screencopy_manager == NULL) {
        blog(LOG_ERROR, "Failed to bind to screencopy manager");
        return NULL;
    }

    // fetch outputs (note: listeners are registered during binding)
    wl_display_roundtrip(data->wl);

    // start capture thread
    pthread_create(&data->capture_thread, NULL, capture_thread, data);
    pthread_mutex_init(&data->capture_mutex, NULL);

    // update source settings
    source_update(data, settings);

    return data;
}

static void source_update(void* _, obs_data_t* settings) {
    source_data* data = (source_data*) _;

    // pause capture thread
    data->capture_running = false;
    pthread_mutex_lock(&data->capture_mutex);

    // find output to capture
    const char* output_pattern = obs_data_get_string(settings, "output");
    wl_output_info* output_info = NULL;
    wl_list_for_each(output_info, &data->outputs, link) {
        if (strcmp(output_info->name, output_pattern) == 0) {
            data->capture_output = output_info->output;
            break;
        }
    }
    if (data->capture_output == NULL) {
        blog(LOG_ERROR, "Invalid output for screen capture specified");
        return;
    }

    // resume capture thread
    data->capture_running = true;
    pthread_mutex_unlock(&data->capture_mutex);

}

static void source_destroy(void* _) {
    source_data* data = (source_data*) _;

    // stop capture thread
    data->capture_stopsignal = true;
    pthread_join(data->capture_thread, NULL);
    pthread_mutex_destroy(&data->capture_mutex);

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
    char label[1024];
    wl_list_for_each(info, &data->outputs, link) {
        snprintf(label, sizeof(label), "%s: %s", info->name, info->description ? info->description : "no description");
        obs_property_list_add_string(output, label, info->name);
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