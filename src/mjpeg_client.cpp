#include <mjpeg_client/mjpeg_client.hpp>
#include <mjpeg_client/mjpeg_client_osc.hpp>
#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>

PLUGINLIB_EXPORT_CLASS(mjpeg_client::MjpegClient, nodelet::Nodelet);
PLUGINLIB_EXPORT_CLASS(mjpeg_client::MjpegClientOSC, nodelet::Nodelet);