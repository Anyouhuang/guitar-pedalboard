// Renders the plugin's editor to a PNG without opening a window or an audio device, to check layouts.
//
// Usage: UiSnapshot <out.png> [select=<chain position 0..8>] [amp=<amp key>] [cab=<cab key>] [slot<1..8>=<model key>]
//                              [<parameter id>=<value 0..1>] ...
//                              [audio=<driver type>,<buffer>,<sample rate>,<input latency>,<output latency>]
//   e.g. UiSnapshot ui.png select=2 amp=blackface_double_nrm slot6=ping_pong
//        UiSnapshot ui.png "audio=Windows Audio,480,48000,960,1440"     (the standalone app's latency readout)
//        UiSnapshot ui.png source=demo                                  (input source: live / demo)
//        UiSnapshot ui.png preset=3 "presetname=3:Solo" s1On=0          (recall preset 3, rename it, then change something)
//        UiSnapshot ui.png savepreset=7                                 (store the board in preset 7)
//        UiSnapshot ui.png "click=SAVE AS"                              (press a button, by its name, before the picture)
//
//        UiSnapshot check       runs the UI interaction checks in UiChecks.h instead (exit code 0 = all passed)

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "UiChecks.h"

// wmain: the arguments arrive as Unicode (main's are in the system code page), so preset names can be in any script
#if JUCE_WINDOWS
int wmain (int argc, wchar_t* argv[])
#else
int main (int argc, char* argv[])
#endif
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc < 2)
    {
        std::cout << "usage: UiSnapshot <out.png> [select=N] [amp=key] [cab=key] [slotN=key] ...\n";
        return 2;
    }

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String (argv[i]));

    if (args[0] == "check")
        return uichecks::run();

    PedalboardProcessor processor;
    int selected = 1;
    juce::StringArray clicks;

    for (int i = 1; i < args.size(); ++i)
    {
        const auto arg = args[i];
        const auto name = arg.upToFirstOccurrenceOf ("=", false, false), value = arg.fromFirstOccurrenceOf ("=", false, false);

        if (name == "select")
            selected = value.getIntValue();
        else if (name == "audio") // pretend to be the standalone app with this audio device
        {
            const auto parts = juce::StringArray::fromTokens (value, ",", "");
            standalone::AudioStatus status;
            status.available = true;
            status.driver = parts[0];
            status.device = "Test device";
            status.bufferSize = parts[1].getIntValue();
            status.sampleRate = parts[2].getDoubleValue();
            status.inputLatency = parts[3].getIntValue();
            status.outputLatency = parts[4].getIntValue();
            standalone::statusForTests = [status] { return status; };
        }
        else if (name == "source")
            processor.getTestSignal().setSource (value == "demo" ? TestSignalPlayer::demo : TestSignalPlayer::live);
        else if (name == "click")
            clicks.add (value);
        else if (name == "preset")
            processor.loadPreset (value.getIntValue() - 1);
        else if (name == "savepreset")
            processor.savePreset (value.getIntValue() - 1);
        else if (name == "presetname")
            processor.renamePreset (value.upToFirstOccurrenceOf (":", false, false).getIntValue() - 1,
                                    value.fromFirstOccurrenceOf (":", false, false));
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

        for (const auto& name : clicks)
        {
            if (auto* button = uichecks::findChild<juce::Button> (*editor, [&name] (juce::Button& b) { return b.getName() == name; }))
                button->onClick();
            else
                std::cout << "no button called " << name << "\n";
            pedalboard->refresh();
        }
    }

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args[0]);
    file.deleteFile();

    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::cout << (ok ? "wrote " : "could not write ") << file.getFullPathName() << "\n";
    return ok ? 0 : 1;
}
