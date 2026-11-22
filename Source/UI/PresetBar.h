#pragma once

#include "PluginProcessor.h"
#include "Widgets.h"

namespace ui
{
/** The preset bank: eight boards that can be recalled with one click, also while playing.

    Click a preset to recall it (the current one again: back to how it was saved). Click an empty one to
    store the board there. SAVE stores the board in the current preset, SAVE AS in the preset clicked next,
    RENAME names the current one. A preset that was changed and not saved turns amber and gets a star;
    recalling another preset drops those changes, as on a hardware unit. */
class PresetBar : public juce::Component
{
public:
    explicit PresetBar (PedalboardProcessor& p) : processor (p)
    {
        for (int i = 0; i < PedalboardProcessor::numPresets; ++i)
        {
            auto tile = std::make_unique<Tile> (i);
            tile->onClick = [this, i] { tileClicked (i); };
            addAndMakeVisible (*tile);
            tiles.push_back (std::move (tile));
        }

        for (auto* b : { &saveButton, &saveAsButton, &renameButton })
        {
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c1c20));
            b->setColour (juce::TextButton::textColourOffId, colours::text);
            addAndMakeVisible (*b);
        }

        saveButton.setTooltip ("Store the current settings in the current preset");
        saveButton.onClick = [this]
        {
            finishRename (true);
            setPicking (false);
            processor.savePreset (processor.getCurrentPreset());
            refresh (true);
        };

        saveAsButton.setTooltip ("Store the current settings in another preset: click this, then the preset to store them in");
        saveAsButton.onClick = [this]
        {
            finishRename (true);
            setPicking (! picking);
            refresh (true);
        };

        renameButton.setTooltip ("Give the current preset another name (Enter to keep it, Esc to cancel)");
        renameButton.onClick = [this]
        {
            setPicking (false);
            startRename();
        };

        nameEditor.setFont (boldFont (12.0f));
        nameEditor.setJustification (juce::Justification::centredLeft);
        nameEditor.setIndents (6, 0);
        nameEditor.setInputRestrictions (PedalboardProcessor::maxPresetNameLength);
        nameEditor.setSelectAllWhenFocused (true);
        nameEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101014));
        nameEditor.setColour (juce::TextEditor::textColourId, colours::text);
        nameEditor.setColour (juce::TextEditor::highlightColourId, colours::amber.withAlpha (0.35f));
        nameEditor.setColour (juce::TextEditor::outlineColourId, colours::amber);
        nameEditor.setColour (juce::TextEditor::focusedOutlineColourId, colours::amber);
        nameEditor.onReturnKey = [this] { finishRename (true); };
        nameEditor.onEscapeKey = [this] { finishRename (false); };
        nameEditor.onFocusLost = [this] { finishRename (true); };
        addChildComponent (nameEditor);

        refresh (true);
    }

    /** Call from the editor's timer: follows the bank, the board and the SAVE AS countdown. */
    void refresh (bool now = false)
    {
        // comparing the board with the preset is the only part with a cost: a few times a second is plenty
        if (now || ++ticks % 6 == 0)
            modified = processor.isPresetModified();

        if (picking && juce::Time::getMillisecondCounter() - pickStarted > 10000u)
            setPicking (false); // forgotten: a later click must recall a preset again, not overwrite one

        const int current = processor.getCurrentPreset();
        const float pulse = picking ? 0.5f + 0.5f * std::sin ((float) (juce::Time::getMillisecondCounter() % 100000u) * 0.008f) : 0.0f;

        for (int i = 0; i < (int) tiles.size(); ++i)
        {
            Tile::Look look;
            look.name = processor.getPresetName (i);
            look.filled = ! processor.isPresetEmpty (i);
            look.current = i == current;
            look.modified = look.current && modified;
            look.picking = picking;

            if (tiles[(size_t) i]->setLook (look, pulse))
                tiles[(size_t) i]->setTooltip (tooltipFor (look));
        }

        if (modified != saveShowsModified)
        {
            saveShowsModified = modified;
            saveButton.setColour (juce::TextButton::textColourOffId, modified ? colours::amber : colours::text);
        }

        if (picking != (saveAsButton.getButtonText() == "CANCEL"))
            saveAsButton.setButtonText (picking ? "CANCEL" : "SAVE AS");
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff26262c), 0.0f, area.getY(),
                                                 juce::Colour (0xff18181c), 0.0f, area.getBottom(), false));
        g.fillRoundedRectangle (area, 10.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (area, 10.0f, 1.2f);

        g.setFont (boldFont (12.0f));
        g.setColour (colours::dimText);
        g.drawText ("PRESET", labelArea, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14, 7);
        labelArea = area.removeFromLeft (58).toFloat();

        auto buttons = area.removeFromRight (52 + 4 + 68 + 4 + 68);
        saveButton.setBounds (buttons.removeFromLeft (52));
        buttons.removeFromLeft (4);
        saveAsButton.setBounds (buttons.removeFromLeft (68));
        buttons.removeFromLeft (4);
        renameButton.setBounds (buttons);
        area.removeFromRight (12);

        constexpr int gap = 4;
        const int width = (area.getWidth() - gap * ((int) tiles.size() - 1)) / (int) tiles.size();
        for (auto& tile : tiles)
        {
            tile->setBounds (area.removeFromLeft (width));
            area.removeFromLeft (gap);
        }

        if (renaming >= 0)
            nameEditor.setBounds (tiles[(size_t) renaming]->getBounds());
    }

