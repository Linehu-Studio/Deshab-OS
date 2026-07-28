/* deshab_ui.h — Deshab 视觉风格系统
 *
 * "Sealed Arc" 设计语言：以 UTSM 封缄内存为灵感的统一视觉体系。
 *
 * 核心设计隐喻：
 *   Arc（弧） — 封缄/解封的视觉表达，圆角、弧形边框、弧线动画
 *   Seal（封） — 密封容器的安全感，卡片容器、发光边缘、透明覆盖
 *   Flow（流） — 数据流的连续性，渐变背景、平滑过渡、流式布局
 *
 * 设计令牌分层：
 *   L0 — 原始色值（Deshab Palette）
 *   L1 — 语义色值（Semantic Colors，引用 L0）
 *   L2 — 组件令牌（Component Tokens，引用 L1）
 *
 * 所有 UI 组件必须通过本头文件获取视觉参数，禁止硬编码颜色/尺寸。
 */

#ifndef DESHAB_UI_H
#define DESHAB_UI_H

/* ===================================================================
 *  基础类型（自包含，不依赖 utsm/types.h 以便 DSK/Shell 独立使用）
 * =================================================================== */
typedef unsigned char      du_u8;
typedef unsigned short     du_u16;
typedef unsigned int       du_u32;
typedef unsigned long long du_u64;
typedef long long          du_i64;

/* ===================================================================
 *  L0 — Deshab 原始色板 (Deshab Palette)
 *
 *  色彩命名规则：{色调}_{明度}
 *  格式：0xAARRGGBB（与 framebuffer 一致，小端 x86 上 BB 置低位）
 * =================================================================== */

/* --- 主色调：深海蓝 (Abyss Blue) --- */
#define DP_ABYSS_900  0xFF0A1428u   /* 最深：终端背景、底色 */
#define DP_ABYSS_800  0xFF0F1E38u   /* 深层：暗卡片背景 */
#define DP_ABYSS_700  0xFF162848u   /* 暗面：悬停态背景 */
#define DP_ABYSS_600  0xFF1E3868u   /* 中暗：输入框背景 */
#define DP_ABYSS_500  0xFF284888u   /* 中间：次级强调 */
#define DP_ABYSS_400  0xFF3868B0u   /* 中亮：默认强调 */
#define DP_ABYSS_300  0xFF5088D0u   /* 亮面：链接色 */
#define DP_ABYSS_200  0xFF78A8E8u   /* 高亮：焦点指示 */
#define DP_ABYSS_100  0xFFA8CCF4u   /* 最亮：浅色辅助 */

/* --- 强调色：封缄青 (Seal Cyan) — 主要操作/交互 --- */
#define DP_SEAL_900   0xFF004858u
#define DP_SEAL_700   0xFF007898u
#define DP_SEAL_500   0xFF00A8CCu   /* 标准强调：按钮、选中态 */
#define DP_SEAL_300   0xFF44CCF0u   /* 高亮强调：焦点环 */
#define DP_SEAL_100   0xFF99E8FFu   /* 亮强调：发光边缘 */

/* --- 辅助色：弧光紫 (Arc Violet) — 次级/装饰 --- */
#define DP_ARC_900    0xFF2A1040u
#define DP_ARC_700    0xFF5A2080u
#define DP_ARC_500    0xFF8844CCu   /* 装饰强调：渐变终点、图标 */
#define DP_ARC_300    0xFFB888E8u
#define DP_ARC_100    0xFFE0C8F8u

/* --- 中性色 (Neutral) --- */
#define DP_NEUTRAL_0  0xFF000000u   /* 纯黑 */
#define DP_NEUTRAL_50 0xFF181820u   /* 接近黑 */
#define DP_NEUTRAL_100 0xFF2A2A38u  /* 暗灰 */
#define DP_NEUTRAL_200 0xFF484860u  /* 中暗灰 */
#define DP_NEUTRAL_300 0xFF6A6A88u  /* 中灰 */
#define DP_NEUTRAL_400 0xFF9090A8u  /* 中亮灰 */
#define DP_NEUTRAL_500 0xFFB0B0C0u  /* 标准灰 */
#define DP_NEUTRAL_600 0xFFD0D0DCu  /* 亮灰 */
#define DP_NEUTRAL_700 0xFFE8E8F0u  /* 浅灰 */
#define DP_NEUTRAL_800 0xFFF0F0F8u  /* 接近白 */
#define DP_NEUTRAL_900 0xFFFFFFFFu  /* 纯白 */

/* --- 语义辅助色 --- */
#define DP_SUCCESS    0xFF40C880u   /* 成功：绿色 */
#define DP_WARNING    0xFFF0A030u   /* 警告：琥珀色 */
#define DP_ERROR      0xFFFF4466u   /* 错误：红色 */
#define DP_INFO       0xFF4499FFu   /* 信息：蓝色 */

/* --- 渐变色 — DSK 加载器 / FirstInit 背景专用 --- */
#define DP_GRAD_TOP   0xFFC8E0F0u   /* 淡蓝顶 */
#define DP_GRAD_MID   0xFF8888B0u   /* 紫灰中 */
#define DP_GRAD_BOT   0xFF49306Fu   /* 深紫底 */

/* --- 欢迎页专用 --- */
#define DP_WELCOME_FG 0xFFE8F2FCu   /* 近白浅蓝（欢迎文字） */

/* ===================================================================
 *  L1 — 语义色值 (Semantic Colors)
 *
 *  暗色主题（Dark Theme）— Shell / 终端
 *  亮色主题（Light Theme）— FirstInit / DSK
 * =================================================================== */

/* --- 暗色主题 --- */
#define DS_DARK_BG_PRIMARY    DP_ABYSS_900       /* 主背景 */
#define DS_DARK_BG_SECONDARY  DP_ABYSS_800       /* 卡片/面板背景 */
#define DS_DARK_BG_TERTIARY   DP_ABYSS_700       /* 悬停/输入框 */
#define DS_DARK_BG_INSET      DP_ABYSS_600       /* 凹陷区域 */

