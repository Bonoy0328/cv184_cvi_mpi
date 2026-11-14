#ifndef KY50_RELAY_CONTROL_H
#define KY50_RELAY_CONTROL_H
/* UI thread only. Nonblocking kernel timer; a new pulse extends from now. */
int relay_control_pulse(int seconds);
void relay_control_stop(void);
#endif
