#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/wait.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>

#define FONT_PATH "variable"

#define KEY_NEW      XK_Return
#define KEY_NEXT     XK_Tab
#define KEY_LEFT     XK_h
#define KEY_DOWN     XK_j
#define KEY_UP       XK_k
#define KEY_RIGHT    XK_l
#define KEY_MAXIMIZE XK_x

#define BUTTON_LEFT  1
#define BUTTON_RIGHT 3

#define SHELL_NAME "/bin/sh"

#define LINE_SIZE 1024
#define DEFAULT_MODMASK Mod1Mask
#define DEFAULT_TERM    "xterm"

#define MouseMask (ButtonPressMask|ButtonReleaseMask|PointerMotionMask)
#define ChildMask (SubstructureRedirectMask|SubstructureNotifyMask)

#define max(a, b) ((a) > (b))? (a) : (b)

typedef struct _PSWMClient {
    Window parent;
    Window window;
    XWindowAttributes init_attr;
    XWindowAttributes attr;
    int maximized;

    struct _PSWMClient *next;
} PSWMClient;

typedef PSWMClient *ClientList;

typedef struct PSWMConfig {
    char *path;

    int modmask;
    char *terminal;
} PSWMConfig;

typedef struct PSWMState {
    int display_number;
    Display *dpy;
    XFontStruct *font;
    Window root;
    int exit;
    int has_randr;
    int randr_base;

    PSWMConfig config;
    PSWMClient *current_client;
    ClientList clients;
    Cursor cursor_drag;
    Cursor cursor_resize;
} PSWMState;

PSWMClient *client_make_from_client(PSWMClient *);

ClientList clientlist_new(void);
void clientlist_free(ClientList);
ClientList clientlist_append(ClientList, PSWMClient *);
ClientList clientlist_delete(ClientList, PSWMClient *);
PSWMClient *init_client(PSWMState *, Window);
PSWMClient *find_client(PSWMState *, Window);

int numeric_string(char *);

int setup(PSWMState *, int);
void create_config_file(char *);
void read_config_file(FILE *, PSWMConfig *);
char **split_line(char *, int *);
unsigned int parse_modmask(char *);
char *parse_term(char *);
void grab_keys(PSWMState *);
void grab_buttons(PSWMState *);
void event_main_loop(PSWMState *);

int handle_xerror(Display *, XErrorEvent *);
void handle_key_press(PSWMState *, XKeyEvent *);
void handle_button_press(PSWMState *, XButtonEvent *);
void handle_configure_request(PSWMState *, XConfigureRequestEvent *);
void handle_map_request(PSWMState *, XMapRequestEvent *);
void handle_unmap(PSWMState *, XUnmapEvent *);
void handle_enter(PSWMState *, XCrossingEvent *);

void spawn(PSWMState *, const char *);
void next_client(PSWMState *);
void move_window(PSWMState *, KeySym, XKeyEvent *);
void maximize_window(PSWMState *, XKeyEvent *);

void drag_window(PSWMState *, XButtonEvent *);
void resize_window(PSWMState *, XButtonEvent *);

int main(int argc, char **argv)
{
    int display_number = 0;
    if (argc < 2) {
        display_number = 0;
        printf("pswm: Defaulting to DISPLAY=:%d\n", display_number);
    } else {
        char *display = argv[1];
        if (!numeric_string(display)) {
            printf("pswm: '%s' is not a valid display\n", display);
            return 1;
        }

        display_number = atoi(argv[1]);
    }

    PSWMState state = { 0 };
    int ret = setup(&state, display_number);
    if (ret != 0)
        return ret;

    event_main_loop(&state);

    free(state.config.terminal);
    free(state.config.path);
    clientlist_free(state.clients);
    XCloseDisplay(state.dpy);
    return 0;
}

PSWMClient *client_make_from_client(PSWMClient *client)
{
    PSWMClient *c = malloc(sizeof(PSWMClient));

    c->parent = client->parent;
    c->window = client->window;
    c->init_attr = client->init_attr;
    c->attr = client->attr;
    c->maximized = client->maximized;

    return c;
}

