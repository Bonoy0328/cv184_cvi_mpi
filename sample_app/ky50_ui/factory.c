#include "factory.h"
#include "relay_control.h"
#include "ui.h"
#include "asoundlib.h"
#include "codec_ioctl.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
static int audio_thread;
static int lights_fd=-1,light_state[FACTORY_LIGHT_COUNT];
static uint64_t relay_until;
static pthread_t speaker_thread;
static atomic_int audio_result,audio_stop;
int factory_lights_init(void)
{
    if(lights_fd>=0) return 0;
    lights_fd=open("/dev/ky50-lights",O_RDWR|O_CLOEXEC);
    if(lights_fd<0) { perror("factory lights");return -1; }
    /* The exclusive device open resets both GPIO outputs to low. */
    memset(light_state,0,sizeof(light_state));
    return 0;
}
int factory_light_state(enum factory_light light)
{
    if(light<0||light>=FACTORY_LIGHT_COUNT||lights_fd<0) return -1;
    return light_state[light];
}
int factory_light_toggle(enum factory_light light)
{
    if(light<0||light>=FACTORY_LIGHT_COUNT) return -1;
    if(factory_lights_init()) return -1;
    int on=!light_state[light];
    char command[]={light==FACTORY_LIGHT_IR?'i':'w',on?'1':'0'};
    ssize_t n;
    do { n=write(lights_fd,command,sizeof(command)); } while(n<0&&errno==EINTR);
    if(n!=(ssize_t)sizeof(command)) {
        perror("factory light switch");
        /* Closing also puts both lamps in a known off state in the driver. */
        close(lights_fd);lights_fd=-1;return -1;
    }
    light_state[light]=on;
    printf("FACTORY_LIGHT %s=%s\n",light==FACTORY_LIGHT_IR?"IR":"WHITE",on?"ON":"OFF");
    return on;
}
int factory_relay(void)
{
    if(relay_until) return -1;
    if(relay_control_pulse(5))return -1;
    relay_until=ui_monotonic_ms()+5000;return 0;
}
static void *play(void *unused)
{
    (void)unused;
    int rc=-1,codec=-1,amp=-1,old_vol=0,have_vol=0;
    FILE *f=fopen("assets/recognition_success.s16le","rb");
    struct pcm *pcm=NULL;
    if(!f) goto done;
    struct pcm_config config={.channels=2,.rate=16000,.period_size=320,.period_count=4,
        .format=PCM_FORMAT_S16_LE,.start_threshold=320};
    pcm=pcm_open(1,0,PCM_OUT,&config);
    if(!pcm||!pcm_is_ready(pcm)) {
        fprintf(stderr,"Speaker PCM: %s\n",pcm?pcm_get_error(pcm):"open failed");goto done;
    }
    codec=open("/dev/misc_dac_0",O_RDWR|O_CLOEXEC);
    if(codec<0) goto done;
    have_vol=!ioctl(codec,ACODEC_GET_OUTPUT_VOL,&old_vol);
    unsigned volume=16;
    if(ioctl(codec,ACODEC_SET_OUTPUT_VOL,&volume)) goto done;
    amp=open("/dev/ky50-amp",O_WRONLY|O_CLOEXEC);
    if(amp<0) { perror("Speaker amplifier");goto done; }
    int16_t mono[320],stereo[640];size_t n;
    while(!atomic_load(&audio_stop)&&(n=fread(mono,sizeof(*mono),320,f))>0) {
        memset(stereo,0,sizeof(stereo));
        for(size_t i=0;i<n;i++) stereo[i*2]=stereo[i*2+1]=mono[i];
        if(pcm_write(pcm,stereo,sizeof(stereo))) { fprintf(stderr,"Speaker: %s\n",pcm_get_error(pcm));goto done; }
    }
    if(ferror(f)||atomic_load(&audio_stop)) goto done;
    /* Flush queued speech with silence. Closing PCM early cuts the last word. */
    memset(stereo,0,sizeof(stereo));
    for(int i=0;i<6;i++) if(pcm_write(pcm,stereo,sizeof(stereo))) goto done;
    rc=0;
done:
    if(pcm) pcm_close(pcm);
    if(amp>=0) close(amp);
    if(codec>=0) { if(have_vol) ioctl(codec,ACODEC_SET_OUTPUT_VOL,&old_vol);close(codec); }
    if(f) fclose(f);
    printf("SPEAKER playback completed rc=%d\n",rc);
    atomic_store(&audio_result,rc?-1:1);return NULL;
}
int factory_speaker(void)
{
    if(audio_thread) return -1;
    atomic_store(&audio_stop,0);atomic_store(&audio_result,0);
    if(pthread_create(&speaker_thread,NULL,play,NULL)) return -1;
    audio_thread=1;return 0;
}
const char *factory_poll(void)
{
    if(audio_thread&&atomic_load(&audio_result)) {
        pthread_join(speaker_thread,NULL);audio_thread=0;
        return atomic_load(&audio_result)>0?"音频播放完成":"音频播放失败";
    }
    if(relay_until&&ui_monotonic_ms()>=relay_until+30) {
        relay_until=0;return "继电器测试计时结束";
    }
    return NULL;
}
void factory_stop(void)
{
    if(lights_fd>=0) { close(lights_fd);lights_fd=-1; }
    relay_control_stop();
    relay_until=0;
    atomic_store(&audio_stop,1);
    if(audio_thread) { pthread_join(speaker_thread,NULL);audio_thread=0; }
}
