#pragma once

#include "PluginProcessor.h"
#include "TapControl.h"
#include "Widgets.h"

namespace ui
{
/** The signal chain, HD500X style: eight FX blocks and the amp/cab block in a row, input to output.
    Click a block to edit it, drag it to move it, double-click to switch it on or off. */
class ChainStrip : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    std::function<void (int position)> onSelect;

    explicit ChainStrip (PedalboardProcessor& p) : processor (p), tempo (p.apvts, ParamIDs::tempo)
    {
        addAndMakeVisible (tempo);
        setTooltip ("Click a block to edit it, drag it to move it in the chain, double-click to switch it on / off");
    }

    int getSelected() const noexcept { return selected; }

    void setSelected (int position)
    {
        selected = juce::jlimit (0, PedalboardProcessor::numBlocks - 1, position);
        repaint();
    }

    /** Call from the UI timer: follows presets / automation, blinks the TAP LED. */
    void tick()
    {
        tempo.tick();

        juce::String signature;
        for (int i = 0; i < PedalboardProcessor::numBlocks; ++i)
        {
            const auto b = processor.getBlock (i);
            signature << (b.isAmp ? "A" + juce::String ((int) isCabOn()) + "/" + juce::String (processor.getAmpModel()) + "/" + juce::String (processor.getCabModel())
                                  : juce::String (processor.getSlotModel (b.slot)) + ":" + juce::String ((int) processor.isSlotOn (b.slot))) << ",";
        }
        if (signature != lastSignature)
        {
            lastSignature = signature;
            repaint();
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 10);
        tempo.setBounds (area.removeFromRight (104).withSizeKeepingCentre (104, 66));
        area.removeFromRight (14);

        inJack = area.removeFromLeft (26).toFloat();
        outJack = area.removeFromRight (30).toFloat();
        tileArea = area.reduced (4, 0);

        constexpr int gap = 8;
        const int n = PedalboardProcessor::numBlocks;
        const int width = (tileArea.getWidth() - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i)
            tiles[(size_t) i] = juce::Rectangle<int> (tileArea.getX() + i * (width + gap), tileArea.getY(), width, tileArea.getHeight()).toFloat();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff18181c));
        g.fillRoundedRectangle (bounds, 12.0f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawRoundedRectangle (bounds.reduced (1.0f), 11.0f, 1.0f);

        // the cable running through the chain
        const float y = tiles[0].getCentreY();
        g.setColour (juce::Colour (0xff4a4a52));
        g.fillRect (juce::Rectangle<float> (inJack.getCentreX(), y - 1.5f, outJack.getCentreX() - inJack.getCentreX(), 3.0f));
        drawJack (g, inJack, "IN");
        drawJack (g, outJack, "OUT");

        for (int i = 0; i < PedalboardProcessor::numBlocks; ++i)
            if (! (dragging && i == dragFrom))
                drawTile (g, i, tiles[(size_t) i], 1.0f);

        if (dragging)
        {
            // where it will land, and the block following the mouse
            const auto& target = tiles[(size_t) dropTarget];
            const float x = dropTarget > dragFrom ? target.getRight() + 4.0f : target.getX() - 4.0f;
            g.setColour (colours::amber);
            g.fillRoundedRectangle (juce::Rectangle<float> (x - 1.5f, target.getY() - 4.0f, 3.0f, target.getHeight() + 8.0f), 1.5f);

            auto floating = tiles[(size_t) dragFrom].withCentre ({ (float) dragX, tiles[0].getCentreY() - 4.0f });
            drawTile (g, dragFrom, floating, 0.85f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragFrom = tileAt (e.position);
        dragging = false;
        if (dragFrom >= 0)
        {
            setSelected (dragFrom);
            if (onSelect)
                onSelect (dragFrom);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragFrom < 0)
            return;

        if (! dragging && e.getDistanceFromDragStart() > 6)
            dragging = true;

        if (dragging)
        {
            dragX = e.x;
            dropTarget = nearestTile ((float) e.x);
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging && dropTarget != dragFrom)
        {
            processor.moveBlock (dragFrom, dropTarget);
            setSelected (dropTarget);
            if (onSelect)
                onSelect (dropTarget);
        }
        dragging = false;
        dragFrom = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int i = tileAt (e.position);
        if (i < 0)
            return;

        const auto b = processor.getBlock (i);
        auto& param = processor.parameter (b.isAmp ? juce::String (ParamIDs::cab) : ParamIDs::slotOn (b.slot));
        if (! b.isAmp && processor.getSlotModel (b.slot) == 0)
            return; // nothing to switch on

        param.beginChangeGesture();
        param.setValueNotifyingHost (param.getValue() > 0.5f ? 0.0f : 1.0f);
        param.endChangeGesture();
    }

private:
    bool isCabOn() const { return processor.apvts.getRawParameterValue (ParamIDs::cab)->load() > 0.5f; }

    /** The amp block's tile: the amp's name (or the cab's when there is no amp), and the cab's on a small second line. */
    std::pair<juce::String, juce::String> ampTileText() const
    {
        const int amp = processor.getAmpModel(), cab = processor.getCabModel();
        if (amp < 0)
            return { cab < 0 ? juce::String ("No Amp") : juce::String (fx::cabs()[(size_t) cab].name), {} };
        return { fx::amps()[(size_t) amp].name, cab < 0 ? juce::String ("No Cab") : juce::String (fx::cabs()[(size_t) cab].name) };
    }

    int tileAt (juce::Point<float> p) const
    {
        for (int i = 0; i < PedalboardProcessor::numBlocks; ++i)
            if (tiles[(size_t) i].expanded (4.0f).contains (p))
                return i;
        return -1;
    }

    int nearestTile (float x) const
    {
        int best = 0;
        for (int i = 1; i < PedalboardProcessor::numBlocks; ++i)
            if (std::abs (tiles[(size_t) i].getCentreX() - x) < std::abs (tiles[(size_t) best].getCentreX() - x))
                best = i;
        return best;
    }

    static void drawJack (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text)
    {
        const auto socket = juce::Rectangle<float> (14.0f, 14.0f).withCentre (area.getCentre());
        g.setColour (juce::Colour (0xff0c0c0e));
        g.fillEllipse (socket);
        g.setColour (juce::Colour (0xffa0a0a8));
        g.drawEllipse (socket, 2.0f);
        g.setFont (boldFont (10.0f));
        g.setColour (colours::dimText);
        g.drawText (text, area.withTop (socket.getBottom() + 2.0f).withHeight (14.0f), juce::Justification::centred);
    }

    void drawTile (juce::Graphics& g, int position, juce::Rectangle<float> r, float alpha)
    {
        const auto b = processor.getBlock (position);
        const int model = b.isAmp ? 0 : processor.getSlotModel (b.slot);
        const auto& info = fx::modelInfo (model);
        const bool empty = ! b.isAmp && model == 0;
        const bool on = b.isAmp ? isCabOn() : (! empty && processor.isSlotOn (b.slot));
        const auto colour = b.isAmp ? colours::amp : colours::forCategory ((int) info.category);
        const bool isSelected = position == selected;

        if (empty)
        {
            g.setColour (juce::Colour (0xff1e1e22));
            g.fillRoundedRectangle (r, 8.0f);
            juce::Path outline;
            outline.addRoundedRectangle (r.reduced (1.0f), 7.0f);
            juce::Path dashed;
            const float dashes[] = { 4.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (juce::Colours::white.withAlpha (0.25f * alpha));
            g.fillPath (dashed);
        }
        else
        {
            const auto body = on ? colour : colour.withMultipliedSaturation (0.35f).withMultipliedBrightness (0.55f);
            g.setGradientFill (juce::ColourGradient (body.brighter (0.25f).withMultipliedAlpha (alpha), r.getX(), r.getY(),
                                                     body.darker (0.5f).withMultipliedAlpha (alpha), r.getX(), r.getBottom(), false));
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (juce::Colours::black.withAlpha (0.5f * alpha));
            g.drawRoundedRectangle (r, 8.0f, 1.2f);
        }

        if (isSelected)
        {
            g.setColour (juce::Colours::white.withAlpha (alpha));
            g.drawRoundedRectangle (r.expanded (2.5f), 10.0f, 2.0f);
        }

        auto inner = r.reduced (6.0f, 5.0f);
        const juce::String category = b.isAmp ? "AMP" : (empty ? juce::String ("EMPTY")
                                                                : juce::String (fx::categoryName (info.category)).toUpperCase());
        g.setFont (boldFont (9.5f));
        g.setColour (juce::Colours::white.withAlpha ((empty ? 0.35f : 0.75f) * alpha));
        g.drawText (category, inner.removeFromTop (12.0f), juce::Justification::centredLeft);

        const auto footer = inner.removeFromBottom (12.0f);
        auto [name, cabName] = b.isAmp ? ampTileText() : std::pair<juce::String, juce::String> { empty ? "+" : info.name, {} };

        if (cabName.isNotEmpty())
        {
            g.setFont (plainFont (10.0f));
            g.setColour (juce::Colours::white.withAlpha (0.75f * alpha));
            g.drawFittedText (cabName, inner.removeFromBottom (11.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }

        g.setFont (boldFont (cabName.isNotEmpty() ? 11.5f : 12.5f));
        g.setColour (juce::Colours::white.withAlpha ((empty ? 0.3f : 1.0f) * alpha));
        g.drawFittedText (name, inner.toNearestInt(), juce::Justification::centred, 2, 0.8f);

        if (! empty)
            drawLed (g, juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ footer.getX() + 4.0f, footer.getCentreY() }), colours::ledRed, on);

        g.setFont (plainFont (10.0f));
        g.setColour (juce::Colours::white.withAlpha (0.45f * alpha));
        g.drawText (b.isAmp ? juce::String() : juce::String (b.slot + 1), footer, juce::Justification::centredRight);
    }

    PedalboardProcessor& processor;
    GlobalTempo tempo;

    std::array<juce::Rectangle<float>, PedalboardProcessor::numBlocks> tiles;
    juce::Rectangle<float> inJack, outJack;
    juce::Rectangle<int> tileArea;

    int selected = 1, dragFrom = -1, dropTarget = 0, dragX = 0;
    bool dragging = false;
    juce::String lastSignature;
};

} // namespace ui
