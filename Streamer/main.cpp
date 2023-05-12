// TODO fix the pixel size and offset slider crashing the app
#include <GL/glew.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <chrono>
#include <thread>
#include <opencv2/opencv.hpp>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <lib/serialib.h>
#include <stdio.h>

#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

static void glfw_error_callback(int error, const char *description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// TODO idk if this actually works as I don't use windows
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
void getSize(int &Width, int &Height){
    Display *display = XOpenDisplay(nullptr);
    Window root = DefaultRootWindow(display);

    XWindowAttributes attributes = {0};
    XGetWindowAttributes(display, root, &attributes);

    Width = attributes.width;
    Height = attributes.height;
}
#endif

uint monitor_width = 0;
uint monitor_height = 0;

uint x_disp_size = 320;
uint y_disp_size = 240;

int x_offset = 0;
int y_offset = 0;

int global_x_step = 1;
int global_y_step = 1;

bool entireDisp = false;

int threshHigh = 100;
int threshMid = 50;

char selected_port[24] = "Select port\0";

char magic_symbol = 'A';

cv::Mat img1;                       // the first frame buffer
bool changeFb = false;              // flag to indicate that the frame buffer has changed
int scWidth = 0;                    // the width of the screenshot
int scHeight = 0;                   // the height of the screenshot

std::mutex scFbChangeMutex;         // mutex for the flag changeFb

float frameRate = 0.0;    // the target frame rate
double deltaScTime = 0.0; // time between two screenshots

void ScreenshotThread()
{
    int Width = 0;
    int Height = 0;
    int Bpp = 0;
    uint size_x = 0;
    uint size_y = 0;
    std::vector<std::uint8_t> Pixels;
    cv::Mat buff;
    while (true)
    {
        // here we just take a screenshot and convert it to a cv::Mat
        auto start = std::chrono::system_clock::now();
        ImageFromDisplay(Pixels, Width, Height, Bpp);
        buff = cv::Mat(Height, Width, Bpp > 24 ? CV_8UC4 : CV_8UC3, &Pixels[0]);
        scWidth = Width;
        scHeight = Height;
        img1 = buff.clone();
        scFbChangeMutex.lock();
        changeFb = true;
        scFbChangeMutex.unlock();
        auto end = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        // calculate the required delay for to hit the target frame rate
        float delay = 1000.0 / frameRate - elapsed_seconds.count() * 1000;
        if (delay > 0){
            std::this_thread::sleep_for(std::chrono::milliseconds((int)delay));
        }
        end = std::chrono::system_clock::now();
        elapsed_seconds = end - start;
        deltaScTime = elapsed_seconds.count() * 1000;
    }
}


std::vector<uint8_t> data_buffer;   // the raw binary frame buffer that will be sent to the display
std::vector<uint8_t> target_buffer; // the buffer containing the image data that will be converted to raw binary data

std::mutex scTbMutex;               // mutex for the preview
std::mutex newImgMutex;             // mutex for the new image flag
std::mutex newDataMutex;            // mutex for the new data flag

cv::Mat img2;
bool newImg = false;
bool newData = false;

void ComputeThread(){
    uint size_x = 0;
    uint size_y = 0;
    int Width = 0;
    int Height = 0;
    int stride = 0;
    int x_step = 0;
    int y_step = 0;
    uint offset_x = 0;
    uint offset_y = 0;
    cv::Mat buff;
    while(true){
        bool changed = false;
        scFbChangeMutex.lock();
        changed = changeFb;
        changeFb = false;
        scFbChangeMutex.unlock();
        if(changed){
            size_x = x_disp_size;
            size_y = y_disp_size;
            Width = scWidth;
            Height = scHeight;
            buff = img1.clone();
            offset_x = x_offset;
            offset_y = y_offset;
            if(entireDisp){
                x_step = Width / size_x;
                y_step = Height / size_y;
            }else{
                x_step = global_x_step;
                y_step = global_y_step;
                if(x_step > Width / size_x){
                    x_step = Width / size_x;
                }
                if(y_step > Height / size_y){
                    y_step = Height / size_y;
                }
            }
            // get the length of the buffer
            if(Height != 0){
                stride = (buff.total() * buff.elemSize()) / Height;
            }else{
                continue;
            }
            uint32_t index = 0;
            scTbMutex.lock();
            // define the target buffer size
            target_buffer.resize(size_x * size_y);
            // iterate over each pixel of target display
            for (int y = 0; y < size_y; y++)
            {
                for (int x = 0; x < size_x; x++)
                {
                    int big_x = x * x_step + offset_x;
                    int big_y = y * y_step + offset_y;
                    // if the pixel is out of bounds, set it to black
                    if(big_x > Width){
                        big_x = Width;
                    }
                    if(big_y > Height){
                        big_y = Height;
                    }

                    // get the pixel value
                    cv::Vec4b pixel = buff.at<cv::Vec4b>(big_y, big_x);
                    // pixel[0] B
                    // pixel[1] G
                    // pixel[2] R
                    // calculate and store the average value
                    uint8_t avg = (pixel[0] + pixel[1] + pixel[2]) / 3;
                    target_buffer[index] = avg;
                    // set the pixel value
                    index++;
                }
            }

            // now we will compute the raw binary data
            data_buffer.resize(size_x * size_y / 4);    // 4 pixels per byte
            for(int i = 0; i < data_buffer.size(); i++){
                // threshold represents the maximum brightness value
                // so if a pixel is > threshold, we set it to 11
                // if it's threshold/2, we set it to 10
                // if it's 0, we set it to 00
                // byte structure:
                // 00 00 00 00 - 1 byte
                // each collection of 2 bits is a brightness value
                // 00 = black
                // 01 = 1 - 50% in our case
                // 10 || 11 = 2 - 100% in our case
                uchar data[4] = {0};
                // get the pixel values
                for(int j = 0; j < 4; j++){
                    uchar buff = target_buffer[i * 4 + j];
                    if(buff > threshHigh){
                        data[j] = 2;
                    }else if(buff > threshMid){
                        data[j] = 1;
                    }else{
                        data[j] = 0;
                    } 
                    target_buffer[i * 4 + j] = data[j] * 127;
                }
                uchar byte = 0;
                for(int j = 0; j < 4; j++){
                    byte |= data[j] << (6 - j * 2);
                }        
                data_buffer[i] = byte;
            }
            // convert the target buffer into a black and white CV2 image
            img2 = cv::Mat(size_y, size_x, CV_8UC1, &target_buffer[0]);
            scTbMutex.unlock();
            newDataMutex.lock();
            newData = true;
            newDataMutex.unlock();
            newImgMutex.lock();
            newImg = true;
            newImgMutex.unlock();
        }
    }

}

double deltaSerialTime = 0.0; // time between two serial frames
void SerialThread(){
    serialib device;
    auto lastStart = std::chrono::system_clock::now();
    while(true){
        if(device.openDevice(selected_port, 115200) == 1){
            bool new_data = false;
            std::vector<uint8_t> data;
            while(true){
                newDataMutex.lock();
                new_data = newData;
                newData = false;
                newDataMutex.unlock();
                if(new_data){
                    scTbMutex.lock();
                    data = data_buffer;
                    scTbMutex.unlock();
                    char read = 0;
                    device.readChar(&read);
                    if(read == magic_symbol){
                        device.flushReceiver();
                        device.writeBytes(data.data(), data.size());
                        auto end = std::chrono::system_clock::now();
                        std::chrono::duration<double> elapsed_seconds = end - lastStart;
                        deltaSerialTime = elapsed_seconds.count() * 1000;
                        lastStart = std::chrono::system_clock::now();
                    }
                    device.flushReceiver();
                }
            }
        }else{
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
}

uint window_width = 800;
uint window_height = 600;
// GUI thread
GLuint textureID;

void guiThread(){
    cv::Mat img;
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
    GLFWwindow *window = glfwCreateWindow(window_width, window_height, "EL Streamer", NULL, NULL);
    if (window == NULL)
        return;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync
    // set the smallest allowed window size
    glfwSetWindowSizeLimits(window, 400, 300, GLFW_DONT_CARE, GLFW_DONT_CARE);

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
    ImVec4 clear_color = ImVec4(1.0f, 1.0f, 0.0f, 1.00f);

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
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // get glfw window size
        int width, height;
        glfwGetWindowSize(window, &width, &height);
        window_width = width;
        window_height = height;
        if (!first_start)
        {
            // set our flags
            ImGuiWindowFlags window_flags = 0;
            window_flags |= ImGuiWindowFlags_NoTitleBar;
            window_flags |= ImGuiWindowFlags_NoResize;
            window_flags |= ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoScrollbar;

            ImGui::Begin("EL STREAMER", NULL, window_flags);
            ImGui::SetWindowSize(ImVec2(window_width, window_height));
            ImGui::SetWindowPos(ImVec2(0, 0));
            ImGui::ColorEdit3("Color", (float *)&clear_color);
            ImGui::SliderInt("High Threshold", &threshHigh, 0, 255);
            ImGui::SliderInt("Mid Threshold", &threshMid, 0, 255);
            
            if(!entireDisp){
                // calculate the maximum offset for the sliders, taking the step size into account
                int max_x_offset = (monitor_width - x_disp_size * global_x_step);
                int max_y_offset = (monitor_height - y_disp_size * global_y_step);
                ImGui::SliderInt("Offset X", &x_offset, 0, max_x_offset);
                ImGui::SliderInt("Offset Y", &y_offset, 0, max_y_offset);
                ImGui::SliderInt("Step X", &global_x_step, 1, monitor_width / x_disp_size);
                ImGui::SliderInt("Step Y", &global_y_step, 1, monitor_height / y_disp_size);
                if(x_offset > max_x_offset){
                    x_offset = max_x_offset;
                }
                if(y_offset > max_y_offset){
                    y_offset = max_y_offset;
                }
            }

            float frameTime = 1000.0f / io.Framerate;
            frameRate = io.Framerate;
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
            scTime = deltaScTime;
            double serialTime = 0;
            serialTime = deltaSerialTime;
            // show the frame time and FPS
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS); Screenshot thread: %.3f ms/frame; Serial thread: %.3f ms", frameTime, io.Framerate, scTime, serialTime);
            ImGui::PlotLines("##Frame Time Graph", arrFrameTimes, IM_ARRAYSIZE(arrFrameTimes), 0, NULL, 0, maxFrameTime, ImVec2(window_width, 80));

            bool bufferChanged = false;
            newImgMutex.lock();
            bufferChanged = newImg;
            newImg = false;
            newImgMutex.unlock();
            if (bufferChanged)
            {
                scTbMutex.lock();
                img = img2.clone();
                scTbMutex.unlock();
                cv::cvtColor(img, img, cv::COLOR_GRAY2RGB);
                // multiply each pixel by respective clear_color value
                for (int i = 0; i < img.rows; i++)
                {
                    for (int j = 0; j < img.cols; j++)
                    {
                        img.at<cv::Vec3b>(i, j)[0] = img.at<cv::Vec3b>(i, j)[0] * clear_color.x;
                        img.at<cv::Vec3b>(i, j)[1] = img.at<cv::Vec3b>(i, j)[1] * clear_color.y;
                        img.at<cv::Vec3b>(i, j)[2] = img.at<cv::Vec3b>(i, j)[2] * clear_color.z;
                    }
                }
                // create a texture from the image
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, img.cols, img.rows, 0, GL_RGB, GL_UNSIGNED_BYTE, img.ptr());
            }
            // display the image
            ImGui::Image((void *)(intptr_t)textureID, ImVec2(img.cols, img.rows));
            ImGui::End();
            // se the background color to white
            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        }else{
            first_start = false;
            ImGuiWindowFlags window_flags = 0;
            window_flags |= ImGuiWindowFlags_NoTitleBar;
            window_flags |= ImGuiWindowFlags_NoResize;
            window_flags |= ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoScrollbar;

            ImGui::Begin("EL STREAMER", NULL, window_flags);

            // define the size of the subwindow
            ImGui::SetWindowSize(ImVec2(300, 200));
            if(window_width > 300 && window_height > 200)
                ImGui::SetWindowPos(ImVec2((window_width/2)-150, (window_height/2)-100));
            ImGui::Text("Please Select the display size");
            static int x_size = x_disp_size;
            static int y_size = y_disp_size;
            ImGui::SliderInt("X Size", &x_size, 1, monitor_width);
            ImGui::SliderInt("Y Size", &y_size, 1, monitor_height);
            // the selected size needs to be a multiple of 4
            if(x_size % 4 != 0){
                x_size = x_size - (x_size % 4);
            }
            if(y_size % 4 != 0){
                y_size = y_size - (y_size % 4);
            }
            x_disp_size = x_size;
            y_disp_size = y_size;
            ImGui::Text("Should the stream be fullscreen?");
            ImGui::Checkbox("Fullscreen", &entireDisp);

            static char* items[99] = {0};
            static int item_current = -1; // If the selection isn't within 0..count, Combo won't display a preview
            ImGui::PushItemWidth(150);
            if (ImGui::BeginCombo("##PortSelector", selected_port))
            {
                for (int n = 0; n < IM_ARRAYSIZE(items); n++)
                {
                    const bool is_selected = (item_current == n);
                    if(items[n] != NULL){
                        if (ImGui::Selectable(items[n], is_selected)){
                            item_current = n;
                            strcpy(selected_port, items[n]);
                        }
                        // Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
                        if (is_selected){
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();

            static bool scan = true;
            char device_name[99][24];
            if(scan){
                scan = false;
                serialib device;
                for (int i=0;i<98;i++)
                {
                    // Prepare the port name (Windows)
                    #if defined (_WIN32) || defined( _WIN64)
                        sprintf (device_name[i],"\\\\.\\COM%d",i+1);
                    #endif
                    // Prepare the port name (Linux)
                    #ifdef __linux__
                        sprintf (device_name[i],"/dev/ttyACM%d",i);
                    #endif
                    // try to connect to the device
                    if (device.openDevice(device_name[i],115200)==1)
                    {
                        // set the pointer to the array
                        items[i] = device_name[i];
                        // Close the device before testing the next port
                        device.closeDevice();
                    }else{
                        items[i] = NULL;
                    }
                }
            }
            scan = ImGui::Button("Scan", ImVec2(100, 20));
            // center the button
            ImGui::SetCursorPosX((300-100)/2);
            ImGui::SetCursorPosY(150);
            first_start = !ImGui::Button("Apply", ImVec2(100, 20));
            ImGui::End();
        }
        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
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
    int Width = 0;
    int Height = 0;
    getSize(Width, Height);
    monitor_width = Width;
    monitor_height = Height;

    // start the screenshot thread
    std::thread screenshotThread(ScreenshotThread);
    // start the compute thread
    std::thread computeThread(ComputeThread);
    // start the serial stream thread
    std::thread serialThread(SerialThread);
    // start the imgui thread
    std::thread imguiThread(guiThread);

    // wait for the threads to finish (which they never do unless the program is closed)
    imguiThread.join();
    return 0;
}
