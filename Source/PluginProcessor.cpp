#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>
#include <algorithm>

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

    ~FreesoundMiniAudioProcessor() override
    {
        transportSource.stop();
        transportSource.setSource (nullptr);
        readerSource.reset();
    }

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
        juce::ScopedNoDenormals noDenormals;
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
        if (auto xml = tree.createXml())
            copyXmlToBinary (*xml, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (auto xmlState = getXmlFromBinary (data, sizeInBytes))
        {
            auto tree = juce::ValueTree::fromXml (*xmlState);
            if (tree.isValid())
            {
                savedToken = tree.getProperty ("apiToken", "").toString();
                auto dir = tree.getProperty ("saveDirectory", "").toString();
                if (dir.isNotEmpty())
                    sampleDirectory = juce::File (dir);
            }
        }
    }

    void loadAndPlayFile (const juce::File& file, bool resetPitchToZero)
    {
        if (!file.existsAsFile())
            return;

        if (resetPitchToZero)
            pitchSemitones = 0.0;

        transportSource.stop();
        transportSource.setSource (nullptr);
        readerSource.reset();
        currentFile = file;

        if (std::unique_ptr<juce::AudioFormatReader> tempReader { formatManager.createReaderFor (file) })
        {
            int numCh = (int) tempReader->numChannels;
            int numSamples = (int) tempReader->lengthInSamples;

            if (numCh > 0 && numSamples > 0)
            {
                juce::AudioBuffer<float> tempBuf (numCh, numSamples);
                tempReader->read (&tempBuf, 0, numSamples, 0, true, true);

                float peak = 0.0f;
                for (int ch = 0; ch < numCh; ++ch)
                    peak = std::max (peak, tempBuf.getMagnitude (ch, 0, numSamples));

                loadedFilePeak = (peak > 0.0001f) ? peak : 1.0f;
                originalFileSampleRate = tempReader->sampleRate;

                const juce::ScopedLock sl (bufferLock);
                cachedBuffer = std::move (tempBuf);
            }
        }

        rebuildTransport (0.0, true);
    }

    void rebuildTransport (double startPosSeconds, bool autoStart)
    {
        if (!currentFile.existsAsFile())
            return;

        bool wasPlaying = transportSource.isPlaying();
        transportSource.stop();
        transportSource.setSource (nullptr);
        readerSource.reset();

        if (auto* reader = formatManager.createReaderFor (currentFile))
        {
            readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);

            double speedRatio = std::pow (2.0, pitchSemitones / 12.0);
            transportSource.setSource (readerSource.get(), 0, nullptr, originalFileSampleRate * speedRatio);

            updateGainInternal();
            transportSource.setPosition (startPosSeconds);

            if (autoStart || wasPlaying)
                transportSource.start();
        }
    }

    void playFromStart()
    {
        if (readerSource != nullptr)
        {
            transportSource.setPosition (0.0);
            transportSource.start();
        }
        else if (currentFile.existsAsFile())
        {
            rebuildTransport (0.0, true);
        }
    }

    void stopPreview()
    {
        transportSource.stop();
    }

    void setGain (float g)
    {
        previewGain = g;
        updateGainInternal();
    }

    void setPitchSemitones (double semitones)
    {
        if ( std::abs (pitchSemitones - semitones) < 0.001 )
            return;

        pitchSemitones = semitones;
        if (readerSource != nullptr)
        {
            double pos = transportSource.getCurrentPosition();
            rebuildTransport (pos, false);
        }
    }

    void setNormalization (bool shouldNormalize)
    {
        useNormalization = shouldNormalize;
        updateGainInternal();
    }

    void copyCachedBuffer (juce::AudioBuffer<float>& dest)
    {
        const juce::ScopedLock sl (bufferLock);
        dest.makeCopyOf (cachedBuffer);
    }

    juce::File getCurrentFile() const { return currentFile; }

    juce::String savedToken;
    juce::File sampleDirectory;

private:
    void updateGainInternal()
    {
        float normGain = useNormalization ? (0.501187f / loadedFilePeak) : 1.0f; // -6 dBFS
        transportSource.setGain (previewGain * normGain);
    }

    float previewGain = 0.8f;
    double pitchSemitones = 0.0;
    bool useNormalization = true;
    float loadedFilePeak = 1.0f;
    double originalFileSampleRate = 44100.0;

    juce::File currentFile;
    juce::CriticalSection bufferLock;
    juce::AudioBuffer<float> cachedBuffer;

    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transportSource;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreesoundMiniAudioProcessor)
};

//==============================================================================
class WaveformDisplay : public juce::Component
{
public:
    std::function<void()> onClicked;
    std::function<void()> onDragged;

    WaveformDisplay() {}

    void setWaveformData (const juce::AudioBuffer<float>& newBuffer)
    {
        localBuffer.makeCopyOf (newBuffer);
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        dragStarted = false;
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (!dragStarted && e.getDistanceFromDragStart() < 6)
        {
            if (onClicked)
                onClicked();
        }
        dragStarted = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (!dragStarted && e.getDistanceFromDragStart() >= 6)
        {
            dragStarted = true;
            if (onDragged)
                onDragged();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff141517));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);

        if (localBuffer.getNumSamples() == 0)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.6f));
            g.setFont (12.0f);
            g.drawText ("No Audio Loaded", getLocalBounds().withTrimmedBottom (18), juce::Justification::centred);
            g.setFont (9.5f);
            g.drawText ("(Click waveform to play, hold & drag into project)",
                        getLocalBounds().removeFromBottom (18), juce::Justification::centred);
            return;
        }

        auto width = getWidth();
        auto height = getHeight();
        auto midY = height / 2.0f;
        auto numSamples = localBuffer.getNumSamples();
        auto numChannels = localBuffer.getNumChannels();

        if (numSamples <= 0 || width <= 0) return;

        juce::Path path;
        path.startNewSubPath (0.0f, midY);

        const int numPoints = std::min (width, 160);
        auto sampleChunk = (double) numSamples / (double) numPoints;
        auto xStep = (float) width / (float) numPoints;

        std::vector<float> topPoints;
        topPoints.reserve ((size_t) numPoints + 1);
        topPoints.push_back (midY);

        for (int i = 0; i < numPoints; ++i)
        {
            int startSample = (int) (i * sampleChunk);
            int endSample = std::min (numSamples, (int) ((i + 1) * sampleChunk));
            float maxVal = 0.0f;

            for (int s = startSample; s < endSample; ++s)
            {
                for (int c = 0; c < numChannels; ++c)
                {
                    float val = std::abs (localBuffer.getSample (c, s));
                    if (val > maxVal) maxVal = val;
                }
            }

            float targetHeight = midY - (maxVal * midY * 0.78f);
            topPoints.push_back (targetHeight);
        }

        for (size_t i = 1; i < topPoints.size(); ++i)
        {
            float xPrev = (float) (i - 1) *
