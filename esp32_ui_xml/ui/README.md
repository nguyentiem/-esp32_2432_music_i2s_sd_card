# ESP32-2432 UI (320x240 ngang) - LVGL XML

Cấu trúc:
- globals.xml            màu, style dùng chung
- screens/idle_clock.xml   màn chờ: giờ, dương/âm, thời tiết
- screens/idle_dashboard.xml  màn chờ: CPU/RAM dashboard
- screens/menu.xml         menu lưới (Cài đặt, Nhạc)
- screens/settings.xml     Wi-Fi, Ngôn ngữ, Màn hình chờ
- screens/music.xml        phát nhạc

Yêu cầu: LVGL 9.4+ có bật XML loader (LV_USE_XML=1).
Mỗi widget quan trọng có name="..." để tìm bằng lv_obj_find_by_name() và cập nhật từ C.
Tiếng Việt có dấu cần font tự convert (lv_font_conv) chứa U+00C0-1EF9; thay các lv_font_montserrat_* tương ứng.
Icon dùng LV_SYMBOL (FontAwesome built-in) qua mã &#xF...; trong text.
Các tên thuộc tính style/chart/arc có thể khác nhẹ tuỳ phiên bản LVGL: nếu parser báo lỗi, đối chiếu docs XML của đúng phiên bản.
