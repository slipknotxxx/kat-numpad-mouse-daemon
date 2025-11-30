/*
 * kat.c
 * Toggles mouse_mode with double-Ctrl press and moves mouse with numpad.
 * Compile: gcc -o kat kat.c -std=c11 -lX11 -lXtst -lpthread -Wall -Wextra
 * Run: sudo ./kat
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <ctype.h>
#include <poll.h>
#include <libgen.h>
#include <sys/stat.h>

#define MAX_KBDS 16
#define INPUT_DIR "/dev/input"
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x) ((x)%BITS_PER_LONG)
#define BIT(x) (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)

#define CONFIG_COMMENT \
    "; Mouse Daemon configuration\n" \
    "; Edit the values below – the daemon will reload them on every start\n" \
    "; Missing entries are filled with the hard-coded defaults\n\n"

#define DOUBLE_PRESS_THRESHOLD 0.3   /* seconds */
#define ALT_DOUBLE_THRESHOLD 0.3
#define MODE_POPUP_DURATION_MS 1000  /* mode toggle popup */
#define MARGIN_OVERLAY_TIMEOUT 1.0   /* seconds after last adjust to hide */
#define JUMP_OVERLAY_TIMEOUT 1.0     /* seconds after last adjust to hide */

static const char *config_file = NULL;

/* ------------------------------------------------------------------ */
/* Configuration Struct                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    int mouse_speed;
    int movement_interval_slow_ms;
    int movement_interval_fast_ms;
    double movement_acceleration_time;
    int jump_horizontal;
    int jump_vertical;
    int jump_diagonal;
    int jump_margin;
    int jump_interval_ms;
    double scroll_speed;
    int scroll_interval_ms;
    double autoscroll_speed;
    int autoscroll_interval_ms;
} Config;

/* ------------------------------------------------------------------ */
/* Config Item Definition (for parsing and display)                   */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name;
    void *value;
    const char *fmt;
    bool is_double;
    double min_val;
    const char *unit;
    const char *shortcut;
    unsigned int bit;
} ConfigItem;

enum {
    CFG_MOUSE_SPEED_BIT = 1u<<0,
    CFG_MOVEMENT_INTERVAL_SLOW_MS_BIT = 1u<<1,
    CFG_MOVEMENT_INTERVAL_FAST_MS_BIT = 1u<<2,
    CFG_MOVEMENT_ACCELERATION_TIME_BIT = 1u<<3,
    CFG_JUMP_HORIZONTAL_BIT = 1u<<4,
    CFG_JUMP_VERTICAL_BIT = 1u<<5,
    CFG_JUMP_DIAGONAL_BIT = 1u<<6,
    CFG_JUMP_MARGIN_BIT = 1u<<7,
    CFG_JUMP_INTERVAL_MS_BIT = 1u<<8,
    CFG_SCROLL_SPEED_BIT = 1u<<9,
    CFG_SCROLL_INTERVAL_MS_BIT = 1u<<10,
    CFG_AUTOSCROLL_SPEED_BIT = 1u<<11,
    CFG_AUTOSCROLL_INTERVAL_MS_BIT = 1u<<12,
};

static ConfigItem config_items[] = {
    {"MOUSE_SPEED", NULL, "%d", false, 1, "px", "(Shift)+Alt+NumLock", CFG_MOUSE_SPEED_BIT},
    {"MOVEMENT_INTERVAL_SLOW_MS", NULL, "%d", false, 1, "ms", "(Shift)+Alt+Asterisk", CFG_MOVEMENT_INTERVAL_SLOW_MS_BIT},
    {"MOVEMENT_INTERVAL_FAST_MS", NULL, "%d", false, 1, "ms", "(Shift)+Alt+Hyphen", CFG_MOVEMENT_INTERVAL_FAST_MS_BIT},
    {"MOVEMENT_ACCELERATION_TIME", NULL, "%.1f", true, 0.1, "s", "(Shift)+Alt+Slash", CFG_MOVEMENT_ACCELERATION_TIME_BIT},
    {"JUMP_HORIZONTAL", NULL, "%d", false, 0, "px", "Alt+6/4", CFG_JUMP_HORIZONTAL_BIT},
    {"JUMP_VERTICAL", NULL, "%d", false, 0, "px", "Alt+8/2", CFG_JUMP_VERTICAL_BIT},
    {"JUMP_DIAGONAL", NULL, "%d", false, 0, "px", "Alt+7/9 / Alt+1/3", CFG_JUMP_DIAGONAL_BIT},
    {"JUMP_MARGIN", NULL, "%d", false, 0, "px", "(Shift)+Alt+5", CFG_JUMP_MARGIN_BIT},
    {"JUMP_INTERVAL_MS", NULL, "%d", false, 1, "ms", "(Shift)+Alt+0", CFG_JUMP_INTERVAL_MS_BIT},
    {"SCROLL_SPEED", NULL, "%.2f", true, 0.01, "ticks", "Alt+Plus/Enter (manual)", CFG_SCROLL_SPEED_BIT},
    {"SCROLL_INTERVAL_MS", NULL, "%d", false, 1, "ms", "(Shift)+Alt+Period (manual)", CFG_SCROLL_INTERVAL_MS_BIT},
    {"AUTOSCROLL_SPEED", NULL, "%.2f", true, 0.01, "ticks", "Alt+Plus/Enter (auto)", CFG_AUTOSCROLL_SPEED_BIT},
    {"AUTOSCROLL_INTERVAL_MS", NULL, "%d", false, 1, "ms", "(Shift)+Alt+Period (auto)", CFG_AUTOSCROLL_INTERVAL_MS_BIT},
};

static const int num_config_items = sizeof(config_items) / sizeof(config_items[0]);

/* ------------------------------------------------------------------ */
/* Application State Struct                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    Config cfg;
    bool ctrl_pressed;
    bool alt_pressed;
    bool shift_pressed;
    bool mouse_mode;
    bool numpad_keys_pressed[8];   /* 0=8,1=2,2=4,3=6,4=7,5=9,6=1,7=3 */
    bool scroll_keys_pressed[2];   /* 0=plus (up), 1=enter (down) */
    bool left_button_held;
	bool drag_locked;
    bool autoscroll_up_active;
    bool autoscroll_down_active;
    double movement_start_time;
    double last_alt_press;
    double last_autoscroll_feedback;
    double adjust_start_times[KEY_MAX + 1];
    bool left_ctrl_forwarded;
    bool right_ctrl_forwarded;
    bool pending_ctrl;
    int pending_ctrl_code;
    int kbd_fds[MAX_KBDS];
    int num_kbds;
    int uinput_fd;
    int mouse_fd;
    pthread_t movement_thread;
    pthread_mutex_t state_mutex;
    bool running;
} AppState;

static AppState state = {0};

/* ------------------------------------------------------------------ */
/* Config Panel Struct                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    bool active;
    double last_activity_time;
    int selected_row;
    int shortcut_col_x;
    int max_shortcut_w;
    int col1_w;
    int initial_mouse_x;
    int initial_mouse_y;
    Window win;
    Display *dpy;
    GC gc;
    XFontStruct *font;
    pthread_t timer_thread;
    pthread_t mouse_monitor_thread;
    pthread_mutex_t mutex;
} ConfigPanel;

/* ------------------------------------------------------------------ */
/* Forward Declarations for Thread Functions                          */
/* ------------------------------------------------------------------ */
static void* movement_thread_func(void *arg);
static void* panel_timer_func(void *arg);
static void* mouse_monitor_func(void *arg);
static void* config_panel_thread(void *arg);
static void* feedback_popup_thread(void *arg);
static void* margin_overlay_thread(void *arg);
static void* jump_overlay_thread(void *arg);

/* ------------------------------------------------------------------ */
/* Forward Declarations for Other Functions                           */
/* ------------------------------------------------------------------ */
static void show_feedback(const char *text);
static void show_margin_overlay(void);
static void hide_margin_overlay(void);
static void show_jump_overlay(int type);
static bool handle_ctrl_key(const struct input_event *ev, double *last_ctrl_press);
static bool handle_alt_key(const struct input_event *ev, ConfigPanel *panel);
static bool handle_shift_key(const struct input_event *ev);
static bool handle_esc_in_panel(const struct input_event *ev, ConfigPanel *panel);
static void disable_autoscroll_if_not_allowed(const struct input_event *ev);
static bool handle_panel_nav_key(const struct input_event *ev, ConfigPanel *panel);
static bool handle_non_nav_in_panel(const struct input_event *ev, ConfigPanel *panel);
static bool handle_alt_adjustment_key(const struct input_event *ev);
static bool handle_scroll_and_autoscroll_key(const struct input_event *ev);
static bool handle_ctrl_minus_key(const struct input_event *ev);
static bool handle_absolute_jump_key(const struct input_event *ev);
static bool handle_numpad_direction_key(const struct input_event *ev);
static bool handle_kp5_key(const struct input_event *ev);
static bool handle_kpslash_key(const struct input_event *ev);
static bool handle_kpasterisk_key(const struct input_event *ev);
static bool handle_kpminus_key(const struct input_event *ev);
static bool handle_numlock_key(const struct input_event *ev);

/* ------------------------------------------------------------------ */
/* Config Handling Functions                                          */
/* ------------------------------------------------------------------ */
static double get_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

