'''
实验名称：K210 人脸检测 + UART 帧发送（CanMV K210 / MicroPython）
文件：src/k210/k210_vision.py
版本：v0.2（P2-T7 人脸检测 + P2-T8 UART 发送端）
日期：2026-09-28

功能：
    1) 用官方预置人脸检测模型（yolo_face_detect.kmodel）跑 KPU 推理，在 LCD 上框出
       人脸并叠加「有人脸 / 无人脸」状态，REPL 串口打印状态变化（P2-T7）；
    2) 每个检测循环按下方帧协议发送 #FACE 帧，并每 2s 发送 #HB 心跳帧（P2-T8）。
       IMX6ULL 侧接收与双板联调属 P2-T9 / P3-T10，不在本脚本实现。

发送通道（SEND_CHANNEL，见配置常量区）：
    "both"（默认）→ **同时**走 UART1/IO7（对端板子 /dev/ttymxc2）与 stdout/REPL（对端 /dev/ttyACM0）；
                    K210 无法感知对端接法，故同时输出，板子侧用「接口」按钮自行选读哪一路；
    "uart"        → 仅 UART1 / IO7（外部排针），对端 IMX6ULL 的 /dev/ttymxc2（JP6 链路）；
    "repl"        → 仅 stdout / REPL（K210 的 USB CDC）。K210 Type-C 插到板子 USB Host 后，
                    板端枚举为 cdc_acm → /dev/ttyACM0（**不是 /dev/ttyUSB0**）。
                    ⚠️ stdout 通道上本脚本的 print() 状态信息也会混入，接收侧按非法帧丢弃并计数
                    （界面"非法帧"不为 0 属正常）。
    三个取值下帧格式与发送时机完全一致，仅输出载体不同；UART 初始化失败不崩溃
    （both 仍保留 stdout 输出，不因单通道失败而整体失效）。

USB 未连接时的 stdout 阻塞防护（both/repl 通用，本任务重点）：
    对端未读 stdout（USB 未连/未枚举）时，USB CDC 缓冲可能写满并拖慢甚至阻塞主循环。
    故对每次 stdout 写用 time.ticks_ms() 计时；单次写耗时 ≥ STDOUT_WRITE_WARN_MS（100ms）
    或写抛异常，即**自动摘除 stdout 端点**（senders 退化为 UART-only）并提示一次，
    检测/显示与心跳继续；UART 通道不受影响。
    防护的硬约束（评审 BLOCKER 后加固）：
      ① 发送顺序**先 UART、后 stdout**——stdout 若阻塞不返回，UART 本帧也已送达；
      ② 摘除与提示**绝不抛异常**（提示通路可能正是坏掉的 stdout，打印失败即静默降级），
         异常绝不逃逸出 send_frame()，故 UART 后续帧与主循环均不受影响。
    边界（脚本无法自救）：若实测"首次 stdout 写即永久阻塞（write 永不返回）"，计时兜底也无从执行，
    按文档回退 SEND_CHANNEL="uart"（见 docs/k210-setup.md §4.5）。

== UART 帧协议 v1（唯一权威定义；docs/uart-protocol.md 为人读版，两者须同步修改）==
    物理层 : 115200 8N1，无流控；K210 仅发送（单向推送，不接收）
    帧格式 : #<TYPE>,<field1>,...<LF>
             帧头 '#'(0x23)，帧尾 '\n'(0x0A)，无长度字段（以帧尾定界）
    编码   : ASCII 可打印字符；载荷字符集限定 [0-9A-Z,.]
    校验   : v1 无校验字段——合法性 = 帧头 + 帧尾 + 各字段均为合法数值；
             非法帧由接收侧丢弃并计数（P3-T10）
    转义   : 无——载荷字符集不含 '#' 与 '\n'，帧定界天然无歧义
    帧类型 :
      #FACE,<count>,<max_score>\n   每个检测循环发送一帧（约 10~15 帧/s）
          count     : 本帧检出人脸数，非负整数（0 表示无人脸）
          max_score : 最高置信度，0.00~1.00，固定 2 位小数（%.2f）；
                      固件无置信度输出时填 0.00，协议不变
      #HB,<uptime_s>\n              每 2000ms 一帧，链路监测
          uptime_s  : 脚本启动以来的秒数，非负整数
    示例 : #FACE,1,0.92\n / #FACE,0,0.00\n / #HB,12\n

API 依据（01Studio 官方 wiki，2026-09-28 抓取）：
    https://wiki.01studio.cc/docs/canmv_k210/machine_vision/face_detection
    https://wiki.01studio.cc/docs/canmv_k210/machine_vision/camera
    https://wiki.01studio.cc/docs/canmv_k210/machine_vision/kpu
    https://wiki.01studio.cc/docs/canmv_k210/basic_examples/lcd
    https://wiki.01studio.cc/docs/canmv_k210/basic_examples/uart

待上板验证点（本机无开发板，未实测，详见 docs/k210-setup.md 与 docs/uart-protocol.md）：
    1. kmodel 加载：优先 SD 卡 /sd/yolo_face_detect.kmodel；无 SD 卡时回退 Flash
       （KMODEL_FLASH_ADDR/LEN），需确认资料包内文件名与大小，并核对 Flash 地址不冲突；
    2. 摄像头镜像/翻转方向：官方人脸检测例程仅 set_vflip(True)；
       set_hmirror(True) 为本脚本待上板确认的预置项，画面镜像不对则删除该行；
    3. regionlayer_yolo2() 返回项的 prob 字段下标（官方文档为第 6 项，下标 5）；
    4. 空结果时 regionlayer_yolo2() 返回 [] 还是 None（本脚本已按两者兼容处理）；
    5. UART 实例与引脚：UART1_TX=IO7 取自官方例程（外部排针），需上板确认其可用、
       与 REPL 所用串口及板载外设不冲突，并核对实际接线引脚后写死本区常量。
    6. USB 未连接时的 stdout 行为：先探测固件是否提供**非阻塞** USB 写 API
       （`dir(machine)` 看有无 `USB_VCP`、`help(sys.stdout)`、`os.dupterm`）；
       有则改用它（缓冲满时丢帧不阻塞），无则依赖本脚本的"写耗时计时 + 自动摘除 stdout"兜底。
       ⚠️ 若实测"首次 stdout 写即永久阻塞"（兜底也救不回），按文档回退 SEND_CHANNEL="uart"。
'''

