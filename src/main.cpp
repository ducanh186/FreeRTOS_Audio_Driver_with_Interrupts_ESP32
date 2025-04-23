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
static bool mix_requested = false;
static TaskHandle_t main_music_task_handle = NULL;

ManageSD *sdManager;

// Function to declare main_music_task before using it
void main_music_task(void *pvParameters);

// Function to wait for button push
void IRAM_ATTR gpio_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(event_group, BIT_BUTTON_PRESSED, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void play_music_with_mutex(Output *output) {
    // Critical section for reading/writing audio buffer with mutex
    if (xSemaphoreTake(audio_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // Play music from the list
        // Assuming music_files and mix_files are populated earlier
        // Add your play logic here

        xSemaphoreGive(audio_buffer_mutex); // Release mutex after using the audio buffer
    }
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
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for a while before checking again
    }
}

void button_trigger_mix_task(void *pvParameters) {
    while (true) {
        if (xEventGroupWaitBits(event_group, BIT_BUTTON_PRESSED, pdTRUE, pdFALSE, portMAX_DELAY)) {
            ESP_LOGI(TAG, "Button pressed - mix requested");
            mix_requested = !mix_requested; // Toggle mixing on/off
            xEventGroupSetBits(event_group, BIT_MIX_REQUESTED);  // Set mix requested event
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for a while before checking again
    }
}

// Correctly declare the main_music_task before use
void main_music_task(void *pvParameters) {
    I2SOutput *output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
    play_music_with_mutex(output); // Play music with mutex protection
    vTaskDelay(pdMS_TO_TICKS(100));
    delete output;
    main_music_task_handle = NULL;
    vTaskDelete(NULL); // Self delete when done
}

void app_main(void)
{
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
    gpio_isr_handler_add(GPIO_BUTTON, gpio_isr_handler, NULL);

    // Start the button press handler task
    xTaskCreate(button_toggle_music_task, "button_toggle_music_task", 2048, NULL, 2, NULL);
    xTaskCreate(button_trigger_mix_task, "button_trigger_mix_task", 2048, NULL, 2, NULL);
}
