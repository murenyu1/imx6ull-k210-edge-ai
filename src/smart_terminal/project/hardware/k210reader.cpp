#include "k210reader.h"

#include <QDateTime>
#include <QRegularExpression>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ---- 调优常量 ---- */
/* poll 分片时长：既用于读等待，也决定 stop() 的最坏返回延迟（约等于本值） */
static const int POLL_SLICE_MS    = 200;
/* 单帧组装缓冲上限：协议单帧 ≤ 32 字节，留 2 倍余量；超限即判非法并清空 */
static const int MAX_FRAME_BYTES   = 64;
/* 打开失败后的重试节奏：每 200ms 检查一次停止标志，共重试约 2s 再重来 */
static const int RETRY_SLICE_MS   = 200;
static const int RETRY_SLICES     = 10;
/* #FACE / #HB 频率统计窗口 */
static const int RATE_INTERVAL_MS = 2000;

K210Reader::K210Reader(const QString &device, QObject *parent)
    : QThread(parent)
    , m_device(device)
    , m_running(true)
{
    /* 不在构造里 start()：调用方需先连接信号再启动，否则会漏掉启动初期的
     * statusText / linkAlive(false)。 */
}

K210Reader::~K210Reader()
{
    stop();
}

void K210Reader::stop()
{
    m_running = false;
    if (isRunning())
        wait();
}

void K210Reader::start(Priority priority)
{
    /* 见头文件说明：复位运行标志，使 stop() 后可再次 start() */
    m_running = true;
    QThread::start(priority);
}

void K210Reader::setDevice(const QString &device)
{
    /* 先停线程并 wait()：返回后工作线程已退出、fd 已关闭，改设备与统计无竞争 */
    stop();
    m_device = device;

    /* 换链路 ⇒ 上一接口的在线状态、计数与频率窗口全部作废，
     * 否则界面会把上一接口的读数当成当前接口的（例如仍显示"在线"或旧帧率）。 */
    m_valid = 0;
    m_invalid = 0;
    m_alive = false;
    m_lastValidMs = 0;
    m_faceFrames = 0;
    m_hbFrames = 0;
    m_faceAtRate = 0;
    m_hbAtRate = 0;
    m_lastRateMs = 0;

    /* 让界面即时反映"新链路尚未就绪"（随后由 start() 后的实际打开结果覆盖） */
    emit stats(0, 0);
    emit linkAlive(false);
}

int K210Reader::openDevice(QString *errMsg) const
{
    const QByteArray path = m_device.toLocal8Bit();

    /* O_NONBLOCK 打开：避免某些串口在无对端时 open 长时间阻塞；
     * 打开后立即清除非阻塞位，读阻塞改由 poll() 超时控制。 */
    int fd = ::open(path.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (errMsg)
            *errMsg = QString::fromLocal8Bit(strerror(errno));
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags != -1)
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tio;
    if (::tcgetattr(fd, &tio) != 0) {
        if (errMsg)
            *errMsg = QString::fromLocal8Bit(strerror(errno));
        ::close(fd);
        return -1;
    }

    /* raw：不做任何字符转换/回显/信号，等价于板端 `stty ... raw -echo`（见 uart-protocol.md §9.4.1） */
    ::cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);    /* 忽略调制解调器控制线，使能接收 */
    tio.c_cflag &= ~CRTSCTS;            /* 无硬件流控 */
    tio.c_cflag &= ~CSTOPB;             /* 1 位停止位 */
    tio.c_cflag &= ~PARENB;             /* 无校验 */
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;                 /* 8 位数据 */
    tio.c_cc[VMIN]  = 1;                /* 至少 1 字节即返回，配合 poll 不会长阻塞 */
    tio.c_cc[VTIME] = 0;

    if (::cfsetispeed(&tio, B115200) != 0 || ::cfsetospeed(&tio, B115200) != 0) {
        if (errMsg)
            *errMsg = QStringLiteral("设置波特率失败");
        ::close(fd);
        return -1;
    }

    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        if (errMsg)
            *errMsg = QString::fromLocal8Bit(strerror(errno));
        ::close(fd);
        return -1;
    }

    /* 丢弃打开前可能残留在缓冲区的旧字节，避免把上一段半帧当数据处理 */
    ::tcflush(fd, TCIFLUSH);
    return fd;
}

