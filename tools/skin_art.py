"""The skin's artwork, drawn as SVG for the port builder's browser renderer ("art": "html"): a night-drive
background, a pedal-enclosure panel per slot (title, model field, corner screws) and a footswitch per stage colour.
Each page has its own stage colour, like a pedalboard's coloured enclosures. Coordinates are the layout's
(x 0-1280, y 86-714); every SVG is inlined into one page, so each one's ids carry its own prefix."""
import math

H_TOP = 86
TITLE_PX = 26


def rgb(c):
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def mix(a, b, t):
    return "%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(rgb(a), rgb(b)))


def title_width(s):
    """Width of a title in the panel's 26 px bold italic with 3 px tracking (Titillium Web, generously)."""
    return int(sum(10 if ch == " " else 17 if ch.isalpha() else 15 for ch in s) + 3 * len(s))


def background(uid, colour):
    """1280 x 628: indigo sky, a sliced sun low on the horizon, a perspective grid in the page's colour, scanlines."""
    w, h, hz = 1280, 628, 420
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>' % (w, h, w, h),
         '<linearGradient id="%s_sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#05020f"/>'
         '<stop offset="0.6" stop-color="#160838"/><stop offset="1" stop-color="#3d0c55"/></linearGradient>' % uid,
         '<linearGradient id="%s_sun" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffd36b"/>'
         '<stop offset="0.5" stop-color="#ff5fa2"/><stop offset="1" stop-color="#b829d6"/></linearGradient>' % uid,
         '<linearGradient id="%s_floor" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1b0636"/>'
         '<stop offset="1" stop-color="#04010b"/></linearGradient>' % uid,
         '<radialGradient id="%s_bloom"><stop offset="0" stop-color="#ff5fd2" stop-opacity="0.5"/>'
         '<stop offset="1" stop-color="#ff5fd2" stop-opacity="0"/></radialGradient></defs>' % uid,
         '<rect width="%d" height="%d" fill="url(#%s_sky)"/>' % (w, hz, uid)]
    seed = sum(map(ord, uid)) * 7919
    for i in range(70):   # stars
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        x, y = seed % w, (seed >> 11) % (hz - 60)
        o.append('<circle cx="%d" cy="%d" r="%.1f" fill="#fff" opacity="%.2f"/>' % (x, y, 0.6 + (seed >> 5) % 3 * 0.4,
                                                                                   0.3 + (seed >> 7) % 6 * 0.1))
    o.append('<ellipse cx="640" cy="%d" rx="420" ry="190" fill="url(#%s_bloom)"/>' % (hz, uid))
    r = 170
    o.append('<clipPath id="%s_cut">' % uid)
    y, gap = hz - r, 3.0
    while y < hz:   # the sun's slices widen toward the horizon
        band = 14 if y < hz - 110 else 9
        o.append('<rect x="0" y="%.1f" width="%d" height="%.1f"/>' % (y, w, band))
        y += band + gap
        gap += 0.9
    o.append('</clipPath><circle cx="640" cy="%d" r="%d" fill="url(#%s_sun)" clip-path="url(#%s_cut)"/>' % (hz, r, uid, uid))
    o.append('<rect y="%d" width="%d" height="%d" fill="url(#%s_floor)"/>' % (hz, w, h - hz, uid))
    for k in range(-22, 23):   # grid: rays from the vanishing point, rows closing in on the horizon
        o.append('<line x1="640" y1="%d" x2="%d" y2="%d" stroke="#%s" stroke-width="1.2" opacity="0.55"/>' % (hz, 640 + k * 90, h, colour))
    for j in range(1, 12):
        y = hz + (h - hz) * (j / 11) ** 2.2
        o.append('<line x1="0" y1="%.1f" x2="%d" y2="%.1f" stroke="#%s" stroke-width="1.2" opacity="%.2f"/>' % (y, w, y, colour, 0.25 + j * 0.04))
    o.append('<line x1="0" y1="%d" x2="%d" y2="%d" stroke="#%s" stroke-width="2" opacity="0.9"/>' % (hz, w, hz, colour))
    for y in range(0, h, 4):
        o.append('<rect y="%d" width="%d" height="1" fill="#000" opacity="0.18"/>' % (y, w))
    o.append('</svg>')
    return "\n".join(o)


