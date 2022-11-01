#ifndef MJPEG_CLIENT_MJPEG_CLIENT_HPP
#define MJPEG_CLIENT_MJPEG_CLIENT_HPP

#include <algorithm>
#include <sstream>
#include <string>

#include <nodelet/nodelet.h>
#include <ros/duration.h>
#include <ros/node_handle.h>
#include <ros/publisher.h>
#include <ros/time.h>
#include <sensor_msgs/CompressedImage.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/bind.hpp>
#include <boost/regex.hpp>
#include <boost/system/error_code.hpp>
#include <boost/thread/thread.hpp>

namespace mjpeg_client {

namespace ba = boost::asio;
namespace bs = boost::system;

class MjpegClient : public nodelet::Nodelet {
public:
  MjpegClient() : resolver_(service_), socket_(service_), timer_(service_) {}

  virtual ~MjpegClient() {
    // stop dispatching asio callbacks
    service_.stop();
    if (service_thread_.joinable()) {
      service_thread_.join();
    }
  }

private:
  virtual void onInit() {
    // get node handles
    ros::NodeHandle &nh = getNodeHandle();
    ros::NodeHandle &pnh = getPrivateNodeHandle();

    // load parameters
    server_ = pnh.param<std::string>("server", "127.0.0.1");
    path_ = pnh.param<std::string>("path", "/");
    authorization_ = pnh.param<std::string>("authorization", "");
    {
      std::ostringstream oss;
      oss << "GET " << path_ << " HTTP/1.0\r\n";
      oss << "Host: " << server_ << "\r\n";
      if (!authorization_.empty()) {
        oss << "Authorization: " << authorization_ << "\r\n";
      }
      oss << "Accept: multipart/x-mixed-replace\r\n";
      oss << "\r\n";
      request_ = oss.str();
    }
    timeout_ = ros::Duration(pnh.param("timeout", 3.)).toBoost();
    frame_id_ = pnh.param<std::string>("frame_id", "web_camera");

    // init image publisher
    publisher_ = nh.advertise<sensor_msgs::CompressedImage>("image/compressed", 1, true);

    // start the operation
    start();

    // start dispatching asio callbacks
    service_thread_ = boost::thread(boost::bind(&ba::io_service::run, &service_));
  }

  void start() {
    // initialize the socket
    if (socket_.is_open()) {
      bs::error_code error;
      socket_.close(error);
      if (error) {
        // print error (can do nothing else)
        NODELET_WARN_STREAM("On starting: " << error.message());
      }
    }

    // initialize the buffer
    response_.consume(response_.size());

    // start the first procedure
    startResolve();
  }

