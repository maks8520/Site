cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vostochnaya_baza_s3)

spiffs_create_partition_image(storage spiffs_dir FLASH_IN_PROJECT)