static int cfg_parse_line(const char *line, char *name, size_t name_sz, char *value, size_t value_sz) {
    const char *p = line;
    while (*p && (isspace(*p) || *p == ';' || *p == '#')) ++p;
    if (!*p) return 0;

    const char *start = p;
    while (*p && !isspace(*p) && *p != '=') ++p;
    size_t len = p - start;
    if (len >= name_sz) len = name_sz - 1;
    memcpy(name, start, len);
    name[len] = '\0';

    while (*p && *p != '=') ++p;
    if (*p != '=') return 0;
    ++p;
    while (*p && isspace(*p)) ++p;

    start = p;
    while (*p && *p != ';' && *p != '#') ++p;
    while (p > start && isspace(*(p-1))) --p;
    len = p - start;
    if (len >= value_sz) len = value_sz - 1;
    memcpy(value, start, len);
    value[len] = '\0';
    return 1;
}

static void write_default_config(void) {
    FILE *f = fopen(config_file, "w");
    if (!f) { perror("fopen config"); return; }

    fprintf(f, CONFIG_COMMENT);
    for (int i = 0; i < num_config_items; i++) {
        ConfigItem *item = &config_items[i];
        fprintf(f, "%s = ", item->name);
        if (item->is_double) {
            fprintf(f, item->fmt, *(double*)item->value);
        } else {
            fprintf(f, item->fmt, *(int*)item->value);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    printf("Created default %s\n", config_file);
}

static void verify_and_restore_config(unsigned int *cfg_seen) {
    bool changed = false;
    FILE *f = fopen(config_file, "a");
    if (!f) { perror("append config"); return; }

    for (int i = 0; i < num_config_items; i++) {
        ConfigItem *item = &config_items[i];
        if (!(*cfg_seen & item->bit)) {
            fprintf(f, "%s = ", item->name);
            if (item->is_double) {
                fprintf(f, item->fmt, *(double*)item->value);
            } else {
                fprintf(f, item->fmt, *(int*)item->value);
            }
            fprintf(f, "\n");
            changed = true;
        }
    }
    fclose(f);
    if (changed) printf("Updated %s with missing defaults\n", config_file);
}

static void load_config(void) {
    state.cfg.mouse_speed = 5;
    state.cfg.movement_interval_slow_ms = 64;
    state.cfg.movement_interval_fast_ms = 8;
    state.cfg.movement_acceleration_time = 0.4;
    state.cfg.jump_horizontal = 100;
    state.cfg.jump_vertical = 100;
    state.cfg.jump_diagonal = 100;
    state.cfg.jump_margin = 20;
    state.cfg.jump_interval_ms = 80;
    state.cfg.scroll_speed = 1.0;
    state.cfg.scroll_interval_ms = 100;
    state.cfg.autoscroll_speed = 0.01;
    state.cfg.autoscroll_interval_ms = 24;

    // Bind config items to struct fields
    config_items[0].value = &state.cfg.mouse_speed;
    config_items[1].value = &state.cfg.movement_interval_slow_ms;
    config_items[2].value = &state.cfg.movement_interval_fast_ms;
    config_items[3].value = &state.cfg.movement_acceleration_time;
    config_items[4].value = &state.cfg.jump_horizontal;
    config_items[5].value = &state.cfg.jump_vertical;
    config_items[6].value = &state.cfg.jump_diagonal;
    config_items[7].value = &state.cfg.jump_margin;
    config_items[8].value = &state.cfg.jump_interval_ms;
    config_items[9].value = &state.cfg.scroll_speed;
    config_items[10].value = &state.cfg.scroll_interval_ms;
    config_items[11].value = &state.cfg.autoscroll_speed;
    config_items[12].value = &state.cfg.autoscroll_interval_ms;

    if (access(config_file, F_OK) != 0) {
        write_default_config();
        return;
    }

    FILE *f = fopen(config_file, "r");
    if (!f) {
        fprintf(stderr, "Can't open %s – using defaults\n", config_file);
        return;
    }

    char line[256];
    char name[64], value[64];
    unsigned int cfg_seen = 0;

    while (fgets(line, sizeof(line), f)) {
        if (cfg_parse_line(line, name, sizeof(name), value, sizeof(value))) {
            for (int i = 0; i < num_config_items; i++) {
                ConfigItem *item = &config_items[i];
                if (strcmp(name, item->name) == 0) {
                    if (item->is_double) {
                        *(double*)item->value = atof(value);
                    } else {
                        *(int*)item->value = atoi(value);
                    }
                    cfg_seen |= item->bit;
                    break;
                }
            }
        }
    }
    fclose(f);

    verify_and_restore_config(&cfg_seen);
}

static void save_config(void) {
    FILE *f = fopen(config_file, "w");
    if (!f) { perror("fopen config for save"); return; }

    fprintf(f, CONFIG_COMMENT);
    for (int i = 0; i < num_config_items; i++) {
        ConfigItem *item = &config_items[i];
        fprintf(f, "%s = ", item->name);
        if (item->is_double) {
            fprintf(f, item->fmt, *(double*)item->value);
        } else {
            fprintf(f, item->fmt, *(int*)item->value);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    printf("Saved config to %s\n", config_file);
}

/* ------------------------------------------------------------------ */
/* Adjustment Helpers                                                 */
/* ------------------------------------------------------------------ */
static int get_step_multiplier(double elapsed) {
    if (elapsed < 0.4) return 1;
    if (elapsed < 0.8) return 2;
    if (elapsed < 1.2) return 4;
    if (elapsed < 1.6) return 8;
    if (elapsed < 2.0) return 16;
    return 32;
}

static double get_scroll_speed_delta(double current, int multi, bool increase) {
    double unit = (current <= 0.10000001) ? 0.01 : 0.1;
    double delta = multi * unit;
    if (!increase) {
        delta = -delta;
        if (unit == 0.1 && (current + delta) < 0.1) {
            return -(multi * 0.01);
        }
    }
    return delta;
}

static void adjust_config_value(int index, int step, bool is_live) {
    ConfigItem *item = &config_items[index];
    if (item->is_double) {
        double delta = (strcmp(item->name, "SCROLL_SPEED") == 0 || strcmp(item->name, "AUTOSCROLL_SPEED") == 0)
                       ? get_scroll_speed_delta(*(double*)item->value, abs(step), step > 0)
                       : step * 0.1;
        *(double*)item->value += delta;
        if (*(double*)item->value < item->min_val) *(double*)item->value = item->min_val;
    } else {
        *(int*)item->value += step;
        if (*(int*)item->value < (int)item->min_val) *(int*)item->value = (int)item->min_val;
    }

    if (is_live) {
        char msg[128], valbuf[64];
        if (item->is_double) {
            snprintf(valbuf, sizeof(valbuf), item->fmt, *(double*)item->value);
        } else {
            snprintf(valbuf, sizeof(valbuf), item->fmt, *(int*)item->value);
        }
        snprintf(msg, sizeof(msg), "%s: %s %s", item->name, valbuf, item->unit);
        show_feedback(msg);
    }
}

/* ------------------------------------------------------------------ */
/* Mouse Control Helpers                                              */
/* ------------------------------------------------------------------ */
static void emit_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev = {
        .type = type,
        .code = code,
        .value = value,
    };
    gettimeofday(&ev.time, NULL);

    /* Silence -Wunused-result cleanly and safely */
    if (write(fd, &ev, sizeof(ev)) < 0) {
        /* Only spam if it's not a broken pipe / device gone */
        if (errno != EPIPE && errno != ENODEV && errno != EINVAL)
            perror("write to uinput failed");
    }
}

static void warp_mouse(Display *dpy, int x, int y) {
    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, x, y);
    XSync(dpy, False);
}

static void mouse_click(int button) {
    emit_event(state.mouse_fd, EV_KEY, button, 1);
    emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
    usleep(10000);
    emit_event(state.mouse_fd, EV_KEY, button, 0);
    emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
}

static void mouse_wheel(double amount) {
    int hi_res_value = (int)(amount * 120);
    emit_event(state.mouse_fd, EV_REL, REL_WHEEL_HI_RES, hi_res_value);
    int full_notches = hi_res_value / 120;
    if (full_notches != 0) {
        emit_event(state.mouse_fd, EV_REL, REL_WHEEL, full_notches);
    }
    emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
}

/* ------------------------------------------------------------------ */
/* Diagonal Component Helper                                          */
/* ------------------------------------------------------------------ */
static void diag_components(int diag, int *dx, int *dy) {
    if (diag <= 0) { *dx = *dy = 0; return; }
    int comp = (int)((diag * 7071) / 10000 + 0.5);   /* 7071 approx 1/sqrt(2)*10000 */
    *dx = comp;
    *dy = comp;
}

/* ------------------------------------------------------------------ */
/* Movement Thread                                                    */
/* ------------------------------------------------------------------ */
static void* movement_thread_func(void *arg) {
    (void)arg;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Failed to open display in movement thread\n");
        return NULL;
    }

    while (state.running) {
        pthread_mutex_lock(&state.state_mutex);

        bool any_numpad = false;
        for (int i = 0; i < 8; ++i) any_numpad |= state.numpad_keys_pressed[i];

        bool scroll_up = state.scroll_keys_pressed[0] || state.autoscroll_up_active;
        bool scroll_down = state.scroll_keys_pressed[1] || state.autoscroll_down_active;

        bool do_jump = state.ctrl_pressed && any_numpad;
        bool do_smooth = !state.ctrl_pressed && any_numpad;

        int dx = 0, dy = 0;
        if (do_jump || do_smooth) {
            int horiz = do_jump ? state.cfg.jump_horizontal : state.cfg.mouse_speed;
            int vert = do_jump ? state.cfg.jump_vertical : state.cfg.mouse_speed;
            int diag = do_jump ? state.cfg.jump_diagonal : state.cfg.mouse_speed;

            if (state.numpad_keys_pressed[0]) dy -= vert;  // 8 → up
            if (state.numpad_keys_pressed[1]) dy += vert;  // 2 → down
            if (state.numpad_keys_pressed[2]) dx -= horiz; // 4 → left
            if (state.numpad_keys_pressed[3]) dx += horiz; // 6 → right

            int cx = 0, cy = 0;
            if (state.numpad_keys_pressed[4]) { diag_components(diag, &cx, &cy); dx -= cx; dy -= cy; } // 7
            if (state.numpad_keys_pressed[5]) { diag_components(diag, &cx, &cy); dx += cx; dy -= cy; } // 9
            if (state.numpad_keys_pressed[6]) { diag_components(diag, &cx, &cy); dx -= cx; dy += cy; } // 1
            if (state.numpad_keys_pressed[7]) { diag_components(diag, &cx, &cy); dx += cx; dy += cy; } // 3
        }

        pthread_mutex_unlock(&state.state_mutex);

        if (state.mouse_mode) {
            double now = get_time();

            if ((do_jump || do_smooth) && (dx != 0 || dy != 0)) {
                Window root = DefaultRootWindow(dpy);
                Window child;
                int root_x, root_y, win_x, win_y;
                unsigned int mask;

                if (!XQueryPointer(dpy, root, &root, &child, &root_x, &root_y, &win_x, &win_y, &mask)) {
                    usleep(1000);
                    continue;
                }

                int scr_w = DisplayWidth(dpy, DefaultScreen(dpy));
                int scr_h = DisplayHeight(dpy, DefaultScreen(dpy));

                int target_x = root_x + dx;
                int target_y = root_y + dy;

                target_x = (target_x % scr_w + scr_w) % scr_w;
                target_y = (target_y % scr_h + scr_h) % scr_h;

                warp_mouse(dpy, target_x, target_y);

                int interval_ms;
                if (do_jump) {
                    interval_ms = state.cfg.jump_interval_ms;
                } else {
                    if (state.movement_start_time == 0.0) state.movement_start_time = now;

                    double elapsed = now - state.movement_start_time;
                    double progress = elapsed / state.cfg.movement_acceleration_time;
                    if (progress > 1.0) progress = 1.0;

                    interval_ms = state.cfg.movement_interval_slow_ms -
                                  (int)((state.cfg.movement_interval_slow_ms - state.cfg.movement_interval_fast_ms) * progress);
                }

                usleep(interval_ms * 1000);
            } else {
                state.movement_start_time = 0.0;
            }

            if (scroll_up || scroll_down) {
                double direction = scroll_up ? 1.0 : -1.0;
                double speed = (state.autoscroll_up_active || state.autoscroll_down_active) ? state.cfg.autoscroll_speed : state.cfg.scroll_speed;
                int interval = (state.autoscroll_up_active || state.autoscroll_down_active) ? state.cfg.autoscroll_interval_ms : state.cfg.scroll_interval_ms;

                mouse_wheel(direction * speed);
                usleep(interval * 1000);
            }

            usleep(1000);
        } else {
            state.movement_start_time = 0.0;
            usleep(1000);
        }
    }

    XCloseDisplay(dpy);
    return NULL;
}

static bool feedback_popup_active = false;
static char feedback_message[64] = "";
static double feedback_end_time = 0.0;
static pthread_mutex_t feedback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t feedback_cond = PTHREAD_COND_INITIALIZER;
static bool drag_popup_visible = false;
static pthread_mutex_t drag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t drag_cond = PTHREAD_COND_INITIALIZER;

static void* feedback_popup_thread(void *arg) {
    (void)arg;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return NULL;

    int scr = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);

    XFontStruct *font = XLoadQueryFont(dpy, "fixed");
    if (!font) font = XLoadQueryFont(dpy, "9x15");

    Window win = None;
    GC gc = None;
    bool first = true;

    while (true) {
        pthread_mutex_lock(&feedback_mutex);
        if (!feedback_popup_active) {
            pthread_mutex_unlock(&feedback_mutex);
            break;
        }

        char msg[128];
        strcpy(msg, feedback_message);
        double end_time = feedback_end_time;
        pthread_mutex_unlock(&feedback_mutex);

        int w = 260, h = 60;
        int text_w = 0, asc = 0, desc = 0;
        if (font) {
            int dir;
            XCharStruct ov;
            XTextExtents(font, msg, strlen(msg), &dir, &asc, &desc, &ov);
            w = ov.width + 40;
            h = asc + desc + 24;
            text_w = ov.width;
        }

        int x = (DisplayWidth(dpy, scr) - w) / 2;
        int y = (DisplayHeight(dpy, scr) - h) / 2;

        if (first) {
            first = false;

            XSetWindowAttributes attrs = {0};
            attrs.override_redirect = True;
            attrs.background_pixel = 0xFFFFFF;
            attrs.border_pixel = 0;

            win = XCreateWindow(dpy, root, x, y, w, h, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel | CWBorderPixel,
                                &attrs);

            Atom opacity_atom = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
            unsigned long opacity = 0xFFFFFFFF;
            XChangeProperty(dpy, win, opacity_atom, XA_CARDINAL, 32,
                            PropModeReplace, (unsigned char*)&opacity, 1);

            XMapWindow(dpy, win);

            gc = XCreateGC(dpy, win, 0, NULL);
            XSetForeground(dpy, gc, BlackPixel(dpy, scr));
            if (font) XSetFont(dpy, gc, font->fid);
        } else {
            XMoveResizeWindow(dpy, win, x, y, w, h);
        }

        XClearWindow(dpy, win);
        if (font) {
            int tx = (w - text_w) / 2;
            int ty = (h + asc - desc) / 2;
            XDrawString(dpy, win, gc, tx, ty, msg, strlen(msg));
        }
        XFlush(dpy);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double now = ts.tv_sec + ts.tv_nsec * 1e-9;
        double wait = end_time - now;
        if (wait <= 0) {
            pthread_mutex_lock(&feedback_mutex);
            feedback_popup_active = false;
            pthread_mutex_unlock(&feedback_mutex);
            continue;
        }

        long sec = (long)wait;
        long nsec = (wait - sec) * 1000000000L;
        ts.tv_sec += sec;
        ts.tv_nsec += nsec;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

        pthread_mutex_lock(&feedback_mutex);
        int r = pthread_cond_timedwait(&feedback_cond, &feedback_mutex, &ts);
        pthread_mutex_unlock(&feedback_mutex);

        if (r == ETIMEDOUT) {
            pthread_mutex_lock(&feedback_mutex);
            feedback_popup_active = false;
            pthread_mutex_unlock(&feedback_mutex);
        }
    }

    if (font) XFreeFont(dpy, font);
    if (gc) XFreeGC(dpy, gc);
    if (win != None) XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return NULL;
}

