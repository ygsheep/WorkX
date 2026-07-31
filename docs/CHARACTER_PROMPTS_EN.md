# Workx Whale-Girl Character · Prompt Library (English)

> Purpose: Generate matching chibi whale-girl character illustrations for Workx technical documentation.
> Output goes under `docs/img/characters/`, ready for README or developer docs.

---

## 🧩 Universal Character Base (MUST be identical across all 10 images)

Copy this **base paragraph** at the start of each image prompt to keep the character visually locked.

```text
masterpiece, best quality, ultra-detailed, chibi, SD big-head-small-body, 2.5-3 head proportions, anime-style expression pack, pure white background, no extra scenery, no ground shadow, thick 2-3px solid black manga-style outlines around every colored region, flat cel-shading with soft minimal shading, saturated and vivid colors.

Bright sapphire-blue long flowing hair past waist, slightly wavy fluffy ends. Twin split antenna ahoge on top of head (two U-shaped curves: one bends left, one bends right, base connected). Multiple oval white glossy highlight spots scattered across hair.
On hair right side (viewer left): a 3D cute white baby whale hair clip, two small black dot eyes, facing viewer.
Instead of human ears: two dark-navy-blue fish-fin ears sticking out sideways like small wings, each fin decorated with 2-3 small white round dots.

Neck: thin dark-navy choker collar, two connected white double-heart pendants dangling below.
Top: white long-sleeve sailor uniform shirt. Cuffs with double stripe (white stripe + royal blue stripe). Double layer sailor collar (royal blue wide outer edge, white narrow inner edge, V-shaped neckline). Large royal blue satin ribbon bow on chest, bow center has a tiny solid white mini whale round button.
Bottom: royal blue pleated short skirt, clear vertical pleats, slightly flared hem.
Behind body lower-right (viewer lower-left): a large gradient-blue whale tail with two leafy lobes curled upward, faint arc scale pattern on tail surface + 5-6 vertical white round spots.

Large oval white speech bubble on UPPER-LEFT of the canvas. Thick navy blue outline. Bubble bottom tail (pointer) touches character's mouth. Inside bubble, deep navy bold sans-serif Chinese text split into TWO lines. (Fill in the two lines per image below.)
```

---

## 🐳 10 Image Prompts (English · for SD / Midjourney / DALL·E 3 / Flux)

Append the **Emotion + Pose/Prop + Bubble Text** to the base paragraph above.

---

### 01 · ReAct Reasoning Loop

Insert after base:

```text
-- EMOTION --
confident smug small smile, mouth corners slightly up, asymmetric slanted eyebrows, deep blue irises with ring highlights, soft oval pink blush on both cheeks.
-- POSE / PROP --
right arm raised bent in front of body, right hand in "number one" gesture (only index finger extended UPWARD, other four fingers curled into fist, thumb tucked). Left hand gently resting on top of the chest bow.
-- BUBBLE TEXT LINES (CHINESE) --
Line1: Thought→Action→Observe
Line2: 循环跑哦~
```

**Midjourney params suffix**: `--niji 6 --ar 4:3 --style expressive --no blurry, lowres, extra fingers, deformed hands`

Suggested placement: README after architecture diagram; above `src/agent/core/react_loop.h`.

---

### 02 · EventBus Cross-Layer Event Hub

```text
-- EMOTION --
playful wink expression: right eye (viewer left) half-closed closed lids wink, left eye (viewer right) wide shiny round; little mischievous smirk with one small fang showing, pink blush cheeks.
-- POSE / PROP --
both hands raised in front of chest, all ten fingertips touching forming a triangle / gable shape (like a broadcast tower / radio antenna, symbolizing pub/sub message hub).
-- BUBBLE TEXT LINES --
Line1: 消息跨层传递
Line2: 全靠 EventBus 喵~
```

**MJ params**: `--niji 6 --ar 4:3 --style expressive`

Placement: README "Event-driven architecture" section; above `core/events/event_bus.h`.

---

