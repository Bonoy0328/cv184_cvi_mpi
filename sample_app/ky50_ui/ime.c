#include "ime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {char *pinyin,*characters;} Entry;
static Entry entries[512];static unsigned count;
int ime_load(const char *path)
{
    if(count)return 0;
    FILE *f=fopen(path,"r");if(!f)return -1;
    char *line=NULL;size_t cap=0;ssize_t n;
    while((n=getline(&line,&cap,f))>0) {
        if(n>20000||count==512)goto fail;
        char *tab=strchr(line,'\t');if(!tab||tab-line>6||tab==line)goto fail;
        *tab++=0;tab[strcspn(tab,"\r\n")]=0;
        if(count&&strcmp(entries[count-1].pinyin,line)>=0)goto fail;
        entries[count].pinyin=strdup(line);entries[count].characters=strdup(tab);
        if(!entries[count].pinyin||!entries[count].characters){free(entries[count].pinyin);free(entries[count].characters);goto fail;}
        ++count;
    }
    free(line);fclose(f);return count?0:-1;
fail:
    free(line);fclose(f);ime_close();return -1;
}
void ime_close(void){for(unsigned i=0;i<count;i++){free(entries[i].pinyin);free(entries[i].characters);}count=0;}
const char *ime_lookup(const char *syllable)
{
    int lo=0,hi=(int)count-1;
    while(lo<=hi){int m=(lo+hi)/2,c=strcmp(syllable,entries[m].pinyin);if(!c)return entries[m].characters;if(c<0)hi=m-1;else lo=m+1;}
    return "";
}
unsigned ime_count(const char *text)
{
    unsigned n=0;for(const unsigned char *p=(const unsigned char*)text;*p;p++)if((*p&0xc0)!=0x80)n++;return n;
}
int ime_candidate(const char *text,unsigned index,char out[5])
{
    const unsigned char *p=(const unsigned char*)text;
    while(index&&*p){p++;while((*p&0xc0)==0x80)p++;index--;}
    if(!*p){out[0]=0;return -1;}
    unsigned n=1;while((p[n]&0xc0)==0x80&&n<4)n++;
    memcpy(out,p,n);out[n]=0;return 0;
}