static void* drag_popup_thread(void *arg) {
    (void)arg;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return NULL;

    int scr = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);

    XFontStruct *font = XLoadQueryFont(dpy, "fixed");
    if (!font) font = XLoadQueryFont(dpy, "9x15");

    const char *msg = "Drag Mode";
    Window win = None;
    GC gc = None;

    int w = 260, h = 60;
    int text_w = 0, asc = 0, desc = 0;
    if (font) {
        int dir;
        XCharStruct ov;
        XTextExtents(font, msg, strlen(msg), &dir, &asc, &desc, &ov);
        w = ov.width + 40;
        h = asc + desc + 24;
        text_w = ov.width;
    }

    int x = (DisplayWidth(dpy, scr) - w) / 2;
    int y = (DisplayHeight(dpy, scr) - h) / 2;

    XSetWindowAttributes attrs = {0};
    attrs.override_redirect = True;
    attrs.background_pixel = 0xFFFFFF;
    attrs.border_pixel = 0;

    win = XCreateWindow(dpy, root, x, y, w, h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWBorderPixel,
                        &attrs);

    Atom opacity_atom = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    unsigned long opacity = 0xFFFFFFFF;
    XChangeProperty(dpy, win, opacity_atom, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char*)&opacity, 1);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, scr));
    if (font) XSetFont(dpy, gc, font->fid);

    XClearWindow(dpy, win);
    if (font) {
        int tx = (w - text_w) / 2;
        int ty = (h + asc - desc) / 2;
        XDrawString(dpy, win, gc, tx, ty, msg, strlen(msg));
    }
    XFlush(dpy);

    pthread_mutex_lock(&drag_mutex);
    while (drag_popup_visible) {
        pthread_cond_wait(&drag_cond, &drag_mutex);
    }
    pthread_mutex_unlock(&drag_mutex);

    if (font) XFreeFont(dpy, font);
    if (gc) XFreeGC(dpy, gc);
    if (win != None) XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return NULL;
}

static void show_feedback(const char *text) {
    pthread_mutex_lock(&feedback_mutex);
    strncpy(feedback_message, text, sizeof(feedback_message)-1);
    feedback_message[sizeof(feedback_message)-1] = '\0';
    feedback_end_time = get_time() + (MODE_POPUP_DURATION_MS / 1000.0);
    bool was_active = feedback_popup_active;
    feedback_popup_active = true;
    pthread_cond_signal(&feedback_cond);
    pthread_mutex_unlock(&feedback_mutex);

    if (!was_active) {
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &attr, feedback_popup_thread, NULL);
        pthread_attr_destroy(&attr);
    }
}

static void show_drag_popup(void) {
    pthread_mutex_lock(&drag_mutex);
    if (drag_popup_visible) {
        pthread_mutex_unlock(&drag_mutex);
        return;
    }
    drag_popup_visible = true;
    pthread_mutex_unlock(&drag_mutex);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, drag_popup_thread, NULL);
    pthread_attr_destroy(&attr);
}

static void hide_drag_popup(void) {
    pthread_mutex_lock(&drag_mutex);
    if (!drag_popup_visible) {
        pthread_mutex_unlock(&drag_mutex);
        return;
    }
    drag_popup_visible = false;
    pthread_cond_signal(&drag_cond);
    pthread_mutex_unlock(&drag_mutex);
}

