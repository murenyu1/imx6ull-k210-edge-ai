#ifndef MAINWIDGET_H
#define MAINWIDGET_H

#include <QWidget>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QVector>
#include <functional>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWidget; }
QT_END_NAMESPACE

class MainWidget : public QWidget
{
    Q_OBJECT

public:
    MainWidget(QWidget *parent = nullptr);
    ~MainWidget();

private:
    Ui::MainWidget *ui;

    /* widget小部件 */
    QWidget *widget;
    /* 水平布局 */
    QHBoxLayout *hBoxLayout;
    /* 列表视图 */
    QListWidget *listWidget;
    /* 堆栈窗口部件 */
    QStackedWidget *stackedWidget;
    /* 3个标签 */
    QLabel *label[3];

    /* 子窗口实例（惰性创建：未创建时对应项为 nullptr，stackedWidget 中先放占位符 */
    QVector<QWidget *> m_pages;
    /* 子窗口创建函数（与 m_pages 一一对应） */
    QVector<std::function<QWidget *()>> m_creators;

    /* 按需实例化第 index 个子窗口（点击进入时才构造，避免启动期执行其构造中的 IO） */
    void ensurePageCreated(int index);

    void windows_init();


};
#endif // MAINWIDGET_H
