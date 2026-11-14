/* Board-only SDK configuration check. Isolated /tmp config and empty databases;
 * no camera, biometric images, enrollment or production user data are used. */
#include "access.h"
#include "hand_overlay.h"
#include "json.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static nlohmann::json read_config(const std::string &path)
{
    std::ifstream input(path);nlohmann::json value;input>>value;return value;
}
static void write_config(const std::string &path,const nlohmann::json &value)
{
    std::ofstream output(path);output<<value.dump(4)<<'\n';output.close();assert(!output.fail());
}
static AccessSnapshot snapshot;
static void submit(AccessCommand command)
{
    assert(!access_submit(&command));access_poll(10000);access_snapshot(&snapshot,0);
}
static void check_face(const std::string &path,const FaceParameters &p)
{
    auto config=read_config(path);
    const auto &pose=config.at("facepose"),&feat=config.at("facefeature");
    assert(std::abs(pose.at("model_probability_threshold").get<double>()-p.detection_threshold/100.0)<1e-6);
    assert(std::abs(pose.at("quality_threshold").get<double>()-p.quality_threshold/100.0)<1e-6);
    assert(std::abs(pose.at("liveness_threshold").get<double>()-p.liveness_threshold/100.0)<1e-6);
    assert(pose.at("quality_enable").get<bool>()==bool(p.quality_enable));
    assert(pose.at("liveness_enable").get<bool>()==bool(p.liveness_enable));
    assert(std::abs(feat.at("auth_threshold").get<double>()-p.auth_threshold/100.0)<1e-6);
    assert(std::abs(feat.at("filter_threshold").get<double>()-p.filter_threshold/100.0)<1e-6);
    assert(!config.contains("palm_leave_frames")&&!config.contains("face_leave_frames"));
}
int main(int argc,char **argv)
{
    assert(argc==2);
    const std::string directory="/tmp/ky50-face-parameters-probe";
    assert(!mkdir(directory.c_str(),0700)); /* Refuse to reuse any existing data. */
    const std::string path=directory+"/pvaConf.json";
    auto baseline=read_config(argv[1]);
    baseline["database"]["database_path"]=directory+"/palm.bin";
    baseline["facedatabase"]["database_path"]=directory+"/face.bin";
    write_config(path,baseline);
    assert(!handpose_start(path.c_str()));
    assert(!access_open_biometrics(directory.c_str(),&ky50_palm_backend,&ky50_face_backend));
    access_snapshot(&snapshot,0);assert(snapshot.ready&&snapshot.face_parameters_ready&&snapshot.app_parameters_ready);
    AccessCommand change={};change.type=ACCESS_FACE_PARAMETERS;
    change.face_parameters={57,41,65,0,0,-12,-3};submit(change);
    assert(!snapshot.face_parameters_result);check_face(path,change.face_parameters);
    auto after_face=read_config(path);
    assert(after_face.at("handpose")==baseline.at("handpose")&&after_face.at("feature")==baseline.at("feature"));
    change.type=ACCESS_APP_PARAMETERS;change.app_parameters={3,7};submit(change);
    assert(!snapshot.app_parameters_result&&snapshot.app_parameters.palm_leave_frames==3&&snapshot.app_parameters.face_leave_frames==7);
    assert(read_config(path)==after_face);
    std::cout<<"PASS live real SDK setters and independent app.conf, no reinitialization required\n";
    access_close();handpose_stop();
    /* Simulate run.sh replacing the temporary SDK config with release defaults. */
    write_config(path,baseline);
    assert(!handpose_start(path.c_str()));
    assert(!access_open_biometrics(directory.c_str(),&ky50_palm_backend,&ky50_face_backend));
    access_snapshot(&snapshot,0);assert(snapshot.ready&&snapshot.face_parameters_ready);
    check_face(path,change.face_parameters);
    assert(snapshot.app_parameters.palm_leave_frames==3&&snapshot.app_parameters.face_leave_frames==7);
    access_close();handpose_stop();
    std::cout<<"PASS board restart restores SDK settings and both application departure thresholds\n";
    return 0;
}