#define DS_DARK_TEXT_PRIMARY   DP_NEUTRAL_700    /* 主文字 */
#define DS_DARK_TEXT_SECONDARY DP_NEUTRAL_500    /* 次级文字 */
#define DS_DARK_TEXT_DIM       DP_NEUTRAL_300    /* 注释/禁用 */
#define DS_DARK_TEXT_INVERT    DP_ABYSS_900      /* 反色文字（亮背景上） */

#define DS_DARK_ACCENT         DP_SEAL_500       /* 主强调 */
#define DS_DARK_ACCENT_LIGHT   DP_SEAL_300       /* 高亮强调 */
#define DS_DARK_ACCENT_GLOW    DP_SEAL_100       /* 发光 */
#define DS_DARK_DECORATIVE     DP_ARC_500        /* 装饰色 */
#define DS_DARK_DECORATIVE_L   DP_ARC_300        /* 浅装饰 */

#define DS_DARK_BORDER         DP_ABYSS_500      /* 边框 */
#define DS_DARK_BORDER_FOCUS   DP_SEAL_300       /* 焦点边框 */
#define DS_DARK_DIVIDER        DP_ABYSS_600      /* 分隔线 */

#define DS_DARK_CURSOR         DP_SEAL_500       /* 光标 */
#define DS_DARK_PROMPT         DP_SEAL_300       /* 提示符 */
#define DS_DARK_SELECTION_BG   DP_SEAL_900       /* 选中背景 */

/* --- 亮色主题 --- */
#define DS_LIGHT_BG_PRIMARY    DP_NEUTRAL_900    /* 主背景（白） */
#define DS_LIGHT_BG_SECONDARY  DP_NEUTRAL_700    /* 卡片背景 */
#define DS_LIGHT_BG_TERTIARY   DP_NEUTRAL_600    /* 悬停 */
#define DS_LIGHT_BG_INSET      DP_NEUTRAL_500    /* 输入框 */

#define DS_LIGHT_TEXT_PRIMARY   DP_ABYSS_900     /* 主文字 */
#define DS_LIGHT_TEXT_SECONDARY DP_ABYSS_500     /* 次级 */
#define DS_LIGHT_TEXT_DIM       DP_NEUTRAL_300   /* 注释 */
#define DS_LIGHT_TEXT_INVERT    DP_NEUTRAL_900   /* 反色 */

#define DS_LIGHT_ACCENT         DP_SEAL_700      /* 主强调（亮色上更深） */
#define DS_LIGHT_ACCENT_LIGHT   DP_SEAL_500
#define DS_LIGHT_DECORATIVE     DP_ARC_700

#define DS_LIGHT_BORDER         DP_NEUTRAL_400
#define DS_LIGHT_BORDER_FOCUS   DP_SEAL_500

/* ===================================================================
 *  排版规范 (Typography)
 * =================================================================== */

/* 字体尺寸族 */
#define DU_FONT_SIZE_XS    10u   /* 辅助标注 */
#define DU_FONT_SIZE_SM    14u   /* 次要说明 */
#define DU_FONT_SIZE_MD    18u   /* 正文（Consolas 18px） */
#define DU_FONT_SIZE_LG    24u   /* 小标题 */
#define DU_FONT_SIZE_XL    32u   /* 标题 */
#define DU_FONT_SIZE_2XL   48u   /* 大标题 / 欢迎文字 */

/* ASCII 位图字体参数（与 ascii_bitmaps.c 对应） */
#define DU_ASCII_CELL_W    11u
#define DU_ASCII_CELL_H    18u
#define DU_ASCII_STEP      12u   /* 字符间距（含 1px gap） */
#define DU_ASCII_LINE_H    20u   /* 行高（含 2px 行间距） */

/* ===================================================================
 *  间距体系 (Spacing)
 * =================================================================== */

#define DU_SPACE_XXS   2u
#define DU_SPACE_XS    4u
#define DU_SPACE_SM    8u
#define DU_SPACE_MD    16u
#define DU_SPACE_LG    24u
#define DU_SPACE_XL    32u
#define DU_SPACE_2XL   48u
#define DU_SPACE_3XL   64u

/* ===================================================================
 *  圆角体系 (Border Radius) — "Sealed Arc" 签名元素
 * =================================================================== */

#define DU_RADIUS_NONE  0u
#define DU_RADIUS_SM    4u    /* 小按钮、标签 */
#define DU_RADIUS_MD    8u    /* 卡片、输入框 */
#define DU_RADIUS_LG    12u   /* 大卡片、面板 */
#define DU_RADIUS_XL    16u   /* 模态框 */
#define DU_RADIUS_FULL  9999u /* 完全圆角（药丸形） */

/* ===================================================================
 *  边框体系 (Border)
 * =================================================================== */

#define DU_BORDER_THIN   1u
#define DU_BORDER_STD    2u
#define DU_BORDER_THICK  3u

/* ===================================================================
 *  动画参数 (Animation)
 * =================================================================== */

/* 帧延迟（busy-wait 循环次数 ≈ 毫秒级，需根据 CPU 速度校准） */
#define DU_ANIM_DELAY_SLOW    200000u   /* 慢速过渡 */
#define DU_ANIM_DELAY_NORMAL  100000u   /* 标准动画 */
#define DU_ANIM_DELAY_FAST     40000u   /* 快速反馈 */

/* 弧线动画参数（DSK spinner 专用 — Sealed Arc Comet 设计） */
#define DU_SPINNER_RADIUS     48u
#define DU_SPINNER_THICKNESS   5u
#define DU_SPINNER_HEAD_WIDTH  40u      /* 弧头宽度（度） */
#define DU_SPINNER_TAIL_LEN   220u      /* 拖尾长度（度） */
#define DU_SPINNER_SPEED       4u      /* 每帧旋转度数 */
#define DU_SPINNER_SPRITE     128u    /* 精灵缓冲边长 */

/* 渐变动画步进 */
#define DU_FADE_STEPS         32u      /* 淡入/淡出步数 */
#define DU_FADE_STEP_DELAY   8000u     /* 每步延迟 */