ClientList clientlist_new(void)
{
    return NULL;
}

void clientlist_free(ClientList head)
{
    PSWMClient *node = head;

    while (node->next != head) {
        PSWMClient *tmp = node;
        node = node->next;
        free(tmp);
    }
}

ClientList clientlist_append(ClientList head, PSWMClient *client)
{
    PSWMClient *c = client_make_from_client(client);
    if (!head) {
        c->next = c;
        return c;
    }

    PSWMClient *tmp;
    for (tmp = head; tmp->next != head; tmp = tmp->next)
        ;

    tmp->next = c;
    c->next = head;
    return head;
}

ClientList clientlist_delete(ClientList head, PSWMClient *client)
{
    if (!client || !head)
        return head;

    if (head == head->next) {
        free(head);
        return NULL;
    }

    PSWMClient *tmp;
    if (client == head) {
        // We must modify who the last pointer points to
        // Since we're deleting head, go to the end and point to head->next
        for (tmp = head; tmp->next != head; tmp = tmp->next)
            ;
        tmp->next = head->next;

        // Now free head
        tmp = head;
        head = head->next;
        free(tmp);
        return head;
    }

    for (tmp = head; tmp->next != head; tmp = tmp->next) {
        if (tmp->next == client) {
            free(tmp->next);
            tmp->next = client->next;
            break;
        }
    }

    return head;
}

PSWMClient *init_client(PSWMState *state, Window w)
{
    PSWMClient *c = malloc(sizeof(PSWMClient));

    c->window = w;
    c->maximized = 0;
    XGetWindowAttributes(state->dpy, c->window, &c->init_attr);
    XGetWindowAttributes(state->dpy, c->window, &c->attr);

    XSelectInput(state->dpy, c->window, EnterWindowMask);

    XSetWindowAttributes attr;
    attr.override_redirect = True;
    attr.event_mask = ChildMask | ButtonPressMask | KeyPressMask | EnterWindowMask;

    c->parent = XCreateWindow(state->dpy, state->root, c->init_attr.x, c->init_attr.y,
                              c->init_attr.width, c->init_attr.height, 0,
                              XDefaultDepth(state->dpy, 0), CopyFromParent,
                              XDefaultVisual(state->dpy, 0),
                              CWOverrideRedirect | CWBorderPixel | CWEventMask, &attr);
    return c;
}

PSWMClient *find_client(PSWMState *state, Window w)
{
    PSWMClient *client;
    for (client = state->clients; client && client->next != state->clients; client = client->next) {
        if (client->window == w || client->parent == w)
            return client;
    }

    if (client && (client->window == w || client->parent == w))
        return client;

    return NULL;
}

int numeric_string(char *s)
{
    for (int i = 0; s[i] != '\0'; ++i)
        if (!isdigit(s[i]))
            return 0;

    return 1;
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
    XSetErrorHandler(handle_xerror);

    state->font = XLoadQueryFont(state->dpy, FONT_PATH);
    if (!state->font) {
        printf("pswm: Can't find font %s\n", FONT_PATH);
        return 2;
    }

    state->root = DefaultRootWindow(state->dpy);
    state->exit = 0;

    int dummy;
    state->has_randr = XRRQueryExtension(state->dpy, &state->randr_base, &dummy);

#define PATH_SIZE 256
    state->config.path = calloc(PATH_SIZE + 1, sizeof(char));
    strcat(state->config.path, getenv("HOME"));
    strcat(state->config.path, "/.pswmrc");

    FILE *f = fopen(state->config.path, "r");
    if (!f) {
        create_config_file(state->config.path);
        f = fopen(state->config.path, "r");
    }

    read_config_file(f, &state->config);

    state->cursor_drag = XCreateFontCursor(state->dpy, XC_fleur);
    state->cursor_resize = XCreateFontCursor(state->dpy, XC_plus);

    unsigned int input_mask = KeyPressMask|MouseMask|ChildMask;
    XSelectInput(state->dpy, state->root, input_mask);
    XRRSelectInput(state->dpy, state->root, RRScreenChangeNotifyMask);

    grab_keys(state);
    grab_buttons(state);

    state->current_client = NULL;
    state->clients = clientlist_new();

    return 0;
}

