# hello.py: a window on the FPC desktop, drawn by the GPU
#
#   python hello.py
#
# The window stays until its close button (or Ctrl-C in the terminal).

import fpc

w = fpc.Window("Hello", 420, 260)

w.clear()
w.gradient(0, 0, 420, 90, fpc.rgb(58, 110, 200), fpc.rgb(28, 52, 110))
w.text(24, 22, "Hello from Python", fpc.rgb(255, 255, 255), font=fpc.HEADING)
w.text(24, 54, "MicroPython on NuttX, drawn by the FPC's GPU",
       fpc.rgb(210, 225, 255))

# Round rectangles and circles, blended over each other
w.rect(24, 110, 150, 110, "card", radius=12)
w.rect(40, 126, 118, 22, "accent", radius=6)
w.text(52, 129, "a rounded card", "accent_text", font=fpc.BOLD)
for i, c in enumerate(((230, 70, 70), (70, 190, 90), (70, 120, 240))):
    w.circle(240 + i * 40, 165, 34, c, alpha=150)

w.text(24, 232, "fpc.Window: rect, circle, gradient, text", "text_dim",
       font=fpc.SMALL)

w.run()
