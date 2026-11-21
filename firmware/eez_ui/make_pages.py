#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
make_pages.py —— 生成「协议网络 / 相遇记录 / 设置」三页，写进 .eez-project

═══════════════════════════════════════════════════════════════════════
 设计约束：这是一块 **480x480 圆屏**（AM160Q480480LK5，radius = 240）
═══════════════════════════════════════════════════════════════════════

第一版按矩形排版，四个角全在圆外 —— 标题和底部提示直接被切掉了。
现在所有文字都按 **圆内安全区** 自动摆放：

    · 内容水平居中（文字中心落在 CX 上）
    · 安全半径 R_SAFE = 192，圆心 (240,240)
    · fit_width(y,h) 给出"这一段高度上圆内允许多宽"，供需要时裁剪

文字宽度用估算（CJK ≈ 1.0em，ASCII ≈ 0.56em）来定 left，标签保持
auto-width（width="content"）—— 既不折行，又能精确居中。

为什么用脚本而不是在 EEZ Studio 里手点：
  1. `.eez-project` 就是纯 JSON，脚本写比点一百多下鼠标快，可重复、可进 Git；
  2. 版面参数留在这份脚本里，改一版重跑一次就行；
  3. 生成完再用 EEZ Studio 打开，它就是一个**正常的 EEZ 工程** ——
     你可以在里面继续拖控件、调样式，然后用它的 Build 生成 C 代码。

schema 来源：EEZ Studio 官方源码（packages/project-editor/）
  · 页面   ：userPages[] = {objID, name, width, height, components[LVGLScreenWidget], ...}
  · 控件   ：{objID, type, left/top/width/height(+Unit), style{useStyle}, children[], ...}
             类型字段例如 Label 的 text/textType/longMode/recolor
  · 样式   ：lvglStyles.styles[] = {name, forWidgetType, childStyles, definition}
             definition = {PART:{STATE:{prop:value}}}；颜色 = "0xRRGGBB"
  · 字体   ：fonts[] = {objID, id, name, source{filePath,size}, bpp, lvglRanges, lvglSymbols}
