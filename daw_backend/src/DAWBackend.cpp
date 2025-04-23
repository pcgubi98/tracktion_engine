#include "DAWBackend.h"
#include <tracktion_engine/tracktion_engine.h>
#include <fstream>
#include <chrono>
#include <iomanip>

class DAWLogger {
public:
    static DAWLogger& getInstance() {
        static DAWLogger instance;
        return instance;
    }

    void log(const std::string& message) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        
        std::string logMessage = ss.str() + " - " + message + "\n";
        
        // Log to console
        std::cout << logMessage;
        
        // Log to file
        std::ofstream logFile("daw_backend.log", std::ios::app);
        if (logFile.is_open()) {
            logFile << logMessage;
            logFile.close();
        }
    }

private:
    DAWLogger() {} // Private constructor for singleton
};

DAWBackend::DAWBackend()
    : engine("DAWBackend"),
      deviceManager(engine.getDeviceManager()),
      oscReceiver(),
      oscSender()
{
    DAWLogger::getInstance().log("Initializing DAWBackend");
    
    // Initialize audio device
    deviceManager.initialise();
    DAWLogger::getInstance().log("Audio device initialized");
    
    // Initialize OSC receiver
    if (!oscReceiver.connect(9000)) {
        DAWLogger::getInstance().log("Error: Could not connect to port 9000");
    } else {
        DAWLogger::getInstance().log("OSC receiver connected to port 9000");
    }
    
    // Initialize OSC sender
    if (!oscSender.connect("127.0.0.1", 9001)) {
        DAWLogger::getInstance().log("Error: Could not connect OSC sender to port 9001");
    } else {
        DAWLogger::getInstance().log("OSC sender connected to port 9001");
    }
    
    // Add OSC message handler
    try {
        oscReceiver.addListener(this);
        DAWLogger::getInstance().log("OSC listener added for all messages");
        
        // Add format error handler to log any malformed messages
        oscReceiver.registerFormatErrorHandler([](const char* data, int dataSize) {
            std::stringstream ss;
            ss << "OSC Format Error - Raw data: ";
            for (int i = 0; i < dataSize; ++i) {
                ss << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)data[i] << " ";
            }
            DAWLogger::getInstance().log(ss.str());
        });
    } catch (const juce::OSCFormatError& e) {
        DAWLogger::getInstance().log("OSC Format Error in addListener: " + std::string(e.what()));
    } catch (const std::exception& e) {
        DAWLogger::getInstance().log("Error adding OSC listener: " + std::string(e.what()));
    } catch (...) {
        DAWLogger::getInstance().log("Unknown error adding OSC listener");
    }

    // Start position update timer (50ms interval)
    startTimer(50);
}

DAWBackend::~DAWBackend()
{
    DAWLogger::getInstance().log("Shutting down DAWBackend");
    stopTimer();
    if (currentEdit != nullptr) {
        te::EditFileOperations(*currentEdit).save(true, true, false);
        DAWLogger::getInstance().log("Edit saved");
    }
    engine.getTemporaryFileManager().getTempDirectory().deleteRecursively();
    DAWLogger::getInstance().log("Temporary files cleaned up");
}

void DAWBackend::createNewEdit()
{
    DAWLogger::getInstance().log("Creating new edit");
    // Create a new edit
    auto edit = te::createEmptyEdit(engine);
    currentEdit = te::loadEditFromState(engine, edit);
    
    // Initialize the transport
    auto& transport = currentEdit->getTransport();
    transport.setLoopRange({ tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(60.0) });
    transport.looping = true;
    transport.addChangeListener(this);
    DAWLogger::getInstance().log("Transport initialized with 60-second loop range");

    // Add the drum loop to the first track
    auto tracks = getAudioTracks(*currentEdit);
    if (tracks.size() == 0) {
        currentEdit->ensureNumberOfAudioTracks(1);
        tracks = getAudioTracks(*currentEdit);
        DAWLogger::getInstance().log("Created new audio track");
    }
    
    if (tracks.size() > 0) {
        auto track = tracks[0];
        auto drumLoopFile = juce::File("/Users/pradeepchakravarti/Git/tracktion_engine/examples/DemoRunner/resources/drum_loop.wav");
        if (drumLoopFile.existsAsFile()) {
            auto startPos = tracktion::TimePosition();
            auto endPos = tracktion::TimePosition::fromSeconds(60.0);
            auto position = te::ClipPosition({ startPos, endPos });
            track->insertWaveClip(drumLoopFile.getFileNameWithoutExtension(), drumLoopFile, position, false);
            DAWLogger::getInstance().log("Added drum loop to track");
        } else {
            DAWLogger::getInstance().log("Error: Drum loop file not found at " + drumLoopFile.getFullPathName().toStdString());
        }
    }
}

