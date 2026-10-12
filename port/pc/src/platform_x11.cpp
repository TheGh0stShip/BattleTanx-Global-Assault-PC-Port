#include "btga/platform.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <string>
#include <stdexcept>

namespace btga {
class X11Platform final : public Platform {
  public:
    explicit X11Platform(std::string_view title) {
        display_ = XOpenDisplay(nullptr);
        if (!display_) throw std::runtime_error("X11 display is unavailable");
        const int screen = DefaultScreen(display_);
        window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen), 0, 0,
                                      kWidth, kHeight, 0, 0, 0);
        XStoreName(display_, window_, std::string(title).c_str());
        XSelectInput(display_, window_, ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);
        wm_delete_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &wm_delete_, 1);
        XMapWindow(display_, window_);
        image_ = XCreateImage(display_, DefaultVisual(display_, screen), 24, ZPixmap, 0,
                              reinterpret_cast<char*>(pixels_.data()), kWidth, kHeight, 32, 0);
        gc_ = XCreateGC(display_, window_, 0, nullptr);
    }
    ~X11Platform() override {
        if (image_) { image_->data = nullptr; XDestroyImage(image_); }
        if (gc_) XFreeGC(display_, gc_);
        if (window_) XDestroyWindow(display_, window_);
        if (display_) XCloseDisplay(display_);
    }
    bool pump(Input& input) override {
        while (XPending(display_)) {
            XEvent event{}; XNextEvent(display_, &event);
            if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_) return false;
            if (event.type == KeyPress || event.type == KeyRelease) {
                const bool down = event.type == KeyPress;
                const KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape && down) return false;
                if (key == XK_w || key == XK_Up) keys_[0] = down;
                if (key == XK_s || key == XK_Down) keys_[1] = down;
                if (key == XK_a || key == XK_Left) keys_[2] = down;
                if (key == XK_d || key == XK_Right) keys_[3] = down;
                if (key == XK_space) keys_[4] = down;
            }
        }
        input = {keys_[0], keys_[1], keys_[2], keys_[3], keys_[4]};
        return true;
    }
    void present(const Framebuffer& framebuffer) override {
        std::copy(framebuffer.pixels.begin(), framebuffer.pixels.end(), pixels_.begin());
        XPutImage(display_, window_, gc_, image_, 0, 0, 0, 0, kWidth, kHeight);
        XFlush(display_);
    }
  private:
    Display* display_{}; Window window_{}; GC gc_{}; Atom wm_delete_{}; XImage* image_{};
    std::array<std::uint32_t, kWidth * kHeight> pixels_{};
    std::array<bool, 5> keys_{};
};

Platform* Platform::create(std::string_view title) { return new X11Platform(title); }
} // namespace btga
