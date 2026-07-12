pragma Singleton

import QtQuick

QtObject {
    id: tokens

    // Global accessibility switch. Components also expose a local
    // motionEnabled property so either policy can disable animation.
    property bool reducedMotion: false

    // Active Material 3 colour scheme, bound from AppController.colorScheme:
    // 0 = Copper, 1 = Indigo (default), 2 = Spruce. Every colour function reads
    // this property, so switching it re-themes the whole application. QML
    // property capture tracks `scheme` through the function calls, so all
    // DesignTokens.<role>(dark) bindings re-evaluate when it changes.
    property int scheme: 1

    readonly property var schemeNames: [
        { en: "Copper", zh: "赤銅" },
        { en: "Indigo", zh: "靛藍" },
        { en: "Spruce", zh: "雲杉" }
    ]

    readonly property string fontDisplay: "Segoe UI Variable Display"
    readonly property string fontBody: "Segoe UI Variable Text"
    readonly property string fontMono: "Cascadia Mono"

    // Refined control metrics taken from the WimForge Material 3 design canvas.
    readonly property int radiusSmall: 10
    readonly property int radiusControl: 12
    readonly property int radiusCard: 18
    readonly property int radiusPill: 22
    readonly property int navWidth: 260
    readonly property int navCompactWidth: 76
    readonly property int topBarHeight: 64
    readonly property int controlHeight: 40
    readonly property int fieldHeight: 44
    readonly property int rowHeight: 44

    readonly property int spacing4: 4
    readonly property int spacing8: 8
    readonly property int spacing12: 12
    readonly property int spacing16: 16
    readonly property int spacing20: 20
    readonly property int spacing24: 24
    readonly property int spacing28: 28
    readonly property int spacing32: 32
    readonly property int spacing40: 40

    readonly property int motionShort: 150
    readonly property int motionMedium: 200

    // ─── Material 3 tonal palettes (Copper / Indigo / Spruce) ───
    // Keys mirror the design canvas: p/onp/pc/onpc = primary family,
    // sec* = secondary, ter* = tertiary, bg/sf0..sf4/card = surfaces,
    // on/onv = on-surface, ol/olv = outline.
    readonly property var _schemes: [
        {   // Copper
            dark:  { p:"#FFB59E", onp:"#571E0A", pc:"#753523", onpc:"#FFDBD0", sec:"#E7BDAF", onsec:"#442A20", secc:"#5D4035", onsecc:"#FFDBD0", ter:"#DDC48C", onter:"#3B2F05", terc:"#544619", onterc:"#FAE1A6", bg:"#17100C", sf0:"#140D0A", sf1:"#231A16", sf2:"#291F1A", sf3:"#342925", sf4:"#3F332E", on:"#F1DFD9", onv:"#D5C0B8", ol:"#A08D85", olv:"#4E413B", card:"#211814" },
            light: { p:"#9A4527", onp:"#FFFFFF", pc:"#FFDBD0", onpc:"#3B0900", sec:"#77574C", onsec:"#FFFFFF", secc:"#F4DDD3", onsecc:"#2C150C", ter:"#6C5D2F", onter:"#FFFFFF", terc:"#F6E1A6", onterc:"#221B00", bg:"#FFF8F6", sf0:"#FFF0EA", sf1:"#FAEEE8", sf2:"#F4E7E1", sf3:"#EFE1DA", sf4:"#E9DBD4", on:"#231A16", onv:"#53433E", ol:"#85736C", olv:"#D8C2BA", card:"#FFFFFF" }
        },
        {   // Indigo
            dark:  { p:"#B9C3FF", onp:"#1F2D61", pc:"#374479", onpc:"#DEE1FF", sec:"#C3C5DD", onsec:"#2C2F42", secc:"#434659", onsecc:"#DFE1F9", ter:"#E5BAD8", onter:"#44263E", terc:"#5D3C55", onterc:"#FFD7F1", bg:"#131318", sf0:"#0E0E13", sf1:"#1B1B21", sf2:"#1F1F25", sf3:"#2A2A31", sf4:"#35353C", on:"#E4E1E9", onv:"#C7C5D0", ol:"#918F9A", olv:"#46464F", card:"#191920" },
            light: { p:"#4A5C92", onp:"#FFFFFF", pc:"#DEE1FF", onpc:"#001947", sec:"#5B5D72", onsec:"#FFFFFF", secc:"#DFE1F9", onsecc:"#181A2C", ter:"#76546D", onter:"#FFFFFF", terc:"#FFD7F1", onterc:"#2C1228", bg:"#FAF8FF", sf0:"#F2EFFA", sf1:"#F2F0FA", sf2:"#ECEAF4", sf3:"#E6E4EF", sf4:"#E0DEE9", on:"#1B1B21", onv:"#46464F", ol:"#777680", olv:"#C8C5D0", card:"#FFFFFF" }
        },
        {   // Spruce
            dark:  { p:"#86D6A5", onp:"#00391F", pc:"#205237", onpc:"#A3F4C3", sec:"#B4CCBB", onsec:"#203528", secc:"#364B3C", onsecc:"#D0E8D6", ter:"#A4CDDE", onter:"#063542", terc:"#234C59", onterc:"#C0E8F8", bg:"#101512", sf0:"#0B100D", sf1:"#191E1A", sf2:"#1D231E", sf3:"#282E29", sf4:"#333934", on:"#E0E4DD", onv:"#C1C9C0", ol:"#8B938A", olv:"#414942", card:"#171C18" },
            light: { p:"#2A6A4B", onp:"#FFFFFF", pc:"#B1F1CB", onpc:"#00210F", sec:"#4F6354", onsec:"#FFFFFF", secc:"#D2E8D5", onsecc:"#0C1F12", ter:"#3B6470", onter:"#FFFFFF", terc:"#BFE9F8", onterc:"#001F27", bg:"#F6FBF4", sf0:"#EDF3EA", sf1:"#EFF4EC", sf2:"#E9EFE7", sf3:"#E3EAE1", sf4:"#DDE4DB", on:"#171D18", onv:"#414942", ol:"#717970", olv:"#C0C9BF", card:"#FFFFFF" }
        }
    ]

    // Semantic error / success stay constant across schemes (design canvas SEM).
    readonly property var _semantic: {
        "dark":  { err:"#FFB4AB", onerr:"#690005", errc:"#93000A", onerrc:"#FFDAD6", ok:"#97D5A0", onok:"#05391B", okc:"#1E5230", onokc:"#B4F1BF" },
        "light": { err:"#BA1A1A", onerr:"#FFFFFF", errc:"#FFDAD6", onerrc:"#410002", ok:"#2F6B43", onok:"#FFFFFF", okc:"#B4F1BF", onokc:"#00210D" }
    }

    // Reads `scheme` so every dependent binding re-evaluates on scheme change.
    function _p(dark) {
        var pack = _schemes[Math.max(0, Math.min(scheme, _schemes.length - 1))]
        return dark ? pack.dark : pack.light
    }
    function _s(dark) { return dark ? _semantic.dark : _semantic.light }

    function primary(dark) { return _p(dark).p }
    function onPrimary(dark) { return _p(dark).onp }
    function primaryContainer(dark) { return _p(dark).pc }
    function onPrimaryContainer(dark) { return _p(dark).onpc }

    function secondary(dark) { return _p(dark).sec }
    function onSecondary(dark) { return _p(dark).onsec }
    function secondaryContainer(dark) { return _p(dark).secc }
    function onSecondaryContainer(dark) { return _p(dark).onsecc }

    function tertiary(dark) { return _p(dark).ter }
    function onTertiary(dark) { return _p(dark).onter }
    function tertiaryContainer(dark) { return _p(dark).terc }
    function onTertiaryContainer(dark) { return _p(dark).onterc }

    function success(dark) { return _s(dark).ok }
    function onSuccess(dark) { return _s(dark).onok }
    function successContainer(dark) { return _s(dark).okc }
    function onSuccessContainer(dark) { return _s(dark).onokc }

    function error(dark) { return _s(dark).err }
    function onError(dark) { return _s(dark).onerr }
    function errorContainer(dark) { return _s(dark).errc }
    function onErrorContainer(dark) { return _s(dark).onerrc }

    function surface(dark) { return _p(dark).bg }
    function surfaceDim(dark) { return _p(dark).sf0 }
    function surfaceLowest(dark) { return _p(dark).card }
    function surfaceLow(dark) { return _p(dark).sf1 }
    function surfaceContainer(dark) { return _p(dark).sf2 }
    function surfaceHigh(dark) { return _p(dark).sf3 }
    function surfaceHighest(dark) { return _p(dark).sf4 }
    function onSurface(dark) { return _p(dark).on }
    function onSurfaceVariant(dark) { return _p(dark).onv }
    function outline(dark) { return _p(dark).ol }
    function outlineVariant(dark) { return _p(dark).olv }

    function navSurface(dark) { return _p(dark).sf1 }
    function navOn(dark) { return _p(dark).on }
    function navHover(dark) { return _p(dark).sf3 }

    function surfaceForLevel(level, dark) {
        if (level === "low") return surfaceLow(dark)
        if (level === "container") return surfaceContainer(dark)
        if (level === "high") return surfaceHigh(dark)
        if (level === "highest") return surfaceHighest(dark)
        return surfaceLowest(dark)
    }

    function toneContainer(tone, dark) {
        if (tone === "primary") return primaryContainer(dark)
        if (tone === "info") return secondaryContainer(dark)
        if (tone === "warning") return tertiaryContainer(dark)
        if (tone === "success") return successContainer(dark)
        if (tone === "error" || tone === "destructive") return errorContainer(dark)
        return surfaceHigh(dark)
    }

    function toneForeground(tone, dark) {
        if (tone === "primary") return onPrimaryContainer(dark)
        if (tone === "info") return onSecondaryContainer(dark)
        if (tone === "warning") return onTertiaryContainer(dark)
        if (tone === "success") return onSuccessContainer(dark)
        if (tone === "error" || tone === "destructive") return onErrorContainer(dark)
        return onSurfaceVariant(dark)
    }

    function toneStrong(tone, dark) {
        if (tone === "primary") return primary(dark)
        if (tone === "info") return secondary(dark)
        if (tone === "warning") return tertiary(dark)
        if (tone === "success") return success(dark)
        if (tone === "error" || tone === "destructive") return error(dark)
        return onSurfaceVariant(dark)
    }

    function motionDuration(nominal, enabled) {
        return reducedMotion || enabled === false ? 0 : nominal
    }
}
