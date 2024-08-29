#include <bits/time.h>
#include <obs/graphics/graphics.h>
#include <obs/obs-module.h>
#include <obs/obs-properties.h>
#include <obs/obs.h>
#include <obs/util/base.h>
#include <obs/util/bmem.h>
#include <time.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>

#include <wlroots/wlr-screencopy-unstable-v1.h>
#include <wayland/linux-dmabuf-unstable-v1.h>

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
    struct zwp_linux_dmabuf_v1* linux_dmabuf;

    pthread_t capture_thread;

    volatile bool capture_stopsignal;
    struct wl_output* capture_output;

    uint32_t screencopy_frame_format;
    uint32_t screencopy_frame_width;
    uint32_t screencopy_frame_height;
    volatile bool screencopy_frame_failed;

    gs_texture_t* obs_texture;
    enum gs_color_space obs_color_space;

    uint64_t frame_duration_ns;
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

    // buffer objects
    struct gbm_bo* gbm_bo = NULL;
    struct wl_buffer* wl_buffer = NULL;
    uint32_t gbm_bo_width = 0;
    uint32_t gbm_bo_height = 0;
    uint32_t gbm_bo_format = 0;

    // loop capture
    struct timespec ts;
    while (!data->capture_stopsignal) {
        if (!data->capture_output) {
            usleep(1000); // 1ms
            continue;
        }

        // prepare timer
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t start_time = ts.tv_sec * 1000000000ULL + ts.tv_nsec;

        // request output capture
        struct zwlr_screencopy_frame_v1* screencopy_frame = zwlr_screencopy_manager_v1_capture_output(data->screencopy_manager, 0, data->capture_output);
        zwlr_screencopy_frame_v1_add_listener(screencopy_frame, &screencopy_frame_listener, data);
        wl_display_roundtrip(data->wl);
        if (data->screencopy_frame_failed) {
            blog(LOG_ERROR, "Failed to capture output");
            continue;
        }

        // create dma-buf
        if ((gbm_bo_width != data->screencopy_frame_width || gbm_bo_height != data->screencopy_frame_height || gbm_bo_format != data->screencopy_frame_format) && gbm_bo) {
            gbm_bo_destroy(gbm_bo);
            wl_buffer_destroy(wl_buffer);

            obs_enter_graphics();
            gs_texture_destroy(data->obs_texture);
            obs_leave_graphics();

            data->obs_texture = NULL;
            gbm_bo = NULL;
            wl_buffer = NULL;
        }

        if (!gbm_bo) {
            gbm_bo_width = data->screencopy_frame_width;
            gbm_bo_height = data->screencopy_frame_height;
            gbm_bo_format = data->screencopy_frame_format;
            gbm_bo = gbm_bo_create(data->gbm, gbm_bo_width, gbm_bo_height, gbm_bo_format, GBM_BO_USE_RENDERING);
            if (gbm_bo == NULL) {
                blog(LOG_ERROR, "Failed to create GBM buffer object");
                continue;
            }

            int32_t fd = gbm_bo_get_fd_for_plane(gbm_bo, 0);
            uint32_t offset = gbm_bo_get_offset(gbm_bo, 0);
            uint32_t stride = gbm_bo_get_stride_for_plane(gbm_bo, 0);
            uint64_t modifier = gbm_bo_get_modifier(gbm_bo);

            // create wl_buffer
            struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(data->linux_dmabuf);
            zwp_linux_buffer_params_v1_add(params,
                fd,
                0,
                offset,
                stride,
                modifier >> 32,
                modifier & 0xFFFFFFFF
            );
            wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, gbm_bo_width, gbm_bo_height, data->screencopy_frame_format, 0);
            zwp_linux_buffer_params_v1_destroy(params);

            // find fitting color format
            enum gs_color_format color_format = GS_BGRX;
            data->obs_color_space = GS_CS_SRGB;
            if (data->screencopy_frame_format == GBM_FORMAT_XRGB2101010
                || data->screencopy_frame_format == GBM_FORMAT_XBGR2101010
                || data->screencopy_frame_format == GBM_FORMAT_RGBX1010102
                || data->screencopy_frame_format == GBM_FORMAT_BGRX1010102

                || data->screencopy_frame_format == GBM_FORMAT_ARGB2101010
                || data->screencopy_frame_format == GBM_FORMAT_ABGR2101010
                || data->screencopy_frame_format == GBM_FORMAT_RGBA1010102
                || data->screencopy_frame_format == GBM_FORMAT_BGRA1010102) {
                color_format = GS_R10G10B10A2;
                data->obs_color_space = GS_CS_SRGB_16F;
            } else if (data->screencopy_frame_format == GBM_FORMAT_XBGR16161616
                || data->screencopy_frame_format == GBM_FORMAT_ABGR16161616) {
                color_format = GS_RGBA16;
                data->obs_color_space = GS_CS_SRGB_16F;
            }

            // create obs texture
            obs_enter_graphics();
            data->obs_texture = gs_texture_create_from_dmabuf(
                gbm_bo_width,
                gbm_bo_height,
                gbm_bo_format,
                color_format,
                1,
                &fd,
                &stride,
                &offset,
                &modifier
            );
            obs_leave_graphics();
        }

        // copy frame to dma-buf
        zwlr_screencopy_frame_v1_copy(screencopy_frame, wl_buffer);
        wl_display_roundtrip(data->wl);
        if (data->screencopy_frame_failed) {
            blog(LOG_ERROR, "Failed to copy frame to DMA-BUF");
            zwlr_screencopy_frame_v1_destroy(screencopy_frame);
            continue;
        }

        // release frame
        zwlr_screencopy_frame_v1_destroy(screencopy_frame);

        // wait for next frame
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t end_time = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        uint64_t frame_time = end_time - start_time;
        if (frame_time < data->frame_duration_ns) {
            uint64_t sleep_micros = (data->frame_duration_ns - frame_time) / 1000;
            usleep(sleep_micros * 0.9); // sleep 90% of the time to allow for some slack, if your display manages 900hz and you're capturing at 60hz.. screw you in particular
        } else {
            blog(LOG_WARNING, "Frame took too long to capture: %lu ns", frame_time);
        }
    }

    // destroy dma-buf
    if (gbm_bo != NULL) {
        gbm_bo_destroy(gbm_bo);
        wl_buffer_destroy(wl_buffer);

        obs_enter_graphics();
        gs_texture_destroy(data->obs_texture);
        data->obs_texture = NULL;
        obs_leave_graphics();
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
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        data->linux_dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version);
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
    wl_list_init(&data->outputs);

    // create gbm device
    const char* gbm_device = obs_data_get_string(settings, "gbm_device");
    data->gbm_fd = open((gbm_device && strlen(gbm_device) != 0) ? gbm_device : "/dev/dri/renderD128", O_RDWR);
    data->gbm = gbm_create_device(data->gbm_fd);
    if (data->gbm == NULL) {
        blog(LOG_ERROR, "Failed to create GBM device");
        return data;
    }

    // connect to compositor
    const char* wl_display = obs_data_get_string(settings, "wl_display");
    data->wl = wl_display_connect(wl_display && strlen(wl_display) != 0 ? wl_display : NULL);
    if (data->wl == NULL) {
        blog(LOG_ERROR, "Failed to connect to Wayland display");
        return data;
    }

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

    // update source settings
    source_update(data, settings);

    return data;
}