def panel(uid, w, h, colour, title, field=None, header=48):
    """A slot's enclosure, w x h: smoked glass, an edge in the page colour, a header band with the title and the
    model field (x, y, w, h relative to the panel, drawn here because the popup line has field=none), four screws."""
    dark = mix(colour, "05020f", 0.82)
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>' % (w, h, w, h),
         '<linearGradient id="%s_hd" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#%s" stop-opacity="0.42"/>'
         '<stop offset="0.55" stop-color="#%s" stop-opacity="0.10"/><stop offset="1" stop-color="#%s" stop-opacity="0.02"/>'
         '</linearGradient>' % (uid, colour, colour, colour),
         '<linearGradient id="%s_bd" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0d0624" stop-opacity="1"/>'
         '<stop offset="1" stop-color="#07031a" stop-opacity="1"/></linearGradient>' % uid,
         '<radialGradient id="%s_sc" cx="0.35" cy="0.3"><stop offset="0" stop-color="#ffffff"/>'
         '<stop offset="1" stop-color="#6d6a80"/></radialGradient>' % uid,
         '<filter id="%s_gl" x="-10%%" y="-40%%" width="120%%" height="180%%"><feGaussianBlur stdDeviation="2.4" result="b"/>'
         '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>' % uid]
    o.append('<rect x="3" y="3" width="%d" height="%d" rx="14" fill="url(#%s_bd)"/>' % (w - 6, h - 6, uid))
    o.append('<path d="M3,%d V17 A14,14 0 0 1 17,3 H%d A14,14 0 0 1 %d,17 V%d Z" fill="url(#%s_hd)"/>' % (
        header, w - 17, w - 3, header, uid))
    o.append('<line x1="16" y1="%d" x2="%d" y2="%d" stroke="#%s" stroke-width="1.5" opacity="0.6"/>' % (header, w - 16, header, colour))
    o.append('<rect x="3" y="3" width="%d" height="%d" rx="14" fill="none" stroke="#%s" stroke-width="2.5" filter="url(#%s_gl)"/>' % (
        w - 6, h - 6, colour, uid))
    o.append('<rect x="3" y="20" width="5" height="%d" rx="2" fill="#%s"/>' % (h - 40, colour))
    o.append('<text x="24" y="%d" font-family="Titillium Web" font-weight="700" font-style="italic" font-size="%d" '
             'letter-spacing="3" fill="#%s" filter="url(#%s_gl)">%s</text>' % (header // 2 + 10, TITLE_PX, colour, uid, title))
    if field:
        fx, fy, fw, fh = field
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="#05020f" stroke="#%s" stroke-width="1.8"/>' % (
            fx, fy, fw, fh, colour))
        o.append('<rect x="%d" y="%d" width="%d" height="%d" rx="0" fill="#%s"/>' % (fx + fw - 38, fy + 6, 1.5, fh - 12, dark))
        cx, cy = fx + fw - 19, fy + fh / 2 + 3
        o.append('<path d="M%g,%g L%g,%g L%g,%g Z" fill="#%s"/>' % (cx - 8, cy - 6, cx + 8, cy - 6, cx, cy + 4, colour))
    for sx, sy in ((14, h - 14), (w - 14, h - 14), (w - 14, header + 12)):
        o.append('<circle cx="%d" cy="%d" r="5" fill="url(#%s_sc)"/><line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#3a3650" '
                 'stroke-width="1.4"/>' % (sx, sy, uid, sx - 3, sy - 3, sx + 3, sy + 3))
    o.append('</svg>')
    return "\n".join(o)


def field(uid, w, h, colour):
    """A slot's model field, w x h: the button that opens the model browser (its live text is the model's name)."""
    dark = mix(colour, "05020f", 0.82)
    cx, cy = w - 19, h / 2 + 3
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h),
        '<rect x="1" y="1" width="%d" height="%d" rx="8" fill="#05020f" stroke="#%s" stroke-width="1.8"/>' % (w - 2, h - 2, colour),
        '<rect x="%d" y="6" width="1.5" height="%d" fill="#%s"/>' % (w - 38, h - 12, dark),
        '<path d="M%g,%g L%g,%g L%g,%g Z" fill="#%s"/>' % (cx - 8, cy - 6, cx + 8, cy - 6, cx, cy + 4, colour),
        '</svg>'])