/* ------------------------------------------------------------------ */
/* Config Panel Functions                                             */
/* ------------------------------------------------------------------ */
static void init_config_panel(ConfigPanel *panel) {
    panel->active = false;
    panel->last_activity_time = 0.0;
    panel->selected_row = 0;
    panel->shortcut_col_x = 0;
    panel->max_shortcut_w = 0;
    panel->col1_w = 0;
    panel->initial_mouse_x = 0;
    panel->initial_mouse_y = 0;
    panel->win = None;
    panel->dpy = NULL;
    panel->gc = None;
    panel->font = NULL;
    pthread_mutex_init(&panel->mutex, NULL);
}

static void* panel_timer_func(void *arg) {
    ConfigPanel *panel = (ConfigPanel *)arg;
    while (panel->active) {
        double now = get_time();
        if (now - panel->last_activity_time > 5.0) {
            panel->active = false;
            break;
        }
        usleep(100000);
    }
    return NULL;
}

static void* mouse_monitor_func(void *arg) {
    ConfigPanel *panel = (ConfigPanel *)arg;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return NULL;
    while (panel->active) {
        Window root, child;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        if (XQueryPointer(dpy, DefaultRootWindow(dpy), &root, &child, &root_x, &root_y, &win_x, &win_y, &mask)) {
            if (root_x != panel->initial_mouse_x || root_y != panel->initial_mouse_y) {
                panel->active = false;
                break;
            }
        }
        usleep(50000);
    }
    XCloseDisplay(dpy);
    return NULL;
}

static void draw_config_panel(ConfigPanel *panel) {
    if (!panel->dpy || panel->win == None) return;

    XClearWindow(panel->dpy, panel->win);

    int ascent = panel->font->ascent;
    int descent = panel->font->descent;
    int line_h = ascent + descent + 8;
    int left_margin = 10;
    int top_margin = 10;
    int col1_w = panel->col1_w;
    int value_col_x = left_margin + col1_w;
    int shortcut_col_x = panel->shortcut_col_x;

    int win_w;
    XWindowAttributes attrs;
    XGetWindowAttributes(panel->dpy, panel->win, &attrs);
    win_w = attrs.width;

    // Title with arrow characters
    int current_x = left_margin;
    int y = top_margin + ascent;

    XChar2b up_arrow = {0x21, 0x91};
    XChar2b down_arrow = {0x21, 0x93};
    XChar2b left_arrow = {0x21, 0x90};
    XChar2b right_arrow = {0x21, 0x92};

    char *part1 = "Esc = Exit | (8";
    XDrawString(panel->dpy, panel->win, panel->gc, current_x, y, part1, strlen(part1));
    current_x += XTextWidth(panel->font, part1, strlen(part1));

    XDrawString16(panel->dpy, panel->win, panel->gc, current_x, y, &up_arrow, 1);
    current_x += XTextWidth16(panel->font, &up_arrow, 1);

    char *part2 = " 2";
    XDrawString(panel->dpy, panel->win, panel->gc, current_x, y, part2, strlen(part2));
    current_x += XTextWidth(panel->font, part2, strlen(part2));

    XDrawString16(panel->dpy, panel->win, panel->gc, current_x, y, &down_arrow, 1);
    current_x += XTextWidth16(panel->font, &down_arrow, 1);

    char *part3 = ") Select | (";
    XDrawString(panel->dpy, panel->win, panel->gc, current_x, y, part3, strlen(part3));
    current_x += XTextWidth(panel->font, part3, strlen(part3));

    XDrawString16(panel->dpy, panel->win, panel->gc, current_x, y, &left_arrow, 1);
    current_x += XTextWidth16(panel->font, &left_arrow, 1);

    char *part4 = "4 6";
    XDrawString(panel->dpy, panel->win, panel->gc, current_x, y, part4, strlen(part4));
    current_x += XTextWidth(panel->font, part4, strlen(part4));

    XDrawString16(panel->dpy, panel->win, panel->gc, current_x, y, &right_arrow, 1);
    current_x += XTextWidth16(panel->font, &right_arrow, 1);

    char *part5 = ") Adjust";
    XDrawString(panel->dpy, panel->win, panel->gc, current_x, y, part5, strlen(part5));

    int line_y = y + 10;
    XSetForeground(panel->dpy, panel->gc, 0x000000);
    XDrawLine(panel->dpy, panel->win, panel->gc, left_margin, line_y, win_w - left_margin, line_y);

    y += line_h + 10;
    XSetForeground(panel->dpy, panel->gc, 0x555555);
    char *header = "Shortcut";
    int header_x = shortcut_col_x + panel->max_shortcut_w - XTextWidth(panel->font, header, strlen(header));
    XDrawString(panel->dpy, panel->win, panel->gc, header_x, y - ascent - 20, header, strlen(header));
    XSetForeground(panel->dpy, panel->gc, 0x000000);

    for (int i = 0; i < num_config_items; ++i) {
        ConfigItem *item = &config_items[i];
        char valbuf[32];
        if (item->is_double) snprintf(valbuf, sizeof(valbuf), item->fmt, *(double*)item->value);
        else snprintf(valbuf, sizeof(valbuf), item->fmt, *(int*)item->value);

        int row_y = y - ascent;

        if (i == panel->selected_row) {
            XSetForeground(panel->dpy, panel->gc, 0xCCCCCC);
            XFillRectangle(panel->dpy, panel->win, panel->gc, 0, row_y - 2, win_w, line_h + 4);
            XSetForeground(panel->dpy, panel->gc, 0x000000);
        }

        XDrawString(panel->dpy, panel->win, panel->gc, left_margin, y, item->name, strlen(item->name));
        XDrawString(panel->dpy, panel->win, panel->gc, value_col_x, y, valbuf, strlen(valbuf));

        XSetForeground(panel->dpy, panel->gc, 0x555555);
        int shortcut_w = XTextWidth(panel->font, item->shortcut, strlen(item->shortcut));
        XDrawString(panel->dpy, panel->win, panel->gc,
                    shortcut_col_x + panel->max_shortcut_w - shortcut_w, y, item->shortcut, strlen(item->shortcut));
        XSetForeground(panel->dpy, panel->gc, 0x000000);
        y += line_h;
    }

    XFlush(panel->dpy);

    if (panel->selected_row == 7) {
        show_margin_overlay();
    } else {
        hide_margin_overlay();
    }

    if (panel->selected_row == 4) {
        show_jump_overlay(1);
    } else if (panel->selected_row == 5) {
        show_jump_overlay(2);
    } else if (panel->selected_row == 6) {
        show_jump_overlay(3);
    } else {
        show_jump_overlay(0);
    }
}

