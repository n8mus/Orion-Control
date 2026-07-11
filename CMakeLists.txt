# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(TenTecSDRConsole VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# SDRplay RSP source is optional so the core app always builds. Turn on with
# -DBUILD_SDRPLAY=ON once the SDRplay API service is running (Phase 0).
option(BUILD_SDRPLAY "Build the SDRplay RSP IQ source (needs sdrplay_api)" OFF)

add_executable(tentec-console
  src/app/main.cpp
  src/app/MainWindow.cpp        src/app/MainWindow.h
  src/app/Bands.h
  src/radio/RadioController.h
  src/radio/SerialPort.cpp      src/radio/SerialPort.h
  src/radio/TenTecOrion.cpp     src/radio/TenTecOrion.h
  src/radio/TenTecOmni7.cpp     src/radio/TenTecOmni7.h
  src/net/RigctldServer.cpp     src/net/RigctldServer.h
  src/net/SpotClient.cpp        src/net/SpotClient.h
  src/ui/PanadapterWidget.cpp   src/ui/PanadapterWidget.h
  src/ui/DisplayPanel.cpp       src/ui/DisplayPanel.h
  src/ui/RoutingPanel.cpp       src/ui/RoutingPanel.h
  src/ui/AudioPanel.cpp         src/ui/AudioPanel.h
  src/ui/SMeterWidget.cpp       src/ui/SMeterWidget.h
  src/ui/ControlPanel.cpp       src/ui/ControlPanel.h
  src/ui/FrequencyDisplay.cpp   src/ui/FrequencyDisplay.h
  src/ui/TxBar.cpp              src/ui/TxBar.h
  src/dsp/Fft.cpp               src/dsp/Fft.h
  src/dsp/SpectrumComputer.cpp  src/dsp/SpectrumComputer.h
  src/sdr/SdrSource.h
  resources/resources.qrc
)
target_include_directories(tentec-console PRIVATE src)
target_link_libraries(tentec-console PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

if(BUILD_SDRPLAY)
  target_sources(tentec-console PRIVATE src/sdr/SdrPlaySource.cpp src/sdr/SdrPlaySource.h)
  target_compile_definitions(tentec-console PRIVATE HAVE_SDRPLAY)
  target_link_libraries(tentec-console PRIVATE sdrplay_api)
  message(STATUS "SDRplay RSP source: ENABLED")

  # Headless streaming smoke test.
  add_executable(sdr_probe tools/sdr_probe.cpp src/sdr/SdrPlaySource.cpp)
  target_include_directories(sdr_probe PRIVATE src)
  target_link_libraries(sdr_probe PRIVATE sdrplay_api)
else()
  message(STATUS "SDRplay RSP source: disabled (-DBUILD_SDRPLAY=ON to enable)")
endif()
