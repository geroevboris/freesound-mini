#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>

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
        currentSampleRate = sampleRate;
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

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        juce::ValueTree tree ("FreesoundMiniSettings");
        tree.setProperty ("apiToken", savedToken, nullptr);
        tree.setProperty ("saveDirectory", sampleDirectory.getFullPathName(), nullptr);
        std::unique_ptr<juce::XmlElement> xml (tree.createXml());
        copyXmlToBinary (*xml, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
        if (xmlState != nullptr)
        {
            auto tree = juce::ValueTree::fromXml (*xmlState);
            if (tree.isValid())
            {
                savedToken = tree.getProperty ("apiToken", "");
                auto dir = tree.getProperty ("saveDirectory", "");
                if (dir.toString().isNotEmpty())
                    sampleDirectory = juce::File (dir.toString());
            }
        }
    }

    void loadPreviewFile (const juce::File& file)
    {
        transportSource.stop();
        readerSource.reset();
        loadedBuffer.clear();

        if (auto* reader = formatManager.createReaderFor (file))
        {
            loadedBuffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&loadedBuffer, 0, (int) reader->lengthInSamples, 0, true, true);
            
            float peak = 0.0f;
            for (int channel = 0; channel < loadedBuffer.getNumChannels(); ++channel)
            {
                auto channelPeak = loadedBuffer.getMagnitude (channel, 0, loadedBuffer.getNumSamples());
                peak = std::max (peak, channelPeak);
            }
            loadedFilePeak = (peak > 0.0f) ? peak : 1.0f;

            originalSampleRate = reader->sampleRate;
            updatePlaySource();
            delete reader;
        }
    }

    void updatePlaySource()
    {
        if (loadedBuffer.getNumSamples() == 0) return;

        transportSource.stop();
        readerSource.reset();

        float normalizationGain = 1.0f;
        if (useNormalization)
            normalizationGain = 0.501187f / loadedFilePeak; // -
