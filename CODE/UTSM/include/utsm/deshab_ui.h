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

/* 弧线动画参数（DSK spinner 专用） */
#define DU_SPINNER_RADIUS     48u
#define DU_SPINNER_THICKNESS   4u
#define DU_SPINNER_ARC_GAP    82u      /* 每弧宽度（度） */
#define DU_SPINNER_ARC_COUNT   4u
#define DU_SPINNER_SPEED       2u      /* 每帧旋转度数 */
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
 *  使用 "Sealed Arc" 四弧旋转设计，弧色取自色板。
 * =================================================================== */

/* 默认弧色（从亮到暗的青蓝色） */
#define DU_SPINNER_ARCS { \
    DP_SEAL_100, DP_SEAL_300, DP_SEAL_500, DP_SEAL_700 \
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

#endif /* DESHAB_UI_H */