void create_config_file(char *path)
{
    FILE *f = fopen(path, "w");
    fprintf(f, "mask mod1\nterm xterm\n");
    fclose(f);
}

void read_config_file(FILE *f, PSWMConfig *config)
{
    char line[LINE_SIZE] = { 0 };

    while (fgets(line, LINE_SIZE, f) != NULL) {
        int split_count = 0;
        line[strcspn(line, "\n")] = '\0';
        char **split = split_line(line, &split_count);

        if (split_count < 2)
            continue;

        if (strcmp(split[0], "mask") == 0)
            config->modmask = parse_modmask(split[1]);
        else if (strcmp(split[0], "term") == 0) {
            char *term = parse_term(split[1]);
            config->terminal = calloc(strlen(term) + 1, sizeof(char));
            strcpy(config->terminal, term);
        }

        for (int i = 0; i < split_count; ++i)
            free(split[i]);
        free(split);
    }
}

char **split_line(char *text, int *count)
{
    int capacity = 2;
    char **result = calloc(capacity, sizeof(char *));

    char buffer[LINE_SIZE] = { 0 };
    int buffer_index = 0;
    memset(buffer, 0, LINE_SIZE);

    char delim = ' ';

    for (int i = 0; ; ++i) {
        buffer[buffer_index] = text[i];
        ++buffer_index;

        if (buffer[buffer_index - 1] == delim || text[i] == '\0') {
            buffer[buffer_index - 1] = '\0';
            result[*count] = calloc(buffer_index + 1, sizeof(char));
            strcpy(result[*count], buffer);
            ++(*count);

            buffer_index = 0;

            if (text[i] == '\0')
                break;
        }
    }

    return result;
}

unsigned int parse_modmask(char *text)
{
    if (strcmp(text, "mod1") == 0) return Mod1Mask;
    else if (strcmp(text, "mod2") == 0) return Mod2Mask;
    else if (strcmp(text, "mod3") == 0) return Mod3Mask;
    else if (strcmp(text, "mod4") == 0) return Mod4Mask;
    else if (strcmp(text, "mod5") == 0) return Mod5Mask;
    else return DEFAULT_MODMASK;
}

char *parse_term(char *text)
{
    // Check if terminal is installed
    pid_t pid;
    int status;

    pid = fork();
    if (!pid)
        execl("which", text);
    else {
        wait(&status);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) // Term is not installed
            return DEFAULT_TERM;
    }

    return text;
}

void grab_keys(PSWMState *state)
{
    XUngrabKey(state->dpy, AnyKey, AnyModifier, state->root);

    KeySym keys_to_grab[] = {
        KEY_NEW, KEY_NEXT, KEY_LEFT, KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_MAXIMIZE,
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
                break;
            case ConfigureRequest:
                handle_configure_request(state, &ev.xconfigurerequest);
                break;
            case MapRequest:
                handle_map_request(state, &ev.xmaprequest);
                break;
            case UnmapNotify:
                handle_unmap(state, &ev.xunmap);
                break;
            case EnterNotify:
                handle_enter(state, &ev.xcrossing);
                break;
            default:
                if (state->has_randr && ev.type == state->randr_base + RRScreenChangeNotify)
                    XRRUpdateConfiguration(&ev);
                break;
        }
    }

}

int handle_xerror(Display *dpy, XErrorEvent *ev)
{
    char error_text[1024];
    XGetErrorText(dpy, ev->error_code, error_text, 1024);
    printf("pswm: X Error: %s\n", error_text);

    return 0;
}

