# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Avoid linking test executables during compiler checks. This helps older
# Android CMake packages configure successfully with newer NDK toolchains.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("ffmpeg_for_android")

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.


#生成的so库在\ffmpegNdkTest\app\build\intermediates\cxx\Debug\6j4d6q1w\obj\arm64-v8a这个目录
add_library(${CMAKE_PROJECT_NAME} SHARED
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        native-lib.cpp
        RtspPlayer.cpp
        ffplay_def.cpp
        ffplay.cpp
        native_error.cpp
        VideoFrameBuffer.cpp
        )

# 接收 Gradle buildType 传入的 native 日志开关参数。
set(NATIVE_ENABLE_FRAME_TRACE 0 CACHE STRING "Enable frame trace logs")
set(NATIVE_ENABLE_QUEUE_TRACE 0 CACHE STRING "Enable queue trace logs")
set(NATIVE_ENABLE_SEI_TRACE 0 CACHE STRING "Enable SEI trace logs")
set(NATIVE_ENABLE_BUFFER_TRACE 0 CACHE STRING "Enable buffer trace logs")

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        NATIVE_ENABLE_FRAME_TRACE=${NATIVE_ENABLE_FRAME_TRACE}
        NATIVE_ENABLE_QUEUE_TRACE=${NATIVE_ENABLE_QUEUE_TRACE}
        NATIVE_ENABLE_SEI_TRACE=${NATIVE_ENABLE_SEI_TRACE}
        NATIVE_ENABLE_BUFFER_TRACE=${NATIVE_ENABLE_BUFFER_TRACE})



#1 设置FFmpeg头文件路径
#C/C++ 编译器默认只会在系统默认路径（如 /usr/include）和当前源文件目录中查找头文件。
#如果头文件放在其他目录（如 src/main/cpp/include），必须通过 include_directories 告诉编译器路径
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

#2 加载动态库
add_library(avcodec SHARED IMPORTED)
# 设置动态库路径 ${CMAKE_SOURCE_DIR}表示CmakeList文件目录 ${ANDROID_ABI}表示系统CPU架构
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libavcodec.so)


add_library(avfilter SHARED IMPORTED)
# 设置动态库路径 ${CMAKE_SOURCE_DIR}表示CmakeList文件目录 ${ANDROID_ABI}表示系统CPU架构
set_target_properties(avfilter PROPERTIES IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libavfilter.so)

add_library(avformat SHARED IMPORTED)
# 设置动态库路径 ${CMAKE_SOURCE_DIR}表示CmakeList文件目录 ${ANDROID_ABI}表示系统CPU架构
set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libavformat.so)

add_library(avutil SHARED IMPORTED)
# 设置动态库路径 ${CMAKE_SOURCE_DIR}表示CmakeList文件目录 ${ANDROID_ABI}表示系统CPU架构
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libavutil.so)

add_library(yuv SHARED IMPORTED)
# 设置动态库路径 ${CMAKE_SOURCE_DIR}表示CmakeList文件目录 ${ANDROID_ABI}表示系统CPU架构
set_target_properties(yuv PROPERTIES IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libyuv.so)


# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        avcodec
        avfilter
        avformat
        avutil
        yuv
        android
        OpenSLES
        log)
