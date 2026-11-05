#include "video.h"

#include <mpv/client.h>

#include <stdio.h>
#include <string.h>

void video_init(Video *video) {
    memset(video, 0, sizeof *video);
}

void video_stop(Video *video) {
    if (video->handle) mpv_terminate_destroy(video->handle);
    video->handle = NULL;
    video->active = false;
}

static bool set_option(mpv_handle *handle, const char *name, const char *value) {
    int error = mpv_set_option_string(handle, name, value);
    if (error < 0) {
        fprintf(stderr, "clayterm: mpv option %s: %s\n", name, mpv_error_string(error));
        return false;
    }
    return true;
}

bool video_start(Video *video, const char *path, int64_t window_id) {
    video_stop(video);
    mpv_handle *handle = mpv_create();
    if (!handle) {
        fprintf(stderr, "clayterm: could not create mpv instance\n");
        return false;
    }
    video->handle = handle;

    int error = mpv_set_option(handle, "wid", MPV_FORMAT_INT64, &window_id);
    if (error < 0 ||
        !set_option(handle, "vo", "x11") ||
        !set_option(handle, "input-default-bindings", "no") ||
        !set_option(handle, "input-vo-keyboard", "no") ||
        !set_option(handle, "osc", "no") ||
        !set_option(handle, "osd-level", "0") ||
        !set_option(handle, "audio-display", "no") ||
        !set_option(handle, "keep-open", "yes")) {
        if (error < 0)
            fprintf(stderr, "clayterm: mpv window option: %s\n", mpv_error_string(error));
        video_stop(video);
        return false;
    }

    error = mpv_initialize(handle);
    if (error < 0) {
        fprintf(stderr, "clayterm: initialize mpv: %s\n", mpv_error_string(error));
        video_stop(video);
        return false;
    }

    const char *command[] = { "loadfile", path, "replace", NULL };
    error = mpv_command(handle, command);
    if (error < 0) {
        fprintf(stderr, "clayterm: load video: %s\n", mpv_error_string(error));
        video_stop(video);
        return false;
    }
    video->active = true;
    return true;
}

void video_toggle_pause(Video *video) {
    if (!video->active) return;
    const char *command[] = { "cycle", "pause", NULL };
    int error = mpv_command(video->handle, command);
    if (error < 0) fprintf(stderr, "clayterm: pause video: %s\n", mpv_error_string(error));
}

void video_seek(Video *video, double seconds) {
    if (!video->active) return;
    char offset[32];
    snprintf(offset, sizeof offset, "%.1f", seconds);
    const char *command[] = { "seek", offset, "relative", "exact", NULL };
    int error = mpv_command(video->handle, command);
    if (error < 0) fprintf(stderr, "clayterm: seek video: %s\n", mpv_error_string(error));
}