import os
import sys
import sensor, image, time, lcd, gc
from maix import KPU
from machine import UART
from fpioa_manager import fm

# ------------------- 配置常量（按上板情况调整）-------------------
# 发送通道："both" = 同时输出 UART1/IO7 与 stdout/REPL（默认；两块板子侧自行选读）；
#           "uart" = 仅 UART1/IO7（对端板子 /dev/ttymxc2，JP6 链路）；
#           "repl" = 仅 stdout/REPL（对端板子 /dev/ttyACM0，USB Host 枚举的 cdc_acm）。
# both 下 UART 初始化失败仍保留 stdout 输出（不因单通道失败而整体失效）。
# 帧格式与发送时机在三个取值下完全一致。
SEND_CHANNEL = "both"
KMODEL_PATH = "/sd/yolo_face_detect.kmodel"   # 官方预置人脸检测模型（放 SD 卡根目录）——有卡时优先
# 无 MicroSD 卡时的回退：把同一 kmodel 用 kflash_gui 烧进 Flash，再从 Flash 地址加载。
# 烧录说明：kflash_gui 没有独立的「kmodel 模式」，官方口径是"模型烧录与固件烧录一致，只需改起始地址"——
#           即用「固件(Firmware)」模式、把地址从 0x00000 改为下面的 KMODEL_FLASH_ADDR（详见 docs/k210-setup.md §4.2.2）。
# 地址选择依据：官方人脸检测例程给的是 (0x300000, 278440)；固件本身从 0x00000 起，
#           故 0x300000 是官方例程用的示例地址（理论上不占固件区，待上板确认）。
#           **若资料包/官方文档给出其它推荐地址，以官方为准并同步改此处；地址填错会覆盖固件**。
# 长度必须等于实际 kmodel 文件字节数（可用 ls -l / wc -c 量出），不一致会加载失败。
KMODEL_FLASH_ADDR = 0x300000
KMODEL_FLASH_LEN = 278440
USE_LCD = True          # 板上无 LCD 时置 False，仅通过 REPL 串口看状态
THRESHOLD = 0.7         # 置信度阈值
NMS_VALUE = 0.3         # 非极大值抑制门限
IMG_W, IMG_H = 320, 240 # 摄像头 QVGA 尺寸
NET_W, NET_H = 320, 256 # 模型输入尺寸（人脸检测模型固定）
# 人脸检测模型固定 anchor（与模型绑定，不可改）
ANCHOR = (0.893, 1.463, 0.245, 0.389, 1.55, 2.58, 0.375, 0.594,
          3.099, 5.038, 0.057, 0.090, 0.567, 0.904, 0.101, 0.160, 0.159, 0.255)
