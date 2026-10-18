#include "FontChooserDialog.h"

#include "Theme.h"
#include "UiFont.h"
#include "i18n/LanguageManager.h"

namespace toybox::ui
{

FontChooserDialog::FontChooserDialog (settings::SettingsStore::FontMode mode,
									  const juce::String& family)
	: selectedFamily (mode == settings::SettingsStore::FontMode::custom ? family.trim() : juce::String()),
	  list ("fonts", this)
{
	setSize (460, 540);
	setLookAndFeel (&lookAndFeel);

	search.setComponentID ("settings.appearance.font.search");
	search.setTextToShowWhenEmpty (i18n::tr ("settings.appearance.font.search"), theme::secondaryText());
	search.onTextChange = [this] { rebuild(); };
	search.onReturnKey = [this] { accept(); };
	search.onEscapeKey = [this] { cancel(); };
	addAndMakeVisible (search);

	list.setComponentID ("settings.appearance.font.list");
	list.setRowHeight (24);
	list.setMultipleSelectionEnabled (false);
	list.setColour (juce::ListBox::backgroundColourId, theme::card());
	list.setColour (juce::ListBox::outlineColourId, theme::border());
	list.setColour (juce::ListBox::textColourId, theme::primaryText());
	addAndMakeVisible (list);

	previewLarge.setComponentID ("settings.appearance.font.previewLarge");
	previewLarge.setText (i18n::tr ("settings.appearance.font.previewLarge"), juce::dontSendNotification);
	previewLarge.setJustificationType (juce::Justification::centredLeft);
	previewLarge.setMinimumHorizontalScale (0.8f);
	addAndMakeVisible (previewLarge);

	previewSmall.setComponentID ("settings.appearance.font.previewSmall");
	previewSmall.setText (i18n::tr ("settings.appearance.font.previewSmall"), juce::dontSendNotification);
	previewSmall.setJustificationType (juce::Justification::centredLeft);
	previewSmall.setMinimumHorizontalScale (0.8f);
	addAndMakeVisible (previewSmall);

	okButton.setComponentID ("settings.appearance.font.ok");
	okButton.setButtonText (i18n::tr ("settings.appearance.font.ok"));
	okButton.onClick = [this] { accept(); };
	addAndMakeVisible (okButton);

	cancelButton.setComponentID ("settings.appearance.font.cancel");
	cancelButton.setButtonText (i18n::tr ("settings.appearance.font.cancel"));
	cancelButton.onClick = [this] { cancel(); };
	addAndMakeVisible (cancelButton);

	for (const auto& name : juce::Font::findAllTypefaceNames())
	{
		if (name.isNotEmpty()
			&& ! name.startsWithChar ('<')
			&& ! name.equalsIgnoreCase (fonts::systemUiFamily())
			&& ! allFonts.contains (name, true))
		{
			allFonts.add (name);
		}
	}

	allFonts.sort (true);
	rebuild();
	lookAndFeelChanged();
}

void FontChooserDialog::paint (juce::Graphics& g)
{
	g.fillAll (theme::card());
}

void FontChooserDialog::resized (void)
{
	auto area = getLocalBounds().reduced (16);

	search.setBounds (area.removeFromTop (30));
	area.removeFromTop (10);

	auto buttons = area.removeFromBottom (32);
	cancelButton.setBounds (buttons.removeFromRight (88));
	buttons.removeFromRight (8);
	okButton.setBounds (buttons.removeFromRight (88));
	buttons.removeFromRight (8);

	area.removeFromBottom (12);

	auto preview = area.removeFromBottom (78);
	previewLarge.setBounds (preview.removeFromTop (38).reduced (2, 0));
	previewSmall.setBounds (preview.removeFromTop (24).reduced (2, 0));

	area.removeFromBottom (10);
	list.setBounds (area);
}

bool FontChooserDialog::keyPressed (const juce::KeyPress& key)
{
	// The search box handles its own escape; this covers the focus being on the list or a
	// button, where an escape that did nothing would make the modal dialog look stuck.
	if (key == juce::KeyPress (juce::KeyPress::escapeKey))
	{
		cancel();
		return true;
	}

	return false;
}

void FontChooserDialog::lookAndFeelChanged (void)
{
	previewLarge.setColour (juce::Label::textColourId, theme::primaryText());
	previewSmall.setColour (juce::Label::textColourId, theme::secondaryText());

	search.setColour (juce::TextEditor::backgroundColourId, theme::window());
	search.setColour (juce::TextEditor::textColourId, theme::primaryText());
	search.setColour (juce::TextEditor::outlineColourId, theme::border());
	search.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent());

