# -*- coding: utf-8 -*-
"""
Workx 四层架构图合成脚本
左侧：用户上传的鲸鱼娘角色图（原图直接使用，不修改）
右侧：Pillow 精确绘制的四层架构图（文字保证准确）
输出：docs/architecture_overview.jpg（16:9，2560x1440）
"""
import os
from PIL import Image, ImageDraw, ImageFont

# ============================================================
# 1. 路径配置
# ============================================================
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHARACTER_PATH = r"C:\Users\young\Downloads\生成现代化终端Code_Work Agent图标.png"
OUTPUT_PATH = os.path.join(PROJECT_ROOT, "docs", "architecture_overview.jpg")

# 画布尺寸 16:9
CANVAS_W, CANVAS_H = 2560, 1440

# 左右分区：左 40% 放角色，右 60% 放架构图
SPLIT_X = int(CANVAS_W * 0.40)  # 1024
RIGHT_MARGIN = 60
RIGHT_INNER_X = SPLIT_X + RIGHT_MARGIN
RIGHT_INNER_W = CANVAS_W - RIGHT_INNER_X - RIGHT_MARGIN  # ~1416

# 字体（微软雅黑 bold/regular）
FONT_DIR = r"C:\Windows\Fonts"
FONT_BOLD = os.path.join(FONT_DIR, "msyhbd.ttc")   # 微软雅黑 Bold
FONT_REG  = os.path.join(FONT_DIR, "msyh.ttc")      # 微软雅黑 Regular
FONT_MONO = os.path.join(FONT_DIR, "consola.ttf")   # 等宽（英文 fallback）


def load_font(path, size):
    """安全加载字体，失败则回退到默认字体"""
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.load_default()


# 字体大小
F_LAYER_TITLE = load_font(FONT_BOLD, 44)    # 每层标题
F_BOX_TITLE   = load_font(FONT_BOLD, 28)    # 模块子标题（如 ReAct...）
F_BODY        = load_font(FONT_REG,  24)    # 内容正文
F_BULLET      = load_font(FONT_BOLD, 24)    # 项目符号前的圆点

# ============================================================
# 2. 颜色 & 样式
# ============================================================
WHITE       = (255, 255, 255)
BLACK       = ( 20,  20,  20)
GRAY_SOFT   = (100, 100, 110)
ARROW       = ( 40,  40,  50)
EVENTBUS_DASH = (160, 160, 180)

# 每层背景 + 边框配色（与参考图同款淡色系）
LAYERS = [
    # (tag, 背景色,       边框色,      前景文字色)
    ("1. APP  应用层",          (237, 228, 255), (180, 155, 230), (60,  30, 110)),
    ("2. AGENT 核心层",         (216, 236, 255), (135, 185, 240), ( 30,  80, 140)),
    ("3. CORE  基础设施层",     (216, 245, 224), (130, 210, 160), ( 20, 100,  60)),
    ("4. TUI  终端用户界面层",  (255, 231, 209), (240, 185, 135), (140,  70,  20)),
]

BORDER_RADIUS = 28
BORDER_THICK = 3

# 自定义图标样式：(方块填充色, [内部装饰线列表(y1,y2,线宽)...])
ICON_BRAIN   = ((255, 165, 190), [(0.28, 0.28, 2), (0.50, 0.50, 3), (0.72, 0.72, 2)])  # 粉色大脑风（3条横线）
ICON_CHAT    = ((120, 200, 150), [(0.25, 0.25, 2), (0.48, 0.48, 2), (0.70, 0.60, 2)])  # 绿色聊天气泡风（3条）

