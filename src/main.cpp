#include <memory>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "core/CoreEngine.hpp"
#include "core/SecurityLayer.hpp"

#ifdef PLATFORM_WINDOWS
    #include "platform/WindowsPlatform.hpp"
#elif defined(PLATFORM_LINUX)
    #include "platform/LinuxPlatform.hpp"
#endif

namespace {

// ---------------------------------------------------------------------
// Thread-safe log buffer shared between CoreEngine's worker thread and
// the ImGui render thread. A ring buffer (capped size) rather than an
// unbounded vector, since a long-running agent session should never let
// its own UI log slowly consume all available RAM.
// ---------------------------------------------------------------------
class UiLog {
public:
    void push(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
        if (lines_.size() > kMaxLines) {
            lines_.pop_front();
        }
    }

    // Returns a snapshot copy rather than exposing the internal deque
    // directly — keeps all synchronization internal to this class so the
    // render loop never has to reason about locking itself.
    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<std::string>(lines_.begin(), lines_.end());
    }

private:
    static constexpr size_t kMaxLines = 500;
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
};

UiLog g_uiLog;

void logLine(const std::string& msg) {
    g_uiLog.push(msg);
    std::fprintf(stdout, "%s\n", msg.c_str()); // also mirror to console for headless debugging
}

// ---------------------------------------------------------------------
// GLFW error callback — without this, GLFW failures print to stderr with
// no context and are easy to miss amid ImGui's own output. Wiring this
// explicitly makes window/context creation failures immediately visible.
// ---------------------------------------------------------------------
void glfwErrorCallback(int error, const char* description) {
    logLine("[GLFW ERROR " + std::to_string(error) + "] " + description);
}

// ---------------------------------------------------------------------
// Constructs the correct IPlatform implementation for the current OS.
// This is the ONLY place in main.cpp that references a platform-specific
// type — everything else works purely through IPlatform/CoreEngine.
// ---------------------------------------------------------------------
std::unique_ptr<rpa::platform::IPlatform> createPlatform() {
#ifdef PLATFORM_WINDOWS
    return std::make_unique<rpa::platform::WindowsPlatform>();
#elif defined(PLATFORM_LINUX)
    return std::make_unique<rpa::platform::LinuxPlatform>();
#else
    #error "No supported platform defined. Expected PLATFORM_WINDOWS or PLATFORM_LINUX to be set by CMake."
#endif
}

// ---------------------------------------------------------------------
// Human-readable status derived from CoreEngine's atomic state, computed
// fresh each frame rather than cached — this is cheap (two atomic loads)
// and guarantees the UI can never display stale status.
// ---------------------------------------------------------------------
struct AgentStatus {
    bool running = false;
    bool halted = false;
    std::string label;
    ImVec4 color;
};

AgentStatus computeStatus(bool isRunning, bool isHalted) {
    AgentStatus status;
    status.running = isRunning;
    status.halted = isHalted;

    if (isHalted) {
        status.label = "HALTED (Security Kill-Switch Triggered)";
        status.color = ImVec4(0.90f, 0.25f, 0.25f, 1.0f); // red
    } else if (isRunning) {
        status.label = "RUNNING";
        status.color = ImVec4(0.25f, 0.85f, 0.35f, 1.0f); // green
    } else {
        status.label = "STOPPED";
        status.color = ImVec4(0.75f, 0.75f, 0.75f, 1.0f); // gray
    }
    return status;
}

} // namespace

