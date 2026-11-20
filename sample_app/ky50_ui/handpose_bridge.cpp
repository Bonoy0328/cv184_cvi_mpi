#include "ky50_features.h"
#include "hand_overlay.h"
#include <unistd.h>
#if !KY50_ENABLE_PALM
#include "pvaConfig.h"
#include "pvaFaceRecInternal.h"
#include "pvaFingerprintInternal.h"
#endif
#include "pva.h"
#include "pvaFaceRec.h"
#include "deviceAuth.h"
#include "json.hpp"
#include <fstream>
#include <cstdio>
#include <cstring>
#include <exception>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include "access.h"
extern "C" int media_face_color_nv12(const uint8_t*,uint16_t,uint16_t,uint8_t*,uint32_t);
extern "C" void media_face_color_stop(void);
static bool face_color_convert(const unsigned char* nv12,uint16_t w,uint16_t h,unsigned char* bgr,uint32_t stride,void*) {
    return media_face_color_nv12(nv12,w,h,bgr,stride)==0;
}
static std::atomic<bool> ready;
static bool face_enabled,finger_enabled;
static PalmParameters parameters;
static FaceParameters face_parameters;
static std::string filtered_config;

// init_pva always initializes palm, and also loads every module enabled in its JSON.
// Keep relative model paths valid when filtering a caller-supplied config.
static bool start_modules(const char *config, nlohmann::json &settings)
{
#if KY50_ENABLE_PALM
    bool filtered=false;
    for(const char *name : {"facepose","fingerprint"}) {
        bool allowed=std::strcmp(name,"facepose")==0 ? KY50_ENABLE_FACE : KY50_ENABLE_FINGERPRINT;
        if(!allowed && settings.contains(name) && settings[name].value("enable",true)) {
            settings[name]["enable"]=false;
            filtered=true;
        }
    }
    if(filtered) {
        std::string pattern=std::string(config)+".ky50.XXXXXX";
        int fd=mkstemp(&pattern[0]);
        if(fd<0)return false;
        filtered_config=pattern;
        FILE *out=fdopen(fd,"w");
        if(!out){close(fd);return false;}
        auto json=settings.dump(4);
        bool written=fwrite(json.data(),1,json.size(),out)==json.size();
        if(fclose(out)!=0||!written)return false;
        config=filtered_config.c_str();
    }
    return init_pva(const_cast<char*>(config));
#else
    // These are the same CV184X submodule initializers used by init_pva.
    // The application has already started the media system and checked the device licence.
    (void)settings;
    return Config_8salWjIBOea585d::getInstance()->loadConfig(config)
        && (!face_enabled || initPvaFace(config))
        && (!finger_enabled || initPvaFingerprint(config));
#endif
}
extern "C" int handpose_start(const char *config)
{
    try {
        if(!config) return -1;
        std::ifstream file(config);
        nlohmann::json settings;
        file >> settings;
        finger_enabled=KY50_ENABLE_FINGERPRINT&&settings.contains("fingerprint")&&settings["fingerprint"].value("enable",false);
        // Existing bindings identify the p333 embedding space. Another model requires
        // an explicit application migration, never automatic rebinding of its UUIDs.
        if(finger_enabled&&settings["fingerprint"].value("model_variant",std::string("embed_p333"))!="embed_p333") {
            std::fprintf(stderr,"FINGER unsupported app model binding (requires embed_p333)\n");return -1;
        }
        face_enabled=KY50_ENABLE_FACE&&settings.contains("facepose")&&settings["facepose"].value("enable",false);
        if(KY50_ENABLE_PALM) {
            parameters.hand_threshold=std::lround(settings.at("handpose").at("model_probability_threshold").get<double>()*100);
            parameters.auth_threshold=std::lround(settings.at("feature").at("auth_threshold").get<double>()*100);
            parameters.dynamic_level=settings.at("handpose").at("dynamic_detection_level").get<int>();
        }
        if(face_enabled) {
            const auto &pose=settings.at("facepose"),&feature=settings.at("facefeature");
            face_parameters.detection_threshold=std::lround(pose.at("model_probability_threshold").get<double>()*100);
            face_parameters.quality_threshold=std::lround(pose.at("quality_threshold").get<double>()*100);
            face_parameters.liveness_threshold=std::lround(pose.at("liveness_threshold").get<double>()*100);
            face_parameters.quality_enable=pose.at("quality_enable").get<bool>();
            face_parameters.liveness_enable=pose.at("liveness_enable").get<bool>();
            face_parameters.auth_threshold=std::lround(feature.at("auth_threshold").get<double>()*100);
            face_parameters.filter_threshold=std::lround(feature.at("filter_threshold").get<double>()*100);
        }
        if(KY50_ENABLE_PALM)std::printf("PALM defaults brightness=%d..%d offset=%.2f/%.2f\n",
            settings.at("handpose").at("brightness_threshold_min").get<int>(),
            settings.at("handpose").at("brightness_threshold_max").get<int>(),
            settings.at("handpose").at("horizontal_offset_tolerance").get<double>(),
            settings.at("handpose").at("vertical_offset_tolerance").get<double>());
        std::string key=settings.at("keyPath").get<std::string>();
        if(!pvacheckAuth(const_cast<char*>(key.c_str()))) {
            std::fprintf(stderr,"PALM device licence check failed: %s\n",key.c_str());return -1;
        }
        ready=start_modules(config,settings);
        if(ready) {
            const char* mode=std::getenv("PVA_FACE_PREPROCESS");if(!mode)mode="direct";
            if(face_enabled&&std::strcmp(mode,"direct")&&std::strcmp(mode,"reference")&&std::strcmp(mode,"vpss")) {
                std::fprintf(stderr,"Invalid PVA_FACE_PREPROCESS: %s\n",mode);handpose_stop();return -1;
            }
            if(face_enabled&&!std::strcmp(mode,"vpss")&&!pvaSetFaceColorConverter(face_color_convert,nullptr)) {
                handpose_stop();return -1;
            }
            std::printf("FACE_PREPROCESS mode=%s\n",mode);

            return 0;
        }
    } catch(const std::exception& e) { std::fprintf(stderr,"PALM init: %s\n",e.what()); }
    catch(...) { std::fprintf(stderr,"PALM init: unknown failure\n"); }
    handpose_stop();
    std::fprintf(stderr,"PALM unavailable: check model/config/device licence\n");
    return -1;
}
extern "C" int handpose_detect(uint8_t *yuv,uint16_t width,uint16_t height,HandPoseRes_t *pose)
{
    if(!ready||!yuv||!pose) return -1;
    std::memset(pose,0,sizeof(*pose));pose->status=STATUS_FAILED;
    try { return pvaGetHandPose(yuv,width,height,pose)?1:0; }
    catch(const std::exception& e) { std::fprintf(stderr,"PALM inference: %s\n",e.what()); }
    catch(...) { std::fprintf(stderr,"PALM inference: unknown failure\n"); }
    ready=false;
    return -1;
}
extern "C" void handpose_stop(void)
{
    try {
#if KY50_ENABLE_PALM
        deinit_pva();
#else
        if(face_enabled)deinitPvaFace();
        if(finger_enabled)deinitPvaFingerprint();
#endif
    } catch(...) { std::fprintf(stderr,"PVA deinit failed\n"); }
    if(!filtered_config.empty()){unlink(filtered_config.c_str());filtered_config.clear();}
    media_face_color_stop();
    ready=false;
}

