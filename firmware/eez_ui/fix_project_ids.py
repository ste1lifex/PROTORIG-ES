#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
fix_project_ids.py —— 修 EEZ 工程的「代码标识符」

问题：EEZ 用页面/控件名生成 C 标识符，而我们的名字是中文 → 被剥光 →
      生成 `create_screen_____()`、`objects.____`，而且**重名**，直接编不过：

          enum ScreensEnum { SCREEN_ID_____ = 1, SCREEN_ID_____ = 2, ... };

解法：给每个页面和控件填上显式的 identifier（EEZ 属性面板里叫 "Code identifier"）。
      这是 EEZ 官方支持的字段，填完再 Build 出来的 C 就是 `SCREEN_ID_NETWORK`、
      `objects.lbl_network_00` 这种可读名字。

顺带把 build.lvglInclude 从 "lvgl/lvgl.h" 改成 "lvgl.h"（我们工程里 lvgl.h 就在搜索路径根上）。
"""
import json
import os
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.join(HERE, "protorig_ui.eez-project")

PAGE_ID = {
    "协议网络": "network",
    "相遇记录": "meets",
    "设置": "settings",
}

TYPE_TAG = {
    "LVGLLabelWidget": "lbl",
    "LVGLContainerWidget": "box",
    "LVGLScreenWidget": "scr",
}


def walk(node, prefix, counter):
    """给控件树里每个节点分配唯一 identifier"""
    n = counter[0]
    counter[0] += 1
    tag = TYPE_TAG.get(node.get("type"), "w")
    node.setdefault("identifier", "%s_%s%02d" % (prefix, tag, n))
    for ch in node.get("children", []) or []:
        walk(ch, prefix, counter)


def main():
    d = json.load(open(PROJ, encoding="utf-8"))

    for p in d["userPages"]:
        pid = PAGE_ID.get(p["name"], "page")
        # ⚠️ 页面和控件用的是两个不同类的字段：
        #     页面（Page 类）     -> codeIdentifier
        #     控件（LVGLWidget）-> identifier
        p["codeIdentifier"] = pid
        p.pop("identifier", None)
        for root in p["components"]:
            walk(root, pid, [0])

    d["settings"]["build"]["lvglInclude"] = "lvgl.h"

    json.dump(d, open(PROJ, "w", encoding="utf-8"), ensure_ascii=False, indent=2)

    print("已写回:", PROJ)
    for p in d["userPages"]:
        scr = p["components"][0]
        print("  页面 %-8s codeIdentifier=%-10s 屏幕=%s  子控件 %d 个" % (
            p["name"], p["codeIdentifier"], scr["identifier"], len(scr["children"])))


if __name__ == "__main__":
    main()
