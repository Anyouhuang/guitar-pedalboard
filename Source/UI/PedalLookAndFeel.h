#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
namespace colours
{
    inline const juce::Colour board    { 0xff131316 };
    inline const juce::Colour panel    { 0xff212126 };
    inline const juce::Colour lcd      { 0xff0a1310 };
    inline const juce::Colour lcdText  { 0xff7dffb0 };
    inline const juce::Colour text     { 0xffeeeeee };
    inline const juce::Colour dimText  { 0xff8e8e96 };
    inline const juce::Colour ledRed   { 0xffff3b30 };
    inline const juce::Colour ledGreen { 0xff35e07a };
    inline const juce::Colour amber    { 0xffffb020 };

    inline const juce::Colour gate     { 0xff5d6b75 };
    inline const juce::Colour drive    { 0xffe0702a };
    inline const juce::Colour mod      { 0xff8157d0 };
    inline const juce::Colour delay    { 0xff2f9a66 };
    inline const juce::Colour reverb   { 0xff2f7fc0 };
    inline const juce::Colour amp      { 0xffb8403a };
    inline const juce::Colour empty    { 0xff34343a };

    inline const juce::Colour filter   { 0xffc9a227 };
    inline const juce::Colour pitch    { 0xffc2478f };
    inline const juce::Colour eq       { 0xff3d9aa0 };
    inline const juce::Colour wah      { 0xff8a9a3a };
    inline const juce::Colour volume   { 0xff7a7f8a };

    /** Block colour by model category (fx::Category order). */
    inline juce::Colour forCategory (int category)
    {
        const juce::Colour byCategory[] = { empty, gate, drive, mod, delay, reverb, filter, pitch, eq, wah, volume, amp, amp };
        return byCategory[juce::jlimit (0, 12, category)];
    }
}

inline juce::Font boldFont (float height)  { return juce::Font (juce::FontOptions (height, juce::Font::bold)); }
inline juce::Font plainFont (float height) { return juce::Font (juce::FontOptions (height)); }

/** Rotary knob with a value read-out underneath. The text-box colours are set on the slider itself
    because its read-out label is created before the component joins the editor's look-and-feel. */
inline void styleKnob (juce::Slider& knob, juce::Colour arcColour)
{
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setColour (juce::Slider::rotarySliderFillColourId, arcColour);
    knob.setColour (juce::Slider::textBoxTextColourId, colours::text.withAlpha (0.85f));
    knob.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    knob.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::black.withAlpha (0.18f));
    knob.setColour (juce::Slider::textBoxHighlightColourId, juce::Colours::white.withAlpha (0.25f));
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 16);
    knob.setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

//==============================================================================
class PedalLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PedalLookAndFeel()
    {
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xcc111114));
        setColour (juce::ComboBox::outlineColourId, juce::Colours::black.withAlpha (0.5f));
        setColour (juce::ComboBox::textColourId, colours::text);
        setColour (juce::ComboBox::arrowColourId, colours::text.withAlpha (0.7f));
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1b1b1f));
        setColour (juce::PopupMenu::textColourId, colours::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff3a3a42));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::Label::textColourId, colours::text);
        setColour (juce::Slider::textBoxTextColourId, colours::text.withAlpha (0.85f));
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, juce::Colours::white.withAlpha (0.25f));
        setColour (juce::TextEditor::textColourId, colours::text);
        setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111114));
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff1b1b1f));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto area   = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
        const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
        const auto centre = area.getCentre();
        const auto angle  = startAngle + pos * (endAngle - startAngle);
        const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);

        // value arc
        const float arcRadius = radius - 2.0f;
        const juce::PathStrokeType arcStroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (track, arcStroke);

        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
        g.setColour (accent);
        g.strokePath (value, arcStroke);

        // knob body with a drop shadow, knurled skirt and a domed cap
        const float knobRadius = arcRadius - 6.0f;
        const auto knob = juce::Rectangle<float> (knobRadius * 2.0f, knobRadius * 2.0f).withCentre (centre);

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (knob.translated (0.0f, 2.5f).expanded (1.0f));

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3c3c42), centre.x, knob.getY(),
                                                 juce::Colour (0xff0c0c0e), centre.x, knob.getBottom(), false));
        g.fillEllipse (knob);

        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (int i = 0; i < 24; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / 24.0f;
            g.drawLine (juce::Line<float> (centre.getPointOnCircumference (knobRadius * 0.78f, a),
                                           centre.getPointOnCircumference (knobRadius * 0.98f, a)), 1.0f);
        }

        const auto cap = knob.reduced (knobRadius * 0.24f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4a52), cap.getX(), cap.getY(),
                                                 juce::Colour (0xff1a1a1e), cap.getRight(), cap.getBottom(), false));
        g.fillEllipse (cap);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawEllipse (cap, 1.0f);

        // pointer
        g.setColour (juce::Colours::white);
        g.drawLine (juce::Line<float> (centre.getPointOnCircumference (knobRadius * 0.30f, angle),
                                       centre.getPointOnCircumference (knobRadius * 0.92f, angle)), 2.5f);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto area = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (area, 5.0f, 1.0f);

        const auto arrowZone = juce::Rectangle<float> ((float) width - 20.0f, 0.0f, 14.0f, (float) height);
        juce::Path arrow;
        arrow.addTriangle (arrowZone.getCentreX() - 4.0f, arrowZone.getCentreY() - 2.0f,
                           arrowZone.getCentreX() + 4.0f, arrowZone.getCentreY() - 2.0f,
                           arrowZone.getCentreX(),        arrowZone.getCentreY() + 3.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.fillPath (arrow);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (4, 1, box.getWidth() - 24, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
        label.setJustificationType (juce::Justification::centred);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return boldFont (13.0f); }
    juce::Font getPopupMenuFont() override               { return plainFont (15.0f); }

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox (slider);
        label->setFont (plainFont (12.0f));
        label->setJustificationType (juce::Justification::centred);
        return label;
    }
};

} // namespace ui
