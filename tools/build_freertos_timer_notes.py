from datetime import datetime
from pathlib import Path
from xml.sax.saxutils import escape
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "FreeRTOS软件定时器内核学习笔记.docx"

BLUE = "2E74B5"
DARK_BLUE = "1F4D78"
LIGHT_BLUE = "E8EEF5"
LIGHT_GRAY = "F2F4F7"
CALLOUT = "F4F6F9"
BORDER = "B7C9DD"


def x(text):
    return escape(str(text), {"\"": "&quot;"})


def r(text, bold=False, color=None, size=22, font="Calibri", east_asia="Microsoft YaHei"):
    b = "<w:b/>" if bold else ""
    c = f'<w:color w:val="{color}"/>' if color else ""
    return (
        "<w:r><w:rPr>"
        f'<w:rFonts w:ascii="{font}" w:hAnsi="{font}" w:eastAsia="{east_asia}"/>'
        f'<w:sz w:val="{size}"/>{b}{c}'
        "</w:rPr>"
        f"<w:t>{x(text)}</w:t>"
        "</w:r>"
    )


def p(text="", style=None, bold=False, color=None, size=22, num_id=None, level=0, before=None, after=None):
    ppr = []
    if style:
        ppr.append(f'<w:pStyle w:val="{style}"/>')
    if num_id is not None:
        ppr.append(
            f"<w:numPr><w:ilvl w:val=\"{level}\"/><w:numId w:val=\"{num_id}\"/></w:numPr>"
        )
    if before is not None or after is not None:
        before_v = 0 if before is None else before
        after_v = 0 if after is None else after
        ppr.append(f'<w:spacing w:before="{before_v}" w:after="{after_v}"/>')
    ppr_xml = f"<w:pPr>{''.join(ppr)}</w:pPr>" if ppr else ""
    return f"<w:p>{ppr_xml}{r(text, bold=bold, color=color, size=size)}</w:p>"


def code_block(text):
    rows = []
    for line in text.strip("\n").splitlines():
        rows.append(p(line, style="Code", size=18))
    return "".join(rows)


def bullet(text):
    return p(text, num_id=1, level=0, after=80)


def number(text):
    return p(text, num_id=2, level=0, after=80)


def cell(content, width, fill=None, center=False, bold=False, color=None):
    shading = f'<w:shd w:fill="{fill}"/>' if fill else ""
    align = '<w:vAlign w:val="center"/>'
    margins = (
        '<w:tcMar><w:top w:w="80" w:type="dxa"/><w:start w:w="120" w:type="dxa"/>'
        '<w:bottom w:w="80" w:type="dxa"/><w:end w:w="120" w:type="dxa"/></w:tcMar>'
    )
    if isinstance(content, list):
        body = "".join(content)
    else:
        para_align = '<w:jc w:val="center"/>' if center else ""
        ppr = f"<w:pPr>{para_align}</w:pPr>" if para_align else ""
        body = f"<w:p>{ppr}{r(content, bold=bold, color=color)}</w:p>"
    return (
        "<w:tc>"
        f'<w:tcPr><w:tcW w:w="{width}" w:type="dxa"/>{shading}{align}{margins}</w:tcPr>'
        f"{body}</w:tc>"
    )


def table(headers, rows, widths):
    grid = "".join(f'<w:gridCol w:w="{w}"/>' for w in widths)
    borders = "".join(
        f'<w:{edge} w:val="single" w:sz="8" w:space="0" w:color="{BORDER}"/>'
        for edge in ("top", "left", "bottom", "right", "insideH", "insideV")
    )
    xml = [
        "<w:tbl>",
        "<w:tblPr>",
        '<w:tblW w:w="9360" w:type="dxa"/>',
        '<w:tblInd w:w="120" w:type="dxa"/>',
        f"<w:tblBorders>{borders}</w:tblBorders>",
        "</w:tblPr>",
        f"<w:tblGrid>{grid}</w:tblGrid>",
        "<w:tr>",
    ]
    for h, w in zip(headers, widths):
        xml.append(cell(h, w, fill=LIGHT_BLUE, center=True, bold=True, color=DARK_BLUE))
    xml.append("</w:tr>")
    for row in rows:
        xml.append("<w:tr>")
        for item, w in zip(row, widths):
            xml.append(cell(item, w))
        xml.append("</w:tr>")
    xml.append("</w:tbl>")
    xml.append(p("", after=120))
    return "".join(xml)


