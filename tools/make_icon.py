#!/usr/bin/env python3
"""Draws the VM2Backup icons on the same gradient tile as Speak2Mic / Show2Cam (a virtual machine and an arrow into a storage disk):
    src/VM2Backup.ico         the program
    src/VM2Backup-Setup.ico   the installer: the same glyph with a download badge
Small sizes (16-24 px) use a simplified glyph so they stay readable.
    python3 tools/make_icon.py
"""
import os
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'src')
W = 1024
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def gradient_tile(size, radius):
    """Rounded square with a diagonal violet -> cyan gradient and a soft top highlight (as Speak2Mic / Show2Cam)."""
    y, x = np.mgrid[0:size, 0:size].astype(np.float32) / (size - 1)
    t = np.clip(0.65 * y + 0.35 * x, 0, 1)[..., None]
    c0 = np.array([108, 59, 255], np.float32)
    c1 = np.array([0, 194, 255], np.float32)
    rgb = c0 * (1 - t) + c1 * t
    hl = np.clip(1 - y * 2.2, 0, 1)[..., None] * 38
    rgb = np.clip(rgb + hl, 0, 255).astype(np.uint8)
    img = Image.fromarray(np.dstack([rgb, np.full((size, size), 255, np.uint8)]), 'RGBA')
    mask = Image.new('L', (size, size), 0)
    m = int(size * 0.035)
    ImageDraw.Draw(mask).rounded_rectangle((m, m, size - m, size - m), radius=radius, fill=255)
    img.putalpha(mask)
    return img


def draw_screen(d, small):
    """A virtual machine (a window with a dark screen) and a big arrow that brings it down into a storage disk."""
    white = (255, 255, 255, 255)
    dark = (40, 30, 110, 255)
    green = (60, 220, 130, 255)
    if small:
        d.rounded_rectangle((200, 130, 824, 470), radius=70, fill=white)
        d.rectangle((250, 180, 774, 420), fill=dark)
        d.polygon([(420, 520), (604, 520), (604, 610), (700, 610), (512, 760), (324, 610), (420, 610)], fill=green)
        d.rounded_rectangle((200, 790, 824, 900), radius=50, fill=white)
        return
    d.rounded_rectangle((190, 120, 834, 480), radius=70, fill=white)                  # the VM window
    d.rounded_rectangle((240, 170, 784, 430), radius=30, fill=dark)
    d.rectangle((240, 170, 784, 205), fill=(70, 60, 160, 255))                          # its title bar
    for i, c in enumerate(((255, 95, 95, 255), (255, 200, 60, 255), (80, 220, 120, 255))):
        d.ellipse((262 + i * 34, 178, 284 + i * 34, 200), fill=c)
    d.polygon([(425, 510), (599, 510), (599, 600), (690, 600), (512, 760), (334, 600), (425, 600)], fill=green, outline=(255, 255, 255, 255))   # the arrow
    d.rounded_rectangle((190, 780, 834, 910), radius=55, fill=white)                  # the storage
    d.ellipse((730, 826, 780, 876), fill=(60, 220, 130, 255))
    d.rounded_rectangle((250, 830, 560, 862), radius=16, fill=(200, 205, 230, 255))


def draw_badge(d):
    """Download badge in the bottom right corner (installer), as Show2Cam."""
    d.ellipse((610, 610, 960, 960), fill=(30, 200, 90, 255), outline=(255, 255, 255, 255), width=28)
    d.rectangle((755, 680, 815, 820), fill=(255, 255, 255, 255))
    d.polygon([(700, 800), (870, 800), (785, 890)], fill=(255, 255, 255, 255))


def render(size, badge):
    img = gradient_tile(W, 180)
    d = ImageDraw.Draw(img)
    draw_screen(d, size <= 24)
    if badge:
        draw_badge(d)
    return img.resize((size, size), Image.LANCZOS)


def save(name, badge):
    imgs = [render(s, badge) for s in SIZES]
    imgs[-1].save(os.path.join(OUT, name), format='ICO', sizes=[(s, s) for s in SIZES], append_images=imgs[:-1])


if __name__ == '__main__':
    save('VM2Backup.ico', False)
    save('VM2Backup-Setup.ico', True)
    print('VM2Backup.ico, VM2Backup-Setup.ico written')
