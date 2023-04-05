# mjpeg_client
ROS nodelets to receive a motion jpeg stream over HTTP and convert it to an image topic

## Nodelet: MjpegClient
receives mjpeg stream over HTTP and convert it to sensor_msgs/CompressedImage

### Published Topics
___image/compressed___ (sensor_msgs/CompressedImage)

### Parameters
___~server___ (string, default: "localhost")
* a descriptive name or a numeric address string of the mjpeg server

___~method___ (string, default: "POST")
* a string corresponding to HTTP request method

___~service___ (string, default: "http")
* a descriptive name (usually "http") or a numeric string corresponding to a port number of the mjpeg server

___~target___ (string, default: "/")
* a string corresponding to HTTP request target

___~headers___ (map<string, string>, default: {{"Accept", "multipart/x-mixed-replace"}})
* a sequence of string pairs corresponding to the field and value of HTTP request header

___~body___ (string, default: "")
* a string corresponding to HTTP request body

___~timeout___ (double, default: 3.0)
* timeout in seconds for communication to the server
* when timeout reached, the connection is reset

___~frame_id___ (string, default: "camera")
* frame id of published images


## Examples
see **[launch/test_axis.launch](launch/test_axis.launch)** for general IP cameras such as AXIS, and **[launch/test_osc.launch](launch/test_osc.launch)** for cameras which support Open Spherical Camera API such as RICOH THETA
