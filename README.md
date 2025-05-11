# Pretty Simple Window Manager

Inspired by [evilwm](https://www.6809.org.uk/evilwm/). Work in Progress

## Quick start
```
$ git clone https://github.com/lauchimoon/pswm.git
$ cd pswm
$ Xephyr -br -ac -noreset -screen 800x600 :1 &
$ gcc -o pswm main.c -lX11
$ DISPLAY=:1 xsetroot -solid \#400040 -cursor_name top_left_arrow
$ ./pswm 1
```

### Keys
- Return: spawn terminal
- K: kill pswm

## References
- https://github.com/nikolas/evilwm
- https://github.com/mackstann/tinywm
- https://github.com/joewing/jwm