# ------------------- UART 常量（引脚号待上板验证后写死，见文件头协议）-------------------
UART_ID = UART.UART1   # 待上板验证：须避开 REPL 所用串口
UART_TX_PIN = 7        # 待上板验证：官方例程外部排针 IO7=UART1_TX
UART_BAUD = 115200
HB_INTERVAL_MS = 2000  # 心跳帧间隔
# stdout 单次写耗时告警阈值（毫秒）：超过即视为"USB 可能未连接导致的偏慢"，自动摘除 stdout 端点。
# 取 100ms 远大于正常写耗时（一次约几十字节，通常 <1ms），避免误摘。
STDOUT_WRITE_WARN_MS = 100


def init_lcd():
    """初始化 LCD；无屏或失败时返回 False，降级为仅串口输出。"""
    if not USE_LCD:
        return False
    try:
        lcd.init()
        return True
    except Exception as e:
        print("[k210] LCD 初始化失败，改为仅串口输出: %s" % e)
        return False


def init_camera():
    """初始化并稳定摄像头。"""
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.set_vflip(True)     # 官方人脸检测例程仅此一项
    sensor.set_hmirror(True)   # 与官方 camera 例程一致（GC0328 需开）；人脸检测例程未开，镜像不对则删除本行
    sensor.skip_frames(time=1000)


def _sd_model_ready():
    """检查 SD 卡上的模型文件是否存在（MicroPython os.stat 失败即视为不存在）。"""
    try:
        os.stat(KMODEL_PATH)
        return True
    except OSError:
        return False


