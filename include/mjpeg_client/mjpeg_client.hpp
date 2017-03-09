#ifndef MJPEG_CLIENT_MJPEG_CLIENT_HPP
#define MJPEG_CLIENT_MJPEG_CLIENT_HPP

#include <algorithm>
#include <sstream>
#include <string>

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <image_transport/publisher.h>
#include <nodelet/nodelet.h>
#include <ros/duration.h>
#include <ros/node_handle.h>
#include <ros/time.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/bind.hpp>
#include <boost/regex.hpp>
#include <boost/thread/thread.hpp>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

namespace mjpeg_client {

class MjpegClient : public nodelet::Nodelet {
private:
  typedef boost::asio::ip::tcp Tcp;

public:
  MjpegClient() : resolver_(service_), socket_(service_), timer_(service_) {}

  virtual ~MjpegClient() {
    service_.stop();
    if (service_thread_.joinable()) {
      service_thread_.join();
    }
  }

private:
  virtual void onInit() {
    // get node handles
    ros::NodeHandle &nh(getNodeHandle());
    ros::NodeHandle &pnh(getPrivateNodeHandle());

    // load parameters
    server_ = pnh.param< std::string >("server", "127.0.0.1");
    path_ = pnh.param< std::string >("path", "/");
    authorization_ = pnh.param< std::string >("authorization", "");
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
    frame_id_ = pnh.param< std::string >("frame_id", "web_camera");
    encoding_ = pnh.param< std::string >("encoding", "bgr8");

    // init image publisher
    publisher_ = image_transport::ImageTransport(nh).advertise("image", 1);

    // enable http timeout
    enableTimer();

    // start resolving the given url
    startResolve();

    // start dispatching asio callbacks
    service_thread_ = boost::thread(boost::bind(&boost::asio::io_service::run, &service_));
  }

  void startResolve() {
    setTimer();
    resolver_.async_resolve(Tcp::resolver::query(server_, "http"),
                            boost::bind(&MjpegClient::onResolved, this, _1, _2));
  }

  void onResolved(const boost::system::error_code &error,
                  Tcp::resolver::iterator endpoint_iterator) {
    if (!error) {
      NODELET_INFO_STREAM("Resolved " << server_);
      startConnect(endpoint_iterator);
    } else {
      NODELET_ERROR_STREAM("On resolving: " << error.message());
      startResolve();
    }
  }

  void startConnect(Tcp::resolver::iterator endpoint_iterator) {
    setTimer();
    boost::asio::async_connect(socket_, endpoint_iterator,
                               boost::bind(&MjpegClient::onConnected, this, _1));
  }

  void onConnected(const boost::system::error_code &error) {
    if (!error) {
      NODELET_INFO_STREAM("Connected to " << socket_.remote_endpoint());
      startRequest();
    } else {
      NODELET_ERROR_STREAM("On connecting: " << error.message());
      startResolve();
    }
  }

  void startRequest() {
    setTimer();
    boost::asio::async_write(socket_, boost::asio::buffer(request_),
                             boost::bind(&MjpegClient::onRequested, this, _1, _2));
  }

  void onRequested(const boost::system::error_code &error, const std::size_t bytes) {
    if (!error) {
      NODELET_INFO_STREAM("Requested http://" << server_ << path_);
      startReceiveHeader();
    } else {
      NODELET_ERROR_STREAM("On requesting: " << error.message());
      startResolve();
    }
  }

  void startReceiveHeader() {
    // clear the buffer
    response_.consume(response_.size());

    // start receive a header that always ends "\r\n\r\n"
    setTimer();
    boost::asio::async_read_until(socket_, response_, "\r\n\r\n",
                                  boost::bind(&MjpegClient::onHeaderReceived, this, _1, _2));
  }

  void onHeaderReceived(const boost::system::error_code &error, const std::size_t bytes) {
    if (!error) {
      // parse the received header to pick a delimiter string.
      // the delimiter string must be copied from the buffer
      // because the buffer is cleared just after this.
      std::string delimiter;
      {
        // parse the header
        const char *const header_begin(boost::asio::buffer_cast< const char * >(response_.data()));
        const char *const header_end(header_begin + bytes - 4); // 4 = length of "\r\n\r\n"
        boost::cmatch match;
        static boost::regex boundary_expr(
            "boundary="                                // boundary declaration begins "boundary="
            "(((?<char>[\\w'\\(\\)\\+,-\\./:=\\?])+)|" // can contain alphanumeric chars
                                                       // and some symbols
            "(\"[(?&char) ]*(?&char)\"))");            // if double-quoted, white space is also ok
                                                       // except the last
        if (!boost::regex_search(header_begin, header_end, match, boundary_expr)) {
          NODELET_ERROR_STREAM("On receiving header: "
                               "the received header does not contatin a boundary declaration\n"
                               << "---\n"
                               << std::string(header_begin, bytes) << "\n---");
          startResolve();
          return;
        }
        delimiter = match[1].str();

        // although delimiter is standardized <CRLF "--" boundary>,
        // but some of commercial servers returns a boundary that already contains "--".
        // so we should optionally add "--".
        while (delimiter.substr(0, 2) != "--") {
          delimiter.insert(0, "-");
        }
        delimiter.insert(0, "\r\n");
      }

      // remove the received header from the buffer
      response_.consume(bytes);

      // start receive the preamble
      startReceivePreamble(delimiter);
    } else {
      NODELET_ERROR_STREAM("On receiving header: " << error.message());
      startResolve();
    }
  }

