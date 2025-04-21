#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <string.h>

#include "I2SOutput.h"
#include "SDCard.h"
#include "SPIFFS.h"
#include "WAVFileReader.h"
#include "config.h"
#include "esp_timer.h"

static const char *TAG = "app";
extern "C" {
  void app_main(void);
}
// Global variables for managing playback state
static bool is_main_music_playing = false;
static bool is_background_music_playing = false;
static bool mix_requested = false;
static TaskHandle_t main_music_task_handle = NULL;
static TaskHandle_t background_music_task_handle = NULL;

// Function to wait for button push
void wait_for_button_push()
{
  while (gpio_get_level(GPIO_BUTTON) == 0)  // Wait for GPIO_BUTTON to be pressed
  {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void wait_for_second_button_push()
{
  while (gpio_get_level(GPIO_BUTTON_1) == 0)  // Wait for GPIO_BUTTON_1 to be pressed
  {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void play_with_mix(Output *output, const char *main_fname, const char *mix_fname)
{
  int16_t *main_buf = (int16_t *)malloc(sizeof(int16_t) * 1024);
  int16_t *mix_buf = (int16_t *)malloc(sizeof(int16_t) * 1024);
  int16_t *output_buf = (int16_t *)malloc(sizeof(int16_t) * 1024);

  FILE *main_fp = fopen(main_fname, "rb");
  FILE *mix_fp = fopen(mix_fname, "rb");
  if (!main_fp || !mix_fp) {
    ESP_LOGE(TAG, "Failed to open files");
    free(main_buf); free(mix_buf); free(output_buf);
    if (main_fp) fclose(main_fp);
    if (mix_fp) fclose(mix_fp);
    return;
  }

  WAVFileReader *main_reader = new WAVFileReader(main_fp);
  WAVFileReader *mix_reader = new WAVFileReader(mix_fp);

  ESP_LOGI(TAG, "Sample rate file 1: %d", main_reader->sample_rate());
  ESP_LOGI(TAG, "Sample rate file 2: %d", mix_reader->sample_rate());
  output->start(main_reader->sample_rate()*2);

  while (is_main_music_playing) {
    int main_samples = main_reader->read(main_buf, 1024);
    if (main_samples == 0) break;
    if (mix_requested) {
      int mix_samples = mix_reader->read(mix_buf, main_samples);
      for (int i = 0; i < main_samples; i++) {
        int32_t mixed = main_buf[i];
        if (mix_requested && i < mix_samples) {
          mixed += mix_buf[i];
          output_buf[i] = mixed / 2;
        } 
        else {
          output_buf[i] = main_buf[i];
        }
      }
      if (mix_samples == 0) {
        mix_requested = false; // Reset mix request if no more samples
        fseek(mix_fp, 44, SEEK_SET); // Reset to start if needed
      }
    } else {
      memcpy(output_buf, main_buf, main_samples * sizeof(int16_t));
    }
    output->write(output_buf, main_samples);
  }
  ESP_LOGI(TAG, "Finished playing main music");
  output->stop();
  delete main_reader;
  delete mix_reader;
  if (main_fp) fclose(main_fp);
  if (mix_fp) fclose(mix_fp);
  free(main_buf); free(mix_buf); free(output_buf);
}

void main_music_task(void *pvParameters) {
  I2SOutput *output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
  play_with_mix(output, "/sdcard/gong.wav", "/sdcard/huh.wav");
  vTaskDelay(pdMS_TO_TICKS(100));
  delete output;
  main_music_task_handle = NULL;  
  vTaskDelete(NULL); // Self delete when done
}

void button_toggle_music_task(void *pvParameters)
{

  while (true)
  {
    if (gpio_get_level(GPIO_BUTTON) == 1) {
      ESP_LOGI(TAG, "GPIO_BUTTON pressed");
      is_main_music_playing = !is_main_music_playing;
      if (is_main_music_playing) {
        ESP_LOGI(TAG, "Main music started playing");
        if (main_music_task_handle == NULL) {
          xTaskCreate(main_music_task, "main_music_task", 4096, NULL, 1, &main_music_task_handle);
        }
        else is_main_music_playing = false;
      }
      vTaskDelay(pdMS_TO_TICKS(500)); // Debounce delay to avoid repeated triggering
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100 ms
  }
}

void button_trigger_mix_task(void *pvParameters)
{
  while (true) {
    if (gpio_get_level(GPIO_BUTTON_1) == 1) {
      ESP_LOGI(TAG, "GPIO_BUTTON_1 pressed - mix requested");
      mix_requested = !mix_requested; // Toggle mixing on/off
      vTaskDelay(pdMS_TO_TICKS(500)); // Debounce delay to avoid repeated triggering
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100 ms
  }
}

void app_main(void)
{
  ESP_LOGI(TAG, "Starting up");

#ifdef USE_SPIFFS
  ESP_LOGI(TAG, "Mounting SPIFFS on /sdcard");
  new SPIFFS("/sdcard");
#else
  ESP_LOGI(TAG, "Mounting SDCard on /sdcard");
  new SDCard("/sdcard", PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CLK, PIN_NUM_CS);
#endif

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

  gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
  gpio_set_pull_mode(GPIO_BUTTON, GPIO_PULLDOWN_ONLY);
  gpio_set_direction(GPIO_BUTTON_1, GPIO_MODE_INPUT);
  gpio_set_pull_mode(GPIO_BUTTON_1, GPIO_PULLDOWN_ONLY);

  // Start the button press handler task
  xTaskCreate(button_toggle_music_task, "button_toggle_music_task", 2048, NULL, 2, NULL);
  xTaskCreate(button_trigger_mix_task, "button_trigger_mix_task", 2048, NULL, 2, NULL);
}
