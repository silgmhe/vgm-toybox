/*
	The button paint test - it renders button faces into images and checks the invariants
	docs/ui-button-design.md promises.

	The light theme draws white faces on white cards, so "does this button float?" cannot be
	answered by asking an API for a colour: the answer only exists in the pixels (the shadow, the
	hairline, the accent ring). That is also why this is a test of its own - "the shadow
	disappeared" is obvious in an image and invisible in a build log.

	Checks: the card background, the hairline edge, the rounded corner, the shadow below the face,
	that pressing shrinks that shadow, that a toggled face is filled with the accent colour, that a
	disabled face reads as inactive, that focus draws the accent ring, that a toggled prominent
	button gets the card-painted glow (and that the repaint area the card is told to use covers
	it), and that a real IconButton paints its own shadow inside its own bounds.

	It links ui/ButtonPaint.h, ui/IconButton.cpp and ui/Icons.cpp - no engine, no libvgm.

	Run: <build dir>/plugin/vgm_ui_test
*/

#include <cstdio>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/ButtonPaint.h"
#include "ui/IconButton.h"
#include "ui/Theme.h"

namespace
{
	using namespace toybox::ui;

	int failures = 0;

	void check (bool ok, const char* what)
	{
		std::printf ("    %s %s\n", ok ? " OK " : "FAIL", what);
		if (! ok)
		{
			++failures;
		}
	}

	// --- the test image: a card with room around the control for its shadow and focus ring ------

	constexpr int kImageWidth = 72;
	constexpr int kImageHeight = 56;
	constexpr int kControlX = 20;
	constexpr int kControlY = 10;
	constexpr int kControlSize = 34;			// what the transport buttons use

	// The face is the control inset by the shadow room (buttonFaceShape), so these follow from
	// the constants above - and would follow a change of buttonShadowInset, which is the point.
	constexpr int kFaceLeft = kControlX + 2;
	constexpr int kFaceTop = kControlY + 2;
	constexpr int kFaceSize = kControlSize - 4;
	constexpr int kFaceCentreX = kFaceLeft + kFaceSize / 2;
	constexpr int kFaceCentreY = kFaceTop + kFaceSize / 2;
	constexpr int kBelowFaceY = kFaceTop + kFaceSize;		// the first row under the face
	constexpr int kBackgroundX = 3;
	constexpr int kBackgroundY = 52;

	juce::Rectangle<float> controlBounds (void)
	{
		return { (float) kControlX, (float) kControlY, (float) kControlSize, (float) kControlSize };
	}

	float brightnessAt (const juce::Image& image, int x, int y)
	{
		return image.getPixelAt (x, y).getPerceivedBrightness ();
	}

	juce::Image renderFace (const ButtonFace& face)
	{
		juce::Image image (juce::Image::ARGB, kImageWidth, kImageHeight, true);
		juce::Graphics g (image);
		g.fillAll (theme::card());
		drawButtonFace (g, controlBounds(), face);
		return image;
	}

	void checkTheFace (void)
	{
		std::printf ("[1] the face of a button\n");

		const auto normal = renderFace (ButtonFace {});
		const float background = brightnessAt (normal, kBackgroundX, kBackgroundY);
		const float hairline = brightnessAt (normal, kFaceCentreX, kFaceTop);

		check (background > 0.98f, "the card behind the button is white");
		check (hairline < background - 0.03f, "the face has a hairline along its edge");
		check (brightnessAt (normal, kFaceLeft, kFaceTop) > hairline + 0.02f,
			   "and a rounded corner - the hairline stops before the corner");
		check (brightnessAt (normal, kFaceCentreX, kBelowFaceY) < background - 0.01f,
			   "the face floats: there is a shadow under it");

		ButtonFace pressed;
		pressed.pressed = true;
		const auto pressedImage = renderFace (pressed);
		std::printf ("    under the face: normal %.4f, pressed %.4f\n",
					 brightnessAt (normal, kFaceCentreX, kBelowFaceY),
					 brightnessAt (pressedImage, kFaceCentreX, kBelowFaceY));
		check (brightnessAt (pressedImage, kFaceCentreX, kBelowFaceY)
				   > brightnessAt (normal, kFaceCentreX, kBelowFaceY),
			   "pressing shrinks the shadow instead of only darkening the fill");
	}

