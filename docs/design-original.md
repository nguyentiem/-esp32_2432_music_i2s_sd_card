ESP32 làm music player kiểu này là một hệ thống có 2 luồng dữ liệu chạy song song: luồng audio (đọc → giải mã → I2S) và luồng hiển thị (đọc lyric → đồng bộ → vẽ UI), cả hai đều cần khớp với nhau qua một "đồng hồ phát nhạc" chung. Đây là kiến trúc tổng quan:

Phần cứng đề xuất
Chip: ESP32-S3 (có PSRAM 8MB) thay vì ESP32 thường. Vì bạn cần đồng thời: buffer WiFi, buffer PCM, framebuffer LVGL cho màn hình, và bộ nhớ decode MP3 — 520KB SRAM gốc sẽ rất chật.
DAC/Amp I2S: MAX98357A (I2S amp tích hợp, không cần amp ngoài) là lựa chọn dễ nhất. Nếu cần chất lượng cao hơn thì PCM5102 (DAC thuần) + amp riêng.
Màn hình: SPI TFT (ILI9341/ST7789), nhiều module có sẵn khe SD trên cùng board (dùng chung bus SPI khác CS).
SD: SPI mode là đủ cho bitrate MP3/WAV thông thường.
Pipeline audio: dùng ESP-ADF thay vì tự viết từ đầu

Vì bài toán "đọc từ SD hoặc HTTP → decode → I2S" đúng là use-case chính của ESP-ADF (Espressif Audio Development Framework), nó có sẵn các audio_element ghép thành audio_pipeline:

fatfs_stream_reader / http_stream_reader → mp3_decoder → i2s_stream_writer

Bạn chỉ cần đổi element đầu vào (fatfs_stream hay http_stream) tùy theo nguồn SD hay WiFi, phần decoder và I2S writer giữ nguyên — đúng ý tưởng "source abstraction" trong sơ đồ trên. ADF còn có sẵn cơ chế lấy vị trí phát (byte đã decode) để tính progress bar mà không phải tự đếm.

Nếu bạn muốn tự kiểm soát toàn bộ (đúng phong cách low-level bạn hay làm với STM32), thì tự viết bằng ESP-IDF thuần:

Driver I2S mới (driver/i2s_std.h, IDF ≥5.x) thay cho driver i2s.h cũ đã deprecated.
Decoder MP3 nhẹ: libhelix-mp3 hoặc minimp3 (không cần ADF).
WAV thì khỏi cần decode, đọc PCM thẳng ra I2S.
Kiến trúc task/queue (FreeRTOS)
Task đọc nguồn (SD file read hoặc HTTP client với esp_http_client, đọc theo chunk) → đẩy vào ring buffer thô (dùng xRingbufferCreate hoặc ADF's ringbuf).
Task decode (pin core 1, priority cao vừa phải) đọc từ ring buffer thô, decode ra PCM, đẩy vào ring buffer PCM.
Task I2S write (priority cao nhất, time-critical) lấy PCM từ buffer, gọi i2s_channel_write(). Đây là task không được phép block lâu, tránh giật/click âm thanh.
Task lyric sync: theo dõi số byte/sample đã decode → suy ra thời gian phát hiện tại (ms = bytes_decoded / (sample_rate * channels * bytes_per_sample)), so với mảng timestamp đã parse từ .lrc, cập nhật index dòng lyric hiện tại.
Task display (LVGL): nhận state (tên bài, % progress, dòng lyric hiện tại) qua queue hoặc biến shared có mutex, vẽ lại UI theo tick timer riêng (không cần vẽ mỗi sample, ~30-60fps là đủ mượt).

Việc tách network task khỏi core chứa I2S task là quan trọng — WiFi có jitter, nếu để chung core với I2S dễ gây xrun (mất tiếng, tạch tạch).

Đồng bộ lyric (.lrc)

Format LRC rất đơn giản: [mm:ss.xx]nội dung dòng. Parse một lần khi load bài thành mảng {timestamp_ms, text}[]. Task sync chỉ cần tìm phần tử có timestamp_ms lớn nhất mà ≤ thời gian hiện tại (binary search hoặc con trỏ tăng dần vì thời gian chỉ tăng) → đó là dòng đang hát. Muốn hiệu ứng "chạy chữ" (karaoke-style) thì nội suy % giữa timestamp dòng hiện tại và dòng kế tiếp để tô màu dần.

Progress bar

Cần 2 số: current_ms (tính như trên) và total_ms (lấy từ header file — WAV có trong header, MP3 thì ước lượng qua bitrate × filesize, hoặc dùng ID3 nếu có; với stream HTTP thì lấy từ metadata server nếu có, hoặc ẩn tổng thời gian nếu không biết trước).

Muốn mình viết cụ thể phần code cho một mảng nào trong sơ đồ trên (VD: cấu hình i2s_std, parser .lrc, hay pipeline ADF) thì nói mình làm tiếp phần đó.

Cách stream audio qua WiFi trên ESP32 với buffer chống giật

Phần WiFi streaming là chỗ dễ gây giật/tạch nhất trong cả hệ thống, vì băng thông WiFi không ổn định (retry, nhiễu, tranh chấp kênh) trong khi I2S cần dữ liệu đều đặn từng mili-giây. Cách giải quyết chuẩn là tách hoàn toàn tốc độ đọc mạng khỏi tốc độ phát, thông qua một buffer trung gian đủ lớn.

1. Buffer trung gian: dùng FreeRTOS Stream Buffer

Giữa task đọc HTTP và task decode, dùng StreamBufferHandle_t (FreeRTOS built-in, thiết kế đúng cho mô hình 1 writer/1 reader với dữ liệu dạng byte-stream liên tục) thay vì tự viết ring buffer thủ công:

c
#define STREAM_BUF_SIZE      (80 * 1024)   // ~5s ở bitrate 128kbps
#define PREBUFFER_THRESHOLD  (48 * 1024)   // chờ ~3s dữ liệu rồi mới phát

StreamBufferHandle_t audio_buf;

// Cấp phát vùng lưu trữ trong PSRAM (ESP32-S3), struct điều khiển ở SRAM
uint8_t *storage = heap_caps_malloc(STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
static StaticStreamBuffer_t sb_struct;
audio_buf = xStreamBufferCreateStatic(STREAM_BUF_SIZE, 1, storage, &sb_struct);

Dùng PSRAM cho buffer này vì nó không time-critical (chỉ decoder/I2S task mới cần SRAM nhanh), nên PSRAM dư dả 8MB là chỗ hợp lý để chứa vài chục KB này mà không đè lên heap chính.

2. Task đọc HTTP — chỉ có nhiệm vụ đổ dữ liệu vào buffer
c
void network_reader_task(void *arg) {
    esp_http_client_config_t cfg = {
        .url = stream_url,
        .timeout_ms = 5000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    while (should_stream) {
        if (esp_http_client_open(client, 0) != ESP_OK ||
            esp_http_client_fetch_headers(client) < 0) {
            reconnect_with_backoff();   // xem mục 4
            continue;
        }

        uint8_t chunk[2048];
        int len;
        while ((len = esp_http_client_read(client, (char*)chunk, sizeof(chunk))) > 0) {
            size_t sent = 0;
            while (sent < (size_t)len) {
                // block khi buffer đầy -> tự tạo backpressure, dừng đọc socket
                sent += xStreamBufferSend(audio_buf, chunk + sent, len - sent, pdMS_TO_TICKS(1000));
            }
        }
        esp_http_client_close(client);
        if (should_stream) reconnect_with_backoff();  // mất kết nối giữa chừng
    }
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

Điểm quan trọng: khi xStreamBufferSend block vì buffer đầy, task này tự động ngưng gọi esp_http_client_read → TCP window sẽ báo ngưng cho phía server/socket, không cần bạn tự viết logic điều tiết tốc độ đọc.

3. Prebuffer + state machine BUFFERING ↔ PLAYING

Task decode/I2S không được bắt đầu phát ngay khi có byte đầu tiên — cần "mồi" trước một khoảng đệm:

c
typedef enum { STATE_BUFFERING, STATE_PLAYING } player_state_t;
player_state_t state = STATE_BUFFERING;

void decoder_task(void *arg) {
    for (;;) {
        size_t avail = xStreamBufferBytesAvailable(audio_buf);

        if (state == STATE_BUFFERING) {
            if (avail >= PREBUFFER_THRESHOLD) {
                state = STATE_PLAYING;
                ui_set_status(NULL);          // tắt "Đang tải..."
            } else {
                ui_set_status("Đang tải...");
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }

        size_t got = xStreamBufferReceive(audio_buf, decode_in, want_len, pdMS_TO_TICKS(200));
        if (got == 0) {
            // underrun giữa chừng -> quay lại buffering, không throw lỗi
            state = STATE_BUFFERING;
            continue;
        }
        // decode got bytes -> PCM -> đẩy vào PCM buffer cho I2S task
    }
}

Khi underrun xảy ra giữa bài hát, đừng để I2S task đọc buffer rỗng và phát ra tiếng rè — quay về STATE_BUFFERING, có thể cho I2S task feed vài chục ms mẫu 0 (im lặng) trong lúc chờ, mượt hơn nhiều so với để mất đồng bộ.

4. Reconnect với backoff
c
void reconnect_with_backoff(void) {
    static int retry = 0;
    int delay_ms = MIN(500 * (1 << retry), 8000);  // 500ms, 1s, 2s... tối đa 8s
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    retry++;
}

Nếu server hỗ trợ, dùng header Range để nối tiếp từ byte đã tải thay vì phát lại từ đầu (áp dụng được với file VOD, không áp dụng được với radio live stream).

5. Một tinh chỉnh hay bị bỏ sót: WiFi power save

Mặc định ESP32 bật modem sleep để tiết kiệm điện, gây ra các khoảng trễ đột biến (vài chục ms) khi radio ngủ rồi thức dậy — đủ để làm cạn buffer nếu buffer nhỏ. Với ứng dụng streaming audio, nên tắt hẳn:

c
esp_wifi_set_ps(WIFI_PS_NONE);

Đánh đổi là tốn điện hơn, nhưng ổn định hơn hẳn nếu thiết bị cắm nguồn hoặc pin dung lượng đủ lớn.

6. Tính kích thước buffer theo bitrate
Nguồn	Bitrate	Buffer cho 3–5s
MP3 128kbps	16 KB/s	48–80 KB
MP3 320kbps	40 KB/s	120–200 KB
WAV PCM 44.1kHz/16-bit stereo	176 KB/s	~530–880 KB

Vì vậy nếu stream qua WiFi, nên ưu tiên nguồn nén (MP3/AAC) thay vì WAV thô — buffer nhỏ hơn nhiều lần cho cùng số giây đệm.

Nếu bạn dùng ESP-ADF: http_stream_reader element đã có sẵn gần như toàn bộ cơ chế này (buffer nội bộ, event callback khi mất kết nối để tự reconnect) — phần trên hữu ích nếu bạn tự viết tay hoặc muốn tinh chỉnh sâu hơn cấu hình mặc định của ADF.