static void source_update(void* _, obs_data_t* settings) {
    source_data* data = (source_data*) _;

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

    // update frame duration
    data->frame_duration_ns = obs_get_frame_interval_ns();
    printf("Frame duration: %lu ns\n", data->frame_duration_ns);

}

static void source_destroy(void* _) {
    source_data* data = (source_data*) _;

    // stop capture thread
    data->capture_stopsignal = true;
    pthread_join(data->capture_thread, NULL);

    // destroy all outputs
    wl_output_info* output, *safe_output;
    wl_list_for_each_safe(output, safe_output, &data->outputs, link) {
        wl_list_remove(&output->link);
        free(output->name);
        free(output->description);
        wl_output_destroy(output->output);
        bfree(output);
    }

    // destroy wayland objects
    zwlr_screencopy_manager_v1_destroy(data->screencopy_manager);
    zwp_linux_dmabuf_v1_destroy(data->linux_dmabuf);
    wl_display_disconnect(data->wl);

    // destroy gbm device
    gbm_device_destroy(data->gbm);

    bfree(data);
}

static void source_render(void* _, gs_effect_t* effect) {
    source_data* data = (source_data*) _;
    if (data->obs_texture == NULL) {
        return;
    }

    // render texture
    char* technique = (data->obs_color_space == GS_CS_SRGB || gs_get_color_space() == GS_CS_SRGB) ? "Draw" : "DrawSrgbDecompress";
    effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), data->obs_texture);

    while (gs_effect_loop(effect, technique)) {
        gs_draw_sprite(data->obs_texture, 0, data->screencopy_frame_width, data->screencopy_frame_height);
    }
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

    // add gbm and wayland device properties
    obs_properties_t* advanced = obs_properties_create();
    obs_properties_add_text(advanced, "gbm_device", "GBM Device", OBS_TEXT_DEFAULT);
    obs_properties_add_text(advanced, "wl_display", "Wayland Display", OBS_TEXT_DEFAULT);
    obs_properties_add_group(properties, "advanced", "Advanced Settings (requires restart)", OBS_GROUP_NORMAL, advanced);

    return properties;
}

static void source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "output", "");
    obs_data_set_default_string(settings, "gbm_device", NULL);
    obs_data_set_default_string(settings, "wl_display", NULL);
}

// obs source definition

static const char* source_get_name(void* _) { return "Screencopy Source"; }
static uint32_t source_get_width(void* _) { return ((source_data*) _)->screencopy_frame_width; }
static uint32_t source_get_height(void* _) { return ((source_data*) _)->screencopy_frame_height; }
static enum gs_color_space source_get_color_space(void* _, size_t count, const enum gs_color_space *preferred_spaces) { return ((source_data*) _)->obs_color_space; }
static struct obs_source_info source_info = {
    .id = "screencopy-source",
    .version = 1,
    .get_name = source_get_name,

    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB,
    .icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,

    .create = source_create,
    .update = source_update,
    .destroy = source_destroy,
    .video_render = source_render,

    .get_properties = source_get_properties,
    .get_defaults = source_get_defaults,

    .get_width = source_get_width,
    .get_height = source_get_height,
    .video_get_color_space = source_get_color_space,
};

// obs module

bool obs_module_load() {
    obs_register_source(&source_info);
    return true;
}