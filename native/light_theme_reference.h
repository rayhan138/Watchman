#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Pure C++17 reference for the supplied CSS, in CSS pixels (use DIPs at 100% zoom).
// This is a sampled, finite-support approximation, not a pixel-perfect CSS blur.
namespace LightThemeReference {
inline constexpr double blur_sigma = 120.0;
inline constexpr double support_sigmas = 4.0;
inline constexpr std::size_t stop_count = 65;

// Straight/unpremultiplied sRGB bytes; alpha is in [0, 1].
struct Color {
    std::uint8_t red, green, blue;
    double alpha;
};

struct Point {
    double x, y;
};

struct AlphaStop {
    double position; // Normalized distance: distance / support_radius.
    double alpha;    // Already includes source alpha; do not multiply it again.
};

struct Layer {
    Color source_color;
    double disk_radius;
    double sigma;
    double support_radius;
    Point center_fraction;
    Point center_offset;
    std::array<AlphaStop, stop_count> stops;

    // Only the center depends on canvas size. Do not scale the disk or blur.
    Point CenterForCanvas(double width, double height) const {
        return {center_fraction.x * width + center_offset.x,
                center_fraction.y * height + center_offset.y};
    }
};

struct CardGradient {
    Color start;
    Color end;
};

struct Palette {
    Color background;
    std::array<Layer, 3> layers; // CSS paint order: pink, yellow, peach.
    CardGradient session_card;
    CardGradient snapshot_card;
};

namespace detail {
// Integrate the normalized 2-D Gaussian over the source disk. Slice at x,
// integrate y analytically with erf, then x = R*sin(t) removes the disk-edge
// square-root singularity. Composite Simpson quadrature runs only at creation.
inline double DiskAlpha(double distance, double radius, double source_alpha) {
    constexpr int intervals = 128; // Even for Simpson's rule.
    constexpr double pi = 3.14159265358979323846;
    constexpr double sqrt_two = 1.41421356237309504880;
    const double step = pi / intervals;
    double sum = 0.0;
    for (int i = 1; i < intervals; ++i) {
        const double t = -pi / 2.0 + i * step;
        const double half_height = radius * std::cos(t);
        const double dx = radius * std::sin(t) - distance;
        const double value = std::exp(-dx * dx / (2.0 * blur_sigma * blur_sigma))
            * std::erf(half_height / (sqrt_two * blur_sigma)) * half_height;
        sum += (i % 2 == 0 ? 2.0 : 4.0) * value;
    }
    // Endpoint integrands vanish because the disk has zero height there.
    return source_alpha * sum * step / (3.0 * std::sqrt(2.0 * pi) * blur_sigma);
}

inline Layer MakeLayer(Color color, double radius, Point fraction, Point offset) {
    Layer layer{color, radius, blur_sigma, radius + support_sigmas * blur_sigma,
                fraction, offset, {}};
    for (std::size_t i = 0; i < stop_count; ++i) {
        const double position = static_cast<double>(i) / (stop_count - 1);
        const double distance = position * layer.support_radius;
        double alpha = DiskAlpha(distance, radius, color.alpha);
        if (i == 0) {
            // Exact center of a Gaussian-convolved disk, not source peak alpha.
            alpha = color.alpha * -std::expm1(-radius * radius
                / (2.0 * blur_sigma * blur_sigma));
        } else if (i == stop_count - 1) {
            // True Gaussian tails are infinite. Zero avoids a clamped outer tint;
            // this deliberately truncates a tiny tail at R + 4 sigma.
            alpha = 0.0;
        }
        layer.stops[i] = {position, alpha};
    }
    return layer;
}
} // namespace detail

// Thread-safe, cached once per program; no quadrature/allocation on frame/resize.
// Cache native gradient stops/brushes separately per D2D device resource lifetime.
inline const Palette& GetPalette() {
    static const Palette palette{
        {255, 255, 255, 1.0},
        {{
            // left:-20%, top:-10%, width/height:300px.
            detail::MakeLayer({244, 114, 182, 0.5}, 150.0, {-0.2, -0.1}, {150.0, 150.0}),
            // right:-20%, bottom:-10%, width/height:400px.
            detail::MakeLayer({253, 224, 71, 0.4}, 200.0, {1.2, 1.1}, {-200.0, -200.0}),
            // left:30%, top:40%, width/height:250px.
            detail::MakeLayer({251, 146, 60, 0.4}, 125.0, {0.3, 0.4}, {125.0, 125.0})
        }},
        {{255, 247, 237, 0.78}, {255, 255, 255, 0.90}},
        {{255, 247, 237, 0.82}, {255, 255, 255, 0.92}}
    };
    return palette;
}
} // namespace LightThemeReference
