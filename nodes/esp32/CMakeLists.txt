# Proyecto ESP-IDF del nodo ESP32. Compilar desde esta carpeta con:
#   idf.py set-target esp32
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rt_node_esp32)