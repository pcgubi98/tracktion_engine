#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

class DAWBackend : public juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
                  public juce::ChangeListener
{
public:
    DAWBackend();
    ~DAWBackend() override;

    // Edit operations
    void createNewEdit();
    bool loadEdit(const juce::File& file);
    bool saveEdit(const juce::File& file);

    // Transport controls
    void play();
    void stop();
    void setPosition(double timeInSeconds);
    double getPosition();

    // Track operations
    juce::ReferenceCountedObjectPtr<te::AudioTrack> addTrack();
    void removeTrack(int index);
    bool addClipToTrack(int trackIndex, const juce::File& file, double startTime);

    // OSC message handling
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

private:
    te::Engine engine;
    te::DeviceManager& deviceManager;
    juce::OSCReceiver oscReceiver;
    juce::OSCSender oscSender;
    std::unique_ptr<te::Edit> currentEdit;

    // Helper method to send OSC responses
    void sendOSCResponse(const juce::String& address, const juce::OSCArgument& arg);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DAWBackend)
}; 