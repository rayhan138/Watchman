#include "light_theme_reference.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace LightThemeReference;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Near(double actual, double expected, double tolerance, const char* message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void CheckColor(Color actual, Color expected) {
    Check(actual.red == expected.red && actual.green == expected.green
        && actual.blue == expected.blue && actual.alpha == expected.alpha,
        "Exact CSS source/card color or alpha mismatch");
}

void SourceValues() {
    const auto& palette = GetPalette();
    Check(&palette == &GetPalette(), "Palette must be cached");
    CheckColor(palette.background, {255, 255, 255, 1.0});
    CheckColor(palette.layers[0].source_color, {244, 114, 182, 0.5});
    CheckColor(palette.layers[1].source_color, {253, 224, 71, 0.4});
    CheckColor(palette.layers[2].source_color, {251, 146, 60, 0.4});
    CheckColor(palette.session_card.start, {255, 247, 237, 0.78});
    CheckColor(palette.session_card.end, {255, 255, 255, 0.90});
    CheckColor(palette.snapshot_card.start, {255, 247, 237, 0.82});
    CheckColor(palette.snapshot_card.end, {255, 255, 255, 0.92});
}

void Geometry() {
    const auto& layers = GetPalette().layers;
    const double radii[] = {150.0, 200.0, 125.0};
    for (std::size_t i = 0; i < layers.size(); ++i) {
        Near(layers[i].disk_radius, radii[i], 0.0, "Source disks must have fixed CSS radii");
        Near(layers[i].sigma, 120.0, 0.0, "CSS blur sigma must be 120");
        Near(layers[i].support_radius, radii[i] + 480.0, 0.0, "Support must be R + 4 sigma");
    }
    // Includes a zero and small canvas: percentage anchors can lie off-canvas.
    for (Point canvas : {Point{1000.0, 800.0}, Point{1920.0, 1080.0},
                         Point{100.0, 80.0}, Point{0.0, 0.0}}) {
        const double w = canvas.x, h = canvas.y;
        const Point expected[] = {
            {-0.20 * w + 150.0, -0.10 * h + 150.0},
            {w - (-0.20 * w) - 400.0 + 200.0,
             h - (-0.10 * h) - 400.0 + 200.0},
            {0.30 * w + 125.0, 0.40 * h + 125.0}
        };
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const auto center = layers[i].CenterForCanvas(w, h);
            Near(center.x, expected[i].x, 1e-10, "CSS horizontal anchor mismatch");
            Near(center.y, expected[i].y, 1e-10, "CSS vertical anchor mismatch");
        }
    }
}

// Independent polar-area midpoint integration, not the header's erf/Simpson
// integration. Used only by this console test to check off-center samples.
double PolarReference(const Layer& layer, double distance) {
    constexpr int radial_steps = 192;
    constexpr int angular_steps = 192;
    constexpr double pi = 3.14159265358979323846;
    const double dr = layer.disk_radius / radial_steps;
    const double da = 2.0 * pi / angular_steps;
    double sum = 0.0;
    for (int r = 0; r < radial_steps; ++r) {
        const double radius = (r + 0.5) * dr;
        for (int a = 0; a < angular_steps; ++a) {
            const double angle = (a + 0.5) * da;
            const double squared = distance * distance + radius * radius
                - 2.0 * distance * radius * std::cos(angle);
            sum += radius * std::exp(-squared / (2.0 * layer.sigma * layer.sigma));
        }
    }
    return layer.source_color.alpha * sum * dr * da / (2.0 * pi * layer.sigma * layer.sigma);
}

void BlurSamples() {
    for (const auto& layer : GetPalette().layers) {
        const double center = layer.source_color.alpha
            * (1.0 - std::exp(-layer.disk_radius * layer.disk_radius
                / (2.0 * layer.sigma * layer.sigma)));
        Near(layer.stops.front().position, 0.0, 0.0, "First stop must be at zero");
        Near(layer.stops.front().alpha, center, 1e-14, "Blur center formula mismatch");
        Check(center > 0.0 && center < layer.source_color.alpha,
            "Blurred center must be weaker than source alpha");
        Near(layer.stops.back().position, 1.0, 0.0, "Last stop must be at one");
        Near(layer.stops.back().alpha, 0.0, 0.0, "Support edge must be transparent");
        for (std::size_t i = 0; i < layer.stops.size(); ++i) {
            const auto& stop = layer.stops[i];
            Check(std::isfinite(stop.alpha) && stop.alpha >= 0.0 && stop.alpha <= center,
                "Invalid sampled alpha");
            Near(stop.position, static_cast<double>(i) / (stop_count - 1), 1e-15,
                "Stops must be normalized and evenly spaced");
            if (i > 0) {
                Check(stop.position > layer.stops[i - 1].position, "Stops must be ordered");
                Check(stop.alpha <= layer.stops[i - 1].alpha, "Blur must decay radially");
            }
        }
        for (std::size_t i : {std::size_t{8}, std::size_t{24}, std::size_t{48}}) {
            const auto& stop = layer.stops[i];
            Near(stop.alpha, PolarReference(layer, stop.position * layer.support_radius),
                1e-5, "Off-center alpha must approximate disk/Gaussian convolution");
            // Check the linear interpolation consumed by a radial gradient too.
            const auto& next = layer.stops[i + 1];
            Near((stop.alpha + next.alpha) / 2.0,
                PolarReference(layer, (stop.position + next.position) * 0.5 * layer.support_radius),
                3e-4, "Interpolated gradient must approximate the blur between samples");
        }
        Check(layer.stops[48].alpha < center * 0.05, "Far tail must be faint");
        Check(PolarReference(layer, layer.support_radius) < 1e-5,
            "Four-sigma truncation must discard only a tiny local alpha");
    }
}
} // namespace

int main() {
    try {
        SourceValues();
        Geometry();
        BlurSamples();
        std::cout << "Light theme reference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Light theme reference test failed: " << error.what() << '\n';
        return 1;
    }
}
