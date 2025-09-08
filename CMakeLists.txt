cmake_minimum_required(VERSION 3.24)
project(BTC_OP_V2 LANGUAGES CXX)

# ---- C++/Qt 基本 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt の場所（あなたの環境に合わせて）
set(CMAKE_PREFIX_PATH "F:/Qt/6.8.1/msvc2022_64" CACHE STRING "")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Charts)

qt_standard_project_setup()

# ---- ソース列挙（存在するものだけ）----
set(APP_SOURCES
  main.cpp
  App.cpp App.h
  MainWindow.cpp MainWindow.h MainWindow.ui
  CurvesChartPane.cpp CurvesChartPane.h
  HttpClient.cpp HttpClient.h
  WebSocketClient.cpp WebSocketClient.h
  nbbo_store.cpp nbbo_store.h
  iv_greeks.cpp iv_greeks.h
  oi_store.cpp oi_store.h
  leg_linker.cpp leg_linker.h
  pnl_metrics.cpp pnl_metrics.h
  greeks_aggregator.cpp greeks_aggregator.h
  pin_map.cpp pin_map.h
  curves.cpp curves.h
  trade_types.h op_types.h
  ux_support.cpp ux_support.h
  engine_helpers.h
)

qt_add_executable(${PROJECT_NAME} ${APP_SOURCES})

# MSVC 向けフラグ（警告多いなら /W3 に下げてもOK）
if (MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /permissive- /W4 /Zc:__cplusplus)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _HAS_STD_BYTE=1)
endif()

target_link_libraries(${PROJECT_NAME}
  PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Charts
)

qt_finalize_executable(${PROJECT_NAME})
