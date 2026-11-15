// **GENERATED FILE - do not edit by hand.**
//
// Source of truth: plugin/assets/icon/*.svg
// Regenerate with: python plugin/tools/svg_to_channel_glyphs.py
//
// The script flattens the dense polylines the SVGs use to spell out curves, keeps the stroke
// widths relative to the 64-unit viewBox with a 1 px floor, and drops `currentColor` in favour of
// the caller's colour so the glyphs follow the theme.

#include "ChannelGlyphs.h"

namespace toybox::ui
{

void draw_pulseGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
	const float s = area.getWidth () / 64.0f;	// the SVG's viewBox is 64 wide
	juce::Path p;

	g.setColour (colour);
	p.startNewSubPath (6 * s, 43 * s);
	p.lineTo (14 * s, 43 * s);
	p.lineTo (14 * s, 19 * s);
	p.lineTo (29 * s, 19 * s);
	p.lineTo (29 * s, 43 * s);
	p.lineTo (43 * s, 43 * s);
	p.lineTo (43 * s, 19 * s);
	p.lineTo (58 * s, 19 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();
}



void draw_triangleGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
	const float s = area.getWidth () / 64.0f;	// the SVG's viewBox is 64 wide
	juce::Path p;

	g.setColour (colour);
	p.startNewSubPath (6 * s, 43 * s);
	p.lineTo (19 * s, 19 * s);
	p.lineTo (32 * s, 43 * s);
	p.lineTo (45 * s, 19 * s);
	p.lineTo (58 * s, 43 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();
}



void draw_sawGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
	const float s = area.getWidth () / 64.0f;	// the SVG's viewBox is 64 wide
	juce::Path p;

	g.setColour (colour);
	p.startNewSubPath (6 * s, 43 * s);
	p.lineTo (6 * s, 19 * s);
	p.lineTo (23 * s, 43 * s);
	p.lineTo (23 * s, 19 * s);
	p.lineTo (40 * s, 43 * s);
	p.lineTo (40 * s, 19 * s);
	p.lineTo (57 * s, 43 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();
}



void draw_noiseGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
	const float s = area.getWidth () / 64.0f;	// the SVG's viewBox is 64 wide
	juce::Path p;

	g.setColour (colour);
	p.startNewSubPath (6 * s, 34 * s);
	p.lineTo (10 * s, 25 * s);
	p.lineTo (14 * s, 39 * s);
	p.lineTo (18 * s, 18 * s);
	p.lineTo (22 * s, 30 * s);
	p.lineTo (26 * s, 22 * s);
	p.lineTo (30 * s, 45 * s);
	p.lineTo (34 * s, 27 * s);
	p.lineTo (38 * s, 36 * s);
	p.lineTo (42 * s, 16 * s);
	p.lineTo (46 * s, 31 * s);
	p.lineTo (50 * s, 23 * s);
	p.lineTo (54 * s, 40 * s);
	p.lineTo (58 * s, 28 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();
}



void draw_fmGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
	const float s = area.getWidth () / 64.0f;	// the SVG's viewBox is 64 wide
	juce::Path p;

