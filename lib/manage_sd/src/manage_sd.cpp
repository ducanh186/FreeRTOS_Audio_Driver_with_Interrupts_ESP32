#include "manage_sd.h"
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"

static const char *TAG = "ManageSD";

// Constructor
ManageSD::ManageSD(SDCard *sdCard) {
  m_sdCard = sdCard;
  m_file = nullptr;
  ESP_LOGI(TAG, "ManageSD initialized");
}

// Destructor
ManageSD::~ManageSD() {
  if (m_file != nullptr) {
    fclose(m_file);
    ESP_LOGI(TAG, "File closed");
  }
  ESP_LOGI(TAG, "ManageSD destroyed");
}

// Mở file
bool ManageSD::openFile(const char *filename) {
  if (m_file != nullptr) {
    ESP_LOGW(TAG, "File already opened, closing current file first");
    fclose(m_file);  // Đóng file hiện tại nếu đã mở
  }

  m_file = fopen(filename, "r");
  if (m_file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file: %s", filename);
    return false;
  }

  strncpy(m_fileName, filename, sizeof(m_fileName) - 1);
  m_fileName[sizeof(m_fileName) - 1] = '\0';  // Đảm bảo tên file kết thúc bằng null
  ESP_LOGI(TAG, "File opened: %s", filename);
  return true;
}

// Đọc nội dung tệp hiện tại
bool ManageSD::readCurrentFile(char *buffer, size_t bufferSize) {
  if (m_file == nullptr) {
    ESP_LOGE(TAG, "No file is opened");
    return false;
  }

  size_t bytesRead = fread(buffer, 1, bufferSize, m_file);
  if (bytesRead == 0) {
    ESP_LOGE(TAG, "Failed to read from file: %s", m_fileName);
    return false;
  }

  buffer[bytesRead] = '\0';  // Đảm bảo buffer kết thúc bằng null
  ESP_LOGI(TAG, "Read %d bytes from file: %s", bytesRead, m_fileName);
  return true;
}

// Liệt kê các tệp trong thư mục
bool ManageSD::listFiles(const char *dirName) {
  struct dirent *entry;
  DIR *dp = opendir(dirName);
  
  if (dp == nullptr) {
    ESP_LOGE(TAG, "Failed to open directory: %s", dirName);
    return false;
  }

  ESP_LOGI(TAG, "Listing files in directory: %s", dirName);
  while ((entry = readdir(dp)) != nullptr) {
    // Bỏ qua các thư mục ẩn ('.' và '..')
    if (entry->d_name[0] == '.') {
      continue;
    }
    ESP_LOGI(TAG, "Found file: %s", entry->d_name);
  }
  closedir(dp);
  return true;
}

// Đóng file
bool ManageSD::closeFile() {
  if (m_file != nullptr) {
    fclose(m_file);
    m_file = nullptr;
    ESP_LOGI(TAG, "File closed");
    return true;
  }
  ESP_LOGW(TAG, "No file is opened to close");
  return false;
}