static void* config_panel_thread(void *arg) {
    ConfigPanel *panel = (ConfigPanel *)arg;
    panel->dpy = XOpenDisplay(NULL);
    if (!panel->dpy) return NULL;

    int scr = DefaultScreen(panel->dpy);
    int scr_w = DisplayWidth(panel->dpy, scr);
    int scr_h = DisplayHeight(panel->dpy, scr);

    panel->font = XLoadQueryFont(panel->dpy, "-misc-fixed-bold-r-normal--12-*-*-*-*-*-iso10646-1");
    if (!panel->font) panel->font = XLoadQueryFont(panel->dpy, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1");
    if (!panel->font) panel->font = XLoadQueryFont(panel->dpy, "-misc-fixed-medium-r-*-*-18-*-*-*-*-*-iso10646-1");
    if (!panel->font) panel->font = XLoadQueryFont(panel->dpy, "-misc-fixed-bold-r-normal--14-*-*-*-*-*-iso10646-1");
    if (!panel->font) panel->font = XLoadQueryFont(panel->dpy, "9x15");
    if (!panel->font) panel->font = XLoadQueryFont(panel->dpy, "fixed");

    int ascent = panel->font ? panel->font->ascent : 12;
    int descent = panel->font ? panel->font->descent : 4;
    int line_h = ascent + descent + 8;

    int left_margin = 10;
    int top_margin = 10;
    int padding = 10;
    int shortcut_padding = 60;

    int max_name_w = 0;
    int max_value_w = 0;
    int max_shortcut_w = 0;
    for (int i = 0; i < num_config_items; i++) {
        ConfigItem *item = &config_items[i];
        int nw = XTextWidth(panel->font, item->name, strlen(item->name));
        if (nw > max_name_w) max_name_w = nw;

        char valbuf[32];
        if (item->is_double) snprintf(valbuf, sizeof(valbuf), item->fmt, *(double*)item->value);
        else snprintf(valbuf, sizeof(valbuf), item->fmt, *(int*)item->value);
        int vw = XTextWidth(panel->font, valbuf, strlen(valbuf));
        if (vw > max_value_w) max_value_w = vw;

        int sw = XTextWidth(panel->font, item->shortcut, strlen(item->shortcut));
        if (sw > max_shortcut_w) max_shortcut_w = sw;
    }

    int col1_w = max_name_w + padding;
    panel->col1_w = col1_w;
    int col2_x = left_margin + col1_w + max_value_w + shortcut_padding;
    int total_content_w = col1_w + max_value_w + shortcut_padding + max_shortcut_w;
    panel->shortcut_col_x = col2_x;
    panel->max_shortcut_w = max_shortcut_w;

    XChar2b up_arrow = {0x21, 0x91};
    XChar2b down_arrow = {0x21, 0x93};
    XChar2b left_arrow = {0x21, 0x90};
    XChar2b right_arrow = {0x21, 0x92};

    char *part1 = "Esc = Exit | (8";
    char *part2 = " 2";
    char *part3 = ") Select | (";
    char *part4 = "4 6";
    char *part5 = ") Adjust";

    int title_w = 0;
    title_w += XTextWidth(panel->font, part1, strlen(part1));
    title_w += XTextWidth16(panel->font, &up_arrow, 1);
    title_w += XTextWidth(panel->font, part2, strlen(part2));
    title_w += XTextWidth16(panel->font, &down_arrow, 1);
    title_w += XTextWidth(panel->font, part3, strlen(part3));
    title_w += XTextWidth16(panel->font, &left_arrow, 1);
    title_w += XTextWidth(panel->font, part4, strlen(part4));
    title_w += XTextWidth16(panel->font, &right_arrow, 1);
    title_w += XTextWidth(panel->font, part5, strlen(part5));

    int inner_w = title_w > total_content_w ? title_w : total_content_w;
    int win_w = left_margin * 2 + inner_w;
    int win_h = top_margin * 2 + num_config_items * line_h + 20;
    int win_x = (scr_w - win_w) / 2;
    int win_y = (scr_h - win_h) / 2;

    Window root = DefaultRootWindow(panel->dpy);

    XSetWindowAttributes attrs = {0};
    attrs.override_redirect = True;
    attrs.background_pixel = 0xFFFFFF;
    attrs.border_pixel = 0x000000;
    attrs.event_mask = KeyPressMask;

    panel->win = XCreateWindow(panel->dpy, root, win_x, win_y, win_w, win_h, 2,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
                               &attrs);

    panel->gc = XCreateGC(panel->dpy, panel->win, 0, NULL);
    XSetForeground(panel->dpy, panel->gc, 0x000000);
    if (panel->font) XSetFont(panel->dpy, panel->gc, panel->font->fid);

    XMapWindow(panel->dpy, panel->win);
    XStoreName(panel->dpy, panel->win, "Mouse Daemon Config");

    draw_config_panel(panel);

    while (panel->active) {
        usleep(10000);
    }

    if (panel->win != None) XDestroyWindow(panel->dpy, panel->win);
    if (panel->gc) XFreeGC(panel->dpy, panel->gc);
    if (panel->font) XFreeFont(panel->dpy, panel->font);
    if (panel->dpy) XCloseDisplay(panel->dpy);

    panel->win = None;
    panel->dpy = NULL;
    panel->gc = None;
    panel->font = NULL;

    return NULL;
}

static void show_config_panel(ConfigPanel *panel) {
    pthread_mutex_lock(&panel->mutex);
    if (panel->active) {
        pthread_mutex_unlock(&panel->mutex);
        return;
    }
    panel->active = true;

    pthread_mutex_lock(&state.state_mutex);
    memset(state.numpad_keys_pressed, 0, sizeof(state.numpad_keys_pressed));
    memset(state.scroll_keys_pressed, 0, sizeof(state.scroll_keys_pressed));
    state.autoscroll_up_active = state.autoscroll_down_active = false;
    pthread_mutex_unlock(&state.state_mutex);

    if (state.left_button_held) {
        emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
        emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
        state.left_button_held = false;
		state.drag_locked = false;
		hide_drag_popup();
    }

    panel->last_activity_time = get_time();

    Display *temp_dpy = XOpenDisplay(NULL);
    if (temp_dpy) {
        Window root, child;
        int win_x, win_y;
        unsigned int mask;
        XQueryPointer(temp_dpy, DefaultRootWindow(temp_dpy), &root, &child, &panel->initial_mouse_x, &panel->initial_mouse_y, &win_x, &win_y, &mask);
        XCloseDisplay(temp_dpy);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&panel->timer_thread, &attr, panel_timer_func, panel);
    pthread_create(&panel->mouse_monitor_thread, &attr, mouse_monitor_func, panel);
    pthread_attr_destroy(&attr);

    pthread_t th;
    pthread_create(&th, NULL, config_panel_thread, panel);
    pthread_detach(th);
    pthread_mutex_unlock(&panel->mutex);
}

static void hide_config_panel(ConfigPanel *panel) {
    pthread_mutex_lock(&panel->mutex);
    panel->active = false;
    state.autoscroll_up_active = state.autoscroll_down_active = false;

    save_config();

    if (state.left_button_held) {
        emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
        emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
        state.left_button_held = false;
		state.drag_locked = false;
		hide_drag_popup();
    }

    panel->selected_row = 0;

    pthread_mutex_unlock(&panel->mutex);
    hide_margin_overlay();
    show_jump_overlay(0);
}

/* ------------------------------------------------------------------ */
/* Overlay Functions (Margin and Jump)                                */
/* ------------------------------------------------------------------ */
static bool margin_overlay_visible = false;
static Window margin_window = None;
static pthread_t margin_thread = 0;
static double last_margin_adjust = 0.0;
static bool margin_need_redraw = false;
static pthread_mutex_t margin_mutex = PTHREAD_MUTEX_INITIALIZER;

static void show_margin_overlay(void) {
    pthread_mutex_lock(&margin_mutex);
    if (margin_thread && !margin_overlay_visible) {
        pthread_join(margin_thread, NULL);
        margin_thread = 0;
    }
    if (margin_overlay_visible) {
        last_margin_adjust = get_time();
        margin_need_redraw = true;
        pthread_mutex_unlock(&margin_mutex);
        return;
    }
    margin_overlay_visible = true;
    last_margin_adjust = get_time();
    margin_need_redraw = true;
    pthread_create(&margin_thread, NULL, margin_overlay_thread, (void*)&state);
    pthread_mutex_unlock(&margin_mutex);
}

static void hide_margin_overlay(void) {
    pthread_mutex_lock(&margin_mutex);
    if (!margin_overlay_visible) {
        pthread_mutex_unlock(&margin_mutex);
        return;
    }
    margin_overlay_visible = false;
    pthread_mutex_unlock(&margin_mutex);
    if (margin_thread) {
        pthread_join(margin_thread, NULL);
        margin_thread = 0;
    }
}

static void* margin_overlay_thread(void *arg) {
    AppState *app_state = (AppState *)arg;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return NULL;

    int scr = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);
    int scr_w = DisplayWidth(dpy, scr);
    int scr_h = DisplayHeight(dpy, scr);

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo)) {
        XCloseDisplay(dpy);
        return NULL;
    }

    XSetWindowAttributes attrs = {0};
    attrs.override_redirect = True;
    attrs.background_pixel = 0x00000000;
    attrs.border_pixel = 0;
    attrs.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);

    Window win = XCreateWindow(dpy, root, 0, 0, scr_w, scr_h, 0,
                               32, InputOutput, vinfo.visual,
                               CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap,
                               &attrs);

    Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False),
                    XA_ATOM, 32, PropModeAppend, (unsigned char *)&above, 1);

    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    unsigned long pixel = 0x33000000UL;
    XSetForeground(dpy, gc, pixel);
    XSetLineAttributes(dpy, gc, 2, LineSolid, CapButt, JoinMiter);

    margin_window = win;

    while (margin_overlay_visible) {
        pthread_mutex_lock(&margin_mutex);
        double now = get_time();
        if (now - last_margin_adjust > MARGIN_OVERLAY_TIMEOUT) {
            margin_overlay_visible = false;
            pthread_mutex_unlock(&margin_mutex);
            break;
        }

        if (margin_need_redraw) {
            margin_need_redraw = false;

            XLockDisplay(dpy);
            XClearWindow(dpy, win);

            int margin = app_state->cfg.jump_margin;
            if (margin < 0) margin = 0;
            int x = margin;
            int y = margin;
            int w = scr_w - 2 * margin;
            int h = scr_h - 2 * margin;
            if (w > 0 && h > 0) {
                XDrawRectangle(dpy, win, gc, x, y, w, h);
            }

            XFlush(dpy);
            XUnlockDisplay(dpy);
        }
        pthread_mutex_unlock(&margin_mutex);

        usleep(20000);
    }

    XLockDisplay(dpy);
    XUnmapWindow(dpy, win);
    XDestroyWindow(dpy, win);
    XFreeGC(dpy, gc);
    XUnlockDisplay(dpy);
    XCloseDisplay(dpy);
    margin_window = None;
    return NULL;
}

static int adjusting_jump_type = 0;
static bool jump_overlay_visible = false;
static Window jump_window = None;
static pthread_t jump_thread = 0;
static double last_jump_adjust = 0.0;
static bool jump_need_redraw = false;
static pthread_mutex_t jump_mutex = PTHREAD_MUTEX_INITIALIZER;

static void show_jump_overlay(int type) {
    pthread_mutex_lock(&jump_mutex);
    if (jump_thread && !jump_overlay_visible) {
        pthread_join(jump_thread, NULL);
        jump_thread = 0;
    }
    if (jump_overlay_visible && adjusting_jump_type != type) {
        jump_overlay_visible = false;
        pthread_mutex_unlock(&jump_mutex);
        if (jump_thread) {
            pthread_join(jump_thread, NULL);
            jump_thread = 0;
        }
        pthread_mutex_lock(&jump_mutex);
    }
    adjusting_jump_type = type;
    if (type == 0) {
        jump_overlay_visible = false;
        pthread_mutex_unlock(&jump_mutex);
        if (jump_thread) {
            pthread_join(jump_thread, NULL);
            jump_thread = 0;
        }
        return;
    }
    jump_overlay_visible = true;
    last_jump_adjust = get_time();
    jump_need_redraw = true;
    pthread_mutex_unlock(&jump_mutex);
    if (!jump_thread) {
        pthread_create(&jump_thread, NULL, jump_overlay_thread, (void*)&state);
    }
}

static void hide_jump_overlay(void) {
    show_jump_overlay(0);
}

