#ifndef FACEDETECT_H
#define FACEDETECT_H

#include <QPoint>
#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class K210Reader;

namespace Ui {
class faceDetect;
}

/*
 * faceDetect —— 「人脸识别」页（重做版，2026-10-06）
 *
 * 数据来源不再依赖板载 OV 摄像头，而是 K210 视觉前端的 UART 帧（协议见 docs/uart-protocol.md）：
 *   - 3 个按钮：开始人脸识别 / 结束识别 / 接口切换（JP6 ↔ USB_HOST）；
 *   - 状态可视化区：有人脸 N 个（无人脸）、置信度（数值 + 进度条）、链路在线/离线、帧率；
 *     **不显示心跳**（#HB 不在可视化区呈现，仅原始帧出现在下方终端区）；
 *   - 串口终端区：只读、等宽、自动滚动、定长回滚，逐行显示原始帧文本。
 *
 * 两个通道的设备节点（板端实测，见 docs/uart-protocol.md §9.4 与 USB 通道小节）：
 *   JP6      → /dev/ttymxc2 （板载 UART3，已跑通）
 *   USB_HOST → /dev/ttyACM0 （K210 Type-C 接板子 USB Host，枚举为 cdc_acm，非 ttyUSB0）
 * 两者均可用环境变量覆盖（K210_UART_DEV / K210_USB_DEV），便于 x86 用 PTY 伪串口验证。
 *
 * 健壮性：端口打不开/权限不足/读错/断开时由 K210Reader 优雅降级（发 statusText + linkAlive(false)），
 *         本页只负责呈现，绝不崩溃。
 */
class faceDetect : public QWidget
{
    Q_OBJECT

public:
    explicit faceDetect(QWidget *parent = nullptr);
    ~faceDetect() override;

private slots:
    /* ---- 3 个主按钮 ---- */
    void onStartClicked();
    void onStopClicked();
    void onDeviceClicked();

    /* ---- 终端区右键菜单（"清空终端"，用非按钮方式满足可清屏，不增加第 4 个按钮）---- */
    void onTerminalContextMenu(const QPoint &pos);

    /* ---- K210Reader 信号 ---- */
    void onFace(int count, double score);
    void onLinkAlive(bool alive);
    void onStats(int valid, int invalid);
    void onFrameRate(double faceFps, double hbFps);
    void onStatusText(const QString &text);
    void onRawLine(const QString &text);

private:
    void initUi();
    void initReader();

    /* 把当前通道节点交给 reader（内部先 stop 回收 fd），并复位本页显示的链路相关状态 */
    void applyDeviceToReader();

    /* 依据 m_receiving / m_alive / 最近读数刷新可视化区 */
    void refreshStatus();

    /* 切换"是否接收"状态，并联动按钮可用性 */
    void setReceiving(bool on);

    QString currentDevice() const;
    void updateDeviceButtonText();

    Ui::faceDetect *ui;

    K210Reader *m_reader = nullptr;

    /* ---- 通道 ---- */
    QString m_jp6Dev;                 /* JP6 / UART3 节点 */
    QString m_usbDev;                 /* USB_HOST 节点 */
    bool m_useUsb = false;            /* false=JP6（默认，板端已跑通） */
    bool m_receiving = false;         /* 是否正在接收 */

    /* ---- 展示状态（由 reader 信号驱动）---- */
    bool m_alive = false;
    int m_faceCount = -1;             /* 最近一次 #FACE 的 count（-1=尚未收到） */
    double m_faceScore = 0.0;
    int m_valid = 0;
    int m_invalid = 0;
    double m_faceFps = -1.0;          /* -1=尚无统计；离线时作废，避免显示过期读数 */
    QString m_statusText;             /* 最近一条状态说明（打不开/超时等原因） */
};

#endif // FACEDETECT_H
