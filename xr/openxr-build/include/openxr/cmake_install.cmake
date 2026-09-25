# Install script for directory: J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/OPENXR")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Headers" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/openxr" TYPE FILE FILES
    "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr/openxr_platform_defines.h"
    "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr/openxr.h"
    "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr/openxr_loader_negotiation.h"
    "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr/openxr_platform.h"
    "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr/openxr_reflection.h"
    "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr/openxr_reflection_structs.h"
    "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/OpenXR-SDK/include/openxr/openxr_reflection_parent_structs.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "J:/EchoVR-Tools-Launcher/EchoVR-Hand-Tracking/xr/openxr-build/include/openxr/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
