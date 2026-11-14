#ifndef KY50_SETTINGS_H
#define KY50_SETTINGS_H
enum camera_source { CAMERA_RGB, CAMERA_IR, CAMERA_DVP, CAMERA_SOURCE_COUNT };
const char *camera_source_name(enum camera_source source);
int settings_load(const char *dir,enum camera_source *source);
int settings_save(const char *dir,enum camera_source source);
#endif
