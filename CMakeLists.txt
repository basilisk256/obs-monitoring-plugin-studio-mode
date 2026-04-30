# --- Detect if the plugin is built out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.16...3.26)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C CXX)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${PROJECT_NAME} MODULE)

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api Qt6::Widgets)
else()
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::frontend-api Qt6::Widgets)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  AUTOMOC ON)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

target_sources(${PROJECT_NAME} PRIVATE
  audio-cue-module.cpp
  audio-cue-engine.cpp
  audio-cue-engine.h
  audio-cue-dock.cpp
  audio-cue-dock.hpp
  version.h)

if(BUILD_OUT_OF_TREE)
  set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
else()
  set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/audio-cue" PREFIX "")
endif()
