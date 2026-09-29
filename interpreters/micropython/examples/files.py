# files.py: the files of a folder, with their sizes, in a list
#
#   python files.py [FOLDER]
#
# A double click (or Enter) on a folder opens it; ".." goes up.

import os
import sys
import fpc

folder = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
w = fpc.Window("Files: " + folder, 420, 360)
w.clear()
title = w.label(folder, 12, 10, 396, font=fpc.BOLD)
names = []


def size_of(n):
    for unit in ("B", "KB", "MB"):
        if n < 1024:
            return "%d %s" % (n, unit)
        n //= 1024
    return "%d GB" % n


def load(path):
    global folder, names
    folder = path
    entries = []
    for name in os.listdir(path):
        st = os.stat(path.rstrip("/") + "/" + name)
        isdir = st[0] & 0x4000
        entries.append((not isdir, name, st[6], isdir))
    entries.sort()
    names = [".."] + [e[1] + ("/" if e[3] else "") for e in entries]
    rows = [".."] + ["%s%s  %s" % (e[1], "/" if e[3] else "",
                                  "" if e[3] else size_of(e[2]))
                     for e in entries]
    files.items = rows
    title.text = path
    w.title("Files: " + path)


def open_row(lst):
    row = lst.value
    if row is None:
        return
    name = names[row]
    if name == "..":
        parent = folder.rstrip("/").rsplit("/", 1)[0] or "/"
        load(parent)
    elif name.endswith("/"):
        load(folder.rstrip("/") + "/" + name[:-1])


files = w.list(12, 40, 396, 306, on_activate=open_row)
load(folder)
w.run()
