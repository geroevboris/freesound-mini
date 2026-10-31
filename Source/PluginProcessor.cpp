#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

//==============================================================================
struct SoundResult
{
    int id = 0;
    juce::String name;
    double duration = 0.0;
    juce::String previewUrl;
    juce::File localFile;
};

//==============================================================================
class FreesoundMiniAudioProcessor : public juce::AudioProcessor
{
public:
    FreesoundMiniAudioProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        formatManager.registerBasicFormats();
    }

    ~FreesoundMiniAudioProcessor() override {}

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        transportSource.prepareToPlay (samplesPerBlock, sampleRate);
    }

    void releaseResources() override
    {
        transportSource.releaseResources();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.clear();
        juce::AudioSourceChannelInfo channelInfo (&buffer, 0, buffer.getNumSamples());
        transportSource.getNextAudioBlock (channelInfo);
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "FreesoundMini"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    void playPreview (const juce::File& file)
    {
        transportSource.stop();
        readerSource.reset();

        if (auto* reader = formatManager.createReaderFor (file))
        {
            readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
            transportSource.setSource (readerSource.get(), 0, nullptr, reader->sampleRate);
            transportSource.start();
        }
    }

    void stopPreview()
    {
        transportSource.stop();
    }

private:
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transportSource;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreesoundMiniAudioProcessor)
};

