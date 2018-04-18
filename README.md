# mjpeg_client
A ROS nodelet to receive a mjpeg stream and convert it to an image topic

## Published Topics
image (sensor_msgs/Image)
* subtopics supported by image_transport are also published

## Parameters
~server (string, defalut: "127.0.0.1")
* IP address or hostname of the mjpeg server

~path (string, defalut: "/")
* path to mjpeg streaming page beggining with "/"
* the URL "http://\<server>\<path>" must be available

~authorization (string, defalut: "")
* base64-encoded string for server authorization
* run "echo -n username:password | base64" in linux to generate this

~timeout (double, default: 3.0)
* timeout in seconds for communication to the server
* when timeout reached, the connection is reset

~frame_id (string, default: "web_camera")
* frame id of published images

~encoding (string, default: "bgr8")
* encoding of published images

## Examples
see [launch/test.launch](launch/test.launch)