# ============================================================
# 3. 每层内容（保证文字 100% 正确）
# ============================================================
LAYER_CONTENTS = [
    # ---------- 1. APP ----------
    {
        "subtitle": None,  # 无副标题
        "cols": [
            [
                "main.cpp  入口",
                "配置管理",
                "CLI  参数解析",
            ],
            [
                "模型选择器",
                "路径补全",
                "文件索引",
            ],
        ],
    },
    # ---------- 2. AGENT ----------
    {
        "subtitle": (ICON_BRAIN, "ReAct 推理循环  ·  核心模块"),
        "bullets": [
            "ChatSession  会话管理",
            "LLM 后端：SSE 流  /  HTTP 客户端  /  OpenAI / Anthropic 适配器",
            "工具集：File 读写编辑  /  Glob  /  Grep  /  Bash  /  WebFetch  /  AgentTool  /  MCPTool",
            "模型路由  /  权限校验  /  系统提示词  /  消息历史管理",
        ],
    },
    # ---------- 3. CORE ----------
    {
        "subtitle": (ICON_CHAT, "EventBus  事件总线  ·  连接所有层"),
        "bullets": [
            "任务管理器（线程池）",
            "配置管理器",
            "通用工具：Result 类型  /  错误处理  /  子进程执行",
        ],
    },
    # ---------- 4. TUI ----------
    {
        "subtitle": None,
        "bullets": [
            "Terminal 终端封装  /  Screen 屏幕管理  /  DisplayBuffer 显示缓冲",
            "ChatRenderer 聊天渲染：Markdown 渲染  /  Tree-sitter 语法高亮  /  流式缓冲",
            "Widgets 组件：StatusBar 状态栏  /  CommandPanel 命令面板  /  SelectPanel 选择面板",
            "LineEditor 行编辑器  /  输入历史管理",
        ],
    },
]


def draw_rounded_rect(draw, xy, radius, fill, outline=None, width=1):
    """圆角矩形（Pillow 8+）"""
    draw.rounded_rectangle(xy, radius=radius, fill=fill,
                           outline=outline, width=width)


def measure_text(text, font):
    """测量文本尺寸（兼容旧版 Pillow）"""
    bbox = font.getbbox(text)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def draw_text_center(draw, xy_center, text, font, fill):
    tw, th = measure_text(text, font)
    x = xy_center[0] - tw // 2
    y = xy_center[1] - th // 2
    draw.text((x, y), text, font=font, fill=fill)