static void* jump_overlay_thread(void *arg) {
    AppState *app_state = (AppState *)arg;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return NULL;

    int scr = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);
    int scr_w = DisplayWidth(dpy, scr);
    int scr_h = DisplayHeight(dpy, scr);

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo)) {
        XCloseDisplay(dpy);
        return NULL;
    }

    XSetWindowAttributes attrs = {0};
    attrs.override_redirect = True;
    attrs.background_pixel = 0x00000000;
    attrs.border_pixel = 0;
    attrs.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);

    Window win = XCreateWindow(dpy, root, 0, 0, scr_w, scr_h, 0,
                               32, InputOutput, vinfo.visual,
                               CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap,
                               &attrs);

    Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False),
                    XA_ATOM, 32, PropModeAppend, (unsigned char *)&above, 1);

    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    unsigned long pixel = 0x33000000UL;
    XSetForeground(dpy, gc, pixel);

    jump_window = win;

    while (jump_overlay_visible) {
        pthread_mutex_lock(&jump_mutex);
        double now = get_time();
        if (now - last_jump_adjust > JUMP_OVERLAY_TIMEOUT) {
            jump_overlay_visible = false;
            pthread_mutex_unlock(&jump_mutex);
            break;
        }

        int current_type = adjusting_jump_type;
        if (jump_need_redraw && current_type > 0) {
            jump_need_redraw = false;

            XLockDisplay(dpy);
            XClearWindow(dpy, win);
            int r = 3;

            if (current_type == 1) {
                if (app_state->cfg.jump_horizontal > 0) {
                    int y = scr_h / 2;
                    for (int x = 0; x < scr_w; x += app_state->cfg.jump_horizontal) {
                        XFillRectangle(dpy, win, gc, x - r, y - r, 2 * r + 1, 2 * r + 1);
                    }
                }
            } else if (current_type == 2) {
                if (app_state->cfg.jump_vertical > 0) {
                    int x = scr_w / 2;
                    for (int y = 0; y < scr_h; y += app_state->cfg.jump_vertical) {
                        XFillRectangle(dpy, win, gc, x - r, y - r, 2 * r + 1, 2 * r + 1);
                    }
                }
            } else if (current_type == 3) {
                if (app_state->cfg.jump_diagonal > 0) {
                    double len = hypot((double)scr_w, (double)scr_h);
                    double t_step = (double)app_state->cfg.jump_diagonal / len;

                    for (double t = 0; t < 1.0; t += t_step) {
                        int px = (int)(t * scr_w);
                        int py = (int)(t * scr_h);
                        XFillRectangle(dpy, win, gc, px - r, py - r, 2 * r + 1, 2 * r + 1);
                    }

                    for (double t = 0; t < 1.0; t += t_step) {
                        int px = (int)(scr_w * (1.0 - t));
                        int py = (int)(t * scr_h);
                        XFillRectangle(dpy, win, gc, px - r, py - r, 2 * r + 1, 2 * r + 1);
                    }
                }
            }
            XFlush(dpy);
            XUnlockDisplay(dpy);
        }
        pthread_mutex_unlock(&jump_mutex);

        usleep(20000);
    }

    XLockDisplay(dpy);
    XUnmapWindow(dpy, win);
    XDestroyWindow(dpy, win);
    XFreeGC(dpy, gc);
    XUnlockDisplay(dpy);
    XCloseDisplay(dpy);
    jump_window = None;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Device Handling Functions                                          */
/* ------------------------------------------------------------------ */
static bool is_keyboard(const char *device_path) {
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) return false;

    unsigned long key_bits[NBITS(KEY_MAX)];
    memset(key_bits, 0, sizeof(key_bits));

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        close(fd);
        return false;
    }

    bool has_a = test_bit(KEY_A, key_bits);
    bool has_space = test_bit(KEY_SPACE, key_bits);
    bool has_ctrl = test_bit(KEY_LEFTCTRL, key_bits);

    close(fd);
    return has_a && has_space && has_ctrl;
}

static bool is_virtual_device(const char *device_path) {
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) return false;

    char name[256] = "";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    close(fd);

    return strstr(name, "evdev") || strstr(name, "uinput") ||
           strstr(name, "Virtual") || strstr(name, "py-");
}

static int find_all_keyboards(void) {
    DIR *dir = opendir(INPUT_DIR);
    if (!dir) {
        perror("Cannot open /dev/input");
        return -1;
    }

    struct dirent *entry;
    char device_path[PATH_MAX];
    state.num_kbds = 0;

    while ((entry = readdir(dir)) != NULL && state.num_kbds < MAX_KBDS) {
        if (strncmp(entry->d_name, "event", 5) == 0) {
            snprintf(device_path, sizeof(device_path), "%s/%s", INPUT_DIR, entry->d_name);

            if (is_virtual_device(device_path)) continue;

            if (is_keyboard(device_path)) {
                int fd = open(device_path, O_RDONLY);
                if (fd < 0) continue;

                if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                    perror("Cannot grab keyboard");
                    close(fd);
                    continue;
                }

                char name[256] = "Unknown";
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                printf("Grabbed keyboard: %s (%s)\n", name, device_path);

                state.kbd_fds[state.num_kbds++] = fd;
            }
        }
    }

    closedir(dir);
    if (state.num_kbds == 0) {
        fprintf(stderr, "Error: Could not find any keyboard devices\n");
        return -1;
    }
    return 0;
}

static int create_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Cannot open /dev/uinput");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    for (int i = 0; i < KEY_MAX; i++) {
        ioctl(fd, UI_SET_KEYBIT, i);
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    strcpy(usetup.name, "Virtual Mouse Daemon Keyboard");

    ioctl(fd, UI_DEV_SETUP, &usetup);
    ioctl(fd, UI_DEV_CREATE);
    usleep(10000);

    printf("Virtual keyboard created\n");
    return fd;
}

static int create_mouse(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Cannot open /dev/uinput for mouse");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);

    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5679;
    strcpy(usetup.name, "Virtual Mouse Daemon Mouse");

    ioctl(fd, UI_DEV_SETUP, &usetup);
    ioctl(fd, UI_DEV_CREATE);
    usleep(10000);

    printf("Virtual mouse created\n");
    return fd;
}

/* ------------------------------------------------------------------ */
/* Key Handling Functions                                             */
/* ------------------------------------------------------------------ */
static bool handle_ctrl_key(const struct input_event *ev, double *last_ctrl_press) {
    if (ev->code != KEY_LEFTCTRL && ev->code != KEY_RIGHTCTRL) return false;

    bool *forwarded = (ev->code == KEY_LEFTCTRL) ? &state.left_ctrl_forwarded : &state.right_ctrl_forwarded;

    pthread_mutex_lock(&state.state_mutex);
    if (ev->value != 0) {
        state.ctrl_pressed = true;
    } else {
        state.ctrl_pressed = false;
    }
    pthread_mutex_unlock(&state.state_mutex);

    if (ev->value == 1) {
        double current_time = get_time();
        double time_since_last = current_time - *last_ctrl_press;
        if (time_since_last < DOUBLE_PRESS_THRESHOLD) {
            pthread_mutex_lock(&state.state_mutex);
            state.mouse_mode = !state.mouse_mode;
            bool was_autoscroll = false;
            if (!state.mouse_mode) {
                was_autoscroll = state.autoscroll_up_active || state.autoscroll_down_active;
                state.autoscroll_up_active = false;
                state.autoscroll_down_active = false;
            }
            pthread_mutex_unlock(&state.state_mutex);

            const char *msg;
            if (state.mouse_mode) {
                msg = "Mouse Mode ON";
            } else if (was_autoscroll) {
                msg = "Mouse Mode and Autoscroll OFF";
            } else {
                msg = "Mouse Mode OFF";
            }
            show_feedback(msg);

            *last_ctrl_press = 0.0;  // reset to prevent triple press issues
            return true;
        }
        *last_ctrl_press = current_time;
        state.pending_ctrl = true;
        state.pending_ctrl_code = ev->code;
        return true;
    } else if (ev->value == 0) {
        if (*forwarded) {
            emit_event(state.uinput_fd, EV_KEY, ev->code, 0);
            emit_event(state.uinput_fd, EV_SYN, SYN_REPORT, 0);
            *forwarded = false;
        }
        return true;
    }
    return false;
}

static bool handle_alt_key(const struct input_event *ev, ConfigPanel *panel) {
    if (ev->code != KEY_LEFTALT && ev->code != KEY_RIGHTALT) return false;

    pthread_mutex_lock(&state.state_mutex);
    state.alt_pressed = (ev->value != 0);
    pthread_mutex_unlock(&state.state_mutex);

    if (ev->value == 1) {
        double now = get_time();
        if (state.mouse_mode && (now - state.last_alt_press) < ALT_DOUBLE_THRESHOLD && (now - state.last_alt_press) > 0.01) {
            show_config_panel(panel);
        }
        state.last_alt_press = now;
    }

    if (!panel->active) {
        emit_event(state.uinput_fd, EV_KEY, ev->code, ev->value);
        emit_event(state.uinput_fd, EV_SYN, SYN_REPORT, 0);
    }

    return true;
}

static bool handle_shift_key(const struct input_event *ev) {
    if (ev->code != KEY_LEFTSHIFT && ev->code != KEY_RIGHTSHIFT) return false;

    pthread_mutex_lock(&state.state_mutex);
    state.shift_pressed = (ev->value != 0);
    pthread_mutex_unlock(&state.state_mutex);

    if (!(state.autoscroll_up_active || state.autoscroll_down_active)) {
        emit_event(state.uinput_fd, EV_KEY, ev->code, ev->value);
        emit_event(state.uinput_fd, EV_SYN, SYN_REPORT, 0);
    }

    return true;
}

static bool handle_esc_in_panel(const struct input_event *ev, ConfigPanel *panel) {
    if (ev->code != KEY_ESC || ev->value != 1) return false;

    hide_config_panel(panel);
    return true;
}

