# Cognitive RPA Agent

A cross-platform (Windows & Linux) Cognitive Robotic Process Automation (RPA) Agent built with C++20. The agent utilizes OpenCV for computer vision, Tesseract for OCR, and features an interactive control panel built with Dear ImGui.

## Features
* **Visual Perception:** Template matching and UI element detection (OpenCV).
* **Text Recognition:** Fallback OCR capabilities using Tesseract.
* **Autonomous Action:** Simulates human-like mouse and keyboard inputs across OS platforms.
* **Self-Learning Memory:** Records successful UI element locations to `memory.json` for future speed improvements.

## Build Instructions (CMake)
The build system automatically fetches dependencies (Dear ImGui, nlohmann/json) via `FetchContent`.

```bash
cmake -B build
cmake --build build
```
