#ifndef KY50_USER_UI_H
#define KY50_USER_UI_H
#include "ui.h"
#include "lvgl.h"
#include "access.h"
void user_ui_before_page(enum ui_page next);
int user_ui_build(lv_obj_t *root,enum ui_page next);
void user_ui_update(const AccessSnapshot *snapshot);
void user_ui_shutdown(void);
const char *user_ui_employee_text(void);
const char *user_ui_name_text(void);
const char *user_ui_composition(void);
#endif