	g.setColour (colour);
	// 241 sampled points, 62 kept (see TOLERANCE)
	p.startNewSubPath (6 * s, 21 * s);
	p.lineTo (6.65 * s, 15.95 * s);
	p.lineTo (7.08 * s, 14.07 * s);
	p.lineTo (7.30 * s, 13.80 * s);
	p.lineTo (7.52 * s, 14 * s);
	p.lineTo (7.95 * s, 15.72 * s);
	p.lineTo (9.47 * s, 26.74 * s);
	p.lineTo (9.68 * s, 27.62 * s);
	p.lineTo (9.90 * s, 28.10 * s);
	p.lineTo (10.12 * s, 28.18 * s);
	p.lineTo (10.77 * s, 26.14 * s);
	p.lineTo (12.50 * s, 15.10 * s);
	p.lineTo (12.93 * s, 13.95 * s);
	p.lineTo (13.15 * s, 13.80 * s);
	p.lineTo (13.37 * s, 13.93 * s);
	p.lineTo (14.02 * s, 15.77 * s);
	p.lineTo (16.18 * s, 26.30 * s);
	p.lineTo (16.83 * s, 27.81 * s);
	p.lineTo (17.48 * s, 28.19 * s);
	p.lineTo (17.92 * s, 27.92 * s);
	p.lineTo (18.57 * s, 26.96 * s);
	p.lineTo (21.38 * s, 21.10 * s);
	p.lineTo (22.25 * s, 19.86 * s);
	p.lineTo (23.33 * s, 18.84 * s);
	p.lineTo (24.42 * s, 18.32 * s);
	p.lineTo (25.72 * s, 18.19 * s);
	p.lineTo (27.02 * s, 18.45 * s);
	p.lineTo (30.92 * s, 19.87 * s);
	p.lineTo (32 * s, 19.96 * s);
	p.lineTo (32.87 * s, 19.80 * s);
	p.lineTo (34.60 * s, 18.67 * s);
	p.lineTo (37.63 * s, 14.58 * s);
	p.lineTo (38.28 * s, 13.96 * s);
	p.lineTo (38.93 * s, 13.83 * s);
	p.lineTo (39.58 * s, 14.41 * s);
	p.lineTo (40.23 * s, 15.90 * s);
	p.lineTo (42.62 * s, 26.54 * s);
	p.lineTo (43.05 * s, 27.79 * s);
	p.lineTo (43.48 * s, 28.20 * s);
	p.lineTo (43.92 * s, 27.60 * s);
	p.lineTo (44.35 * s, 25.97 * s);
	p.lineTo (46.08 * s, 15.13 * s);
	p.lineTo (46.52 * s, 13.89 * s);
	p.lineTo (46.73 * s, 13.82 * s);
	p.lineTo (47.38 * s, 15.90 * s);
	p.lineTo (48.90 * s, 26.82 * s);
	p.lineTo (49.33 * s, 28.15 * s);
	p.lineTo (49.55 * s, 28.14 * s);
	p.lineTo (50.20 * s, 25.46 * s);
	p.lineTo (51.50 * s, 15.43 * s);
	p.lineTo (51.93 * s, 13.90 * s);
	p.lineTo (52.15 * s, 13.83 * s);
	p.lineTo (52.80 * s, 16.31 * s);
	p.lineTo (54.10 * s, 26.19 * s);
	p.lineTo (54.53 * s, 27.94 * s);
	p.lineTo (54.75 * s, 28.20 * s);
	p.lineTo (54.97 * s, 28.03 * s);
	p.lineTo (55.40 * s, 26.54 * s);
	p.lineTo (57.13 * s, 15.20 * s);
	p.lineTo (57.57 * s, 13.96 * s);
	p.lineTo (57.78 * s, 13.80 * s);
	p.lineTo (58 * s, 13.95 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();

	g.setColour (colour.withAlpha (0.72f));
	// 241 sampled points, 16 kept (see TOLERANCE)
	p.startNewSubPath (6 * s, 46 * s);
	p.lineTo (10.33 * s, 43.05 * s);
	p.lineTo (13.15 * s, 41.64 * s);
	p.lineTo (16.18 * s, 40.86 * s);
	p.lineTo (19 * s, 40.94 * s);
	p.lineTo (21.38 * s, 41.61 * s);
	p.lineTo (23.77 * s, 42.76 * s);
	p.lineTo (31.78 * s, 48.22 * s);
	p.lineTo (34.60 * s, 49.85 * s);
	p.lineTo (37.63 * s, 50.94 * s);
	p.lineTo (40.45 * s, 51.19 * s);
	p.lineTo (43.27 * s, 50.65 * s);
	p.lineTo (45.87 * s, 49.52 * s);
	p.lineTo (48.47 * s, 47.94 * s);
	p.lineTo (54.53 * s, 43.69 * s);
	p.lineTo (58 * s, 41.79 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 2.50f * s)));
	p.clear ();

	g.setColour (colour.withAlpha (0.72f));
	p.startNewSubPath (32 * s, 31 * s);
	p.lineTo (32 * s, 36 * s);
	p.startNewSubPath (29 * s, 33.50 * s);
	p.lineTo (32 * s, 36 * s);
	p.lineTo (35 * s, 33.50 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 2.00f * s)));
	p.clear ();
}