  void startResolve() {
    // start the timer. resolver handlers will be invoked with an error on the timeout.
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onResolverTimeout, this, _1));
    // start resolving. the handler will be invoked on success, timeout, or other errors.
    resolver_.async_resolve(ba::ip::tcp::resolver::query(server_, "http"),
                            boost::bind(&MjpegClient::onResolved, this, _1, _2));
  }

  void onResolved(const bs::error_code &error, ba::ip::tcp::resolver::iterator endpoint_iterator) {
    // cancel the timer
    timer_.cancel();

    // restart the operation on an error (including the timeout)
    if (error) {
      NODELET_ERROR_STREAM("On resolving: " << error.message());
      restart();
      return;
    }

    // start the next procedure on success
    NODELET_INFO_STREAM("Resolved " << server_);
    startConnect(endpoint_iterator);
  }

  void startConnect(ba::ip::tcp::resolver::iterator endpoint_iterator) {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSocketTimeout, this, _1));
    ba::async_connect(socket_, endpoint_iterator, boost::bind(&MjpegClient::onConnected, this, _1));
  }

  void onConnected(const bs::error_code &error) {
    timer_.cancel();

    if (error) {
      NODELET_ERROR_STREAM("On connecting: " << error.message());
      restart();
      return;
    }

    NODELET_INFO_STREAM("Connected to " << socket_.remote_endpoint());
    startRequest();
  }

  void startRequest() {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSocketTimeout, this, _1));
    ba::async_write(socket_, ba::buffer(request_),
                    boost::bind(&MjpegClient::onRequested, this, _1, _2));
  }

  void onRequested(const bs::error_code &error, const std::size_t bytes) {
    timer_.cancel();

    if (error) {
      NODELET_ERROR_STREAM("On requesting: " << error.message());
      restart();
      return;
    }

    NODELET_INFO_STREAM("Requested http://" << server_ << path_);
    startReceiveHeader();
  }

  void startReceiveHeader() {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSocketTimeout, this, _1));
    // a header is expected to end by "\r\n\r\n"
    ba::async_read_until(socket_, response_, "\r\n\r\n",
                         boost::bind(&MjpegClient::onHeaderReceived, this, _1, _2));
  }

  void onHeaderReceived(const bs::error_code &error, const std::size_t bytes) {
    timer_.cancel();

    if (error) {
      NODELET_ERROR_STREAM("On receiving header: " << error.message());
      restart();
      return;
    }

    // remove the received header from the buffer
    response_.consume(bytes);

    startReceiveBody();
  }

  void startReceiveBody() {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSocketTimeout, this, _1));
    ba::async_read_until(socket_, response_, "\xff\xd9",
                         boost::bind(&MjpegClient::onBodyReceived, this, _1, _2));
  }

  void onBodyReceived(const bs::error_code &error, const std::size_t bytes) {
    timer_.cancel();

    if (error) {
      NODELET_ERROR_STREAM("On receiving body: " << error.message());
      restart();
      return;
    }

    // decode jpeg data in the received message body
    sensor_msgs::CompressedImagePtr msg(new sensor_msgs::CompressedImage());
    msg->header.stamp = ros::Time::now();
    msg->header.frame_id = frame_id_;
    msg->format = "jpeg";
    {
      const char *const body_begin = ba::buffer_cast<const char *>(response_.data());
      const char *const body_end = body_begin + bytes;

      static const char SOI[] = "\xff\xd8"; // start of image
      const char *const jpeg_begin = std::find_first_of(body_begin, body_end, SOI, SOI + 2);

      static const char EOI[] = "\xff\xd9"; // end of image
      const char *const jpeg_end = std::find_end(body_begin, body_end, EOI, EOI + 2) + 2;

      if ((jpeg_begin != body_end) && (jpeg_end != body_end + 2) && (jpeg_begin < jpeg_end)) {
        msg->data.assign(jpeg_begin, jpeg_end);
      }
    }

    // publish the decoded image
    if (!msg->data.empty()) {
      publisher_.publish(msg);
    } else {
      NODELET_WARN("On receiving body: not a valid jpeg image");
    }

    response_.consume(bytes);

    startReceiveBody();
  }

  void onResolverTimeout(const bs::error_code &error) {
    // do nothing if the timeout operation is canceled
    if (error == ba::error::operation_aborted) {
      return;
    }

    // print error (logically not expected)
    if (error) {
      NODELET_ERROR_STREAM("On waiting resolver timeout: " << error.message());
      return;
    }

    // cancel resolver operations if the timeout has reached
    resolver_.cancel();
  }

  void onSocketTimeout(const bs::error_code &error) {
    if (error == ba::error::operation_aborted) {
      return;
    }

    if (error) {
      NODELET_ERROR_STREAM("On waiting socket timeout: " << error.message());
      return;
    }

    socket_.cancel();
  }

  void restart() {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSleepTimeout, this, _1));
  }

  void onSleepTimeout(const bs::error_code &error) {
    if (error == ba::error::operation_aborted) {
      return;
    }

    if (error) {
      NODELET_ERROR_STREAM("On sleeping: " << error.message());
      return;
    }

    start();
  }

private:
  // parameters
  std::string server_;
  std::string path_;
  std::string authorization_;
  std::string request_;
  ba::deadline_timer::duration_type timeout_;
  std::string frame_id_;

  // mutable objects
  ba::io_service service_;
  boost::thread service_thread_;
  ba::ip::tcp::resolver resolver_;
  ba::ip::tcp::socket socket_;
  ba::streambuf response_;
  ba::deadline_timer timer_;
  ros::Publisher publisher_;
};
} // namespace mjpeg_client

#endif // MJPEG_CLIENT_MJPEG_CLIENT_HPP