#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#define FONT_PATH "variable"

#define KEY_NEW  XK_Return
#define KEY_KILL XK_k

#define BUTTON_LEFT  1
#define BUTTON_RIGHT 3

#define SHELL_NAME "/bin/sh"
#define TERMINAL_NAME "xterm"

typedef struct PSWMState {
    int display_number;
    Display *dpy;
    XFontStruct *font;
    Window root;
    int exit;
} PSWMState;

int setup(PSWMState *, int);
void grab_keys(PSWMState *);
void grab_buttons(PSWMState *);
void event_main_loop(PSWMState *);
void handle_key(PSWMState *, XKeyEvent *);
void handle_button(PSWMState *, XButtonEvent *);

void spawn(PSWMState *, const char *);

int main(int argc, char **argv)
{
    int display_number;
    if (argc < 2) {
        display_number = 1;
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

    state->exit = 0;
    state->root = XDefaultRootWindow(state->dpy);

    unsigned int input_mask = KeyPressMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask;
    XSelectInput(state->dpy, state->root, input_mask);

    grab_keys(state);
    grab_buttons(state);

    return 0;
}

void grab_keys(PSWMState *state)
{
    XUngrabKey(state->dpy, AnyKey, AnyModifier, state->root);

    KeySym keys_to_grab[] = {
        KEY_NEW, KEY_KILL,
    };

#define NUM_GRABS (sizeof(keys_to_grab)/sizeof(keys_to_grab[0]))

    for (int i = 0; i < NUM_GRABS; ++i) {
        KeyCode keycode = XKeysymToKeycode(state->dpy, keys_to_grab[i]);
        unsigned int mask = ControlMask|Mod1Mask;
        XGrabKey(state->dpy, keycode, mask, state->root, True,
                GrabModeAsync, GrabModeAsync);
    }
}

void grab_buttons(PSWMState *state)
{
    XUngrabButton(state->dpy, AnyButton, AnyModifier, state->root);

#define NUM_BUTTONS 2
    unsigned int buttons[NUM_BUTTONS] = { BUTTON_LEFT, BUTTON_RIGHT };
    for (int i = 0; i < NUM_BUTTONS; ++i) {
        XGrabButton(state->dpy, buttons[i], Mod1Mask, state->root,
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
                handle_key(state, &ev.xkey);
                break;
            case ButtonPress:
                handle_button(state, &ev.xbutton);
            default:
                break;
        }
    }
}

void handle_key(PSWMState *state, XKeyEvent *ev)
{
    KeySym key = XkbKeycodeToKeysym(state->dpy, ev->keycode, 0, 0);
    switch (key) {
        case KEY_NEW:
            printf("pswm: key: New term\n");
            spawn(state, TERMINAL_NAME);
            break;
        case KEY_KILL:
            printf("pswm: key: Kill\n");
            state->exit = 1;
            break;
        default:
            break;
    }
}

void handle_button(PSWMState *state, XButtonEvent *ev)
{
    switch (ev->button) {
        case BUTTON_LEFT:
            printf("pswm: button: Left mouse\n");
            break;
        case BUTTON_RIGHT:
            printf("pswm: button: Right mouse\n");
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
