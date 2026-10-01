#pragma once

#include "KnobText.h"
#include "PluginProcessor.h"
#include "Widgets.h"

namespace ui
{
/** Edits the block selected in the chain.
    An FX slot: effect type and model pickers, the model's knobs (relabelled per model, switches and note
    values as menus), a footswitch, and buttons to move or clear it.
    The amp block: amp and cabinet pickers, the amp's knobs and the cab / microphone controls. */
class SlotEditor : public juce::Component
{
public:
    std::function<void (int newPosition)> onMoved;

    explicit SlotEditor (PedalboardProcessor& p) : processor (p)
    {
        firstBox.onChange = [this] { firstPicked(); };
        addAndMakeVisible (firstBox);
        secondBox.onChange = [this] { secondPicked(); };
        addAndMakeVisible (secondBox);

        basedOn.setFont (plainFont (12.5f));
        basedOn.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
        basedOn.setJustificationType (juce::Justification::topLeft);
        basedOn.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (basedOn);

        for (auto* b : { &leftButton, &rightButton, &clearButton })
        {
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xcc111114));
            b->setColour (juce::TextButton::textColourOffId, colours::text);
            addAndMakeVisible (*b);
        }
        leftButton.setTooltip ("Move this block one step towards the input");
        rightButton.setTooltip ("Move this block one step towards the output");
        clearButton.setTooltip ("Empty this slot");
        leftButton.onClick  = [this] { move (-1); };
        rightButton.onClick = [this] { move (+1); };
        clearButton.onClick = [this] { if (! block.isAmp) processor.setSlotModel (block.slot, 0); refresh (true); };

        footswitch.setTooltip ("On / Off");
        addAndMakeVisible (footswitch);

        for (size_t i = 0; i < cells.size(); ++i)
        {
            auto& cell = cells[i];
            styleKnob (cell.slider, juce::Colours::white.withAlpha (0.9f));
            cell.label.setJustificationType (juce::Justification::centred);
            cell.label.setFont (boldFont (12.0f));
            cell.label.setMinimumHorizontalScale (0.7f);
            cell.label.setInterceptsMouseClicks (false, false);
            cell.choice.onChange = [this, i] { choicePicked (i); };
            addChildComponent (cell.slider);
            addChildComponent (cell.label);
            addChildComponent (cell.choice);
        }

