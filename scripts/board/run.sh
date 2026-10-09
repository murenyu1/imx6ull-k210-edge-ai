#!/bin/sh
#
# 板端启动包装脚本（IMX6ULL）：停出厂桌面 → 运行本程序 → 退出后恢复桌面
#
# 背景：linuxfb 下没有窗口管理器，"谁后画谁覆盖"。出厂桌面 /opt/QDesktop 会与本程序
#       争抢 /dev/fb0 与输入设备（表现为"露出桌面""点我们的按钮却触发桌面按钮"），
#       故必须先停掉它；而用户希望退出程序后回到桌面，所以在程序退出（无论正常、异常，
#       还是 Ctrl+C / kill 等信号中断）之后把它重新拉起。应用内的"退出清屏"保留：
#       桌面随后重绘覆盖，退出到桌面之间可能有一瞬黑屏，属正常。
#
# 部署：本文件是仓库内模板；板端放置为 /home/root/project/run.sh 并 chmod +x。
#       生成/部署命令见 docs/env-setup.md §3 与 README.md「板端运行行为（linuxfb）」。
# 特性：可重复执行、不残留僵尸进程；任何退出路径（正常/异常/INT/TERM/HUP）都会走恢复桌面分支。
#
# 可用环境变量覆盖（主要便于本机验证）：
#   QDESKTOP_NAME          桌面进程名         （默认 QDesktop）
#   QDESKTOP_PATH          桌面可执行文件路径 （默认 /opt/QDesktop）
#   QDESKTOP_RESTORE_MODE  退出后恢复策略     （默认 always）
#       always     : 只要桌面可执行存在且当前未在运行，退出后一律拉起（**默认**；
#                    覆盖"桌面原本就没跑"的场景——用户按旧清单注释 /etc/rc.local 自启时的情形）
#       if-running : 仅"原本在跑"才恢复（旧语义；供回归测试与"不想无中生有"的场合）
#       never      : 关闭自动恢复
#   APP_PATH               要启动的程序路径   （默认 本脚本同目录下的 project）

APP_DIR=$(cd "$(dirname "$0")" && pwd)
APP=${APP_PATH:-"$APP_DIR/project"}
DESKTOP=${QDESKTOP_PATH:-/opt/QDesktop}
DESKTOP_NAME=${QDESKTOP_NAME:-QDesktop}
RESTORE_MODE=${QDESKTOP_RESTORE_MODE:-always}

# 沿用既有运行环境：无 RPATH 的产物需要库搜索路径；linuxfb 为板端显示后端
export LD_LIBRARY_PATH="$APP_DIR/lib:$LD_LIBRARY_PATH"
export QT_QPA_PLATFORM=linuxfb

desktop_was_running=0
desktop_stop_issued=0
desktop_restored=0
DESKTOP_EXIT_WAIT=5          # 等待桌面退出的上限（秒）：停桌面与恢复前都会用到

# 恢复桌面：**幂等且不漏**——任何调用序列下最多拉起一次，且绝不"漏掉"该拉起的场景。
# 关键：一次性标记 desktop_restored 只在"确已退出、即将/已经拉起"时才置，绝不在早退分支里消耗。
restore_desktop() {
    [ "$desktop_restored" = "1" ] && return 0

    # 是否应恢复：never 一律不恢复；if-running 仅"原本在跑"才恢复；always（默认）总是恢复。
    case "$RESTORE_MODE" in
        never) return 0 ;;
        if-running) [ "$desktop_was_running" = "1" ] || return 0 ;;
    esac

    # 桌面仍在运行时分两种情形：
    #   ① 从未发过停止信号（它本就该在跑，如信号落在"停桌面"之前）→ 无需动作（早退，**不**置一次性标记）；
    #   ② 已发过停止信号（"桌面已被 kill、正在退出"的窗口，信号恰好落在其中）→ 等它真正退出后再拉起。
    if pidof "$DESKTOP_NAME" >/dev/null 2>&1; then
        [ "$desktop_stop_issued" = "1" ] || return 0

        m=0
        while pidof "$DESKTOP_NAME" >/dev/null 2>&1; do
            m=$((m + 1))
            [ "$m" -ge "$DESKTOP_EXIT_WAIT" ] && break
            sleep 1
        done
        # 等待超时仍未退出：与"停桌面"路径一致地强杀一次，确认退出后才拉起
        if pidof "$DESKTOP_NAME" >/dev/null 2>&1; then
            kill -9 $(pidof "$DESKTOP_NAME") 2>/dev/null
            sleep 1
        fi
        # 强杀后仍存活（罕见，如不可中断状态）：如实告知并放弃自动恢复
        if pidof "$DESKTOP_NAME" >/dev/null 2>&1; then
            desktop_restored=1
            echo "[run.sh] 桌面未能停止（强杀后仍在运行），不自动恢复；如需恢复请手动执行 $DESKTOP" >&2
            return 0
        fi
    fi

    # 桌面确已不在运行 → 真正拉起，并在此处（唯一执行点）置一次性标记
    desktop_restored=1
    if [ -x "$DESKTOP" ]; then
        "$DESKTOP" >/dev/null 2>&1 &
    elif [ "$desktop_was_running" = "1" ]; then
        # 只有"停过它却拉不回来"才是需要告知的问题；x86 等本就无桌面的环境静默跳过
        echo "[run.sh] 桌面可执行不存在：$DESKTOP（可手动执行它恢复桌面）" >&2
    fi
}

# 陷阱：**无条件**安装（早于"停桌面"）——无论桌面原本是否在运行，任何退出路径都走 restore_desktop；
# 是否真的拉起由 RESTORE_MODE 与桌面当前状态决定。信号落在"桌面正在退出"的窗口内时，
# restore_desktop 会等它退出后再拉起（不被幂等标记误吞）。
trap 'restore_desktop' EXIT
trap 'restore_desktop; exit 130' INT
trap 'restore_desktop; exit 143' TERM
trap 'restore_desktop; exit 129' HUP

# 1) 桌面原本在运行 → 记录、置"已发停止"，再停掉它（发 TERM）
#    （先置 stop_issued 再 kill：信号恰好落在 kill 前一刻时，恢复段也会等它退出后拉起，不会漏）
if pidof "$DESKTOP_NAME" >/dev/null 2>&1; then
    desktop_was_running=1
    desktop_stop_issued=1
    kill $(pidof "$DESKTOP_NAME") 2>/dev/null
fi

# 2) 若刚停桌面：等待其退出（最多约 5s），避免与我们的程序抢 fb0/触摸；仍未退则强杀一次
if [ "$desktop_was_running" = "1" ]; then
    n=0
    while pidof "$DESKTOP_NAME" >/dev/null 2>&1; do
        n=$((n + 1))
        [ "$n" -ge "$DESKTOP_EXIT_WAIT" ] && break
        sleep 1
    done
    if pidof "$DESKTOP_NAME" >/dev/null 2>&1; then
        kill -9 $(pidof "$DESKTOP_NAME") 2>/dev/null
        sleep 1
    fi
fi

# 3) 前台运行本程序；无论正常/异常返回都继续往下（EXIT 陷阱会恢复桌面）
"$APP"
app_rc=$?

# 4) 恢复桌面由 EXIT 陷阱统一负责（正常/异常/信号各路径一致，避免重复代码）
# 5) 透传程序退出码，便于上层判断程序是否异常退出
exit "$app_rc"
