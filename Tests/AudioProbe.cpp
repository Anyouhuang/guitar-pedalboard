// Lists what each Windows audio driver type can do on this PC: devices, buffer sizes, and the latency
// the driver reports once a stream is open. Used to choose the standalone app's low-latency defaults.
//
// Usage: AudioProbe            list only
//        AudioProbe open       also open the default devices of every shared type for a moment (silent)
//        AudioProbe open all   ... and the exclusive / ASIO types too (interrupts other apps' sound briefly)

#include <juce_audio_devices/juce_audio_devices.h>

namespace
{
struct Silence : juce::AudioIODeviceCallback
{
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* output, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (output[ch] != nullptr)
                juce::FloatVectorOperations::clear (output[ch], numSamples);
        ++callbacks;
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    std::atomic<int> callbacks { 0 };
};

juce::String list (const juce::Array<int>& values)
{
    juce::StringArray s;
    for (int v : values)
        s.add (juce::String (v));
    return s.joinIntoString (" ");
}

juce::String ms (double samples, double rate) { return juce::String (1000.0 * samples / rate, 1) + " ms"; }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    const bool open = argc > 1 && juce::String (argv[1]) == "open";
    const bool openAll = open && argc > 2 && juce::String (argv[2]) == "all";

    juce::AudioDeviceManager manager;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    manager.createAudioDeviceTypes (types);

    for (auto* type : types)
    {
        type->scanForDevices();
        const auto outputs = type->getDeviceNames (false), inputs = type->getDeviceNames (true);
        std::cout << "\n== " << type->getTypeName() << " ==\n";
        std::cout << "  outputs: " << outputs.joinIntoString (" | ") << "\n";
        std::cout << "  inputs:  " << inputs.joinIntoString (" | ") << "\n";

        if (outputs.isEmpty())
            continue;

        const auto outName = outputs[juce::jmax (0, type->getDefaultDeviceIndex (false))];
        const auto inName = inputs.isEmpty() ? juce::String() : inputs[juce::jmax (0, type->getDefaultDeviceIndex (true))];
        std::unique_ptr<juce::AudioIODevice> device (type->createDevice (outName, type->hasSeparateInputsAndOutputs() ? inName : juce::String()));
        if (device == nullptr)
        {
            std::cout << "  could not create the default device\n";
            continue;
        }

        const auto sizes = device->getAvailableBufferSizes();
        std::cout << "  default devices: out \"" << outName << "\", in \"" << inName << "\"\n";
        std::cout << "  buffer sizes: " << list (sizes) << "   (default " << device->getDefaultBufferSize() << ")\n";

        const bool exclusive = type->getTypeName().containsIgnoreCase ("exclusive") || type->getTypeName().containsIgnoreCase ("ASIO");
        if (! open || (exclusive && ! openAll) || type->getTypeName().containsIgnoreCase ("DirectSound") || sizes.isEmpty())
            continue;

        for (const int size : { sizes.getFirst(), device->getDefaultBufferSize() })
        {
            juce::BigInteger ins, outs;
            ins.setRange (0, 1, true);
            outs.setRange (0, 2, true);
            const auto rates = device->getAvailableSampleRates();
            const double rate = rates.contains (48000.0) ? 48000.0 : (rates.isEmpty() ? 44100.0 : rates[0]);
            const auto error = device->open (inName.isEmpty() ? juce::BigInteger() : ins, outs, rate, size);
            if (error.isNotEmpty())
            {
                std::cout << "  open with buffer " << size << ": " << error << "\n";
                continue;
            }

            Silence silence;
            device->start (&silence);
            juce::Thread::sleep (700);
            const double fs = device->getCurrentSampleRate();
            const int buffer = device->getCurrentBufferSizeSamples(), in = device->getInputLatencyInSamples(), out = device->getOutputLatencyInSamples();
            const int callbacks = silence.callbacks.load();
            device->stop();
            device->close();

            std::cout << "  asked for " << size << " -> buffer " << buffer << " (" << ms (buffer, fs) << ") @ " << fs << " Hz, "
                      << callbacks << " callbacks in 0.7 s"
                      << ", reported latency in " << ms (in, fs) << " + out " << ms (out, fs) << " = " << ms (in + out, fs) << "\n";
        }
    }

    return 0;
}
