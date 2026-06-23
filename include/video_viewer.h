#ifndef VIDEO_VIEWER_H
#define VIDEO_VIEWER_H

#include <3ds.h>
#include <citro2d.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <malloc.h>

#define MAX_VIDEOS 100
#define THUMBNAIL_WIDTH 64
#define THUMBNAIL_HEIGHT 48
#define VIDEOS_PER_ROW 4
#define VIDEOS_PER_COL 3
#define VIDEOS_PER_PAGE (VIDEOS_PER_ROW * VIDEOS_PER_COL)

// Stores video metadata and thumbnail state.
typedef struct {
    char filename[256];
    char filepath[512];
    C2D_Image thumbnail;
    bool hasThumbnail;
} VideoInfo;

// Viewer state.
typedef struct {
    VideoInfo videos[MAX_VIDEOS];
    int videoCount;
    int selectedIndex;
    int scrollOffset;
    C3D_RenderTarget* top;
    C3D_RenderTarget* bottom;
} VideoViewer;

// Main viewer functions.
void initViewer(VideoViewer* viewer);
void scanVideos(VideoViewer* viewer, const char* path);
void generateThumbnail(VideoViewer* viewer, int index);
void renderTopScreen(VideoViewer* viewer);
void renderBottomScreen(VideoViewer* viewer);
void handleInput(VideoViewer* viewer);
void cleanupViewer(VideoViewer* viewer);

#endif // VIDEO_VIEWER_H