private:
    //==============================================================================
    class Tile : public juce::Button
    {
    public:
        struct Look
        {
            juce::String name;
            bool filled = false, current = false, modified = false, picking = false;

            bool operator== (const Look& o) const
            {
                return name == o.name && filled == o.filled && current == o.current && modified == o.modified && picking == o.picking;
            }
        };

        explicit Tile (int i) : juce::Button ("Preset " + juce::String (i + 1)), index (i)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        /** Returns true when anything but the pulse changed. */
        bool setLook (const Look& newLook, float newPulse)
        {
            const bool changed = ! shown || ! (newLook == look);
            if (changed || newPulse != pulse)
            {
                shown = true;
                look = newLook;
                pulse = newPulse;
                if (changed)
                    setButtonText (look.filled ? look.name : juce::String ("+"));
                repaint();
            }
            return changed;
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool) override
        {
            const auto area = getLocalBounds().toFloat().reduced (1.0f);
            const auto accent = look.modified ? colours::amber : colours::ledGreen;

            if (look.current && look.filled)
                g.setColour (accent.withAlpha (0.22f));
            else if (look.filled)
                g.setColour (juce::Colour (highlighted ? 0xff2a2a30 : 0xff1c1c20));
            else
                g.setColour (juce::Colour (0xff1c1c20).withAlpha (highlighted ? 0.9f : 0.35f));
            g.fillRoundedRectangle (area, 5.0f);

            if (look.picking)
            {
                g.setColour (colours::amber.withAlpha (0.4f + 0.6f * pulse));
                g.drawRoundedRectangle (area, 5.0f, 1.5f);
            }
            else if (look.current && look.filled)
            {
                g.setColour (accent);
                g.drawRoundedRectangle (area, 5.0f, 1.5f);
            }
            else if (look.filled)
            {
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.drawRoundedRectangle (area, 5.0f, 1.0f);
            }
            else // empty: a dashed outline, like the chain's empty slots
            {
                juce::Path outline, dashed;
                outline.addRoundedRectangle (area, 5.0f);
                const float dashes[] = { 4.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
                g.setColour (colours::dimText.withAlpha (highlighted ? 0.9f : 0.5f));
                g.fillPath (dashed);
            }

            auto textArea = area.reduced (7.0f, 0.0f);
            g.setFont (boldFont (10.5f));
            g.setColour (look.current && look.filled ? accent : colours::dimText);
            g.drawText (juce::String (index + 1), textArea.removeFromLeft (12.0f), juce::Justification::centredLeft);

            g.setFont (boldFont (12.0f));
            if (look.modified)
            {
                g.setColour (colours::amber);
                g.drawText ("*", textArea.removeFromRight (9.0f).translated (0.0f, 2.0f), juce::Justification::centredRight);
            }

            g.setColour (look.current && look.filled ? colours::text
                         : look.filled ? colours::text.withAlpha (highlighted ? 0.95f : 0.7f)
                                       : colours::dimText.withAlpha (highlighted ? 1.0f : 0.7f));
            g.drawFittedText (getButtonText(), textArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
        }

    private:
        int index;
        Look look;
        float pulse = 0.0f;
        bool shown = false;
    };

    //==============================================================================
    static juce::String tooltipFor (const Tile::Look& look)
    {
        if (look.picking)
            return look.filled ? "Store the current settings here, replacing \"" + look.name + "\""
                               : juce::String ("Store the current settings here");
        if (! look.filled)
            return "Empty: click to store the current settings here";
        if (look.modified)
            return look.name + ": changed and not saved. SAVE keeps the changes; click here to go back to the saved settings";
        if (look.current)
            return look.name + " (the current preset)";
        return look.name + ": click to recall this preset";
    }

    void tileClicked (int index)
    {
        finishRename (true);

        if (picking || processor.isPresetEmpty (index))
            processor.savePreset (index);
        else
            processor.loadPreset (index);

        setPicking (false);
        refresh (true);
    }

    void setPicking (bool shouldPick)
    {
        picking = shouldPick;
        pickStarted = juce::Time::getMillisecondCounter();
    }

    void startRename()
    {
        const int index = processor.getCurrentPreset();
        if (processor.isPresetEmpty (index))
            return;

        renaming = index;
        nameEditor.setBounds (tiles[(size_t) index]->getBounds());
        nameEditor.setText (processor.getPresetName (index), false);
        nameEditor.setVisible (true);
        nameEditor.toFront (false);
        nameEditor.grabKeyboardFocus();
        nameEditor.selectAll();
    }

    void finishRename (bool keep)
    {
        if (renaming < 0)
            return;

        const int index = std::exchange (renaming, -1); // first: hiding the editor makes it lose focus, which lands here again
        if (keep)
            processor.renamePreset (index, nameEditor.getText());

        nameEditor.setVisible (false);
        refresh (true);
    }

    PedalboardProcessor& processor;
    std::vector<std::unique_ptr<Tile>> tiles;
    juce::TextButton saveButton { "SAVE" }, saveAsButton { "SAVE AS" }, renameButton { "RENAME" };
    juce::TextEditor nameEditor;
    juce::Rectangle<float> labelArea;

    bool modified = false, saveShowsModified = false, picking = false;
    juce::uint32 pickStarted = 0;
    int renaming = -1, ticks = 0;
};

} // namespace ui
