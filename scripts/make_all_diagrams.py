"""
Workx 项目图解批量生成器
- 左侧 40%：直接嵌入用户上传的鲸鱼娘原图（自动盖住原图气泡，并重画新气泡 + 新文案）
- 右侧 60%：Pillow 精确绘制框图（文字零乱码）
- 输出：docs/img/*.jpg (16:9 2560x1440)

运行：
    pip install pillow
    python scripts/make_all_diagrams.py
"""

import os
import math
from PIL import Image, ImageDraw, ImageFont

# ================ 基础配置 ================
WORKSPACE = r"d:\develop\Workspace\workx"
OUT_DIR = os.path.join(WORKSPACE, "docs", "img")
os.makedirs(OUT_DIR, exist_ok=True)

CHARACTER_DIR = os.path.join(WORKSPACE, "docs", "img", "characters")  # 文生图专属角色立绘目录
# 文件名映射：架构图输出名 -> 专属角色立绘文件名
CHARACTER_MAP = {
    "01_react_loop": "01_react_whale.jpg",
    "02_eventbus_flow": "02_eventbus_whale.jpg",
    "03_render_pipeline": "03_render_pipeline_whale.jpg",
    "04_tool_pipeline": "04_tool_pipeline_whale.jpg",
    "05_token_compression": "05_token_compression_whale.jpg",
    "06_setup_wizard": "06_setup_wizard_whale.jpg",
    "07_build_pipeline": "07_build_pipeline_whale.jpg",
    "08_backend_adapter": "08_backend_adapter_whale.jpg",
    "09_mcp_bridge": "09_mcp_bridge_whale.jpg",
    "10_dependency_overview": "10_dependency_overview_whale.jpg",
}

CANVAS_W, CANVAS_H = 2560, 1440
SPLIT_X = int(CANVAS_W * 0.40)  # 左角色宽，右框图从这里开始

BG_COLOR = (255, 255, 255)
TEXT_DARK = (28, 33, 50)
TEXT_NAVY = (16, 50, 110)
BLACK = (0, 0, 0)

# 字体：优先微软雅黑
try:
    FONT_DIR = r"C:\Windows\Fonts"
    FONT_BOLD = ImageFont.truetype(os.path.join(FONT_DIR, "msyhbd.ttc"), 26)
    FONT_BOLD_L = ImageFont.truetype(os.path.join(FONT_DIR, "msyhbd.ttc"), 38)
    FONT_BOLD_XL = ImageFont.truetype(os.path.join(FONT_DIR, "msyhbd.ttc"), 50)
    FONT_REG = ImageFont.truetype(os.path.join(FONT_DIR, "msyh.ttc"), 22)
    FONT_REG_L = ImageFont.truetype(os.path.join(FONT_DIR, "msyh.ttc"), 26)
    FONT_SM = ImageFont.truetype(os.path.join(FONT_DIR, "msyh.ttc"), 18)
except Exception:
    # fallback
    FONT_BOLD = FONT_BOLD_L = FONT_BOLD_XL = ImageFont.load_default()
    FONT_REG = FONT_REG_L = FONT_SM = ImageFont.load_default()


def find_character_image():
    """在项目目录找最新的角色图（png/jpg）"""
    if CHARACTER_PATH and os.path.exists(CHARACTER_PATH):
        return CHARACTER_PATH
    candidates = []
    for root, dirs, files in os.walk(WORKSPACE):
        # 跳过 .git node_modules build vcpkg_installed
        dirs[:] = [d for d in dirs if d not in (".git", "node_modules", "build",
                                               "vcpkg_installed", "out", "dist")]
        for f in files:
            if f.lower().endswith((".png", ".jpg", ".jpeg", ".webp")):
                p = os.path.join(root, f)
                # 过滤掉架构图、生成过的图、应用图标源文件
                fl = f.lower()
                if ("architecture" in fl or "diagram" in fl or "react_loop" in fl
                        or "eventbus" in fl or "pipeline" in fl or "setup_wizard" in fl
                        or "adapter" in fl or "bridge" in fl or "dependency" in fl
                        or "compression" in fl or "token_" in fl):
                    continue
                p_norm = os.path.normpath(p)
                if p_norm.endswith(os.path.normpath("src/icon.png")):
                    continue
                try:
                    size = os.path.getsize(p)
                    if not (200 * 1024 < size < 40 * 1024 * 1024):
                        continue
                    # 打开看尺寸：方形 1024/512/256 基本是应用图标，排除
                    try:
                        with Image.open(p) as im:
                            w, h = im.size
                            # 排除方形图标尺寸（鲸鱼娘角色通常是竖图或接近1:1但带气泡的表情包）
                            if w == h and w in (128, 256, 512, 1024, 2048):
                                continue
                            # 附加置信度：表情包一般不会 RGB 无 alpha；角色 PNG 多为 RGBA 透明
                    except Exception:
                        pass
                    candidates.append((p, os.path.getmtime(p), size))
                except OSError:
                    pass
    if not candidates:
        raise FileNotFoundError(
            "没找到鲸鱼娘原图！\n"
            "  👉 请把你的角色 PNG/JPG 放到项目目录下（例如放在 docs/whale_girl.png）\n"
            "  👉 或者直接修改脚本顶部 CHARACTER_PATH = r'C:/你的角色图路径.png'")
    # 按 (最新修改时间, 更大分辨率偏好) 排序
    def _reso(item):
        try:
            with Image.open(item[0]) as im:
                return im.width * im.height
        except Exception:
            return 0
    candidates.sort(key=lambda x: (x[1], _reso(x)), reverse=True)
    return candidates[0][0]


# ================ 绘图辅助函数 ================
def rounded_rect(draw, xy, radius, fill=None, outline=None, width=2):
    draw.rounded_rectangle(xy, radius=radius, fill=fill, outline=outline, width=width)


def draw_down_arrow(draw, cx, y_top, y_bot, color=(40, 40, 40), w=4):
    """画向下实心箭头"""
    draw.line([(cx, y_top), (cx, y_bot - 18)], fill=color, width=w)
    # 三角
    draw.polygon([(cx - 14, y_bot - 8), (cx + 14, y_bot - 8), (cx, y_bot + 8)], fill=color)


def draw_right_arrow(draw, x_l, x_r, cy, color=(40, 40, 40), w=4):
    draw.line([(x_l, cy), (x_r - 18), cy], fill=color, width=w)
    draw.polygon([(x_r - 8, cy - 14), (x_r - 8, cy + 14), (x_r + 8, cy)], fill=color)


def draw_dashed_vline(draw, cx, y1, y2, color=(120, 120, 130), dash=12, gap=8, w=3):
    """垂直虚线"""
    y = y1
    while y < y2:
        ey = min(y + dash, y2)
        draw.line([(cx, y), (cx, ey)], fill=color, width=w)
        y += dash + gap


def centered_text(draw, box, text, font, fill=TEXT_DARK):
    """在矩形框内垂直+水平居中画一行文字"""
    x1, y1, x2, y2 = box
    bbox = draw.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    draw.text((x1 + (x2 - x1 - tw) / 2 - bbox[0],
               y1 + (y2 - y1 - th) / 2 - bbox[1]), text, font=font, fill=fill)


def left_text(draw, xy, text, font, fill=TEXT_DARK):
    x, y = xy
    draw.text((x, y), text, font=font, fill=fill)


