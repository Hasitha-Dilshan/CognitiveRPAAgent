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
find_package(OpenCV REQUIRED COMPONENTS core highgui imgproc calib3d features2d)

# Use pkg-config for Tesseract and Leptonica
find_package(PkgConfig REQUIRED)
pkg_check_modules(Tesseract REQUIRED tesseract)
pkg_check_modules(Leptonica REQUIRED lept)

find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# FetchContent for Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    SYSTEM
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )

    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )

    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
endif()

# ---------------------------------------------------------------------------
# Core project sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_SOURCE_DIR}/src/perception/*.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/*.cpp
    ${CMAKE_SOURCE_DIR}/src/action/*.cpp
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
    ${OpenCV_INCLUDE_DIRS}
    ${Tesseract_INCLUDE_DIRS}
    ${Leptonica_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui
    nlohmann_json::nlohmann_json
    opencv_core opencv_highgui opencv_imgproc opencv_calib3d opencv_features2d
    ${Tesseract_LIBRARIES}
    ${Leptonica_LIBRARIES}
    Threads::Threads
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
        Xtst
        dl
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_LINUX)
endif()

# ---------------------------------------------------------------------------
# Compiler optimization flags
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /O2)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O3)
endif()

# ---------------------------------------------------------------------------
# Post-build: copy runtime assets next to the executable
# ---------------------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/data
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
    COMMENT "Copying data/ and assets/ next to the built executable"
)