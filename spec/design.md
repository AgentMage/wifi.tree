# Wifi Tree — Design Specification

## Visual Identity

| Element | Value |
|---|---|
| Logo | 🌳 (U+1F333 DECIDUOUS TREE) |
| Leaf symbol | 🌿 (U+1F33F HERB) |
| Background | `#0b1a0f` (deep forest dark) |
| Text | `#eaffea` (pale green-white) |
| Heading | `#9fe89f` (soft green) |
| Accent (default) | `#2e7d32` (forest green) — operator-configurable |
| Border (card) | `#1f3d29` |
| Border (active) | accent color |
| Border (warn) | `#b8860b` |
| Border (bad) | `#aa3333` |

Canonical stylesheet: [`design.css`](design.css)

## Copy Strings (defaults)

```
title:           wifi.tree
emoji:           🌳
tagline:         community wifi · please be mindful, it's shared
welcome_heading: Welcome to the gathering 🌿
welcome_text:    This is shared, free, community wifi. Enter a name and
                 grow a leaf to get online for 3 hours.
footer:          Shared, fair, bandwidth-limited.
                 Be kind, keep it light.
button_label:    Grow a Leaf 🌿
success_sub:     a fresh leaf, just for you
success_body:    You grew a leaf, {name}!
success_note:    You're online — close this page and start browsing.
                 Your leaf stays fresh for {ttl}.
success_link:    Go to wifi.tree →
```

All strings are operator-configurable except `button_label`, `success_sub`,
`success_body`, `success_note`, and `success_link`.

## Page Structure

### Welcome page (new visitor)

```
🌳
wifi.tree
community wifi · please be mindful, it's shared

┌─ card ──────────────────────────────────┐
│ Welcome to the gathering 🌿              │
│ This is shared, free, community wifi... │
└─────────────────────────────────────────┘

┌─ card ──────────────────────────────────┐
│ [enter your name              ]         │
│ [    Grow a Leaf 🌿           ]         │
└─────────────────────────────────────────┘

Shared, fair, bandwidth-limited.
Be kind, keep it light.
```

### Success screen (after form submit)

```
🌳
wifi.tree
community wifi · please be mindful, it's shared

┌─ card ok leafy ─────────────────────────┐
│         🌿  (animated grow+sway)        │
│    A FRESH LEAF, JUST FOR YOU           │
│    You grew a leaf, {name}!             │
│    You're online — close this page...   │
│                                         │
│ [    Go to wifi.tree →        ]         │
└─────────────────────────────────────────┘

Shared, fair, bandwidth-limited.
Be kind, keep it light.
```

### Status card (Pi full implementation — returning visitor)

Active leaf → green `.card.ok` with time + data meter bars + early-extend form  
Expired leaf → amber `.card.warn` + extend form  
Over quota → red `.card.bad`, no action button

## Leaf Animation

On success, the 🌿 emoji plays two layered animations:

- **leafgrow** (0.75s, plays once): scale from 0 + rotate from -45°, overshoot
  to 1.18× at 60%, settle to 1× — `cubic-bezier(0.2, 0.8, 0.3, 1.3)`
- **leafsway** (3.2s, infinite, ease-in-out): rotates ±7° around the bottom
  centre (`transform-origin: 50% 90%`)
