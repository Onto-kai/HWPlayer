# HWPlayer

基于 C++、FFmpeg、CUDA 和 OpenGL 的 Windows 多媒体播放器。

## 项目简介

本项目用于学习和实践 C++ 音视频开发，围绕本地媒体文件解析、硬件视频解码和 GPU 图像渲染构建播放器。

## 已实现功能

- 基于 FFmpeg 的媒体文件解析与音视频流识别
- 基于 CUDA/NVDEC 的视频硬件解码
- 支持 NV12 视频帧处理
- 基于 OpenGL 双纹理渲染 Y、UV 分量
- 使用 GLSL 着色器完成视频画面渲染
- 支持从命令行传入本地视频路径

## 技术栈

- C++ / Visual Studio 2022
- FFmpeg
- CUDA / NVDEC
- OpenGL / GLSL
- GLAD
- SDL2

## 运行环境

- Windows
- Visual Studio 2022
- 支持相应硬件解码能力的 NVIDIA GPU
- 与项目配置匹配的 FFmpeg、CUDA、SDL2 和 GLAD 开发依赖

## 编译与运行

1. 安装 Visual Studio 2022 和所需开发依赖。
2. 根据本机目录配置 FFmpeg、SDL2、GLAD 和 CUDA 相关头文件、库文件。
3. 使用 Visual Studio 打开解决方案并编译。
4. 将运行所需的动态库和着色器资源放置到正确位置。
5. 通过命令行传入视频文件路径启动播放器。

示例：

```
hwplayer_win.exe "D:\Videos\demo.mp4"
```

具体依赖版本、库目录和运行配置以项目实际配置为准。

## 项目说明

本项目主要用于学习 FFmpeg 视频解码、CUDA 硬件加速和 OpenGL 视频渲染流程。后续计划完善音视频同步、播放控制、进度拖动和跨环境构建支持。