	okButton.setColour (juce::TextButton::buttonColourId, theme::accent());
	okButton.setColour (juce::TextButton::textColourOffId, theme::onAccent());
	cancelButton.setColour (juce::TextButton::buttonColourId, theme::card());
	cancelButton.setColour (juce::TextButton::textColourOffId, theme::primaryText());
}

int FontChooserDialog::getNumRows (void)
{
	return 1 + filteredFonts.size();
}

void FontChooserDialog::paintListBoxItem (int row, juce::Graphics& g,
										  int width, int height, bool rowIsSelected)
{
	if (row < 0 || row > filteredFonts.size())
	{
		return;
	}

	g.fillAll (rowIsSelected ? theme::accent().withAlpha (0.16f) : theme::card());

	const auto text = row == 0
		? i18n::tr ("settings.appearance.font.systemOption")
		: filteredFonts[row - 1];

	g.setColour (rowIsSelected ? theme::accent() : theme::primaryText());
	g.setFont (juce::Font (juce::FontOptions (13.0f, row == 0 ? juce::Font::bold : juce::Font::plain)));
	g.drawText (text, juce::Rectangle<int> (10, 0, width - 20, height),
				juce::Justification::centredLeft, true);
}

void FontChooserDialog::listBoxItemDoubleClicked (int, const juce::MouseEvent&)
{
	accept();
}

void FontChooserDialog::selectedRowsChanged (int lastRowSelected)
{
	if (lastRowSelected < 0)
	{
		return;
	}

	selectedFamily = familyForRow (lastRowSelected);
	updatePreview();
}

void FontChooserDialog::returnKeyPressed (int)
{
	accept();
}

void FontChooserDialog::rebuild (void)
{
	const auto filter = search.getText().trim();

	filteredFonts.clear();

	for (const auto& family : allFonts)
	{
		if (filter.isEmpty() || family.containsIgnoreCase (filter))
		{
			filteredFonts.add (family);
		}
	}

	list.updateContent();

	auto row = rowForFamily (selectedFamily);

	// While the user types a filter, keep the preview on something the list shows. Falling
	// back to System UI here would make Enter accept the system font even when the filter
	// matched a family, so the first visible family is the better fallback.
	if (filter.isNotEmpty() && (selectedFamily.isEmpty() || row < 0))
	{
		row = filteredFonts.isEmpty() ? 0 : 1;
	}
	else if (row < 0)
	{
		row = 0;
	}

	list.selectRow (row, true);
	updatePreview();
}

void FontChooserDialog::updatePreview (void)
{
	const auto family = selectedFamily.isEmpty() ? fonts::systemUiFamily() : selectedFamily;

	previewLarge.setFont (juce::Font (juce::FontOptions (family, 22.0f, juce::Font::plain)));
	previewSmall.setFont (juce::Font (juce::FontOptions (family, 12.5f, juce::Font::plain)));
}

void FontChooserDialog::accept (void)
{
	if (onAccept != nullptr)
	{
		onAccept (selectedFamily.isEmpty() ? settings::SettingsStore::FontMode::systemUI
										   : settings::SettingsStore::FontMode::custom,
				  selectedFamily);
	}

	closeDialog();
}

void FontChooserDialog::cancel (void)
{
	closeDialog();
}

void FontChooserDialog::closeDialog (void)
{
	if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
	{
		dialog->exitModalState (0);
	}
}

int FontChooserDialog::rowForFamily (const juce::String& family) const
{
	if (family.trim().isEmpty())
	{
		return 0;
	}

	for (int i = 0; i < filteredFonts.size(); ++i)
	{
		if (filteredFonts[i].equalsIgnoreCase (family))
		{
			return i + 1;
		}
	}

	return -1;
}

juce::String FontChooserDialog::familyForRow (int row) const
{
	if (row <= 0 || row > filteredFonts.size())
	{
		return {};
	}

	return filteredFonts[row - 1];
}

} // namespace toybox::ui
