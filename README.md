# semantic_inference

<div align="center">
   <img src="docs/media/demo_segmentation.png"/>
</div>

This repository provides code for running inference on images with pre-trained models to provide both closed and open-set semantics.
Closed-set and open-set segmentation are implemented as follows:
  - Inference using dense 2D closed-set semantic segmentation models is implemented in c++ using TensorRT
  - Inference using open-set segmentation models and language features is implemented in python

Both kinds of semantic segmentation have a ROS interface associated with them, split between c++ and python as appropriate.

## Dependency
✅ This setup was tested using the Docker image nvcr.io/nvidia/pytorch:22.11-py3.
- CUDA 11.8 (Container)
- TensorRT 8.5.1.7 (Container)
- OpenCV 3.4.2

## Changes
✅ You can check changes [here](https://github.com/MIT-SPARK/semantic_inference/compare/main...cho-jang-hyun:main)

The following files have been modified:

- trt_utilities.cpp: Updated for compatibility with the current TensorRT version.
- image_utilities.cpp: Fixed an issue where an if statement was incorrectly skipped.
- semantic_inference.launch: Configured to use an ONNX file compatible with cv::resize().

## Output with Hydra
![Screenshot from 2025-04-17 16-07-59](https://github.com/user-attachments/assets/1f07368f-4bbe-4d98-bb7a-d357fea5bb30)

## Table of Contents

- [Credits](#credits)
- [Getting started](#getting-started)
  - [Closed-set](docs/closed_set.md#setting-up)
  - [Open-set](docs/open_set.md#setting-up)
- [Usage](#usage)

## Credits

`semantic_inference` was primarily developed by [Nathan Hughes](https://mit.edu/sparklab/people.html) at the [MIT-SPARK Lab](https://mit.edu/sparklab), assisted by [Yun Chang](https://mit.edu/sparklab/people.html), [Jared Strader](https://mit.edu/sparklab/people.html), [Aaron Ray](https://mit.edu/sparklab/people.html), and [Dominic Maggio](https://mit.edu/sparklab/people.html).
A full list of contributors is maintaned [here](contributors.md).
We welcome additional contributions!

## Getting started

The recommended use-case for the repository is with ROS.
We assume some familiarity with ROS in these instructions.
To start, clone this repository into your catkin workspace and run rosdep to get any missing dependencies.
This usually looks like the following:
```bash
cd /path/to/catkin_ws/src
git clone git@github.com:MIT-SPARK/semantic_inference.git
rosdep install --from-paths . --ignore-src -r -y
```

An (optional) quick primer for setting up a minimal workspace is below for those less familiar with ROS.

<details>

<summary>Making a workspace</summary>

First, make sure rosdep is setup:
```bash
# Initialize necessary tools for working with ROS and catkin
sudo apt install python3-catkin-tools python3-rosdep
sudo rosdep init
rosdep update
```

Then, make the workspace and initialize it:
```bash
# Setup the workspace
mkdir -p path/to/catkin_ws/src
cd catkin_ws
catkin init
catkin config -DCMAKE_BUILD_TYPE=Release
```

</details>

Once you've added this repository to your workspace, follow one (or both) of the following setup-guides as necessary:
- [Closed-Set](docs/closed_set.md#setting-up)
- [Open-Set](docs/open_set.md#setting-up)

> **Note** </br>
> Some of our other (larger) packages have or will have more accessible guides to getting `semantic_inference` set up for specific applications, such as [Hydra](https://github.com/MIT-SPARK/Hydra), [Khronos](https://github.com/MIT-SPARK/Khronos) or [Clio](https://github.com/MIT-SPARK/Clio).

## Usage

`semantic_inference` is not intended for standalone usage.
Instead, the intention is for the launch files in `semantic_inference` to be used in a larger project.
More details about including them can be found in the [closed-set](docs/closed_set.md#using-closed-set-segmentation-online) and [open-set](docs/open_set.md#using-open-set-segmentation-online) documentation.
However, it is possible to do something like
```
roslaunch semantic_inference_ros semantic_inference.launch
```
and then
```
rosbag play path/to/rosbag /some/color/image/topic:=/semantic_inference/color/image_raw
```
in a separate terminal to quickly test a particular segmentation model.

> **Note** </br>
> This usage (remapping the rosbag output topic) is a little bit backwards from how remappings from ROS are normally specified and is because launch files are unable to take remappings from the command line.
