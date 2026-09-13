/**
 * music_player_ui.c
 * -----------------------------------------------------------------------
 * LVGL v8.x UI cho ESP32 + TFT 240x320 (dọc / portrait)
 * Bố cục: Song info -> Progress bar -> Lyric (realtime, dòng hiện tại nổi bật)
 *         -> Controls (Prev/Play-Pause/Next) -> Volume slider
 *
 * Cách dùng:
 *   1. Gọi player_ui_create(lv_scr_act()) một lần khi khởi tạo màn hình.
 *   2. Trong task xử lý nhạc/BLE/lyric của bạn, gọi các hàm player_ui_set_xxx()
 *      để cập nhật dữ liệu (nhớ gọi trong LVGL task context, hoặc dùng
 *      lv_async_call nếu update từ task khác).
 *   3. Gán callback cho các nút bằng player_ui_set_callbacks().
 * -----------------------------------------------------------------------
 */

#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ------------------------- CONFIG / THEME ------------------------------ */

#define UI_BG_COLOR         lv_color_hex(0x0B0F1A)   /* nền navy rất tối */
#define UI_CARD_COLOR       lv_color_hex(0x131A2B)
#define UI_ACCENT_COLOR     lv_color_hex(0x3ED6A5)   /* xanh mint làm điểm nhấn */
#define UI_TEXT_PRIMARY     lv_color_hex(0xF5F6FA)
#define UI_TEXT_SECONDARY   lv_color_hex(0x8A93A6)
#define UI_LYRIC_ACTIVE     lv_color_hex(0xFFFFFF)
#define UI_LYRIC_INACTIVE   lv_color_hex(0x4B5468)

/* Lop phu toi mo len anh nen tinh de chu/luong con doc duoc.
 * Tang gia tri (0-255) neu anh sang, giam neu anh da toi san. */
#define UI_BG_OVERLAY_OPA    LV_OPA_60

#define LYRIC_VISIBLE_LINES 3   /* số dòng lyric hiển thị cùng lúc (lẻ, dòng giữa là active) */

/* ------------------------- INTERNAL STATE ------------------------------- */

typedef struct {
    lv_obj_t *scr;

    lv_obj_t *img_bg;      /* anh nen tinh, toan man hinh */
    lv_obj_t *bg_overlay;  /* lop phu toi de tang tuong phan chu */

    lv_obj_t *label_title;
    lv_obj_t *label_artist;

    lv_obj_t *bar_progress;
    lv_obj_t *label_time_cur;
    lv_obj_t *label_time_total;

    lv_obj_t *lyric_cont;
    lv_obj_t *lyric_labels[LYRIC_VISIBLE_LINES];
    lv_obj_t *lyric_blur_canvas;   /* anh nen da lam mo, dat sau lung chu lyric */
    lv_obj_t *lyric_tint;          /* lop kinh mo (frosted) phu nhe len tren blur */
    void     *lyric_blur_buf;      /* buffer pixel cho canvas, cap phat dong */

    lv_obj_t *btn_prev;
    lv_obj_t *btn_play;
    lv_obj_t *label_play_icon;
    lv_obj_t *btn_next;

    lv_obj_t *slider_volume;
    lv_obj_t *label_vol_icon;

    bool is_playing;
} player_ui_t;

static player_ui_t ui;

/* callbacks do người dùng đăng ký */
typedef struct {
    void (*on_prev)(void);
    void (*on_play_pause)(bool now_playing);
    void (*on_next)(void);
    void (*on_volume_change)(int percent);
} player_ui_callbacks_t;

static player_ui_callbacks_t cb = {0};

/* con tro toi mo ta anh nen hien tai, de blur engine doc pixel goc */
static const lv_img_dsc_t *s_bg_img_dsc = NULL;

/* -------------------- BLUR ENGINE (chi chay 1 lan khi doi anh) ----------
 * Gia dinh anh nen la RGB565 (LV_IMG_CF_TRUE_COLOR, LV_COLOR_DEPTH 16).
 * Neu project dung color depth khac, can doi lai unpack/pack tuong ung.
 * ------------------------------------------------------------------------- */

static inline void unpack565(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (px >> 11) & 0x1F;
    *g = (px >> 5) & 0x3F;
    *b = px & 0x1F;
}

