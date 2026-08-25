cmake_minimum_required(VERSION 3.20)
project(CognitiveRPAAgent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build type default
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(OpenCV REQUIRED)
find_package(Tesseract REQUIRED)

# nlohmann/json (header-only)
add_subdirectory(third_party/json)

# ---------------------------------------------------------------------------
# Dear ImGui (compiled as a static lib, backend chosen per-OS)
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

find_package(glfw3 REQUIRED)
target_link_libraries(imgui PUBLIC glfw)

# ---------------------------------------------------------------------------
# Core project sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_SOURCE_DIR}/src/perception/*.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/*.cpp
    ${CMAKE_SOURCE_DIR}/src/action/*.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/*.cpp
)

# Platform-specific source selection
if(WIN32)
    list(APPEND CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/platform/WindowsPlatform.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/platform/LinuxPlatform.cpp)
endif()

add_executable(${PROJECT_NAME}
    src/main.cpp
    ${CORE_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/core
    ${CMAKE_SOURCE_DIR}/include/perception
    ${CMAKE_SOURCE_DIR}/include/memory
    ${CMAKE_SOURCE_DIR}/include/action
    ${CMAKE_SOURCE_DIR}/include/platform
    ${CMAKE_SOURCE_DIR}/include/ui
    ${OpenCV_INCLUDE_DIRS}
    ${Tesseract_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui
    ${OpenCV_LIBS}
    Tesseract::libtesseract
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# OS-specific linking
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        opengl32
        user32
        gdi32
        gdiplus
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_WINDOWS)
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${X11_LIBRARIES}
        Xtst          # For synthetic input events (XTestFakeMotionEvent etc.)
        GL
        dl
        pthread
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_LINUX)
endif()

# ---------------------------------------------------------------------------
# Compiler hardening / optimization flags
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /O2)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O3 -pthread)
endif()