int main(int, char**) {
    // -----------------------------------------------------------------
    // 1. GLFW + OpenGL context setup
    // -----------------------------------------------------------------
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Fatal: glfwInit() failed\n");
        return 1;
    }

    // OpenGL 3.3 core profile — sufficient for ImGui's rendering needs
    // and broadly supported across both Windows and Linux GPU drivers,
    // including software/VM-backed drivers that a low-end target machine
    // might be running.
    const char* glslVersion = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(
        900, 600, "Cognitive RPA Agent - Control Panel", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Fatal: glfwCreateWindow() failed\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync — this is a control panel, not a game;
                          // no reason to burn CPU/GPU rendering faster than the display

    // -----------------------------------------------------------------
    // 2. Dear ImGui setup
    // -----------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        std::fprintf(stderr, "Fatal: ImGui_ImplGlfw_InitForOpenGL() failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        std::fprintf(stderr, "Fatal: ImGui_ImplOpenGL3_Init() failed\n");
        ImGui_ImplGlfw_Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // -----------------------------------------------------------------
    // 3. CoreEngine construction and initialization
    // -----------------------------------------------------------------
    // Constructed but not yet started — start()/stop() are driven by UI
    // buttons below, not automatically on launch, so the operator always
    // has explicit control before the agent touches mouse/keyboard.
    std::unique_ptr<rpa::core::CoreEngine> engine;
    bool engineReady = false;

    try {
        auto platform = createPlatform();
        engine = std::make_unique<rpa::core::CoreEngine>(std::move(platform));

        const bool initOk = engine->initialize("data/base_skills.json", "data/memory.json");
        if (!initOk) {
            logLine("[INIT FAILED] " + engine->lastInitError());
        } else {
            logLine("[INIT OK] Base skills and memory loaded successfully.");
            engineReady = true;
        }
    } catch (const std::exception& e) {
        // Platform construction itself can throw (e.g. no X server found,
        // XTest unavailable) — this is caught here rather than left to
        // crash the process before any UI can even display the error.
        logLine(std::string("[FATAL] Platform/engine construction failed: ") + e.what());
    }

    if (!engineReady) {
        logLine("[WARNING] Agent is not operational. Check the log above. "
                "The control panel will still run so you can review the error.");
    }

    // -----------------------------------------------------------------
    // 4. Main render/event loop
    // -----------------------------------------------------------------
    char skillInputBuffer[128] = "SaveButton"; // scratch buffer for the "execute skill" text field

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("Cognitive RPA Agent", nullptr,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        // --- Status header -------------------------------------------------
        const bool isRunning = engineReady && engine->isRunning();
        const bool isHalted  = engineReady && engine->isHalted();
        const AgentStatus status = computeStatus(isRunning, isHalted);

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        ImGui::TextColored(status.color, "%s", status.label.c_str());

        ImGui::Separator();

        // --- Controls --------------------------------------------------
        ImGui::BeginDisabled(!engineReady);

        if (ImGui::Button("Start Agent", ImVec2(140, 32))) {
            engine->start();
            logLine("[UI] Start requested.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop Agent", ImVec2(140, 32))) {
            engine->stop();
            logLine("[UI] Stop requested. Memory persisted to data/memory.json.");
        }
        ImGui::SameLine();

        // Halted state requires explicit operator acknowledgement to clear —
        // there is deliberately no "auto-resume" path here. Resetting the
        // security layer without a human confirming the underlying problem
        // (a loop, an out-of-bounds click, etc.) was actually addressed
        // would defeat the entire purpose of the kill-switch.
        ImGui::BeginDisabled(!isHalted);
        if (ImGui::Button("Acknowledge Halt && Reset", ImVec2(220, 32))) {
            engine->stop();
            engine->resetSecurityState();
            logLine("[UI] Operator acknowledged halt. Security state reset. "
                    "Agent is stopped — press Start Agent to resume.");
        }
        ImGui::EndDisabled();

        ImGui::EndDisabled();

        ImGui::Separator();

        // --- Manual skill execution (for testing individual skills) -------
        ImGui::TextUnformatted("Manual Skill Execution");
        ImGui::InputText("Skill Name", skillInputBuffer, sizeof(skillInputBuffer));
        ImGui::SameLine();

        ImGui::BeginDisabled(!engineReady || !isRunning || isHalted);
        if (ImGui::Button("Execute")) {
            const std::string skillName(skillInputBuffer);
            logLine("[UI] Executing skill: " + skillName);
            const bool ok = engine->executeSkill(skillName);
            if (ok) {
                logLine("[SUCCESS] Skill '" + skillName + "' executed.");
            } else {
                logLine("[FAILED] Skill '" + skillName + "': " + engine->lastExecutionError());
            }
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        // --- Log console -----------------------------------------------
        ImGui::TextUnformatted("Activity Log");
        ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : g_uiLog.snapshot()) {
            ImGui::TextUnformatted(line.c_str());
        }
        // Auto-scroll to the latest entry, but only if the user was
        // already at the bottom — otherwise scrolling up to read older
        // entries would get yanked back down on every new log line.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::End();

        // --- Render ------------------------------------------------------
        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // -----------------------------------------------------------------
    // 5. Shutdown
    // -----------------------------------------------------------------
    // Ensure the engine is stopped (and memory persisted) before tearing
    // down the window — relying solely on CoreEngine's destructor here
    // would still work (it calls stop() internally), but doing it
    // explicitly makes the shutdown log entry visible before the window
    // closes rather than firing after the UI is already gone.
    if (engineReady && engine->isRunning()) {
        engine->stop();
        logLine("[SHUTDOWN] Agent stopped, memory persisted.");
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}