#include "DAWBackend.h"
#include <tracktion_engine/tracktion_engine.h>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <cmath>

// Small epsilon value for floating-point comparisons
constexpr double EPSILON = 0.000001;

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

int DAWBackend::getTrackId(juce::ReferenceCountedObjectPtr<te::AudioTrack> track)
{
    if (trackIdMap.find(track) == trackIdMap.end()) {
        // Assign a new ID
        trackIdMap[track] = nextTrackId++;
    }
    return trackIdMap[track];
}

juce::ReferenceCountedObjectPtr<te::AudioTrack> DAWBackend::getTrackById(int id)
{
    for (auto& pair : trackIdMap) {
        if (pair.second == id) {
            return pair.first;
        }
    }
    return nullptr;
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
    auto track = currentEdit->insertNewAudioTrack(insertPoint, nullptr);
    
    if (track != nullptr) {
        // Assign and store track ID
        int trackId = getTrackId(track);
        DAWLogger::getInstance().log("Created track with ID: " + std::to_string(trackId));
    }
    
    return track;
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

bool DAWBackend::addClipToTrackById(int trackId, const juce::File& file, double startTime)
{
    if (currentEdit == nullptr) {
        return false;
    }

    // Find the track by ID
    auto track = getTrackById(trackId);
    if (track == nullptr) {
        DAWLogger::getInstance().log("Error: Track with ID " + std::to_string(trackId) + " not found");
        return false;
    }

    auto startPos = tracktion::TimePosition::fromSeconds(startTime);
    auto endPos = tracktion::TimePosition::fromSeconds(startTime + 60.0);
    auto position = te::ClipPosition({ startPos, endPos });
    auto clip = track->insertWaveClip(file.getFileNameWithoutExtension(), file, position, false);
    
    if (clip != nullptr) {
        DAWLogger::getInstance().log("Added clip to track with ID " + std::to_string(trackId) + 
                                   ", file: " + file.getFullPathName().toStdString() + 
                                   ", start time: " + std::to_string(startTime));
        return true;
    } else {
        DAWLogger::getInstance().log("Error: Failed to add clip to track with ID " + std::to_string(trackId));
        return false;
    }
}

bool DAWBackend::addMidiClipToTrackById(int trackId, const juce::String& name, double startTime, double endTime, int& outClipId)
{
    if (currentEdit == nullptr) {
        return false;
    }

    // Find the track by ID
    auto track = getTrackById(trackId);
    if (track == nullptr) {
        DAWLogger::getInstance().log("Error: Track with ID " + std::to_string(trackId) + " not found");
        return false;
    }

    auto startPos = tracktion::TimePosition::fromSeconds(startTime);
    auto endPos = tracktion::TimePosition::fromSeconds(endTime);
    auto timeRange = tracktion::TimeRange(startPos, endPos);
    
    auto midiClip = track->insertMIDIClip(name, timeRange, nullptr);
    
    if (midiClip != nullptr) {
        // Generate a unique clip ID and store it in the map
        static int nextMidiClipId = 1;
        int clipId = nextMidiClipId++;
        midiClipIdMap[clipId] = midiClip;
        outClipId = clipId;
        
        DAWLogger::getInstance().log("Added MIDI clip to track with ID " + std::to_string(trackId) + 
                                   ", clip ID: " + std::to_string(clipId) +
                                   ", name: " + name.toStdString() + 
                                   ", start time: " + std::to_string(startTime) +
                                   ", end time: " + std::to_string(endTime));
        return true;
    } else {
        DAWLogger::getInstance().log("Error: Failed to add MIDI clip to track with ID " + std::to_string(trackId));
        return false;
    }
}

juce::ReferenceCountedObjectPtr<te::MidiClip> DAWBackend::getMidiClipById(int clipId)
{
    auto it = midiClipIdMap.find(clipId);
    if (it != midiClipIdMap.end()) {
        return it->second;
    }
    return nullptr;
}

bool DAWBackend::addNoteToMidiClip(int clipId, int noteNumber, float velocity, double startTimeBeats, double lengthInBeats)
{
    auto midiClip = getMidiClipById(clipId);
    if (midiClip == nullptr) {
        DAWLogger::getInstance().log("Error: MIDI clip with ID " + std::to_string(clipId) + " not found");
        return false;
    }
    
    try {
        // Get the sequence from the clip
        auto& sequence = midiClip->getSequence();
        
        // Create a MIDI note
        auto& undoManager = midiClip->edit.getUndoManager();
        auto startBeat = tracktion::BeatPosition::fromBeats(startTimeBeats);
        auto endBeat = tracktion::BeatPosition::fromBeats(startTimeBeats + lengthInBeats);
        
        // Add the note to the sequence
        auto note = sequence.addNote(noteNumber, startBeat, endBeat, 
                                     sequence.getMidiChannel(), 
                                     velocity, 
                                     &undoManager);
        
        if (note != nullptr) {
            DAWLogger::getInstance().log("Added note to MIDI clip " + std::to_string(clipId) + 
                                       ", note: " + std::to_string(noteNumber) + 
                                       ", velocity: " + std::to_string(velocity) + 
                                       ", start: " + std::to_string(startTimeBeats) + 
                                       ", length: " + std::to_string(lengthInBeats));
            return true;
        } else {
            DAWLogger::getInstance().log("Error: Failed to add note to MIDI clip " + std::to_string(clipId));
            return false;
        }
    } catch (const std::exception& e) {
        DAWLogger::getInstance().log("Exception adding note to MIDI clip: " + std::string(e.what()));
        return false;
    }
}

bool DAWBackend::addNotesToMidiClip(int clipId, const std::vector<int>& noteNumbers, const std::vector<float>& velocities, 
                                  const std::vector<double>& startTimesBeats, const std::vector<double>& lengthsInBeats)
{
    auto midiClip = getMidiClipById(clipId);
    if (midiClip == nullptr) {
        DAWLogger::getInstance().log("Error: MIDI clip with ID " + std::to_string(clipId) + " not found");
        return false;
    }
    
    // Check that all arrays have the same size
    size_t numNotes = noteNumbers.size();
    if (velocities.size() != numNotes || startTimesBeats.size() != numNotes || lengthsInBeats.size() != numNotes) {
        DAWLogger::getInstance().log("Error: Arrays for bulk note addition must have the same size");
        return false;
    }
    
    if (numNotes == 0) {
        DAWLogger::getInstance().log("Warning: No notes to add");
        return true;
    }
    
    try {
        // Get the sequence from the clip
        auto& sequence = midiClip->getSequence();
        auto& undoManager = midiClip->edit.getUndoManager();
        
        int successCount = 0;
        
        // Start a single undo transaction for all notes
        undoManager.beginNewTransaction("Add multiple MIDI notes");
        
        // Add all notes
        for (size_t i = 0; i < numNotes; ++i) {
            auto startBeat = tracktion::BeatPosition::fromBeats(startTimesBeats[i]);
            auto endBeat = tracktion::BeatPosition::fromBeats(startTimesBeats[i] + lengthsInBeats[i]);
            
            auto note = sequence.addNote(noteNumbers[i], startBeat, endBeat, 
                                         sequence.getMidiChannel(), 
                                         velocities[i], 
                                         &undoManager);
            
            if (note != nullptr) {
                successCount++;
            }
        }
        
        DAWLogger::getInstance().log("Added " + std::to_string(successCount) + " of " + 
                                   std::to_string(numNotes) + " notes to MIDI clip " + std::to_string(clipId));
        
        return successCount > 0;
    } catch (const std::exception& e) {
        DAWLogger::getInstance().log("Exception adding bulk notes to MIDI clip: " + std::string(e.what()));
        return false;
    }
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
        else if (message.getAddressPattern() == "/daw/position/set") {
            if (message.size() == 1 && message[0].isFloat32()) {
                float positionInBeats = message[0].getFloat32();
                DAWLogger::getInstance().log("Position command received: " + std::to_string(positionInBeats) + " beats");
                
                if (currentEdit != nullptr) {
                    // Convert from beats to time position
                    auto timePosition = currentEdit->tempoSequence.toTime(tracktion::BeatPosition::fromBeats(positionInBeats));
                    
                    // Set the position using the converted time
                    currentEdit->getTransport().setPosition(timePosition);
                    DAWLogger::getInstance().log("Set position to: " + std::to_string(timePosition.inSeconds()) + " seconds (converted from " + std::to_string(positionInBeats) + " beats)");
                    sendOSCResponse("/daw/position/set/response", juce::OSCArgument(1)); // Send success response
                } else {
                    DAWLogger::getInstance().log("Error: Cannot set position - no edit loaded");
                    sendOSCResponse("/daw/position/set/response", juce::OSCArgument(0)); // Send error response
                }
            } else {
                DAWLogger::getInstance().log("Error: Invalid position message format");
                sendOSCResponse("/daw/position/set/response", juce::OSCArgument(0)); // Send error response
            }
        }
        else if (message.getAddressPattern() == "/daw/bpm/set") {
            if (message.size() == 1 && message[0].isFloat32()) {
                float newBPM = message[0].getFloat32();
                DAWLogger::getInstance().log("BPM set command received: " + std::to_string(newBPM));
                
                if (currentEdit != nullptr) {
                    // Log current transport state
                    auto& transport = currentEdit->getTransport();
                    DAWLogger::getInstance().log("Current transport state - Playing: " + 
                        std::to_string(transport.isPlaying()) + 
                        ", Position: " + std::to_string(transport.getPosition().inSeconds()) + 
                        "s, Loop range: " + std::to_string(transport.getLoopRange().getStart().inSeconds()) + 
                        "s to " + std::to_string(transport.getLoopRange().getEnd().inSeconds()) + "s");

                    // Remove all existing tempo changes
                    int numTempos = currentEdit->tempoSequence.getNumTempos();
                    DAWLogger::getInstance().log("Removing " + std::to_string(numTempos) + " existing tempo changes");
                    
                    // Safety check - if there are too many tempos, something might be wrong
                    if (numTempos > 100) {
                        DAWLogger::getInstance().log("Warning: Suspicious number of tempo changes (" + std::to_string(numTempos) + "), skipping removal");
                    } else {
                        int removedCount = 0;
                        while (currentEdit->tempoSequence.getNumTempos() > 0 && removedCount < numTempos) {
                            currentEdit->tempoSequence.removeTempo(0, false);
                            removedCount++;
                            DAWLogger::getInstance().log("Removed tempo " + std::to_string(removedCount) + " of " + std::to_string(numTempos));
                        }
                        if (removedCount < numTempos) {
                            DAWLogger::getInstance().log("Warning: Could not remove all tempo changes");
                        }
                    }
                    
                    // Insert a single tempo change at the start
                    if (auto tempo = currentEdit->tempoSequence.insertTempo(tracktion::TimePosition())) {
                        tempo->setBpm(newBPM);
                        // Update the tempo sequence to apply the changes
                        currentEdit->tempoSequence.updateTempoData();
                        DAWLogger::getInstance().log("BPM set to " + std::to_string(newBPM));
                        
                        // Log new transport state
                        DAWLogger::getInstance().log("New transport state - Playing: " + 
                            std::to_string(transport.isPlaying()) + 
                            ", Position: " + std::to_string(transport.getPosition().inSeconds()) + 
                            "s, Loop range: " + std::to_string(transport.getLoopRange().getStart().inSeconds()) + 
                            "s to " + std::to_string(transport.getLoopRange().getEnd().inSeconds()) + "s");
                        
                        // If transport was playing, restart it
                        if (transport.isPlaying()) {
                            DAWLogger::getInstance().log("Restarting transport after BPM change");
                            transport.stop(false, false);
                            transport.play(false);
                        }
                        
                        sendOSCResponse("/daw/bpm/set/response", juce::OSCArgument(1)); // Send success response
                    } else {
                        DAWLogger::getInstance().log("Error: Failed to set tempo");
                        sendOSCResponse("/daw/bpm/set/response", juce::OSCArgument(0)); // Send error response
                    }
                } else {
                    DAWLogger::getInstance().log("Error: No edit loaded");
                    sendOSCResponse("/daw/bpm/set/response", juce::OSCArgument(0)); // Send error response
                }
            } else {
                DAWLogger::getInstance().log("Error: Invalid BPM message format");
                sendOSCResponse("/daw/bpm/set/response", juce::OSCArgument(0)); // Send error response
            }
        }
        else if (message.getAddressPattern() == "/daw/track/add") {
            DAWLogger::getInstance().log("Add track command received");
            
            // Check if a frontend ID was provided
            juce::String frontendId;
            if (message.size() >= 1) {
                if (message[0].isString()) {
                    frontendId = message[0].getString();
                    DAWLogger::getInstance().log("Frontend ID provided as string: " + frontendId.toStdString());
                } else if (message[0].isInt32()) {
                    frontendId = juce::String(message[0].getInt32());
                    DAWLogger::getInstance().log("Frontend ID provided as integer: " + frontendId.toStdString());
                }
            }
            
            if (currentEdit != nullptr) {
                auto track = addTrack();
                if (track != nullptr) {
                    // Get the track ID for reference (not the index)
                    int trackId = getTrackId(track);
                    
                    // Store the frontend ID if provided
                    if (frontendId.isNotEmpty()) {
                        frontendIdMap[trackId] = frontendId;
                        DAWLogger::getInstance().log("Associated frontend ID " + frontendId.toStdString() + 
                                                    " with backend ID " + std::to_string(trackId));
                    }
                    
                    DAWLogger::getInstance().log("Track added successfully, Backend ID: " + std::to_string(trackId) + 
                                                ", Frontend ID: " + frontendId.toStdString());
                    
                    // Create a response with both IDs
                    juce::OSCMessage response("/daw/track/add/response");
                    response.addInt32(1); // Success
                    response.addInt32(trackId); // Backend track ID
                    response.addString(frontendId); // Frontend ID (as string)
                    
                    oscSender.send(response);
                    DAWLogger::getInstance().log("Sent track add response with backend and frontend IDs");
                } else {
                    DAWLogger::getInstance().log("Error: Failed to add track");
                    sendOSCResponse("/daw/track/add/response", juce::OSCArgument(0)); // Send error response
                }
            } else {
                DAWLogger::getInstance().log("Error: No edit loaded");
                sendOSCResponse("/daw/track/add/response", juce::OSCArgument(0)); // Send error response
            }
        }
        else if (message.getAddressPattern() == "/daw/track/remove") {
            int trackId = -1;
            juce::String frontendId;
            bool validRequest = false;
            
            // Handle case 1: Backend track ID as integer
            if (message.size() >= 1 && message[0].isInt32()) {
                trackId = message[0].getInt32();
                validRequest = true;
                
                // Check if a frontend ID was also provided
                if (message.size() >= 2 && message[1].isString()) {
                    frontendId = message[1].getString();
                }
                
                DAWLogger::getInstance().log("Remove track command received, Backend ID: " + std::to_string(trackId) + 
                                          ", Frontend ID: " + frontendId.toStdString());
            }
            // Handle case 2: Frontend ID as string
            else if (message.size() >= 1 && message[0].isString()) {
                frontendId = message[0].getString();
                DAWLogger::getInstance().log("Remove track command received with Frontend ID: " + frontendId.toStdString());
                
                // Look up the backend track ID corresponding to this frontend ID
                for (const auto& pair : frontendIdMap) {
                    if (pair.second == frontendId) {
                        trackId = pair.first;
                        validRequest = true;
                        DAWLogger::getInstance().log("Found matching Backend ID: " + std::to_string(trackId));
                        break;
                    }
                }
                
                if (!validRequest) {
                    DAWLogger::getInstance().log("Error: No backend track ID found for frontend ID: " + frontendId.toStdString());
                }
            }
            
            if (validRequest) {
                if (currentEdit != nullptr) {
                    auto track = getTrackById(trackId);
                    if (track != nullptr) {
                        // Remove the track
                        currentEdit->deleteTrack(track.get());
                        // Remove from our ID maps
                        trackIdMap.erase(track);
                        frontendIdMap.erase(trackId);
                        DAWLogger::getInstance().log("Track removed successfully, Backend ID: " + std::to_string(trackId) + 
                                                   ", Frontend ID: " + frontendId.toStdString());
                        
                        // Create a response with both IDs
                        juce::OSCMessage response("/daw/track/remove/response");
                        response.addInt32(1); // Success
                        response.addInt32(trackId); // Backend track ID
                        response.addString(frontendId); // Frontend ID (as string)
                        
                        oscSender.send(response);
                        DAWLogger::getInstance().log("Sent track remove response with backend and frontend IDs");
                    } else {
                        DAWLogger::getInstance().log("Error: Track with ID " + std::to_string(trackId) + " not found");
                        sendOSCResponse("/daw/track/remove/response", juce::OSCArgument(0)); // Error
                    }
                } else {
                    DAWLogger::getInstance().log("Error: No edit loaded");
                    sendOSCResponse("/daw/track/remove/response", juce::OSCArgument(0)); // Error
                }
            } else {
                DAWLogger::getInstance().log("Error: Invalid track remove message format");
                sendOSCResponse("/daw/track/remove/response", juce::OSCArgument(0)); // Error
            }
        }
        else if (message.getAddressPattern() == "/daw/track/list") {
            DAWLogger::getInstance().log("List tracks command received");
            
            if (currentEdit != nullptr) {
                auto tracks = getAudioTracks(*currentEdit);
                
                // First, create a response with the number of tracks
                juce::OSCMessage response("/daw/track/list/response");
                response.addInt32(1); // Success
                response.addInt32(static_cast<int>(tracks.size())); // Number of tracks
                
                DAWLogger::getInstance().log("Sending list of " + std::to_string(tracks.size()) + " tracks");
                
                // Then send the response
                oscSender.send(response);
                
                // Now send individual track info messages
                for (int i = 0; i < tracks.size(); ++i) {
                    auto track = tracks[i];
                    int trackId = getTrackId(track);
                    juce::String associatedFrontendId = getFrontendId(trackId);
                    
                    juce::OSCMessage trackInfo("/daw/track/info");
                    trackInfo.addString("backend_track_id: " + juce::String(trackId)); // Track ID
                    trackInfo.addString("track_index: " + juce::String(i)); // Track index (position)
                    trackInfo.addString("track_name: " + track->getName()); // Track name
                    trackInfo.addString("frontend_id: " + associatedFrontendId); // Frontend ID (empty string if not associated)
                    
                    oscSender.send(trackInfo);
                    
                    DAWLogger::getInstance().log("Track info - Backend ID: " + std::to_string(trackId) + 
                                               ", Index: " + std::to_string(i) + 
                                               ", Name: " + track->getName().toStdString() + 
                                               ", Frontend ID: " + associatedFrontendId.toStdString());
                }
            } else {
                DAWLogger::getInstance().log("Error: No edit loaded");
                sendOSCResponse("/daw/track/list/response", juce::OSCArgument(0)); // Error
            }
        }
        else if (message.getAddressPattern() == "/daw/clip/add") {
            if (message.size() >= 3 && message[0].isInt32() && message[1].isString() && message[2].isFloat32()) {
                int trackId = message[0].getInt32();
                juce::String filePath = message[1].getString();
                float startTime = message[2].getFloat32();
                
                DAWLogger::getInstance().log("Add clip command received - Track ID: " + std::to_string(trackId) + 
                                           ", File: " + filePath.toStdString() + 
                                           ", Start time: " + std::to_string(startTime));
                
                juce::File file(filePath);
                if (!file.existsAsFile()) {
                    DAWLogger::getInstance().log("Error: File not found: " + filePath.toStdString());
                    sendOSCResponse("/daw/clip/add/response", juce::OSCArgument(0)); // Error
                    return;
                }
                
                bool success = addClipToTrackById(trackId, file, startTime);
                if (success) {
                    DAWLogger::getInstance().log("Clip added successfully");
                    sendOSCResponse("/daw/clip/add/response", juce::OSCArgument(1)); // Success
                } else {
                    DAWLogger::getInstance().log("Error: Failed to add clip");
                    sendOSCResponse("/daw/clip/add/response", juce::OSCArgument(0)); // Error
                }
            } else {
                DAWLogger::getInstance().log("Error: Invalid add clip message format - Expected trackId, filePath, startTime");
                sendOSCResponse("/daw/clip/add/response", juce::OSCArgument(0)); // Error
            }
        }
        else if (message.getAddressPattern() == "/daw/midi/add") {
            if (message.size() >= 4 && message[0].isInt32() && message[1].isString() && 
                message[2].isFloat32() && message[3].isFloat32()) {
                int trackId = message[0].getInt32();
                juce::String clipName = message[1].getString();
                float startTime = message[2].getFloat32();
                float endTime = message[3].getFloat32();
                
                DAWLogger::getInstance().log("Add MIDI clip command received - Track ID: " + std::to_string(trackId) + 
                                           ", Name: " + clipName.toStdString() + 
                                           ", Start time: " + std::to_string(startTime) +
                                           ", End time: " + std::to_string(endTime));
                
                int clipId = -1;
                bool success = addMidiClipToTrackById(trackId, clipName, startTime, endTime, clipId);
                if (success) {
                    DAWLogger::getInstance().log("MIDI clip added successfully with ID: " + std::to_string(clipId));
                    
                    // Create a response with the clipId
                    juce::OSCMessage response("/daw/midi/add/response");
                    response.addInt32(1); // Success
                    response.addInt32(clipId); // Clip ID
                    oscSender.send(response);
                } else {
                    DAWLogger::getInstance().log("Error: Failed to add MIDI clip");
                    sendOSCResponse("/daw/midi/add/response", juce::OSCArgument(0)); // Error
                }
            } else {
                DAWLogger::getInstance().log("Error: Invalid add MIDI clip message format - Expected trackId, clipName, startTime, endTime");
                sendOSCResponse("/daw/midi/add/response", juce::OSCArgument(0)); // Error
            }
        }
        else if (message.getAddressPattern() == "/daw/midi/note/add") {
            if (message.size() >= 5 && message[0].isInt32() && message[1].isInt32() && 
                message[2].isFloat32() && message[3].isFloat32() && message[4].isFloat32()) {
                int clipId = message[0].getInt32();
                int noteNumber = message[1].getInt32();
                float velocity = message[2].getFloat32();
                float startTimeBeats = message[3].getFloat32();
                float lengthInBeats = message[4].getFloat32();
                
                DAWLogger::getInstance().log("Add MIDI note command received - Clip ID: " + std::to_string(clipId) + 
                                           ", Note: " + std::to_string(noteNumber) + 
                                           ", Velocity: " + std::to_string(velocity) + 
                                           ", Start beats: " + std::to_string(startTimeBeats) +
                                           ", Length beats: " + std::to_string(lengthInBeats));
                
                bool success = addNoteToMidiClip(clipId, noteNumber, velocity, startTimeBeats, lengthInBeats);
                if (success) {
                    DAWLogger::getInstance().log("MIDI note added successfully");
                    sendOSCResponse("/daw/midi/note/add/response", juce::OSCArgument(1)); // Success
                } else {
                    DAWLogger::getInstance().log("Error: Failed to add MIDI note");
                    sendOSCResponse("/daw/midi/note/add/response", juce::OSCArgument(0)); // Error
                }
            } else {
                DAWLogger::getInstance().log("Error: Invalid add MIDI note message format - Expected clipId, noteNumber, velocity, startTimeBeats, lengthInBeats");
                sendOSCResponse("/daw/midi/note/add/response", juce::OSCArgument(0)); // Error
            }
        }
        else if (message.getAddressPattern() == "/daw/midi/notes/add") {
            // Parse the message - first argument is clipId, then arrays of note data
            if (message.size() >= 2 && message[0].isInt32()) {
                int clipId = message[0].getInt32();
                
                // The rest of the message should be arrays of note data
                std::vector<int> noteNumbers;
                std::vector<float> velocities;
                std::vector<double> startTimesBeats;
                std::vector<double> lengthsInBeats;
                
                bool validFormat = false;
                
                // Parse the message format - depends on how the arrays are sent via OSC
                if (message.size() >= 5 && message[1].isArray() && message[2].isArray() && 
                    message[3].isArray() && message[4].isArray()) {
                    // Format 1: Arrays of data as separate arguments
                    // TODO: Implement parsing of OSC arrays based on your OSC library's capabilities
                    // This is a placeholder since the exact format will depend on how your OSC library handles arrays
                    DAWLogger::getInstance().log("OSC array format for notes not implemented");
                    validFormat = false;
                } else {
                    // Format 2: Interleaved values [note, vel, start, length, note, vel, start, length, ...]
                    if ((message.size() - 1) % 4 == 0) {
                        int numNotes = (message.size() - 1) / 4;
                        
                        for (int i = 0; i < numNotes; i++) {
                            int baseIdx = 1 + (i * 4);
                            
                            if (message[baseIdx].isInt32() && message[baseIdx+1].isFloat32() && 
                                message[baseIdx+2].isFloat32() && message[baseIdx+3].isFloat32()) {
                                
                                noteNumbers.push_back(message[baseIdx].getInt32());
                                velocities.push_back(message[baseIdx+1].getFloat32());
                                startTimesBeats.push_back(message[baseIdx+2].getFloat32());
                                lengthsInBeats.push_back(message[baseIdx+3].getFloat32());
                            } else {
                                validFormat = false;
                                break;
                            }
                        }
                        validFormat = true;
                    }
                }
                
                if (validFormat) {
                    DAWLogger::getInstance().log("Add bulk MIDI notes command received - Clip ID: " + std::to_string(clipId) + 
                                               ", Note count: " + std::to_string(noteNumbers.size()));
                    
                    bool success = addNotesToMidiClip(clipId, noteNumbers, velocities, startTimesBeats, lengthsInBeats);
                    if (success) {
                        DAWLogger::getInstance().log("Bulk MIDI notes added successfully");
                        sendOSCResponse("/daw/midi/notes/add/response", juce::OSCArgument(1)); // Success
                    } else {
                        DAWLogger::getInstance().log("Error: Failed to add bulk MIDI notes");
                        sendOSCResponse("/daw/midi/notes/add/response", juce::OSCArgument(0)); // Error
                    }
                } else {
                    DAWLogger::getInstance().log("Error: Invalid format for bulk MIDI notes");
                    sendOSCResponse("/daw/midi/notes/add/response", juce::OSCArgument(0)); // Error
                }
            } else {
                DAWLogger::getInstance().log("Error: Invalid add bulk MIDI notes message format");
                sendOSCResponse("/daw/midi/notes/add/response", juce::OSCArgument(0)); // Error
            }
        }
        else {
            DAWLogger::getInstance().log("Warning: Unknown OSC message pattern: " + message.getAddressPattern().toString().toStdString());
        }
        
        // Log that we've finished processing this message
        DAWLogger::getInstance().log("Finished processing OSC message: " + message.getAddressPattern().toString().toStdString());
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
        auto currentBPM = currentEdit->tempoSequence.getTempoAt(timePosition).getBpm();
        
        bool positionChanged = std::abs(timePosition.inSeconds() - lastSentTimePosition) > EPSILON || 
                             std::abs(beatPosition.inBeats() - lastSentBeatPosition) > EPSILON;
        bool bpmChanged = std::abs(currentBPM - lastSentBPM) > EPSILON;
        
        // Only send if position or BPM has changed
        if (positionChanged || bpmChanged) {
            // Send position updates if changed
            if (positionChanged) {
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
            
            // Send BPM update if changed
            if (bpmChanged) {
                oscSender.send("/daw/bpm/update", 
                    juce::OSCArgument(static_cast<float>(currentBPM))
                );
                
                // Update last sent BPM
                lastSentBPM = currentBPM;
                
                DAWLogger::getInstance().log("BPM update - " + 
                    std::to_string(currentBPM) + " BPM");
            }
        }
    }
}

void DAWBackend::timerCallback()
{
    sendPositionUpdate();
} 