def draw_layer_box(draw, top_y, height, layer_idx):
    """绘制单个层（圆角矩形 + 标题栏 + 内容）"""
    bg, border, fg = LAYERS[layer_idx][1], LAYERS[layer_idx][2], LAYERS[layer_idx][3]
    title_text = LAYERS[layer_idx][0]
    content    = LAYER_CONTENTS[layer_idx]

    box = (RIGHT_INNER_X, top_y,
           RIGHT_INNER_X + RIGHT_INNER_W, top_y + height)

    # --- 框体 ---
    draw_rounded_rect(draw, box, BORDER_RADIUS, fill=bg,
                      outline=border, width=BORDER_THICK)

    # --- 标题（顶栏，不额外分块，仅在上方绘制大字）---
    title_y = top_y + 28
    draw_text_center(draw,
                     ((box[0] + box[2]) // 2, title_y),
                     title_text, F_LAYER_TITLE, fg)
    cursor_y = title_y + 36 + 10  # 标题下方间距

    # --- 副标题 ---
    if content.get("subtitle"):
        icon_style, sub = content["subtitle"]
        # 副标题底色（比层背景更亮一点的条）
        sub_box_h = 56
        sub_box = (box[0] + 36, cursor_y,
                   box[2] - 36, cursor_y + sub_box_h)
        sub_fill = tuple(min(255, c + 10) for c in bg)
        draw_rounded_rect(draw, sub_box, 14, fill=sub_fill, outline=border, width=2)

        # --- 彩色图标方块（代替 emoji，避免 Pillow 不支持彩色 emoji）---
        icon_size = 32
        icon_r = 8
        icon_x = sub_box[0] + 22
        icon_y = sub_box[1] + (sub_box_h - icon_size) // 2
        icon_color, inner_lines = icon_style
        draw_rounded_rect(draw, (icon_x, icon_y, icon_x + icon_size, icon_y + icon_size),
                          icon_r, fill=icon_color, outline=border, width=2)
        # 内部装饰线（让小方块有内容）
        for i, (y1, y2, lw) in enumerate(inner_lines):
            yy1 = icon_y + int(icon_size * y1)
            yy2 = icon_y + int(icon_size * y2)
            draw.line((icon_x + 6,  yy1, icon_x + icon_size - 6, yy1),
                      fill=WHITE, width=lw)
            if y2 != y1:
                draw.line((icon_x + 6,  yy2, icon_x + icon_size - 6, yy2),
                          fill=WHITE, width=lw)

        # 文本（图标右侧）
        tw, th = measure_text(sub, F_BOX_TITLE)
        text_x = icon_x + icon_size + 18
        text_y = sub_box[1] + (sub_box_h - th) // 2 - 2
        draw.text((text_x, text_y), sub, font=F_BOX_TITLE, fill=fg)
        cursor_y = sub_box[3] + 18

    # --- 内容（双列 or 子弹列表）---
    pad_left = 70
    if "cols" in content and content["cols"]:
        # 双列
        col_count = len(content["cols"])
        col_w = (box[2] - box[0] - pad_left * 2) // col_count
        for ci, col in enumerate(content["cols"]):
            cx = box[0] + pad_left + ci * col_w
            cy = cursor_y
            for item in col:
                # 小圆点
                dot_r = 5
                dot_cx = cx + 8
                dot_cy = cy + 13
                draw.ellipse((dot_cx - dot_r, dot_cy - dot_r,
                              dot_cx + dot_r, dot_cy + dot_r), fill=fg)
                # 文字
                draw.text((cx + 28, cy), item, font=F_BODY, fill=BLACK)
                _, h = measure_text(item, F_BODY)
                cy += h + 14
    else:
        # 子弹列表
        bx = box[0] + pad_left
        by = cursor_y
        for item in content["bullets"]:
            # 子弹点（加粗方块 / 圆点）
            draw.ellipse((bx, by + 11, bx + 10, by + 21), fill=fg)
            draw.text((bx + 26, by), item, font=F_BODY, fill=BLACK)
            _, h = measure_text(item, F_BODY)
            by += h + 14

    return box


def draw_down_arrow(draw, x_center, top_y, bottom_y):
    """向下箭头：竖线 + 三角"""
    # 竖线
    line_w = 4
    draw.line((x_center, top_y, x_center, bottom_y - 20), fill=ARROW, width=line_w)
    # 三角
    tri_h = 26
    tri_hw = 18
    draw.polygon([
        (x_center - tri_hw, bottom_y - tri_h),
        (x_center + tri_hw, bottom_y - tri_h),
        (x_center,          bottom_y),
    ], fill=ARROW)


# ============================================================
# 主合成函数
# ============================================================
def main():
    # 1) 加载角色图
    if not os.path.isfile(CHARACTER_PATH):
        raise FileNotFoundError(f"找不到角色图: {CHARACTER_PATH}")
    char_img = Image.open(CHARACTER_PATH).convert("RGBA")
    print(f"[角色图] 原始尺寸: {char_img.size}")

    # 2) 创建画布
    canvas = Image.new("RGB", (CANVAS_W, CANVAS_H), WHITE)
    draw = ImageDraw.Draw(canvas)

    # 3) 放角色图到左侧（等比缩放，垂直居中）
    target_char_w = SPLIT_X - 20       # 留左右各 10px 边距
    scale = target_char_w / char_img.width
    target_char_h = int(char_img.height * scale)
    if target_char_h > CANVAS_H - 40:
        scale = (CANVAS_H - 40) / char_img.height
        target_char_w = int(char_img.width * scale)
        target_char_h = CANVAS_H - 40

    char_resized = char_img.resize((target_char_w, target_char_h),
                                   Image.LANCZOS)
    char_x = (SPLIT_X - target_char_w) // 2
    char_y = (CANVAS_H - target_char_h) // 2
    canvas.paste(char_resized, (char_x, char_y), char_resized)

    # 4) 计算每层高度并绘制 4 个层
    total_right_h = CANVAS_H - 120  # 上下各 60px 边距
    # 每层基础高：按内容估算 - APP/TUI稍矮, AGENT/CORE稍高
    layer_heights = [210, 380, 250, 310]
    arrow_gap = 60
    sum_h = sum(layer_heights) + arrow_gap * 3
    # 如果超出或不足，整体按比例缩放
    scale2 = total_right_h / sum_h
    layer_heights = [int(h * scale2) for h in layer_heights]
    arrow_gap    = int(arrow_gap * scale2)

    top_y = 60
    layer_boxes = []
    for i in range(4):
        h = layer_heights[i]
        box = draw_layer_box(draw, top_y, h, i)
        layer_boxes.append(box)
        # 箭头
        if i < 3:
            x_center = (box[0] + box[2]) // 2
            draw_down_arrow(draw, x_center,
                            box[3] + 4,
                            box[3] + arrow_gap)
            top_y = h + arrow_gap + top_y
        else:
            top_y = h + top_y

    # 5) EventBus 虚线连接（从第3层右侧 -> 1/2/4 层右侧）
    core_box = layer_boxes[2]
    eventbus_x = core_box[2] + 30
    # 竖线（贯穿右侧）
    dashed_vert_x = CANVAS_W - 30
    # 画主竖虚线
    total_top = layer_boxes[0][1]
    total_bot = layer_boxes[3][3]
    draw_dashed_line(draw,
                     (dashed_vert_x, total_top - 10),
                     (dashed_vert_x, total_bot + 10),
                     EVENTBUS_DASH, 3, 14, 10)
    # 每层连一条横向虚线
    for li in (0, 1, 2, 3):
        y_mid = (layer_boxes[li][1] + layer_boxes[li][3]) // 2
        draw_dashed_line(draw,
                         (layer_boxes[li][2], y_mid),
                         (dashed_vert_x, y_mid),
                         EVENTBUS_DASH, 3, 14, 10)

    # 标签 "EventBus"
    tag_text = "EventBus"
    tag_font = F_BOX_TITLE
    tw, th = measure_text(tag_text, tag_font)
    tag_box = (dashed_vert_x - tw // 2 - 12, layer_boxes[2][1] - 10,
               dashed_vert_x + tw // 2 + 12, layer_boxes[2][1] + th + 14)
    draw_rounded_rect(draw, tag_box, 10, fill=(214, 245, 222),
                      outline=(80, 180, 120), width=2)
    draw.text((tag_box[0] + 12, tag_box[1] + 7),
              tag_text, font=tag_font, fill=(20, 110, 60))

    # 6) 保存
    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    canvas.save(OUTPUT_PATH, "JPEG", quality=95)
    print(f"[完成] 合成图已保存到: {OUTPUT_PATH}")
    print(f"       尺寸: {canvas.size}")


def draw_dashed_line(draw, p1, p2, fill, width=3, dash=14, gap=10):
    """绘制水平或垂直虚线"""
    x1, y1 = p1
    x2, y2 = p2
    if abs(x2 - x1) < 2:
        # 垂直
        y_step = dash + gap
        y = y1
        while y < y2:
            end = min(y + dash, y2)
            draw.line((x1, y, x2, end), fill=fill, width=width)
            y += y_step
    else:
        # 水平
        x_step = dash + gap
        x = x1
        while x < x2:
            end = min(x + dash, x2)
            draw.line((x, y1, end, y2), fill=fill, width=width)
            x += x_step


if __name__ == "__main__":
    main()