        showPosition (1);
    }

    /** Shows the block at chain position `position`. */
    void showPosition (int position)
    {
        position = juce::jlimit (0, PedalboardProcessor::numBlocks - 1, position);
        const auto b = processor.getBlock (position);
        if (position == shownPosition && b.isAmp == block.isAmp && b.slot == block.slot)
            return;

        shownPosition = position;
        block = b;
        refresh (true);
    }

    int getShownPosition() const noexcept { return shownPosition; }

    /** Call from the UI timer: follows model changes (presets, automation, moves) and tempo sync. */
    void tick()
    {
        const auto b = processor.getBlock (shownPosition);
        const bool blockChanged = b.isAmp != block.isAmp || b.slot != block.slot;
        block = b;
        refresh (blockChanged);
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (2.0f, 1.0f).withTrimmedBottom (4.0f);
        constexpr float corner = 14.0f;

        const auto& info = fx::modelInfo (slotModel());
        const bool empty = isEmptySlot();
        const auto body = block.isAmp ? colours::amp : colours::forCategory ((int) info.category);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (area.translated (0.0f, 4.0f), corner);
        g.setGradientFill (juce::ColourGradient (body.brighter (0.22f), area.getX(), area.getY(),
                                                 body.darker (0.45f), area.getX(), area.getBottom(), false));
        g.fillRoundedRectangle (area, corner);
        g.setColour (juce::Colours::white.withAlpha (0.2f));
        g.drawRoundedRectangle (area.reduced (1.5f), corner - 1.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (area, corner, 1.5f);

        // darker plate behind the knobs
        if (! knobPlate.isEmpty())
        {
            g.setColour (juce::Colours::black.withAlpha (0.16f));
            g.fillRoundedRectangle (knobPlate, 10.0f);
        }

        const juce::String title = block.isAmp ? juce::String ("AMP / CAB")
                                               : (empty ? juce::String ("EMPTY SLOT") : juce::String (info.name).toUpperCase());
        g.setFont (boldFont (21.0f));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawFittedText (title, titleArea.translated (0, 1), juce::Justification::centredLeft, 1, 0.7f);
        g.setColour (juce::Colours::white);
        g.drawFittedText (title, titleArea, juce::Justification::centredLeft, 1, 0.7f);

        g.setFont (boldFont (11.0f));
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.drawText (block.isAmp ? juce::String ("AMP BLOCK") : "SLOT " + juce::String (block.slot + 1), slotLabelArea,
                    juce::Justification::centredRight);

        if (controls.empty())
        {
            g.setFont (plainFont (14.0f));
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawText (block.isAmp ? "Pick an amp and a cabinet on the left" : "Pick an effect type and model on the left",
                        knobPlate.toNearestInt(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        area.removeFromBottom (4);

        // left column: title, pickers, "based on", move / clear
        auto left = area.removeFromLeft (212);
        auto titleRow = left.removeFromTop (30);
        slotLabelArea = titleRow.removeFromRight (60);
        titleArea = titleRow;
        left.removeFromTop (6);
        firstBox.setBounds (left.removeFromTop (28));
        left.removeFromTop (6);
        secondBox.setBounds (left.removeFromTop (28));
        left.removeFromTop (8);
        auto buttons = left.removeFromBottom (28);
        leftButton.setBounds (buttons.removeFromLeft (56));
        buttons.removeFromLeft (6);
        rightButton.setBounds (buttons.removeFromLeft (56));
        buttons.removeFromLeft (6);
        clearButton.setBounds (buttons);
        left.removeFromBottom (6);
        basedOn.setBounds (left);

        area.removeFromLeft (14);

        // right: footswitch
        auto switchArea = area.removeFromRight (84);
        footswitch.setBounds (switchArea.withSizeKeepingCentre (64, 88));
        area.removeFromRight (8);

        knobPlate = area.toFloat();

        // controls: one row of up to nine, or two rows (the amp block has up to eighteen)
        const int count = (int) controls.size();
        if (count == 0)
            return;

        auto grid = area.reduced (4, 6);
        const int rows = count > 9 ? 2 : 1;
        const int perRow = (count + rows - 1) / rows;
        const int rowHeight = std::min (rows == 2 ? 110 : 132, grid.getHeight() / rows);

        // menus need more room than knobs: they count as one and a half cells
        auto units = [this] (int i) { return controls[(size_t) i].spec.unit == fx::Unit::choice ? 1.5f : 1.0f; };
        float widest = 0.0f;
        for (int row = 0; row < rows; ++row)
        {
            float sum = 0.0f;
            for (int i = row * perRow; i < std::min (count, (row + 1) * perRow); ++i)
                sum += units (i);
            widest = std::max (widest, sum);
        }

        const float unit = std::min (88.0f, (float) grid.getWidth() / widest);
        const int top = grid.getCentreY() - rowHeight * rows / 2;

        for (int row = 0; row < rows; ++row)
        {
            const int first = row * perRow, last = std::min (count, (row + 1) * perRow);
            float sum = 0.0f;
            for (int i = first; i < last; ++i)
                sum += units (i);

            float x = (float) grid.getCentreX() - 0.5f * sum * unit;
            for (int i = first; i < last; ++i)
            {
                const float width = units (i) * unit;
                auto cell = juce::Rectangle<float> (x, (float) (top + row * rowHeight), width, (float) rowHeight).toNearestInt();
                x += width;

                auto& c = cells[(size_t) i];
                c.label.setBounds (cell.removeFromTop (18));
                c.slider.setBounds (cell.reduced (2, 0));
                c.choice.setBounds (cell.withSizeKeepingCentre (cell.getWidth() - 6, 28).translated (0, -10));
            }
        }
    }

private:
    static constexpr size_t maxControls = (size_t) (fx::maxAmpKnobs + fx::maxCabKnobs);

    /** One knob or menu: the parameter it edits and what that parameter means right now. */
    struct Control
    {
        juce::String paramId;
        fx::KnobSpec spec;
        int slotKnob = -1; // FX slots: the knob's index (for tempo sync)
        bool cabinet = false;
    };

    struct Cell
    {
        juce::Slider slider;
        juce::Label label;
        juce::ComboBox choice;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    int slotModel() const     { return block.isAmp ? 0 : processor.getSlotModel (block.slot); }
    bool isEmptySlot() const  { return ! block.isAmp && slotModel() == 0; }

    /** Everything that decides which controls are shown. */
    juce::String signature() const
    {
        return block.isAmp ? "amp:" + juce::String (processor.getAmpModel()) + ":" + juce::String (processor.getCabModel())
                           : "slot:" + juce::String (block.slot) + ":" + juce::String (slotModel());
    }

    float controlValue (const Control& c) const
    {
        return c.spec.fromNorm (processor.parameter (c.paramId).getValue());
    }

    /** The time a tempo-synced knob of this slot is at right now, or a negative value when it isn't synced. */
    float syncedTime (int knob) const
    {
        const auto& info = fx::modelInfo (slotModel());
        const int note = knob == info.timeKnob ? info.noteKnob : (knob == info.timeKnob2 ? info.noteKnob2 : -1);
        if (block.isAmp || note < 0)
            return -1.0f;

        fx::SlotParams slot;
        slot.model = slotModel();
        for (size_t k = 0; k < info.knobs.size(); ++k)
            slot.knobs[k] = info.knobs[k].fromNorm (processor.parameter (ParamIDs::slotKnob (block.slot, (int) k)).getValue());

        if (juce::roundToInt (slot.knobs[(size_t) note]) <= 0)
            return -1.0f;

        fx::resolveTempo (slot, processor.getTempo());
        return slot.knobs[(size_t) knob];
    }

    /** Rebuilds the controls when the block or its model changed; otherwise just syncs menus and sync state. */
    void refresh (bool force)
    {
        const auto now = signature();
        if (force || now != shownSignature)
        {
            shownSignature = now;
            rebuild();
        }

        juce::String sync;
        for (size_t i = 0; i < controls.size(); ++i)
        {
            const auto& control = controls[i];
            auto& cell = cells[i];

            if (control.spec.unit == fx::Unit::choice)
            {
                const int index = juce::roundToInt (controlValue (control));
                if (cell.choice.getSelectedItemIndex() != index)
                    cell.choice.setSelectedItemIndex (index, juce::dontSendNotification);
            }
            else if (control.slotKnob >= 0)
            {
                // a time that follows the tempo can't be turned by hand; it shows the time it's at
                const float time = syncedTime (control.slotKnob);
                cell.slider.setEnabled (time < 0.0f);
                cell.slider.setAlpha (time < 0.0f ? 1.0f : 0.6f);
                sync << juce::String (time, 1) << ";";
            }
        }

        if (sync != syncSignature)
        {
            syncSignature = sync;
            for (size_t i = 0; i < controls.size(); ++i)
                if (controls[i].spec.unit != fx::Unit::choice)
                    cells[i].slider.updateText();
        }
    }

    void rebuild()
    {
        const int model = slotModel();
        const auto& info = fx::modelInfo (model);
        const bool empty = isEmptySlot();
        const int ampModel = processor.getAmpModel(), cabModel = processor.getCabModel();

        // pickers
        firstBox.clear (juce::dontSendNotification);
        secondBox.clear (juce::dontSendNotification);

        if (block.isAmp)
        {
            firstBox.addItem ("No Amp", 1);
            for (size_t a = 0; a < fx::amps().size(); ++a)
                firstBox.addItem (fx::amps()[a].name, (int) a + 2);
            firstBox.setSelectedId (ampModel + 2, juce::dontSendNotification);
            firstBox.setTooltip ("Amp model");

            secondBox.addItem ("No Cab", 1);
            for (size_t c = 0; c < fx::cabs().size(); ++c)
                secondBox.addItem (fx::cabs()[c].name, (int) c + 2);
            secondBox.setSelectedId (cabModel + 2, juce::dontSendNotification);
            secondBox.setTooltip ("Speaker cabinet (its microphone is one of the knobs)");
        }
        else
        {
            for (const auto c : fx::categoryOrder) // the HD500X's order
                firstBox.addItem (fx::categoryName (c), (int) c + 1);
            firstBox.setSelectedId ((int) info.category + 1, juce::dontSendNotification);
            firstBox.setTooltip ("Effect type");

            for (int m = 1; m < fx::numModels(); ++m)
                if (fx::modelInfo (m).category == info.category)
                    secondBox.addItem (fx::modelInfo (m).name, m);
            secondBox.setSelectedId (model, juce::dontSendNotification);
            secondBox.setTooltip ("Model (named after the POD HD500X model list)");
        }

        secondBox.setVisible (block.isAmp || ! empty);
        clearButton.setVisible (! block.isAmp && ! empty);

        juce::String about;
        if (block.isAmp)
        {
            if (ampModel >= 0) about << "Amp: " << fx::amps()[(size_t) ampModel].basedOn << "\n";
            if (cabModel >= 0) about << "Cab: " << fx::cabs()[(size_t) cabModel].basedOn;
            if (about.isEmpty()) about = "No amp and no cab: the signal passes straight through.";
        }
        else
        {
            about = empty ? juce::String ("Empty: the signal passes straight through.") : "Based on: " + juce::String (info.basedOn);
        }
        basedOn.setText (about, juce::dontSendNotification);

        leftButton.setEnabled (shownPosition > 0);
        rightButton.setEnabled (shownPosition < PedalboardProcessor::numBlocks - 1);

        // footswitch
        footAttachment.reset();
        footswitch.setVisible (! empty);
        if (! empty)
            footAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                processor.apvts, block.isAmp ? juce::String (ParamIDs::cab) : ParamIDs::slotOn (block.slot), footswitch);

        // what there is to control
        controls.clear();
        if (block.isAmp)
        {
            if (ampModel >= 0)
                for (size_t k = 0; k < fx::amps()[(size_t) ampModel].knobs.size(); ++k)
                    controls.push_back ({ ParamIDs::ampKnob ((int) k), fx::amps()[(size_t) ampModel].knobs[k], -1, false });
            if (cabModel >= 0)
                for (size_t k = 0; k < fx::cabs()[(size_t) cabModel].knobs.size(); ++k)
                    controls.push_back ({ ParamIDs::cabKnob ((int) k), fx::cabs()[(size_t) cabModel].knobs[k], -1, true });
        }
        else
        {
            for (size_t k = 0; k < info.knobs.size(); ++k)
                controls.push_back ({ ParamIDs::slotKnob (block.slot, (int) k), info.knobs[k], (int) k, false });
        }
        jassert (controls.size() <= cells.size());

        for (size_t i = 0; i < cells.size(); ++i)
        {
            auto& c = cells[i];
            c.attachment.reset();
            const bool used = i < controls.size();
            const bool isChoice = used && controls[i].spec.unit == fx::Unit::choice;
            c.slider.setVisible (used && ! isChoice);
            c.choice.setVisible (isChoice);
            c.label.setVisible (used);
            if (! used)
                continue;

            const auto& control = controls[i];
            c.label.setText (juce::String (control.spec.name).toUpperCase(), juce::dontSendNotification);
            c.label.setColour (juce::Label::textColourId, control.cabinet ? colours::amber.brighter (0.4f) : juce::Colours::white);

            if (isChoice)
            {
                c.choice.clear (juce::dontSendNotification);
                for (int n = 0; n <= (int) control.spec.max; ++n)
                    c.choice.addItem (control.spec.choices[n], n + 1);
                c.choice.setSelectedItemIndex (juce::roundToInt (controlValue (control)), juce::dontSendNotification);

                const auto& m = fx::modelInfo (model);
                const bool isNote = ! block.isAmp && (control.slotKnob == m.noteKnob || control.slotKnob == m.noteKnob2);
                c.choice.setTooltip (isNote ? "ms = set the time with the knob; a note value follows the TAP tempo" : juce::String());
                continue;
            }

            c.slider.setEnabled (true);
            c.slider.setAlpha (1.0f);
            c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, control.paramId, c.slider);
            c.slider.setDoubleClickReturnValue (true, control.spec.toNorm (control.spec.def));
            c.slider.textFromValueFunction = [this, i] (double travel)
            {
                if (i >= controls.size())
                    return juce::String();

                const auto& shown = controls[i];
                const float time = shown.slotKnob >= 0 ? syncedTime (shown.slotKnob) : -1.0f;
                return fx::knobText (shown.spec, time >= 0.0f ? time : shown.spec.fromNorm ((float) travel));
            };
            c.slider.updateText();
        }

        syncSignature.clear();
        resized();
        repaint();
    }

    void firstPicked()
    {
        if (block.isAmp)
        {
            processor.setAmpModel (firstBox.getSelectedId() - 2);
        }
        else
        {
            const auto category = (fx::Category) (firstBox.getSelectedId() - 1);
            int model = 0;
            if (category != fx::Category::none)
                for (int m = 1; m < fx::numModels(); ++m)
                    if (fx::modelInfo (m).category == category) { model = m; break; }

            if (fx::modelInfo (slotModel()).category != category)
                processor.setSlotModel (block.slot, model);
        }
        refresh (true);
    }

    void secondPicked()
    {
        if (block.isAmp)
            processor.setCabModel (secondBox.getSelectedId() - 2);
        else if (const int model = secondBox.getSelectedId(); model > 0 && model != slotModel())
            processor.setSlotModel (block.slot, model);
        refresh (true);
    }

    void choicePicked (size_t index)
    {
        if (index >= controls.size())
            return;

        const auto& control = controls[index];
        auto& param = processor.parameter (control.paramId);
        param.beginChangeGesture();
        param.setValueNotifyingHost (control.spec.toNorm ((float) cells[index].choice.getSelectedItemIndex()));
        param.endChangeGesture();
    }

    void move (int direction)
    {
        const int to = juce::jlimit (0, PedalboardProcessor::numBlocks - 1, shownPosition + direction);
        if (to == shownPosition)
            return;

        processor.moveBlock (shownPosition, to);
        shownPosition = to;
        block = processor.getBlock (to);
        refresh (true);
        if (onMoved)
            onMoved (to);
    }

    PedalboardProcessor& processor;
    PedalboardProcessor::Block block;
    int shownPosition = -1;
    juce::String shownSignature, syncSignature;

    juce::ComboBox firstBox, secondBox; // slots: effect type, model; the amp block: amp, cabinet
    juce::Label basedOn;
    juce::TextButton leftButton { "< MOVE" }, rightButton { "MOVE >" }, clearButton { "CLEAR" };
    FootSwitch footswitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> footAttachment;
    std::vector<Control> controls;
    std::array<Cell, maxControls> cells;

    juce::Rectangle<int> titleArea, slotLabelArea;
    juce::Rectangle<float> knobPlate;
};

} // namespace ui
