#include "facedetect.h"
#include "ui_facedetect.h"

#include "k210reader.h"   // K210 串口帧读取线程（P3-T10）

#include <QAction>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCursor>

namespace {

/* 两个通道 → 设备节点。默认值即板端实测结论（见 docs/uart-protocol.md §9.4 与「USB_HOST 通道」小节）：
 *   JP6 / UART3 : /dev/ttymxc2 （双板直连，板端已跑通 60s 465 帧 / 0 乱帧）
 *   USB_HOST    : /dev/ttyACM0 （K210 Type-C 插板子 USB Host，枚举为 cdc_acm，**不是 ttyUSB0**）
 * 两者都可用同名环境变量覆盖，便于 x86 用 PTY 伪串口验证而无需改代码。 */
const char *const kEnvJp6Dev = "K210_UART_DEV";   // 沿用旧名，指 JP6/UART3 通道
const char *const kEnvUsbDev = "K210_USB_DEV";
const QString kDefaultJp6Dev = QStringLiteral("/dev/ttymxc2");
const QString kDefaultUsbDev = QStringLiteral("/dev/ttyACM0");

/* 终端区最多保留的行数：限制内存增长（超出后自动丢弃最旧行） */
const int kTerminalMaxLines = 500;

}   // namespace

faceDetect::faceDetect(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::faceDetect)
{
    ui->setupUi(this);

    initUi();
    initReader();
    refreshStatus();
}

faceDetect::~faceDetect()
{
    /* 先停读取线程（内部 wait() 并关闭 fd），保证线程退出后不再向本窗口发信号。
     * m_reader 是本对象的子对象，随后由 Qt 统一 delete，无需手动释放。 */
    if (m_reader)
        m_reader->stop();
    delete ui;
}

void faceDetect::initUi()
{
    /* 可视化区：值列占据剩余宽度 */
    ui->vizLayout->setColumnStretch(1, 1);

    /* 置信度进度条：0.00~1.00 映射到 0~100，数值同时以文本显示在条上 */
    ui->scoreBar->setRange(0, 100);
    ui->scoreBar->setValue(0);
    ui->scoreBar->setFormat(QStringLiteral("--"));

    /* 终端区：只读、等宽、定长回滚 */
    ui->terminalEdit->setReadOnly(true);
    ui->terminalEdit->setMaximumBlockCount(kTerminalMaxLines);
    /* 等宽字体：优先系统 FixedFont，但**校验其确实等宽**——部分环境（本机实测）会把
     * systemFont(FixedFont) 解析成非等宽字体；此时退回 "Monospace" + TypeWriter 提示，
     * 由 Qt 字体匹配挑一个等宽字体（linuxfb 上同样适用）。 */
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!QFontInfo(mono).fixedPitch()) {
        mono.setFamily(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::TypeWriter);
    }
    mono.setPointSize(9);
    ui->terminalEdit->setFont(mono);
    /* "清空终端"放在右键菜单：满足可清屏，又不增加第 4 个主按钮 */
    ui->terminalEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->terminalEdit, &QPlainTextEdit::customContextMenuRequested,
            this, &faceDetect::onTerminalContextMenu);

    connect(ui->startBtn, &QPushButton::clicked, this, &faceDetect::onStartClicked);
    connect(ui->stopBtn, &QPushButton::clicked, this, &faceDetect::onStopClicked);
    connect(ui->deviceBtn, &QPushButton::clicked, this, &faceDetect::onDeviceClicked);

    setReceiving(false);            // 初始为"未接收"，并联动按钮可用性
    updateDeviceButtonText();
}

void faceDetect::initReader()
{
    const QByteArray jp6 = qgetenv(kEnvJp6Dev);
    const QByteArray usb = qgetenv(kEnvUsbDev);
    m_jp6Dev = jp6.isEmpty() ? kDefaultJp6Dev : QString::fromLocal8Bit(jp6);
    m_usbDev = usb.isEmpty() ? kDefaultUsbDev : QString::fromLocal8Bit(usb);

    /* 构造 reader 但**不启动**：用户点"开始人脸识别"才打开串口，
     * 避免进入本页就长期占用设备节点。 */
    m_reader = new K210Reader(currentDevice(), this);
    /* 用指针式连接：签名在编译期校验，避免字符串连接在签名漂移时静默失效 */
    connect(m_reader, &K210Reader::faceEvent, this, &faceDetect::onFace);
    connect(m_reader, &K210Reader::linkAlive, this, &faceDetect::onLinkAlive);
    connect(m_reader, &K210Reader::stats,     this, &faceDetect::onStats);
    connect(m_reader, &K210Reader::frameRate, this, &faceDetect::onFrameRate);
    connect(m_reader, &K210Reader::statusText, this, &faceDetect::onStatusText);
    connect(m_reader, &K210Reader::rawLine,   this, &faceDetect::onRawLine);
    /* 心跳信号（heartbeat）有意不接入可视化区：按要求 #HB 不在可视化区呈现，
     * 其原始帧仍会经 rawLine 出现在下方终端区。 */
}

QString faceDetect::currentDevice() const
{
    return m_useUsb ? m_usbDev : m_jp6Dev;
}

void faceDetect::updateDeviceButtonText()
{
    ui->deviceBtn->setText(m_useUsb ? QStringLiteral("接口：USB_HOST")
                                    : QStringLiteral("接口：JP6"));
    ui->deviceBtn->setToolTip(QStringLiteral("当前节点：%1（点击切换接口）").arg(currentDevice()));
}