void handle_key_press(PSWMState *state, XKeyEvent *ev)
{
    KeySym key = XkbKeycodeToKeysym(state->dpy, ev->keycode, 0, 0);

    switch (key) {
        case KEY_NEW:
            spawn(state, state->config.terminal);
            break;
        case KEY_NEXT:
            next_client(state);
            break;
        case KEY_LEFT: case KEY_DOWN: case KEY_UP: case KEY_RIGHT:
            if (ev->subwindow != None)
                move_window(state, key, ev);
            break;
        case KEY_MAXIMIZE:
            maximize_window(state, ev);
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

void handle_configure_request(PSWMState *state, XConfigureRequestEvent *ev)
{
    PSWMClient *client = find_client(state, ev->window);
    XWindowChanges wc;

    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;

    Window window_to_configure = client? client->parent : ev->window;
    XConfigureWindow(state->dpy, window_to_configure, ev->value_mask, &wc);
}

void handle_map_request(PSWMState *state, XMapRequestEvent *ev)
{
    PSWMClient *client = find_client(state, ev->window);
    if (!client)
        client = init_client(state, ev->window);

    state->clients = clientlist_append(state->clients, client);
    state->current_client = state->clients;

    XMapWindow(state->dpy, client->window);
    XMapWindow(state->dpy, client->parent);
    XReparentWindow(state->dpy, client->window, client->parent, 0, 0);
    XRaiseWindow(state->dpy, client->parent);
    XSetInputFocus(state->dpy, client->window, RevertToPointerRoot, CurrentTime);
}

void handle_unmap(PSWMState *state, XUnmapEvent *ev)
{
    PSWMClient *client = find_client(state, ev->window);
    if (!client)
        return;

    if (ev->event == client->parent) {
        XUnmapWindow(state->dpy, client->parent);
        XReparentWindow(state->dpy, client->parent, state->root, client->attr.x, client->attr.y);
        XDestroyWindow(state->dpy, client->parent);
        client->window = None;
        client->parent = None;
        state->clients = clientlist_delete(state->clients, client);
    }
}

void handle_enter(PSWMState *state, XCrossingEvent *ev)
{
    if (ev->mode != NotifyNormal || ev->mode == NotifyInferior)
        return;

    Window window_to_enter = (ev->subwindow != None)? ev->subwindow : ev->window;

    PSWMClient *client = find_client(state, window_to_enter);
    if (!client)
        return;

    XSetInputFocus(state->dpy, client->window, RevertToPointerRoot, CurrentTime);
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

void next_client(PSWMState *state)
{
    if (!state->clients || !state->current_client)
        return;

    PSWMClient *client = state->current_client->next;
    state->current_client = client;
    XRaiseWindow(state->dpy, state->current_client->parent);
    XSetInputFocus(state->dpy, state->current_client->window, RevertToPointerRoot, CurrentTime);
}

void move_window(PSWMState *state, KeySym key, XKeyEvent *ev)
{
    PSWMClient *client = find_client(state, ev->subwindow);
    if (!client)
        return;

    XGetWindowAttributes(state->dpy, client->parent, &client->attr);
    XRaiseWindow(state->dpy, client->parent);
    XSetInputFocus(state->dpy, client->window, RevertToPointerRoot, CurrentTime);

    switch (key) {
        case KEY_LEFT:
            XMoveWindow(state->dpy, client->parent, client->attr.x - 16, client->attr.y);
            XMoveWindow(state->dpy, client->window, 0, 0);
            break;
        case KEY_DOWN:
            XMoveWindow(state->dpy, client->parent, client->attr.x, client->attr.y + 16);
            XMoveWindow(state->dpy, client->window, 0, 0);
            break;
        case KEY_UP:
            XMoveWindow(state->dpy, client->parent, client->attr.x, client->attr.y - 16);
            XMoveWindow(state->dpy, client->window, 0, 0);
            break;
        case KEY_RIGHT:
            XMoveWindow(state->dpy, client->parent, client->attr.x + 16, client->attr.y);
            XMoveWindow(state->dpy, client->window, 0, 0);
            break;
    }

    // Update init_attr to match some current attr fields
    XGetWindowAttributes(state->dpy, client->parent, &client->attr);
    client->init_attr.x = client->attr.x;
    client->init_attr.y = client->attr.y;
}

void maximize_window(PSWMState *state, XKeyEvent *ev)
{
    PSWMClient *client = find_client(state, ev->subwindow);
    if (!client)
        return;

    int display_width = XDisplayWidth(state->dpy, 0);
    int display_height = XDisplayHeight(state->dpy, 0);

    client->maximized = !client->maximized;

    int x = client->maximized? 0 : client->init_attr.x;
    int y = client->maximized? 0 : client->init_attr.y;
    int width = client->maximized? display_width : client->init_attr.width;
    int height = client->maximized? display_height : client->init_attr.height;

    XMoveResizeWindow(state->dpy, client->parent, x, y, width, height);
    XMoveResizeWindow(state->dpy, client->window, 0, 0, width, height);
    XRaiseWindow(state->dpy, client->parent);
    XSetInputFocus(state->dpy, client->window, RevertToPointerRoot, CurrentTime);
}

void drag_window(PSWMState *state, XButtonEvent *ev)
{
    PSWMClient *client = find_client(state, ev->subwindow);
    if (!client)
        return;

    if (XGrabPointer(state->dpy, state->root,
                     False, MouseMask, GrabModeAsync, GrabModeAsync,
                     None, state->cursor_drag, CurrentTime) != GrabSuccess)
        return;

    XRaiseWindow(state->dpy, client->parent);
    XSetInputFocus(state->dpy, client->window, RevertToPointerRoot, CurrentTime);

    XEvent xev;
    for (;;) {
        XMaskEvent(state->dpy, MouseMask, &xev);
        switch (xev.type) {
            case MotionNotify:
                if (xev.xmotion.root != ev->root)
                    break;

                int xdiff = xev.xbutton.x_root - ev->x_root;
                int ydiff = xev.xbutton.y_root - ev->y_root;
                XMoveWindow(state->dpy, client->parent, client->attr.x + xdiff, client->attr.y + ydiff);
                XMoveWindow(state->dpy, client->window, 0, 0);
                break;
            case ButtonRelease:
                XUngrabPointer(state->dpy, CurrentTime);
                ev->subwindow = None;

                // Update init_attr to match some current attr fields
                XGetWindowAttributes(state->dpy, client->parent, &client->attr);
                client->init_attr.x = client->attr.x;
                client->init_attr.y = client->attr.y;

                return;
            default: break;
        }
    }
}

void resize_window(PSWMState *state, XButtonEvent *ev)
{
    PSWMClient *client = find_client(state, ev->subwindow);
    if (!client)
        return;

    if (XGrabPointer(state->dpy, state->root,
                     False, MouseMask, GrabModeAsync, GrabModeAsync,
                     None, state->cursor_resize, CurrentTime) != GrabSuccess)
        return;

    int width, height;

    XRaiseWindow(state->dpy, client->parent);
    XSetInputFocus(state->dpy, client->window, RevertToPointerRoot, CurrentTime);

    XEvent xev;
    for (;;) {
        XMaskEvent(state->dpy, MouseMask, &xev);
        switch (xev.type) {
            case MotionNotify:
                if (xev.xmotion.root != ev->root)
                    break;

                int xdiff = xev.xbutton.x_root - ev->x_root;
                int ydiff = xev.xbutton.y_root - ev->y_root;

                width = max(1, client->attr.width + xdiff);
                height = max(1, client->attr.height + ydiff);
                XResizeWindow(state->dpy, client->parent, width, height);
                XResizeWindow(state->dpy, client->window, width, height);
                break;
            case ButtonRelease:
                XUngrabPointer(state->dpy, CurrentTime);

                ev->subwindow = None;

                // Update init_attr to match some current attr fields
                XGetWindowAttributes(state->dpy, client->parent, &client->attr);
                client->init_attr.width = client->attr.width;
                client->init_attr.width = client->attr.width;
                return;
            default: break;
        }
    }
}