def callout(title, body):
    return table(
        [title],
        [[body]],
        [9360],
    ).replace(f'w:fill="{LIGHT_BLUE}"', f'w:fill="{CALLOUT}"', 1)


def h1(text):
    return p(text, style="Heading1", bold=True, color=BLUE, size=32, before=360, after=200)


def h2(text):
    return p(text, style="Heading2", bold=True, color=BLUE, size=26, before=280, after=140)


def build_body():
    body = []
    body.append(p("FreeRTOS 软件定时器内核学习笔记", bold=True, color=DARK_BLUE, size=48, after=60))
    body.append(p("主题：定时器列表、Timer Service Task、Timer Queue、回调执行上下文", color="555555", after=260))
    body.append(
        callout(
            "一句话总览",
            "FreeRTOS 软件定时器不是每个定时器一个线程，而是很多定时器对象 + 内核定时器列表 + 一个统一的 Timer Service Task。Tick 提供时间基准，Timer Service Task 负责处理命令和执行到期回调。",
        )
    )

    body.append(h1("1. 软件定时器整体链路"))
    body.append(p("软件定时器的底层流程可以从一条主线理解："))
    body.append(
        code_block(
            """
硬件 Tick 中断
    -> FreeRTOS tick 计数增加
    -> 内核维护软件定时器到期时间
    -> Timer Service Task 处理定时器命令和到期事件
    -> 调用用户注册的 callback
    -> callback 通知业务任务继续处理
"""
        )
    )
    body.append(p("关键点：回调函数通常不是在中断里执行，而是在 FreeRTOS 内部的 Timer Service Task 里执行。"))
    body.append(
        table(
            ["角色", "负责什么", "学习时怎么理解"],
            [
                ["Tick 中断", "周期性推动系统 tick 计数前进", "系统时间基准，像节拍器"],
                ["定时器列表", "保存已经启动的软件定时器，以及下一次到期 tick", "一张按到期时间管理的闹钟表"],
                ["Timer Queue", "接收 start/stop/reset/change/delete 等命令", "任务和 ISR 给定时器服务任务递纸条"],
                ["Timer Service Task", "统一处理命令，调用到期定时器回调", "定时器系统的执行者"],
            ],
            [1800, 3600, 3960],
        )
    )

    body.append(h1("2. 创建定时器和启动定时器不是同一件事"))
    body.append(p("创建只是分配并初始化一个软件定时器对象，启动才表示它开始进入活动定时器列表并等待到期。"))
    body.append(
        code_block(
            """
xHeartbeatTimer = xTimerCreate(
    "heartbeat",
    pdMS_TO_TICKS( 2000 ),
    pdTRUE,
    NULL,
    vHeartbeatTimerCallback
);

xTimerStart( xHeartbeatTimer, 0 );
"""
        )
    )
    body.append(bullet("xTimerCreate()：创建定时器控制块，记录名字、周期、是否自动重装、ID、回调函数。"))
    body.append(bullet("xTimerStart()：把“启动这个定时器”的请求送进 Timer Queue。"))
    body.append(bullet("Timer Service Task 收到启动命令后，才把定时器插入活动定时器列表。"))

    body.append(h1("3. 定时器列表：FreeRTOS 内部的闹钟表"))
    body.append(p("活动定时器列表里放的是已经启动、正在等待到期的软件定时器。每个节点本质上关联一个定时器控制块，里面保存："))
    for item in [
        "定时器名称，用于调试识别。",
        "定时器周期，单位是 tick。",
        "下一次到期的 tick 值。",
        "是否自动重装，也就是周期定时器还是一次性定时器。",
        "用户 ID，供回调函数取上下文。",
        "回调函数指针。",
    ]:
        body.append(bullet(item))
    body.append(p("可以想象内部状态类似这样："))
    body.append(
        table(
            ["定时器", "周期", "下一次到期 tick", "类型"],
            [
                ["heartbeat", "2000 ms", "当前 tick + 2000ms 对应 tick", "自动重装"],
                ["sensorTimeout", "5000 ms", "当前 tick + 5000ms 对应 tick", "一次性"],
                ["ledBlink", "1000 ms", "当前 tick + 1000ms 对应 tick", "自动重装"],
            ],
            [2200, 1800, 3500, 1860],
        )
    )

    body.append(h1("4. 为什么通常有两个定时器列表"))
    body.append(p("TickType_t 是整数，整数会溢出。比如把 tick 简化成 8 位，最大值是 255："))
    body.append(code_block("tick = 253\ntick = 254\ntick = 255\ntick = 0\ntick = 1"))
    body.append(
        p(
            "如果当前 tick 是 250，一个 20 tick 后到期的定时器实际到期点会绕回到 14。14 数值上小于 250，但它其实是未来。所以 FreeRTOS 用当前列表和溢出列表解决回绕比较问题。"
        )
    )
    body.append(
        table(
            ["列表", "放什么定时器", "什么时候切换"],
            [
                ["当前定时器列表", "本轮 tick 未溢出前会到期的定时器", "正常检查最近到期项"],
                ["溢出定时器列表", "到期时间跨过 tick 溢出的定时器", "tick 从最大值回到 0 后交换到当前列表"],
            ],
            [2200, 5000, 2160],
        )
    )
    body.append(callout("理解技巧", "当前列表可以看作“这一圈会响的闹钟”，溢出列表可以看作“tick 绕回 0 以后才会响的闹钟”。"))

    body.append(h1("5. Timer Queue：为什么 API 往往先发命令"))
    body.append(p("很多定时器 API 不直接修改定时器列表，而是向 Timer Queue 发送命令。这样可以让 Timer Service Task 统一管理内部列表，避免多个任务同时改链表。"))
    body.append(
        table(
            ["用户调用", "内部可以理解成", "最终由谁处理"],
            [
                ["xTimerStart()", "发送 START 命令", "Timer Service Task 插入活动列表"],
                ["xTimerStop()", "发送 STOP 命令", "Timer Service Task 从活动列表移除"],
                ["xTimerReset()", "发送 RESET 命令", "Timer Service Task 重新计算到期时间"],
                ["xTimerChangePeriod()", "发送 CHANGE_PERIOD 命令", "Timer Service Task 修改周期并重排列表"],
                ["xTimerDelete()", "发送 DELETE 命令", "Timer Service Task 删除定时器对象"],
            ],
            [2300, 3600, 3460],
        )
    )

    body.append(h1("6. Timer Service Task 的主循环"))
    body.append(p("Timer Service Task 是 FreeRTOS 内部创建的任务。它的循环可以用下面的伪代码理解："))
    body.append(
        code_block(
            """
for( ;; )
{
    找到最近要到期的定时器;

    等待：
        1. 最近的定时器到期
        2. 或者 Timer Queue 收到新命令

    如果收到命令：
        处理 start / stop / reset / change / delete

    如果有定时器到期：
        从定时器列表取出它
        调用它的 callback

        如果是自动重装定时器：
            计算下一次到期 tick
            重新插入定时器列表
}
"""
        )
    )
    body.append(p("因此它平时大部分时间处于阻塞等待状态，不会一直空转占 CPU。"))

    body.append(h1("7. 回调函数的执行上下文"))
    body.append(p("软件定时器回调运行在 Timer Service Task 中。这带来一个非常重要的工程规则：回调函数要短、快、不阻塞。"))
    body.append(
        table(
            ["推荐做法", "不推荐做法"],
            [
                ["设置事件位、发送通知、释放信号量", "在回调里长时间循环"],
                ["把复杂业务交给普通任务处理", "在回调里 vTaskDelay()"],
                ["尽快返回，让其他定时器回调有机会执行", "等待队列、等待互斥锁、做大量 printf"],
            ],
            [4680, 4680],
        )
    )
    body.append(callout("为什么要短快？", "所有软件定时器的回调共用同一个 Timer Service Task。一个回调拖太久，其他到期定时器的回调都会被延迟。"))

    body.append(h1("8. 自动重装定时器和一次性定时器"))
    body.append(
        table(
            ["xTimerCreate 第三个参数", "含义", "到期后行为"],
            [
                ["pdTRUE", "自动重装，周期定时器", "执行 callback 后重新计算下一次到期时间"],
                ["pdFALSE", "一次性定时器", "执行 callback 后停止，不会自动再次到期"],
            ],
            [2600, 3000, 3760],
        )
    )
    body.append(p("你的 heartbeat 使用 pdTRUE，所以它每 2 秒重复产生一次心跳事件。"))

    body.append(h1("9. 与你的 heartbeat demo 对照"))
    body.append(p("你的 demo 中，软件定时器本身不直接打印日志，而是设置事件位，让 supervisor 任务打印。"))
    body.append(
        code_block(
            """
static void vHeartbeatTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer;
    xEventGroupSetBits( xSystemEvents, EVENT_HEARTBEAT );
}
"""
        )
    )
    for item in [
        "main() 创建 heartbeat 软件定时器。",
        "main() 调用 xTimerStart()，向 Timer Queue 发启动命令。",
        "Timer Service Task 处理命令，把 heartbeat 加入活动定时器列表。",
        "到期后 Timer Service Task 调用 vHeartbeatTimerCallback()。",
        "回调设置 EVENT_HEARTBEAT 事件位。",
        "vSupervisorTask 的 xEventGroupWaitBits() 被唤醒。",
        "supervisor 打印 heartbeat observed 日志。",
    ]:
        body.append(number(item))

    body.append(h1("10. 相关 FreeRTOSConfig.h 配置"))
    body.append(
        table(
            ["配置项", "作用", "影响"],
            [
                ["configUSE_TIMERS", "是否启用软件定时器", "必须为 1，软件定时器 API 才可用"],
                ["configTIMER_TASK_PRIORITY", "Timer Service Task 优先级", "太低可能导致回调不及时"],
                ["configTIMER_QUEUE_LENGTH", "Timer Queue 长度", "太小可能导致 start/stop/reset 命令发送失败"],
                ["configTIMER_TASK_STACK_DEPTH", "Timer Service Task 栈大小", "回调函数栈使用过大时可能溢出"],
                ["configTICK_RATE_HZ", "系统 tick 频率", "决定软件定时器的时间粒度"],
            ],
            [2700, 3000, 3660],
        )
    )

    body.append(h1("11. 调试和排查清单"))
    for item in [
        "定时器没有回调：先确认 configUSE_TIMERS 是否为 1，xTimerCreate() 和 xTimerStart() 是否成功。",
        "回调不准时：检查 Timer Service Task 优先级是否太低，高优先级任务是否长期占用 CPU。",
        "命令发送失败：检查 configTIMER_QUEUE_LENGTH 是否太小，xTimerStart() 的 block time 是否为 0。",
        "多个定时器互相影响：检查某个 callback 是否执行太久或发生阻塞。",
        "周期看起来不对：确认 pdMS_TO_TICKS() 换算后是否为预期 tick 数。",
        "系统启动失败：检查软件定时器任务和队列是否消耗了额外 FreeRTOS heap。",
    ]:
        body.append(bullet(item))

    body.append(h1("12. 最后总结"))
    body.append(
        p(
            "FreeRTOS 软件定时器的内核逻辑可以浓缩成：用 tick 计时，用定时器列表记录到期点，用 Timer Queue 接收操作命令，用 Timer Service Task 统一执行回调。它轻量、适合毫秒级业务定时，但准确性依赖调度器、任务优先级和回调函数是否足够短快。"
        )
    )
    return "".join(body)


