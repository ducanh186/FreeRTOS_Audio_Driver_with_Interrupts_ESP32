

# Cấu hình hệ điều hành FreeRTOS cho ESP32
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Bao gồm tệp CMake của ESP-IDF
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Lọc các tệp nguồn trong thư mục src
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# Đảm bảo bao gồm tất cả các tệp nguồn của thư mục chính
FILE(GLOB_RECURSE main_sources ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.c)

# Kết hợp các tệp nguồn từ thư mục chính và các thư viện phụ
list(APPEND app_sources ${main_sources})

# Đăng ký các tệp nguồn và cấu hình thư viện
idf_component_register(SRCS ${app_sources})

# Bao gồm các thư mục header nếu có
include_directories(${CMAKE_SOURCE_DIR}/include)

# Đảm bảo bao gồm các thư viện con từ thư mục lib
add_subdirectory(lib/audio_input/src)
add_subdirectory(lib/audio_output/src)
add_subdirectory(lib/sd_card/src)
add_subdirectory(lib/spiffs/src)
add_subdirectory(lib/wav_file/src)