static inline uint16_t pack565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
}

/* Box blur tach truc (ngang roi doc) tren vung crop cua anh nen goc,
 * ket qua ghi thang vao buffer canvas dst (kich thuoc dst_w x dst_h). */
static void blur_crop_from_bg(uint16_t *dst, int dst_w, int dst_h,
                               int crop_x, int crop_y, int radius)
{
    if (!s_bg_img_dsc || !dst) return;

    const uint16_t *src = (const uint16_t *)s_bg_img_dsc->data;
    int src_w = s_bg_img_dsc->header.w;
    int src_h = s_bg_img_dsc->header.h;

    uint16_t *tmp = lv_mem_alloc(dst_w * dst_h * sizeof(uint16_t));
    if (!tmp) return;

    /* pass 1: blur ngang, doc truc tiep tu anh nen goc */
    for (int y = 0; y < dst_h; y++) {
        int sy = crop_y + y;
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;

        for (int x = 0; x < dst_w; x++) {
            uint32_t r = 0, g = 0, b = 0;
            int n = 0;
            for (int k = -radius; k <= radius; k++) {
                int sx = crop_x + x + k;
                if (sx < 0) sx = 0;
                if (sx >= src_w) sx = src_w - 1;
                uint8_t pr, pg, pb;
                unpack565(src[sy * src_w + sx], &pr, &pg, &pb);
                r += pr; g += pg; b += pb; n++;
            }
            tmp[y * dst_w + x] = pack565(r / n, g / n, b / n);
        }
    }

    /* pass 2: blur doc, doc tu ket qua pass 1 */
    for (int x = 0; x < dst_w; x++) {
        for (int y = 0; y < dst_h; y++) {
            uint32_t r = 0, g = 0, b = 0;
            int n = 0;
            for (int k = -radius; k <= radius; k++) {
                int sy = y + k;
                if (sy < 0) sy = 0;
                if (sy >= dst_h) sy = dst_h - 1;
                uint8_t pr, pg, pb;
                unpack565(tmp[sy * dst_w + x], &pr, &pg, &pb);
                r += pr; g += pg; b += pb; n++;
            }
            dst[y * dst_w + x] = pack565(r / n, g / n, b / n);
        }
    }

    lv_mem_free(tmp);
}

#define LYRIC_BLUR_RADIUS 6   /* tang de mo hon (ton them chut CPU 1 lan), giam de net hon */

