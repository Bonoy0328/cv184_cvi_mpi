/* Diagnostic only: hold DAC and amplifier on, with a 10-second silent lead-in.
 * LCD must be unplugged and C25 released by the accompanying shell wrapper.
 * Uses the original mono asset, duplicated to stereo, without normalization.
 */
#include "asoundlib.h"
#include "codec_ioctl.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
static volatile sig_atomic_t stop;
static void interrupted(int sig) { (void)sig; stop=1; }
int main(void)
{
    struct sigaction sa={.sa_handler=interrupted};
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); sigaction(SIGALRM,&sa,NULL);
    alarm(620);
    setvbuf(stdout,NULL,_IOLBF,0);
    int rc=1,codec=-1,amp=-1,old_vol=0,have_vol=0;
    unsigned volume=32;
    FILE *f=fopen("assets/recognition_success.s16le","rb");
    struct pcm *pcm=NULL;
    if(!f) { perror("speech file"); goto done; }
    struct pcm_config config={.channels=2,.rate=16000,.period_size=320,.period_count=4,
        .format=PCM_FORMAT_S16_LE,.start_threshold=320};
    pcm=pcm_open(1,0,PCM_OUT,&config);
    if(!pcm||!pcm_is_ready(pcm)) { fprintf(stderr,"PCM: %s\n",pcm?pcm_get_error(pcm):"open failed"); goto done; }
    codec=open("/dev/misc_dac_0",O_RDWR|O_CLOEXEC);
    if(codec<0) { perror("codec"); goto done; }
    have_vol=!ioctl(codec,ACODEC_GET_OUTPUT_VOL,&old_vol);
    if(ioctl(codec,ACODEC_SET_OUTPUT_VOL,&volume)) { perror("volume"); goto done; }
    amp=open("/dev/ky50-amp",O_WRONLY|O_CLOEXEC);
    if(amp<0) { perror("amplifier"); goto done; }
    int16_t mono[320],stereo[640]={0};
    unsigned frames=0,silence=500,round=0;
    printf("VOICE_HOLD_START volume=%u; initial silence=10s; total<=600s\n",volume);
    while(!stop && frames<16000U*600U) {
        memset(stereo,0,sizeof(stereo));
        if(silence) --silence;
        else {
            size_t n=fread(mono,sizeof(*mono),320,f);
            if(ferror(f)) { perror("read speech"); goto done; }
            if(n) {
                if(ftell(f)==(long)(n*sizeof(*mono))) printf("VOICE round=%u frame=%u\n",++round,frames);
                for(size_t i=0;i<n;i++) stereo[2*i]=stereo[2*i+1]=mono[i];
            } else {
                rewind(f); silence=600; /* 12 seconds; keep PCM/amp running. */
            }
        }
        if(pcm_write(pcm,stereo,sizeof(stereo))) { fprintf(stderr,"PCM write: %s\n",pcm_get_error(pcm)); goto done; }
        frames+=320;
    }
    rc=0;
done:
    if(pcm) pcm_close(pcm);
    if(amp>=0) close(amp);
    if(codec>=0) { if(have_vol) ioctl(codec,ACODEC_SET_OUTPUT_VOL,&old_vol); close(codec); }
    if(f) fclose(f);
    printf("VOICE_HOLD_END rc=%d\n",rc);
    return rc;
}
