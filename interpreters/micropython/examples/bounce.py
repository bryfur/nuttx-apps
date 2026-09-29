# bounce.py: an animation, a ball bouncing about a window
#
#   python bounce.py
#
# Frames come about 60 times a second (Window(frames=True)); each one the
# ball moves and is drawn again: the background where it was, then it
# where it is.  Space pauses; a click puts the ball there.

import fpc

W, H, R = 400, 300, 16
BG = fpc.rgb(24, 28, 40)
BALL = fpc.rgb(250, 190, 60)

w = fpc.Window("Bounce", W, H, frames=True)
x, y, dx, dy = 60.0, 60.0, 3.0, 2.2
running = True
bounces = 0


def draw_all():
    w.clear(BG)
    w.circle(int(x), int(y), R, BALL)
    w.text(8, 6, "bounces: %d" % bounces, fpc.rgb(150, 160, 190),
           font=fpc.SMALL)


draw_all()
while True:
    ev = w.event()
    if ev.type == fpc.CLOSE:
        break
    if ev.type == fpc.RESIZE:
        W, H = ev.w, ev.h
        draw_all()
    elif ev.type == fpc.EXPOSE:
        draw_all()
    elif ev.type == fpc.KEY and ev.char == " ":
        running = not running
    elif ev.type == fpc.MOUSE and ev.buttons & fpc.LEFT:
        x, y = float(ev.x), float(ev.y)
    elif ev.type == fpc.FRAME and running:
        ox, oy = int(x), int(y)
        x += dx
        y += dy
        if x < R or x > W - R:
            dx = -dx
            x = max(R, min(W - R, x))
            bounces += 1
        if y < R or y > H - R:
            dy = -dy
            y = max(R, min(H - R, y))
            bounces += 1
        # Where it was, then where it is: two operations, no flicker
        w.fill(ox - R, oy - R, 2 * R, 2 * R, BG)
        w.circle(int(x), int(y), R, BALL)
        w.fill(0, 0, 120, 20, BG)
        w.text(8, 6, "bounces: %d" % bounces, fpc.rgb(150, 160, 190),
               font=fpc.SMALL)
        w.flush()