  void startReceivePreamble(const std::string &delimiter) {
    setTimer();
    boost::asio::async_read_until(
        socket_, response_, delimiter,
        boost::bind(&MjpegClient::onPreambleReceived, this, _1, _2, delimiter));
  }

  void onPreambleReceived(const boost::system::error_code &error, const std::size_t bytes,
                          const std::string &delimiter) {
    if (!error) {
      // just ignore the received preamble (this is standard)
      // and then start receiving the first message body
      response_.consume(bytes);
      startReceiveBody(delimiter, 0);
    } else {
      NODELET_ERROR_STREAM("On receiving preamble: " << error.message());
      startResolve();
    }
  }

  void startReceiveBody(const std::string &delimiter, const std::size_t seq) {
    setTimer();
    boost::asio::async_read_until(
        socket_, response_, delimiter,
        boost::bind(&MjpegClient::onBodyReceived, this, _1, _2, delimiter, seq));
  }

  void onBodyReceived(const boost::system::error_code &error, const std::size_t bytes,
                      const std::string &delimiter, const std::size_t seq) {
    if (!error) {
      // decode jpeg data in the received message body
      cv_bridge::CvImage image;
      image.header.seq = seq;
      image.header.stamp = ros::Time::now();
      image.header.frame_id = frame_id_;
      image.encoding = encoding_;
      {
        const char *const body_begin(boost::asio::buffer_cast< const char * >(response_.data()));
        const char *const body_end(body_begin + bytes - delimiter.length());

        static const char SOI[] = {0xff, 0xd8}; // start of image
        const char *const jpeg_begin(std::find_first_of(body_begin, body_end, SOI, SOI + 2));

        static const char EOI[] = {0xff, 0xd9}; // end of image
        const char *const jpeg_end(std::find_end(body_begin, body_end, EOI, EOI + 2) + 2);

        if ((jpeg_begin != body_end) && (jpeg_end != body_end + 2) && (jpeg_begin < jpeg_end)) {
          const cv::_InputArray jpeg(jpeg_begin, jpeg_end - jpeg_begin);
          image.image = cv::imdecode(jpeg, -1 /* decode the data as is */);
        }
      }

      // publish the decoded image
      if (!image.image.empty()) {
        publisher_.publish(image.toImageMsg());
      } else {
        NODELET_WARN_STREAM("On receiving body: message body #"
                            << seq << " does not contain a valid jpeg image");
      }

      // remove the received message body from the buffer
      response_.consume(bytes);

      // start receive the next message body
      startReceiveBody(delimiter, seq + 1);
    } else {
      NODELET_ERROR_STREAM("On receiving body: " << error.message());
      startResolve();
    }
  }

  void enableTimer() { timer_.async_wait(boost::bind(&MjpegClient::onTimerEvent, this, _1)); }

  void setTimer() { timer_.expires_from_now(timeout_); }

  void onTimerEvent(const boost::system::error_code &error) {
    // once the timer enabled, this can be called
    // when the timer reset or expired.
    // although there is nothing to do on reset,
    // but tcp operations should be canceled on expiration
    if (timer_.expires_at() <= boost::asio::deadline_timer::traits_type::now()) {
      resolver_.cancel();
      socket_.close();
      timer_.expires_at(boost::posix_time::pos_infin);
    }
    enableTimer();
  }

private:
  // parameters
  std::string server_;
  std::string path_;
  std::string authorization_;
  std::string request_;
  boost::asio::deadline_timer::duration_type timeout_;
  std::string frame_id_;
  std::string encoding_;

  // mutable objects
  boost::asio::io_service service_;
  boost::thread service_thread_;
  Tcp::resolver resolver_;
  Tcp::socket socket_;
  boost::asio::streambuf response_;
  boost::asio::deadline_timer timer_;
  image_transport::Publisher publisher_;
};
}

#endif // MJPEG_CLIENT_MJPEG_CLIENT_HPP