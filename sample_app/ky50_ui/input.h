#ifndef KY50_INPUT_H
#define KY50_INPUT_H
int touch_open(const char *path);
int touch_prepare(void);
int touch_poll(int fd);
#include <stdint.h>
uint64_t touch_pending_ms(void);
void touch_presented(void);
#endif