/* ===================================================================
 *  组件尺寸 (Component Sizes)
 * =================================================================== */

#define DU_BUTTON_HEIGHT     36u
#define DU_INPUT_HEIGHT      32u
#define DU_CARD_PADDING_X    24u
#define DU_CARD_PADDING_Y    16u
#define DU_CARD_MIN_WIDTH   200u
#define DU_CARD_MAX_WIDTH   480u
#define DU_CURSOR_SIZE       24u       /* 鼠标光标边长 */
#define DU_NOTIFICATION_H    40u

/* ===================================================================
 *  渲染上下文 (Render Context)
 *
 *  所有绘制函数通过此上下文访问 framebuffer，避免全局状态。
 * =================================================================== */

typedef struct {
    du_u32 *fb;          /* framebuffer 基址 */
    du_u64  width;       /* 像素宽 */
    du_u64  height;      /* 像素高 */
    du_u64  pitch;       /* 字节行宽 */
} du_context;

/* 初始化渲染上下文 */
static inline void du_context_init(du_context *ctx, du_u64 fb_addr,
                                    du_u64 w, du_u64 h, du_u64 pitch) {
    ctx->fb     = (du_u32 *)(du_u64)fb_addr;
    ctx->width  = w;
    ctx->height = h;
    ctx->pitch  = pitch;
}

/* ===================================================================
 *  L2 — 绘制原语 (Rendering Primitives)
 * =================================================================== */

/* 颜色混合：alpha 范围 0–255 */
static inline du_u32 du_blend(du_u32 bg, du_u32 fg, du_u32 alpha) {
    du_u32 na = 256 - alpha;
    du_u32 r  = ((bg & 0xFF) * na + (fg & 0xFF) * alpha) >> 8;
    du_u32 g  = (((bg >> 8) & 0xFF) * na + ((fg >> 8) & 0xFF) * alpha) >> 8;
    du_u32 b  = (((bg >> 16) & 0xFF) * na + ((fg >> 16) & 0xFF) * alpha) >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* 线性插值两色 */
static inline du_u32 du_lerp_color(du_u32 c1, du_u32 c2, du_u32 t) {
    return du_blend(c1, c2, t);
}

/* 渐变背景：顶色 → 底色，按 Y 坐标插值 */
static inline du_u32 du_gradient_v(du_u32 top_color, du_u32 bot_color,
                                    du_i64 y, du_i64 total_h) {
    if (total_h <= 1) return top_color;
    if (y < 0) y = 0;
    if (y >= total_h) y = total_h - 1;
    du_u32 alpha = (du_u32)(((du_u64)y * 255ULL) / ((du_u64)total_h - 1));
    return du_blend(top_color, bot_color, alpha);
}

/* 单像素写（带边界检查） */
static inline void du_pixel(du_context *ctx, du_i64 x, du_i64 y, du_u32 color) {
    if (x < 0 || (du_u64)x >= ctx->width || y < 0 || (du_u64)y >= ctx->height) return;
    ((du_u32 *)((du_u8 *)ctx->fb + (du_u64)y * ctx->pitch))[(du_u64)x] = color;
}

/* 读取像素（带边界检查） */
static inline du_u32 du_pixel_read(du_context *ctx, du_i64 x, du_i64 y) {
    if (x < 0 || (du_u64)x >= ctx->width || y < 0 || (du_u64)y >= ctx->height) return 0;
    return ((du_u32 *)((du_u8 *)ctx->fb + (du_u64)y * ctx->pitch))[(du_u64)x];
}

/* 矩形填充 */
static inline void du_fill_rect(du_context *ctx, du_i64 x, du_i64 y,
                                 du_i64 w, du_i64 h, du_u32 color) {
    for (du_i64 r = 0; r < h; r++) {
        du_i64 yy = y + r;
        if (yy < 0 || (du_u64)yy >= ctx->height) continue;
        du_u32 *line = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)yy * ctx->pitch);
        for (du_i64 c = 0; c < w; c++) {
            du_i64 xx = x + c;
            if (xx < 0 || (du_u64)xx >= ctx->width) continue;
            line[(du_u64)xx] = color;
        }
    }
}

/* 渐变矩形填充（垂直渐变） */
static inline void du_fill_rect_gradient(du_context *ctx, du_i64 x, du_i64 y,
                                          du_i64 w, du_i64 h,
                                          du_u32 top_color, du_u32 bot_color) {
    for (du_i64 r = 0; r < h; r++) {
        du_i64 yy = y + r;
        if (yy < 0 || (du_u64)yy >= ctx->height) continue;
        du_u32 color = du_gradient_v(top_color, bot_color, r, h);
        du_u32 *line = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)yy * ctx->pitch);
        for (du_i64 c = 0; c < w; c++) {
            du_i64 xx = x + c;
            if (xx < 0 || (du_u64)xx >= ctx->width) continue;
            line[(du_u64)xx] = color;
        }
    }
}

/* 全屏渐变背景 */
static inline void du_fill_bg_gradient(du_context *ctx,
                                        du_u32 top_color, du_u32 bot_color) {
    du_fill_rect_gradient(ctx, 0, 0, (du_i64)ctx->width, (du_i64)ctx->height,
                          top_color, bot_color);
}

/* 全屏纯色背景 */
static inline void du_fill_bg_solid(du_context *ctx, du_u32 color) {
    du_fill_rect(ctx, 0, 0, (du_i64)ctx->width, (du_i64)ctx->height, color);
}

/* ===================================================================
 *  圆角矩形 (Rounded Rectangle) — "Sealed Arc" 核心形状
 * =================================================================== */

