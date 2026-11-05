#ifndef VIDEO_H
#define VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_MAX_WIDTH  640
#define VIDEO_MAX_HEIGHT 360

typedef struct mpv_handle mpv_handle;

typedef struct {
    mpv_handle *handle;
    bool active;
} Video;

void video_init(Video *video);
bool video_start(Video *video, const char *path, int64_t window_id);
void video_stop(Video *video);
void video_toggle_pause(Video *video);
void video_seek(Video *video, double seconds);

#endif
