#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>

#define FONT_PATH "variable"

#define KEY_NEW  XK_Return

#define BUTTON_LEFT  1
#define BUTTON_RIGHT 3

#define SHELL_NAME "/bin/sh"

#define DEFAULT_MODMASK Mod1Mask
#define DEFAULT_TERM    "xterm"

#define MouseMask (ButtonPressMask|ButtonReleaseMask|PointerMotionMask)

#define max(a, b) ((a) > (b))? (a) : (b)

typedef struct PSWMConfig {
    int modmask;
    char *terminal;
} PSWMConfig;

typedef struct PSWMState {
    int display_number;
    Display *dpy;
    XFontStruct *font;
    Window root;
    int exit;

    PSWMConfig config;
    Cursor cursor_drag;
} PSWMState;

int setup(PSWMState *, int);
void grab_keys(PSWMState *);
void grab_buttons(PSWMState *);
void event_main_loop(PSWMState *);
void handle_key_press(PSWMState *, XKeyEvent *);
void handle_button_press(PSWMState *, XButtonEvent *);

void spawn(PSWMState *, const char *);
void drag_window(PSWMState *, XButtonEvent *);
void resize_window(PSWMState *, XButtonEvent *);

int main(int argc, char **argv)
{
    int display_number;
    if (argc < 2) {
        display_number = 0;
        printf("pswm: Defaulting to DISPLAY=:%d\n", display_number);
    } else
        // TODO: error checking
        display_number = atoi(argv[1]);

    PSWMState state = { 0 };
    int ret = setup(&state, display_number);
    if (ret != 0)
        return ret;

    event_main_loop(&state);

    XCloseDisplay(state.dpy);
    return 0;
}

int setup(PSWMState *state, int display_number)
{
    state->display_number = display_number;

    char display_name[256] = {};
    snprintf(display_name, 256, ":%d", state->display_number);

    state->dpy = XOpenDisplay(display_name);
    if (!state->dpy) {
        printf("pswm: Can't open display %s\n", display_name);
        return 1;
    }

    state->font = XLoadQueryFont(state->dpy, FONT_PATH);
    if (!state->font) {
        printf("pswm: Can't find font %s\n", FONT_PATH);
        return 2;
    }

    state->root = DefaultRootWindow(state->dpy);
    state->exit = 0;

    state->config.modmask = DEFAULT_MODMASK;
    state->config.terminal = DEFAULT_TERM;

    state->cursor_drag = XCreateFontCursor(state->dpy, XC_fleur);

    unsigned int input_mask = KeyPressMask|MouseMask;
    XSelectInput(state->dpy, state->root, input_mask);

    grab_keys(state);
    grab_buttons(state);

    return 0;
}

void grab_keys(PSWMState *state)
{
    XUngrabKey(state->dpy, AnyKey, AnyModifier, state->root);

    KeySym keys_to_grab[] = {
        KEY_NEW,
    };

#define NUM_GRABS (sizeof(keys_to_grab)/sizeof(keys_to_grab[0]))

    for (int i = 0; i < NUM_GRABS; ++i) {
        KeyCode keycode = XKeysymToKeycode(state->dpy, keys_to_grab[i]);
        XGrabKey(state->dpy, keycode, state->config.modmask, state->root, True,
                GrabModeAsync, GrabModeAsync);
        XGrabKey(state->dpy, keycode, state->config.modmask|LockMask, state->root, True,
                GrabModeAsync, GrabModeAsync);
    }
}

void grab_buttons(PSWMState *state)
{
    XUngrabButton(state->dpy, AnyButton, AnyModifier, state->root);

#define NUM_BUTTONS 2
    unsigned int buttons[NUM_BUTTONS] = { BUTTON_LEFT, BUTTON_RIGHT };
    for (int i = 0; i < NUM_BUTTONS; ++i) {
        XGrabButton(state->dpy, buttons[i], state->config.modmask, state->root,
                True, ButtonPressMask|ButtonReleaseMask|PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    }
}

void event_main_loop(PSWMState *state)
{
    XEvent ev;
    while (!state->exit) {
        XNextEvent(state->dpy, &ev);

        switch (ev.type) {
            case KeyPress:
                if ((ev.xkey.state & state->config.modmask) == state->config.modmask)
                    handle_key_press(state, &ev.xkey);
                break;
            case ButtonPress:
                if (ev.xbutton.subwindow != None)
                    handle_button_press(state, &ev.xbutton);
            default:
                break;
        }
    }
}

void handle_key_press(PSWMState *state, XKeyEvent *ev)
{
    KeySym key = XkbKeycodeToKeysym(state->dpy, ev->keycode, 0, 0);
    switch (key) {
        case KEY_NEW:
            spawn(state, state->config.terminal);
            break;
        default:
            break;
    }
}

void handle_button_press(PSWMState *state, XButtonEvent *ev)
{
    switch (ev->button) {
        case BUTTON_LEFT:
            drag_window(state, ev);
            break;
        case BUTTON_RIGHT:
            resize_window(state, ev);
            break;
        default:
            break;
    }
}

void spawn(PSWMState *state, const char *cmd)
{
    if (!fork()) {
        char display_string[256] = { 0 };
        snprintf(display_string, 256, "DISPLAY=:%d", state->display_number);
        putenv(display_string);

        setsid();
        execl(SHELL_NAME, SHELL_NAME, "-c", cmd, NULL);
        exit(0);
    }
}

void drag_window(PSWMState *state, XButtonEvent *ev)
{
    XWindowAttributes attr;
    XGetWindowAttributes(state->dpy, ev->subwindow, &attr);

    XEvent xev;

    XRaiseWindow(state->dpy, ev->subwindow);

    for (;;) {
        XMaskEvent(state->dpy, MouseMask, &xev);
        switch (xev.type) {
            case MotionNotify:
                if (xev.xmotion.root != ev->root)
                    break;

                int xdiff = xev.xbutton.x_root - ev->x_root;
                int ydiff = xev.xbutton.y_root - ev->y_root;
                XMoveWindow(state->dpy, ev->subwindow, attr.x + xdiff, attr.y + ydiff);
                break;
            case ButtonRelease:
                ev->subwindow = None;
                return;
            default: break;
        }
    }
}

void resize_window(PSWMState *state, XButtonEvent *ev)
{
    XWindowAttributes attr;
    XGetWindowAttributes(state->dpy, ev->subwindow, &attr);

    XEvent xev;

    XRaiseWindow(state->dpy, ev->subwindow);

    for (;;) {
        XMaskEvent(state->dpy, MouseMask, &xev);
        switch (xev.type) {
            case MotionNotify:
                if (xev.xmotion.root != ev->root)
                    break;

                int xdiff = xev.xbutton.x_root - ev->x_root;
                int ydiff = xev.xbutton.y_root - ev->y_root;
                XResizeWindow(state->dpy, ev->subwindow,
                              max(1, attr.width + xdiff), max(1, attr.height + ydiff));
                break;
            case ButtonRelease:
                ev->subwindow = None;
                return;
            default: break;
        }
    }
}
