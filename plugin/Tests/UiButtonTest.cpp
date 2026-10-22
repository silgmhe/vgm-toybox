/*
	The button paint test - it renders button faces into images and checks the invariants
	docs/desgin/ui-button-design.md promises.

	The light theme draws white faces on white cards, so "does this button float?" cannot be
	answered by asking an API for a colour: the answer only exists in the pixels (the shadow, the
	hairline, the accent ring). That is also why this is a test of its own - "the shadow
	disappeared" is obvious in an image and invisible in a build log.

	Checks: the card background, the hairline edge, the rounded corner, the shadow below the face,
	that pressing shrinks that shadow, that a toggled face is filled with the accent colour, that a
	disabled face reads as inactive, that focus draws the accent ring, that a toggled prominent
	button gets the card-painted glow (and that the repaint area the card is told to use covers
	it), and that a real IconButton paints its own shadow inside its own bounds.

	It also covers the mixer's MIDI marks, which are the one place in this UI where a wrong shape
	is invisible in the build log and obvious to the user: the three glyphs have to be distinct,
	they have to stay inside the area they are given, the keyboard has to be top-heavy (its black
	keys) and the sample left-heavy (its attack) - and the selector's popup has to carry the MIDI
	mark on every channel entry, not just on the first.

	It links ui/ButtonPaint.h, ui/IconButton.cpp and ui/Icons.cpp - no engine, no libvgm.

	Run: <build dir>/plugin/vgm_ui_test
*/

#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/ButtonPaint.h"
#include "ui/IconButton.h"
#include "ui/Icons.h"
#include "ui/MidiMenu.h"
#include "ui/Theme.h"

#include "TestSettings.h"
#include "TestSupport.h"

namespace
{
	using namespace toybox::ui;

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

	void checkTheCompactGlow (void)
	{
		std::printf ("[3b] the compact glow of a narrow lane\n");

		// The mixer's M/S buttons sit 6 px from their strip's edge. Render both blur sizes into a
		// lane of exactly that size and look at the pixel just inside its edge: the glow has to be
		// gone there, or the strip's bounds cut it into a glowing rectangle around the button -
		// which is what a 12 px blur did before the compact one existed.
		const auto lane = controlBounds().toNearestInt().expanded (6);
		const float white = theme::card().getPerceivedBrightness();

		// The pixels are read *after* the Graphics has been destroyed: while it is alive the
		// renderer has not flushed yet and every sample comes back transparent black.
		const auto brightnessAtLaneEdge = [&lane] (int blur)
		{
			juce::Image image (juce::Image::ARGB, lane.getWidth(), lane.getHeight(), true);
			{
				juce::Graphics g (image);
				g.fillAll (theme::card());
				drawButtonGlow (g, controlBounds().toNearestInt().translated (-lane.getX(), -lane.getY()),
								theme::muteGlow(), blur);
			}
			return image.getPixelAt (lane.getWidth() / 2, lane.getHeight() - 1).getPerceivedBrightness();
		};

		const float compactEdge = brightnessAtLaneEdge (theme::compactGlowBlur);
		const float wideEdge = brightnessAtLaneEdge (theme::accentGlowBlur);
		std::printf ("    at the lane's edge: compact %.4f, wide %.4f (the white card is %.4f)\n",
					 compactEdge, wideEdge, white);
		check ((white - compactEdge) < 0.4f * (white - wideEdge),
			   "the compact glow has faded out by the lane's edge (the wide one has not)");
	}

