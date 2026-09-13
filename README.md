# CYD Music — ESP-IDF thuần

Music player cho **ESP32-2432S028R (CYD), ESP32-WROOM-32, ST7789 320×240,
XPT2046, flash 4 MB**, không cần PSRAM. Dự án chính dùng **ESP-IDF 5.5.3**,
không phụ thuộc Arduino, PlatformIO hoặc ESP-ADF. Âm thanh xuất qua module **MAX98357A I2S** bên ngoài.

## Build và nạp

Trên máy phát triển hiện tại, ESP-IDF đã có tại:

```sh
. /home/nguyentiem/.espressif/v5.5.3/esp-idf/export.sh
idf.py build
idf.py menuconfig
idf.py -p /dev/ttyUSB0 flash monitor
```

Trên máy khác, cài [ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/get-started/index.html),
chạy `export.sh` của bản cài đó, rồi chạy các lệnh `idf.py` trong thư mục dự án.
Target mặc định là `esp32`. Thay `/dev/ttyUSB0` bằng cổng thực tế; thoát monitor bằng `Ctrl+]`.
Nếu board không tự vào bootloader, giữ BOOT khi bắt đầu nạp.

Firmware: `build/cyd_music.bin`. Dùng `idf.py flash` để nạp đủ bootloader,
partition table và ứng dụng. Partition ứng dụng 3 MB, không có OTA. Không cần xóa toàn bộ flash.

## Chức năng

- Phát MP3 bằng minimp3; WAV PCM 8/16-bit, mono/stereo, 8–48 kHz.
- Nguồn SD FAT hoặc URL HTTP trả audio trực tiếp. HTTP hỗ trợ Content-Length
  và chunked transfer qua `esp_http_client`; không hỗ trợ HTTPS, redirect,
  HLS, M3U hoặc metadata ICY.
- Task nguồn trên core 0, task decode/I2S trên core 1; stream buffer 24 KB,
  prebuffer 12 KB. WiFi kết nối nền, tắt power save.
- I2S0 standard Philips, PCM signed 16-bit, hai slot L/R ở 48 kHz, không cần MCLK.
  Trộn stereo thành mono rồi gửi cùng mẫu vào cả hai slot cho một MAX98357A.
  Âm lượng xử lý ở 16-bit; DMA tự xóa dữ liệu đã gửi khi underrun.
  Không chèn mẫu im lặng vào giữa các block nhạc khi xử lý UI/đọc nguồn.
  DMA có 12 buffer × 256 frame, tương đương 64 ms PCM và 12 KiB RAM;
  đây là dung lượng dự trữ tối đa, không phải bảo đảm chống mọi lần nghẽn.
  Sample rate khác dùng lặp mẫu theo tỷ lệ (zero-order hold), chưa có bộ lọc
  resample chất lượng cao. GPIO25 được giữ cho clock cảm ứng.
- LRC UTF-8 NFC tiếng Việt: nhiều timestamp, offset, tìm dòng bằng binary search.
- Màn hình: tên bài, trạng thái, ba dòng lời, thời gian, âm lượng, tiến trình;
  cảm ứng và Serial có bài trước/sau, phát/tạm dừng, tăng/giảm âm lượng.
- Hết bài tự chuyển và lặp danh sách. Bài lỗi dừng; nhấn PLAY để thử lại từ đầu.

## Pin phần cứng

| Chức năng | GPIO |
|---|---|
| TFT SCLK / MOSI / MISO | 14 / 13 / 12 |
| TFT CS / DC / backlight | 15 / 2 / 21 |
| SD SCLK / MOSI / MISO / CS | 18 / 23 / 19 / 5 |
| Touch CLK / DIN / DOUT / CS / IRQ | 25 / 32 / 39 / 33 / 36 |
| I2S BCLK / LRC (WS) / DOUT → MAX98357A DIN | 22 / 27 / 26 |

TFT dùng SPI2, SD dùng SPI3, cảm ứng dùng software SPI. Cấu hình này không
dành cho ESP32-S3, màn ST7789 hoặc cảm ứng điện dung.

### Nối MAX98357A

| MAX98357A | Kết nối mặc định |
|---|---|
| BCLK | ESP32 GPIO22 |
| LRC / LRCLK / WS | ESP32 GPIO27 |
| DIN | ESP32 GPIO26 (I2S DOUT) |
| VIN | Nguồn 5V phù hợp với module |
| GND | GND chung với ESP32 và nguồn |
| SPK+ / SPK− | Hai đầu loa 4Ω hoặc 8Ω |

