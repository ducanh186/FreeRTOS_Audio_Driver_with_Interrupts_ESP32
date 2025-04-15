#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

// Play a specific WAV file
void play(Output *output, const char *fname)
{
  int16_t *samples = (int16_t *)malloc(sizeof(int16_t) * 1024);
  ESP_LOGI(TAG, "Opening file: %s", fname);  // Log when a file is about to be opened

  FILE *fp = fopen(fname, "rb");
  if (fp == NULL)
  {
    ESP_LOGE(TAG, "Failed to open WAV file: %s", fname);
    free(samples);
    return;
  }

  // Create a new WAV file reader
  WAVFileReader *reader = new WAVFileReader(fp);
  ESP_LOGI(TAG, "Start playing file: %s", fname);
  output->start(reader->sample_rate());

  while (true)
  {
    int samples_read = reader->read(samples, 1024);
    if (samples_read == 0)
    {
      ESP_LOGI(TAG, "End of file reached: %s", fname);
      break;
    }
    output->write(samples, samples_read);
  }

  output->stop();
  fclose(fp);
  delete reader;
  free(samples);
  ESP_LOGI(TAG, "Finished playing: %s", fname);  // Log when file playback finishes
}

// Task to play main music (main_music.wav)
void main_music_task(void *pvParameters)
{
  while (true)
  {
    if (is_main_music_playing)
    {
      ESP_LOGI(TAG, "Playing main music...");
      play(new I2SOutput(I2S_NUM_0, i2s_speaker_pins), "/sdcard/main_music.wav");
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // Delay for a while before checking the status again
  }
}

// Task to play background music (background_music_1.wav)
void background_music_task(void *pvParameters)
{
  while (true)
  {
    if (is_background_music_playing)
    {
      ESP_LOGI(TAG, "Playing background music...");
      play(new I2SOutput(I2S_NUM_0, i2s_speaker_pins), "/sdcard/background_music_1.wav");
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // Delay for a while before checking the status again
  }
}

// Main task for GPIO button press handling
void handle_button_push(void *pvParameters)  // Thêm đối số void* cho phù hợp với xTaskCreate
{
  gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
  gpio_set_pull_mode(GPIO_BUTTON, GPIO_PULLDOWN_ONLY);  // Setup GPIO button for play
  gpio_set_direction(GPIO_BUTTON_1, GPIO_MODE_INPUT);
  gpio_set_pull_mode(GPIO_BUTTON_1, GPIO_PULLDOWN_ONLY); // Setup GPIO button for play alternate song

  while (true)
  {
    // Wait for the user to push and hold the first button (GPIO_BUTTON)
    wait_for_button_push();
    ESP_LOGI(TAG, "GPIO_BUTTON pressed");

    // Toggle playing main music when GPIO_BUTTON is pressed
    is_main_music_playing = !is_main_music_playing;
    if (is_main_music_playing)
    {
      ESP_LOGI(TAG, "Main music started playing");
      // Start main music task
      if (main_music_task_handle == NULL)
      {
        xTaskCreate(main_music_task, "main_music_task", 8192, NULL, 1, &main_music_task_handle);
      }
    }
    else
    {
      ESP_LOGI(TAG, "Main music stopped");
      // Stop main music task
      if (main_music_task_handle != NULL)
      {
        vTaskDelete(main_music_task_handle);
        main_music_task_handle = NULL;
      }
    }

    // Wait for the user to push the second button (GPIO_BUTTON_1) to start background music
    wait_for_second_button_push();
    ESP_LOGI(TAG, "GPIO_BUTTON_1 pressed");

    // Start or stop background music when GPIO_BUTTON_1 is pressed
    if (is_main_music_playing)  // Ensure main music is playing before playing background music
    {
      is_background_music_playing = !is_background_music_playing;
      if (is_background_music_playing)
      {
        ESP_LOGI(TAG, "Background music started playing");
        // Start background music task
        if (background_music_task_handle == NULL)
        {
          xTaskCreate(background_music_task, "background_music_task", 8192, NULL, 1, &background_music_task_handle);
        }
      }
      else
      {
        ESP_LOGI(TAG, "Background music stopped");
        // Stop background music task
        if (background_music_task_handle != NULL)
        {
          vTaskDelete(background_music_task_handle);
          background_music_task_handle = NULL;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));  // 1-second delay before checking again
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

  gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
  gpio_set_pull_mode(GPIO_BUTTON, GPIO_PULLDOWN_ONLY);

  // Start the button press handler task
  xTaskCreate(handle_button_push, "handle_button_push", 8192, NULL, 1, NULL);
}