"""
import json
import os
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))          # .../firmware/eez_ui
FW = os.path.dirname(HERE)                                 # .../firmware
PROJ = os.path.join(HERE, "protorig_ui.eez-project")

sys.path.insert(0, os.path.join(FW, "font"))
import gen_font  # noqa: E402

CJK = "".join(gen_font.UI_CHARS)

# ------------------------------------------------------------------ 圆屏几何
SCR_W = 480
SCR_H = 480
CX = SCR_W // 2          # 240
CY = SCR_H // 2          # 240
R_SAFE = 192             # 文字安全半径（边缘留约 48px）


def fit_width(y, h):
    """y..y+h 这段高度上，圆内允许的最大宽度（取上下边缘里离圆心更远的那个）"""
    dy = max(abs(y - CY), abs(y + h - CY))
    if dy >= R_SAFE:
        return 80
    return 2 * int((R_SAFE * R_SAFE - dy * dy) ** 0.5)


def tw(text, px):
    """估算文字宽度：CJK 约 1.0em，ASCII 约 0.56em"""
    return sum(px if ord(c) > 0x7F else int(px * 0.56) for c in text)


# ------------------------------------------------------------------ 调色板
GOLD = "0xF2ED65"
GOLD_HI = "0xFFE27A"
GOLD_DIM = "0x90958D"
WARM = "0xECEEE5"
WARN = "0xFF6C30"
BLACK = "0x000000"
CARD_BG = "0x141713"

STYLE_PX = {
    "PageTitle": 28,
    "PageSub": 14,
    "RowMain": 20,
    "RowDim": 14,
    "RowGold": 20,
    "BigNum": 48,
    "WarnText": 14,
}

# ---------------------------------------------------------------- 工具函数


def oid():
    return str(uuid.uuid4())


def style_ref(name):
    """控件上的 style 块：useStyle 指向一个命名样式"""
    return {
        "objID": oid(),
        "useStyle": name,
        "conditionalStyles": [],
        "childStyles": [],
    }


def base_widget(wtype, left, top, w, h, use_style, children=None):
    return {
        "objID": oid(),
        "type": wtype,
        "left": int(left),
        "top": int(top),
        "width": w,
        "height": h,
        "customInputs": [],
        "customOutputs": [],
        "style": style_ref(use_style),
        "timeline": [],
        "eventHandlers": [],
        "leftUnit": "px",
        "topUnit": "px",
        "widthUnit": "px" if isinstance(w, int) else w,
        "heightUnit": "px" if isinstance(h, int) else h,
        "children": children if children is not None else [],
        "widgetFlags": "GESTURE_BUBBLE|PRESS_LOCK",
        "hiddenFlagType": "literal",
        "clickableFlagType": "literal",
        "checkedStateType": "literal",
        "disabledStateType": "literal",
        "states": "",
        "localStyles": {"objID": oid()},
        "groupIndex": 0,
    }


def label(text, left, top, use_style, w="content", h="content"):
    wdg = base_widget("LVGLLabelWidget", left, top, w, h, use_style)
    wdg.update({
        "text": text,
        "textType": "literal",
        "longMode": "WRAP",
        "recolor": False,
    })
    return wdg


def clabel(text, cx, y, use_style):
    """★ 圆屏版式用的居中标签：文字**水平中心**落在 cx 上"""
    px = STYLE_PX.get(use_style, 16)
    w = tw(text, px)
    wdg = label(text, int(cx - w / 2), int(y), use_style)
    wdg["terminalCenterX"] = cx
    return wdg


def screen(name, children, at_start=False):
    scr = base_widget("LVGLScreenWidget", 0, 0, SCR_W, SCR_H, "ScreenBg", children)
    scr["widgetFlags"] = "CLICKABLE|PRESS_LOCK|GESTURE_BUBBLE"
    scr["clickableFlag"] = True
    return {
        "objID": oid(),
        "components": [scr],
        "connectionLines": [],
        "localVariables": [],
        "userProperties": [],
        "name": name,
        "left": 0,
        "top": 0,
        "width": SCR_W,
        "height": SCR_H,
        "createAtStart": at_start,
        "deleteOnScreenUnload": False,
    }


def style(name, for_widget_type, definition):
    """LVGLStyle = {name, forWidgetType, childStyles, definition}

    ⚠️ 关键是 forWidgetType：EEZ 要求 style.forWidgetType == widget.type
       才允许把这个样式挂上去（见 widgets/Base.tsx 的 useStyle 过滤条件）。
       漏了这个字段，样式会静默失效、退回默认主题（浅色）。
    """
    return {
        "objID": oid(),
        "name": name,
        "forWidgetType": for_widget_type,
        "childStyles": [],
        "definition": definition,
    }


def defn(**props):
    """definition = {MAIN:{DEFAULT:{...}}} 的简写"""
    return {"MAIN": {"DEFAULT": props}}


# ------------------------------------------------------------------ 字体
FONTS = [
    ("ui14", "HarmonyOS_Sans_SC_Regular.ttf", 14, 1),
    ("ui16", "HarmonyOS_Sans_SC_Medium.ttf", 16, 2),
    ("ui20", "HarmonyOS_Sans_SC_Medium.ttf", 20, 3),
    ("ui28", "HarmonyOS_Sans_SC_Medium.ttf", 28, 4),
    ("ui48", "HarmonyOS_Sans_SC_Bold.ttf", 48, 5),
]


def make_fonts():
    out = []
    for name, ttf, size, fid in FONTS:
        out.append({
            "objID": oid(),
            "id": fid,
            "name": name,
            "description": "",
            "renderingEngine": "freetype",
            "source": {
                "objID": oid(),
                # 相对工程目录（firmware/eez_ui）→ firmware/font/ttf
                "filePath": "../font/ttf/" + ttf,
                "size": size,
            },
            "embeddedFontFile": "",
            "bpp": 4,
            "threshold": 128,
            "height": 0,
            "ascent": 0,
            "descent": 0,
            "glyphs": [],
            "screenOrientation": "all",
            "alwaysBuild": True,
            "lvglRanges": "0x20-0x7F",
            "lvglSymbols": CJK,
        })
    return out


# ------------------------------------------------------------------ 样式
SCREEN = "LVGLScreenWidget"
LABEL = "LVGLLabelWidget"
CONTAINER = "LVGLContainerWidget"


def make_styles():
    return [
        style("ScreenBg", SCREEN,
              defn(bg_color=BLACK, bg_opa=255, border_width=0, radius=0, pad_all=0)),
        style("PageTitle", LABEL, defn(text_color=GOLD, text_font="ui28")),
        style("PageSub", LABEL, defn(text_color=GOLD_DIM, text_font="ui14")),
        style("RowMain", LABEL, defn(text_color=WARM, text_font="ui20")),
        style("RowDim", LABEL, defn(text_color=GOLD_DIM, text_font="ui14")),
        style("RowGold", LABEL, defn(text_color=GOLD, text_font="ui20")),
        style("BigNum", LABEL, defn(text_color=WARM, text_font="ui48")),
        style("WarnText", LABEL, defn(text_color=WARN, text_font="ui14")),
        style("Card", CONTAINER,
              defn(bg_color=CARD_BG, bg_opa=255, border_color=GOLD_DIM,
                   border_width=1, radius=0, pad_all=0)),
        style("Accent", CONTAINER, defn(bg_color=GOLD, bg_opa=255, border_width=0, radius=0, pad_all=0)),
        style("HLine", CONTAINER,
              defn(bg_color=GOLD_DIM, bg_opa=120, border_width=0, radius=0)),
    ]


# Industrial terminal / circular safe area. Runtime adds the same 4-tab rail.
def box(x,y,w,h,sty="Card"):
    return base_widget(CONTAINER,x,y,w,h,sty)

def heading(title, serial):
    return [clabel(serial, CX, 51, "PageSub"),
            clabel(title, CX, 78, "PageTitle"),
            box(94,126,292,1,"HLine"), box(94,124,34,4,"Accent")]

def named(w, name):
    w["identifier"] = name
    return w

def page_network():
    kids=heading("协议网络", "01 / PROTOCOL LINK")
    kids += [box(80,150,320,136), box(80,150,4,136,"Accent"),
             label("链路待启用",102,166,"PageTitle"),
             label("PROTORIG / LOCAL TERMINAL",102,210,"PageSub"),
             label("附近的容器将在这里出现",102,246,"RowMain"),
             clabel("无线相遇功能尚未接入",CX,311,"RowDim"),
             clabel("ENDFIELD / FIELD SYSTEM",CX,344,"PageSub")]
    return screen("协议网络",kids,at_start=True)

def page_meets():
    kids=heading("相遇记录", "02 / ENCOUNTER ARCHIVE")
    kids += [clabel("00",CX,148,"BigNum"),
             clabel("等待第一份相遇记录",CX,214,"RowMain"),
             box(102,256,276,1,"HLine"),
             clabel("独一无二的地形",CX,281,"RowGold"),
             clabel("每一次相遇，留下新的轮廓",CX,316,"RowDim")]
    return screen("相遇记录",kids)

def page_settings():
    kids=heading("终端设置", "03 / DEVICE CONTROL")
    kids += [label("屏幕亮度",96,145,"RowMain")]
    for i,text in enumerate(["25%","60%","100%"]):
        kids += [named(box(92+i*102,175,94,64),f"brightness_{i}"),
                 clabel(text,139+i*102,195,"RowMain")]
    kids += [label("自动息屏",96,253,"RowMain")]
    for i,text in enumerate(["15 秒","30 秒","60 秒"]):
        kids += [named(box(92+i*102,283,94,64),f"timeout_{i}"),
                 clabel(text,139+i*102,303,"RowMain")]

    return screen("设置",kids)


# ------------------------------------------------------------------ main
def main():
    d = json.load(open(PROJ, encoding="utf-8"))

    # 1) 屏尺寸 + 圆屏（模拟器里能看到圆形安全区）
    g = d["settings"]["general"]
    g["displayWidth"] = SCR_W
    g["displayHeight"] = SCR_H
    g["displayBorderRadius"] = SCR_W // 2

    # 2) 字体 / 样式 / 页面
    d["fonts"] = make_fonts()
    d["lvglStyles"]["styles"] = make_styles()
    d["userPages"] = [page_network(), page_meets(), page_settings()]

    # 3) 每类控件的默认样式（键 = 控件类型名，值 = 样式名）
    d["lvglStyles"]["defaultStyles"] = {
        SCREEN: "ScreenBg",
        LABEL: "RowDim",
        CONTAINER: "Card",
    }

    json.dump(d, open(PROJ, "w", encoding="utf-8"), ensure_ascii=False, indent=2)

    # 4) 顺手把 codeIdentifier / 控件 identifier 填上
    #    不填的话中文页面名会被剥成 `_____`，生成的 C 直接编不过
    import fix_project_ids
    fix_project_ids.main()

    # 5) 圆屏安全检查：每个标签都必须落在圆内
    print()
    print("圆屏版式  圆心(%d,%d)  安全半径 %d" % (CX, CY, R_SAFE))
    bad = 0
    for p in d["userPages"]:
        scr = p["components"][0]
        worst = 0
        for wdg in scr["children"]:
            if "text" not in wdg:
                continue
            x, y = wdg["left"], wdg["top"]
            px = STYLE_PX.get(wdg["style"]["useStyle"], 16)
            w = tw(wdg["text"], px)
            h = int(px * 1.6)
            # 四个角到圆心的距离
            for (cx2, cy2) in ((x, y), (x + w, y), (x, y + h), (x + w, y + h)):
                d2 = ((cx2 - CX) ** 2 + (cy2 - CY) ** 2) ** 0.5
                worst = max(worst, d2)
            if worst > 240:
                bad += 1
        flag = "OK " if worst <= 240 else "!! 超出圆外"
        print("  %-10s 控件 %2d 个   最远角距圆心 %.0f px   %s"
              % (p["name"], len(scr["children"]), worst, flag))
    if bad:
        print("  !! 有 %d 个控件超出圆外，调坐标" % bad)
    print()
    print("接下来：EEZ Studio 里打开工程点 Build → 跑 post_build_patch.py")


if __name__ == "__main__":
    main()