//==============================================================================
class FreesoundMiniEditor : public juce::AudioProcessorEditor,
                            public juce::TableListBoxModel,
                            public juce::DragAndDropContainer,
                            private juce::Thread
{
public:
    explicit FreesoundMiniEditor (FreesoundMiniAudioProcessor& p)
        : AudioProcessorEditor (&p), Thread ("FreesoundNetworkThread"), audioProcessor (p)
    {
        setSize (640, 500);

        cacheDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreesoundCache");
        cacheDir.createDirectory();

        tokenInput.setTextToShowWhenEmpty ("Paste Freesound API Key here...", juce::Colours::grey);
        addAndMakeVisible (tokenInput);

        searchInput.setTextToShowWhenEmpty ("Search sounds (e.g. kick, synth, snare)...", juce::Colours::grey);
        searchInput.onReturnKey = [this] { searchButton.triggerClick(); };
        addAndMakeVisible (searchInput);

        searchButton.onClick = [this] {
            if (!isThreadRunning())
            {
                currentQuery = searchInput.getText().trim();
                statusLabel.setText ("Searching...", juce::dontSendNotification);
                startThread();
            }
        };
        addAndMakeVisible (searchButton);

        table.setModel (this);
        table.getHeader().addColumn ("Title", 1, 380);
        table.getHeader().addColumn ("Duration", 2, 90);
        table.getHeader().addColumn ("Action", 3, 130);
        addAndMakeVisible (table);

        statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (statusLabel);
    }

    ~FreesoundMiniEditor() override
    {
        stopThread (4000);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1b1b1f));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        tokenInput.setBounds (area.removeFromTop (30));
        area.removeFromTop (8);

        auto searchRow = area.removeFromTop (32);
        searchButton.setBounds (searchRow.removeFromRight (90));
        searchRow.removeFromRight (8);
        searchInput.setBounds (searchRow);
        area.removeFromTop (10);

        statusLabel.setBounds (area.removeFromBottom (24));
        table.setBounds (area);
    }

    int getNumRows() override
    {
        const juce::ScopedLock sl (resultsLock);
        return static_cast<int> (results.size());
    }

    void paintRowBackground (juce::Graphics& g, int, int, int, bool rowIsSelected) override
    {
        if (rowIsSelected)
            g.fillAll (juce::Colour (0xff323a48));
        else
            g.fillAll (juce::Colour (0xff242429));
    }

    void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool) override
    {
        SoundResult item;
        {
            const juce::ScopedLock sl (resultsLock);
            if (rowNumber < 0 || rowNumber >= static_cast<int> (results.size()))
                return;
            item = results[static_cast<size_t> (rowNumber)];
        }

        g.setColour (juce::Colours::white);

        if (columnId == 1)
        {
            g.drawText (item.name, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }
        else if (columnId == 2)
        {
            g.drawText (juce::String (item.duration, 1) + " s", 0, 0, width, height, juce::Justification::centred);
        }
        else if (columnId == 3)
        {
            g.setColour (item.localFile.existsAsFile() ? juce::Colours::lightgreen : juce::Colours::orange);
            g.drawText (item.localFile.existsAsFile() ? "[DRAG TO LIVE]" : "[Click to Load]", 0, 0, width, height, juce::Justification::centred);
        }
    }

    void cellClicked (int rowNumber, int, const juce::MouseEvent&) override
    {
        SoundResult item;
        {
            const juce::ScopedLock sl (resultsLock);
            if (rowNumber < 0 || rowNumber >= static_cast<int> (results.size()))
                return;
            item = results[static_cast<size_t> (rowNumber)];
        }

        if (item.localFile.existsAsFile())
        {
            audioProcessor.playPreview (item.localFile);
            juce::StringArray files;
            files.add (item.localFile.getFullPathName());
            performExternalDragDropOfFiles (files, false);
        }
        else
        {
            downloadAndPlay (rowNumber);
        }
    }

private:
    void run() override
    {
        auto token = tokenInput.getText().trim();
        if (token.isEmpty() || currentQuery.isEmpty()) return;

        auto urlStr = "https://freesound.org/apiv2/search/text/?query="
                    + juce::URL::addEscapeChars (currentQuery, true)
                    + "&filter=" + juce::URL::addEscapeChars ("license:\"Creative Commons 0\"", true)
                    + "&token=" + token
                    + "&fields=id,name,duration,previews&page_size=25";

        juce::URL url (urlStr);
        auto response = url.readEntireTextStream();

        auto varJson = juce::JSON::parse (response);
        if (auto* obj = varJson.getDynamicObject())
        {
            auto resultsVar = obj->getProperty ("results");
            if (resultsVar.isArray())
            {
                auto* resultsArray = resultsVar.getArray();
                std::vector<SoundResult> newResults;

                for (auto& elem : *resultsArray)
                {
                    SoundResult r;
                    r.id = static_cast<int> (elem["id"]);
                    r.name = elem["name"].toString();
                    r.duration = static_cast<double> (elem["duration"]);

                    if (auto* prev = elem["previews"].getDynamicObject())
                    {
                        r.previewUrl = prev->getProperty ("preview-hq-mp3").toString();
                        if (r.previewUrl.isEmpty())
                            r.previewUrl = prev->getProperty ("preview-lq-mp3").toString();
                    }
                    newResults.push_back (r);
                }

                juce::MessageManager::callAsync ([this, res = std::move (newResults)]() mutable {
                    {
                        const juce::ScopedLock sl (resultsLock);
                        results = std::move (res);
                    }
                    table.updateContent();
                    statusLabel.setText ("Found: " + juce::String (getNumRows()) + " sounds", juce::dontSendNotification);
                });
                return;
            }
        }

        juce::MessageManager::callAsync ([this] {
            statusLabel.setText ("Error: Check Token or Connection", juce::dontSendNotification);
        });
    }

    void downloadAndPlay (int index)
    {
        statusLabel.setText ("Loading preview...", juce::dontSendNotification);

        juce::Thread::launch ([this, index]() {
            juce::String previewUrl;
            int soundId = 0;

            {
                const juce::ScopedLock sl (resultsLock);
                if (index < 0 || index >= static_cast<int> (results.size()))
                    return;
                previewUrl = results[static_cast<size_t> (index)].previewUrl;
                soundId = results[static_cast<size_t> (index)].id;
            }

            auto local = cacheDir.getChildFile (juce::String (soundId) + ".mp3");

            if (!local.existsAsFile() && previewUrl.isNotEmpty())
            {
                juce::URL url (previewUrl);
                juce::MemoryBlock mb;
                if (url.readEntireBinaryStream (mb, 10000))
                {
                    local.replaceWithData (mb.getData(), mb.getSize());
                }
            }

            juce::MessageManager::callAsync ([this, index, local]() {
                {
                    const juce::ScopedLock sl (resultsLock);
                    if (index >= 0 && index < static_cast<int> (results.size()))
                        results[static_cast<size_t> (index)].localFile = local;
                }
                table.repaintRow (index);
                audioProcessor.playPreview (local);
                statusLabel.setText ("Ready! Click row again & Drag into Live", juce::dontSendNotification);
            });
        });
    }

    FreesoundMiniAudioProcessor& audioProcessor;

    juce::TextEditor tokenInput;
    juce::TextEditor searchInput;
    juce::TextButton searchButton { "Search" };
    juce::TableListBox table;
    juce::Label statusLabel;

    std::vector<SoundResult> results;
    juce::CriticalSection resultsLock;
    juce::String currentQuery;
    juce::File cacheDir;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreesoundMiniEditor)
};

//==============================================================================
juce::AudioProcessorEditor* FreesoundMiniAudioProcessor::createEditor()
{
    return new FreesoundMiniEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FreesoundMiniAudioProcessor();
}
