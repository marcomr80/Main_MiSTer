#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <unistd.h>
#include <sys/stat.h>

#include "video.h"
#include "shmem.h" 

#define FB_SIZE  (1920*1080)
#define FB_ADDR  (0x20000000 + (32*1024*1024))

static std::string target_image = "";
static bool has_new_request = false;
static std::mutex mtx;
static std::condition_variable cv;
static bool worker_running = false;

static uint32_t* shmem_fb = nullptr;
static int fb_w = 1280;
static int fb_h = 720;

static std::string last_hook_path = "INIT";
static bool fb_is_on = false; 

inline bool file_exists(const std::string& name) {
    struct stat buffer;
    return (stat(name.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
}

// THREAD PINTORA (Totalmente isolada, mexe apenas na RAM)
void cover_worker_thread() {
    std::string current_drawn = "INIT";

    while (true) {
        std::string to_draw;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, []{ return has_new_request; });
            to_draw = target_image;
            has_new_request = false;
        }

        if (to_draw != current_drawn) {
            if (to_draw == "") {
                if (shmem_fb) {
                    uint32_t* fbp = shmem_fb + (4096 / 4);
                    memset(fbp, 0, fb_w * fb_h * 4);
                }
            } else {
                int img_width, img_height, channels;
                unsigned char *img_data = stbi_load(to_draw.c_str(), &img_width, &img_height, &channels, 4);

                if (img_data && shmem_fb) {
                    uint32_t* fbp = shmem_fb + (4096 / 4);
                    memset(fbp, 0, fb_w * fb_h * 4); 

                    float scale = (float)(fb_h * 0.9f) / (float)img_height;
                    int draw_h = (int)(img_height * scale);
                    int draw_w = (int)(img_width * scale);

                    int start_x = (fb_w - draw_w) / 2; 
                    int start_y = (fb_h - draw_h) / 2; 
                    
                    for (int y = 0; y < draw_h; y++) {
                        for (int x = 0; x < draw_w; x++) {
                            int screen_x = start_x + x;
                            int screen_y = start_y + y;

                            if (screen_x >= 0 && screen_x < fb_w && screen_y >= 0 && screen_y < fb_h) {
                                int src_x = (int)(x / scale);
                                int src_y = (int)(y / scale);
                                
                                int img_offset = (src_y * img_width + src_x) * 4;
                                unsigned char r = img_data[img_offset + 0];
                                unsigned char g = img_data[img_offset + 1];
                                unsigned char b_col = img_data[img_offset + 2];
                                unsigned char a = img_data[img_offset + 3];

                                if (a > 128) {
                                    fbp[screen_y * fb_w + screen_x] = (a << 24) | (r << 16) | (g << 8) | b_col; 
                                }
                            }
                        }
                    }
                    stbi_image_free(img_data);
                }
            }
            current_drawn = to_draw;
        }
    }
}

// A FUNÇÃO DE LIMPEZA SEGURA (Chamada pelo menu.cpp)
void custom_cover_clear() {
    if (fb_is_on) {
        video_fb_enable(0);
        int tty = open("/dev/tty0", O_RDWR);
        if (tty != -1) { ioctl(tty, KDSETMODE, KD_TEXT); close(tty); }
        fb_is_on = false;
        last_hook_path = "CLEARED";
        
        std::lock_guard<std::mutex> lock(mtx);
        target_image = "";
        has_new_request = true;
        cv.notify_one();
    }
}