/* 绘制圆角矩形边框（单像素宽） */
static inline void du_rect_outline(du_context *ctx, du_i64 x, du_i64 y,
                                    du_i64 w, du_i64 h,
                                    du_u32 color, du_u32 radius) {
    /* 四个角的圆弧 + 四条直线 */
    du_i64 r = (du_i64)radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* 顶线 */
    for (du_i64 c = r; c < w - r; c++) {
        du_pixel(ctx, x + c, y, color);
    }
    /* 底线 */
    for (du_i64 c = r; c < w - r; c++) {
        du_pixel(ctx, x + c, y + h - 1, color);
    }
    /* 左线 */
    for (du_i64 rr = r; rr < h - r; rr++) {
        du_pixel(ctx, x, y + rr, color);
    }
    /* 右线 */
    for (du_i64 rr = r; rr < h - r; rr++) {
        du_pixel(ctx, x + w - 1, y + rr, color);
    }
    /* 四个圆角 */
    for (du_i64 dy = 0; dy <= r; dy++) {
        for (du_i64 dx = 0; dx <= r; dx++) {
            du_i64 d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 > (r - 1) * (r - 1)) {
                /* 左上 */
                du_pixel(ctx, x + r - dx, y + r - dy, color);
                /* 右上 */
                du_pixel(ctx, x + w - 1 - r + dx, y + r - dy, color);
                /* 左下 */
                du_pixel(ctx, x + r - dx, y + h - 1 - r + dy, color);
                /* 右下 */
                du_pixel(ctx, x + w - 1 - r + dx, y + h - 1 - r + dy, color);
            }
        }
    }
}

/* 填充圆角矩形 */
static inline void du_fill_rounded_rect(du_context *ctx, du_i64 x, du_i64 y,
                                          du_i64 w, du_i64 h,
                                          du_u32 color, du_u32 radius) {
    du_i64 r = (du_i64)radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* 中间区域（不含角） */
    du_fill_rect(ctx, x + r, y, w - 2 * r, h, color);
    /* 左侧矩形 */
    du_fill_rect(ctx, x, y + r, r, h - 2 * r, color);
    /* 右侧矩形 */
    du_fill_rect(ctx, x + w - r, y + r, r, h - 2 * r, color);

    /* 四个圆角填充 */
    for (du_i64 dy = 0; dy < r; dy++) {
        for (du_i64 dx = 0; dx < r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                /* 左上 */
                du_pixel(ctx, x + r - 1 - dx, y + r - 1 - dy, color);
                /* 右上 */
                du_pixel(ctx, x + w - r + dx, y + r - 1 - dy, color);
                /* 左下 */
                du_pixel(ctx, x + r - 1 - dx, y + h - r + dy, color);
                /* 右下 */
                du_pixel(ctx, x + w - r + dx, y + h - r + dy, color);
            }
        }
    }
}

/* 带发光效果的圆角矩形边框（模拟 UTSM 封缄发光） */
static inline void du_rect_glow(du_context *ctx, du_i64 x, du_i64 y,
                                  du_i64 w, du_i64 h,
                                  du_u32 glow_color, du_u32 radius,
                                  du_i64 glow_width) {
    /* 从外到内逐层降低透明度 */
    for (du_i64 i = glow_width; i >= 1; i--) {
        du_u32 alpha = (du_u32)(40 + (glow_width - i) * 50 / glow_width);
        du_u32 color = du_blend(DP_ABYSS_900, glow_color, alpha);
        du_rect_outline(ctx, x - i, y - i, w + 2 * i, h + 2 * i, color, radius + (du_u32)i);
    }
    /* 核心边框 */
    du_rect_outline(ctx, x, y, w, h, glow_color, radius);
}

/* ===================================================================
 *  分隔线 (Divider)
 * =================================================================== */

static inline void du_divider_h(du_context *ctx, du_i64 x, du_i64 y,
                                 du_i64 width, du_u32 color) {
    du_fill_rect(ctx, x, y, width, 1, color);
}

static inline void du_divider_v(du_context *ctx, du_i64 x, du_i64 y,
                                 du_i64 height, du_u32 color) {
    du_fill_rect(ctx, x, y, 1, height, color);
}

/* ===================================================================
 *  ASCII 字符渲染（需要外部提供 g_ascii 位图数组）
 *
 *  使用方法：包含 ascii_bitmaps.c 后调用 du_draw_char / du_draw_string
 * =================================================================== */

/* ASCII 位图数组 — 由 ascii_bitmaps.c 定义（static），在包含该 .c 的编译单元内可见 */

/* 渲染单个 ASCII 字符（灰度位图 + 前景色 alpha 混合） */
static inline void du_draw_char(du_context *ctx, char ch,
                                 du_i64 x, du_i64 y,
                                 du_u32 fg, du_u32 bg) {
    du_u32 idx = (du_u32)(ch - ' ');
    if (idx > 94) idx = 0;
    const du_u8 *glyph = g_ascii[idx];
    du_i64 gw = g_ascii_w;
    du_i64 gh = g_ascii_h;
    for (du_i64 r = 0; r < gh; r++) {
        du_i64 yy = y + r;
        if (yy < 0 || (du_u64)yy >= ctx->height) continue;
        du_u32 *line = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)yy * ctx->pitch);
        for (du_i64 c = 0; c < gw; c++) {
            du_u8 a = glyph[r * gw + c];
            if (a == 0) continue;
            du_i64 xx = x + c;
            if (xx < 0 || (du_u64)xx >= ctx->width) continue;
            line[(du_u64)xx] = (a == 255) ? fg : du_blend(bg, fg, a);
        }
    }
}

/* 渲染字符串（不换行，无自动折行） */
static inline void du_draw_string(du_context *ctx, const char *s,
                                   du_i64 x, du_i64 y,
                                   du_u32 fg, du_u32 bg,
                                   du_u32 step) {
    du_i64 cx = x;
    while (*s) {
        du_draw_char(ctx, *s, cx, y, fg, bg);
        cx += (du_i64)step;
        s++;
    }
}

/* ===================================================================
 *  组件令牌 — 按钮 (Button)
 * =================================================================== */

typedef struct {
    du_u32 bg_normal;     /* 正常背景 */
    du_u32 bg_hover;      /* 悬停背景 */
    du_u32 bg_active;     /* 按下背景 */
    du_u32 fg;            /* 文字色 */
    du_u32 border;        /* 边框色 */
    du_u32 border_focus;  /* 焦点边框色 */
    du_u32 radius;        /* 圆角半径 */
    du_u32 height;        /* 高度 */
    du_u32 padding_x;     /* 水平内边距 */
} du_button_style;

