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
# Fetch Dependencies (FetchContent)
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.1
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(imgui nlohmann_json)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(OpenCV REQUIRED)

# Use find_package in CONFIG mode for Tesseract/Leptonica on Windows,
# otherwise fall back to PkgConfig.
find_package(Tesseract QUIET)
if (NOT Tesseract_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(Tesseract REQUIRED tesseract)
endif()

find_package(Leptonica QUIET)
if (NOT Leptonica_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(Leptonica REQUIRED lept)
endif()

find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Dear ImGui (compiled as a static lib)
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${imgui_SOURCE_DIR})

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

target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

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
)

if (Tesseract_FOUND AND NOT "${Tesseract_INCLUDE_DIRS}" STREQUAL "")
    target_include_directories(${PROJECT_NAME} PRIVATE ${Tesseract_INCLUDE_DIRS})
endif()

if (Leptonica_FOUND AND NOT "${Leptonica_INCLUDE_DIRS}" STREQUAL "")
    target_include_directories(${PROJECT_NAME} PRIVATE ${Leptonica_INCLUDE_DIRS})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui
    nlohmann_json::nlohmann_json
    ${OpenCV_LIBS}
    Threads::Threads
)

if (TARGET Tesseract::tesseract)
    target_link_libraries(${PROJECT_NAME} PRIVATE Tesseract::tesseract)
elseif(Tesseract_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${Tesseract_LIBRARIES})
endif()

if (TARGET Leptonica::leptonica)
    target_link_libraries(${PROJECT_NAME} PRIVATE Leptonica::leptonica)
elseif(Leptonica_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${Leptonica_LIBRARIES})
endif()

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
if (EXISTS ${CMAKE_SOURCE_DIR}/data)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/data
        COMMENT "Copying data/ next to the built executable"
    )
endif()

if (EXISTS ${CMAKE_SOURCE_DIR}/assets)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
        COMMENT "Copying assets/ next to the built executable"
    )
endif()