def styles_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:style w:type="paragraph" w:default="1" w:styleId="Normal">
    <w:name w:val="Normal"/>
    <w:pPr><w:spacing w:after="120" w:line="300" w:lineRule="auto"/></w:pPr>
    <w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="Microsoft YaHei"/><w:sz w:val="22"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading1">
    <w:name w:val="heading 1"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>
    <w:pPr><w:keepNext/><w:spacing w:before="360" w:after="200" w:line="300" w:lineRule="auto"/></w:pPr>
    <w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="Microsoft YaHei"/><w:b/><w:color w:val="{BLUE}"/><w:sz w:val="32"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading2">
    <w:name w:val="heading 2"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>
    <w:pPr><w:keepNext/><w:spacing w:before="280" w:after="140" w:line="300" w:lineRule="auto"/></w:pPr>
    <w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="Microsoft YaHei"/><w:b/><w:color w:val="{BLUE}"/><w:sz w:val="26"/></w:rPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Code">
    <w:name w:val="Code"/><w:basedOn w:val="Normal"/>
    <w:pPr><w:spacing w:before="0" w:after="0" w:line="240" w:lineRule="auto"/><w:ind w:left="260" w:right="260"/></w:pPr>
    <w:rPr><w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:eastAsia="Microsoft YaHei"/><w:sz w:val="18"/></w:rPr>
  </w:style>
