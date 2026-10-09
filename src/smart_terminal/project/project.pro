QT       += core gui network multimedia multimediawidgets charts sql

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11
#CONFIG += console

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwidget.cpp

HEADERS += \
    mainwidget.h

FORMS += \
    mainwidget.ui


# ---- 平台相关配置：按 QT_ARCH 自动选择 x86 / arm（原上游为手工注释切换，且含原作者绝对路径）----
CONFIG += link_pkgconfig

contains(QT_ARCH, arm)|contains(QT_ARCH, arm64) {
    # arm（IMX6ULL 交叉编译；Qt 来自正点原子 SDK 的 sysroot）
    #
    # opencv 用**工程内 vendored 3.4.1 ARM 库**，不用 SDK sysroot 里的 opencv（3.1），两条理由：
    #   ① 板端 /home/root/project/lib 就位的就是这一套 3.4（SONAME libopencv_*.so.3.4），链路自洽；
    #   ② sysroot 的 opencv 3.1 会带入几十项 BSP 传递依赖（gtk-3/gstreamer/ffmpeg/gphoto2/libtbb…），
    #      且其头文件（usr/include/opencv2）会与 vendored 3.4 头混用 —— 故这里也不再 include sysroot 的 usr/include。
    # 该目录只有 .so.3.4.1（无 .so / .so.3.4 短名链接），故与下面 mqtt 库同样用显式文件路径；
    # 链接器会按库自身 SONAME 记录为 libopencv_*.so.3.4，从而与板端文件对上。
    OPENCV_ARM = $$PWD/lib/open_lib/opencv_3.4.1/arm_opencv
    INCLUDEPATH += $$OPENCV_ARM/include
    DEPENDPATH  += $$OPENCV_ARM/include
    unix:!macx: LIBS += \
        $$OPENCV_ARM/lib/libopencv_core.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_highgui.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_imgproc.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_videoio.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_imgcodecs.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_calib3d.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_features2d.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_video.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_ml.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_objdetect.so.3.4.1 \
        $$OPENCV_ARM/lib/libopencv_flann.so.3.4.1

    # mqtt lib（原作者绝对路径改为工程内 vendored 库；arm_qtmqtt/lib 无 .so 短名符号链接，故用显式文件路径）
    unix:!macx: LIBS += $$PWD/lib/arm_qtmqtt/lib/libQt5Mqtt.so.5.12.9
    INCLUDEPATH += $$PWD/lib/arm_qtmqtt/src/.
    DEPENDPATH += $$PWD/lib/arm_qtmqtt/.
} else {
    # x86（桌面编译；opencv 走系统 pkg-config 的 opencv4）
    PKGCONFIG += opencv4
    PKGCONFIG += gtk+-3.0

    # mqtt lib（x86_qtmqtt 未入库，获取与放置方式见根 README；取回后其 src/mqtt 头文件的绝对 include 需同样修正）
    unix:!macx: LIBS += -L$$PWD/lib/x86_qtmqtt/lib/ -lQt5Mqtt
    INCLUDEPATH += $$PWD/lib/x86_qtmqtt/src/.
    DEPENDPATH += $$PWD/lib/x86_qtmqtt/.
}



# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    res.qrc

include(./form/form.pri)
include(./hardware/hardware.pri)

INCLUDEPATH += $$PWD/hardware
INCLUDEPATH += $$PWD/form

#DISTFILES += \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_calib3d.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_calib3d.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_core.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_core.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_dnn.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_dnn.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_features2d.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_features2d.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_flann.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_flann.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_highgui.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_highgui.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_imgcodecs.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_imgcodecs.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_imgproc.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_imgproc.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_ml.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_ml.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_objdetect.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_objdetect.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_photo.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_photo.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_shape.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_shape.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_stitching.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_stitching.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_superres.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_superres.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_video.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_video.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_videoio.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_videoio.so.3.4.1 \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_videostab.so \
#    lib/open_lib/opencv_3.4.1/x86_opencv/lib/libopencv_videostab.so.3.4.1