#include <vector>
static int backend_ids(uint16_t *ids,int capacity)
{
    if(!ready)return -1;
    try {
        int n=pvaDataBaseGetAllPalmCount();if(n<0||n>capacity)return -1;
        return pvaDataBaseGetAllPalmIds(ids);
    } catch(...) {return -1;}
}
static int backend_exists(uint16_t uuid)
{
    if(!ready)return -1;
    try {User_For_C_t u={};u.uuid=uuid;return pvaDataBaseExistUser(&u)?1:0;}catch(...){return -1;}
}
static int backend_complete(uint16_t uuid)
{
    if(!ready)return -1;
    try {
        User_For_C_t u={};u.uuid=uuid;
        std::vector<uint8_t> data(APP_TEMPLATE_BYTES,0);
        bool ok=pvaDataBaseGetFeat(&u,data.data(),data.size());
        std::memset(data.data(),0,data.size());return ok?1:0;
    }catch(...){return -1;}
}
static int backend_remove(uint16_t uuid)
{
    if(!ready)return -1;
    try {User_For_C_t u={};u.uuid=uuid;return pvaDataBaseDeleteUser(&u)?0:-1;}catch(...){return -1;}
}
static int backend_clear(void)
{
    try{return ready&&pvaClearRegisterBuffer()?0:-1;}catch(...){return -1;}
}
static int backend_capacity(void)
{
    try{return ready?pvaGetRegisterBufferCapacity():-1;}catch(...){return -1;}
}
static int backend_features(uint8_t *yuv,uint16_t w,uint16_t h,HandPoseRes_t *pose,uint8_t *feature)
{
    if(ready&&pose) {
        try{return pvaGetFeat(yuv,w,h,pose,feature);}catch(...){}
    }
    if(pose)pose->status=STATUS_FAILED;
    return STATUS_FAILED;
}
static int backend_enroll(User_For_C_t *user,uint8_t *feature,ResgisRes_t *progress)
{
    try{return ready&&pvaRegister(user,feature,true,progress)?0:-1;}catch(...){return -1;}
}
static int backend_recognize(uint8_t *feature,User_For_C_t *user)
{
    try{return ready&&pvaGetAuthRes(feature,user)?1:0;}catch(...){return -1;}
}
static int backend_parameters_get(PalmParameters *out)
{
    if(!ready||!out)return -1;
    *out=parameters;return 0;
}
static int backend_parameters_set(const PalmParameters *p)
{
    if(!ready||!p||p->hand_threshold<0||p->hand_threshold>100||p->auth_threshold<0||
       p->auth_threshold>100||p->dynamic_level<0||p->dynamic_level>4)return -1;
    try {
        if(!pvaConfigSetHandThreshold(p->hand_threshold/100.0f)||
           !pvaConfigSetAuthThreshold(p->auth_threshold/100.0f)||
           !pvaConfigSetDynamicDetectionLevel(p->dynamic_level))return -1;
        parameters=*p;
        std::printf("PALM_PARAMETERS hand=%.2f auth=%.2f dynamic=%d applied\n",
            p->hand_threshold/100.0,p->auth_threshold/100.0,p->dynamic_level);
        return 0;
    }catch(...){return -1;}
}
static int backend_pick_free(void)
{
    if(!ready)return -1;
    try {uint16_t id=pvaDataBasePickFreeId();return id<APP_TEMPLATE_SLOTS?id:-1;}catch(...){return -1;}
}
extern "C" const PalmBackend ky50_palm_backend={backend_ids,backend_exists,backend_complete,backend_remove,
    backend_clear,backend_capacity,backend_features,backend_enroll,backend_recognize,
    backend_parameters_get,backend_parameters_set,backend_pick_free};

