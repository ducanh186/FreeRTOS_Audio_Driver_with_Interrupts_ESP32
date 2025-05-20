#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include <dirent.h>
#include <string.h>
#include "I2SOutput.h"
#include "SDCard.h"
#include "SPIFFS.h"
#include "WAVFileReader.h"
#include "config.h"

static const char *TAG = "app";
extern "C" {
  void app_main(void);
}

/*Task:
+ audio_processing_task: Đọc file WAV, mix âm thanh, gửi dữ liệu vào queue.
+ i2s_output_task: Lấy dữ liệu từ queue và phát qua I2S.
+ button_task: Xử lý sự kiện nút bấm qua ISR.
*/
// Định nghĩa hằng số
#define SAMPLE_RATE 44100
#define BUFFER_SIZE 1024
#define QUEUE_SIZE 20 // Tăng kích thước queue để giảm underrun
#define DEBOUNCE_TIME_MS 500 //Thời gian debounce (500ms) để loại bỏ nhiễu khi nhấn nút.

// Biến toàn cục FreeRTOS
static QueueHandle_t audio_queue;
static EventGroupHandle_t event_group; //EventGroup để đồng bộ hóa trạng thái (phát, mix, dừng).
static TimerHandle_t debounce_timer; //Timer phần mềm để debounce nút bấm.

// Event Bits
#define BIT_MUSIC_PLAYING (1 << 0)
#define BIT_MIX_REQUESTED (1 << 1)
#define BIT_BUTTON_PLAY (1 << 2)
#define BIT_BUTTON_MIX (1 << 3)
#define BIT_STOP_REQUESTED (1 << 4)