/* 暗色主题按钮样式 */
#define DU_BTN_DARK_PRIMARY  { \
    DS_DARK_ACCENT, DS_DARK_ACCENT_LIGHT, DP_SEAL_700, \
    DP_NEUTRAL_900, DP_SEAL_300, DP_SEAL_100, \
    DU_RADIUS_MD, DU_BUTTON_HEIGHT, DU_SPACE_LG \
}

#define DU_BTN_DARK_SECONDARY { \
    DS_DARK_BG_TERTIARY, DS_DARK_BG_INSET, DS_DARK_BG_INSET, \
    DS_DARK_TEXT_PRIMARY, DS_DARK_BORDER, DS_DARK_BORDER_FOCUS, \
    DU_RADIUS_MD, DU_BUTTON_HEIGHT, DU_SPACE_LG \
}

/* ===================================================================
 *  组件令牌 — 卡片 (Card)
 * =================================================================== */

typedef struct {
    du_u32 bg;              /* 卡片背景 */
    du_u32 border;          /* 边框色 */
    du_u32 border_focus;    /* 焦点边框色 */
    du_u32 shadow_color;    /* 阴影色 */
    du_u32 radius;          /* 圆角半径 */
    du_u32 padding_x;       /* 水平内边距 */
    du_u32 padding_y;       /* 垂直内边距 */
    du_u32 border_width;    /* 边框宽度 */
} du_card_style;

#define DU_CARD_DARK { \
    DS_DARK_BG_SECONDARY, DS_DARK_BORDER, DS_DARK_BORDER_FOCUS, \
    DP_ABYSS_900, DU_RADIUS_LG, DU_CARD_PADDING_X, DU_CARD_PADDING_Y, DU_BORDER_STD \
}

#define DU_CARD_LIGHT { \
    DS_LIGHT_BG_SECONDARY, DS_LIGHT_BORDER, DS_LIGHT_BORDER_FOCUS, \
    DP_NEUTRAL_300, DU_RADIUS_LG, DU_CARD_PADDING_X, DU_CARD_PADDING_Y, DU_BORDER_STD \
}

/* ===================================================================
 *  组件令牌 — 输入框 (Input)
 * =================================================================== */

typedef struct {
    du_u32 bg;
    du_u32 bg_focus;
    du_u32 fg;
    du_u32 placeholder;
    du_u32 border;
    du_u32 border_focus;
    du_u32 cursor;
    du_u32 radius;
    du_u32 height;
} du_input_style;

#define DU_INPUT_DARK { \
    DS_DARK_BG_INSET, DS_DARK_BG_TERTIARY, DS_DARK_TEXT_PRIMARY, \
    DS_DARK_TEXT_DIM, DS_DARK_BORDER, DS_DARK_BORDER_FOCUS, \
    DS_DARK_CURSOR, DU_RADIUS_MD, DU_INPUT_HEIGHT \
}

/* ===================================================================
 *  组件令牌 — 通知栏 (Notification)
 * =================================================================== */

typedef enum {
    DU_NOTIFY_INFO,
    DU_NOTIFY_SUCCESS,
    DU_NOTIFY_WARNING,
    DU_NOTIFY_ERROR
} du_notify_type;

static inline du_u32 du_notify_color(du_notify_type type) {
    switch (type) {
    case DU_NOTIFY_SUCCESS: return DP_SUCCESS;
    case DU_NOTIFY_WARNING: return DP_WARNING;
    case DU_NOTIFY_ERROR:   return DP_ERROR;
    default:                return DP_INFO;
    }
}

/* ===================================================================
 *  组件绘制函数 (Component Drawing)
 * =================================================================== */

/* 绘制按钮 */
static inline void du_draw_button(du_context *ctx,
                                   du_i64 x, du_i64 y, du_i64 w,
                                   const du_button_style *style,
                                   const char *label, int focused) {
    du_u32 bg     = style->bg_normal;
    du_u32 border = style->border;
    if (focused) {
        border = style->border_focus;
    }
    /* 填充圆角矩形背景 */
    du_fill_rounded_rect(ctx, x, y, w, (du_i64)style->height, bg, style->radius);
    /* 边框 */
    du_rect_outline(ctx, x, y, w, (du_i64)style->height, border, style->radius);
    /* 文字居中 */
    du_i64 label_len = 0;
    while (label[label_len]) label_len++;
    du_i64 text_w = label_len * (du_i64)DU_ASCII_STEP;
    du_i64 text_x = x + (w - text_w) / 2;
    du_i64 text_y = y + ((du_i64)style->height - (du_i64)DU_ASCII_CELL_H) / 2;
    du_draw_string(ctx, label, text_x, text_y, style->fg, bg, DU_ASCII_STEP);
}

/* 绘制卡片 */
static inline void du_draw_card(du_context *ctx,
                                 du_i64 x, du_i64 y, du_i64 w, du_i64 h,
                                 const du_card_style *style, int focused) {
    du_u32 border = style->border;
    if (focused) {
        border = style->border_focus;
        /* 焦点发光效果 */
        du_rect_glow(ctx, x - 2, y - 2, w + 4, h + 4,
                     border, style->radius + 2, 3);
    }
    du_fill_rounded_rect(ctx, x, y, w, h, style->bg, style->radius);
    du_rect_outline(ctx, x, y, w, h, border, style->radius);
}