	void checkTheOtherStates (void)
	{
		std::printf ("[2] filled, disabled and focused faces\n");

		const auto normal = renderFace (ButtonFace {});

		ButtonFace filled;
		filled.level = ButtonLevel::filled;
		filled.toggled = true;
		filled.fill = theme::accent();
		const auto filledImage = renderFace (filled);
		const auto filledCentre = filledImage.getPixelAt (kFaceCentreX, kFaceCentreY);
		// relative, because Colour's accessors are on their own scale (0..1 or 0..255 depending on
		// the JUCE version) - a grey pixel has no blue-over-red at all, the accent has plenty
		check ((filledCentre.getBlue() - filledCentre.getRed()) > 0.5f * filledCentre.getBlue(),
			   "a toggled face is filled with the accent colour");

		ButtonFace disabled;
		disabled.enabled = false;
		const auto disabledImage = renderFace (disabled);
		std::printf ("    face centre: enabled %.4f, disabled %.4f\n",
					 brightnessAt (normal, kFaceCentreX, kFaceCentreY),
					 brightnessAt (disabledImage, kFaceCentreX, kFaceCentreY));
		check (brightnessAt (disabledImage, kFaceCentreX, kFaceCentreY)
				   < brightnessAt (normal, kFaceCentreX, kFaceCentreY) - 0.02f,
			   "a disabled face reads as inactive on a white card");

		ButtonFace focused;
		focused.hasFocus = true;
		const auto focusedImage = renderFace (focused);
		const auto ring = focusedImage.getPixelAt (kFaceCentreX, kFaceTop - 1);
		std::printf ("    focus ring: blue %.1f, red %.1f\n", (double) ring.getBlue(), (double) ring.getRed());
		check ((ring.getBlue() - ring.getRed()) > 0.15f * ring.getBlue(),
			   "focus draws the accent ring around the face");
	}

	void checkTheGlow (void)
	{
		std::printf ("[3] the glow of a toggled prominent button\n");

		juce::Image image (juce::Image::ARGB, kImageWidth, kImageHeight, true);
		{
			juce::Graphics g (image);
			g.fillAll (theme::card());
			drawButtonGlow (g, controlBounds().toNearestInt());
		}

		const auto glow = image.getPixelAt (kFaceCentreX, kBelowFaceY + 2);
		const auto without = renderFace (ButtonFace {}).getPixelAt (kFaceCentreX, kBelowFaceY + 2);
		const double glowTint = glow.getBlue() - glow.getRed();
		const double plainTint = without.getBlue() - without.getRed();
		std::printf ("    below the button: accent tint %.1f (with glow) / %.1f (without)\n",
					 glowTint, plainTint);
		check (glowTint > plainTint + 0.1f * glow.getBlue(),
			   "the card paints an accent glow behind a toggled button");
		check (buttonGlowBounds (controlBounds().toNearestInt()).contains (kFaceCentreX, kBelowFaceY + 2),
			   "and the area the card is told to repaint covers it");
	}

	void checkTheWidget (void)
	{
		std::printf ("[4] a real IconButton\n");

		IconButton button { "Play", IconButton::Icon::play };
		button.setBounds (0, 0, kControlSize, kControlSize);

		juce::Image image (juce::Image::ARGB, kControlSize, kControlSize, true);
		{
			juce::Graphics g (image);
			g.fillAll (theme::card());
			button.paintEntireComponent (g, true);
		}

		const float topLeft = brightnessAt (image, 1, 1);
		const float bottomRow = brightnessAt (image, kControlSize / 2, kControlSize - 1);
		std::printf ("    IconButton: top-left %.4f, bottom row %.4f\n", topLeft, bottomRow);
		check (bottomRow < topLeft - 0.01f, "an IconButton paints its own shadow inside its bounds");
	}
}

int main()
{
	const juce::ScopedJuceInitialiser_GUI init;

	std::printf ("VGMToybox button paint test\n");
	checkTheFace();
	checkTheOtherStates();
	checkTheGlow();
	checkTheWidget();

	std::printf ("Result    : %s\n", (failures == 0) ? "all checks passed" : "FAILURES");
	return (failures == 0) ? 0 : 1;
}
