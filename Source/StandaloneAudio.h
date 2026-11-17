#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

// The standalone app owns its audio device (inside a DAW the host does). This reaches that device, to show
// its latency on the pedalboard and to open the audio settings from there.
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace standalone
{
/** What the app's audio device is doing. `available` is false inside a DAW, or while no device is open. */
struct AudioStatus
{
    bool available = false;
    juce::String driver;                      // "ASIO", "Windows Audio", "Windows Audio (Exclusive Mode)", ...
    juce::String device;
    int bufferSize = 0;                       // samples per callback
    double sampleRate = 0.0;
    int inputLatency = 0, outputLatency = 0;  // samples, as the driver reports them (buffers included)

    double bufferMs() const noexcept    { return sampleRate > 0.0 ? 1000.0 * bufferSize / sampleRate : 0.0; }
    double inputMs() const noexcept     { return sampleRate > 0.0 ? 1000.0 * inputLatency / sampleRate : 0.0; }
    double outputMs() const noexcept    { return sampleRate > 0.0 ? 1000.0 * outputLatency / sampleRate : 0.0; }
    /** Guitar in to sound out, without the converters' own fraction of a millisecond. */
    double roundTripMs() const noexcept { return inputMs() + outputMs(); }

    /** A short name for the driver type, for the header. */
    juce::String shortDriver() const
    {
        if (driver.containsIgnoreCase ("exclusive"))   return "WASAPI exclusive";
        if (driver.containsIgnoreCase ("low latency")) return "WASAPI low-latency";
        if (driver == "Windows Audio")                 return "WASAPI shared";
        return driver;
    }

    /** True for the driver types that can't get below Windows' 10 ms mixing period. */
    bool isSharedMode() const { return driver == "Windows Audio" || driver == "DirectSound"; }
};

/** For tests that render the editor without an audio device: when set, getAudioStatus() returns its result. */
inline std::function<AudioStatus()> statusForTests;

inline AudioStatus getAudioStatus()
{
    if (statusForTests)
        return statusForTests();

   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        if (auto* device = holder->deviceManager.getCurrentAudioDevice())
        {
            AudioStatus s;
            s.available = true;
            s.driver = device->getTypeName();
            s.device = device->getName();
            s.bufferSize = device->getCurrentBufferSizeSamples();
            s.sampleRate = device->getCurrentSampleRate();
            s.inputLatency = device->getInputLatencyInSamples();
            s.outputLatency = device->getOutputLatencyInSamples();
            return s;
        }
    }
   #endif

    return {};
}

/** True when running as the standalone app (whether or not a device is open). */
inline bool isStandaloneApp()
{
   #if JucePlugin_Build_Standalone
    return juce::StandalonePluginHolder::getInstance() != nullptr;
   #else
    return false;
   #endif
}

/** Opens the app's Audio/MIDI Settings window (as Options > Audio/MIDI Settings does). */
inline void showAudioSettings()
{
   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        holder->showAudioSettingsDialog();
   #endif
}

} // namespace standalone
