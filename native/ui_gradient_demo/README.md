# Watchman Dashboard UI Gradient Demo

This directory contains standalone implementations of the **Watchman Dashboard UI** translating your TSX code, with a specific focus on **linear/radial gradient backgrounds** and **blurred background blobs** in both WinUI 3 (XAML) and Direct2D (C++).

## Files Included

1. **[WinUI3_Gradient_Demo.xaml](file:///d:/watchman%20c++%20windows/ui_gradient_demo/WinUI3_Gradient_Demo.xaml)**:
   - Contains a complete WinUI 3 / UWP XAML representation of your dashboard layout.
   - Shows how to write `<LinearGradientBrush>` for the Current Session and Usage Snapshot cards.
   - Demonstrates the use of overlapping `<Ellipse>` controls with `<RadialGradientBrush>` to simulate the ambient background blur blobs (pink, yellow, and orange).
   - Maps the layout controls (`Border`, `Grid`, `StackPanel`, `ProgressRing`) directly to your React components.

2. **[Direct2D_Gradient_Demo.cpp](file:///d:/watchman%20c++%20windows/ui_gradient_demo/Direct2D_Gradient_Demo.cpp)**:
   - Shows how to achieve the exact same rendering dynamically in standard C++ using Direct2D.
   - Demonstrates creating and configuring `ID2D1LinearGradientBrush` and `ID2D1RadialGradientBrush`.
   - Explains how to update the brush coordinates dynamically when resizing or rendering the layout.

## Core Concepts (Gradients & Blobs)

### WinUI 3 (XAML)
In WinUI 3, gradients are fully integrated into standard controls via their `Background` or `BorderBrush` properties. For example, the orange-to-white card is styled like this:
```xml
<Border.Background>
    <LinearGradientBrush StartPoint="0,0" EndPoint="1,1">
        <GradientStop Color="#C7FFF7ED" Offset="0.0"/> <!-- rgba(255, 247, 237, 0.78) -->
        <GradientStop Color="#E5FFFFFF" Offset="1.0"/> <!-- rgba(255, 255, 255, 0.9) -->
    </LinearGradientBrush>
</Border.Background>
```

### Direct2D (C++)
In Direct2D, you must create a `GradientStopCollection` containing your colors and offsets, and then use it to instantiate an `ID2D1LinearGradientBrush`. The start and end points are specified in local coordinates:
```cpp
D2D1_GRADIENT_STOP stops[2] = {
    { 0.0f, D2D1::ColorF(1.0f, 0.968f, 0.929f, 0.78f) },
    { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f) }
};
pRenderTarget->CreateGradientStopCollection(stops, 2, &pStops);
pRenderTarget->CreateLinearGradientBrush(
    D2D1::LinearGradientBrushProperties(startPt, endPt),
    pStops,
    &pLinearBrush
);
```