void custom_draw_cover_hook(const char* dir_path, const char* file_name) 
{
    if (!worker_running) {
        worker_running = true;
        FILE *fp = fopen("/sys/module/MiSTer_fb/parameters/mode", "rt");
        if (fp) {
            int fmt, rb, stride;
            fscanf(fp, "%d %d %d %d %d", &fmt, &rb, &fb_w, &fb_h, &stride);
            fclose(fp);
        }
        if (!shmem_fb) {
            shmem_fb = (uint32_t*)shmem_map(FB_ADDR, FB_SIZE * 4 * 3);
        }
        std::thread(cover_worker_thread).detach();
    }

    std::string path = "";
    if (strcmp(file_name, "..") != 0) {
        std::string full_dir = dir_path;
        if (full_dir.length() > 0 && full_dir[0] != '/') full_dir = "/media/fat/" + full_dir;
        
        std::string f_name = file_name;
        std::string ext = "";
        size_t edot = f_name.find_last_of(".");
        if (edot != std::string::npos) ext = f_name.substr(edot);

        std::string target_dir = full_dir;
        std::string target_file = f_name;

        // --- MÁGICA PARA LER O INTERIOR DO ARQUIVO MGL ---
        if (ext == ".mgl" || ext == ".MGL") {
            std::string mgl_path = full_dir + "/" + f_name;
            FILE* fp = fopen(mgl_path.c_str(), "r");
            if (fp) {
                char line[1024];
                while (fgets(line, sizeof(line), fp)) {
                    std::string sline = line;
                    size_t pos = sline.find("path=\"");
                    if (pos != std::string::npos) {
                        pos += 6; 
                        size_t end = sline.find("\"", pos);
                        if (end != std::string::npos) {
                            std::string t_path = sline.substr(pos, end - pos);
                            
                            size_t zip_pos = t_path.find(".zip/");
                            if (zip_pos == std::string::npos) zip_pos = t_path.find(".rar/");
                            if (zip_pos == std::string::npos) zip_pos = t_path.find(".7z/");

                            if (zip_pos != std::string::npos) {
                                size_t slash = t_path.rfind('/', zip_pos);
                                if (slash != std::string::npos) target_dir = t_path.substr(0, slash);
                            } else {
                                size_t slash = t_path.find_last_of('/');
                                if (slash != std::string::npos) target_dir = t_path.substr(0, slash);
                            }

                            if (target_dir.length() > 0 && target_dir[0] != '/') {
                                target_dir = "/media/fat/" + target_dir;
                            }

                            size_t last_slash = t_path.find_last_of('/');
                            if (last_slash != std::string::npos) {
                                target_file = t_path.substr(last_slash + 1);
                            } else {
                                target_file = t_path;
                            }
                            break; 
                        }
                    }
                }
                fclose(fp);
            }
        }
        // --- MÁGICA PARA LER O INTERIOR DO ARQUIVO MRA ---
        else if (ext == ".mra" || ext == ".MRA") {
            std::string mra_path = full_dir + "/" + f_name;
            FILE* fp = fopen(mra_path.c_str(), "r");
            if (fp) {
                char line[1024];
                while (fgets(line, sizeof(line), fp)) {
                    std::string sline = line;
                    size_t pos = sline.find("zip=\"");
                    if (pos != std::string::npos) {
                        pos += 5; // Pula zip="
                        size_t end = sline.find("\"", pos);
                        if (end != std::string::npos) {
                            std::string t_zip = sline.substr(pos, end - pos);
                            
                            // Remove vírgulas se o MRA tiver múltiplos zips declarados
                            size_t comma = t_zip.find(",");
                            if (comma != std::string::npos) t_zip = t_zip.substr(0, comma);

                            // Remove o .zip do nome da ROM
                            size_t zdot = t_zip.find(".zip");
                            if (zdot == std::string::npos) zdot = t_zip.find(".ZIP");
                            if (zdot != std::string::npos) target_file = t_zip.substr(0, zdot);
                            else target_file = t_zip;

                            // Verifica MAME e HBMAME nas diversas diretorias possíveis
                            if (file_exists("/media/fat/_Arcade/mame/.images/" + target_file + ".png")) {
                                target_dir = "/media/fat/_Arcade/mame";
                            } else if (file_exists("/media/fat/_Arcade/hbmame/.images/" + target_file + ".png")) {
                                target_dir = "/media/fat/_Arcade/hbmame";
                            } else if (file_exists("/media/fat/games/mame/.images/" + target_file + ".png")) {
                                target_dir = "/media/fat/games/mame";
                            } else if (file_exists("/media/fat/games/hbmame/.images/" + target_file + ".png")) {
                                target_dir = "/media/fat/games/hbmame";
                            } else if (file_exists(full_dir + "/mame/.images/" + target_file + ".png")) {
                                target_dir = full_dir + "/mame";
                            } else if (file_exists(full_dir + "/hbmame/.images/" + target_file + ".png")) {
                                target_dir = full_dir + "/hbmame";
                            } else {
                                target_dir = "/media/fat/_Arcade/mame"; // Default fallback
                            }
                            
                            break; 
                        }
                    }
                }
                fclose(fp);
            }
        }
        // --------------------------------------------------

        path = target_dir + "/.images/";
        size_t dot = target_file.find_last_of(".");
        if (dot != std::string::npos) {
            path += target_file.substr(0, dot) + ".png";
        } else {
            path += target_file + ".png";
        }
    }

    if (path == last_hook_path) return; 
    last_hook_path = path;

    bool has_cover = (path != "" && file_exists(path));

    if (has_cover && !fb_is_on) {
        int tty = open("/dev/tty0", O_RDWR);
        if (tty != -1) { ioctl(tty, KDSETMODE, KD_GRAPHICS); close(tty); }
        video_fb_enable(1, 0);
        fb_is_on = true;
    } 
    else if (!has_cover && fb_is_on) {
        custom_cover_clear();
        return; 
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        target_image = has_cover ? path : "";
        has_new_request = true;
    }
    cv.notify_one(); 
}