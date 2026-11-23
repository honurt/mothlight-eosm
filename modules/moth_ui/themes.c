#include "themes.h"
#include "ui_gfx.h"

/* Colours are Magic Lantern palette indices: the same 8-bit overlay every ML build uses,
 * so themes cost nothing at run time and never touch Canon's palette. */

#define DARK_TEXT  .text = UC_WHITE, .hl_text = UC_WHITE, .title = UC_WHITE
#define VERDICTS   .ok = UC_GREEN, .edge = UC_YELLOW, .burst = UC_RED

const struct ui_theme moth_themes[THEME_COUNT] =
{
    [THEME_MOTHLIGHT] = {
        .name = "Mothlight", .blurb = "Graphite and lantern orange. The default.",
        .bg = UC_GRAY(8), .bar = UC_GRAY(5), .pane = UC_GRAY(11), .line = UC_GRAY(18), .hl = UC_GRAY(22),
        .track = UC_GRAY(17), .shadow = 0, DARK_TEXT, .dim = UC_GRAY(62), .faint = UC_GRAY(40),
        .accent = UC_ORANGE, VERDICTS, .active = UC_GREEN,
        .radius = 0, .hl_radius = 10, .hl_style = HL_FILL_PILL, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_ORANGE,
    },
    [THEME_STOCK] = {
        .name = "Stock", .blurb = "Like the EOS M's own menus: black, square, tabbed, orange frame.",
        .bg = UC_BLACK, .bar = UC_BLACK, .pane = UC_GRAY(6), .line = UC_GRAY(22), .hl = UC_GRAY(14),
        .track = UC_GRAY(20), .shadow = 0, DARK_TEXT, .dim = UC_GRAY(68), .faint = UC_GRAY(45),
        .accent = UC_ORANGE, VERDICTS, .active = UC_GREEN,
        .radius = 0, .hl_radius = 0, .hl_style = HL_FRAME, .head_style = HEAD_TABS,
        .frame = 0, .brand = UC_GRAY(68),
    },
    [THEME_STUDIO] = {
        .name = "Studio", .blurb = "Cinema-camera grey with cool blue highlights.",
        .bg = UC_GRAY(16), .bar = UC_GRAY(10), .pane = UC_GRAY(20), .line = UC_GRAY(28), .hl = UC_GRAY(30),
        .track = UC_GRAY(26), .shadow = 0, DARK_TEXT, .dim = UC_GRAY(72), .faint = UC_GRAY(50),
        .accent = UC_LIGHT_BLUE, VERDICTS, .active = UC_LIGHT_BLUE,
        .radius = 0, .hl_radius = 4, .hl_style = HL_FILL_PILL, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_LIGHT_BLUE,
    },
    [THEME_AMBER] = {
        .name = "Amber", .blurb = "Black with amber frames, like a film-set monitor.",
        .bg = UC_BLACK, .bar = UC_GRAY(4), .pane = UC_GRAY(4), .line = UC_GRAY(20), .hl = UC_BLACK,
        .track = UC_GRAY(18), .shadow = 0, DARK_TEXT, .dim = UC_GRAY(60), .faint = UC_GRAY(38),
        .accent = UC_YELLOW, VERDICTS, .active = UC_YELLOW,
        .radius = 0, .hl_radius = 0, .hl_style = HL_FRAME, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_YELLOW,
    },
    [THEME_SIGNAL] = {
        .name = "Signal", .blurb = "A solid orange selection bar you can read at a glance.",
        .bg = UC_BLACK, .bar = UC_GRAY(8), .pane = UC_GRAY(6), .line = UC_GRAY(18), .hl = UC_ORANGE,
        .track = UC_GRAY(20), .shadow = 0, .text = UC_WHITE, .hl_text = UC_BLACK, .title = UC_WHITE,
        .dim = UC_GRAY(62), .faint = UC_GRAY(40),
        .accent = UC_ORANGE, VERDICTS, .active = UC_ORANGE,
        .radius = 0, .hl_radius = 0, .hl_style = HL_FILL, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_ORANGE,
    },
    [THEME_MATERIAL] = {
        .name = "Material", .blurb = "Soft dark surfaces, rounded cards, cyan accent.",
        .bg = UC_GRAY(10), .bar = UC_GRAY(10), .pane = UC_GRAY(14), .line = UC_GRAY(18), .hl = UC_GRAY(24),
        .track = UC_GRAY(22), .shadow = 0, DARK_TEXT, .dim = UC_GRAY(66), .faint = UC_GRAY(44),
        .accent = UC_CYAN, VERDICTS, .active = UC_CYAN,
        .radius = 0, .hl_radius = 24, .hl_style = HL_FILL_PILL, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_CYAN,
    },
    [THEME_SOFT] = {
        .name = "Soft", .blurb = "Phone-like: near black, grouped rows, blue tint.",
        .bg = UC_BLACK, .bar = UC_BLACK, .pane = UC_GRAY(9), .line = UC_GRAY(15), .hl = UC_GRAY(17),
        .track = UC_GRAY(20), .shadow = 0, DARK_TEXT, .dim = UC_GRAY(60), .faint = UC_GRAY(40),
        .accent = UC_BLUE, VERDICTS, .active = UC_LIGHT_BLUE,
        .radius = 0, .hl_radius = 12, .hl_style = HL_FILL, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_LIGHT_BLUE,
    },
    [THEME_GLASS] = {
        .name = "Glass", .blurb = "See-through: keep watching the frame while you choose.",
        .bg = UC_SHADE, .bar = UC_SHADE, .pane = UC_SHADE, .line = UC_GRAY(40), .hl = UC_GRAY(28),
        .track = UC_GRAY(30), .shadow = 0, DARK_TEXT, .dim = UC_GRAY(82), .faint = UC_GRAY(62),
        .accent = UC_CYAN, VERDICTS, .active = UC_CYAN,
        .radius = 0, .hl_radius = 14, .hl_style = HL_FILL_PILL, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_CYAN,
    },
    [THEME_DAYLIGHT] = {
        .name = "Daylight", .blurb = "Light and high-contrast, for the screen in direct sun.",
        .bg = UC_GRAY(92), .bar = UC_BLUE, .pane = UC_WHITE, .line = UC_GRAY(75), .hl = UC_WHITE,
        .track = UC_GRAY(80), .shadow = 0, .text = UC_BLACK, .hl_text = UC_BLACK, .title = UC_WHITE,
        .dim = UC_GRAY(30), .faint = UC_GRAY(50),
        .accent = UC_BLUE, .ok = UC_GREEN, .edge = UC_ORANGE, .burst = UC_RED, .active = UC_BLUE,
        .radius = 0, .hl_radius = 6, .hl_style = HL_FILL_PILL, .head_style = HEAD_BAR,
        .frame = 0, .brand = UC_WHITE,
    },
    [THEME_NIGHT] = {
        .name = "Night", .blurb = "Red on black keeps your night vision on dark sets.",
        .bg = UC_BLACK, .bar = UC_BLACK, .pane = UC_BLACK, .line = UC_DARK_RED, .hl = UC_BLACK,
        .track = UC_DARK_RED, .shadow = 0, .text = UC_RED, .hl_text = UC_RED, .title = UC_RED,
        .dim = UC_DARK_RED, .faint = UC_DARK_RED,
        .accent = UC_RED, .ok = UC_RED, .edge = UC_RED, .burst = UC_RED, .active = UC_RED,
        .radius = 0, .hl_radius = 4, .hl_style = HL_FRAME, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_DARK_RED,
    },
    [THEME_MONO] = {
        .name = "Mono", .blurb = "Minimal: inverted selection, colour only where it means something.",
        .bg = UC_BLACK, .bar = UC_BLACK, .pane = UC_BLACK, .line = UC_GRAY(18), .hl = UC_WHITE,
        .track = UC_GRAY(22), .shadow = 0, .text = UC_WHITE, .hl_text = UC_BLACK, .title = UC_WHITE,
        .dim = UC_GRAY(58), .faint = UC_GRAY(38),
        .accent = UC_WHITE, VERDICTS, .active = UC_WHITE,
        .radius = 0, .hl_radius = 0, .hl_style = HL_FILL, .head_style = HEAD_PLAIN,
        .frame = 0, .brand = UC_GRAY(58),
    },
};
