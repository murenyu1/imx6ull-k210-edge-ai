#include "mainwidget.h"
#include "ui_mainwidget.h"

#include "form/appdeps.h"     // 共享的依赖可用性判定

#include <QCoreApplication>   // 独立"退出"按钮使用 QCoreApplication::quit()
#include <QPushButton>
#include <QVBoxLayout>

#include "opencv.h"
#include "imx6ulltest.h"
#include "facedetect.h"
#include "musicplay.h"
#include "videoplayer.h"
#include "banna.h"
#include "mqtt_client.h"
#include "video_monitor.h"
#include "sqlCLock.h"

//基于Qt的多窗口切换系统



struct WidgetItemData {
    QString title;

    // 使用函数指针，返回QWidget *
    typedef QWidget *(*CreateInstanceFunc)();
    CreateInstanceFunc createInstance;

    // 依赖探测：返回 false 表示"依赖未就绪"。
    // 注意：本项**不做置灰**（置灰会让用户点不进去、提示永远看不到），
    // 改为在标题后**追加可见标注**（如"（缺 myMusic）"），提示仍在进入后弹出。
    typedef bool (*AvailableFunc)();
    AvailableFunc isAvailable;
};

/* ---- 依赖探测（复用共享判定；探测不到一律返回 true，避免误伤）---- */
static bool availAlways()        { return true; }   // 轮播图（qrc 内嵌）/ OpenCV（窗口内已优雅提示）
static bool availImx6ullDev()    { return AppDeps::imx6ullLedOrBeep(); }
static bool availMusic()         { return AppDeps::musicDirExists(); }
static bool availVideo()         { return AppDeps::videoDirExists(); }
static bool availMonitor()       { return AppDeps::monitorCamera(); }
static bool availUnknown()       { return true; }   // 无法判定（MQTT 需 broker、SQL 需驱动）→ 默认可用

/* 导航项统一行高与"退出"按钮高度。
 * 纵向预算（窗口固定 1024×600，hBoxLayout 上下边距约 9px×2，可用约 582px）：
 *   9 × NAV_ITEM_HEIGHT(55) + EXIT_BUTTON_HEIGHT(55) = 550 ≤ 582  → 余量约 32px（最坏取边距 11 时约 28px）。
 * 取 55 而非原 63，是为了让"列表 + 退出按钮"整体装得下（63 时 9×63+60=627 会溢出裁切）。 */
static const int NAV_ITEM_HEIGHT = 55;
static const int EXIT_BUTTON_HEIGHT = 55;

//主窗口类，继承自QWidget，负责管理整体布局和子窗口的加载
MainWidget::MainWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::MainWidget)
{
    //ui->setupUi(this);

    widget = new QWidget(this);
    this->setGeometry(0, 0, 1024, 600);
    this->setObjectName("project");
    widget->resize(this->size());

    this->windows_init();
}

MainWidget::~MainWidget()
{
    delete ui;
}

