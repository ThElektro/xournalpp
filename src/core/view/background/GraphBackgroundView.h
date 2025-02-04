/*
 * Xournal++
 *
 * Class for graph backgrounds
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cairo.h>  // for cairo_t

#include "util/Color.h"  // for Color

#include "OneColorBackgroundView.h"  // for OneColorBackgroundView

class BackgroundConfig;

namespace xoj::view {
class GraphBackgroundView: public OneColorBackgroundView {
public:
    GraphBackgroundView(double pageWidth, double pageHeight, Color backgroundColor, const BackgroundConfig& config);
    virtual ~GraphBackgroundView() = default;

    virtual void draw(cairo_t* cr) const override;

protected:
    bool roundUpMargin = false;
    double margin = 0.0;
    double squareSize = 14.17;  // 5mm

    constexpr static Color DEFAULT_LINE_COLOR = Color(0xBDBDBDU);
    constexpr static Color ALT_DEFAULT_LINE_COLOR = Color(0x434343U);
    constexpr static double DEFAULT_LINE_WIDTH = 0.5;
};
};  // namespace xoj::view
