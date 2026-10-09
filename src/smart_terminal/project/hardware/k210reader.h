#ifndef K210READER_H
#define K210READER_H

#include <QThread>
#include <QString>
#include <QByteArray>

#include <atomic>

/*
 * K210Reader —— K210 视觉帧串口读取线程（P3-T10）
 *
 * 职责：用 POSIX termios + QThread 打开**可配置**的串口设备节点，按 UART 帧协议
 *      （权威定义见 src/k210/k210_vision.py 文件头，人读版见 docs/uart-protocol.md §3/§4）
 *      逐行解析 `#FACE` / `#HB` 帧，丢弃并计数非法帧，再通过信号交给界面层。
 *
 * 协议要点：115200 8N1、无流控；帧格式 `#<TYPE>,<fields>\n`；
 *      #FACE,<count>,<max_score>   count 非负整数、max_score 0.00~1.00 两位小数
 *      #HB,<uptime_s>              uptime_s 非负整数
 *
 * 健壮性（P0-T3b 之后坚持的原则）：
 *   - 设备不存在 / 权限不足 / 中途读错误：**不崩溃**，发 linkAlive(false) 与 statusText()，
 *     并周期性重试打开（断开后可自愈重连）；
 *   - 组装缓冲超长（长时间收不到 '\n'）：丢弃该段并计非法帧，避免缓冲无界增长。
 *
 * 线程生命周期：run() 内所有阻塞点都分片（poll 超时 200ms；打开失败的重试等待也分片），
 *     故 stop() 最坏约 200ms 即可返回；run() 退出前必定 close(fd)。
 *     析构函数会自动 stop()，保证线程与 fd 都被回收。
 *
 * 用法：构造后先连接信号；调用 start() 开始读取，stop() 停止。
 *     运行期换设备节点用 setDevice()（内部先 stop() 回收 fd，再等待 start()）。
 */
class K210Reader : public QThread
{
    Q_OBJECT

public:
    explicit K210Reader(const QString &device = QStringLiteral("/dev/ttymxc2"),
                        QObject *parent = nullptr);
    ~K210Reader() override;

    /* 请求停止并等待线程退出（幂等，可重复调用；未启动时也安全） */
    void stop();

    /* 启动读取。
     * 覆盖 QThread::start 只为复位运行标志：stop() 会把它清零，若不复位，
     * stop() 之后再 start() 的线程会因 run() 的 while 条件立即为假而马上退出。
     * 首次启动与重复启动（stop→setDevice→start）行为一致。 */
    void start(Priority priority = InheritPriority);

    /* 运行期切换设备节点：先 stop()（wait 回收 fd），再设置新节点并重置链路状态
     * （在线标志、合法/非法计数、频率窗口），随后须调用方再 start() 才会读取。
     * stop() 返回后工作线程已退出，故此处读写成员无数据竞争。 */
    void setDevice(const QString &device);

    /* 当前使用的设备节点 */
    QString devicePath() const { return m_device; }

    /* 心跳/数据超时判链路离线的时间（毫秒），与 docs/uart-protocol.md §4.2 建议一致 */
    static const int LINK_TIMEOUT_MS = 5000;

signals:
    /* 收到一帧合法 #FACE */
    void faceEvent(int count, double score);
    /* 收到一帧合法 #HB：脚本运行秒数 */
    void heartbeat(int uptimeSec);
    /* 链路在线状态变化（打不开 / 超时无帧 = 离线；收到合法帧 = 在线） */
    void linkAlive(bool alive);
    /* 合法帧 / 非法帧累计数（每处理一行即发出；界面侧按文本变化自行去重重绘） */
    void stats(int validFrames, int invalidFrames);
    /* #FACE / #HB 到达频率（帧/秒，按约 2s 滑动窗口计算；每窗口结束发一次） */
    void frameRate(double faceFps, double hbFps);
    /* 人类可读的状态说明（连接失败原因、断开提示等），供界面显示 */
    void statusText(const QString &text);
    /* 收到的每一行原始文本（不含换行符；含非法帧与连接时的首行残片），
     * 供界面"串口终端"区逐行忠实显示。空行不发。 */
    void rawLine(const QString &text);

protected:
    void run() override;

private:
    /* 打开并按 termios 配置设备；成功返回 fd，失败返回 -1 并把原因写入 errMsg */
    int openDevice(QString *errMsg) const;

    /* 处理一行（不含换行符）：合法则发事件信号，非法则计非法帧 */
    void processLine(const QByteArray &line);

    QString m_device;
    std::atomic<bool> m_running;

    /* 以下成员仅在 run() 所在线程内访问，无需加锁 */
    int m_valid = 0;            /* 合法帧累计 */
    int m_invalid = 0;          /* 非法帧累计 */
    bool m_alive = false;       /* 当前是否判定在线 */
    qint64 m_lastValidMs = 0;   /* 最近一次合法帧时刻（用于超时判离线） */

    int m_faceFrames = 0;       /* #FACE 帧累计（用于频率统计） */
    int m_hbFrames = 0;         /* #HB 帧累计（用于频率统计） */
    int m_faceAtRate = 0;       /* 上一频率窗口起点的 #FACE 计数 */
    int m_hbAtRate = 0;         /* 上一频率窗口起点的 #HB 计数 */
    qint64 m_lastRateMs = 0;    /* 上一频率窗口起点时刻 */
};

#endif // K210READER_H
