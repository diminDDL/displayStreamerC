// Dear ImGui: standalone example application for GLFW + OpenGL 3, using programmable pipeline
// (GLFW is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation, etc.)
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs
#include <GL/glew.h>
// #define _GLFW_BUILD_DLL
#include "imgui.h"
// #include "imgui_impl_opengl3_loader.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// include time for delay
#include <chrono>
#include <thread>

// include opencv and other stuff for screenshot
#include <opencv2/opencv.hpp>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace cv;

// threading
#include <thread>
#include <mutex>

#include <stdio.h>
#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
// To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
// Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#include <Windows.h>
#define WIN_ENABLED
#endif

// This example can also compile and run with Emscripten! See 'Makefile.emscripten' for details.
#ifdef __EMSCRIPTEN__
#include "../libs/emscripten/emscripten_mainloop_stub.h"
#endif

static void glfw_error_callback(int error, const char *description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

#ifdef WIN_ENABLED
void ImageFromDisplay(std::vector<uint8_t> &Pixels, int &Width, int &Height, int &BitsPerPixel)
{
    HWND hDesktop = GetDesktopWindow();
    HDC hDC = GetDC(hDesktop);
    HDC hMemDC = CreateCompatibleDC(hDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hDC, Width, Height);
    SelectObject(hMemDC, hBitmap);
    BitBlt(hMemDC, 0, 0, Width, Height, hDC, 0, 0, SRCCOPY);
    BITMAPINFOHEADER bi;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = Width;
    bi.biHeight = -Height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = 0;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;
    GetDIBits(hDC, hBitmap, 0, Height, &Pixels[0], (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(hDesktop, hDC);
}
#else
void ImageFromDisplay(std::vector<uint8_t> &Pixels, int &Width, int &Height, int &BitsPerPixel)
{
    Display *display = XOpenDisplay(nullptr);
    Window root = DefaultRootWindow(display);

    XWindowAttributes attributes = {0};
    XGetWindowAttributes(display, root, &attributes);

    Width = attributes.width;
    Height = attributes.height;

    XImage *img = XGetImage(display, root, 0, 0, Width, Height, AllPlanes, ZPixmap);
    BitsPerPixel = img->bits_per_pixel;
    Pixels.resize(Width * Height * 4);

    memcpy(&Pixels[0], img->data, Pixels.size());

    XDestroyImage(img);
    XCloseDisplay(display);
}
#endif

uint monitor_width = 0;
uint monitor_height = 0;

uint x_disp_size = 320;
uint y_disp_size = 240;

// we use an interleaved frame buffer
Mat img1;                           // the first frame buffer
std::vector<uint8_t> data_buffer;   // the raw binary frame buffer that will be sent to the display
std::vector<uint8_t> target_buffer; // the raw binary frame buffer that will be sent to the display
bool changeFb = false;              // flag to indicate that the frame buffer has changed
std::mutex scFbMutex;               // mutex for the frame buffer
std::mutex scDtMutex;               // mutex for the time between two screenshots
std::mutex scFbChangeMutex;         // mutex for the flag changeFb
std::mutex gtFrMutex;               // mutex for the frame rate
std::mutex dispSizeMutex;           // mutex for the display size
std::mutex scTbMutex;               // mutex for the target buffer

float frameRate = 0.0;    // the target frame rate
double deltaScTime = 0.0; // time between two screenshots

void takeScreenshot()
{

    int Width = 0;
    int Height = 0;
    int Bpp = 0;
    uint size_x = 0;
    uint size_y = 0;
    std::vector<std::uint8_t> Pixels;
    Mat buff;
    while (true)
    {
        auto start = std::chrono::system_clock::now();

        ImageFromDisplay(Pixels, Width, Height, Bpp);
        buff = Mat(Height, Width, Bpp > 24 ? CV_8UC4 : CV_8UC3, &Pixels[0]);

        dispSizeMutex.lock();
        size_x = x_disp_size;
        size_y = y_disp_size;
        dispSizeMutex.unlock();

        int x_step = Width / size_x;
        int y_step = Height / size_y;
        // get the length of the buffer
        int stride = (buff.total() * buff.elemSize()) / Height;

        // uint8_t target_buffer [size_x * size_y];
        uint32_t index = 0;
        scTbMutex.lock();
        // define the target buffer size
        target_buffer.resize(size_x * size_y);
        // clear the target buffer
        target_buffer.clear();
        // iterate over each pixel of target display
        for (int y = 0; y < size_y; y++)
        {
            for (int x = 0; x < size_x; x++)
            {
                int big_x = x * x_step;
                int big_y = y * y_step;
                // get the pixel value
                Vec4b pixel = buff.at<Vec4b>(big_y, big_x);
                pixel[0] = 0; // B
                pixel[1] = 0; // G
                pixel[2] = 0; // R
                // calculate and store the average value
                uint8_t avg = (pixel[0] + pixel[1] + pixel[2]) / 3;
                target_buffer[index] = avg;
                // set the pixel value
                index++;
            }
        }
        scTbMutex.unlock();

        // crop buff to X_DISP x Y_DISP and store the result in bwCropped
        // buff = buff(Rect(0, 0, size_x, size_y));
        // convert bwCropped to black and white
        // cvtColor(buff, buff, COLOR_BGR2GRAY);

        scFbChangeMutex.lock();
        changeFb = true;
        scFbChangeMutex.unlock();

        scFbMutex.lock();
        img1 = buff.clone();
        scFbMutex.unlock();

        auto end = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        gtFrMutex.lock();
        // calculate the required delay for to hit the target frame rate
        float delay = 1000.0 / frameRate - elapsed_seconds.count() * 1000;
        gtFrMutex.unlock();
        if (delay > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds((int)delay));
        }
        end = std::chrono::system_clock::now();
        elapsed_seconds = end - start;
        scDtMutex.lock();
        deltaScTime = elapsed_seconds.count() * 1000;
        scDtMutex.unlock();
    }
}

// GUI thread
GLuint textureID;
void guiThread()
{

    Mat img;
    static bool first_start = true;

    // a FIFO with the last 250 frametimes
    std::deque<double> frametimes;

    std::vector<std::uint8_t> Pixels;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return;

        // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100
    const char *glsl_version = "#version 100";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
    // GL 3.2 + GLSL 150
    const char *glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);           // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char *glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);           // 3.0+ only
#endif

    // Create window with graphics context
    GLFWwindow *window = glfwCreateWindow(1280, 720, "Dear ImGui GLFW+OpenGL3 example", NULL, NULL);
    if (window == NULL)
        return;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    glewInit();
    // Our state
    bool show_demo_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // Main loop
#ifdef __EMSCRIPTEN__
    // For an Emscripten build we are disabling file-system access, so let's not attempt to do a fopen() of the imgui.ini file.
    // You may manually call LoadIniSettingsFromMemory() to load settings from your own storage.
    io.IniFilename = NULL;
    EMSCRIPTEN_MAINLOOP_BEGIN
#else
    while (!glfwWindowShouldClose(window))
#endif
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.

        // static float f = 0.0f;
        // static int counter = 0;
        if (!first_start)
        {
            ImGui::Begin("EL STREAMER");                       // Create a window called "Hello, world!" and append into it.
            ImGui::SetWindowSize(ImVec2(800, 600));
            ImGui::Text("Welcome to EL Streamer");             // Display some text (you can use a format strings too)
            ImGui::Checkbox("Demo Window", &show_demo_window); // Edit bools storing our window open/close state
            ImGui::ColorEdit3("clear color", (float *)&clear_color); // Edit 3 floats representing a color
            // if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
            //     counter++;
            // ImGui::SameLine();
            // ImGui::Text("counter = %d", counter);
            float frameTime = 1000.0f / io.Framerate;
            gtFrMutex.lock();
            frameRate = io.Framerate;
            gtFrMutex.unlock();
            // add frame time to the vector and if it's too big, remove the first element
            frametimes.push_back(frameTime);
            if (frametimes.size() > 250)
            {
                frametimes.erase(frametimes.begin());
            }
            float arrFrameTimes[250] = {0};
            for (long unsigned int i = 0; i < frametimes.size(); i++)
            {
                arrFrameTimes[i] = frametimes[i];
            }
            float maxFrameTime = frameTime + 1;
            for (long unsigned int i = 0; i < frametimes.size(); i++)
            {
                if (frametimes[i] > maxFrameTime)
                {
                    maxFrameTime = frametimes[i];
                }
            }
            double scTime = 0;
            scDtMutex.lock();
            scTime = deltaScTime;
            scDtMutex.unlock();
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS); Screenshot thread: %.3f ms/frame", frameTime, io.Framerate, scTime);
            ImGui::PlotLines("Frame Times", arrFrameTimes, IM_ARRAYSIZE(arrFrameTimes), 0, NULL, 0, maxFrameTime, ImVec2(0, 80));

            // ImageFromDisplay(Pixels, Width, Height, Bpp);
            // img1 = Mat(Height, Width, Bpp > 24 ? CV_8UC4 : CV_8UC3, &Pixels[0]);

            bool bufferChanged = false;
            scFbChangeMutex.lock();
            bufferChanged = changeFb;
            changeFb = false;
            scFbChangeMutex.unlock();

            if (bufferChanged)
            {
                scFbMutex.lock();
                img = img1.clone();
                scFbMutex.unlock();
                cvtColor(img, img, cv::COLOR_BGR2RGB);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, img.cols, img.rows, 0, GL_RGB, GL_UNSIGNED_BYTE, img.ptr());
                // glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, x_size, y_size, 0, GL_RGB, GL_UNSIGNED_BYTE, img.ptr());
            }
            ImGui::Image((void *)(intptr_t)textureID, ImVec2(img.cols / 2 /**((float)x_size/img.cols)*/, img.rows / 2 /**((float)y_size/img.rows)*/));
            ImGui::End();
        }else{
            first_start = false;
            ImGui::Begin("EL STREAMER");
            // define the size of the subwindow
            ImGui::SetWindowSize(ImVec2(300, 200));
            ImGui::SetWindowPos(ImVec2(0, 0));
            ImGui::Text("Please Select the display size");
            static int x_size = x_disp_size;
            static int y_size = y_disp_size;
            ImGui::SliderInt("X Size", &x_size, 1, monitor_width);
            ImGui::SliderInt("Y Size", &y_size, 1, monitor_height);
            if (x_size > monitor_width)
            {
                x_size = monitor_width;
            }
            if (y_size > monitor_height)
            {
                y_size = monitor_height;
            }
            dispSizeMutex.lock();
            x_disp_size = x_size;
            y_disp_size = y_size;
            dispSizeMutex.unlock();
            first_start = !ImGui::Button("Apply", ImVec2(100, 20));
            ImGui::End();

        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        // wait 10ms blocking
        // std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_MAINLOOP_END;
#endif

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return;
}

int main(int, char **)
{

    /////////////////////////  OPENCV SCREENSHOT  /////////////////////////
    int Width = 0;
    int Height = 0;
    int Bpp = 0;
    std::vector<std::uint8_t> Pixels;

    ImageFromDisplay(Pixels, Width, Height, Bpp);

    monitor_width = Width;
    monitor_height = Height;

    img1 = Mat(Height, Width, Bpp > 24 ? CV_8UC4 : CV_8UC3, &Pixels[0]); // Mat(Size(Height, Width), Bpp > 24 ? CV_8UC4 : CV_8UC3, &Pixels[0]);
    // namedWindow("WindowTitle", WINDOW_AUTOSIZE);
    // imshow("Display window", img1);
    // waitKey(0);

    // cv::cvtColor(img1, img, cv::COLOR_BGR2RGB);
    // // create a texture

    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    // return 0;
    /////////////////////////  OPENCV SCREENSHOT END  /////////////////////////

    // start the screenshot thread
    std::thread screenshotThread(takeScreenshot);

    // start the imgui thread
    std::thread imguiThread(guiThread);

    // wait for the threads to finish
    // screenshotThread.join();
    imguiThread.join();
}
