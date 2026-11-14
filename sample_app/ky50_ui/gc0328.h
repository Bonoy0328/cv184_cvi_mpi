#ifndef KY50_GC0328_H
#define KY50_GC0328_H
int gc0328_start(void);
/* Third receiver/pipe, with MCLK owned by the dual GC2083 pipeline. */
int gc0328_start_parallel(void);
void gc0328_stop(void);
#endif