/* 绘制输入框 */
static inline void du_draw_input(du_context *ctx,
                                  du_i64 x, du_i64 y, du_i64 w,
                                  const du_input_style *style,
                                  const char *text, int cursor_pos,
                                  int focused) {
    du_u32 bg     = focused ? style->bg_focus : style->bg;
    du_u32 border = focused ? style->border_focus : style->border;
    du_fill_rounded_rect(ctx, x, y, w, (du_i64)style->height, bg, style->radius);
    du_rect_outline(ctx, x, y, w, (du_i64)style->height, border, style->radius);

    /* 文字 */
    du_i64 text_x = x + (du_i64)DU_SPACE_SM;
    du_i64 text_y = y + ((du_i64)style->height - (du_i64)DU_ASCII_CELL_H) / 2;
    du_u32 fg = text[0] ? style->fg : style->placeholder;
    du_draw_string(ctx, text, text_x, text_y, fg, bg, DU_ASCII_STEP);

    /* 光标 */
    if (focused && cursor_pos >= 0) {
        du_i64 cx = text_x + (du_i64)cursor_pos * (du_i64)DU_ASCII_STEP;
        du_fill_rect(ctx, cx, text_y, 2, (du_i64)DU_ASCII_CELL_H, style->cursor);
    }
}

/* 绘制通知栏 */
static inline void du_draw_notification(du_context *ctx,
                                         du_i64 x, du_i64 y, du_i64 w,
                                         du_notify_type type,
                                         const char *message) {
    du_u32 accent = du_notify_color(type);
    /* 半透明背景 */
    du_fill_rounded_rect(ctx, x, y, w, (du_i64)DU_NOTIFICATION_H,
                         DP_ABYSS_800, DU_RADIUS_MD);
    /* 左侧色条 */
    du_fill_rect(ctx, x, y + 4, 4, (du_i64)DU_NOTIFICATION_H - 8, accent);
    /* 文字 */
    du_i64 text_y = y + ((du_i64)DU_NOTIFICATION_H - (du_i64)DU_ASCII_CELL_H) / 2;
    du_draw_string(ctx, message, x + (du_i64)DU_SPACE_MD, text_y,
                   DS_DARK_TEXT_PRIMARY, DP_ABYSS_800, DU_ASCII_STEP);
}

/* 绘制进度条 */
static inline void du_draw_progress(du_context *ctx,
                                     du_i64 x, du_i64 y, du_i64 w, du_i64 h,
                                     du_u32 progress_0_255) {
    /* 背景 */
    du_fill_rounded_rect(ctx, x, y, w, h, DP_ABYSS_700, DU_RADIUS_FULL);
    /* 填充 */
    du_i64 fill_w = (w * (du_i64)progress_0_255) / 255;
    if (fill_w > 0) {
        du_fill_rounded_rect(ctx, x, y, fill_w, h, DP_SEAL_500, DU_RADIUS_FULL);
    }
}

/* ===================================================================
 *  Spinner（加载弧） — DSK 启动加载动画
 *
 *  使用 "Sealed Arc Comet" 单弧彗星尾设计：
 *  一个明亮的弧头 + 渐变拖尾，颜色从弧头色过渡到拖尾色。
 * =================================================================== */

/* 默认弧色：[0]=弧头色（亮蓝）, [1]=拖尾色（紫） */
#define DU_SPINNER_ARCS { \
    DP_SEAL_300, DP_SEAL_700, DP_SEAL_500, DP_SEAL_100 \
}

/* ===================================================================
 *  主题切换辅助 (Theme Switch)
 * =================================================================== */

typedef enum {
    DU_THEME_DARK,
    DU_THEME_LIGHT
} du_theme;

/* 根据主题获取对应的背景色 */
static inline du_u32 du_theme_bg(du_theme t) {
    return (t == DU_THEME_DARK) ? DS_DARK_BG_PRIMARY : DS_LIGHT_BG_PRIMARY;
}

/* 根据主题获取对应的文字色 */
static inline du_u32 du_theme_text(du_theme t) {
    return (t == DU_THEME_DARK) ? DS_DARK_TEXT_PRIMARY : DS_LIGHT_TEXT_PRIMARY;
}

/* 根据主题获取对应的强调色 */
static inline du_u32 du_theme_accent(du_theme t) {
    return (t == DU_THEME_DARK) ? DS_DARK_ACCENT : DS_LIGHT_ACCENT;
}

/* ===================================================================
 *  L0+ — Winux-Kate 调色板 (eDEX-UI Neon Cyber Theme)
 *
 *  源自 D:\Code\Winux-Kate\src\styles\theme.css
 *  风格：深蓝黑底 + 青色霓虹 + 绿色辅助 + 扫描线纹理
 * =================================================================== */

/* --- 主色调：深空蓝黑 (Deep Space Black) --- */
#define DP_KATE_BG          0xFF02040Au   /* --bg: #02040a */
#define DP_KATE_BG2         0xFF050A18u   /* --bg2: #050a18 */
#define DP_KATE_PANEL       0xB20A1228u   /* --panel: rgba(10,18,40,0.72) 半透明 */
#define DP_KATE_PANEL_SOLID 0xFF0A1228u   /* 不透明版本（用于无 alpha 混合场景） */

/* --- 强调色：霓虹青 (Neon Cyan) --- */
#define DP_KATE_ACCENT      0xFF00E5FFu   /* --accent: #00e5ff */
#define DP_KATE_ACCENT_DIM  0x2E00E5FFu   /* --accent-dim: rgba(0,229,255,0.18) */
#define DP_KATE_ACCENT_DIM_SOLID 0xFF1A3A4Au  /* 不透明近似 */
#define DP_KATE_PANEL_BORDER 0x5900E5FFu  /* --panel-border: rgba(0,229,255,0.35) */
#define DP_KATE_PANEL_BORDER_SOLID 0xFF005A6Au  /* 不透明近似 */

/* --- 辅助色：霓虹绿 (Neon Green) --- */
#define DP_KATE_ACCENT2     0xFF39FF14u   /* --accent2: #39ff14 */

/* --- 文本色 --- */
#define DP_KATE_TEXT        0xFFCFEFFBu   /* --text: #cfeffb */
#define DP_KATE_TEXT_DIM    0xFF6F8AA8u   /* --text-dim: #6f8aa8 */

/* --- 状态色 --- */
#define DP_KATE_WARN        0xFFFFCC00u   /* --warn: #ffcc00 */
#define DP_KATE_DANGER      0xFFFF4D6Du   /* --danger: #ff4d6d */

/* ===================================================================
 *  L1 — Winux-Kate 语义色值
 * =================================================================== */