bool DAWBackend::loadEdit(const juce::File& file)
{
    if (!file.existsAsFile()) {
        return false;
    }

    // Load the edit from file
    currentEdit = te::loadEditFromFile(engine, file);
    if (currentEdit == nullptr) {
        return false;
    }

    // Set up transport
    auto& transport = currentEdit->getTransport();
    transport.setLoopRange({ tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(60.0) });
    transport.looping = true;
    transport.addChangeListener(this);

    return true;
}

bool DAWBackend::saveEdit(const juce::File& file)
{
    if (currentEdit == nullptr) {
        return false;
    }

    return te::EditFileOperations(*currentEdit).saveAs(file);
}

void DAWBackend::play()
{
    if (currentEdit != nullptr) {
        DAWLogger::getInstance().log("Starting playback");
        currentEdit->getTransport().play(false);
    }
}

void DAWBackend::stop()
{
    if (currentEdit != nullptr) {
        DAWLogger::getInstance().log("Stopping playback");
        currentEdit->getTransport().stop(false, false);
    }
}

void DAWBackend::setPosition(double timeInSeconds)
{
    if (currentEdit != nullptr) {
        DAWLogger::getInstance().log("Setting position to " + std::to_string(timeInSeconds) + " seconds");
        currentEdit->getTransport().setPosition(tracktion::TimePosition::fromSeconds(timeInSeconds));
    }
}

double DAWBackend::getPosition()
{
    if (currentEdit != nullptr) {
        return currentEdit->getTransport().getPosition().inSeconds();
    }
    return 0.0;
}

juce::ReferenceCountedObjectPtr<te::AudioTrack> DAWBackend::addTrack()
{
    if (currentEdit == nullptr) {
        return nullptr;
    }

    // Add a new audio track at the end
    auto& trackList = currentEdit->getTrackList();
    auto lastTrack = trackList.objects.size() > 0 ? trackList.objects[trackList.objects.size() - 1] : nullptr;
    auto insertPoint = te::TrackInsertPoint(nullptr, lastTrack);
    return currentEdit->insertNewAudioTrack(insertPoint, nullptr);
}

void DAWBackend::removeTrack(int index)
{
    if (currentEdit == nullptr) {
        return;
    }

    auto tracks = getAudioTracks(*currentEdit);
    if (index >= 0 && index < tracks.size()) {
        currentEdit->deleteTrack(tracks[index]);
    }
}

bool DAWBackend::addClipToTrack(int trackIndex, const juce::File& file, double startTime)
{
    if (currentEdit == nullptr) {
        return false;
    }

    auto tracks = getAudioTracks(*currentEdit);
    if (trackIndex < 0 || trackIndex >= tracks.size()) {
        return false;
    }

    auto track = tracks[trackIndex];
    auto startPos = tracktion::TimePosition::fromSeconds(startTime);
    auto endPos = tracktion::TimePosition::fromSeconds(startTime + 60.0);
    auto position = te::ClipPosition({ startPos, endPos });
    auto clip = track->insertWaveClip(file.getFileNameWithoutExtension(), file, position, false);
    return clip != nullptr;
}

void DAWBackend::sendOSCResponse(const juce::String& address, const juce::OSCArgument& arg)
{
    oscSender.send(address, arg);
    DAWLogger::getInstance().log("Sent OSC response: " + address.toStdString());
}