static void disable_autoscroll_if_not_allowed(const struct input_event *ev) {
    if (ev->value != 1) return;

    bool is_allowed = (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL ||
                       ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT ||
                       ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT ||
                       (state.ctrl_pressed && (ev->code == KEY_KPPLUS || ev->code == KEY_KPENTER || (state.shift_pressed && ev->code == KEY_EQUAL))) ||
                       (state.alt_pressed && (ev->code == KEY_KPPLUS || ev->code == KEY_KPENTER || (state.shift_pressed && ev->code == KEY_EQUAL) || ev->code == KEY_KPDOT)) ||
                       (state.alt_pressed && state.shift_pressed && ev->code == KEY_KPDOT));

    if (!is_allowed && (state.autoscroll_up_active || state.autoscroll_down_active)) {
        state.autoscroll_up_active = false;
        state.autoscroll_down_active = false;
        double now = get_time();
        if (now - state.last_autoscroll_feedback > 0.8) {
            state.last_autoscroll_feedback = now;
            show_feedback("Autoscroll OFF");
        }
    }
}

static bool handle_panel_nav_key(const struct input_event *ev, ConfigPanel *panel) {
    bool is_nav = (ev->code == KEY_KP8 || ev->code == KEY_UP ||
                   ev->code == KEY_KP2 || ev->code == KEY_DOWN ||
                   ev->code == KEY_KP4 || ev->code == KEY_LEFT ||
                   ev->code == KEY_KP6 || ev->code == KEY_RIGHT);

    if (!is_nav) return false;

    if (ev->value != 1 && ev->value != 2) return true;

    double now = get_time();
    int multi = 1;
    if (ev->value == 1) {
        state.adjust_start_times[ev->code] = now;
    } else {
        double elapsed = now - state.adjust_start_times[ev->code];
        multi = get_step_multiplier(elapsed);
    }

    switch (ev->code) {
        case KEY_KP8:
        case KEY_UP:
            panel->selected_row = (panel->selected_row - 1 + num_config_items) % num_config_items;
            break;
        case KEY_KP2:
        case KEY_DOWN:
            panel->selected_row = (panel->selected_row + 1) % num_config_items;
            break;
        case KEY_KP4:
        case KEY_LEFT:
            adjust_config_value(panel->selected_row, -multi, false);
            break;
        case KEY_KP6:
        case KEY_RIGHT:
            adjust_config_value(panel->selected_row, multi, false);
            break;
        default:
            return false;
    }

    panel->last_activity_time = now;
    draw_config_panel(panel);

    return true;
}

static bool handle_non_nav_in_panel(const struct input_event *ev, ConfigPanel *panel) {
    if (ev->value != 1) return false;

    hide_config_panel(panel);
    emit_event(state.uinput_fd, EV_KEY, ev->code, ev->value);
    emit_event(state.uinput_fd, EV_SYN, SYN_REPORT, 0);

    return true;
}

static bool handle_alt_adjustment_key(const struct input_event *ev) {
    if (!state.alt_pressed || (ev->value != 1 && ev->value != 2)) return false;

    int index = -1;
    bool increase = true;
    double now = get_time();

    switch (ev->code) {
        case KEY_KPPLUS:
        case KEY_KPENTER:
            index = (state.autoscroll_up_active || state.autoscroll_down_active) ? 11 : 9;
            increase = (ev->code == KEY_KPPLUS);
            break;
        case KEY_NUMLOCK:
            index = 0;
            increase = !state.shift_pressed;
            break;
        case KEY_KPSLASH:
            index = 3;
            increase = !state.shift_pressed;
            break;
        case KEY_KPASTERISK:
            index = 1;
            increase = !state.shift_pressed;
            break;
        case KEY_KPMINUS:
            index = 2;
            increase = !state.shift_pressed;
            break;
        case KEY_KP5:
            index = 7;
            increase = !state.shift_pressed;
            show_margin_overlay();
            last_margin_adjust = now;
            margin_need_redraw = true;
            break;
        case KEY_KP6:
        case KEY_KP4:
            index = 4;
            increase = (ev->code == KEY_KP6);
            show_jump_overlay(1);
            last_jump_adjust = now;
            jump_need_redraw = true;
            break;
        case KEY_KP8:
        case KEY_KP2:
            index = 5;
            increase = (ev->code == KEY_KP8);
            show_jump_overlay(2);
            last_jump_adjust = now;
            jump_need_redraw = true;
            break;
        case KEY_KP7:
        case KEY_KP9:
        case KEY_KP1:
        case KEY_KP3:
            index = 6;
            increase = (ev->code == KEY_KP7 || ev->code == KEY_KP9);
            show_jump_overlay(3);
            last_jump_adjust = now;
            jump_need_redraw = true;
            break;
        case KEY_KP0:
            index = 8;
            increase = !state.shift_pressed;
            break;
        case KEY_KPDOT:
            index = (state.autoscroll_up_active || state.autoscroll_down_active) ? 12 : 10;
            increase = !state.shift_pressed;
            break;
        default:
            return false;
    }

    int multi = 1;
    if (ev->value == 1) {
        state.adjust_start_times[ev->code] = now;
    } else {
        double elapsed = now - state.adjust_start_times[ev->code];
        multi = get_step_multiplier(elapsed);
    }
    int step = multi * (increase ? 1 : -1);
    adjust_config_value(index, step, true);

    save_config();
    return true;
}

static bool handle_scroll_and_autoscroll_key(const struct input_event *ev) {
    if (ev->code != KEY_KPPLUS && ev->code != KEY_KPENTER) return false;

    bool is_plus = (ev->code == KEY_KPPLUS);

    if (!state.ctrl_pressed) {
        pthread_mutex_lock(&state.state_mutex);
        state.scroll_keys_pressed[is_plus ? 0 : 1] = (ev->value != 0);
        pthread_mutex_unlock(&state.state_mutex);
        return true;
    }

    if (ev->value != 1) return true;

    if (is_plus) {
        state.autoscroll_up_active = !state.autoscroll_up_active;
        if (state.autoscroll_up_active) state.autoscroll_down_active = false;
    } else {
        state.autoscroll_down_active = !state.autoscroll_down_active;
        if (state.autoscroll_down_active) state.autoscroll_up_active = false;
    }

    pthread_mutex_lock(&state.state_mutex);
    state.scroll_keys_pressed[0] = state.scroll_keys_pressed[1] = false;
    pthread_mutex_unlock(&state.state_mutex);

    double now = get_time();
    if (now - state.last_autoscroll_feedback > 0.8) {
        state.last_autoscroll_feedback = now;
        if (state.autoscroll_up_active)
            show_feedback("Autoscroll UP ON");
        else if (state.autoscroll_down_active)
            show_feedback("Autoscroll DOWN ON");
        else
            show_feedback("Autoscroll OFF");
    }

    state.pending_ctrl = false;  // NEW LINE HERE

    return true;
}

static bool handle_ctrl_minus_key(const struct input_event *ev) {
    if (!state.ctrl_pressed || ev->code != KEY_KPMINUS || ev->value == 0) return false;
    state.pending_ctrl = false;  // NEW LINE HERE
    return true;
}

static bool handle_absolute_jump_key(const struct input_event *ev) {
    if (!state.ctrl_pressed || !state.shift_pressed || ev->value != 1) return false;

    bool is_jump_key = (ev->code == KEY_KP8 || ev->code == KEY_KP2 || ev->code == KEY_KP4 || ev->code == KEY_KP6 ||
                        ev->code == KEY_KP9 || ev->code == KEY_KP7 || ev->code == KEY_KP3 || ev->code == KEY_KP1 || ev->code == KEY_KP5);

    if (!is_jump_key) return false;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return true;

    int scr = DefaultScreen(dpy);
    int scr_w = DisplayWidth(dpy, scr);
    int scr_h = DisplayHeight(dpy, scr);
    int margin = state.cfg.jump_margin;
    int left = margin;
    int right = scr_w - 1 - margin;
    int top = margin;
    int bottom = scr_h - 1 - margin;
    int center_x = scr_w / 2;
    int center_y = scr_h / 2;

    Window root, child;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    XQueryPointer(dpy, DefaultRootWindow(dpy), &root, &child, &root_x, &root_y, &win_x, &win_y, &mask);

    int target_x = root_x;
    int target_y = root_y;

    switch (ev->code) {
        case KEY_KP8:
            target_y = top;
            if (root_y == top) target_x = center_x;
            break;
        case KEY_KP2:
            target_y = bottom;
            if (root_y == bottom) target_x = center_x;
            break;
        case KEY_KP4:
            target_x = left;
            if (root_x == left) target_y = center_y;
            break;
        case KEY_KP6:
            target_x = right;
            if (root_x == right) target_y = center_y;
            break;
        case KEY_KP9:
            target_x = right;
            target_y = top;
            break;
        case KEY_KP7:
            target_x = left;
            target_y = top;
            break;
        case KEY_KP3:
            target_x = right;
            target_y = bottom;
            break;
        case KEY_KP1:
            target_x = left;
            target_y = bottom;
            break;
        case KEY_KP5:
            target_x = center_x;
            target_y = center_y;
            break;
        default:
            XCloseDisplay(dpy);
            return false;
    }

    warp_mouse(dpy, target_x, target_y);
    XCloseDisplay(dpy);

    state.pending_ctrl = false;  // NEW LINE HERE

    return true;
}

