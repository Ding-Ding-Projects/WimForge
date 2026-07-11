pragma Singleton

import QtQuick

QtObject {
    // Global accessibility switch. Components also expose a local
    // motionEnabled property so either policy can disable animation.
    property bool reducedMotion: false

    readonly property string fontDisplay: "Segoe UI Variable Display"
    readonly property string fontBody: "Segoe UI Variable Text"
    readonly property string fontMono: "Cascadia Mono"

    readonly property int radiusControl: 8
    readonly property int radiusCard: 12
    readonly property int radiusPill: 20
    readonly property int navWidth: 260
    readonly property int navCompactWidth: 76
    readonly property int topBarHeight: 56
    readonly property int controlHeight: 38
    readonly property int fieldHeight: 38
    readonly property int rowHeight: 40

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

    function primary(dark) { return dark ? "#AFC6FF" : "#2F6FED" }
    function onPrimary(dark) { return dark ? "#102554" : "#FFFFFF" }
    function primaryContainer(dark) { return dark ? "#17376F" : "#E5ECFF" }
    function onPrimaryContainer(dark) { return dark ? "#E4EBFF" : "#132B63" }

    function secondary(dark) { return dark ? "#79D6EC" : "#1A7184" }
    function onSecondary(dark) { return dark ? "#083640" : "#FFFFFF" }
    function secondaryContainer(dark) { return dark ? "#164E5B" : "#C3F0FA" }
    function onSecondaryContainer(dark) { return dark ? "#D5F7FF" : "#123F49" }

    function tertiary(dark) { return dark ? "#F5BD62" : "#8A5B00" }
    function onTertiary(dark) { return dark ? "#432C00" : "#FFFFFF" }
    function tertiaryContainer(dark) { return dark ? "#5D4000" : "#FFE3AE" }
    function onTertiaryContainer(dark) { return dark ? "#FFEBC9" : "#4B3200" }

    function success(dark) { return dark ? "#8BD7A6" : "#27834A" }
    function onSuccess(dark) { return dark ? "#0B3A20" : "#FFFFFF" }
    function successContainer(dark) { return dark ? "#174F2B" : "#D6F3DE" }
    function onSuccessContainer(dark) { return dark ? "#C5F3D2" : "#123D23" }

    function error(dark) { return dark ? "#FFB4AB" : "#B3261E" }
    function onError(dark) { return dark ? "#690005" : "#FFFFFF" }
    function errorContainer(dark) { return dark ? "#8C1D18" : "#FFDAD6" }
    function onErrorContainer(dark) { return dark ? "#FFDAD6" : "#410002" }

    function surface(dark) { return dark ? "#151519" : "#FCFCFF" }
    function surfaceDim(dark) { return dark ? "#101014" : "#F1F2F7" }
    function surfaceLowest(dark) { return dark ? "#0D0D11" : "#FFFFFF" }
    function surfaceLow(dark) { return dark ? "#1B1B20" : "#F7F7FC" }
    function surfaceContainer(dark) { return dark ? "#202127" : "#F1F2F7" }
    function surfaceHigh(dark) { return dark ? "#292A31" : "#E9EAF1" }
    function surfaceHighest(dark) { return dark ? "#31323B" : "#E2E3EA" }
    function onSurface(dark) { return dark ? "#ECECF2" : "#27272B" }
    function onSurfaceVariant(dark) { return dark ? "#C0C1CA" : "#61636B" }
    function outline(dark) { return dark ? "#71737D" : "#A8AAB3" }
    function outlineVariant(dark) { return dark ? "#444650" : "#D8DAE2" }

    function navSurface(dark) { return dark ? "#0F1118" : "#171C2B" }
    function navOn(dark) { return dark ? "#E5E7F0" : "#F3F5FF" }
    function navHover(dark) { return dark ? "#20232E" : "#262C3E" }

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
