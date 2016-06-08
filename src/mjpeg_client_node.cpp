#include <mjpeg_client/mjpeg_client.hpp>
#include <ros/init.h>
#include <ros/names.h>
#include <ros/this_node.h>

int main(int argc, char *argv[]) {
    ros::init(argc, argv, "mjepg_client_node", ros::init_options::AnonymousName);

    ros::V_string args;
    ros::removeROSArgs(argc, argv, args);

    mjpeg_client::MjpegClient node;
    node.init(ros::this_node::getName(), ros::names::getRemappings(), args);

    ros::spin();
    
    return 0;
}