# ================ 角色侧（左40%）绘制 ================
def paste_character_simple(canvas, character_img_path):
    """左侧贴专属角色立绘：直接保留自带的气泡/姿势/手持道具，不再盖旧气泡+重画
    - 适合 docs/img/characters/*.jpg 那种文生图自带正确气泡的成品立绘
    - 等比缩放到左 40% 区域 92% 宽，居中放置
    """
    draw = ImageDraw.Draw(canvas)

    # 1. 左半淡渐变白背景（增加层次）
    for y in range(CANVAS_H):
        t = y / CANVAS_H
        c = int(255 - 20 * t)
        draw.line([(0, y), (SPLIT_X, y)], fill=(c, c, 255))

    # 2. 加载 + 等比缩放
    char_img = Image.open(character_img_path).convert("RGBA")
    target_w = int(SPLIT_X * 0.92)
    ratio = target_w / char_img.width
    target_h = int(char_img.height * ratio)
    if target_h > int(CANVAS_H * 0.96):
        target_h = int(CANVAS_H * 0.96)
        ratio = target_h / char_img.height
        target_w = int(char_img.width * ratio)
    char_img = char_img.resize((target_w, target_h), Image.LANCZOS)

    # 3. 居中粘贴（由于立绘是4:3竖版，垂直方向上稍微偏上一点，避免气泡顶到画布边缘）
    paste_x = (SPLIT_X - target_w) // 2
    paste_y = (CANVAS_H - target_h) // 2 - 20  # 上移20px
    if paste_y < 20:
        paste_y = 20
    canvas.paste(char_img, (paste_x, paste_y), char_img if char_img.mode == "RGBA" else None)

    # 小水印
    draw.text((20, CANVAS_H - 50), "Workx · 鲸鱼娘图解系列", font=FONT_SM, fill=(130, 130, 150))


def find_character_image():
    """兼容保留：返回 None 代表每张图通过 CHARACTER_MAP 单独取专属立绘"""
    return None


# ================ 10 张图独立绘图函数 ================