void draw_wavetableGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
	const float s = area.getWidth () / 64.0f;	// the SVG's viewBox is 64 wide
	juce::Path p;

	g.setColour (colour.withAlpha (0.28f));
	p.startNewSubPath (7 * s, 15 * s);
	p.lineTo (49 * s, 15 * s);
	p.startNewSubPath (7 * s, 31 * s);
	p.lineTo (49 * s, 31 * s);
	p.startNewSubPath (7 * s, 47 * s);
	p.lineTo (49 * s, 47 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 1.70f * s)));
	p.clear ();

	g.setColour (colour);
	p.startNewSubPath (7 * s, 15 * s);
	p.cubicTo (13 * s, 15 * s, 13 * s, 7 * s, 20 * s, 7 * s);
	p.cubicTo (27 * s, 7 * s, 27 * s, 23 * s, 34 * s, 15 * s);
	p.cubicTo (41 * s, 7 * s, 42 * s, 8 * s, 49 * s, 15 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();

	g.setColour (colour);
	p.startNewSubPath (7 * s, 31 * s);
	p.lineTo (15 * s, 24 * s);
	p.lineTo (23 * s, 38 * s);
	p.lineTo (31 * s, 27 * s);
	p.lineTo (39 * s, 35 * s);
	p.lineTo (49 * s, 31 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();

	g.setColour (colour);
	p.startNewSubPath (7 * s, 47 * s);
	p.cubicTo (13 * s, 38 * s, 19 * s, 56 * s, 25 * s, 47 * s);
	p.cubicTo (31 * s, 38 * s, 37 * s, 38 * s, 43 * s, 47 * s);
	p.cubicTo (49 * s, 56 * s, 47 * s, 51 * s, 49 * s, 47 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();

	g.setColour (colour);
	p.startNewSubPath (53 * s, 10 * s);
	p.lineTo (53 * s, 53 * s);
	p.startNewSubPath (50 * s, 49 * s);
	p.lineTo (53 * s, 53 * s);
	p.lineTo (56 * s, 49 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 2.50f * s)));
	p.clear ();
}



void draw_pcmGlyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
	const float s = area.getWidth () / 64.0f;	// the SVG's viewBox is 64 wide
	juce::Path p;

	g.setColour (colour);
	p.startNewSubPath (7 * s, 43 * s);
	p.lineTo (13 * s, 43 * s);
	p.lineTo (13 * s, 34 * s);
	p.lineTo (20 * s, 34 * s);
	p.lineTo (20 * s, 22 * s);
	p.lineTo (27 * s, 22 * s);
	p.lineTo (27 * s, 28 * s);
	p.lineTo (34 * s, 28 * s);
	p.lineTo (34 * s, 42 * s);
	p.lineTo (41 * s, 42 * s);
	p.lineTo (41 * s, 31 * s);
	p.lineTo (48 * s, 31 * s);
	p.lineTo (48 * s, 18 * s);
	p.lineTo (57 * s, 18 * s);
	g.fillEllipse (10.40 * s, 40.40 * s,
			   5.20 * s, 5.20 * s);
	g.fillEllipse (17.40 * s, 31.40 * s,
			   5.20 * s, 5.20 * s);
	g.fillEllipse (24.40 * s, 19.40 * s,
			   5.20 * s, 5.20 * s);
	g.fillEllipse (31.40 * s, 25.40 * s,
			   5.20 * s, 5.20 * s);
	g.fillEllipse (38.40 * s, 39.40 * s,
			   5.20 * s, 5.20 * s);
	g.fillEllipse (45.40 * s, 28.40 * s,
			   5.20 * s, 5.20 * s);
	g.fillEllipse (54.40 * s, 15.40 * s,
			   5.20 * s, 5.20 * s);
	g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, 3.00f * s)));
	p.clear ();
}



void drawChannelGlyph (juce::Graphics& g, ChannelGlyph glyph,
						  juce::Rectangle<float> area, juce::Colour colour)
{
	// The generated bodies draw in viewBox units from (0, 0); this puts that origin where
	// the caller asked without every one of them repeating the offset.
	juce::Graphics::ScopedSaveState save (g);
	g.addTransform (juce::AffineTransform::translation (area.getX (), area.getY ()));

	switch (glyph)
	{
	case ChannelGlyph::pulse:	draw_pulseGlyph (g, area, colour); break;
	case ChannelGlyph::triangle:	draw_triangleGlyph (g, area, colour); break;
	case ChannelGlyph::saw:	draw_sawGlyph (g, area, colour); break;
	case ChannelGlyph::noise:	draw_noiseGlyph (g, area, colour); break;
	case ChannelGlyph::fm:	draw_fmGlyph (g, area, colour); break;
	case ChannelGlyph::wavetable:	draw_wavetableGlyph (g, area, colour); break;
	case ChannelGlyph::pcm:	draw_pcmGlyph (g, area, colour); break;
	case ChannelGlyph::none:	break;
	}
}

} // namespace toybox::ui