### 03 · Resize / Overlay Render Pipeline (Dev Warning!)

```text
-- EMOTION --
stern angry warning expression: both eyebrows angled sharply downward (8-shape frown), half-lidded serious eyes with sharp glare, mouth slightly open showing one tiny fang, cheeks flushed red with anger. A small angry 3-lobe cloud puff symbol near top-left of head with a small cross/anger mark inside.
-- POSE / PROP --
left arm holds an old parchment scroll / checklist rolled up (wooden spindles on both ends, lines drawn on paper symbolizing the 5-step atomic sequence). Right hand raised, right index + middle fingers crossed forming an X / CROSS OUT sign (symbolizing DO NOT MIX UP ORDER).
-- BUBBLE TEXT LINES --
Line1: Resize顺序错了
Line2: 光标会乱飞哦😡
```

**MJ params**: `--niji 6 --ar 4:3 --style expressive --seed 12345` (fixed seed for consistent angry-face)

Placement: top of `docs/TUI_RENDER_PIPELINE.md` (required reading).

---

### 04 · Tool Calling Full Pipeline

```text
-- EMOTION --
alert warning surprised look: eyes wide round with small pupils, mouth small O shape open, one hand covering mouth whisper-reminder pose. Light sweat blush.
-- POSE / PROP --
right hand raised near mouth covering lips (the "psst! be careful" gesture). Left index finger pointing down toward viewer. In front of character floats a semi-transparent glowing light-blue CRYSTAL SPHERE orb, inside the orb levitate mini tool icons: wrench, folder, magnifier glass, laptop terminal, document page (represents 9+1 built-in tool set).
-- BUBBLE TEXT LINES --
Line1: 调工具前要过
Line2: 权限+密钥脱敏！
```

Placement: README "Agent Tools" section; above `src/agent/tool/`.

---

### 05 · Token Compression & Budget Panel

```text
-- EMOTION --
relieved happy smile: BOTH EYES closed into two crescent moon arcs / ^ ^ shapes (no visible pupils). Mouth corners up. 2-3 sweat drop symbols on the side of face.
-- POSE / PROP --
back of right hand / sleeve wiping sweat off forehead. Other hand holds up a large blue RECTANGLE PROGRESS-BAR SIGN BOARD: top white text "Tokens 72% ¥", below a gradient 70%-filled bar. Whole thing looks like a budget / usage dashboard.
-- BUBBLE TEXT LINES --
Line1: Token超了不会报错
Line2: 先裁最老对话哒~
```

Placement: README new "Context Management" chapter.

---

### 06 · Setup Wizard (First-Run Guide)

```text
-- EMOTION --
super-happy energetic welcome grin: eyes wide round with 4-pointed STAR sparkles in irises. Mouth wide open laughing showing one small fang + tongue, heavy deep pink blush cheeks.
-- POSE / PROP --
left hand holding a white feather QUILL PEN magic wand. Pen tip has a small glowing yellow circle sparkle (like guiding wizard). Right hand at side making "number FOUR" hand sign (four fingers outstretched, thumb tucked in) -> symbolizes 4 setup steps.
-- BUBBLE TEXT LINES --
Line1: 第一次启动会问你
Line2: 4个小问题啦~
```

Placement: README Quick Start / Run section before command examples.

---

### 07 · CMake Cross-Platform Build Pipeline

```text
-- EMOTION --
confident can-do grin: single-sided wink (left eye shut, right open), corner of mouth up showing one tiny fang, pumped thumbs-up energy vibe.
-- POSE / PROP --
right hand holds mini Windows logo (four-pane blue window). Left hand holds mini Apple logo (black square with white bitten apple). At character feet, a tiny Tux the Linux Penguin sitting (black head, white belly, orange feet) -> tri-platform symbolism.
-- BUBBLE TEXT LINES --
Line1: vcpkg + FetchContent
Line2: 一键三平台~
```

Placement: README build section after instructions.

---

### 08 · LLM Backend Adapters (8 Provider Plugin)

