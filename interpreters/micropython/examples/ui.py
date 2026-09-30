# ui.py: the desktop's widgets from Python: a button, a slider, a check
# box, a text field, and a swatch drawn with the GPU
#
#   python ui.py

import fpc

w = fpc.Window("Colour mixer", 360, 250)
w.clear()
w.label("Mix a colour", 20, 16, font=fpc.HEADING)

red = w.slider(20, 60, 200, value=200, max=255)
green = w.slider(20, 96, 200, value=120, max=255)
blue = w.slider(20, 132, 200, value=60, max=255)
info = w.label("", 20, 170, 200)
name = w.field(20, 204, 200, placeholder="a name for it")
rounded = w.check("Round", 240, 170, value=True)


def swatch(*_):
    c = (red.value, green.value, blue.value)
    w.fill(236, 56, 104, 100, "window_bg")
    w.rect(240, 60, 96, 92, c, radius=46 if rounded.value else 6)
    info.text = "#%02x%02x%02x" % c


for s in (red, green, blue):
    s.on_change = swatch
rounded.on_change = swatch


def save(b):
    print("saved", name.text or "unnamed", info.text)
    b.text = "Saved"


w.button("Save", 240, 204, 96, style=fpc.DEFAULT, on_click=save)
swatch()
w.run()
