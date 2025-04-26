#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <dirent.h>
#include <string.h>

#include "I2SOutput.h"
#include "SDCard.h"
#include "SPIFFS.h"
#include "WAVFileReader.h"
#include "config.h"
#include "esp_timer.h"
#include "manage_sd.h"

static const char *TAG = "app";
extern "C" {
  void app_main(void);
}

// FreeRTOS Semaphores and Mutexes
SemaphoreHandle_t sd_card_mutex;
SemaphoreHandle_t audio_buffer_mutex;
SemaphoreHandle_t mix_mutex;
EventGroupHandle_t event_group;

// Event Bits
#define BIT_MUSIC_PLAYING    (1 << 0)
#define BIT_MIX_REQUESTED    (1 << 1)
#define BIT_BUTTON_PRESSED   (1 << 2)

// Global variables for managing playback state
static bool is_main_music_playing = false;
static TaskHandle_t main_music_task_handle = NULL;

ManageSD *sdManager;

// ISR handler for button press
void IRAM_ATTR gpio_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(event_group, BIT_BUTTON_PRESSED, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// Adapted play_with_mix function with mutex and event group synchronization
void play_with_mix(Output *output, const char *main_fname, const char *mix_fname) {
    int16_t *main_buf = (int16_t *)malloc(sizeof(int16_t) * 1024);
    int16_t *mix_buf = (int16_t *)malloc(sizeof(int16_t) * 1024);
    int16_t *output_buf = (int16_t *)malloc(sizeof(int16_t) * 1024);

    // Take SD card mutex to safely open files
    if (xSemaphoreTake(sd_card_mutex, portMAX_DELAY) == pdTRUE) {
        FILE *main_fp = fopen(main_fname, "rb");
        FILE *mix_fp = fopen(mix_fname, "rb");
        if (!main_fp || !mix_fp) {
            ESP_LOGE(TAG, "Failed to open files");
            free(main_buf); free(mix_buf); free(output_buf);
            if (main_fp) fclose(main_fp);
            if (mix_fp) fclose(mix_fp);
            xSemaphoreGive(sd_card_mutex);
            return;
        }
        xSemaphoreGive(sd_card_mutex); // Release mutex after opening files

        WAVFileReader *main_reader = new WAVFileReader(main_fp);
        WAVFileReader *mix_reader = new WAVFileReader(mix_fp);

        ESP_LOGI(TAG, "Sample rate file 1: %d", main_reader->sample_rate());
        ESP_LOGI(TAG, "Sample rate file 2: %d", mix_reader->sample_rate());
        output->start(main_reader->sample_rate() * 2);

        while (is_main_music_playing) {
            int main_samples = main_reader->read(main_buf, 1024);
            if (main_samples == 0) break;

            // Check for mix request using event group
            EventBits_t bits = xEventGroupGetBits(event_group);
            if (bits & BIT_MIX_REQUESTED) {
                int mix_samples = mix_reader->read(mix_buf, main_samples);
                if (xSemaphoreTake(mix_mutex, portMAX_DELAY) == pdTRUE) {
                    for (int i = 0; i < main_samples; i++) {
                        int32_t mixed = main_buf[i];
                        if (i < mix_samples) {
                            mixed += mix_buf[i];
                            output_buf[i] = mixed / 2;
                        } else {
                            output_buf[i] = main_buf[i];
                        }
                    }
                    xSemaphoreGive(mix_mutex);
                }
                if (mix_samples == 0) {
                    xEventGroupClearBits(event_group, BIT_MIX_REQUESTED);
                    fseek(mix_fp, 44, SEEK_SET); // Reset mix file to start
                }
            } else {
                memcpy(output_buf, main_buf, main_samples * sizeof(int16_t));
            }

            // Write to audio buffer with mutex protection
            if (xSemaphoreTake(audio_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                output->write(output_buf, main_samples);
                xSemaphoreGive(audio_buffer_mutex);
            }
        }

        ESP_LOGI(TAG, "Finished playing main music");
        output->stop();
        delete main_reader;
        delete mix_reader;
        if (main_fp) fclose(main_fp);
        if (mix_fp) fclose(mix_fp);
        free(main_buf); free(mix_buf); free(output_buf);
    }
}

void main_music_task(void *pvParameters) {
    I2SOutput *output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
    play_with_mix(output, "/sdcard/gong.wav", "/sdcard/huh.wav");
    vTaskDelay(pdMS_TO_TICKS(100));
    delete output;
    main_music_task_handle = NULL;
    vTaskDelete(NULL); // Self delete when done
}

void button_toggle_music_task(void *pvParameters) {
    while (true) {
        if (xEventGroupWaitBits(event_group, BIT_BUTTON_PRESSED, pdTRUE, pdFALSE, portMAX_DELAY)) {
            ESP_LOGI(TAG, "Button pressed");
            is_main_music_playing = !is_main_music_playing;
            if (is_main_music_playing) {
                ESP_LOGI(TAG, "Main music started playing");
                if (main_music_task_handle == NULL) {
                    xTaskCreate(main_music_task, "main_music_task", 4096, NULL, 1, &main_music_task_handle);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait before checking again
    }
}

void button_trigger_mix_task(void *pvParameters) {
    while (true) {
        if (xEventGroupWaitBits(event_group, BIT_BUTTON_PRESSED, pdTRUE, pdFALSE, portMAX_DELAY)) {
            ESP_LOGI(TAG, "Button pressed - mix requested");
            EventBits_t bits = xEventGroupGetBits(event_group);
            if (bits & BIT_MIX_REQUESTED) {
                xEventGroupClearBits(event_group, BIT_MIX_REQUESTED);
            } else {
                xEventGroupSetBits(event_group, BIT_MIX_REQUESTED);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait before checking again
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting up");

    // Initialize semaphores and mutexes
    sd_card_mutex = xSemaphoreCreateMutex();
    audio_buffer_mutex = xSemaphoreCreateMutex();
    mix_mutex = xSemaphoreCreateMutex();
    event_group = xEventGroupCreate();

    // Initialize SD card and manage SD
    SDCard *sdCard = new SDCard("/sdcard", PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CLK, PIN_NUM_CS);
    sdManager = new ManageSD(sdCard);

    // List files in the SD card directory
    sdManager->listFiles("/sdcard");

    // Set up GPIO interrupt for button press
    gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BUTTON, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(GPIO_BUTTON_1, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_BUTTON_1, GPIO_PULLDOWN_ONLY);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_BUTTON, gpio_isr_handler, NULL);
    gpio_isr_handler_add(GPIO_BUTTON_1, gpio_isr_handler, NULL);

    // Start the button press handler tasks
    xTaskCreate(button_toggle_music_task, "button_toggle_music_task", 2048, NULL, 2, NULL);
    xTaskCreate(button_trigger_mix_task, "button_trigger_mix_task", 2048, NULL, 2, NULL);
}