def browser_panel(uid, w, h, colour, head):
    """The model browser over a whole page, w x h: the page colour's edge, a header band for the title, the arrows
    and CLOSE."""
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>' % (w, h, w, h),
        '<linearGradient id="%s_hd" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#%s" stop-opacity="0.45"/>'
        '<stop offset="1" stop-color="#%s" stop-opacity="0.05"/></linearGradient>' % (uid, colour, colour),
        '<filter id="%s_gl" x="-5%%" y="-5%%" width="110%%" height="110%%"><feGaussianBlur stdDeviation="2.4" result="b"/>'
        '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>' % uid,
        '<rect x="3" y="3" width="%d" height="%d" rx="14" fill="#07031a"/>' % (w - 6, h - 6),
        '<path d="M3,%d V17 A14,14 0 0 1 17,3 H%d A14,14 0 0 1 %d,17 V%d Z" fill="url(#%s_hd)"/>' % (head, w - 17, w - 3, head, uid),
        '<line x1="16" y1="%d" x2="%d" y2="%d" stroke="#%s" stroke-width="1.5" opacity="0.6"/>' % (head, w - 16, head, colour),
        '<rect x="3" y="3" width="%d" height="%d" rx="14" fill="none" stroke="#%s" stroke-width="2.5" filter="url(#%s_gl)"/>' % (
            w - 6, h - 6, colour, uid),
        '</svg>'])


ARROW_W, ARROW_H = 72, 48


def arrow(uid, colour, left):
    """A page arrow for the model browser."""
    w, h = ARROW_W, ARROW_H
    tip, back = (24, 46) if left else (48, 26)
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h),
        '<rect x="1" y="1" width="%d" height="%d" rx="10" fill="#0b0522" stroke="#%s" stroke-width="2"/>' % (w - 2, h - 2, colour),
        '<path d="M%d,%d L%d,%d L%d,%d" fill="none" stroke="#%s" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>' % (
            back, 12, tip, 24, back, 36, colour),
        '</svg>'])


CLOSE_W, CLOSE_H = 96, 44


def close_button(uid, colour):
    """CLOSE for the model browser (a pick closes it too)."""
    w, h = CLOSE_W, CLOSE_H
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h),
        '<rect x="1" y="1" width="%d" height="%d" rx="10" fill="#0b0522" stroke="#%s" stroke-width="2"/>' % (w - 2, h - 2, colour),
        '<path d="M12,16 l12,12 M24,16 l-12,12" stroke="#%s" stroke-width="4" stroke-linecap="round"/>' % colour,
        '<text x="62" y="29" text-anchor="middle" font-family="Titillium Web" font-weight="700" font-size="18" '
        'letter-spacing="1" fill="#ffffff">CLOSE</text>',
        '</svg>'])


FS_W, FS_H = 76, 100


def footswitch(uid, colour, on):
    """A stomp switch with its LED: the LED lit in the page colour when the slot is on."""
    w, h = FS_W, FS_H
    led = colour if on else mix(colour, "05020f", 0.78)
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>' % (w, h, w, h),
         '<radialGradient id="%s_cap" cx="0.38" cy="0.32" r="0.75"><stop offset="0" stop-color="#ffffff"/>'
         '<stop offset="0.45" stop-color="#c9c6dc"/><stop offset="1" stop-color="#4b4763"/></radialGradient>' % uid,
         '<linearGradient id="%s_nut" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#e9e6f5"/>'
         '<stop offset="0.5" stop-color="#8c88a3"/><stop offset="1" stop-color="#3a3650"/></linearGradient>' % uid,
         '<radialGradient id="%s_led" cx="0.4" cy="0.35"><stop offset="0" stop-color="#ffffff" stop-opacity="%s"/>'
         '<stop offset="0.45" stop-color="#%s"/><stop offset="1" stop-color="#%s"/></radialGradient>' % (
             uid, "0.95" if on else "0.15", led, mix(led, "000000", 0.35)),
         '<radialGradient id="%s_halo"><stop offset="0" stop-color="#%s" stop-opacity="0.85"/>'
         '<stop offset="1" stop-color="#%s" stop-opacity="0"/></radialGradient></defs>' % (uid, colour, colour)]
    if on:
        o.append('<circle cx="%g" cy="15" r="15" fill="url(#%s_halo)"/>' % (w / 2, uid))
    o.append('<circle cx="%g" cy="15" r="8" fill="#0a0614"/><circle cx="%g" cy="15" r="6.5" fill="url(#%s_led)"/>' % (w / 2, w / 2, uid))
    cx, cy = w / 2, 64
    hexagon = " ".join("%.1f,%.1f" % (cx + 30 * math.cos(math.radians(30 + 60 * i)), cy + 30 * math.sin(math.radians(30 + 60 * i)))
                       for i in range(6))
    o.append('<ellipse cx="%g" cy="%g" rx="31" ry="9" fill="#000" opacity="0.55"/>' % (cx, cy + 27))
    o.append('<polygon points="%s" fill="url(#%s_nut)" stroke="#2a2640" stroke-width="1"/>' % (hexagon, uid))
    o.append('<circle cx="%g" cy="%g" r="21" fill="#1b1830"/>' % (cx, cy + (2 if on else 0)))
    o.append('<circle cx="%g" cy="%g" r="18" fill="url(#%s_cap)" stroke="#2a2640" stroke-width="1"/>' % (cx, cy + (2 if on else 0), uid))
    o.append('<ellipse cx="%g" cy="%g" rx="8" ry="4.5" fill="#fff" opacity="0.55"/>' % (cx - 6, cy - 8 + (2 if on else 0)))
    o.append('</svg>')
    return "\n".join(o)


