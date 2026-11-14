#ifndef KY50_FACTORY_H
#define KY50_FACTORY_H
int factory_relay(void);
int factory_speaker(void);
enum factory_light { FACTORY_LIGHT_WHITE, FACTORY_LIGHT_IR, FACTORY_LIGHT_COUNT };
/* UI thread only. States are 0/1, or -1 when the device is unavailable. */
int factory_lights_init(void);
int factory_light_toggle(enum factory_light light);
int factory_light_state(enum factory_light light);
/* Returns a completion message at most once, or NULL. UI thread only. */
const char *factory_poll(void);
void factory_stop(void);
#endif
