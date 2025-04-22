#include <JuceHeader.h>
#include "DAWBackend.h"

class Application : public juce::JUCEApplication
{
public:
    Application() = default;

    const juce::String getApplicationName() override       { return "DAWBackend"; }
    const juce::String getApplicationVersion() override    { return "1.0.0"; }

    void initialise(const juce::String&) override
    {
        backend = std::make_unique<DAWBackend>();
        backend->createNewEdit();
    }

    void shutdown() override
    {
        backend = nullptr;
    }

private:
    std::unique_ptr<DAWBackend> backend;
};

START_JUCE_APPLICATION(Application) 