def init_kpu():
    """加载人脸检测模型并初始化 YOLO2 参数。

    加载顺序：① SD 卡 KMODEL_PATH（有卡时优先）→ ② Flash（KMODEL_FLASH_ADDR 起，
    需先用 kflash_gui 把 kmodel 烧到该地址）。两者都失败时打印清晰的中文提示并抛出异常，
    不静默失败（详见 docs/k210-setup.md）。
    """
    kpu = KPU()
    loaded = False
    err_flash = None

    # ① 优先 SD 卡路径（保持原有行为）
    if _sd_model_ready():
        try:
            kpu.load_kmodel(KMODEL_PATH)
            print("[k210] 已从 SD 卡加载模型: %s" % KMODEL_PATH)
            loaded = True
        except Exception as e:
            print("[k210] SD 卡模型加载失败，改试 Flash: %s" % e)
    else:
        print("[k210] SD 卡模型不存在（%s），改试 Flash" % KMODEL_PATH)

    # ② 回退：从 Flash 地址加载（模型须已用 kflash_gui 烧入）
    if not loaded:
        try:
            kpu.load_kmodel(KMODEL_FLASH_ADDR, KMODEL_FLASH_LEN)
            print("[k210] 已从 Flash 加载模型（addr=0x%X, len=%d）" % (KMODEL_FLASH_ADDR, KMODEL_FLASH_LEN))
            loaded = True
        except Exception as e:
            err_flash = e
            print("[k210] Flash 模型加载失败: %s" % e)

    # ③ 两条路都失败：给出清晰提示，不静默失败
    if not loaded:
        print("[k210] 未能加载人脸检测模型，请二选一：")
        print("        ① 插 SD 卡，并把 yolo_face_detect.kmodel 放到卡根目录（%s）；" % KMODEL_PATH)
        print("        ② 用 kflash_gui 把 kmodel 烧到 Flash 地址 0x%X（长度 %d 字节），详见 docs/k210-setup.md。"
              % (KMODEL_FLASH_ADDR, KMODEL_FLASH_LEN))
        print("        （Flash 加载错误：%s）" % err_flash)
        raise RuntimeError("kmodel 加载失败：SD 卡与 Flash 均不可用")

    kpu.init_yolo2(ANCHOR, anchor_num=9, img_w=IMG_W, img_h=IMG_H,
                   net_w=NET_W, net_h=NET_H, layer_w=10, layer_h=8,
                   threshold=THRESHOLD, nms_value=NMS_VALUE, classes=1)
    return kpu


_prob_warned = False   # prob 字段异常只告警一次，避免刷屏


def max_score_of(detections):
    """取所有检出框中的最大置信度；prob 字段下标待上板验证。"""
    global _prob_warned
    best = 0.0
    for d in detections:
        try:
            p = float(d[5])   # 官方文档：返回项为 (x, y, w, h, class, prob)
            if p > best:
                best = p
        except (IndexError, TypeError, ValueError):
            if not _prob_warned:
                print("[k210] prob 字段下标与预期不符，请核对固件返回结构")
                _prob_warned = True
    return best


def _now_ms():
    """当前单调毫秒时间。CanMV 提供 time.ticks_ms；本机桩环境下回退 time.time。"""
    try:
        return time.ticks_ms()
    except AttributeError:
        return int(time.time() * 1000)


def _elapsed_ms(t0):
    """自 t0 起的毫秒差；优先用 time.ticks_diff 以正确处理计数器回绕。"""
    try:
        return time.ticks_diff(time.ticks_ms(), t0)
    except AttributeError:
        return _now_ms() - t0


class ReplEndpoint:
    """stdout/REPL 发送端点（USB CDC，对端 /dev/ttyACM0）。

    is_repl 标记供 send_frame() 识别并施加"写耗时防护"：USB 未连接/对端未读时 stdout 可能
    变慢甚至阻塞，写耗时超 STDOUT_WRITE_WARN_MS 即由 send_frame 自动把本端点摘除。
    """

    is_repl = True

    def write(self, frame):
        sys.stdout.write(frame)


def init_sender():
    """初始化发送端，返回**端点列表**（元素为 UART 实例或 ReplEndpoint），交给 send_frame() 逐个发送。

      - SEND_CHANNEL == "both"：同时启用 stdout 与 UART1；UART 初始化失败时**仍保留 stdout**；
      - SEND_CHANNEL == "uart"：仅 UART1（失败则列表为空）；
      - SEND_CHANNEL == "repl"：仅 stdout；
      - 无任何可用端点：返回空列表并告警，检测与显示不受影响。
    """
    senders = []

    if SEND_CHANNEL == "repl" or SEND_CHANNEL == "both":
        senders.append(ReplEndpoint())   # 走 sys.stdout
        print("[k210] 发送通道：stdout/REPL（对端 /dev/ttyACM0）")

    if SEND_CHANNEL == "uart" or SEND_CHANNEL == "both":
        try:
            fm.register(UART_TX_PIN, fm.fpioa.UART1_TX, force=True)
            senders.append(UART(UART_ID, UART_BAUD, read_buf_len=256))   # 仅发送，接收缓冲取小
            print("[k210] 发送通道：UART1/IO7（对端 /dev/ttymxc2）")
        except Exception as e:
            # both 模式下不得因 UART 失败而丢掉 stdout 通道
            if senders:
                print("[k210] UART 初始化失败，保留 stdout 通道继续发送: %s" % e)
            else:
                print("[k210] UART 初始化失败，无可用发送通道（仅本地检测与显示）: %s" % e)

    if not senders:
        print("[k210] 无可用发送通道，仅保留本地检测与显示")
    return senders


