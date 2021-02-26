![image](https://github.com/baoachun/tiny-tensorrt/tree/master/docs/tiny-tensorrt.png)


# Note
This project is forked from [tiny-tensorrt](https://github.com/zerollzeng/tiny-tensorrt), thanks zerollzeng!

# System Requirements
cuda 10.0+

TensorRT 6 or 7

For python api, python 2.x/3.x and numpy in needed.

# Installation
Make sure you had install dependencies list above, if you are familiar with docker, you can use [official docker](https://ngc.nvidia.com/catalog/containers/nvidia:tensorrt)
```bash
# clone project and submodule
git clone --recurse-submodules -j8 https://github.com/baoachun/tiny-tensorrt.git

cd tiny-tensorrt

mkdir build && cd build 

cmake .. -DTENSORRT_INCLUDE_DIR="your tensorrt include path"

make -j8

# Or use the python API. You should copy the so files from your_tensorrt_path/lib to /usr/local/lib/
cmake .. -DTENSORRT_INCLUDE_DIR="your tensorrt include path" -DBUILD_PYTHON=ON
```
Then you can intergrate it into your own project with libtinytrt.so and Trt.h, for python module, you get pytrt.so.

# Docs

[User Guide](https://github.com/baoachun/tiny-tensorrt/tree/master/docs/UserGuide.md)

[Custom Plugin Tutorial](https://github.com/baoachun/tiny-tensorrt/tree/master/docs/CustomPlugin.md) (En-Ch)

If you want some examples with tiny-tensorrt, you can refer to [tensorrt-zoo](https://github.com/zerollzeng/tensorrt-zoo)

For the windows port of tiny-tensorrt, you can refer to @Devincool's [repo](https://github.com/Devincool/tiny-tensorrt)

# Extra Support layer
- upsample with custom scale, under test with yolov3.
- yolo-det, last layer of yolov3 which sum three scales output and generate final result for nms. under test with yolov3.
- PRELU, under test with openpose and mtcnn.

# About License
For the 3rd-party module and TensorRT, maybe you need to follow their license.

For the part I wrote, you can do anything you want.