// ISR cho nút bấm
/*   ISR (Interrupt Service Routine)
Phản ứng nhanh với sự kiện phần cứng (như nhấn nút) mà không cần thăm dò (polling) liên tục, tiết kiệm CPU.
IRAM (Internal RAM) 
ISR tiêu tốn IRAM, giới hạn trên ESP32 (128 KB IRAM).
*/
void IRAM_ATTR button_play_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(event_group, BIT_BUTTON_PLAY, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void IRAM_ATTR button_mix_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(event_group, BIT_BUTTON_MIX, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// Callback cho timer debounce
void debounce_timer_callback(TimerHandle_t xTimer) {
    // Không cần xử lý, chỉ dùng để đảm bảo debounce
    // Không hiểu cái này lắmlắm
}

// Task đọc và mixing âm thanh
void audio_processing_task(void *pvParameters) {
    I2SOutput *output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
    int16_t *main_buf = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    int16_t *mix_buf = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    int16_t *output_buf = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    if (!main_buf || !mix_buf || !output_buf) {
        ESP_LOGE(TAG, "Not enough memory for buffers");
        delete output;
        vTaskDelete(NULL);
    }

    FILE *main_fp = fopen("/sdcard/gong.wav", "rb");
    FILE *mix_fp = fopen("/sdcard/huh.wav", "rb");
    if (!main_fp || !mix_fp) {
        ESP_LOGE(TAG, "Cannot open WAV files");
        if (main_fp) fclose(main_fp);
        if (mix_fp) fclose(mix_fp);
        free(main_buf); free(mix_buf); free(output_buf);
        delete output;
        vTaskDelete(NULL);
    }

    WAVFileReader *main_reader = new WAVFileReader(main_fp);
    WAVFileReader *mix_reader = new WAVFileReader(mix_fp);

    ESP_LOGI(TAG, "Sample rate: %d", main_reader->sample_rate());
    output->start(main_reader->sample_rate());

    while (xEventGroupGetBits(event_group) & BIT_MUSIC_PLAYING) {
        if (xEventGroupGetBits(event_group) & BIT_STOP_REQUESTED) {
            break;
        }

        int main_samples = main_reader->read(main_buf, BUFFER_SIZE);
        if (main_samples <= 0) break;

        if (xEventGroupGetBits(event_group) & BIT_MIX_REQUESTED) {
            int mix_samples = mix_reader->read(mix_buf, main_samples);
            for (int i = 0; i < main_samples; i++) {
                if (i < mix_samples) {
                    int32_t mixed = (int32_t)main_buf[i] + (int32_t)mix_buf[i];
                    output_buf[i] = (int16_t)(mixed / 2);
                } else {
                    output_buf[i] = main_buf[i];
                }
            }
            if (mix_samples < main_samples) {
                xEventGroupClearBits(event_group, BIT_MIX_REQUESTED);
                fseek(mix_fp, 44, SEEK_SET);
            }
        } else {
            memcpy(output_buf, main_buf, main_samples * sizeof(int16_t));
        }

        // Dùng xQueueOverwrite để ưu tiên dữ liệu mới nếu queue đầy
        if (!xQueueOverwrite(audio_queue, output_buf)) {
            ESP_LOGW(TAG, "Queue full, overwriting data");
        }
    }

    output->stop();
    delete main_reader;
    delete mix_reader;
    fclose(main_fp);
    fclose(mix_fp);
    free(main_buf); free(mix_buf); free(output_buf);
    delete output;
    xEventGroupClearBits(event_group, BIT_MUSIC_PLAYING | BIT_STOP_REQUESTED);
    vTaskDelete(NULL);
}

// Task phát âm thanh qua I2S
void i2s_output_task(void *pvParameters) {
    int16_t *buffer = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    if (!buffer) {
        ESP_LOGE(TAG, "Not enough memory for buffer");
        vTaskDelete(NULL);
    }

    while (xEventGroupGetBits(event_group) & BIT_MUSIC_PLAYING) {
        if (xQueueReceive(audio_queue, buffer, portMAX_DELAY) == pdTRUE) {
            size_t bytes_written;
            i2s_write(I2S_NUM_0, buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_written, portMAX_DELAY);
            if (bytes_written != BUFFER_SIZE * sizeof(int16_t)) {
                ESP_LOGW(TAG, "Not all data written to I2S");
            }
        }
    }
    free(buffer);
    vTaskDelete(NULL);
}

// Task xử lý nút bấm
void button_task(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(event_group, BIT_BUTTON_PLAY | BIT_BUTTON_MIX, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & BIT_BUTTON_PLAY) {
            ESP_LOGI(TAG, "GPIO_BUTTON pressed");
            if (xEventGroupGetBits(event_group) & BIT_MUSIC_PLAYING) {
                ESP_LOGI(TAG, "Main music stopping");
                xEventGroupSetBits(event_group, BIT_STOP_REQUESTED);
            } else {
                ESP_LOGI(TAG, "Main music starting");
                xEventGroupSetBits(event_group, BIT_MUSIC_PLAYING);
                xTaskCreate(audio_processing_task, "audio_processing_task", 4096, NULL, 5, NULL);
                xTaskCreate(i2s_output_task, "i2s_output_task", 4096, NULL, 5, NULL);
            }
            xTimerStart(debounce_timer, 0); // Bắt đầu timer debounce
        }
        if (bits & BIT_BUTTON_MIX) {
            ESP_LOGI(TAG, "GPIO_BUTTON_1 pressed - mix requested");
            xEventGroupSetBits(event_group, BIT_MIX_REQUESTED);
            xTimerStart(debounce_timer, 0);
        }
        // Dùng vTaskDelayUntil để kiểm soát tần suất
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting audio player with deep OS integration");

#ifdef USE_SPIFFS
    ESP_LOGI(TAG, "Mounting SPIFFS on /sdcard");
    new SPIFFS("/sdcard");
#else
    ESP_LOGI(TAG, "Mounting SDCard on /sdcard");
    new SDCard("/sdcard", PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CLK, PIN_NUM_CS);
#endif

    // Kiểm tra nội dung thẻ SD
    DIR *dir = opendir("/sdcard");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "Found file: %s", ent->d_name);
        }
        closedir(dir);
    } else {
        ESP_LOGE(TAG, "Cannot open /sdcard directory!");
    }

    // Khởi tạo FreeRTOS components
    event_group = xEventGroupCreate();
    audio_queue = xQueueCreate(QUEUE_SIZE, BUFFER_SIZE * sizeof(int16_t));
    debounce_timer = xTimerCreate("debounce_timer", pdMS_TO_TICKS(DEBOUNCE_TIME_MS), pdFALSE, NULL, debounce_timer_callback);

    // Cấu hình GPIO cho nút bấm
    gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BUTTON, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(GPIO_BUTTON_1, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BUTTON_1, GPIO_PULLDOWN_ONLY);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_BUTTON, button_play_isr_handler, NULL);
    gpio_isr_handler_add(GPIO_BUTTON_1, button_mix_isr_handler, NULL);

    // Tạo task xử lý nút bấm
    xTaskCreate(button_task, "button_task", 2048, NULL, 2, NULL);
}