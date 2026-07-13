cmake_minimum_required(VERSION 3.16)

# common/ lives outside the project root (one level up), so register it explicitly.
# components/ under mcu/ is auto-discovered and needs no entry here.
set(EXTRA_COMPONENT_DIRS "../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mcu)
