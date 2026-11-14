#ifndef KY50_UI_H
#define KY50_UI_H
#include <stdint.h>
#include <time.h>
#include "face_ir_calibration.h"
#define UI_WIDTH 480
#define UI_HEIGHT 854
enum ui_page { UI_HOME, UI_MENU, UI_INPUT, UI_FEATURE, UI_TOUCH_TEST,
    UI_ADVANCED, UI_FACTORY, UI_SCREEN_TEST, UI_CAMERA_PREVIEW,
    UI_USERS, UI_USER_ID, UI_USER_NAME, UI_USER_TYPES, UI_PALM_ENROLL, UI_USERS_MANAGE, UI_PARAMETERS, UI_FACE_IR_CALIBRATE };
enum ui_action { UI_ACTION_NONE, UI_ACTION_RGB, UI_ACTION_IR, UI_ACTION_DVP,
    UI_ACTION_RELAY, UI_ACTION_BELL, UI_ACTION_SPEAKER,
    UI_ACTION_WHITE_LIGHT, UI_ACTION_IR_LIGHT, UI_ACTION_FACE_IR_CALIBRATE };
/* All functions run in the UI thread. Snapshot is straight ARGB8888. */
int ui_init(void);
void ui_destroy(void);
void ui_pointer(int x, int y, int pressed);
void ui_update(int ethernet, time_t now, int camera_ok);
void ui_process(void);
void ui_render(void);
void ui_show(enum ui_page page, int feature);
enum ui_page ui_current_page(void);
const char *ui_input_text(void);
const uint32_t *ui_pixels(void);
unsigned ui_generation(void);
int ui_time_text(time_t now, char *out, unsigned size);
int ui_carrier(const char *path);
uint64_t ui_monotonic_ms(void);
enum ui_action ui_take_action(void);
void ui_factory_status(const char *text);
void ui_factory_lights(int white, int infrared);
void ui_face_ir_calibration_status(const FaceIrCalibrationStatus *status);
void ui_set_source(const char *name);
void ui_access_poll(void);
struct AccessSnapshot;
/* Same immutable UI-thread copy used by the recognition label this frame. */
const struct AccessSnapshot *ui_access_state(void);
#endif