#define KS_BG_PRIMARY       DP_KATE_BG
#define KS_BG_SECONDARY     DP_KATE_PANEL_SOLID
#define KS_BG_TERTIARY      DP_KATE_BG2

#define KS_TEXT_PRIMARY     DP_KATE_TEXT
#define KS_TEXT_DIM         DP_KATE_TEXT_DIM
#define KS_TEXT_INVERT      DP_KATE_BG

#define KS_ACCENT           DP_KATE_ACCENT
#define KS_ACCENT2          DP_KATE_ACCENT2
#define KS_ACCENT_DIM       DP_KATE_ACCENT_DIM_SOLID

#define KS_BORDER           DP_KATE_PANEL_BORDER_SOLID
#define KS_BORDER_FOCUS     DP_KATE_ACCENT
#define KS_BORDER_DIM       0xFF1A2840u   /* 暗边框 */

#define KS_DANGER           DP_KATE_DANGER
#define KS_WARN             DP_KATE_WARN

/* ===================================================================
 *  Winux-Kate 布局常量（源自 theme.css / pages.css）
 * =================================================================== */

#define KATE_TOPBAR_H       34     /* .topbar height */
#define KATE_STATUSBAR_H    32     /* .statusbar height */
#define KATE_TASKBAR_H      40     /* .taskbar height */
#define KATE_PANEL_HEADER_H 24     /* .panel-header */
#define KATE_TITLEBAR_H     28     /* 窗口标题栏 */
#define KATE_ICON_W         104    /* .desktop-icon width */
#define KATE_ICON_H         90     /* .desktop-icon height (含 label) */
#define KATE_ICON_IMG       40     /* .di-img / .di-fallback */
#define KATE_ICON_GAP       18     /* .desktop-grid gap */
#define KATE_ICON_PAD       24     /* .desktop-canvas padding */
#define KATE_TB_ITEM_MAX_W  200    /* .tb-item max-width */
#define KATE_PAGE_PAD       8      /* 页面 padding */

/* ===================================================================
 *  Winux-Kate 特效函数
 * =================================================================== */

/* 绘制扫描线纹理（ repeating-linear-gradient 0deg, 3px 周期）
 * 在指定矩形区域内叠加半透明青色水平细线 */