</w:styles>"""


def numbering_xml():
    return """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:abstractNum w:abstractNumId="1">
    <w:multiLevelType w:val="singleLevel"/>
    <w:lvl w:ilvl="0">
      <w:start w:val="1"/><w:numFmt w:val="bullet"/><w:lvlText w:val="•"/>
      <w:pPr><w:ind w:left="540" w:hanging="270"/></w:pPr>
      <w:rPr><w:rFonts w:ascii="Symbol" w:hAnsi="Symbol"/></w:rPr>
    </w:lvl>
  </w:abstractNum>
  <w:num w:numId="1"><w:abstractNumId w:val="1"/></w:num>
  <w:abstractNum w:abstractNumId="2">
    <w:multiLevelType w:val="singleLevel"/>
    <w:lvl w:ilvl="0">
      <w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/>
      <w:pPr><w:ind w:left="540" w:hanging="270"/></w:pPr>
    </w:lvl>
  </w:abstractNum>
  <w:num w:numId="2"><w:abstractNumId w:val="2"/></w:num>
</w:numbering>"""


def document_xml():
    sect = (
        '<w:sectPr><w:pgSz w:w="12240" w:h="15840"/>'
        '<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" '
        'w:header="708" w:footer="708" w:gutter="0"/></w:sectPr>'
    )
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body>
    {build_body()}
    {sect}
  </w:body>
</w:document>"""


def write_docx():
    now = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
    content_types = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
  <Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>"""
    rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>"""
    doc_rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/>
</Relationships>"""
    core = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <dc:title>FreeRTOS 软件定时器内核学习笔记</dc:title>
  <dc:creator>Codex</dc:creator>
  <cp:lastModifiedBy>Codex</cp:lastModifiedBy>
  <dcterms:created xsi:type="dcterms:W3CDTF">{now}</dcterms:created>
  <dcterms:modified xsi:type="dcterms:W3CDTF">{now}</dcterms:modified>
</cp:coreProperties>"""
    app = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties" xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes">
  <Application>Codex</Application>
</Properties>"""
    with ZipFile(OUT, "w", ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/_rels/document.xml.rels", doc_rels)
        z.writestr("word/document.xml", document_xml())
        z.writestr("word/styles.xml", styles_xml())
        z.writestr("word/numbering.xml", numbering_xml())
        z.writestr("docProps/core.xml", core)
        z.writestr("docProps/app.xml", app)
    print(OUT)


if __name__ == "__main__":
    write_docx()
