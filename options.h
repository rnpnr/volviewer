/* NOTE(rnp): for video output we will render a full rotation in this much time at the
 * the specified frame rate */
#define OUTPUT_TIME_SECONDS     8.0f
#define OUTPUT_FRAME_RATE         60
#define OUTPUT_BG_CLEAR_COLOUR (v4){{0.05, 0.05, 0.05, 1}}

#define RAW_OUTPUT_PATH "/tmp/out.raw"

#define RENDER_MSAA_SAMPLES    8
#define RENDER_TARGET_WIDTH    1920
#define RENDER_TARGET_HEIGHT   1080
#define CAMERA_ELEVATION_ANGLE 25.0f
#define CAMERA_RADIUS          200.0f

#define BOUNDING_BOX_COLOUR    0.78, 0.07, 0.20, 1
#define BOUNDING_BOX_FRACTION  0.007f

#define DYNAMIC_RANGE 30
#define LOG_SCALE     1

typedef struct {
	c8  *file_path;
	u32  width;         /* number of points in data */
	u32  height;
	u32  depth;
	v3   min_coord_mm;
	v3   max_coord_mm;
	f32  clip_fraction; /* fraction of half volume used to create pyramidal shape (0 for cube) */
	f32  threshold;
	f32  translate_x;   /* mm to translate by when multi display is active */
	b32  swizzle;       /* 1 -> swap y-z coordinates when sampling texture */
	f32  gain;          /* uniform image gain */
	u32  texture;
} VolumeDisplayItem;

#define DRAW_ALL_VOLUMES 1
global u32 single_volume_index = 0;
global VolumeDisplayItem volumes[] = {
	/* WALKING FORCES */
	{"./data/walking.bin", 512, 1024, 64, {{-20.5, -9.6, 5}}, {{20.5, 9.6, 50}}, 0.62, 72, 0, 0, 3.7},
	/* RCA */
	{"./data/tpw.bin", 512, 64, 1024, {{-9.6, -9.6, 5}}, {{9.6, 9.6, 50}}, 0, 92, -5 *  18.5, 1, 5},
	{"./data/vls.bin", 512, 64, 1024, {{-9.6, -9.6, 5}}, {{9.6, 9.6, 50}}, 0, 89,  5 *  18.5, 1, 5},
};