extern "C" int facepose_detect(uint8_t *nv12,uint16_t w,uint16_t h,FacePoseRes_t *pose)
{
    if(!ready||!nv12||!pose)return -1;
    *pose=FacePoseRes_t{};
    try {
        if(pvaGetFacePose(nv12,w,h,pose,0))return 1;
        return pose->status==STATUS_FACE_NO_FACE?0:-1;
    } catch(...) {pose->status=STATUS_FACE_MODEL_FAILED;return -1;}
}
extern "C" int facepose_enabled(void){return ready&&face_enabled;}
extern "C" int facepose_detect_ir(const uint8_t *nv12,uint16_t w,uint16_t h,PvaFaceGeometry *geometry)
{
    try{return ready&&face_enabled&&pvaGetFaceGeometryIr(nv12,w,h,geometry)?1:0;}catch(...){return -1;}
}
static int face_ids(uint16_t *ids,int capacity)
{
    if(!ready)return -1;
    try {
        int n=pvaDataBaseGetAllFaceCount();if(n<0||n>capacity)return -1;
        return pvaDataBaseGetAllFaceIds(ids);
    }catch(...){return -1;}
}
static int face_exists(uint16_t id)
{
    if(!ready)return -1;
    try{User_For_C_t user={};user.uuid=id;return pvaDataBaseExistUserFace(&user)?1:0;}catch(...){return -1;}
}
static int face_complete(uint16_t id)
{
    if(!ready)return -1;
    try {
        int exists=face_exists(id);if(exists<=0)return exists;
        int bytes=pvaDataBaseGetFeatSizeFace();if(bytes<=0)return -1;
        std::vector<uint8_t> data(bytes,0);User_For_C_t user={};user.uuid=id;
        bool ok=pvaDataBaseGetFeatFace(&user,data.data(),bytes);
        std::memset(data.data(),0,data.size());return ok?1:-1;
    }catch(...){return -1;}
}
static int face_remove(uint16_t id)
{
    try{return ready&&pvaDataBaseDeleteUserFace(id)?0:-1;}catch(...){return -1;}
}
static int face_clear(void)
{
    try{return ready&&pvaClearRegisterBufferFace()?0:-1;}catch(...){return -1;}
}
static int face_capacity(void)
{
    try{return ready?pvaGetRegisterBufferCapacityFace():-1;}catch(...){return -1;}
}
static int face_features(uint8_t *nv12,uint16_t w,uint16_t h,FacePoseRes_t *pose,uint8_t *feature)
{
    if(ready&&pose)try{return pvaGetFaceFeat(nv12,w,h,pose,feature);}catch(...){}
    if(pose)pose->status=STATUS_FACE_MODEL_FAILED;
    std::memset(feature,0,PVA_FACE_FEATURE_BYTES);return STATUS_FACE_MODEL_FAILED;
}
static int face_enroll(User_For_C_t *user,uint8_t *feature,float quality,ResgisRes_t *progress)
{
    try{return ready&&pvaRegisterFace(user,feature,quality,true,progress,0)?0:-1;}catch(...){return -1;}
}
static int face_recognize(uint8_t *feature,User_For_C_t *user)
{
    try{return ready&&pvaGetAuthResFace(feature,user)?1:0;}catch(...){return -1;}
}
static int face_pick_free(void)
{
    if(!ready||!face_enabled)return -1;
    try {uint16_t id=pvaDataBasePickFreeIdFace();return id<APP_TEMPLATE_SLOTS?id:-1;}catch(...){return -1;}
}
static int face_parameters_get(FaceParameters *out)
{
    if(!ready||!face_enabled||!out)return -1;
    *out=face_parameters;return 0;
}
static int face_parameters_set(const FaceParameters *p)
{
    if(!ready||!face_enabled||!face_parameters_valid(p))return -1;
    try {
        /* pvaConfigSetFaceThreshold is an alias of the model probability setter. */
        if(!pvaConfigSetModelProbabilityThresholdFace(p->detection_threshold/100.0f)||
           !pvaConfigSetQualityThresholdFace(p->quality_threshold/100.0f)||
           !pvaConfigSetLivenessThresholdFace(p->liveness_threshold/100.0f)||
           !pvaConfigSetQualityEnableFace(p->quality_enable!=0)||
           !pvaConfigSetLivenessEnableFace(p->liveness_enable!=0)||
           !pvaConfigSetAuthThresholdFace(p->auth_threshold/100.0f)||
           !pvaConfigSetFilterThresholdFace(p->filter_threshold/100.0f))return -1;
        face_parameters=*p;
        std::printf("FACE_PARAMETERS detection=%.2f quality=%d/%.2f liveness=%d/%.2f auth=%.2f filter=%.2f applied\n",
            p->detection_threshold/100.0,p->quality_enable,p->quality_threshold/100.0,
            p->liveness_enable,p->liveness_threshold/100.0,p->auth_threshold/100.0,p->filter_threshold/100.0);
        return 0;
    }catch(...){return -1;}
}
static void *face_sample_capture(const uint8_t *image,uint16_t w,uint16_t h,const FacePoseRes_t *pose)
{
    try{return ready?pvaCaptureFaceSample(image,w,h,pose):nullptr;}catch(...){return nullptr;}
}
static int face_sample_features(const void *sample,FacePoseRes_t *pose,uint8_t *feat)
{
    try{if(ready)return pvaGetFaceSampleFeat(static_cast<const PvaFaceSample *>(sample),pose,feat);}catch(...){}
    pose->status=STATUS_FACE_MODEL_FAILED;return pose->status;
}
static void face_sample_free(void *sample){pvaFreeFaceSample(static_cast<PvaFaceSample *>(sample));}
static int face_live_ir(uint8_t *ir,uint16_t w,uint16_t h)
{
    try{return ready&&face_enabled&&pvaFaceLiveIr(ir,w,h)?1:0;}catch(...){return -1;}
}
static int face_live_ir_region(const uint8_t *ir,uint16_t w,uint16_t h,const PvaFaceGeometry *geometry)
{
    try{return ready&&face_enabled&&pvaFaceLiveIrRegion(ir,w,h,geometry)?1:0;}catch(...){return -1;}
}
extern "C" const FaceBackend ky50_face_backend={face_ids,face_exists,face_complete,face_remove,
    face_clear,face_capacity,face_features,face_enroll,face_recognize,face_pick_free,
    face_parameters_get,face_parameters_set,face_sample_capture,face_sample_features,face_sample_free,face_live_ir,face_live_ir_region};