def draw_01_react_loop(draw):
    """① ReAct 循环流程图"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    # 标题
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "① ReAct 推理循环工作流", FONT_BOLD_XL, fill=TEXT_NAVY)

    steps = [
        ("1", "Thought 思考",
         ["LLM 流式推理 → Thought",
          "解析 tool_use / FinalAnswer",
          "Token / Step 回调 → TUI"],
         (235, 222, 255)),  # 淡紫
        ("2", "Action 工具执行",
         ["并行调用多个工具 (n<=8)",
          "PermissionChecker 权限校验",
          "SecretScanner 密钥脱敏"],
         (216, 236, 255)),  # 淡蓝
        ("3", "Observation 观察",
         ["ToolResult → ChatMessage",
          "回写到 messages 历史",
          "TUI 右侧面板结果渲染"],
         (216, 245, 224)),  # 淡绿
        ("4", "FinalAnswer 终止",
         ["没有 tool_use 结束循环",
          "max_iterations(25) 兜底",
          "生成最终用户回复"],
         (255, 231, 209)),  # 淡橙
    ]

    box_top = area[1] + 120
    box_h = 220
    gap_v = 90  # 箭头高度
    w = area[2] - area[0]

    positions = []
    for i, (num, title, lines, color) in enumerate(steps):
        y = box_top + i * (box_h + gap_v)
        x1, x2 = area[0], area[2]
        rounded_rect(draw, (x1, y, x2, y + box_h), radius=24,
                     fill=color, outline=(100, 100, 130), width=2)
        # 左侧序号圆
        rounded_rect(draw, (x1 + 30, y + 60, x1 + 130, y + 160), radius=50,
                     fill=(255, 255, 255), outline=TEXT_NAVY, width=4)
        centered_text(draw, (x1 + 30, y + 60, x1 + 130, y + 160),
                      num, FONT_BOLD_XL, fill=TEXT_NAVY)
        # 标题
        draw.text((x1 + 180, y + 28), title, font=FONT_BOLD_L, fill=TEXT_NAVY)
        # 项目符号
        for li, line in enumerate(lines):
            draw.ellipse((x1 + 180, y + 98 + li * 38 + 4,
                          x1 + 196, y + 98 + li * 38 + 20), fill=TEXT_NAVY)
            draw.text((x1 + 218, y + 90 + li * 38),
                      line, font=FONT_REG_L, fill=TEXT_DARK)
        positions.append((x1, y, x2, y + box_h))
        # 向下箭头（除最后一个）
        if i < len(steps) - 1:
            draw_down_arrow(draw, (x1 + x2) // 2, y + box_h + 4, y + box_h + gap_v - 4)

    # 环形箭头（Step3 观察 → Step1 思考，循环）
    (x1, y1, x2, y2) = positions[2]  # Observation box
    (tx1, ty1, tx2, ty2) = positions[0]  # Thought box
    loop_x = x2 + 70
    # 右侧竖线：Observation 中 → Thought 中
    draw.line([(x2 - 20, y1 + box_h // 2), (loop_x, y1 + box_h // 2)],
              fill=(200, 60, 60), width=5)
    draw.line([(loop_x, y1 + box_h // 2), (loop_x, ty1 + box_h // 2)],
              fill=(200, 60, 60), width=5)
    draw.line([(loop_x, ty1 + box_h // 2), (tx1 + tx2 - tx1, ty1 + box_h // 2)],
              fill=(200, 60, 60), width=5)
    # 修正：回到 step1 左侧
    draw.line([(loop_x, ty1 + box_h // 2), (tx1 + 40, ty1 + box_h // 2)],
              fill=(200, 60, 60), width=5)
    draw.polygon([(tx1 + 24, ty1 + box_h // 2 - 14),
                  (tx1 + 24, ty1 + box_h // 2 + 14),
                  (tx1 + 2, ty1 + box_h // 2)], fill=(200, 60, 60))
    # 循环标签
    centered_text(draw, (loop_x - 60, (y1 + ty1 + box_h) // 2 - 24,
                         loop_x + 60, (y1 + ty1 + box_h) // 2 + 24),
                  "≤25轮", FONT_BOLD_L, fill=(200, 60, 60))

    # Step4 最后一个：终止符号
    (lx1, ly1, lx2, ly2) = positions[3]
    draw.text((lx2 - 260, ly2 + 32), "✓ 输出最终答案 → 用户", font=FONT_BOLD, fill=(40, 140, 80))


def draw_02_eventbus(draw):
    """② EventBus 星型中枢图"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "② EventBus 跨层事件驱动中枢", FONT_BOLD_XL, fill=TEXT_NAVY)

    # 中心 EventBus
    cx = (area[0] + area[2]) // 2
    cy = area[1] + (area[3] - area[1]) // 2
    bus_r = 130
    draw.ellipse((cx - bus_r, cy - bus_r, cx + bus_r, cy + bus_r),
                 fill=(130, 210, 255), outline=TEXT_NAVY, width=5)
    centered_text(draw, (cx - bus_r + 20, cy - 40, cx + bus_r - 20, cy - 20),
                  "EventBus", FONT_BOLD_XL, fill=TEXT_NAVY)
    centered_text(draw, (cx - bus_r + 10, cy + 5, cx + bus_r - 10, cy + 35),
                  "publish() 同步", FONT_BOLD, fill=(160, 50, 50))
    centered_text(draw, (cx - bus_r + 10, cy + 45, cx + bus_r - 10, cy + 75),
                  "publish_async() 异步", FONT_BOLD, fill=(40, 130, 70))

    # 6 个外围节点：上3个 Publisher，下3个 Subscriber
    def node(x1, y1, x2, y2, title, lines, color, is_pub):
        rounded_rect(draw, (x1, y1, x2, y2), radius=20,
                     fill=color, outline=(80, 80, 110), width=2)
        centered_text(draw, (x1, y1 + 8, x2, y1 + 52),
                      ("📢 " if is_pub else "👂 ") + title, FONT_BOLD_L, fill=TEXT_NAVY)
        for i, ln in enumerate(lines):
            centered_text(draw, (x1 + 10, y1 + 58 + i * 30, x2 - 10, y1 + 88 + i * 30),
                          ln, FONT_REG, fill=TEXT_DARK)

    w = 330
    h = 170
    pub_color = (255, 228, 200)
    sub_color = (216, 245, 224)

    # 上排 Publishers (左中右)
    y_top = area[1] + 110
    pub_x = area[0] + 10
    node(pub_x, y_top, pub_x + w, y_top + h,
         "ChatSession", ["MessageAppendedEvent", "StreamTokenEvent"], pub_color, True)
    node(cx - w // 2, y_top, cx + w // 2, y_top + h,
         "ReActLoop", ["NewThoughtEvent", "ToolCallStartEvent", "IterationDoneEvent"], pub_color, True)
    node(area[2] - w - 10, y_top, area[2] - 10, y_top + h,
         "Terminal", ["TerminalResizeEvent", "KeyPressedEvent", "UserSubmitEvent"], pub_color, True)

    # 下排 Subscribers
    y_bot = area[3] - h - 10
    node(area[0] + 10, y_bot, area[0] + 10 + w, y_bot + h,
         "ChatRenderer", ["Markdown 重渲染", "流式 token 更新", "Tree-sitter 高亮"], sub_color, False)
    node(cx - w // 2, y_bot, cx + w // 2, y_bot + h,
         "StatusBar / BottomBar", ["上下文条更新", "工具执行进度", "错误闪烁提示"], sub_color, False)
    node(area[2] - w - 10, y_bot, area[2] - 10, y_bot + h,
         "main.cpp 主循环", ["切换 Overlay", "等待用户输入", "Permission 弹窗"], sub_color, False)

    # 箭头：外围节点 ↔ Center（绿=异步安全，红=同步需谨慎）
    def arrow(nx, ny, is_async=True):
        color = (40, 170, 80) if is_async else (210, 60, 60)
        # 计算方向
        import math
        dx = cx - nx
        dy = cy - ny
        dist = math.hypot(dx, dy)
        sx = nx + dx * (bus_r + 20) / dist if dist else nx
        sy = ny + dy * (bus_r + 20) / dist if dist else ny
        # 从节点边到圆边
        ex = cx - dx * (bus_r + 60) / dist if dist else cx
        ey = cy - dy * (bus_r + 60) / dist if dist else cy
        draw.line([(sx, sy), (ex, ey)], fill=color, width=4)
        # 箭头头
        ang = math.atan2(ey - sy, ex - sx)
        ax1 = ex - 20 * math.cos(ang - 0.45)
        ay1 = ey - 20 * math.sin(ang - 0.45)
        ax2 = ex - 20 * math.cos(ang + 0.45)
        ay2 = ey - 20 * math.sin(ang + 0.45)
        draw.polygon([(ex, ey), (int(ax1), int(ay1)), (int(ax2), int(ay2))], fill=color)

    # 上排中心：画绿(异步)箭头（从 Publisher 指向中心）
    for nx, ny in [(area[0] + 10 + w // 2, y_top + h),
                   (cx, y_top + h),
                   (area[2] - 10 - w // 2, y_top + h)]:
        arrow(nx, ny, is_async=True)
    # 下排：从中心指到 Subscriber
    for nx, ny in [(area[0] + 10 + w // 2, y_bot),
                   (cx, y_bot),
                   (area[2] - 10 - w // 2, y_bot)]:
        # 反过来，从中心画到节点
        import math
        dx = nx - cx
        dy = ny - cy
        dist = math.hypot(dx, dy) or 1
        sx = cx + dx * (bus_r + 60) / dist
        sy = cy + dy * (bus_r + 60) / dist
        ex = nx - dx * (h / 2 - 10) / dist * 0
        draw.line([(sx, sy), (nx, ny)], fill=(40, 170, 80), width=4)
        ang = math.atan2(ny - sy, nx - sx)
        ax1 = nx - 20 * math.cos(ang - 0.45)
        ay1 = ny - 20 * math.sin(ang - 0.45)
        ax2 = nx - 20 * math.cos(ang + 0.45)
        ay2 = ny - 20 * math.sin(ang + 0.45)
        draw.polygon([(nx, ny), (int(ax1), int(ay1)), (int(ax2), int(ay2))], fill=(40, 170, 80))

    # 图例
    lx = area[0] + 20
    ly = area[3] - 60
    draw.rectangle((lx, ly, lx + 30, ly + 22), fill=(40, 170, 80))
    draw.text((lx + 42, ly - 2), "publish_async() 安全异步（推荐）", font=FONT_REG, fill=(40, 130, 70))
    draw.rectangle((lx + 450, ly, lx + 480, ly + 22), fill=(210, 60, 60))
    draw.text((lx + 492, ly - 2), "publish() 同步（持锁时禁止！易死锁）", font=FONT_REG, fill=(170, 40, 40))


def draw_03_render_pipeline(draw):
    """③ Resize/Overlay 渲染管线三泳道图"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "③ TUI Resize / Overlay 渲染管线（开发必读！）", FONT_BOLD_XL, fill=TEXT_NAVY)

    # 三列泳道
    col_w = (area[2] - area[0]) // 3
    cols = [
        (area[0] + 5, "TUI 主线程", (235, 222, 255)),
        (area[0] + 5 + col_w, "后台 ReAct 线程", (216, 236, 255)),
        (area[0] + 5 + col_w * 2, "m_output_mutex", (255, 243, 209)),
    ]
    for i, (cx_x, title, color) in enumerate(cols):
        rounded_rect(draw, (cx_x, area[1] + 105, cx_x + col_w - 10, area[3] - 70),
                     radius=14, fill=color, outline=(130, 130, 150), width=2)
        centered_text(draw, (cx_x, area[1] + 105, cx_x + col_w - 10, area[1] + 150),
                      title, FONT_BOLD_L, fill=TEXT_NAVY)

    # 三种状态：Normal / Overlay / Resize 三行块
    scenarios = [
        ("正常渲染 (Normal)", (216, 245, 224), [
            ["用户输入", "→ feed() 流式", "StreamingBuffer → 即时 flush"],
            ["（无阻塞 UI）", "LLM 后台推理", ""],
            ["", "lock_guard 短持锁", "刷新 buffer 到终端"],
        ]),
        ("Overlay 激活期间 ⚠", (255, 228, 200), [
            ["弹出选择面板/命令行", "Tool 后台继续跑", "🔒 m_overlay_active=true"],
            ["面板 UI 独立绘制", "新输出不会 flush 屏幕", "✗ 禁止 erase_output_zone"],
            ["", "→ 全部塞进 m_stream_buf", "✓ 仅更新 width/height 记录"],
            ["收起 Overlay → 统一 flush", "", "顺序: end_overlay → setup_scroll → cursor"],
        ]),
        ("Resize 事件 🔴（整批原子）", (255, 210, 210), [
            ["收到 WM_SIZE / SIGWINCH", "", "单一 lock_guard{ ... } 内完成："],
            ["", "", "① snapshot() 捕获当前内容"],
            ["", "", "② clear() 清屏"],
            ["", "", "③ set_width/set_height 写入新尺寸"],
            ["", "", "④ setup_scroll_region() 设滚动区"],
            ["", "", "⑤ replay() 按新尺寸重放内容"],
            ["TUI 重新绘制完成", "", "← 锁释放后再发布 TerminalResizeEvent"],
        ]),
    ]

    start_y = area[1] + 170
    num_cols = len(cols)
    for s_idx, (stitle, scolor, rows) in enumerate(scenarios):
        # 横向标题
        sub_y = start_y + 8
        rounded_rect(draw, (area[0] + 15, sub_y, area[2] - 15, sub_y + 44),
                     radius=10, fill=scolor, outline=(100, 100, 120), width=2)
        centered_text(draw, (area[0] + 15, sub_y, area[2] - 15, sub_y + 44),
                      stitle, FONT_BOLD_L, fill=TEXT_NAVY)
        start_y += 60
        # 每行按三列放内容（列越界保护）
        for ri, row in enumerate(rows):
            for ci, cell_text in enumerate(row):
                if not cell_text:
                    continue
                if ci >= num_cols:
                    break  # 列越界硬保护
                cx1 = cols[ci][0] + 12
                cx2 = cols[ci][0] + col_w - 22
                cy1 = start_y + ri * 42
                cy2 = start_y + ri * 42 + 36
                if cell_text.startswith(("①", "②", "③", "④", "⑤", "🔒", "✗", "✓", "←")):
                    draw.rectangle((cx1, cy1, cx2, cy2), fill=(255, 255, 255))
                    draw.text((cx1 + 6, cy1 + 4), cell_text, font=FONT_REG, fill=TEXT_DARK)
                else:
                    draw.text((cx1 + 4, cy1 + 4), cell_text, font=FONT_REG, fill=TEXT_DARK)
        start_y += len(rows) * 42 + 18

    # 底部警告
    rounded_rect(draw, (area[0] + 10, area[3] - 58, area[2] - 10, area[3] - 12),
                 radius=12, fill=(255, 230, 230), outline=(200, 60, 60), width=3)
    centered_text(draw, (area[0] + 10, area[3] - 58, area[2] - 10, area[3] - 12),
                  "⚠ 持锁期间绝对不能 publish 任何事件！否则会死锁（异步事件也要在释放锁后再发）",
                  FONT_BOLD, fill=(170, 30, 30))


def draw_04_tool_pipeline(draw):
    """④ Tool Calling 执行管线（纵向漏斗）"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "④ 工具调用执行全管线", FONT_BOLD_XL, fill=TEXT_NAVY)

    stages = [
        ("输入", "LLM 生成 tool_use (N 个)", (240, 230, 255), ["模型原生格式：name/input/id"]),
        ("1", "ToolRegistry 查表", (230, 240, 255), ["按 name 查 ITool 实例", "9+1 内置工具 + MCPTool 动态"]),
        ("2", "构造 ToolContext", (220, 245, 250), ["注入 cwd / ConfigManager / TaskManager", "Session id + 事件发布器"]),
        ("3", "Permission 校验 ⚠", (255, 240, 215),
         ["Ask: 弹 Modal 让用户 Y/N", "Auto: 直接放行", "Deny: 一律拒绝（错误回传）"]),
        ("4", "SecretScanner 脱敏", (255, 230, 220), ["regex 检测 AK/SK/Token", "日志里只留 *** 掩码"]),
        ("5", "并行执行 invoke()", (220, 255, 230),
         ["Bash/AgentTool → TaskManager 后台线程池", "File/Grep/MCP → 同步或轻线程", "最大并发 ≤ 8"]),
        ("6", "结果整理", (225, 240, 250), ["统一 ToolResult 结构", "再次脱敏输出字符串", "StatusBar 更新进度"]),
        ("7", "回写历史", (250, 230, 240), ["包装成 ChatMessage", "插入 messages[]", "触发下一轮 Thought"]),
    ]
    total = len(stages)
    y_start = area[1] + 120
    box_h = (area[3] - 100 - y_start) // total - 14
    width_total = area[2] - area[0]
    # 漏斗形状：起始宽度稍小中间最大尾部稍小
    for i, (num, title, color, lines) in enumerate(stages):
        shrink = int(abs(i - total / 2) * 18)
        x1 = area[0] + shrink
        x2 = area[2] - shrink
        y = y_start + i * (box_h + 14)
        rounded_rect(draw, (x1, y, x2, y + box_h), radius=18,
                     fill=color, outline=(110, 110, 140), width=2)
        # 序号
        r = 26
        draw.ellipse((x1 + 20, y + box_h // 2 - r, x1 + 20 + r * 2, y + box_h // 2 + r),
                     fill=(255, 255, 255), outline=TEXT_NAVY, width=3)
        centered_text(draw, (x1 + 20, y + box_h // 2 - r, x1 + 20 + r * 2, y + box_h // 2 + r),
                      num, FONT_BOLD, fill=TEXT_NAVY)
        # 标题
        draw.text((x1 + 92, y + 8), title, font=FONT_BOLD_L, fill=TEXT_NAVY)
        # 行
        for li, ln in enumerate(lines):
            draw.ellipse((x1 + 96, y + 58 + li * 30 + 5,
                          x1 + 110, y + 58 + li * 30 + 19), fill=TEXT_NAVY)
            draw.text((x1 + 128, y + 54 + li * 30), ln, font=FONT_REG_L, fill=TEXT_DARK)
        # 箭头
        if i < total - 1:
            draw_down_arrow(draw, (area[0] + area[2]) // 2, y + box_h + 2, y + box_h + 12)


def draw_05_token_compression(draw):
    """⑤ Token 压缩 + 预算机制"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "⑤ 上下文压缩 & Token 预算统计", FONT_BOLD_XL, fill=TEXT_NAVY)

    # 左：压缩流程
    left = (area[0], area[1] + 110, area[0] + (area[2] - area[0]) // 2 - 20, area[3] - 60)
    rounded_rect(draw, left, radius=20, fill=(240, 240, 255), outline=(130, 130, 150), width=2)
    centered_text(draw, (left[0], left[1] + 8, left[2], left[1] + 50),
                  "上下文压缩流程（超限触发）", FONT_BOLD_L, fill=TEXT_NAVY)

    comp_steps = [
        ("检测", "context_window() - 预估 tokens 计算",
         "优先级: provider → cfg → capability → preset → default"),
        ("判断", "总 tokens > 阈值（90% window）？",
         "超过才压缩；否则直接发送"),
        ("保留区（永不裁剪）",
         "• system prompt（含角色/工具描述）\n• 最近 5 轮对话\n• 上一轮 tool_calls + results",
         "保证逻辑闭环"),
        ("可裁剪区",
         "最早的历史对话 → 先裁掉 user/assistant 旧轮次\n如果还超 → 用 LLM 生成摘要替代中段",
         "裁剪数轮后循环复检"),
        ("复检",
         "二次检测：依然超限？降级模型能力\n如关闭多工具并行/缩短 Thought 长度",
         "兜底：抛 ContextOverflowError"),
    ]
    ys = left[1] + 68
    for i, (t1, t2, small) in enumerate(comp_steps):
        y1 = ys + i * ((left[3] - ys - 10) // len(comp_steps))
        y2 = y1 + (left[3] - ys - 10) // len(comp_steps) - 12
        rounded_rect(draw, (left[0] + 16, y1, left[2] - 16, y2), radius=12,
                     fill=(255, 255, 255), outline=(130, 130, 150), width=1)
        draw.text((left[0] + 30, y1 + 6), t1, font=FONT_BOLD, fill=TEXT_NAVY)
        draw.text((left[0] + 140, y1 + 6), t2, font=FONT_BOLD, fill=TEXT_DARK)
        draw.text((left[0] + 140, y1 + 38), small, font=FONT_SM, fill=(90, 90, 110))
        if i < len(comp_steps) - 1:
            draw_down_arrow(draw, (left[0] + left[2]) // 2, y2 + 2, y2 + 10, w=3)

    # 右：Token 预算统计面板
    right = (left[2] + 20, area[1] + 110, area[2], area[3] - 60)
    rounded_rect(draw, right, radius=20, fill=(230, 250, 240), outline=(130, 130, 150), width=2)
    centered_text(draw, (right[0], right[1] + 8, right[2], right[1] + 50),
                  "Token 预算面板（StatusBar 实时显示）", FONT_BOLD_L, fill=TEXT_NAVY)

    # 画一个大的进度条
    bar_x1 = right[0] + 40
    bar_y1 = right[1] + 80
    bar_x2 = right[2] - 40
    bar_y2 = right[1] + 140
    rounded_rect(draw, (bar_x1, bar_y1, bar_x2, bar_y2), radius=20,
                 fill=(245, 245, 245), outline=(100, 100, 130), width=3)
    # 填充 72%
    pct = 0.72
    fill_x2 = int(bar_x1 + (bar_x2 - bar_x1) * pct)
    rounded_rect(draw, (bar_x1, bar_y1, fill_x2, bar_y2), radius=20,
                 fill=(70, 160, 220))
    centered_text(draw, (bar_x1, bar_y1, bar_x2, bar_y2),
                  f"72% · 已用 72,000 / 100,000 tokens", FONT_BOLD_L, fill=(255, 255, 255) if False else TEXT_DARK)

    # 4 类计数方块
    counter_items = [
        ("Prompt Tokens", "58,240", (220, 240, 255)),
        ("Cache Create", "12,800", (255, 235, 210)),
        ("Cache Read", "1,050", (255, 220, 220)),
        ("Generated", "9,910", (220, 250, 225)),
    ]
    cw = (right[2] - right[0] - 80 - 30) // 4
    for i, (name, val, color) in enumerate(counter_items):
        bx = right[0] + 40 + i * (cw + 10)
        by = right[1] + 180
        bw = cw
        bh = 120
        rounded_rect(draw, (bx, by, bx + bw, by + bh), radius=16,
                     fill=color, outline=(110, 110, 140), width=2)
        centered_text(draw, (bx, by + 10, bx + bw, by + 48), name, FONT_BOLD, fill=TEXT_NAVY)
        centered_text(draw, (bx, by + 55, bx + bw, by + 105), val, FONT_BOLD_XL, fill=TEXT_DARK)

    # 下方：费用估算小条
    fy = right[1] + 320
    rounded_rect(draw, (right[0] + 40, fy, right[2] - 40, fy + 170), radius=16,
                 fill=(255, 255, 255), outline=(110, 110, 140), width=2)
    centered_text(draw, (right[0] + 40, fy + 12, right[2] - 40, fy + 50),
                  "本轮会话累计估算费用", FONT_BOLD_L, fill=TEXT_NAVY)
    centered_text(draw, (right[0] + 40, fy + 58, right[2] - 40, fy + 120),
                  "$ 0.0384 USD ≈ ¥ 0.28",
                  FONT_BOLD_XL, fill=(40, 130, 80))
    centered_text(draw, (right[0] + 40, fy + 122, right[2] - 40, fy + 160),
                  "（含 prompt caching 折扣减免 $0.012）", FONT_REG, fill=(110, 110, 130))


def draw_06_setup_wizard(draw):
    """⑥ Setup Wizard 首次启动引导流程"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "⑥ 首次运行 Setup Wizard 引导流程", FONT_BOLD_XL, fill=TEXT_NAVY)

    # 开始节点
    start_x = (area[0] + area[2]) // 2
    start_y = area[1] + 130
    draw.ellipse((start_x - 80, start_y - 50, start_x + 80, start_y + 50),
                 fill=(255, 240, 200), outline=TEXT_NAVY, width=4)
    centered_text(draw, (start_x - 80, start_y - 50, start_x + 80, start_y + 50),
                  "启动 workx.exe", FONT_BOLD_L, fill=TEXT_NAVY)

    # 判断
    j_y = start_y + 120
    draw.polygon([(start_x, j_y - 70), (start_x + 150, j_y),
                  (start_x, j_y + 70), (start_x - 150, j_y)],
                 fill=(255, 220, 220), outline=TEXT_NAVY, width=4)
    centered_text(draw, (start_x - 140, j_y - 30, start_x + 140, j_y),
                  "配置文件为空？", FONT_BOLD_L, fill=TEXT_NAVY)
    centered_text(draw, (start_x - 140, j_y, start_x + 140, j_y + 30),
                  "(Provider/Key/Remote)", FONT_REG, fill=TEXT_DARK)

    draw_down_arrow(draw, start_x, start_y + 50, j_y - 70)

    # 左：否 → 直接进入 REPL
    no_x = area[0] + 80
    no_y = j_y + 130
    rounded_rect(draw, (no_x, no_y, no_x + 360, no_y + 80), radius=18,
                 fill=(220, 250, 230), outline=(60, 150, 90), width=3)
    centered_text(draw, (no_x, no_y, no_x + 360, no_y + 80),
                  "✓ 直接进入 REPL 主界面", FONT_BOLD_L, fill=(30, 100, 50))
    # 否 箭头（向左下方绘制）
    draw.line([(start_x - 150, j_y), (no_x + 180, no_y)], fill=(40, 40, 40), width=4)
    ang = math.atan2(no_y - j_y, no_x + 180 - (start_x - 150))
    ax1 = no_x + 180 - 22 * math.cos(ang - 0.4)
    ay1 = no_y - 22 * math.sin(ang - 0.4)
    ax2 = no_x + 180 - 22 * math.cos(ang + 0.4)
    ay2 = no_y - 22 * math.sin(ang + 0.4)
    draw.polygon([(no_x + 180, no_y), (int(ax1), int(ay1)), (int(ax2), int(ay2))], fill=(40, 40, 40))
    draw.text((no_x + 40, j_y + 10), "否（已有配置）", font=FONT_BOLD, fill=(60, 140, 80))

    # 右：是 → 4步流程竖排
    yes_x = area[2] - 480
    # 标注是
    draw.text((start_x + 30, j_y - 20), "是 → 首次启动", font=FONT_BOLD, fill=(160, 60, 60))
    steps = [
        ("Step 1", "Provider 选择",
         ["7 个预设下拉：OpenAI / Anthropic / DeepSeek",
          "/ Groq / Together / 智谱 / 通义 / 零一万物",
          "+ LM-Studio / Ollama / OpenRouter 自定义"],
         (240, 230, 255)),
        ("Step 2", "API Key 粘贴输入",
         ["带 * 号遮罩输入",
          "本地加密后写入用户目录配置文件",
          "支持 environment variable 覆盖"],
         (220, 240, 255)),
        ("Step 3", "权限模式选择",
         ["🔘 Ask（推荐）：每次敏感工具弹窗确认",
          "🔘 Auto：所有工具自动放行（本地可信场景）",
          "🔘 Deny：除只读外全部拒绝"],
         (255, 240, 220)),
        ("Step 4", "自定义模型 / Remote（可跳过）",
         ["可选：填写非官方 base_url (Remote)",
          "可选：指定具体 model-id (claude-sonnet-4)",
          "默认按 Provider 自动选推荐模型"],
         (230, 250, 235)),
        ("完成 ✓", "写配置 → 进入 REPL",
         ["配置路径: ~/.config/workx/config.json",
          "之后启动都跳过 Wizard",
          "想重来：workx --wizard 手动触发"],
         (220, 250, 255)),
    ]
    s_w = 470
    s_h = 140
    s_y = j_y + 60
    # "是"箭头
    draw.line([(start_x + 150, j_y), (yes_x + s_w // 2, s_y)], fill=(40, 40, 40), width=4)
    for i, (tag, title, lines, color) in enumerate(steps):
        y = s_y + i * (s_h + 28)
        rounded_rect(draw, (yes_x, y, yes_x + s_w, y + s_h), radius=18,
                     fill=color, outline=(100, 100, 130), width=2)
        # 左上标签
        rounded_rect(draw, (yes_x + 16, y + 16, yes_x + 120, y + 58), radius=10,
                     fill=(255, 255, 255), outline=TEXT_NAVY, width=3)
        centered_text(draw, (yes_x + 16, y + 16, yes_x + 120, y + 58),
                      tag, FONT_BOLD, fill=TEXT_NAVY)
        draw.text((yes_x + 140, y + 16), title, font=FONT_BOLD_L, fill=TEXT_NAVY)
        for li, ln in enumerate(lines):
            draw.ellipse((yes_x + 146, y + 66 + li * 26 + 5,
                          yes_x + 160, y + 66 + li * 26 + 19), fill=TEXT_NAVY)
            draw.text((yes_x + 176, y + 62 + li * 26), ln, font=FONT_REG, fill=TEXT_DARK)
        if i < len(steps) - 1:
            draw_down_arrow(draw, yes_x + s_w // 2, y + s_h + 2, y + s_h + 26)


def draw_07_build_pipeline(draw):
    """⑦ 跨平台构建管线全景"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "⑦ CMake 跨平台构建管线 + 产物目录", FONT_BOLD_XL, fill=TEXT_NAVY)

    # 三平台列
    plat_colors = [
        ("🪟 Windows", (200, 225, 255), ["MSVC ≥v14.5", "vcpkg triplet x64-windows", "Pillow 生成 ICO"]),
        ("🍎 macOS", (255, 230, 240), ["Xcode / Clang", "arm64 + x86_64 双架构", "Pillow 生成 ICNS"]),
        ("🐧 Linux", (225, 255, 230), ["GCC ≥10 / Clang", "apt/yum 依赖 curl-dev", "用 PNG 图标直接"]),
    ]
    col_w = (area[2] - area[0]) // 3
    for i, (pname, pcolor, pmeta) in enumerate(plat_colors):
        cx = area[0] + i * col_w + 8
        cx2 = cx + col_w - 16
        rounded_rect(draw, (cx, area[1] + 110, cx2, area[3] - 60), radius=18,
                     fill=pcolor, outline=(100, 100, 130), width=2)
        centered_text(draw, (cx, area[1] + 115, cx2, area[1] + 155),
                      pname, FONT_BOLD_L, fill=TEXT_NAVY)
        for mj, mt in enumerate(pmeta):
            draw.text((cx + 14, area[1] + 162 + mj * 28),
                      "• " + mt, font=FONT_SM, fill=TEXT_DARK)
        # 共用流水线：列中画下来
        stages = [
            ("① vcpkg 安装依赖", (255, 255, 255),
             ["nlohmann-json", "libcurl", "catch2", "tree-sitter runtime"]),
            ("② FetchContent", (255, 255, 255),
             ["拉取 9 种 Tree-sitter grammar", "C/C++/CMake/Python", "Bash/JSON/JS/Rust/Go", "指定 GIT_TAG 版本"]),
            ("③ 编译子模块", (255, 255, 255),
             ["lib/liblogger → static lib", "src/core 基础库", "src/agent agent库", "src/tui tui库"]),
            ("④ 链接 workx.exe", (255, 255, 255),
             ["嵌入 .rc 资源 (Win)", "MACOSX_BUNDLE (mac)", "普通 ELF (Linux)", "生成可执行文件"]),
            ("⑤ POST_BUILD 打包", (255, 220, 220),
             ["convert_icon.py 图标 (可选)", "fetch-ripgrep.py → rg 二进制",
              "复制到 <exe>/tools/ 子目录", "图标 → <exe>/resources/"]),
        ]
        y = area[1] + 260
        s_h = ((area[3] - 110) - y - 20) // len(stages) - 10
        for si, (st, sc, lines) in enumerate(stages):
            rounded_rect(draw, (cx + 10, y, cx2 - 10, y + s_h), radius=12,
                         fill=sc, outline=(90, 90, 120), width=1)
            centered_text(draw, (cx + 10, y + 4, cx2 - 10, y + 32), st, FONT_BOLD, fill=TEXT_NAVY)
            for li, ln in enumerate(lines):
                draw.text((cx + 22, y + 34 + li * 22),
                          "· " + ln, font=FONT_SM, fill=TEXT_DARK)
            if si < len(stages) - 1:
                draw_down_arrow(draw, (cx + cx2) // 2, y + s_h + 1, y + s_h + 8, w=3)
            y += s_h + 10

    # 底部产物目录示例
    fy = area[3] - 50
    rounded_rect(draw, (area[0] + 20, fy - 4, area[2] - 20, fy + 40), radius=12,
                 fill=(245, 245, 245), outline=(100, 100, 130), width=1)
    centered_text(draw, (area[0] + 20, fy - 2, area[2] - 20, fy + 38),
                  "📁 产物示例：bin/workx(.exe) + tools/rg(.exe) + resources/icon.{ico,icns,png}",
                  FONT_BOLD, fill=TEXT_NAVY)


def draw_08_backend_adapter(draw):
    """⑧ LLM 后端适配层（三层架构 8 Providers）"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "⑧ LLM 多提供商适配层（插件式）", FONT_BOLD_XL, fill=TEXT_NAVY)

    # 顶层：应用层
    top_y = area[1] + 110
    top_w = area[2] - area[0]
    rounded_rect(draw, (area[0], top_y, area[2], top_y + 80), radius=22,
                 fill=(240, 230, 255), outline=TEXT_NAVY, width=3)
    centered_text(draw, (area[0], top_y, area[2], top_y + 80),
                  "上层：ReActLoop / ChatSession （只依赖接口，不知具体提供商）",
                  FONT_BOLD_L, fill=TEXT_NAVY)

    # 中间层：双接口隔离
    mid_y = top_y + 140
    mid_w = (area[2] - area[0] - 40) // 2
    rounded_rect(draw, (area[0], mid_y, area[0] + mid_w, mid_y + 110), radius=20,
                 fill=(216, 236, 255), outline=TEXT_NAVY, width=3)
    centered_text(draw, (area[0], mid_y + 10, area[0] + mid_w, mid_y + 50),
                  "ICompletionProvider", FONT_BOLD_XL, fill=TEXT_NAVY)
    centered_text(draw, (area[0], mid_y + 52, area[0] + mid_w, mid_y + 92),
                  "chat_stream() · 流式补全 + tool_use", FONT_BOLD, fill=TEXT_DARK)

    rounded_rect(draw, (area[2] - mid_w, mid_y, area[2], mid_y + 110), radius=20,
                 fill=(255, 240, 215), outline=TEXT_NAVY, width=3)
    centered_text(draw, (area[2] - mid_w, mid_y + 10, area[2], mid_y + 50),
                  "IBackendAdmin", FONT_BOLD_XL, fill=TEXT_NAVY)
    centered_text(draw, (area[2] - mid_w, mid_y + 52, area[2], mid_y + 92),
                  "list_models() · switch_model() · capability()", FONT_BOLD, fill=TEXT_DARK)

    # 箭头：上层 → 中间
    cx1 = area[0] + top_w * 0.28
    cx2 = area[0] + top_w * 0.72
    draw_down_arrow(draw, cx1, top_y + 80, mid_y - 10)
    draw_down_arrow(draw, cx2, top_y + 80, mid_y - 10)

    # 中间 → 下：BackendFactory
    fac_y = mid_y + 170
    rounded_rect(draw, (area[0] + top_w * 0.35, fac_y,
                        area[0] + top_w * 0.65, fac_y + 70), radius=18,
                 fill=(255, 220, 220), outline=TEXT_NAVY, width=3)
    centered_text(draw, (area[0] + top_w * 0.35, fac_y,
                         area[0] + top_w * 0.65, fac_y + 70),
                  "BackendFactory(provider_name)", FONT_BOLD_L, fill=TEXT_NAVY)
    # 中间接口向下两条线汇到 Factory
    draw.line([(area[0] + mid_w // 2, mid_y + 110),
               (area[0] + top_w * 0.35 + (top_w * 0.30) * 0.3, fac_y)],
              fill=(40, 40, 40), width=4)
    draw.line([(area[2] - mid_w // 2, mid_y + 110),
               (area[0] + top_w * 0.35 + (top_w * 0.30) * 0.7, fac_y)],
              fill=(40, 40, 40), width=4)

    # 最下：8 个 Adapter 框（2 行 × 4 列）
    adapters = [
        ("OpenAIAdapter", "GPT / o1 / 自定义兼容", (255, 230, 240)),
        ("AnthropicAdapter", "Claude 4.5/Opus", (230, 240, 255)),
        ("DeepSeekAdapter", "DeepSeek-V3/R1", (240, 255, 240)),
        ("GroqAdapter", "超快推理", (255, 255, 220)),
        ("TogetherAdapter", "开源模型托管", (255, 235, 230)),
        ("ZhiPu / Qwen / LingYi", "三家国内中文", (240, 250, 250)),
        ("LM-StudioAdapter", "本地模型 OAI兼容", (245, 240, 255)),
        ("OllamaAdapter", "本地开源 LLM", (235, 255, 240)),
    ]
    row_y = fac_y + 130
    a_w = (area[2] - area[0] - 60) // 4
    a_h = 100
    for i, (name, sub, acolor) in enumerate(adapters):
        col = i % 4
        row = i // 4
        ax = area[0] + 10 + col * (a_w + 18)
        ay = row_y + row * (a_h + 22)
        rounded_rect(draw, (ax, ay, ax + a_w, ay + a_h), radius=16,
                     fill=acolor, outline=(90, 90, 120), width=2)
        centered_text(draw, (ax, ay + 10, ax + a_w, ay + 48),
                      name, FONT_BOLD, fill=TEXT_NAVY)
        centered_text(draw, (ax, ay + 52, ax + a_w, ay + 82),
                      sub, FONT_SM, fill=TEXT_DARK)
        # Factory 到每个 Adapter 的箭头（简化：两竖一总）
        if row == 0 and col in (1, 2):  # 画两条总箭头
            tx = ax + a_w // 2
            draw.line([(area[0] + top_w * 0.35 + (top_w * 0.30) * (0.3 if col == 1 else 0.7),
                        fac_y + 70), (tx, ay)], fill=(40, 40, 40), width=3)
            ang = math.atan2(ay - (fac_y + 70),
                             tx - (area[0] + top_w * 0.35 + (top_w * 0.30) * (0.3 if col == 1 else 0.7)))
            ax1 = tx - 18 * math.cos(ang - 0.4)
            ay1 = ay - 18 * math.sin(ang - 0.4)
            ax2 = tx - 18 * math.cos(ang + 0.4)
            ay2 = ay - 18 * math.sin(ang + 0.4)
            draw.polygon([(tx, ay), (int(ax1), int(ay1)), (int(ax2), int(ay2))], fill=(40, 40, 40))
    # 共用底：SSE Parser + libcurl
    bot_y = row_y + 2 * (a_h + 22) + 10
    rounded_rect(draw, (area[0], bot_y, area[2], bot_y + 70), radius=16,
                 fill=(220, 220, 245), outline=(90, 90, 120), width=2)
    centered_text(draw, (area[0], bot_y, area[2], bot_y + 35),
                  "共用底层：SSE Parser (data: 行流式)   +   libcurl HTTP Client   +   retry.h 重试策略",
                  FONT_BOLD_L, fill=TEXT_NAVY)


def draw_09_mcp_bridge(draw):
    """⑨ MCP Tool 桥接工作流（四泳道）"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "⑨ MCP 协议跨进程工具桥接", FONT_BOLD_XL, fill=TEXT_NAVY)

    lanes = [
        ("ReActLoop", (240, 230, 255)),
        ("MCPTool 封装", (216, 236, 255)),
        ("MCP Server (第三方)", (255, 240, 215)),
        ("返回 Workx 管线", (220, 250, 230)),
    ]
    l_w = (area[2] - area[0]) // 4
    for i, (lname, lcolor) in enumerate(lanes):
        lx = area[0] + i * l_w + 4
        rounded_rect(draw, (lx, area[1] + 110, lx + l_w - 8, area[3] - 60),
                     radius=14, fill=lcolor, outline=(110, 110, 140), width=2)
        centered_text(draw, (lx, area[1] + 110, lx + l_w - 8, area[1] + 155),
                      lname, FONT_BOLD_L, fill=TEXT_NAVY)

    steps = [
        # (lane_idx, title, desc_lines, color_box)
        (0, "LLM 决定调用 mcp_*",
         ["tool_use.name = mcp_<server>__<tool>",
          "例如 mcp_filesystem__read_file",
          "参数按 JSON 传入"], (255, 255, 255)),
        (1, "查找动态注册的 MCPTool",
         ["MCPToolRegistry 存 Tool Schema",
          "启动时 list_tools RPC 拉取所有工具",
          "自动改名 mcp_svr__tool → name"], (255, 255, 255)),
        (1, "封装 JSON-RPC Request",
         ["method = tools/call",
          "params = {name, arguments}",
          "request id 递增"], (255, 255, 255)),
        (2, "通过 Transport 发送",
         ["stdio：父进程写子进程 stdin",
          "SSE：HTTPS POST 远端 MCP Server",
          "JSON-RPC 2.0 包格式"], (255, 255, 255)),
        (2, "服务端实际执行",
         ["Filesystem: 读/写/枚举文件",
          "GitHub: 查 PR/提 Issue",
          "Browser: 导航/点按钮/取 DOM"], (255, 255, 255)),
        (2, "返回 JSON-RPC Response",
         ["{id, result: {content:[{type,text}]}}",
          "出错时 {id, error: {code,message}}"], (255, 255, 255)),
        (3, "MCPTool 反序列化为 ToolResult",
         ["拼接 content[].text → 单个字符串",
          "超 4000 字符自动摘要",
          "错误映射成 Result::Err"], (255, 255, 255)),
        (0, "走通用工具管线",
         ["SecretScanner 脱敏",
          "Permission 结果持久化",
          "结果回写 messages 下一轮 Thought"], (255, 255, 255)),
    ]
    s_h = ((area[3] - 60) - (area[1] + 170)) // len(steps) - 8
    y0 = area[1] + 170
    for i, (li, title, descs, bcolor) in enumerate(steps):
        lx = area[0] + li * l_w + 20
        lw = l_w - 40
        y = y0 + i * (s_h + 8)
        rounded_rect(draw, (lx, y, lx + lw, y + s_h), radius=12,
                     fill=bcolor, outline=(100, 100, 130), width=1)
        draw.text((lx + 10, y + 6), title, font=FONT_BOLD, fill=TEXT_NAVY)
        for di, d in enumerate(descs):
            draw.text((lx + 14, y + 36 + di * 22),
                      "· " + d, font=FONT_SM, fill=TEXT_DARK)
        # 箭头：向下（同泳道相邻）或 斜向（跨泳道）
        if i < len(steps) - 1:
            nli, _, _, _ = steps[i + 1]
            from_cx = lx + lw // 2
            from_cy = y + s_h
            to_x = area[0] + nli * l_w + 20 + (l_w - 40) // 2
            to_y = y0 + (i + 1) * (s_h + 8)
            if nli == li:
                draw_down_arrow(draw, from_cx, from_cy + 1, from_cy + 7, w=3)
            else:
                # 先竖再横再竖（阶梯状）
                mid_y1 = from_cy + (to_y - from_cy) // 3
                mid_y2 = from_cy + (to_y - from_cy) * 2 // 3
                draw.line([(from_cx, from_cy), (from_cx, mid_y1)], fill=(40, 40, 40), width=3)
                draw.line([(from_cx, mid_y1), (to_x, mid_y2)], fill=(40, 40, 40), width=3)
                draw.line([(to_x, mid_y2), (to_x, to_y)], fill=(40, 40, 40), width=3)
                ang = math.atan2(to_y - mid_y2, 0.0001 if to_x - mid_y2 == 0 else 0)
                # 简单画三角形箭头
                draw.polygon([(to_x - 10, to_y - 4), (to_x + 10, to_y - 4), (to_x, to_y + 10)], fill=(40, 40, 40))


def draw_10_dependency(draw):
    """⑩ 项目依赖全景图（之前只写了Prompt未生成）"""
    area = (SPLIT_X + 60, 60, CANVAS_W - 60, CANVAS_H - 60)
    centered_text(draw, (area[0], area[1], area[2], area[1] + 90),
                  "⑩ 项目依赖全景图（6 层分类）", FONT_BOLD_XL, fill=TEXT_NAVY)

    layers = [
        ("1. 构建工具链 Toolchain", (245, 245, 247),
         ["CMake ≥ 3.21", "C++20 编译器 (MSVC 14.5+ / GCC 10+ / Clang)",
          "vcpkg 包管理器", "Python ≥ 3.8"]),
        ("2. vcpkg 声明依赖 (vcpkg.json)", (237, 228, 255),
         ["nlohmann-json  (JSON 序列化反序列化)",
          "curl / libcurl  (HTTP & HTTPS 请求)",
          "catch2  (单元测试框架)",
          "tree-sitter  (语法高亮运行时 core)"]),
        ("3. Tree-sitter Grammars × 9 (FetchContent)", (216, 236, 255),
         ["C v0.24.2      C++ v0.23.4      CMake v0.5.0",
          "Python v0.23.6   Bash v0.23.3     JSON v0.24.8",
          "JavaScript v0.23.1   Rust v0.23.1   Go v0.23.4"]),
        ("4. 内置自研/内嵌子模块", (216, 245, 224),
         ["lib/liblogger  DearTs Logger v1.0.0 (自研日志)",
          "src/icon.png   1024×1024 应用图标源图",
          "src/agent/tool  10+1 内置工具实现源码"]),
        ("5. Python 辅助脚本依赖 (scripts/)", (255, 231, 209),
         ["scripts/convert_icon.py  →  Pillow ≥10.1 (PNG→ICO/ICNS)",
          "scripts/fetch-ripgrep.py →  标准库 urllib/zipfile/tarfile  无需额外安装"]),
        ("6. Vendor 随 EXE 分发 (POST_BUILD 注入 tools/)", (255, 220, 231),
         ["ripgrep v14.1.1   (rg.exe / rg 跨平台预编译二进制)",
          "icon.ico (Win)   icon.icns (macOS)   icon.png (Linux)"]),
    ]
    n = len(layers)
    h = area[3] - 70 - (area[1] + 110)
    box_h = (h - (n - 1) * 40) // n
    y = area[1] + 110
    w = area[2] - area[0]
    for i, (title, color, lines) in enumerate(layers):
        rounded_rect(draw, (area[0], y, area[2], y + box_h), radius=22,
                     fill=color, outline=(110, 110, 140), width=2)
        draw.text((area[0] + 28, y + 16), title, font=FONT_BOLD_L, fill=TEXT_NAVY)
        for li, ln in enumerate(lines):
            draw.ellipse((area[0] + 40, y + 66 + li * 34 + 5,
                          area[0] + 56, y + 66 + li * 34 + 21), fill=TEXT_NAVY)
            draw.text((area[0] + 78, y + 62 + li * 34),
                      ln, font=FONT_REG_L, fill=TEXT_DARK)
        if i < n - 1:
            draw_down_arrow(draw, (area[0] + area[2]) // 2,
                            y + box_h + 2, y + box_h + 38)
            y += box_h + 40
    # 右侧 Package Manager 竖虚线
    dash_x = area[2] - 24
    draw_dashed_vline(draw, dash_x, area[1] + 115, area[3] - 80,
                      color=(100, 100, 120), dash=14, gap=10, w=3)
    centered_text(draw, (dash_x - 100, area[3] - 70, dash_x + 60, area[3] - 30),
                  "Package Manager", FONT_BOLD, fill=(100, 100, 120))


# ================ 10 张图汇总配置 ================
ALL_DIAGRAMS = [
    ("01_react_loop", "① ReAct 循环",
     ["Thought → Action →", "Observe 循环哦~"], draw_01_react_loop),
    ("02_eventbus_flow", "② EventBus 中枢",
     ["EventBus 是跨层的", "消息中枢哒！"], draw_02_eventbus),
    ("03_render_pipeline", "③ TUI 渲染管线",
     ["Resize/Overlay 超容易踩坑", "务必要按顺序喵~"], draw_03_render_pipeline),
    ("04_tool_pipeline", "④ 工具管线",
     ["工具要先过权限", "和密钥扫描哦😱"], draw_04_tool_pipeline),
    ("05_token_compression", "⑤ Token 压缩",
     ["超 Token 不会报错", "会裁最老对话哒~"], draw_05_token_compression),
    ("06_setup_wizard", "⑥ Setup 引导",
     ["第一次启动会问你", "4 个小问题啦~"], draw_06_setup_wizard),
    ("07_build_pipeline", "⑦ 构建管线",
     ["vcpkg + FetchContent", "一键构建三平台~"], draw_07_build_pipeline),
    ("08_backend_adapter", "⑧ LLM 适配层",
     ["加新 Provider 只要写", "一个 Adapter 就行！"], draw_08_backend_adapter),
    ("09_mcp_bridge", "⑨ MCP 桥接",
     ["MCP 是跨进程 JSON-RPC", "接第三方工具超省心~"], draw_09_mcp_bridge),
    ("10_dependency_overview", "⑩ 依赖全景",
     ["这些就是Workx", "全部的依赖哦~"], draw_10_dependency),
]


def get_character_path(filename):
    """从 CHARACTER_MAP 中查架构图对应的专属角色立绘路径，找不到回退到目录找第一个jpg"""
    char_name = CHARACTER_MAP.get(filename)
    if char_name:
        p = os.path.join(CHARACTER_DIR, char_name)
        if os.path.exists(p):
            return p
    # 回退：取第一个立绘
    for f in sorted(os.listdir(CHARACTER_DIR)):
        if f.lower().endswith((".jpg", ".png", ".jpeg")):
            return os.path.join(CHARACTER_DIR, f)
    raise FileNotFoundError(f"docs/img/characters/ 里找不到任何角色立绘文件！请确认 10 张文生图立绘都已生成。")


def build_one(filename, tag, bubble_lines, draw_fn):
    canvas = Image.new("RGB", (CANVAS_W, CANVAS_H), BG_COLOR)
    char_path = get_character_path(filename)
    paste_character_simple(canvas, char_path)  # 贴专属立绘，保留自带气泡
    draw = ImageDraw.Draw(canvas)
    draw_fn(draw)
    out_path = os.path.join(OUT_DIR, filename + ".jpg")
    canvas.save(out_path, "JPEG", quality=95, optimize=True)
    print(f"[✓] {out_path}   (立绘: {os.path.basename(char_path)})")


def main():
    print(f"[+] 专属角色立绘目录：{CHARACTER_DIR}")
    missing = [f for k, f in CHARACTER_MAP.items()
               if not os.path.exists(os.path.join(CHARACTER_DIR, f))]
    if missing:
        print(f"[!] 缺失立绘：{missing}")
    print(f"[+] 输出架构图目录：{OUT_DIR}")
    for cfg in ALL_DIAGRAMS:
        build_one(*cfg)
    print("[✓] 全部 10 张架构框图生成完毕！（每张使用自己对应的独立角色立绘）")


if __name__ == "__main__":
    main()
