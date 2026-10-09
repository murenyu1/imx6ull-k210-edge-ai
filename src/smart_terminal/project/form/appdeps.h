#ifndef APPDEP_H
#define APPDEP_H

/*
 * 应用级依赖可用性判定（共享）
 * 目的：把"某功能所需素材/设备是否存在"的判断集中一处，供导航项标注与各子窗口提示复用，
 *       避免同一判断在多个 .cpp 里各写一遍后发生漂移。
 * 约定：这里只做轻量存在性探测；探测不到时由调用方决定策略（本项目默认放行/仅作标注）。
 */

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QtWidgets/QMessageBox>

namespace AppDeps {

/* 运行目录下是否有 myMusic 目录 */
inline bool musicDirExists()
{
    return QDir(QCoreApplication::applicationDirPath() + "/myMusic").exists();
}

/* 运行目录下是否有 myVideo 目录 */
inline bool videoDirExists()
{
    return QDir(QCoreApplication::applicationDirPath() + "/myVideo").exists();
}

/* 是否存在可用摄像头设备节点 */
inline bool anyCameraDevice()
{
    const QStringList devs = {"/dev/video0", "/dev/video1", "/dev/video2"};
    for (const QString &d : devs)
        if (QFileInfo::exists(d))
            return true;
    return false;
}

/* 板级 LED / 蜂鸣器 sysfs 是否有其一 */
inline bool imx6ullLedOrBeep()
{
    return QFileInfo::exists("/sys/devices/platform/leds/leds/sys-led/brightness")
        || QFileInfo::exists("/sys/devices/platform/leds/leds/beep/brightness");
}

/* FFMPEG 采集所用的摄像头设备 */
inline bool monitorCamera()
{
    return QFileInfo::exists("/dev/video1");
}

}   // namespace AppDeps

namespace AppUi {

/*
 * 延迟弹出一条信息提示。
 * 子窗口在被惰性创建时（构造期）尚未入栈/显示，此时直接弹模态框位置不可控；
 * 用 singleShot(0) 排到事件循环下一轮，待页面真正显示后再提示。
 */
inline void showDeferredHint(QWidget *parent, const QString &title, const QString &text)
{
    QTimer::singleShot(0, parent, [parent, title, text]() {
        QMessageBox::information(parent, title, text);
    });
}

}   // namespace AppUi

#endif // APPDEP_H
