# mjpeg_client
A ROS nodelet to receive a mjpeg stream and convert it to an image topic

## Published Topics
image (sensor_msgs/Image)
* subtopics supported by image_transport are also published

## Parameters
~server (string, defalut: "127.0.0.1")
* IP address or domain name of the mjpeg server

~path (string, defalut: "/")
* path to mjpeg streaming page
* "http://\<server>/\<path>" must be available

~authorization (string, defalut: "")
* base64-encoded string for server authorization
* "echo -n username:password | base64"

~timeout (double, default: 3.0)
* timeout for communication to the server
* when timeout reached, the connection is reset

~frame_id (string, default: "web_camera")
* frame id of published images

~encoding (string, default: "bgr8")
* encoding of published images

## Examples
see [launch/test.launch](launch/test.launch)