static bool handle_numpad_direction_key(const struct input_event *ev) {
    bool is_dir = (ev->code == KEY_KP1 || ev->code == KEY_KP2 || ev->code == KEY_KP3 ||
                   ev->code == KEY_KP4 || ev->code == KEY_KP6 ||
                   ev->code == KEY_KP7 || ev->code == KEY_KP8 || ev->code == KEY_KP9);
    if (!is_dir) return false;

    if (state.ctrl_pressed) {
        pthread_mutex_lock(&state.state_mutex);
        memset(state.numpad_keys_pressed, 0, sizeof(state.numpad_keys_pressed));
        pthread_mutex_unlock(&state.state_mutex);
        state.pending_ctrl = false;  // NEW LINE HERE
    }

    pthread_mutex_lock(&state.state_mutex);
    switch (ev->code) {
        case KEY_KP8: state.numpad_keys_pressed[0] = (ev->value != 0); break;
        case KEY_KP2: state.numpad_keys_pressed[1] = (ev->value != 0); break;
        case KEY_KP4: state.numpad_keys_pressed[2] = (ev->value != 0); break;
        case KEY_KP6: state.numpad_keys_pressed[3] = (ev->value != 0); break;
        case KEY_KP7: state.numpad_keys_pressed[4] = (ev->value != 0); break;
        case KEY_KP9: state.numpad_keys_pressed[5] = (ev->value != 0); break;
        case KEY_KP1: state.numpad_keys_pressed[6] = (ev->value != 0); break;
        case KEY_KP3: state.numpad_keys_pressed[7] = (ev->value != 0); break;
        default: 
            pthread_mutex_unlock(&state.state_mutex);
            return false;
    }
    pthread_mutex_unlock(&state.state_mutex);

    return true;
}

static bool handle_kp5_key(const struct input_event *ev) {
    if (ev->code != KEY_KP5) return false;

    if (ev->value == 1) {
        if (!state.drag_locked) {
            emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 1);
            emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
            state.left_button_held = true;
        }
    } else if (ev->value == 0) {
        if (state.left_button_held) {
            if (!(state.ctrl_pressed && state.shift_pressed)) {
                emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
                emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
                state.left_button_held = false;
            }
            if (state.drag_locked && !(state.ctrl_pressed && state.shift_pressed)) {
                state.drag_locked = false;
                hide_drag_popup();
            }
        }
    }
    return true;
}

static bool handle_kpslash_key(const struct input_event *ev) {
    if (ev->code != KEY_KPSLASH || ev->value != 1) return false;

    if (state.left_button_held) {
        emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
        emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
        state.left_button_held = false;
        state.drag_locked = false;
        hide_drag_popup();
    } else {
        emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 1);
        emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
        state.left_button_held = true;
        state.drag_locked = true;
        show_drag_popup();
    }
    return true;
}

static bool handle_kpasterisk_key(const struct input_event *ev) {
    if (ev->code != KEY_KPASTERISK || ev->value != 1) return false;

    if (state.left_button_held) {
        emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
        emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
        state.left_button_held = false;
		state.drag_locked = false;
		hide_drag_popup();
    }
    mouse_click(BTN_MIDDLE);
    return true;
}

static bool handle_kpminus_key(const struct input_event *ev) {
    if (ev->code != KEY_KPMINUS || ev->value != 1) return false;

    if (state.left_button_held) {
        emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
        emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
        state.left_button_held = false;
		state.drag_locked = false;
		hide_drag_popup();
    }
    mouse_click(BTN_RIGHT);
    return true;
}

static bool handle_numlock_key(const struct input_event *ev) {
    if (ev->code != KEY_NUMLOCK) return false;

    if (ev->value == 1) {
        if (!state.drag_locked) {
            emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 1);
            emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
            state.left_button_held = true;
        }
    } else if (ev->value == 0) {
        if (state.left_button_held) {
            emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
            emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
            state.left_button_held = false;
            if (state.drag_locked) {
                state.drag_locked = false;
                hide_drag_popup();
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Cleanup Handler                                                    */
/* ------------------------------------------------------------------ */
static void cleanup(int sig) {
    (void)sig;
    printf("\nExiting daemon...\n");
    state.running = false;
    pthread_join(state.movement_thread, NULL);

    for (int i = 0; i < state.num_kbds; i++) {
        if (state.kbd_fds[i] >= 0) {
            ioctl(state.kbd_fds[i], EVIOCGRAB, 0);
            close(state.kbd_fds[i]);
        }
    }
    if (state.uinput_fd >= 0) {
        ioctl(state.uinput_fd, UI_DEV_DESTROY);
        close(state.uinput_fd);
    }
    if (state.mouse_fd >= 0) {
        ioctl(state.mouse_fd, UI_DEV_DESTROY);
        close(state.mouse_fd);
    }

    hide_margin_overlay();
    hide_jump_overlay();
    exit(0);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";  // safety
    static char path_buf[512];
    snprintf(path_buf, sizeof(path_buf), "%s/.config/kat/config.ini", home);
    mkdir(dirname(strdup(path_buf)), 0755);  // ensure dir exists
    config_file = path_buf;

    ConfigPanel panel;
    init_config_panel(&panel);

    XInitThreads();
    load_config();

    struct input_event ev;
    double last_ctrl_press = 0.0;

    if (!XInitThreads()) {
        fprintf(stderr, "Warning: XInitThreads failed\n");
    }

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    if (find_all_keyboards() < 0) {
        return 1;
    }

    state.uinput_fd = create_uinput();
    if (state.uinput_fd < 0) cleanup(0);

    state.mouse_fd = create_mouse();
    if (state.mouse_fd < 0) cleanup(0);

    state.running = true;
    pthread_mutex_init(&state.state_mutex, NULL);
    memset(state.adjust_start_times, 0, sizeof(state.adjust_start_times));

    if (pthread_create(&state.movement_thread, NULL, movement_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create movement thread\n");
        cleanup(0);
    }

    printf("\n*** Daemon started ***\n");
    printf("Double-Ctrl → toggle mouse mode\n\n");

    struct pollfd polls[MAX_KBDS];
    for (int i = 0; i < state.num_kbds; i++) {
        polls[i].fd = state.kbd_fds[i];
        polls[i].events = POLLIN;
    }

    while (state.running) {
        int ret = poll(polls, state.num_kbds, 1000);
        if (ret > 0) {
            for (int i = 0; i < state.num_kbds; i++) {
                if (polls[i].revents & POLLIN) {
                    ssize_t n = read(polls[i].fd, &ev, sizeof(ev));
                    if (n == sizeof(ev)) {
                        if (ev.type == EV_KEY) {
                            bool consumed = false;
                            bool was_active = panel.active;
                            if (handle_ctrl_key(&ev, &last_ctrl_press)) consumed = true;
                            else if (handle_alt_key(&ev, &panel)) consumed = true;
                            else if (handle_shift_key(&ev)) consumed = true;
                            else if (panel.active && handle_esc_in_panel(&ev, &panel)) consumed = true;

                            if (state.mouse_mode) {
                                disable_autoscroll_if_not_allowed(&ev);

                                if (panel.active) {
                                    if (was_active) {
                                        if (handle_panel_nav_key(&ev, &panel)) consumed = true;
                                        else if (handle_non_nav_in_panel(&ev, &panel)) consumed = true;
                                    }
                                } else {
                                    if (handle_alt_adjustment_key(&ev)) consumed = true;
                                    else if (handle_scroll_and_autoscroll_key(&ev)) consumed = true;
                                    else if (handle_ctrl_minus_key(&ev)) consumed = true;
                                    else if (handle_absolute_jump_key(&ev)) consumed = true;
                                    else if (handle_numpad_direction_key(&ev)) consumed = true;
                                    else if (handle_kp5_key(&ev)) consumed = true;
                                    else if (handle_kpslash_key(&ev)) consumed = true;
                                    else if (handle_kpasterisk_key(&ev)) consumed = true;
                                    else if (handle_kpminus_key(&ev)) consumed = true;
									else if (handle_numlock_key(&ev)) consumed = true;
                                }
                            }

							if (state.left_button_held && ev.value == 1) {
								bool is_numpad = (ev.code >= KEY_KP7 && ev.code <= KEY_KPDOT) ||
												 ev.code == KEY_KPSLASH ||
												 ev.code == KEY_KPASTERISK ||
												 ev.code == KEY_KPENTER ||
												 ev.code == KEY_NUMLOCK;
								bool is_ctrl_or_shift = (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL ||
														 ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT);
								bool should_release = false;

								if (is_numpad) {
									if (ev.code == KEY_KP0 || ev.code == KEY_KPDOT) {
										should_release = true;
									}
								} else {
									if (!is_ctrl_or_shift) {
										should_release = true;
									}
								}

								if (should_release) {
									emit_event(state.mouse_fd, EV_KEY, BTN_LEFT, 0);
									emit_event(state.mouse_fd, EV_SYN, SYN_REPORT, 0);
									state.left_button_held = false;
									state.drag_locked = false;
									hide_drag_popup();
								}
							}

                            if (!consumed) {
                                if (state.ctrl_pressed && state.pending_ctrl) {
                                    emit_event(state.uinput_fd, EV_KEY, state.pending_ctrl_code, 1);
                                    emit_event(state.uinput_fd, EV_SYN, SYN_REPORT, 0);
                                    bool *forwarded = (state.pending_ctrl_code == KEY_LEFTCTRL) ? &state.left_ctrl_forwarded : &state.right_ctrl_forwarded;
                                    *forwarded = true;
                                    state.pending_ctrl = false;
                                }
                                emit_event(state.uinput_fd, EV_KEY, ev.code, ev.value);
                                emit_event(state.uinput_fd, EV_SYN, SYN_REPORT, 0);
                            }

                            if (ev.value == 0) {
                                state.adjust_start_times[ev.code] = 0.0;
                            }
                        } else {
                            emit_event(state.uinput_fd, ev.type, ev.code, ev.value);
                        }
                    }
                }
            }
        } else if (ret < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
    }

    cleanup(0);
    return 0;
}