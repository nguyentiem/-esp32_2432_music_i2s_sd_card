# Music player cho ESP32-2432S028R (CYD)

Firmware PlatformIO/Arduino dành cho **ESP32-WROOM-32, màn ILI9341 2.8 inch, cảm ứng điện trở XPT2046**, flash 4 MB, không cần PSRAM. Nếu board là S3, màn ST7789 hoặc bản cảm ứng điện dung thì cần đổi cấu hình/driver; firmware này chưa hỗ trợ các biến thể đó.

Thiết kế ban đầu được giữ tại [docs/design-original.md](docs/design-original.md), cùng [sơ đồ kiến trúc](esp32_music_player_architecture.svg).

## Chức năng

- Phát MP3 và WAV PCM 8/16-bit mono/stereo (sample rate tối đa 48 kHz) từ `/music` trên SD, tự chuyển bài khi hết và lặp danh sách.
- Phát URL HTTP trực tiếp qua WiFi; file `.wav` được nhận dạng theo đuôi URL, các URL khác dùng decoder MP3.
- Producer trên core 0 đọc SD/mạng; decoder và I2S DMA trên core 1. Buffer SRAM 24 KB, prebuffer 12 KB; khi thiếu dữ liệu, chờ nạp lại và gửi mẫu im lặng, không tăng đồng hồ bài hát.
- LRC UTF-8: nhiều timestamp trên một dòng, phần giây thập phân, `[offset:...]`, sắp xếp thời gian và tìm dòng bằng binary search. Offset dương hiển thị lời sớm hơn.
- Hiển thị tiếng Việt Unicode NFC, tên bài, trạng thái, thời gian, âm lượng và tiến trình. WAV SD tính thời lượng từ header; MP3 SD **ước lượng CBR**, không chính xác với VBR. HTTP chưa có tổng thời lượng.
- Cảm ứng: bài trước, phát/tạm dừng, bài sau, giảm/tăng âm lượng. Serial có cùng chức năng.
- HTTP thử kết nối tối đa 4 lần, chờ 0.5/1/2 giây giữa các lần thử; tắt WiFi power save sau khi kết nối.

## Phần cứng

| Chức năng | GPIO |
|---|---|
| TFT SCLK / MOSI / MISO | 14 / 13 / 12 |
| TFT CS / DC / đèn nền | 15 / 2 / 21 |
| SD SCLK / MOSI / MISO / CS | 18 / 23 / 19 / 5 |
| Touch CLK / DIN / DOUT / CS / IRQ | 25 / 32 / 39 / 33 / 36 |
| DAC tích hợp → ampli loa trên board | 26 |

Dùng cổng loa `SPEAK` trên board theo thông số loa của nhà sản xuất. Không cần MAX98357A cho cấu hình này. TFT dùng HSPI, SD dùng VSPI, cảm ứng dùng software SPI để tránh chiếm bus. DAC chỉ bật kênh GPIO26 vì GPIO25 là clock cảm ứng.

Tham khảo pin: [CYD community board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md). Audio dùng [ESP8266Audio 1.9.7](https://github.com/earlephilhower/ESP8266Audio/tree/1.9.7). Phiên bản Arduino 2.0.17/ESP-IDF 4.4 được cố định để dùng DAC I2S của ESP32 thường; đây là điều chỉnh so với gợi ý ESP-IDF 5/S3 trong thiết kế.

## Build và nạp

Cài PlatformIO Core hoặc extension PlatformIO cho VS Code, mở thư mục này:

```sh
pio run
pio device list
pio run -t upload --upload-port /dev/ttyUSB0
pio device monitor --port /dev/ttyUSB0 --baud 115200
```

Thay cổng bằng cổng thật. Nếu board không tự vào bootloader, giữ BOOT lúc bắt đầu upload. Firmware nằm ở `.pio/build/cyd/firmware.bin`; dùng `pio run -t upload` để nạp đủ bootloader, partition và ứng dụng. Partition `huge_app` không có OTA.

## Chuẩn bị SD

Format FAT32, chép **nội dung** thư mục `sdcard` vào gốc thẻ:

```text
/music/demo.wav
/music/demo.lrc
/music/ten-bai.mp3
/music/ten-bai.lrc
/streams.txt                 # tùy chọn
```

Tạo bài demo 8 giây (âm lượng nhỏ, không chứa nhạc có bản quyền):

```sh
python3 tools/make_demo.py
```

Danh sách SD quét một lần lúc boot, tối đa 128 bài/URL, không quét thư mục con. Khởi động lại sau khi thay thẻ hoặc đổi danh sách. LRC đầu vào tối đa 32 KB/600 timestamp, tổng text lưu tối đa 16 KB, mỗi dòng tối đa 191 byte; dùng UTF-8 NFC. Lời dài được giới hạn vùng ba dòng trên màn hình.

## WiFi / HTTP

```sh
cp include/secrets.example.h include/secrets.h
```

Sửa SSID/mật khẩu trong `secrets.h` (đã bỏ qua bởi Git). Có thể thêm `#define STREAM_URL "http://192.168.1.10:8000/music/demo.wav"` để dùng một URL ngay cả khi không có SD.

Hoặc tạo `/streams.txt` trên SD, mỗi dòng là một URL `http://...`; dòng khác bị bỏ qua. Dùng server LAN để thử:

```sh
python3 -m http.server 8000 --directory sdcard
```

Điền IP LAN của máy chạy server, không dùng `localhost`. LRC cho URL `/music/demo.wav` được đọc từ SD `/music/demo.lrc` nếu có. URL phải trả trực tiếp audio HTTP 200; chưa hỗ trợ HTTPS, redirect, chunked transfer, HLS, playlist M3U hoặc metadata ICY. HTTP/1.0 được yêu cầu để tránh chunked transfer.

Nếu mất kết nối giữa bài, nguồn đọc báo lỗi và dừng; nhấn PLAY để tải lại từ đầu. Chưa có HTTP Range resume. Lỗi mở/giải mã không tự nhảy liên tục sang bài khác. Buffer này phù hợp thử MP3 128 kbps (khoảng 1.5 giây dữ liệu); WAV cần băng thông cao hơn và không có mức chống jitter 3–5 giây như cấu hình S3/PSRAM trong thiết kế.

## Điều khiển và kiểm tra

Serial 115200: `p` phát/tạm dừng, `n` bài sau, `b` bài trước, `-`/`+` âm lượng. Khi đang chờ nguồn mạng, xử lý phím có thể trễ đến khi timeout/retry kết thúc; UI chạy độc lập.

Nếu tọa độ cảm ứng lệch, sửa các giới hạn `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX` trong `include/config.h`; có thể đảo MIN/MAX cho trục bị ngược. Màn dùng rotation 1. Màn trắng/sai màu cần kiểm tra đúng biến thể ILI9341 trước khi đổi driver.

Kiểm thử parser trên máy tính:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude test/lyrics_test.cpp -o /tmp/music-lyrics-test
/tmp/music-lyrics-test
```

Kiểm tra trên board: chạy demo; xác nhận 8 giây và 4 dòng tiếng Việt; thử pause/resume, đổi bài, âm lượng và cả 5 vùng cảm ứng. Sau đó thử MP3, HTTP và ngắt WiFi giữa bài. Đồng hồ đếm mẫu được I2S nhận nên có thể đi trước âm thực tế một khoảng DMA ngắn; không phải timestamp đo trực tiếp ở loa.

Build và test máy tính không thay thế kiểm chứng phần cứng. Chưa kiểm chứng loa, hướng cảm ứng hoặc độ ổn định mạng trên board thật trong môi trường phát triển hiện tại.