/* Tinh lai va ve lai blur cho dung vi tri/kich thuoc hien tai cua lyric_cont */
static void player_ui_refresh_lyric_blur(void)
{
    if (!s_bg_img_dsc) {
        lv_obj_add_flag(ui.lyric_blur_canvas, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_update_layout(ui.lyric_cont);

    lv_area_t area;
    lv_obj_get_content_coords(ui.lyric_cont, &area); /* toa do tuyet doi tren man hinh */

    int w = lv_area_get_width(&area);
    int h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0) return;

    if (ui.lyric_blur_buf) {
        lv_mem_free(ui.lyric_blur_buf);
        ui.lyric_blur_buf = NULL;
    }
    ui.lyric_blur_buf = lv_mem_alloc((size_t)w * h * sizeof(uint16_t));
    if (!ui.lyric_blur_buf) return;

    blur_crop_from_bg((uint16_t *)ui.lyric_blur_buf, w, h, area.x1, area.y1, LYRIC_BLUR_RADIUS);

    lv_canvas_set_buffer(ui.lyric_blur_canvas, ui.lyric_blur_buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(ui.lyric_blur_canvas, w, h);
    lv_obj_clear_flag(ui.lyric_blur_canvas, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------- EVENT HANDLERS -------------------------------- */

static void play_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ui.is_playing = !ui.is_playing;
    lv_label_set_text(ui.label_play_icon, ui.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (cb.on_play_pause) cb.on_play_pause(ui.is_playing);
}

static void prev_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (cb.on_prev) cb.on_prev();
}

static void next_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (cb.on_next) cb.on_next();
}

static void volume_slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int val = lv_slider_get_value(ui.slider_volume);

    if (val <= 0) lv_label_set_text(ui.label_vol_icon, LV_SYMBOL_MUTE);
    else if (val < 50) lv_label_set_text(ui.label_vol_icon, LV_SYMBOL_VOLUME_MID);
    else lv_label_set_text(ui.label_vol_icon, LV_SYMBOL_VOLUME_MAX);

    if (cb.on_volume_change) cb.on_volume_change(val);
}

/* ------------------------- BUILD HELPERS --------------------------------- */

static lv_obj_t *make_ctrl_button(lv_obj_t *parent, const char *symbol, lv_coord_t size, bool accent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, accent ? UI_ACCENT_COLOR : UI_CARD_COLOR, 0);
    lv_obj_set_style_shadow_width(btn, accent ? 12 : 0, 0);
    lv_obj_set_style_shadow_color(btn, UI_ACCENT_COLOR, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(btn, 0, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, accent ? lv_color_hex(0x0B0F1A) : UI_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);

    return btn;
}

/* ------------------------- PUBLIC: CREATE -------------------------------- */

lv_obj_t *player_ui_create(lv_obj_t *parent)
{
    memset(&ui, 0, sizeof(ui));
    ui.is_playing = false;

    ui.scr = parent;
    lv_obj_set_style_bg_color(ui.scr, UI_BG_COLOR, 0);  /* mau nen fallback khi chua co anh */
    lv_obj_set_style_pad_all(ui.scr, 0, 0);

    /* -------- Anh nen tinh (an cho den khi player_ui_set_background_image() duoc goi) -------- */
    ui.img_bg = lv_img_create(parent);
    lv_obj_set_size(ui.img_bg, 240, 320);
    lv_obj_align(ui.img_bg, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(ui.img_bg, LV_OBJ_FLAG_HIDDEN);

    /* -------- Lop phu toi ban trong suot, chi lo khi co anh nen -------- */
    ui.bg_overlay = lv_obj_create(parent);
    lv_obj_set_size(ui.bg_overlay, 240, 320);
    lv_obj_align(ui.bg_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(ui.bg_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui.bg_overlay, UI_BG_OVERLAY_OPA, 0);
    lv_obj_set_style_border_width(ui.bg_overlay, 0, 0);
    lv_obj_set_style_radius(ui.bg_overlay, 0, 0);
    lv_obj_clear_flag(ui.bg_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui.bg_overlay, LV_OBJ_FLAG_HIDDEN);

    /* -------- Container tổng, flex column, chiếm toàn bộ 240x320 -------- */
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, 240, 320);
    lv_obj_center(root);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* -------- 1. Song title + artist -------- */
    ui.label_title = lv_label_create(root);
    lv_obj_set_width(ui.label_title, 220);
    lv_label_set_long_mode(ui.label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(ui.label_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui.label_title, UI_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_align(ui.label_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ui.label_title, "Ten bai hat");

    ui.label_artist = lv_label_create(root);
    lv_obj_set_width(ui.label_artist, 220);
    lv_obj_set_style_text_font(ui.label_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui.label_artist, UI_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_align(ui.label_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(ui.label_artist, 8, 0);
    lv_label_set_text(ui.label_artist, "Ca si");

    /* -------- 2. Progress bar + time -------- */
    lv_obj_t *progress_row = lv_obj_create(root);
    lv_obj_set_size(progress_row, 220, 20);
    lv_obj_set_style_bg_opa(progress_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_row, 0, 0);
    lv_obj_set_style_pad_all(progress_row, 0, 0);
    lv_obj_clear_flag(progress_row, LV_OBJ_FLAG_SCROLLABLE);

    ui.bar_progress = lv_bar_create(progress_row);
    lv_obj_set_size(ui.bar_progress, 220, 4);
    lv_obj_align(ui.bar_progress, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(ui.bar_progress, UI_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.bar_progress, UI_ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.bar_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_bar_set_range(ui.bar_progress, 0, 100);
    lv_bar_set_value(ui.bar_progress, 0, LV_ANIM_OFF);

    ui.label_time_cur = lv_label_create(progress_row);
    lv_obj_align(ui.label_time_cur, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_text_font(ui.label_time_cur, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.label_time_cur, UI_TEXT_SECONDARY, 0);
    lv_label_set_text(ui.label_time_cur, "00:00");

    ui.label_time_total = lv_label_create(progress_row);
    lv_obj_align(ui.label_time_total, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_font(ui.label_time_total, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.label_time_total, UI_TEXT_SECONDARY, 0);
    lv_label_set_text(ui.label_time_total, "00:00");

    lv_obj_set_style_pad_top(progress_row, 10, 0);
    lv_obj_set_style_pad_bottom(progress_row, 10, 0);

    /* -------- 3. Lyric area (center focus) -------- */
    ui.lyric_cont = lv_obj_create(root);
    lv_obj_set_size(ui.lyric_cont, 220, 130);
    lv_obj_set_style_bg_color(ui.lyric_cont, UI_CARD_COLOR, 0);
    lv_obj_set_style_bg_opa(ui.lyric_cont, LV_OPA_40, 0);
    lv_obj_set_style_border_width(ui.lyric_cont, 0, 0);
    lv_obj_set_style_radius(ui.lyric_cont, 12, 0);
    lv_obj_set_style_clip_corner(ui.lyric_cont, true, 0);  /* bo goc noi dung theo bo goc container */
    lv_obj_set_style_pad_all(ui.lyric_cont, 6, 0);
    lv_obj_clear_flag(ui.lyric_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui.lyric_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui.lyric_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* -- Anh nen da blur, chi hien khi co background image -- */
    ui.lyric_blur_canvas = lv_canvas_create(ui.lyric_cont);
    lv_obj_set_pos(ui.lyric_blur_canvas, 0, 0);
    lv_obj_clear_flag(ui.lyric_blur_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui.lyric_blur_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui.lyric_blur_canvas, LV_OBJ_FLAG_HIDDEN);

    /* -- Lop kinh mo (frosted) de tang tuong phan chu, luon hien -- */
    ui.lyric_tint = lv_obj_create(ui.lyric_cont);
    lv_obj_set_pos(ui.lyric_tint, 0, 0);
    lv_obj_set_size(ui.lyric_tint, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui.lyric_tint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui.lyric_tint, LV_OPA_30, 0);
    lv_obj_set_style_border_width(ui.lyric_tint, 0, 0);
    lv_obj_set_style_radius(ui.lyric_tint, 0, 0);
    lv_obj_clear_flag(ui.lyric_tint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui.lyric_tint, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < LYRIC_VISIBLE_LINES; i++) {
        lv_obj_t *l = lv_label_create(ui.lyric_cont);
        lv_obj_set_width(l, 200);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

        bool is_active_row = (i == LYRIC_VISIBLE_LINES / 2);
        lv_obj_set_style_text_font(l, is_active_row ? &lv_font_montserrat_18 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, is_active_row ? UI_LYRIC_ACTIVE : UI_LYRIC_INACTIVE, 0);
        lv_label_set_text(l, "");
        ui.lyric_labels[i] = l;
    }

    /* -------- 4. Controls: Prev / Play-Pause / Next -------- */
    lv_obj_t *ctrl_row = lv_obj_create(root);
    lv_obj_set_size(ctrl_row, 220, 64);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_style_pad_top(ctrl_row, 12, 0);
    lv_obj_clear_flag(ctrl_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui.btn_prev = make_ctrl_button(ctrl_row, LV_SYMBOL_PREV, 44, false);
    lv_obj_add_event_cb(ui.btn_prev, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);

    ui.btn_play = make_ctrl_button(ctrl_row, LV_SYMBOL_PLAY, 60, true);
    lv_obj_add_event_cb(ui.btn_play, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
    ui.label_play_icon = lv_obj_get_child(ui.btn_play, 0);

    ui.btn_next = make_ctrl_button(ctrl_row, LV_SYMBOL_NEXT, 44, false);
    lv_obj_add_event_cb(ui.btn_next, next_btn_event_cb, LV_EVENT_CLICKED, NULL);

    /* -------- 5. Volume row -------- */
    lv_obj_t *vol_row = lv_obj_create(root);
    lv_obj_set_size(vol_row, 220, 30);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_style_pad_top(vol_row, 8, 0);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_row, 8, 0);

    ui.label_vol_icon = lv_label_create(vol_row);
    lv_label_set_text(ui.label_vol_icon, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(ui.label_vol_icon, UI_TEXT_SECONDARY, 0);

    ui.slider_volume = lv_slider_create(vol_row);
    lv_obj_set_size(ui.slider_volume, 160, 6);
    lv_slider_set_range(ui.slider_volume, 0, 100);
    lv_slider_set_value(ui.slider_volume, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.slider_volume, UI_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.slider_volume, UI_ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.slider_volume, UI_ACCENT_COLOR, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui.slider_volume, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(ui.slider_volume, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    return root;
}

/* ------------------------- PUBLIC: SETTERS -------------------------------- */

void player_ui_set_callbacks(player_ui_callbacks_t callbacks)
{
    cb = callbacks;
}

void player_ui_set_song_info(const char *title, const char *artist)
{
    lv_label_set_text(ui.label_title, title);
    lv_label_set_text(ui.label_artist, artist);
}

/* progress: current_sec/total_sec tinh bang giay */
void player_ui_set_progress(uint32_t current_sec, uint32_t total_sec)
{
    int pct = (total_sec > 0) ? (int)((current_sec * 100) / total_sec) : 0;
    lv_bar_set_value(ui.bar_progress, pct, LV_ANIM_ON);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(current_sec / 60), (unsigned)(current_sec % 60));
    lv_label_set_text(ui.label_time_cur, buf);
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(total_sec / 60), (unsigned)(total_sec % 60));
    lv_label_set_text(ui.label_time_total, buf);
}

/* dong bo trang thai play/pause tu ben ngoai (vd sau khi auto-next) */
void player_ui_set_playing_state(bool playing)
{
    ui.is_playing = playing;
    lv_label_set_text(ui.label_play_icon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void player_ui_set_volume(int percent)
{
    lv_slider_set_value(ui.slider_volume, percent, LV_ANIM_ON);
    if (percent <= 0) lv_label_set_text(ui.label_vol_icon, LV_SYMBOL_MUTE);
    else if (percent < 50) lv_label_set_text(ui.label_vol_icon, LV_SYMBOL_VOLUME_MID);
    else lv_label_set_text(ui.label_vol_icon, LV_SYMBOL_VOLUME_MAX);
}

/*
 * Cap nhat lyric realtime.
 * lines: mang cac dong lyric xung quanh dong hien tai
 *        (lines[LYRIC_VISIBLE_LINES/2] la dong dang hat)
 * Truyen NULL cho dong khong ton tai (vd dau/cuoi bai).
 */
void player_ui_set_lyric_window(const char *lines[LYRIC_VISIBLE_LINES])
{
    for (int i = 0; i < LYRIC_VISIBLE_LINES; i++) {
        lv_label_set_text(ui.lyric_labels[i], lines[i] ? lines[i] : "");
    }
}

/*
 * Gan anh nen tinh cho man hinh (vd anh ban tu chup, background co dinh...).
 *
 * img_src co the la:
 *   - Con tro toi lv_img_dsc_t (anh da convert thanh mang C, nhung vao flash) -> truyen &my_bg_img
 *   - Duong dan string neu dung file system, vd "S:/bg.bin" (SD card qua lv_fs_drv)
 *     hoac "A:/spiffs/bg.bin" tuy driver ban dang gan cho LVGL.
 *
 * Anh nen ly tuong: da resize dung 240x320, dinh dang RGB565 (true color) de
 * do gio giai ma va tiet kiem RAM/flash. Convert bang cong cu online:
 *   https://lvgl.io/tools/imageconverter (Output format: C array, Color format: CF_TRUE_COLOR)
 *
 * Goi voi img_src = NULL de an anh, quay lai mau nen mac dinh (UI_BG_COLOR).
 */
void player_ui_set_background_image(const void *img_src)
{
    if (img_src == NULL) {
        s_bg_img_dsc = NULL;
        lv_obj_add_flag(ui.img_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.bg_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.lyric_blur_canvas, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    s_bg_img_dsc = (const lv_img_dsc_t *)img_src;
    lv_img_set_src(ui.img_bg, img_src);
    lv_obj_clear_flag(ui.img_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui.bg_overlay, LV_OBJ_FLAG_HIDDEN);

    player_ui_refresh_lyric_blur();
}

/* Chinh do toi cua lop phu (0-255). Anh sang -> tang gia tri; anh toi san -> giam. */
void player_ui_set_background_overlay_opa(lv_opa_t opa)
{
    lv_obj_set_style_bg_opa(ui.bg_overlay, opa, 0);
}
