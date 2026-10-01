// Renders the plugin's editor to a PNG without opening a window or an audio device, to check layouts.
//
// Usage: UiSnapshot <out.png> [select=<chain position 0..8>] [amp=<amp key>] [cab=<cab key>] [slot<1..8>=<model key>]
//                              [<parameter id>=<value 0..1>] ...
//   e.g. UiSnapshot ui.png select=2 amp=blackface_double_nrm slot6=ping_pong

#include "PluginEditor.h"
#include "PluginProcessor.h"

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc < 2)
    {
        std::cout << "usage: UiSnapshot <out.png> [select=N] [amp=key] [cab=key] [slotN=key] ...\n";
        return 2;
    }

    PedalboardProcessor processor;
    int selected = 1;

    for (int i = 2; i < argc; ++i)
    {
        const auto arg = juce::String (argv[i]);
        const auto name = arg.upToFirstOccurrenceOf ("=", false, false), value = arg.fromFirstOccurrenceOf ("=", false, false);

        if (name == "select")
            selected = value.getIntValue();
        else if (name == "amp")
            processor.setAmpModel (fx::findIn (fx::amps(), value.toStdString()));
        else if (name == "cab")
            processor.setCabModel (fx::findIn (fx::cabs(), value.toStdString()));
        else if (name.startsWith ("slot"))
            processor.setSlotModel (name.substring (4).getIntValue() - 1, fx::findModel (value.toStdString()));
        else if (auto* param = processor.apvts.getParameter (name)) // any parameter by id, as 0..1
            param->setValueNotifyingHost (value.getFloatValue());
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    if (auto* pedalboard = dynamic_cast<PedalboardEditor*> (editor.get()))
    {
        pedalboard->selectBlock (selected);
        pedalboard->refresh();
    }

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    file.deleteFile();

    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::cout << (ok ? "wrote " : "could not write ") << file.getFullPathName() << "\n";
    return ok ? 0 : 1;
}