def strip(uid, w, h, colour):
    """The bar over a paged section's frames, w x h: its page title, the page arrows and ADD."""
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>' % (w, h, w, h),
        '<linearGradient id="%s_bg" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#%s" stop-opacity="0.38"/>'
        '<stop offset="0.6" stop-color="#%s" stop-opacity="0.08"/><stop offset="1" stop-color="#%s" stop-opacity="0.22"/>'
        '</linearGradient></defs>' % (uid, colour, colour, colour),
        '<rect x="2" y="2" width="%d" height="%d" rx="12" fill="#07031a" fill-opacity="0.92"/>' % (w - 4, h - 4),
        '<rect x="2" y="2" width="%d" height="%d" rx="12" fill="url(#%s_bg)" stroke="#%s" stroke-width="2"/>' % (
            w - 4, h - 4, uid, colour),
        '</svg>'])


def empty_panel(uid, w, h, colour):
    """An empty frame of a paged section, w x h: a dashed outline (its ADD button sits in the middle)."""
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h),
        '<rect x="3" y="3" width="%d" height="%d" rx="14" fill="#07031a" fill-opacity="0.55" stroke="#%s" '
        'stroke-width="2" stroke-dasharray="14 10" opacity="0.7"/>' % (w - 6, h - 6, colour),
        '</svg>'])


ICON_W, ICON_H = 56, 38


def icon_button(uid, colour, kind):
    """A frame's small header button: "up" (earlier in the chain), "down" (later) or "remove"."""
    w, h = ICON_W, ICON_H
    cx, cy = w / 2, h / 2
    if kind == "remove":
        g = '<path d="M%g,%g l14,14 M%g,%g l-14,14" stroke="#ff6b8b" stroke-width="4" stroke-linecap="round"/>' % (
            cx - 7, cy - 7, cx + 7, cy - 7)
    else:
        d = -1 if kind == "up" else 1
        g = '<path d="M%g,%g L%g,%g L%g,%g" fill="none" stroke="#%s" stroke-width="4.5" stroke-linecap="round" ' \
            'stroke-linejoin="round"/>' % (cx - 10, cy - 5 * d, cx, cy + 5 * d, cx + 10, cy - 5 * d, colour)
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h),
        '<rect x="1" y="1" width="%d" height="%d" rx="9" fill="#0b0522" stroke="#%s" stroke-width="1.8"/>' % (w - 2, h - 2, colour),
        g, '</svg>'])


def add_button(uid, colour, w, h, text):
    """ADD for a paged section: a plus and the item's name."""
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h),
        '<rect x="1" y="1" width="%d" height="%d" rx="10" fill="#%s" fill-opacity="0.22" stroke="#%s" stroke-width="2"/>' % (
            w - 2, h - 2, colour, colour),
        '<path d="M%g,%g h16 M%g,%g v16" stroke="#%s" stroke-width="4" stroke-linecap="round"/>' % (
            22, h / 2, 30, h / 2 - 8, colour),
        '<text x="%g" y="%g" text-anchor="middle" font-family="Titillium Web" font-weight="700" font-size="20" '
        'letter-spacing="1.5" fill="#ffffff">%s</text>' % ((w + 40) / 2, h / 2 + 7, text),
        '</svg>'])
