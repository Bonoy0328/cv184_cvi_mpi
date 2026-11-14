#ifndef KY50_IME_H
#define KY50_IME_H
int ime_load(const char *path);
void ime_close(void);
/* Exact syllable candidates, without tones; v represents ü. */
const char *ime_lookup(const char *syllable);
int ime_candidate(const char *text,unsigned index,char out[5]);
unsigned ime_count(const char *text);
#endif
