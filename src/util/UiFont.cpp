#include "util/UiFont.h"

namespace polish {

namespace {

// One query, shared by both entry points below -- SystemParametersInfoForDpi
// (not plain SystemParametersInfo) is what makes this per-monitor
// correct: it returns the metrics Windows would use at `dpi`, rather
// than the ones for whatever DPI the process happens to have been
// started on.
NONCLIENTMETRICSW QueryMetrics(UINT dpi) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    return metrics;
}

}  // namespace

HFONT MakeUiFont(UINT dpi) {
    const NONCLIENTMETRICSW metrics = QueryMetrics(dpi);
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

HFONT MakeUiFontWithWeight(UINT dpi, LONG weight) {
    NONCLIENTMETRICSW metrics = QueryMetrics(dpi);
    metrics.lfMessageFont.lfWeight = weight;
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

}  // namespace polish
