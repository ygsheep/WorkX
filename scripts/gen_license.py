# -*- coding: utf-8 -*-
"""生成 packaging/license.rtf（中文 MIT 许可，RTF \\uN 转义）"""
import sys

license_text = """Workx 终端智能助手

本项目基于 MIT License 开源发布。

MIT License

Copyright (c) 2026 Workx Project

特此免费授予任何获得本软件副本和相关文档文件（下称“软件”）的人不受限制地处置本软件的权利，包括但不限于使用、复制、修改、合并、发布、分发、再许可和/或出售本软件的副本，并允许向其提供本软件的人这样做，但须符合以下条件：

上述版权声明和本许可声明应包含在本软件的所有副本或主要部分中。

本软件按“原样”提供，不提供任何明示或暗示的担保，包括但不限于适销性、特定用途适用性和不侵权保证。在任何情况下，作者或版权持有人均不对任何索赔、损害或其他责任负责，无论这些责任是基于合同、侵权或其他方式，由本软件或本软件的使用或其他交易引起或与之相关。"""

BSLASH = chr(92)  # 反斜杠


def rtf_escape(text):
    out = []
    for ch in text:
        o = ord(ch)
        if o < 128:
            out.append(ch)
        else:
            if o > 32767:
                o -= 65536
            out.append(BSLASH + "u%d?" % o)
    return "".join(out)


parts = []
for line in license_text.split("\n"):
    parts.append(rtf_escape(line))

body = (BSLASH + "par ").join(parts)

rtf = (
    "{" + BSLASH + "rtf1" + BSLASH + "ansi" + BSLASH + "deff0 {"
    + BSLASH + "fonttbl {" + BSLASH + "f0 Arial;}{" + BSLASH + "f1 SimSun;}}"
    + BSLASH + "f0" + BSLASH + "fs24 "
    + body
    + BSLASH + "par"
    + "}"
)

with open("packaging/license.rtf", "w", encoding="ascii") as f:
    f.write(rtf)
print("license.rtf OK, size:", len(rtf))
