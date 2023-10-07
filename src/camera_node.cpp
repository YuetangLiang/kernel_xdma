#include "npu_camera_source.hpp"

static int calibration = 0;
#define ZYNQ_SYNC_MODE calibration
#include "xdma_camera_drv.hpp"
#include <ros/ros.h>
#include "lib_pixel.h"

#include <ros/package.h>
#include <image_transport/image_transport.h>
#include <camera_info_manager/camera_info_manager.h>
#include <sstream>
#include <string.h>
#include <cv_bridge/cv_bridge.h>

using namespace std;


perception::FullPerception* fpr = nullptr;

typedef struct {
    int id;
    sensor_msgs::Image img;
    image_transport::CameraPublisher *pb;
    ros_h264_img_t h264_img;
    int enable;
    task_t t;
    cpu_yuyv2rgb_t yuyv2rgb;

    void init(int cam_id) {
        id = cam_id;
        img.is_bigendian = 0;
        img.width  = cams_cfg()->width;
        img.height = cams_cfg()->height;
        img.encoding = "rgb8";
        img.step = img.width * 3;//rgb
        img.data.resize(img.height * img.step);
        img.header.frame_id = "camera" + to_string(id) + "_systime";
        h264_img.img.header = img.header;
        yuyv2rgb.init(img.width, img.height);
    }

    void copy_from(void *yuyv) {
        void *rgb = (typeof(rgb))&img.data[0];
        yuyv2rgb.convert_8u(rgb, yuyv);
    }

    void pub() {
        PUB_IMG(pb, "camera" + to_string(id) + "/image_raw", img);
    }
} ros_cam_t;

typedef struct {
    ros_cam_t cams[12];
    int nr_cams;
    void init() {
        cams_cfg()->bytes_per_pixel = 2;
        cams_cfg()->width           = 1920;
        cams_cfg()->height          = 1080;

        for(int i = 0; i < ARRAY_SIZE(cams); i++) {
            cams[i].init(i);
            extern void zynq_camera_callback(void *d);
            subscribe_zynq_cam(i, zynq_camera_callback);
        }
    }
} ros_cams_t;

static ros_cams_t ros_cams;

void zynq_camera_callback(void *d)
{
    ros::Time   stamp  = ros::Time::now();
    zynq_cam_t *cam    = (typeof(cam))d;

    switch(cam->id) {
    case 0:
        g_frame_id = ++g_frame_id % 10;
        ts[0][g_frame_id] = stamp.toSec();
        npu_camera0(ptr_of_cam(cam->id));
        break;


    case 4:
        g_frame_id1 = ++g_frame_id1 % 10;
        ts[1][g_frame_id1] = stamp.toSec();
        npu_camera4(ptr_of_cam(cam->id));
        break;

    }



    if(!calibration) return;
    ros_cam_t *r   = &ros_cams.cams[cam->id];
    r->copy_from((void*)ptr_of_cam(cam->id));
    r->pub();
}

namespace perception {

class Nsu_ar0231_Node
{
public:
    ros::NodeHandle& m_nh_;
    ros::NodeHandle& m_pnh_;


    virtual ~Nsu_ar0231_Node()
    {
        cams_cfg()->t.stop = 1;
    }

    Nsu_ar0231_Node(ros::NodeHandle nh, ros::NodeHandle pnh) :
        m_nh_(nh),
        m_pnh_(pnh)
    {
        std::string model_config_file;
        int raw_im_width;
        int raw_im_height;
        std::string calib_file_path;

        m_pnh_.param("calibration", calibration, 0);
        m_pnh_.param("model_config_file", model_config_file, std::string("")); 
        m_pnh_.param("image_width", raw_im_width, 1920); 
        m_pnh_.param("image_height", raw_im_height, 1080);
        m_pnh_.param("camera_paramters_file_path", calib_file_path, std::string("poc_c/yaml/"));

        ros_cams.init();
    }


    bool spin()
    {
        ros::Rate loop_rate(20);

        while (m_nh_.ok())
        {
            ros::spinOnce();
            fpr->postProcess();
            loop_rate.sleep();
        }
        return true;
    }

};//class
} //namespace


int main(int argc, char **argv)
{
    ros::init(argc, argv, PROJECT_NAME);
    std::string pkg_path = ros::package::getPath(PROJECT_NAME);
    ros::NodeHandle nh;
    ros::NodeHandle pnh("~"); 

    perception::LocationReceiver::instance().init(nh, "/location_msg");
    perception::Nsu_ar0231_Node node(nh, pnh);
    npu_init((char*)pkg_path.c_str());
    node.spin();

    return EXIT_SUCCESS;
}