Loa nối trực tiếp giữa SPK+ và SPK− của MAX98357A; không nối đầu loa xuống GND
hoặc cổng `SPEAK` của CYD. Không nối tín hiệu I2S vào cổng `SPEAK`.
Module phải ở trạng thái bật qua SD/MODE: với breakout Adafruit có pull-up sẵn,
để mặc định; với module khác kiểm tra sơ đồ/pull-up SD của module, không kéo SD xuống GND.
Firmware gửi hai slot giống nhau nên chọn left, right hoặc mix đều phát cùng nội dung.
Xem [pinout MAX98357A của Adafruit](https://learn.adafruit.com/adafruit-max98357-i2s-class-d-mono-amp/pinouts).

**Lưu ý bố trí CYD:** GPIO22/27 có ở CN1, nhưng GPIO26 nối vào ampli tích hợp,
không có ở đầu nối loa dưới dạng tín hiệu GPIO. Cần lấy GPIO26 ở phía ESP32,
trước ampli, và để cổng loa tích hợp không nối loa. Nếu không tiếp cận được chân
này, đổi DOUT sang GPIO4/16/17 sau khi giải phóng kết nối LED RGB tương ứng.
GPIO35 chỉ là input, không dùng cho I2S output. Tham khảo
[pinout CYD](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md).

Đổi chân tại `idf.py menuconfig` → **CYD music player** → **MAX98357A ... GPIO**,
rồi build/nạp lại. Firmware kiểm tra ba chân khác nhau và chỉ cho phép
GPIO4/16/17/22/26/27 để tránh xung đột với flash, LCD, SD, touch và UART.

## Thẻ SD và WiFi

Format thẻ FAT32, chép **nội dung** `sdcard/` vào gốc thẻ:

```text
/music/demo.wav
/music/demo.lrc
/music/ten-bai.mp3
/music/ten-bai.lrc
/streams.txt
```

Tạo WAV demo 8 giây:

```sh
python3 tools/make_demo.py
```

`streams.txt` tùy chọn, mỗi dòng một URL `http://...`, tối đa 1022 byte/URL.
Danh sách quét lúc boot, không quét thư mục con, tối đa 128 file/URL.
Khởi động lại sau khi đổi nội dung thẻ. Nếu không có thư mục `/music`, firmware
quét MP3/WAV ngay tại gốc thẻ; LRC đặt cạnh file nhạc cùng tên. Nếu `/music`
tồn tại thì chỉ quét thư mục đó. Log `sd` ghi các file thấy được và lý do bỏ qua.

WiFi và URL dự phòng: `idf.py menuconfig` → **CYD music player**.
Hoặc dùng cấu hình riêng (ghi đè menuconfig):

```sh
cp include/secrets.example.h include/secrets.h
```

Điền `WIFI_SSID`, `WIFI_PASSWORD`, tùy chọn `STREAM_URL`.
`secrets.h` và `sdkconfig` được bỏ qua bởi Git; thông tin WiFi được nhúng vào firmware.
Có thể dùng HTTP không cần SD nếu đã cấu hình `STREAM_URL`.

```sh
python3 -m http.server 8000 --directory sdcard
```

Ví dụ URL: `http://192.168.1.10:8000/music/demo.wav` — thay bằng IP LAN của máy.
Đuôi `.wav` (bỏ query string) chọn WAV; URL khác chọn MP3.
LRC của URL được tìm trên SD tại `/music/<tên-bài>.lrc`.

Nguồn HTTP chờ WiFi tối đa 12 giây, sau đó mở kết nối tối đa 4 lần với
backoff 0.5/1/2 giây; timeout mỗi thao tác mạng 1.5 giây. Mất mạng giữa bài
sẽ dừng; PLAY mở lại từ đầu, chưa có HTTP Range resume. WiFi tự thử kết nối lại.
Chuyển bài có thể chờ thao tác mạng hiện tại kết thúc khi thu hồi task nguồn.

## Điều khiển và giới hạn

Serial 115200: `p` phát/tạm dừng, `n` bài sau, `b` bài trước, `-`/`+` âm lượng.
Hiệu chỉnh touch X/Y MIN/MAX trong menuconfig; có thể đảo MIN/MAX để đảo trục.
Mặc định theo project `esp32_2432_clock_touch`: raw X=300..3900, Y=200..3700,
lọc median 5 mẫu. Giao diện ngang dùng MADCTL=0x60 (MX | MV) để chữ không bị lật gương;
touch dùng x=raw Y, y=raw X, không đảo chiều (đã đối chiếu log bấm 5 nút trên board).
Mỗi lần nhấn chỉ gửi một lệnh; xem log `Touch raw=... point=...` để hiệu chỉnh.
LCD dùng chuỗi khởi tạo ST7789 của project tham chiếu, RGB565, SPI 20 MHz.
Touch dùng SPI mềm trên chân riêng vì SPI3 đang dùng cho SD.

WAV tính thời lượng từ header, MP3 ước lượng CBR từ bitrate frame đầu,
không chính xác với VBR hoặc encoder padding. HTTP MP3 không có Content-Length
thì không hiển thị tổng thời lượng. HTTP không có độ dài cũng không thể phân biệt
EOF hợp lệ với kết nối bị cắt ở mọi trường hợp.

Đồng hồ lời bài hát đếm mẫu audio đã nạp DMA, không tính khoảng pause/buffering;
có thể đi trước loa khoảng hàng chục ms. Buffer 24 KB tương đương khoảng 1.5 giây
MP3 128 kbps; WAV có ít dự trữ hơn. Chưa hỗ trợ seek hoặc phát gapless.

LRC tối đa 32 KB, 600 timestamp, tổng text 16 KB, 191 byte/dòng.
Font bitmap gồm ASCII, Latin và tiếng Việt NFC; ký tự ngoài bộ font hiện `?`.

## Kiểm thử

### Kiểm tra độ ổn định trên board

Mỗi 10 giây, log `memory` báo heap còn trống, heap thấp nhất từ lúc khởi động
và block RAM nội lớn nhất. Khi kết thúc/đổi bài, log `source` báo số lần phải
buffer lại (không tính prebuffer ban đầu). Đây là số lần đọc nguồn bị đói,
không phải bộ đếm underrun DMA; giá trị 0 chưa chứng minh âm thanh không khựng.
Lỗi ghi I2S được log cùng số byte thực tế/yêu cầu.

Sau khi nạp firmware, phát WAV và MP3 từ SD, rồi MP3 qua HTTP ít nhất 30 phút
mỗi nguồn, đồng thời bấm cảm ứng và chuyển bài. Ghi lại log và nghe/thu đầu ra
để phát hiện ngắt tiếng. Theo dõi heap có giảm liên tục hay không; thử mạng yếu
và mất kết nối để xác nhận báo lỗi/phục hồi bằng PLAY. Host tests và build
không thay thế kiểm tra thời gian thực này.

### Giới hạn video có âm thanh

Firmware hiện chưa hỗ trợ video, demux hay đồng bộ audio/video. Với SPI LCD
20 MHz và RGB565 320×240, chỉ truyền pixel đã cần ít nhất 61,44 ms/khung
(trần lý thuyết khoảng 16,3 FPS), chưa tính giải mã, lệnh LCD và chia sẻ CPU.
Không thể cam kết video 30 FPS toàn màn hình với cấu hình truyền hiện tại.

Nếu bổ sung video, cần ưu tiên task âm thanh, giới hạn RAM cho bộ đệm hình,
dùng timestamp theo âm thanh thực sự phát và bỏ khung hình trễ thay vì chặn
âm thanh. Định dạng nguồn, độ phân giải và FPS phải được chọn rồi đo trên board;
chưa có kết quả kiểm thử video để công bố mức FPS được hỗ trợ.

### Kiểm thử trên máy phát triển

```sh
./tools/test_host.sh
idf.py build size
```

Host tests kiểm tra LRC, WAV hợp lệ/lỗi/truncated header/chunk padding và giải mã
MP3 tạo bằng ffmpeg, chạy AddressSanitizer/UndefinedBehaviorSanitizer.
Cần `g++`; MP3 test cần `ffmpeg` có libmp3lame. Tạo font lại chỉ khi cần:
`python3 tools/make_font.py` (Pillow và DejaVu Sans).

Đã build bằng ESP-IDF 5.5.3 và chạy host tests thành công. **Chưa kiểm thử trên
board thật**. Khi có board: chạy demo 8 giây, kiểm tra tiếng Việt/loa/5 nút cảm ứng,
pause/resume, đổi bài; thử MP3 và HTTP; rút SD/ngắt WiFi giữa bài và thử PLAY lại.

## Mã nguồn

- `main/main.cpp`: khởi tạo SD/WiFi, danh sách nhạc, Serial và vòng lặp UI.
- `main/source.*`: producer SD/HTTP và stream buffer, hủy/thu hồi task.
- `main/audio.cpp`: WAV/MP3, I2S DMA cho MAX98357A, state và đồng bộ lời.
- `main/display.cpp`: ST7789 SPI, font bitmap, XPT2046 software SPI.
- `include/lyrics.h`, `include/wav.h`: parser độc lập phần cứng.
- `third_party/`: minimp3 được cố định commit và giấy phép font.
- `legacy/arduino/`: bản mã nguồn Arduino trước khi chuyển đổi, dùng để đối chiếu.
- `docs/design-original.md`: thiết kế ban đầu; không phải mô tả firmware hiện tại.

Tham khảo: [ESP-IDF I2S standard](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/api-reference/peripherals/i2s.html),
[minimp3](https://github.com/lieff/minimp3).