extern "C" int fingerprint_enabled(void) {return ready&&finger_enabled&&pvaGetFeatCapacityFingerprint()>0&&pvaGetFeatCapacityFingerprint()<=APP_FEATURE_BYTES;}
static FingerprintSample_t finger_sample(const uint8_t *gray,uint16_t w,uint16_t h,uint64_t id) {
    FingerprintSample_t s={};s.source_type=PVA_FINGERPRINT_SAMPLE_GRAY8;
    s.image.data=gray;s.image.data_size=uint32_t(w)*h;s.image.width=w;s.image.height=h;s.image.stride=w;s.image.format=PVA_IMAGE_GRAY8;s.sample_id=id;return s;
}
static int finger_detect(const uint8_t *gray,uint16_t w,uint16_t h,uint64_t id,FingerprintRes_t *res) {
    auto s=finger_sample(gray,w,h,id);
    return pvaDetectFingerprint(&s,res);
}
static int finger_features(const uint8_t *gray,uint16_t w,uint16_t h,uint64_t id,FingerprintRes_t *res,uint8_t *out,uint32_t cap,uint32_t *size) {
    auto s=finger_sample(gray,w,h,id);return pvaGetFingerprintFeat(&s,res,out,cap,size);
}
static int finger_ids(uint16_t *ids,int cap){return pvaDataBaseGetAllFingerprintIds(ids,cap);}
static int finger_exists(uint16_t id){User_For_C_t u={};u.uuid=id;return pvaDataBaseExistUserFingerprint(&u)?1:0;}
static int finger_complete(uint16_t id){
    User_For_C_t u={};u.uuid=id;uint32_t cap=pvaDataBaseGetFeatSizeFingerprint(&u),size=0;
    if(!cap)return 0;
    if(cap>APP_TEMPLATE_BYTES)return -1;
    std::vector<uint8_t> data(cap);bool ok=pvaDataBaseGetFeatFingerprint(&u,data.data(),cap,&size);
    std::fill(data.begin(),data.end(),0);return ok&&size==cap?1:-1;
}
static int finger_remove(uint16_t id){return pvaDataBaseDeleteUserFingerprint(id)?0:-1;}
static int finger_clear(void){return pvaClearRegisterBufferFingerprint()?0:-1;}
static int finger_capacity(void){return pvaGetRegisterBufferCapacityFingerprint();}
static int finger_free(void){uint16_t id=pvaDataBasePickFreeIdFingerprint();return id==UINT16_MAX?-1:id;}
static int finger_enroll(User_For_C_t *u,const uint8_t *f,uint32_t size,ResgisRes_t *r){return pvaRegisterFingerprint(u,f,size,true,r)?0:-1;}
static int finger_auth(const uint8_t *f,uint32_t size,User_For_C_t *u){return pvaGetAuthResFingerprint(f,size,u)?1:0;}
static int finger_set_enrolling(int active){
    const double ratio=active?0.20:0.15;
    int rc=pvaSetMinAreaRatioFingerprint(ratio)?0:-1;
    std::printf("FINGER_AREA mode=%s min_area_ratio=%.2f rc=%d\n",active?"enroll":"recognize",ratio,rc);
    return rc;
}
extern "C" const FingerBackend ky50_finger_backend={finger_ids,finger_exists,finger_complete,finger_remove,finger_clear,finger_capacity,finger_free,finger_detect,finger_features,finger_enroll,finger_auth,finger_set_enrolling};