def face_frame(count, score):
    """构造 #FACE 帧（协议唯一权威定义见文件头）。"""
    return "#FACE,%d,%.2f\n" % (count, score)


def hb_frame(uptime_s):
    """构造 #HB 心跳帧（协议唯一权威定义见文件头）。"""
    return "#HB,%d\n" % uptime_s


_send_fail_warned = False        # 非 stdout 端点写失败只告警一次，避免每帧刷屏
_stdout_stripped_warned = False  # stdout 端点被自动摘除的提示只打印一次


def _safe_warn(msg):
    """尽力打印一条中文提示；**绝不抛异常**。

    提示通路（sys.stdout / REPL）恰恰可能是我们正要告警的那个坏掉的通道，
    因此这里必须整体兜底：打印失败就静默降级，绝不让告警动作反过来中断主循环。
    """
    try:
        print(msg)
    except Exception:
        pass


def _strip_stdout(senders, reason):
    """把 stdout 端点从 senders 中移除（退化为 UART-only），并只提示一次。

    **本函数保证不抛异常**：它正是由"stdout 已损坏/写失败"的路径调用，任何二次异常
    都会逃逸出 send_frame() 并中断整个检测主循环。故顺序固定为：
      ① 先置一次性标记（提示失败也不会重复尝试）→ ② 摘除端点 → ③ 最后尝试提示（自行兜底）。
    摘除是"尽力而为"：找不到可摘的端点（如已摘过、或列表不可变）也不报错。
    """
    global _stdout_stripped_warned
    already = _stdout_stripped_warned
    _stdout_stripped_warned = True
    try:
        for s in list(senders or []):
            if getattr(s, "is_repl", False):
                senders.remove(s)
    except Exception:
        pass
    if not already:
        _safe_warn("[k210] stdout 写入异常或持续偏慢（USB 可能未连接），已自动摘除 stdout 通道，"
                   "仅保留 UART 发送；原因：%s" % reason)


def send_frame(senders, frame):
    """向**所有已启用端点**发送同一帧；某端点故障不影响其它端点，**任何情况下都不向上抛异常**。

    发送顺序：**先关键端点（UART）、后易故障端点（stdout/REPL）**——万一 stdout 写阻塞不返回，
    UART 本帧也已送达（顺序在遍历前固定，见下方 ordered）。

    端点两种形态：
      - ReplEndpoint（is_repl=True）→ 写 sys.stdout（帧自带 '\\n'，不能用 print()，否则会多一个换行）；
        并对其写耗时计时：单次写耗时 ≥ STDOUT_WRITE_WARN_MS（100ms，等价覆盖"连续偏慢"）或写抛异常，
        即**安全摘除**该端点（见 _strip_stdout，不会抛异常），避免 USB 未连接时拖慢主循环。
      - 其余（UART 实例）→ 直接 write；写失败只告警一次，不影响其它端点。

    遍历用副本，便于在循环中安全地移除 stdout 端点；摘除发生在"处理完当前端点后继续遍历剩余端点"，
    因此异常路径下 UART 等后续端点仍会收到本帧。
    """
    global _send_fail_warned
    # 先 UART 后 stdout：stdout 排在最后，即使其阻塞/失败，前面端点的本帧已发出。
    ordered = sorted(senders or [], key=lambda ep: 1 if getattr(ep, "is_repl", False) else 0)
    for s in ordered:
        if getattr(s, "is_repl", False):
            try:
                t0 = _now_ms()
                s.write(frame)
                dt = _elapsed_ms(t0)
                if dt >= STDOUT_WRITE_WARN_MS:
                    _strip_stdout(senders, "单次写耗时 %dms ≥ %dms" % (dt, STDOUT_WRITE_WARN_MS))
            except Exception as e:
                # _strip_stdout 自身不抛异常；此处再兜一层，确保 stdout 故障绝不逃逸出本函数
                try:
                    _strip_stdout(senders, "写失败: %s" % e)
                except Exception:
                    pass
        else:
            try:
                s.write(frame)
            except Exception as e:
                if not _send_fail_warned:
                    _safe_warn("[k210] 发送帧失败（该端点后续帧可能持续丢失）: %s" % e)
                    _send_fail_warned = True


