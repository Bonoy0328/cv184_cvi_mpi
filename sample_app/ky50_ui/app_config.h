#ifndef KY50_APP_CONFIG_H
#define KY50_APP_CONFIG_H
#define APP_DEFAULT_LEAVE_FRAMES 5
#define APP_MAX_LEAVE_FRAMES 100
#define APP_DEFAULT_RELAY_SECONDS 5
#define APP_MAX_RELAY_SECONDS 60
#define APP_DEFAULT_MATCH_CONFIRMATIONS 1
#define APP_MAX_MATCH_CONFIRMATIONS 10
#define APP_DEFAULT_SUCCESS_COOLDOWN_SECONDS 0
#define APP_MAX_SUCCESS_COOLDOWN_SECONDS 60
#define APP_DEFAULT_STRONG_LIVENESS 1
typedef struct { int palm_leave_frames,face_leave_frames,relay_seconds,match_confirmations,success_cooldown_seconds,strong_liveness; } AppParameters;
/* Main application policy only. Never read or write the SDK's pvaConf.json. */
int app_parameters_valid(const AppParameters *p);
/* 0 loaded, 1 missing or v1/v2/v3 upgraded (caller saves), -1 malformed/unreadable. */
int app_config_load(const char *directory,AppParameters *p);
int app_config_save(const char *directory,const AppParameters *p);
#endif