```text
-- EMOTION --
tsundere proud arms-crossed pose: arms folded tight over chest (right over left), chin slightly lifted up, one eyebrow raised cocky, little smirk, a 4-point sparkle STAR drawn near top-right of head.
-- POSE / PROP --
Orbiting around character body like satellite planets: 8 small colorful rounded square letter tiles each with white border and bold white letter:
O-purple (OpenAI) · A-red (Anthropic) · D-blue (DeepSeek) · G-yellow (Groq) · T-green (Together) · C-pink (Chinese 3) · LM-dark-navy (LM-Studio) · Ollama-light-blue.
-- BUBBLE TEXT LINES --
Line1: 加新Provider只要写
Line2: 一个Adapter就行！
```

Placement: README multi-provider section; inside `src/agent/api/`.

---

### 09 · MCP Cross-Process Tool Bridge

```text
-- EMOTION --
lightbulb "aha!" epiphany look: eyes large shiny round with sparkles. Floating above head a classic cartoon YELLOW LIGHTBULB with inner coil filament and 8 radial rays glowing. Mouth slightly open "I know!" shape.
-- POSE / PROP --
right index finger extended pointing straight UP (number 1 / important point pose). Left hand holding a RAINBOW-STRIPE DUAL-HEAD USB / NETWORK CABLE: cable body rainbow gradient stripes, both ends have grey USB-A plugs (symbolizes Workx ↔ external MCP Server connection).
-- BUBBLE TEXT LINES --
Line1: MCP是跨进程JSON-RPC
Line2: 接第三方工具超省心~
```

Placement: README MCP Integration section; `src/agent/tool/MCPTool/`.

---

### 10 · Project Dependency Overview (6 Layer Tower)

```text
-- EMOTION --
proud display grin: both eyes closed into happy crescents, mouth wide open laughing one fang, heavy blush.
-- POSE / PROP --
right hand thumb up / patting chest proud gesture. Left hand open palm presenting / introducing toward viewer's RIGHT side. Floating stack of 6 graduated-size colorful gift boxes with ribbons arranged as a tower (bottom largest, top smallest):
Orange bottom (build tools 🔧) → Blue (vcpkg 📦) → Green (tree-sitter 🌳) → Light-blue (in-house libs 💙) → Yellow (python scripts 🐍) → Purple smallest top (vendor bins 📥). Each box has a white label/tag with icon and category name.
-- BUBBLE TEXT LINES --
Line1: 这些就是Workx
Line2: 全部的依赖哦~
```

Placement: New README "Project Dependencies" chapter.

---

## 🔁 Negative Prompt (for Stable Diffusion family, paste into Negative)

```text
blurry, low quality, lowres, bad anatomy, bad hands, missing fingers, extra fingers, fewer digits, cropped, watermark, signature, jpeg artifacts, text artifacts, multiple girls, multiple views, deformed, mutation, ugly, out of frame, pillarboxed, letterboxed, gradient background, scenery, desk, floor shadow, cat ears, fox ears, non-fish-fin ears, wrong hair color, pink hair, green hair, purple hair, red clothes instead of sailor uniform.
```

## 🧪 Midjourney Shared Params (prepend `--`)

| Type | String |
|------|--------|
| Niji Anime Style | `--niji 6 --style expressive --stylize 200` |
| Consistent Seed (for same face across batch) | `--seed 42000` |
| Size (Portrait 4:3) | `--ar 4:3` |
| High detail | `--hd --q 2` |

> 💡 To keep face consistent across all 10, use the same `--seed XXXXX` value for all of them, plus add `character reference: [URL of the reference image]` if the MJ version supports `--cref`.

## ✏️ How to tweak an image (example)

If you want to change **05 Token Compression** bubble text and swap the sign:

- Replace the two `-- BUBBLE TEXT LINES --` entries.
- Swap the prop description from *"large blue rectangle progress-bar sign board"* to e.g. *"small pocket calculator screen showing token delta"*.
- Keep the **Universal Character Base** paragraph untouched, always.
