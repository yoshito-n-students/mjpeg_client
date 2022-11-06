#ifndef MJPEG_CLIENT_MJPEG_CLIENT_HPP
#define MJPEG_CLIENT_MJPEG_CLIENT_HPP

#include <map>
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
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/write.hpp>
#include <boost/bind.hpp>
#include <boost/system/error_code.hpp>
#include <boost/thread/thread.hpp>

namespace mjpeg_client {

namespace ba = boost::asio;
namespace bs = boost::system;

class MjpegClient : public nodelet::Nodelet {
public:
  MjpegClient()
      : io_threads_(1), resolver_(io_threads_), socket_(io_threads_), timer_(io_threads_) {}

  virtual ~MjpegClient() {}

private:
  virtual void onInit() {
    // get node handles
    ros::NodeHandle &nh = getNodeHandle();
    ros::NodeHandle &pnh = getPrivateNodeHandle();

    // load parameters
    server_ = pnh.param<std::string>("server", "localhost");
    service_ = pnh.param<std::string>("service", "80");
    const std::string path = pnh.param<std::string>("path", "/");
    const std::map<std::string, std::string> headers =
        pnh.param<std::map<std::string, std::string>>("headers",
                                                      {{"Accept", "multipart/x-mixed-replace"}});
    const std::string body = pnh.param<std::string>("body", "");
    timeout_ = ros::Duration(pnh.param("timeout", 3.)).toBoost();
    frame_id_ = pnh.param<std::string>("frame_id", "camera");

    // compile http request
    {
      std::ostringstream oss;
      oss << "POST " << path << " HTTP/1.1\r\n";
      oss << "Host: " << server_ << "\r\n";
      for (const auto h : headers) {
        oss << h.first << ": " << h.second << "\r\n";
      }
      oss << "\r\n";
      oss << body;
      request_ = oss.str();
    }

    // init image publisher
    publisher_ = nh.advertise<sensor_msgs::CompressedImage>("image/compressed", 1, true);

    // start the operation
    start();
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
    resolver_.async_resolve(server_, service_, boost::bind(&MjpegClient::onResolved, this, _1, _2));
  }

  void onResolved(const bs::error_code &error, ba::ip::tcp::resolver::results_type endpoints) {
    // cancel the timer
    timer_.cancel();

    // restart the operation on an error (including the timeout)
    if (error) {
      NODELET_ERROR_STREAM("On resolving: " << error.message());
      restart();
      return;
    }

    // start the next procedure on success
    NODELET_INFO_STREAM("Resolved {" << server_ << ", " << service_ << "}");
    startConnect(endpoints);
  }

  void startConnect(ba::ip::tcp::resolver::results_type endpoints) {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSocketTimeout, this, _1));
    ba::async_connect(socket_, endpoints, boost::bind(&MjpegClient::onConnected, this, _1, _2));
  }

  void onConnected(const bs::error_code &error, const ba::ip::tcp::endpoint &endpoint) {
    timer_.cancel();

    if (error) {
      NODELET_ERROR_STREAM("On connecting: " << error.message());
      restart();
      return;
    }

    NODELET_INFO_STREAM("Connected to " << endpoint);
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

    NODELET_INFO_STREAM("Requested \"" << request_ << "\"");
    startReceiveBoundary();
  }

  void startReceiveBoundary() {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSocketTimeout, this, _1));
    ba::async_read_until(socket_, response_, "\xff\xd8",
                         boost::bind(&MjpegClient::onBoundaryReceived, this, _1, _2));
  }

  void onBoundaryReceived(const bs::error_code &error, const std::size_t bytes) {
    timer_.cancel();

    if (error) {
      NODELET_ERROR_STREAM("On receiving boundary: " << error.message());
      restart();
      return;
    }

    // remove the received boundary from the buffer
    // (keep last 2 bytes which are SOI of jpeg image)
    response_.consume(bytes - 2);

    startReceiveJpeg();
  }

  void startReceiveJpeg() {
    timer_.expires_from_now(timeout_);
    timer_.async_wait(boost::bind(&MjpegClient::onSocketTimeout, this, _1));
    ba::async_read_until(socket_, response_, "\xff\xd9",
                         boost::bind(&MjpegClient::onJpegReceived, this, _1, _2));
  }

  void onJpegReceived(const bs::error_code &error, const std::size_t bytes) {
    timer_.cancel();

    if (error) {
      NODELET_ERROR_STREAM("On receiving jpeg: " << error.message());
      restart();
      return;
    }

    // pack a jpeg image message
    const sensor_msgs::CompressedImagePtr msg(new sensor_msgs::CompressedImage());
    msg->header.stamp = ros::Time::now();
    msg->header.frame_id = frame_id_;
    msg->format = "jpeg";
    const char *const jpeg_begin = static_cast<const char *>(response_.data().data());
    const std::size_t jpeg_size = 2 + bytes;
    msg->data.assign(jpeg_begin, jpeg_begin + jpeg_size);

    // publish the jpeg image
    publisher_.publish(msg);

    response_.consume(jpeg_size);

    startReceiveBoundary();
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
  std::string server_, service_, request_;
  ba::deadline_timer::duration_type timeout_;
  std::string frame_id_;

  // mutable objects
  ba::thread_pool io_threads_;
  ba::ip::tcp::resolver resolver_;
  ba::ip::tcp::socket socket_;
  ba::streambuf response_;
  ba::deadline_timer timer_;
  ros::Publisher publisher_;
};
} // namespace mjpeg_client

#endif // MJPEG_CLIENT_MJPEG_CLIENT_HPP