static inline void du_kate_scanlines(du_context *ctx, du_i64 x, du_i64 y,
                                     du_i64 w, du_i64 h) {
    for (du_i64 ry = 0; ry < h; ry++) {
        du_i64 yy = y + ry;
        if (yy < 0 || (du_u64)yy >= ctx->height) continue;  /* 每 3 像素一条线 */
        du_u32 *line = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)yy * ctx->pitch);
        for (du_i64 rx = 0; rx < w; rx++) {
            du_i64 xx = x + rx;
            if (xx < 0 || (du_u64)xx >= ctx->width) continue;
            /* 叠加极淡青色（alpha ~0.03 ≈ 8/255）*/
            du_u32 cur = line[(du_u64)xx];
            du_u32 b = (cur >> 16) & 0xFF;
            du_u32 g = (cur >> 8) & 0xFF;
            du_u32 r = cur & 0xFF;
            b = (b + 2 < 255) ? b + 2 : 255;
            g = (g + 4 < 255) ? g + 4 : 255;
            r = (r + 2 < 255) ? r + 2 : 255;
            line[(du_u64)xx] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

/* 绘制带光晕的矩形边框（box-shadow 效果）
 * 外发光：在矩形外围绘制渐变青色光晕 */
static inline void du_kate_glow_border(du_context *ctx, du_i64 x, du_i64 y,
                                       du_i64 w, du_i64 h, du_u32 glow_color) {
    /* 2px 渐变光晕，alpha 混合到实际背景（逐像素读-混合-写） */
    for (du_i64 i = 2; i >= 1; i--) {
        du_u32 alpha = (i == 2) ? 50 : 100;
        /* 上下边 */
        for (du_i64 c = -i; c < w + i; c++) {
            du_i64 xt = x + c;
            du_pixel(ctx, xt, y - i,
                     du_blend(du_pixel_read(ctx, xt, y - i), glow_color, alpha));
            du_pixel(ctx, xt, y + h + i - 1,
                     du_blend(du_pixel_read(ctx, xt, y + h + i - 1), glow_color, alpha));
        }
        /* 左右边 */
        for (du_i64 r = -i; r < h + i; r++) {
            du_i64 yt = y + r;
            du_pixel(ctx, x - i, yt,
                     du_blend(du_pixel_read(ctx, x - i, yt), glow_color, alpha));
            du_pixel(ctx, x + w + i - 1, yt,
                     du_blend(du_pixel_read(ctx, x + w + i - 1, yt), glow_color, alpha));
        }
    }
}

/* 绘制 Winux-Kate 面板（.panel 样式）
 * - 半透明深蓝背景
 * - 青色边框
 * - 内外光晕
 * - 扫描线纹理
 * - 可选标题栏 */
static inline void du_kate_panel(du_context *ctx, du_i64 x, du_i64 y,
                                 du_i64 w, du_i64 h, const char *header) {
    /* 1. 背景 */
    du_fill_rounded_rect(ctx, x, y, w, h, KS_BG_SECONDARY, 2);
    /* 2. 边框 */
    du_rect_outline(ctx, x, y, w, h, KS_BORDER, 2);
    /* 3. 外光晕 */
    du_kate_glow_border(ctx, x, y, w, h, KS_ACCENT);
    /* 4. 扫描线纹理 */
    du_kate_scanlines(ctx, x + 1, y + 1, w - 2, h - 2);
    /* 5. 标题栏 */
    if (header) {
        du_fill_rect(ctx, x, y, w, KATE_PANEL_HEADER_H, 0xFF062838u);  /* 深青色标题栏 */
        du_divider_h(ctx, x, y + KATE_PANEL_HEADER_H, w, KS_BORDER);
        /* 标题文字：青色，大写，letter-spacing */
        du_draw_string(ctx, header, x + 10, y + (KATE_PANEL_HEADER_H - DU_ASCII_LINE_H) / 2 + 2,
                       KS_ACCENT, 0xFF062838u, DU_ASCII_STEP);
        /* 标题前的小绿点 */
        du_fill_rect(ctx, x + 4, y + KATE_PANEL_HEADER_H / 2 - 3, 6, 6, KS_ACCENT2);
    }
}

/* 绘制 Winux-Kate 顶部栏（.topbar 样式）
 * - 34px 高
 * - 渐变背景
 * - 底部边框
 * - 品牌名 + 时钟槽位 */
static inline void du_kate_topbar(du_context *ctx, du_i64 w,
                                  const char *brand, const char *clock) {
    /* 渐变背景：左青色微亮 → 右深色 */
    du_fill_rect_gradient(ctx, 0, 0, w, KATE_TOPBAR_H,
                          0xFF0A3050u, 0xFF02040Au);
    /* 底部分隔线 */
    du_divider_h(ctx, 0, KATE_TOPBAR_H, w, KS_BORDER);
    /* 品牌名（青色，加粗，letter-spacing 4px）*/
    if (brand) {
        du_draw_string(ctx, brand, 12, (KATE_TOPBAR_H - DU_ASCII_LINE_H) / 2 + 1,
                       KS_ACCENT, 0, DU_ASCII_STEP);
    }
    /* 时钟（右侧，绿色）*/
    if (clock) {
        du_i64 cl = 0;
        while (clock[cl]) cl++;
        du_draw_string(ctx, clock, w - 12 - cl * DU_ASCII_STEP,
                       (KATE_TOPBAR_H - DU_ASCII_LINE_H) / 2 + 1,
                       KS_ACCENT2, 0, DU_ASCII_STEP);
    }
}

/* 绘制 Winux-Kate 状态栏（.statusbar 样式）
 * - 32px 高
 * - 顶部边框
 * - 半透明青色背景 */
static inline void du_kate_statusbar(du_context *ctx, du_i64 x, du_i64 y,
                                     du_i64 w, const char *status_text) {
    du_fill_rect(ctx, x, y, w, KATE_STATUSBAR_H, 0xFF051828u);
    du_divider_h(ctx, x, y, w, KS_BORDER);
    if (status_text) {
        du_draw_string(ctx, status_text, x + 12,
                       y + (KATE_STATUSBAR_H - DU_ASCII_LINE_H) / 2 + 1,
                       KS_TEXT_DIM, 0, DU_ASCII_STEP);
    }
}

/* 绘制 Winux-Kate 桌面图标（.desktop-icon 样式）
 * - 104px 宽
 * - 40x40 图标区域
 * - 下方文字标签
 * - 选中态：青色边框 + 光晕 */
static inline void du_kate_desktop_icon(du_context *ctx, du_i64 x, du_i64 y,
                                        const char *label, int selected) {
    /* 悬停/选中背景 */
    if (selected) {
        du_fill_rect(ctx, x, y, KATE_ICON_W, KATE_ICON_H, KS_ACCENT_DIM);
        du_rect_outline(ctx, x, y, KATE_ICON_W, KATE_ICON_H, KS_ACCENT, 1);
        du_kate_glow_border(ctx, x, y, KATE_ICON_W, KATE_ICON_H, KS_ACCENT);
    }
    /* 图标占位区域（40x40 居中）*/
    du_i64 ix = x + (KATE_ICON_W - KATE_ICON_IMG) / 2;
    du_i64 iy = y + 10;
    du_rect_outline(ctx, ix, iy, KATE_ICON_IMG, KATE_ICON_IMG, KS_BORDER, 1);
    /* 标签 */
    if (label) {
        du_i64 ll = 0;
        while (label[ll]) ll++;
        du_i64 lx = x + (KATE_ICON_W - ll * DU_ASCII_STEP) / 2;
        if (lx < x + 2) lx = x + 2;
        du_draw_string(ctx, label, lx, y + KATE_ICON_IMG + 18,
                       KS_TEXT_PRIMARY, 0, DU_ASCII_STEP);
    }
}

/* 绘制 Winux-Kate 任务栏项（.tb-item 样式） */
static inline void du_kate_tb_item(du_context *ctx, du_i64 x, du_i64 y,
                                   const char *title, int active) {
    du_i64 tl = 0;
    while (title[tl]) tl++;
    du_i64 w = tl * DU_ASCII_STEP + 24;
    if (w > KATE_TB_ITEM_MAX_W) w = KATE_TB_ITEM_MAX_W;

    du_u32 fg = active ? KS_ACCENT : KS_TEXT_PRIMARY;
    if (active) {
        du_fill_rect(ctx, x, y, w, 28, KS_ACCENT_DIM);
    }
    du_rect_outline(ctx, x, y, w, 28, KS_BORDER, 1);
    if (active) {
        du_kate_glow_border(ctx, x, y, w, 28, KS_ACCENT);
    }
    du_draw_string(ctx, title, x + 8, y + (28 - DU_ASCII_LINE_H) / 2 + 1,
                   fg, 0, DU_ASCII_STEP);
}

/* 绘制 Winux-Kate 按钮 (.btn 样式) */
static inline void du_kate_button(du_context *ctx, du_i64 x, du_i64 y,
                                  du_i64 w, du_i64 h, const char *label,
                                  int hover) {
    du_u32 fg = KS_ACCENT;
    if (hover) {
        du_fill_rect(ctx, x, y, w, h, KS_ACCENT_DIM);
    }
    du_rect_outline(ctx, x, y, w, h, KS_BORDER, 1);
    if (hover) {
        du_kate_glow_border(ctx, x, y, w, h, KS_ACCENT);
    }
    du_i64 ll = 0;
    while (label[ll]) ll++;
    du_i64 lx = x + (w - ll * DU_ASCII_STEP) / 2;
    du_draw_string(ctx, label, lx, y + (h - DU_ASCII_LINE_H) / 2 + 1,
                   fg, hover ? KS_ACCENT_DIM : KS_BG_SECONDARY, DU_ASCII_STEP);
}

#endif /* DESHAB_UI_H */