def detect_once(kpu, od_img):
    """拍一帧并送 KPU 推理，返回 (img, detections, max_score)。"""
    img = sensor.snapshot()
    od_img.draw_image(img, 0, 0)
    od_img.pix_to_ai()
    kpu.run_with_output(od_img)
    detections = kpu.regionlayer_yolo2()
    if not detections:      # 兼容返回 [] 或 None
        detections = []
    return img, detections, max_score_of(detections)


def main():
    has_lcd = init_lcd()
    init_camera()
    kpu = init_kpu()
    sender = init_sender()

    od_img = image.Image(size=(NET_W, NET_H))   # KPU 输入缓冲（320x256）
    clock = time.clock()
    last_state = None   # None 表示尚未打印过；True=有人脸，False=无人脸
    t0 = time.ticks_ms()      # 脚本启动时刻，用于 #HB 的 uptime_s
    last_hb = t0              # 上次心跳发送时刻

    print("[k210] 人脸检测 demo 已启动，等待画面...")
    try:
        while True:
            clock.tick()
            img, detections, score = detect_once(kpu, od_img)

            count = len(detections)
            has_face = count > 0

            # 框出人脸
            for d in detections:
                img.draw_rectangle(d[0], d[1], d[2], d[3], color=(0, 255, 0))

            # 状态变化时打印到 REPL 串口（有人脸/无人脸 两种状态可区分）
            if has_face != last_state:
                if has_face:
                    print("[k210] 检测到人脸 count=%d max_score=%.2f" % (count, score))
                else:
                    print("[k210] 未检测到人脸")
                last_state = has_face

            # LCD 叠加状态并显示
            if has_lcd:
                if has_face:
                    img.draw_string(0, 0, "FACE x%d" % count, color=(0, 255, 0), scale=2.0)
                else:
                    img.draw_string(0, 0, "NO FACE", color=(255, 0, 0), scale=2.0)
                img.draw_string(0, 24, "%2.1ffps" % clock.fps(), color=(0, 60, 128), scale=2.0)
                lcd.display(img)

            # 按协议发送检测结果帧与心跳帧（P2-T8，协议见文件头）
            send_frame(sender, face_frame(count, score))
            now = time.ticks_ms()
            if time.ticks_diff(now, last_hb) >= HB_INTERVAL_MS:
                send_frame(sender, hb_frame(time.ticks_diff(now, t0) // 1000))
                last_hb = now

            gc.collect()
    finally:
        # CanMV IDE「停止」会以 KeyboardInterrupt 中断脚本，finally 确保资源释放；
        # 否则模型内存不释放，不复位直接重跑可能 load_kmodel 失败
        kpu.deinit()
        sensor.run(0)   # 官方 camera 例程：sensor.run(0) 停止摄像头
        print("[k210] 脚本已停止，KPU/摄像头资源已释放")


if __name__ == "__main__":
    main()
