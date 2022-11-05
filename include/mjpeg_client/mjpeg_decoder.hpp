#ifndef MJPEG_CLIENT_MJPEG_DECODER_HPP
#define MJPEG_CLIENT_MJPEG_DECODER_HPP

#include <cv_bridge/cv_bridge.h>
#include <nodelet/nodelet.h>
#include <ros/node_handle.h>
#include <ros/publisher.h>
#include <ros/subscriber.h>
#include <sensor_msgs/CompressedImage.h>
#include <sensor_msgs/Image.h>

#include <opencv2/highgui/highgui.hpp>

namespace mjpeg_client {

class MjpegDecoder : public nodelet::Nodelet {
public:
  MjpegDecoder() {}

  virtual ~MjpegDecoder() {
    // finalize subscriber before publisher
    subscriber_.shutdown();
  }

private:
  virtual void onInit() {
    // initialize publisher for decoded images and subscriber for compressed images
    ros::NodeHandle &nh = getNodeHandle();
    publisher_ = nh.advertise<sensor_msgs::Image>("image", 1, true);
    subscriber_ = nh.subscribe("image/compressed", 1, &MjpegDecoder::onJpegSubscribed, this);
  }

  void onJpegSubscribed(const sensor_msgs::CompressedImage::ConstPtr &msg) {
    // decode an image and publish
    cv_bridge::CvImage image;
    image.header = msg->header;
    image.encoding = "bgr8";
    image.image = cv::imdecode(msg->data, cv::IMREAD_UNCHANGED);
    if (!image.image.empty()) {
      publisher_.publish(image.toImageMsg());
    } else {
      NODELET_WARN("On decoding: not a valid jpeg image");
    }
  }

private:
  ros::Publisher publisher_;
  ros::Subscriber subscriber_;
};
} // namespace mjpeg_client

#endif // MJPEG_CLIENT_MJPEG_DECODER_HPP