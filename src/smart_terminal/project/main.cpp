#include "mainwidget.h"

#include <QApplication>
#include <QFile>
#include <QGuiApplication>   // platformName()：判定当前显示平台（linuxfb / eglfs / xcb …）
#include <QSocketNotifier>   // 用 socketpair 把 Unix 信号安全地转成 Qt 事件

#include <csignal>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* ---- Unix 信号 → Qt 事件（Ctrl+C / kill 优雅退出）----
 * 约定：信号处理函数里只做异步信号安全操作（write 一个字节），
 * 真正的退出动作（QCoreApplication::quit()）放到 QSocketNotifier 的槽里执行，
 * 避免在信号上下文中调用 Qt API。 */
static int sigFd[2] = { -1, -1 };

static void unixSignalHandler(int)
{
    /* 信号可能中断主线程任意系统调用，需保存/恢复 errno，避免污染被中断代码的 errno */
    int savedErrno = errno;
    char token = 1;
    /* 写端已设为非阻塞，信号上下文中不会阻塞；忽略返回值（只需唤醒读端） */
    ssize_t n = ::write(sigFd[1], &token, sizeof(token));
    (void)n;
    errno = savedErrno;
}

/* ---- 退出时清屏（仅 linuxfb）----
 * 板端无窗口管理器：程序退出后 /dev/fb0 里仍留着最后一帧画面。这里直接在事件
 * 循环结束、窗口销毁之后把 framebuffer 整块填 0（黑），避免画面残留。
 *
 * 约束（重要）：清屏只是"锦上添花"，任何一步失败都必须**静默跳过**——
 * 绝不能因此崩溃、报错刷屏或阻塞退出。eglfs/xcb/offscreen 下也不得执行：
 * eglfs 的显示由 GPU/DRM 管理，绕过去直接写 fb0 可能破坏显示栈。 */
static void clearFramebufferOnExit()
{
    /* 只在 linuxfb 后端清屏（命令行 -platform 与环境变量两种设置方式都覆盖） */
    if (QGuiApplication::platformName() != QLatin1String("linuxfb"))
        return;

    int fd = ::open("/dev/fb0", O_RDWR);
    if (fd < 0)
        return;                       /* 无 fb0 或权限不足：跳过 */

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (::ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0
            || ::ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        ::close(fd);
        return;                       /* 不是 framebuffer 设备：跳过 */
    }

    /* smem_len 是整块显存长度，含虚拟分辨率（可能双缓冲）——一并清掉，
     * 避免"清了可见页、翻页后仍露出旧画面"。 */
    const size_t len = finfo.smem_len;
    if (len == 0) {
        ::close(fd);
        return;
    }

    void *base = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        ::close(fd);
        return;
    }

    ::memset(base, 0, len);

    ::munmap(base, len);
    ::close(fd);
}

static void setupUnixSignalHandlers()
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigFd) != 0)
        return;

    /* 写端设为非阻塞：信号上下文里 write 不会阻塞（缓冲充足时本不会阻塞，此处加固） */
    int flags = ::fcntl(sigFd[1], F_GETFL, 0);
    if (flags != -1)
        ::fcntl(sigFd[1], F_SETFL, flags | O_NONBLOCK);

    QSocketNotifier *notifier = new QSocketNotifier(sigFd[0], QSocketNotifier::Read);
    /* 注：Qt 5.15+ 为 activated 增加 (QSocketDescriptor, Type) 重载，此处按项目锁定的
     * Qt 5.12 取 int 重载；若日后升级 Qt，请改为 QOverload<int>::of(&QSocketNotifier::activated)。 */
    QObject::connect(notifier, &QSocketNotifier::activated, [](int) {
        char token;
        ssize_t n = ::read(sigFd[0], &token, sizeof(token));
        (void)n;
        QCoreApplication::quit();   // 在事件循环线程中安全退出
    });

    struct sigaction sa;
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    /* 让 Ctrl+C(SIGINT) 与 kill(SIGTERM) 能优雅退出（Qt GUI 程序默认对 Ctrl+C 无反应）。
     * 注：quit() 只退出事件循环，不发关闭事件、不主动释放窗口内资源，
     *     资源清理依赖下面 MainWidget w 在 main 返回时的析构（既有属性）。 */
    setupUnixSignalHandlers();

    QWidget window;
    window.setWindowTitle("My Window");

    /* 指定文件 */
    QFile file(":/style/mainstyle.qss");

    /* 判断文件是否存在 */
    if (file.exists() ) {
        /* 以只读的方式打开 */
        file.open(QFile::ReadOnly);
        /* 以字符串的方式保存读出的结果 */
        QString styleSheet = QLatin1String(file.readAll());
        /* 设置全局样式 */
        qApp->setStyleSheet(styleSheet);
        /* 关闭文件 */
        file.close();
    }

    int rc = 0;
    {
        MainWidget w;
        /* 全屏显示：板端无窗口管理器，窗口本就没有标题栏/边框，因此
         * 不需要 Qt::FramelessWindowHint（加了反而会改变 x86 桌面调试的外观）。 */
        w.showFullScreen();
        rc = a.exec();
    }   /* 窗口与其子对象在此析构（子窗口析构里的停线程/关 fd 都在此完成） */

    /* 顺序刻意如此：事件循环已结束、窗口已销毁，此时清屏才不会被随后的
     * 析构或重绘覆盖（若放在 aboutToQuit 里，窗口销毁过程仍在事件循环内）。 */
    clearFramebufferOnExit();
    return rc;
}