	void checkTheWidget (void)
	{
		std::printf ("[4] a real IconButton\n");

		IconButton button { "play", "Play", IconButton::Icon::play };
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

	// --- the mixer's MIDI marks ---------------------------------------------------------------

	using Glyph = void (*) (juce::Graphics&, juce::Rectangle<float>, juce::Colour);

	constexpr int kGlyphImage = 32;
	constexpr int kGlyphInset = 8;		// a 16x16 mark in the middle of a white page
	constexpr int kGlyphSize = 16;
	// Anything darker than this counts as ink. Not 0.5: a one-pixel line at badge size is often
	// drawn half on one pixel and half on the next, and both halves have to count.
	constexpr float kInkedBrightness = 0.75f;

	juce::Image renderGlyph (Glyph glyph)
	{
		juce::Image image (juce::Image::ARGB, kGlyphImage, kGlyphImage, true);
		{
			juce::Graphics g (image);
			g.fillAll (juce::Colours::white);
			glyph (g, juce::Rectangle<float> ((float) kGlyphInset, (float) kGlyphInset,
											  (float) kGlyphSize, (float) kGlyphSize),
				   juce::Colours::black);
		}
		return image;
	}

	int inkIn (const juce::Image& image, juce::Rectangle<int> area)
	{
		int ink = 0;
		for (int y = area.getY(); y < area.getBottom(); ++y)
		{
			for (int x = area.getX(); x < area.getRight(); ++x)
			{
				if (image.getPixelAt (x, y).getPerceivedBrightness() < kInkedBrightness)
				{
					++ink;
				}
			}
		}
		return ink;
	}

	/** The number of separate ink runs along one row: a keyboard row crosses its frame and its
		white-key separators, and a glyph without separators simply has fewer. */
	int inkRuns (const juce::Image& image, int y, juce::Rectangle<int> area)
	{
		int runs = 0;
		bool inRun = false;
		for (int x = area.getX(); x < area.getRight(); ++x)
		{
			const bool ink = image.getPixelAt (x, y).getPerceivedBrightness() < kInkedBrightness;
			if (ink && ! inRun)
			{
				++runs;
			}
			inRun = ink;
		}
		return runs;
	}

	juce::Rectangle<int> inkBounds (const juce::Image& image)
	{
		int left = image.getWidth();
		int top = image.getHeight();
		int right = -1;
		int bottom = -1;
		for (int y = 0; y < image.getHeight(); ++y)
		{
			for (int x = 0; x < image.getWidth(); ++x)
			{
				if (image.getPixelAt (x, y).getPerceivedBrightness() < kInkedBrightness)
				{
					left = juce::jmin (left, x);
					top = juce::jmin (top, y);
					right = juce::jmax (right, x);
					bottom = juce::jmax (bottom, y);
				}
			}
		}
		return juce::Rectangle<int>::leftTopRightBottom (left, top, right + 1, bottom + 1);
	}

	void checkTheMidiGlyphs (void)
	{
		std::printf ("[5] the MIDI marks\n");

		const auto melody = renderGlyph (drawMelodyGlyph);
		const auto sample = renderGlyph (drawSampleGlyph);
		const auto midi = renderGlyph (drawMidiGlyph);

		const auto area = juce::Rectangle<int> (kGlyphInset, kGlyphInset, kGlyphSize, kGlyphSize);
		const int melodyInk = inkIn (melody, area);
		const int sampleInk = inkIn (sample, area);
		const int midiInk = inkIn (midi, area);
		std::printf ("    ink: keyboard %d, sample %d, MIDI %d\n", melodyInk, sampleInk, midiInk);

		check (melodyInk > 15 && sampleInk > 15 && midiInk > 15, "every mark paints something");
		check (melodyInk != sampleInk && sampleInk != midiInk && melodyInk != midiInk,
			   "the three marks are three different drawings");

		const auto drawn = area.expanded (1);
		check (drawn.contains (inkBounds (melody)) && drawn.contains (inkBounds (sample))
				   && drawn.contains (inkBounds (midi)),
			   "and none of them paints outside the area it was given");

		// A keyboard is heavier at the top: the black keys hang from the top edge. A sample is
		// heavier on the left: the attack and the loudest part of the burst. If either glyph were
		// replaced by "some lines", these are the two checks that would notice.
		const auto melodyTop = area.withHeight (kGlyphSize / 2);
		const auto melodyBottom = area.withTop (area.getY() + kGlyphSize / 2);
		const int melodyTopInk = inkIn (melody, melodyTop);
		const int melodyBottomInk = inkIn (melody, melodyBottom);

		const auto sampleLeft = area.withWidth (kGlyphSize / 2);
		const auto sampleRight = area.withLeft (area.getX() + kGlyphSize / 2);
		const int sampleLeftInk = inkIn (sample, sampleLeft);
		const int sampleRightInk = inkIn (sample, sampleRight);

		std::printf ("    keyboard top %d / bottom %d, sample left %d / right %d\n",
					 melodyTopInk, melodyBottomInk, sampleLeftInk, sampleRightInk);
		check (melodyTopInk > melodyBottomInk, "the keyboard puts its black keys on top");
		check (sampleLeftInk > sampleRightInk, "the sample decays to the right");

		// The white keys have to be separated, not just framed: a row across the lower half of the
		// keyboard crosses the two frame edges and the separators between the white keys. Without
		// them the mark reads as a frame with two tabs, which is what the first version looked like.
		// 0.70 of the height: inside the keyboard body, below the black keys (they end at 0.46) and
		// above the bottom frame edge (0.86) - a row on the frame would be one long run.
		const int bodyRow = area.getY() + (int) std::lround ((float) kGlyphSize * 0.70f);
		const int runs = inkRuns (melody, bodyRow, area);
		const auto lowerMiddle = juce::Rectangle<int> (area.getX() + 2, bodyRow, kGlyphSize - 4, 3);
		std::printf ("    keyboard row %d: %d ink runs, %d inked pixels between the frame edges\n",
					 bodyRow, runs, inkIn (melody, lowerMiddle));
		check (runs >= 4, "the keyboard separates its white keys");
		check (inkIn (melody, lowerMiddle) > 0, "and the separators are inside the frame, not at the edge");

		// The MIDI mark is a ring with contacts, not a disc: the middle stays empty.
		const auto centre = juce::Rectangle<int> (area.getCentreX() - 2, area.getCentreY() - 2, 4, 4);
		check (inkIn (midi, centre) < 4, "the MIDI mark is a connector ring, not a filled blob");
	}

	void checkTheMidiMenu (void)
	{
		std::printf ("[6] the MIDI selector popup\n");

		const auto icon = midiGlyphImage (56, juce::Colours::black);
		check (icon.isValid() && icon.getWidth() == 56 && icon.getHeight() == 56,
			   "the popup icon rasterises at the size it is asked for");

		juce::StringArray channels;
		for (int channel = 1; channel <= 16; ++channel)
		{
			channels.add ("Ch." + juce::String (channel));
		}

		const auto menu = makeMidiChannelMenu ("Off", channels, 3, icon);

		struct Entry
		{
			int id = 0;
			juce::String text;
			bool hasIcon = false;
			bool ticked = false;
		};

		std::vector<Entry> entries;
		for (juce::PopupMenu::MenuItemIterator iterator (menu); iterator.next();)
		{
			const auto& item = iterator.getItem();
			// Item::image is a Drawable (an Image passed to addItem is wrapped in one), so the
			// question is "does this entry carry an icon at all", not "is the image valid".
			entries.push_back (Entry { item.itemID, item.text, item.image != nullptr, item.isTicked });
		}

		std::printf ("    %d entries: \"%s\" ... \"%s\"\n", (int) entries.size(),
					 entries.empty() ? "" : entries.front().text.toRawUTF8(),
					 entries.empty() ? "" : entries.back().text.toRawUTF8());

		check (entries.size() == 17, "Off plus sixteen channels");
		if (entries.size() != 17)
		{
			return;
		}

		check (entries.front().id == 1 && entries.front().text == "Off" && ! entries.front().hasIcon,
			   "Off is a plain entry: there is no MIDI channel behind it");
		check (entries[1].id == 2 && entries[1].text == "Ch.1", "then Ch.1");
		check (entries[16].id == 17 && entries[16].text == "Ch.16",
			   "through Ch.16, with the ids rising one per entry");

		int withIcon = 0;
		int ticked = 0;
		for (const auto& entry : entries)
		{
			withIcon += entry.hasIcon ? 1 : 0;
			ticked += entry.ticked ? 1 : 0;
		}

		check (withIcon == 16, "every channel entry carries the MIDI mark");
		check (ticked == 1 && entries[2].ticked, "and only the selected channel is ticked");
	}
}

int main()
{
	const juce::ScopedJuceInitialiser_GUI init;

	std::printf ("VGMToybox button paint test\n");

	// Isolated settings - see TestSettings.h.
	const toybox::test::ScopedSettingsDirectory settings;
	checkTheFace();
	checkTheOtherStates();
	checkTheGlow();
	checkTheCompactGlow();
	checkTheWidget();
	checkTheMidiGlyphs();
	checkTheMidiMenu();

	std::printf ("Result    : %s\n", (toybox::test::failures() == 0) ? "all checks passed" : "FAILURES");
	return (toybox::test::failures() == 0) ? 0 : 1;
}