void K210Reader::processLine(const QByteArray &line)
{
    /* 合法性判据（uart-protocol.md §7/§10）：帧首 `#`、字段均为合法数值。
     * 字符集限定 ASCII，故用 [0-9] 而非 \d（避免匹配非 ASCII 数字）。 */
    static const QRegularExpression faceRe(
        QStringLiteral("^#FACE,([0-9]+),([0-9]+\\.[0-9]{2})$"));
    static const QRegularExpression hbRe(
        QStringLiteral("^#HB,([0-9]+)$"));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString s = QString::fromLatin1(line);
    bool valid = false;
    bool isFace = false;

    QRegularExpressionMatch m = faceRe.match(s);
    if (m.hasMatch()) {
        const int count = m.captured(1).toInt();
        const double score = m.captured(2).toDouble();
        /* 置信度须落在协议约定的 0.00~1.00 区间 */
        if (score >= 0.0 && score <= 1.0) {
            valid = true;
            isFace = true;
            emit faceEvent(count, score);
        }
    } else {
        m = hbRe.match(s);
        if (m.hasMatch()) {
            valid = true;
            emit heartbeat(m.captured(1).toInt());
        }
    }

    if (valid) {
        ++m_valid;
        if (isFace)
            ++m_faceFrames;
        else
            ++m_hbFrames;
        m_lastValidMs = now;
        if (!m_alive) {
            m_alive = true;
            emit linkAlive(true);
        }
    } else {
        ++m_invalid;
    }

    /* 每处理一行即发出累计统计：数据停止时界面也能拿到最终准确值
     * （界面侧按渲染文本是否变化去重，避免无谓重绘）。 */
    emit stats(m_valid, m_invalid);

    /* #FACE / #HB 频率：按固定窗口计算，窗口结束时发一次 */
    if (m_lastRateMs == 0) {
        m_lastRateMs = now;
    } else if (now - m_lastRateMs >= RATE_INTERVAL_MS) {
        const double secs = static_cast<double>(now - m_lastRateMs) / 1000.0;
        emit frameRate((m_faceFrames - m_faceAtRate) / secs,
                       (m_hbFrames - m_hbAtRate) / secs);
        m_faceAtRate = m_faceFrames;
        m_hbAtRate = m_hbFrames;
        m_lastRateMs = now;
    }
}

void K210Reader::run()
{
    while (m_running) {
        QString err;
        const int fd = openDevice(&err);
        if (fd < 0) {
            /* 优雅降级：报告不可用并离线，不崩溃；随后周期重试（设备后插入也能自愈） */
            emit statusText(QStringLiteral("%1 不可用：%2").arg(m_device, err));
            m_alive = false;
            emit linkAlive(false);
            for (int i = 0; i < RETRY_SLICES && m_running; ++i)
                msleep(RETRY_SLICE_MS);
            continue;
        }

        emit statusText(QStringLiteral("已打开 %1，等待数据").arg(m_device));

        bool discardFirstFragment = true;   /* 打开时可能落在某帧中间，首行残片不计非法 */
        QByteArray buf;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;

        while (m_running) {
            /* 超时判定必须与 poll 是否就绪**无关**：只有合法帧刷新 m_lastValidMs，
             * 若一直收到非法帧，poll 每次都就绪、超时分支永不进入，链路会永远显示在线。
             * 故每轮迭代都判一次（判据＝"超过 5s 无合法帧"，见 uart-protocol.md §7/§11.2）。 */
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_alive && m_lastValidMs > 0
                    && now - m_lastValidMs > LINK_TIMEOUT_MS) {
                m_alive = false;
                emit linkAlive(false);
                emit statusText(QStringLiteral("%1 链路超时（超过 %2s 无合法帧）")
                                .arg(m_device).arg(LINK_TIMEOUT_MS / 1000));
            }

            pfd.revents = 0;
            const int pr = ::poll(&pfd, 1, POLL_SLICE_MS);

            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                emit statusText(QStringLiteral("读取 %1 出错：%2")
                                .arg(m_device, QString::fromLocal8Bit(strerror(errno))));
                break;   /* 跳出并重开 */
            }

            if (pr == 0)
                continue;

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                emit statusText(QStringLiteral("%1 已断开").arg(m_device));
                break;   /* 跳出并重开 */
            }
            if (!(pfd.revents & POLLIN))
                continue;

            char chunk[256];
            const ssize_t n = ::read(fd, chunk, sizeof(chunk));
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                emit statusText(QStringLiteral("读取 %1 出错：%2")
                                .arg(m_device, QString::fromLocal8Bit(strerror(errno))));
                break;
            }
            if (n == 0)
                continue;

            for (ssize_t i = 0; i < n; ++i) {
                const char c = chunk[i];
                if (c != '\n') {
                    buf.append(c);
                    if (buf.size() > MAX_FRAME_BYTES) {
                        /* 长时间无帧尾：判非法并清空，避免缓冲无界增长 */
                        ++m_invalid;
                        buf.clear();
                        discardFirstFragment = false;
                    }
                    continue;
                }

                /* 帧尾：协议只发 LF，此处容错去掉可能存在的 CR */
                if (buf.endsWith('\r'))
                    buf.chop(1);

                /* 原始行发给界面终端区（含非法帧与首行残片）：终端要忠实反映收到的文本，
                 * 走 USB/REPL 通道时的 REPL 噪声也因此肉眼可见（其不参与解析，只计非法帧）。 */
                if (!buf.isEmpty())
                    emit rawLine(QString::fromLatin1(buf));

                if (discardFirstFragment) {
                    discardFirstFragment = false;
                    /* 首行不是以 '#' 开头 → 判为帧中段残片，丢弃且不计非法
                     * （与 uart-protocol.md §9.4.1「首尾半帧不计乱帧」口径一致） */
                    if (!buf.startsWith('#')) {
                        buf.clear();
                        continue;
                    }
                }

                if (!buf.isEmpty())
                    processLine(buf);

                buf.clear();
            }
        }

        ::close(fd);
        /* 连接结束：离线告警（重开成功后由首帧重新置在线） */
        if (m_alive) {
            m_alive = false;
            emit linkAlive(false);
        }
    }
}