void faceDetect::applyDeviceToReader()
{
    /* setDevice 内部先 stop() 回收 fd，再设新节点并重置链路状态（会发 stats(0,0)/linkAlive(false)）。
     * 这里再显式复位本页展示状态，保证不残留上一接口的人脸读数。 */
    m_reader->setDevice(currentDevice());
    m_alive = false;
    m_faceCount = -1;
    m_faceScore = 0.0;
    m_valid = 0;
    m_invalid = 0;
    m_faceFps = -1.0;
    m_statusText.clear();
}

void faceDetect::setReceiving(bool on)
{
    m_receiving = on;
    ui->startBtn->setEnabled(!on);
    ui->stopBtn->setEnabled(on);
}

void faceDetect::onStartClicked()
{
    if (m_receiving)
        return;                      // 幂等：重复点击不重启

    applyDeviceToReader();           // 设节点（并复位显示状态）
    m_reader->start();               // 连接信号已在 initReader 完成，此处不会漏收启动初期信号
    setReceiving(true);
    refreshStatus();
}

void faceDetect::onStopClicked()
{
    if (!m_receiving)
        return;

    m_reader->stop();                // wait 回收线程与 fd；退出时若在线会发 linkAlive(false)
    m_alive = false;
    setReceiving(false);
    refreshStatus();
}

void faceDetect::onDeviceClicked()
{
    m_useUsb = !m_useUsb;
    updateDeviceButtonText();

    if (m_receiving) {
        /* 切换接口：先停（setDevice 内 stop）再以新节点启动；
         * 新节点打不开时由 reader 发 statusText + linkAlive(false)，界面显示"不可用（原因）"，不崩溃。 */
        applyDeviceToReader();
        m_reader->start();
    }
    refreshStatus();
}

void faceDetect::onTerminalContextMenu(const QPoint &pos)
{
    QMenu menu(ui->terminalEdit);
    QAction *clearAct = menu.addAction(QStringLiteral("清空终端"));
    QAction *copyAct  = menu.addAction(QStringLiteral("复制全部"));
    QAction *chosen = menu.exec(ui->terminalEdit->mapToGlobal(pos));

    if (chosen == clearAct) {
        ui->terminalEdit->clear();
    } else if (chosen == copyAct) {
        ui->terminalEdit->selectAll();
        ui->terminalEdit->copy();
        /* 复制后把光标移回末尾，避免保持全选状态影响后续自动滚动 */
        ui->terminalEdit->moveCursor(QTextCursor::End);
    }
}

void faceDetect::onFace(int count, double score)
{
    m_faceCount = count;
    m_faceScore = score;
    refreshStatus();
}

void faceDetect::onLinkAlive(bool alive)
{
    m_alive = alive;
    if (!alive) {
        /* 离线后作废帧率与人脸读数，避免把上一状态当成当前读数（评审 SUGGESTION 1） */
        m_faceFps = -1.0;
        m_faceCount = -1;
        m_faceScore = 0.0;
    }
    refreshStatus();
}

void faceDetect::onStats(int valid, int invalid)
{
    m_valid = valid;
    m_invalid = invalid;
    refreshStatus();
}

void faceDetect::onFrameRate(double faceFps, double hbFps)
{
    Q_UNUSED(hbFps);                 // 心跳帧率不在可视化区呈现（按要求）
    m_faceFps = faceFps;
    refreshStatus();
}

void faceDetect::onStatusText(const QString &text)
{
    m_statusText = text;
    refreshStatus();
}

void faceDetect::onRawLine(const QString &text)
{
    /* 自动滚动：仅当用户本就在底部时才跟随，避免用户上翻查看历史时被强制拉回 */
    QScrollBar *sb = ui->terminalEdit->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 4;
    ui->terminalEdit->appendPlainText(text);
    if (atBottom)
        sb->setValue(sb->maximum());
}

void faceDetect::refreshStatus()
{
    const QString dev = currentDevice();

    /* 人脸状态：只在"正在接收且链路在线"时展示读数，避免残留旧值误导 */
    QString face;
    if (!m_receiving)
        face = QStringLiteral("—（未接收）");
    else if (!m_alive)
        face = QStringLiteral("—（链路离线）");
    else if (m_faceCount < 0)
        face = QStringLiteral("等待 #FACE …");
    else if (m_faceCount > 0)
        face = QStringLiteral("有人脸 %1 个").arg(m_faceCount);
    else
        face = QStringLiteral("无人脸");
    ui->faceStateLabel->setText(face);

    /* 置信度：数值 + 进度条（仅在有新鲜读数时显示） */
    if (m_receiving && m_alive && m_faceCount >= 0) {
        ui->scoreBar->setValue(qBound(0, int(m_faceScore * 100.0 + 0.5), 100));
        ui->scoreBar->setFormat(QString::number(m_faceScore, 'f', 2));
    } else {
        ui->scoreBar->setValue(0);
        ui->scoreBar->setFormat(QStringLiteral("--"));
    }

    /* 链路：在线/离线 + 节点；离线时附上原因（即"不可用（原因）"） */
    QString link;
    if (!m_receiving)
        link = QStringLiteral("未接收 · %1").arg(dev);
    else if (m_alive)
        link = QStringLiteral("在线 · %1").arg(dev);
    else if (m_statusText.isEmpty())
        link = QStringLiteral("离线 · %1").arg(dev);
    else
        link = QStringLiteral("离线 · %1").arg(m_statusText);
    ui->linkLabel->setText(link);

    /* 帧率：只报 #FACE（可视化区不呈现心跳） */
    if (m_receiving && m_alive && m_faceFps >= 0.0)
        ui->fpsLabel->setText(QStringLiteral("#FACE %1 帧/秒").arg(m_faceFps, 0, 'f', 1));
    else
        ui->fpsLabel->setText(QStringLiteral("—"));

    ui->statsLabel->setText(QStringLiteral("合法 %1 / 非法 %2")
                            .arg(m_valid).arg(m_invalid));
}