void MainWidget::windows_init(){
    /* 水平布局实例化，将导航列表和内容区域并排排列 */
    hBoxLayout = new QHBoxLayout();
    /* 堆栈部件实例化，用于管理多个子窗口的叠加显示 */
    stackedWidget = new QStackedWidget();
    /* 列表实例化 */
    listWidget = new QListWidget(this);  //左侧导航栏列表
    listWidget->setStyleSheet("background-color: black;");

    /*
    在创建WidgetItemData数组时即创建了每个窗口的实例。
    如果这些窗口在程序启动时并未立即用到，过早的实例化确实会消耗不必要的资源。
    考虑一种惰性加载（Lazy Loading）的方式，即在需要使用窗口时再将其实例化。
    这种方式需要配合工厂模式（Factory Pattern）使用，通过使用工厂方法在运行时动态创建相应的对象。
    */
    //定义导航项的数据结构，包含：title：导航项显示的文本；createInstance：​​函数指针​​，用于动态创建对应的子窗口（惰性加载的关键）
    WidgetItemData items[] = {
        {"轮播图",          []() -> QWidget * { return new banna; },        availAlways },
        {"OpenCV测试",      []() -> QWidget * { return new opencv; },       availAlways },
        /* 本页已重做为 K210 视觉前端（UART 帧）展示，不再依赖板载 OV 摄像头，
         * 故不再用"是否存在 /dev/video*"标注依赖；串口不可用时页面内自有降级提示。 */
        {"摄像头人脸识别",  []() -> QWidget * { return new faceDetect; },   availAlways },
        {"imx6ull板级设备", []() -> QWidget * { return new imx6ullTest; },  availImx6ullDev },
        {"音频播放器",      []() -> QWidget * { return new MusicPlay; },    availMusic },
        {"视频播放器",      []() -> QWidget * { return new VideoPlayer; },  availVideo },
        {"MQTT_CLIENT",     []() -> QWidget * { return new mqtt_client; },  availUnknown },
        {"FFMPEG_TEST",     []() -> QWidget * { return new Monitor; },      availMonitor },
        {"SQL_TEST",        []() -> QWidget * { return new sqlCLock; },     availUnknown },
    };

    for(auto & itemData : items) {
        /* 依赖未就绪：不置灰（置灰会导致"点不进去、看不到原因"），
         * 改为在标题后追加可见标注，触摸端即可看到；具体提示仍在窗口进入后弹出。 */
        const bool avail = (itemData.isAvailable == nullptr) ? true : itemData.isAvailable();
        QString title = itemData.title;
        if (!avail)
            title += "（依赖未就绪）";

        QListWidgetItem *item = new QListWidgetItem(title);
        item->setTextColor(Qt::white);
        item->setTextAlignment(Qt::AlignCenter);
        item->setSizeHint(QSize(100, NAV_ITEM_HEIGHT));
        if (!avail)
            item->setToolTip(QStringLiteral("依赖的设备/素材未就绪，进入后会给出提示"));
        listWidget->addItem(item);

        // 惰性实例化：此处只登记创建函数并放入轻量占位符，子窗口本身延迟到
        // 用户点击对应导航项时才构造（见 ensurePageCreated），避免启动阶段执行
        // 各子窗口构造函数里的读图/开设备等 IO —— 之前因急切构造导致"缺一张素材图就启动即崩"。
        m_creators.append(itemData.createInstance);
        m_pages.append(nullptr);
        stackedWidget->addWidget(new QWidget());
    }

    // 将焦点设置为第一个项目
    if(listWidget->count() > 0) {
        QListWidgetItem* item1 = listWidget->item(0);
        listWidget->setCurrentItem(item1);
        listWidget->setItemSelected(item1, true);
        item1->setSelected(true);
        listWidget->setFocus();
    }

    /* 设置列表的最大宽度 */
    listWidget->setMaximumWidth(200);

    /* 左侧导航列：列表 + 底部固定的"退出"按钮。
     * "退出"是**动作**而非页面，做成独立按钮后：① 不占 m_pages/stackedWidget 下标，
     * 不再需要行下标特判；② 固定可见，不会因列表项增多而被裁到屏幕外（触摸板端更可靠）。 */
    QVBoxLayout *navLayout = new QVBoxLayout();
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(0);
    navLayout->addWidget(listWidget);

    QPushButton *exitButton = new QPushButton(QStringLiteral("退出"));
    /* 固定高度（不随布局拉伸），保证纵向总量可控：见上方 NAV_ITEM_HEIGHT 处的预算 */
    exitButton->setFixedHeight(EXIT_BUTTON_HEIGHT);
    exitButton->setMaximumWidth(200);
    exitButton->setStyleSheet("QPushButton { background-color: #8b1a1a; color: white; "
                              "font-size: 18px; border: none; }"
                              "QPushButton:pressed { background-color: #b91c1c; }");
    connect(exitButton, &QPushButton::clicked, this, []() {
        /* 退出依赖 main() 中 MainWidget 的析构做清理（quit() 不发送关闭事件） */
        QCoreApplication::quit();
    });
    navLayout->addWidget(exitButton);

    /* 添加到水平布局 */
    hBoxLayout->addLayout(navLayout);
    hBoxLayout->addWidget(stackedWidget);


    /* 将widget的布局设置成hboxLayout */
    widget->setLayout(hBoxLayout);

    /* 切换导航项时：先按需实例化目标子窗口，再切换到它（保持既有切换行为不变）。
     * "退出"已是列表外的独立按钮，此处不再需要任何行下标特判。 */
    connect(listWidget, &QListWidget::currentRowChanged,
            this, [this](int row) {
                ensurePageCreated(row);
                stackedWidget->setCurrentIndex(row);
            });

    /* 首个导航项在前面已被设为当前项（此时信号尚未连接），这里补一次惰性创建，
     * 保证程序启动即显示第一个子窗口。 */
    ensurePageCreated(listWidget->currentRow());
    stackedWidget->setCurrentIndex(listWidget->currentRow());
}

void MainWidget::ensurePageCreated(int index)
{
    /* 越界或已创建则直接返回 */
    if (index < 0 || index >= m_pages.size())
        return;
    if (m_pages.at(index) != nullptr)
        return;

    /* 此时才真正构造子窗口（执行其构造函数，含其中的 IO/设备打开等） */
    QWidget *page = m_creators.at(index)();

    /* 用真实子窗口替换占位符，保持 stackedWidget 的索引与导航项一一对应 */
    QWidget *placeholder = stackedWidget->widget(index);
    stackedWidget->removeWidget(placeholder);
    if (placeholder)
        placeholder->deleteLater();
    stackedWidget->insertWidget(index, page);
    m_pages[index] = page;
}