void DAWBackend::oscMessageReceived(const juce::OSCMessage& message)
{
    try {
        // Log the raw message details
        std::stringstream ss;
        ss << "Received OSC message - Pattern: '" << message.getAddressPattern().toString() << "'";
        ss << " Arguments: [";
        for (int i = 0; i < message.size(); ++i) {
            if (i > 0) ss << ", ";
            if (message[i].isFloat32()) ss << message[i].getFloat32();
            else if (message[i].isInt32()) ss << message[i].getInt32();
            else if (message[i].isString()) ss << "'" << message[i].getString() << "'";
            else ss << "unknown type";
        }
        ss << "]";
        DAWLogger::getInstance().log(ss.str());
        
        // Handle different OSC addresses
        if (message.getAddressPattern() == "/daw/init") {
            DAWLogger::getInstance().log("Initializing new edit");
            createNewEdit();
            sendOSCResponse("/daw/init/response", juce::OSCArgument(1)); // Send success response
        }
        else if (message.getAddressPattern() == "/daw/play") {
            DAWLogger::getInstance().log("Play command received");
            play();
            sendOSCResponse("/daw/play/response", juce::OSCArgument(1)); // Send success response
        }
        else if (message.getAddressPattern() == "/daw/pause") {
            DAWLogger::getInstance().log("Pause command received");
            stop(); // Using stop for pause since we don't have a separate pause function
            sendOSCResponse("/daw/pause/response", juce::OSCArgument(1)); // Send success response
        }
        else if (message.getAddressPattern() == "/daw/stop") {
            DAWLogger::getInstance().log("Stop command received");
            stop();
            sendOSCResponse("/daw/stop/response", juce::OSCArgument(1)); // Send success response
        }
        else if (message.getAddressPattern() == "/daw/position") {
            if (message.size() == 1 && message[0].isFloat32()) {
                DAWLogger::getInstance().log("Position command received: " + std::to_string(message[0].getFloat32()));
                setPosition(message[0].getFloat32());
                sendOSCResponse("/daw/position/response", juce::OSCArgument(1)); // Send success response
            } else {
                DAWLogger::getInstance().log("Error: Invalid position message format");
                sendOSCResponse("/daw/position/response", juce::OSCArgument(0)); // Send error response
            }
        } else {
            DAWLogger::getInstance().log("Warning: Unknown OSC message pattern: " + message.getAddressPattern().toString().toStdString());
        }
    } catch (const juce::OSCFormatError& e) {
        DAWLogger::getInstance().log("OSC Format Error: " + std::string(e.what()));
    } catch (const std::exception& e) {
        DAWLogger::getInstance().log("Error processing OSC message: " + std::string(e.what()));
    } catch (...) {
        DAWLogger::getInstance().log("Unknown error processing OSC message");
    }
}

void DAWBackend::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (currentEdit != nullptr && source == &currentEdit->getTransport()) {
        // Handle transport state changes
        DAWLogger::getInstance().log("Transport state changed");
    }
}

void DAWBackend::sendPositionUpdate()
{
    if (currentEdit != nullptr) {
        auto timePosition = currentEdit->getTransport().getPosition();
        auto beatPosition = currentEdit->tempoSequence.toBeats(timePosition);
        
        // Only send if position has changed
        if (timePosition.inSeconds() != lastSentTimePosition || 
            beatPosition.inBeats() != lastSentBeatPosition) {
            
            // Send both time and beat positions
            oscSender.send("/daw/position/update", 
                juce::OSCArgument(static_cast<float>(timePosition.inSeconds())),  // Time in seconds
                juce::OSCArgument(static_cast<float>(beatPosition.inBeats()))     // Position in beats
            );
            
            // Update last sent positions
            lastSentTimePosition = timePosition.inSeconds();
            lastSentBeatPosition = beatPosition.inBeats();
            
            DAWLogger::getInstance().log("Position update - Time: " + 
                std::to_string(timePosition.inSeconds()) + 
                "s, Beats: " + 
                std::to_string(beatPosition.inBeats()));
        }
    }
}

void DAWBackend::timerCallback()
{
    sendPositionUpdate();
} 