#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

class DAWBackend : public juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
                  public juce::ChangeListener,`
                  public juce::Timer
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
    bool addClipToTrackById(int trackId, const juce::File& file, double startTime);

    // OSC message handling
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

private:
    te::Engine engine;
    te::DeviceManager& deviceManager;
    juce::OSCReceiver oscReceiver;
    juce::OSCSender oscSender;
    std::unique_ptr<te::Edit> currentEdit;

    // Helper method to send OSC responses
    void sendOSCResponse(const juce::String& address, const juce::OSCArgument& arg);
    void sendPositionUpdate();

    // Store last sent position and BPM
    double lastSentTimePosition = -1.0;
    double lastSentBeatPosition = -1.0;
    double lastSentBPM = -1.0;

    // Track ID management
    int nextTrackId = 1;
    std::map<juce::ReferenceCountedObjectPtr<te::AudioTrack>, int> trackIdMap;
    std::map<int, int> frontendIdMap; // Maps backend track IDs to frontend IDs
    int getTrackId(juce::ReferenceCountedObjectPtr<te::AudioTrack> track);
    juce::ReferenceCountedObjectPtr<te::AudioTrack> getTrackById(int id);
    int getFrontendId(int backendId) { return frontendIdMap.count(backendId) ? frontendIdMap[backendId] : -1; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DAWBackend)
}; 