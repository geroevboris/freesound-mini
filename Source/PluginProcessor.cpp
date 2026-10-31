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
            float xPrev = (float) (i - 1) * xStep;
            float xCurr = (float) i * xStep;
            float yPrev = topPoints[i - 1];
            float yCurr = topPoints[i];
            path.quadraticTo (xPrev, yPrev, (xPrev + xCurr) * 0.5f, (yPrev + yCurr) * 0.5f);
        }

        path.lineTo ((float) width, midY);

        for (size_t i = topPoints.size() - 1; i > 0; --i)
        {
            float xCurr = (float) i * xStep;
            float xPrev = (float) (i - 1) * xStep;
            float yCurr = midY + (midY - topPoints[i]);
            float yPrev = midY + (midY - topPoints[i - 1]);
            path.quadraticTo (xCurr, yCurr, (xCurr + xPrev) * 0.5f, (yCurr + yPrev) * 0.5f);
        }

        path.closeSubPath();

        juce::ColourGradient grad (juce::Colour (0xff2d6a4f), 0.0f, midY,
                                   juce::Colour (0xff52b788), (float) width * 0.75f, midY, false);
        g.setGradientFill (grad);
        g.fillPath (path);

        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (9.0f);
        g.drawText ("(Click waveform to play, hold & drag into project)",
                    getLocalBounds().removeFromBottom (16), juce::Justification::centred);
    }

private:
    juce::AudioBuffer<float> localBuffer;
    bool dragStarted = false;
};

//==============================================================================
class CustomKnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CustomKnobLookAndFeel()
    {
        setColour (juce::Slider::thumbColourId, juce::Colour (0xff52b788));
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff1a1c1e));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, const float startAngle, const float endAngle,
                           juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        auto radius = std::min (bounds.getWidth(), bounds.getHeight()) / 2.0f;
        auto toX = bounds.getCentreX();
        auto toY = bounds.getCentreY();
        auto arcRadius = radius - 4.0f;

        g.setColour (juce::Colour (0xff1a1c1e));
        g.drawEllipse (toX - arcRadius, toY - arcRadius, arcRadius * 2.0f, arcRadius * 2.0f, 3.5f);

        auto angle = startAngle + sliderPos * (endAngle - startAngle);
        juce::Path activeArc;
        activeArc.addCentredArc (toX, toY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);

        g.setColour (juce::Colour (0xff52b788));
        g.strokePath (activeArc, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (juce::Colour (0xff0d0e10));
        g.fillEllipse (toX - (radius - 7.0f), toY - (radius - 7.0f), (radius - 7.0f) * 2.0f, (radius - 7.0f) * 2.0f);

        juce::Path p;
        auto pointerLength = radius - 7.0f;
        auto pointerThickness = 2.0f;
        p.addRectangle (-pointerThickness * 0.5f, -pointerLength, pointerThickness, pointerLength * 0.55f);
        p.applyTransform (juce::AffineTransform::rotation (angle).translated (toX, toY));
        g.setColour (juce::Colour (0xff52b788));
        g.fillPath (p);

        g.setColour (juce::Colours::white);
        g.setFont (11.0f);
        auto valText = juce::String ((int) std::round (slider.getValue())) + " st";
        g.drawText (valText, (int) (toX - radius), (int) (toY - 8.0f), (int) (radius * 2.0f), 16, juce::Justification::centred);
    }
};

//==============================================================================
class InfoOverlay : public juce::Component
{
public:
    explicit InfoOverlay (std::function<void()> closeCallback) : onClose (std::move (closeCallback))
    {
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.94f));

        auto area = getLocalBounds().reduced (24);
        g.setColour (juce::Colour (0xff52b788));
        g.setFont (18.0f);
        g.drawText ("FREESOUND MINI", area.removeFromTop (35), juce::Justification::centredLeft);

        g.setFont (12.0f);
        g.setColour (juce::Colours::lightgrey);

        juce::String text =
            "Быстрый поиск звуков прямо в Ableton Live.\n\n"
            "1. Подключение API Token:\n"
            "   Получите бесплатный ключ (Client Secret / API Key) на freesound.org/apiv2/apply/.\n"
            "   Вставьте в поле 'Enter API Key'. Зелёная плашка [✔ API OK] подтвердит ввод.\n\n"
            "2. Фильтры длины и каналов:\n"
            "   [All Ch / Mono / Stereo] фильтрует сэмплы по количеству каналов.\n"
            "   Слайдер Max ограничивает максимальную длительность файла в секундах.\n\n"
            "3. Предпрослушивание:\n"
            "   Звук воспроизводится сразу при выборе строки или клике по волне.\n"
            "   Нормализация [✔ Norm -6dB] выравнивает пик до -6 dBFS без клиппинга.\n"
            "   Ручка Pitch (-12..+12) меняет тон в реальном времени и сбрасывается на 0 при смене звука.\n\n"
            "4. Drag-and-Drop:\n"
            "   Зажмите ЛКМ прямо на зеленой волне и перетащите звук на дорожку Ableton Live.\n"
            "   Экспортируется чистый исходный файл.";

        g.drawFittedText (text, area, juce::Justification::topLeft, 24);
    }

    void resized() override
    {
        closeButton.setBounds (getWidth() - 36, 12, 24, 24);
    }

private:
    std::function<void()> onClose;
    juce::TextButton closeButton { "X" };
};

//==============================================================================
class FreesoundMiniEditor : public juce::AudioProcessorEditor,
                            public juce::TableListBoxModel,
                            public juce::DragAndDropContainer,
                            private juce::Thread
{
public:
    explicit FreesoundMiniEditor (FreesoundMiniAudioProcessor& p)
        : AudioProcessorEditor (&p), Thread ("FreesoundSearchThread"), proc (p)
    {
        setSize (360, 560);

        if (proc.sampleDirectory == juce::File())
        {
            proc.sampleDirectory = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                       .getChildFile ("FreesoundSamples");
        }
        proc.sampleDirectory.createDirectory();

        tokenInput.setTextToShowWhenEmpty ("Enter API Key...", juce::Colours::grey);
        tokenInput.setText (proc.savedToken, false);
        tokenInput.onTextChange = [this] {
            proc.savedToken = tokenInput.getText().trim();
            updateAPIStatus();
        };
        addAndMakeVisible (tokenInput);

        updateAPIStatus();
        addAndMakeVisible (apiStatusLabel);

        dirButton.onClick = [this] {
            fileChooser = std::make_unique<juce::FileChooser> ("Choose Sample Folder",
                                                               proc.sampleDirectory,
                                                               "*");
            auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
            fileChooser->launchAsync (flags, [this] (const juce::FileChooser& fc) {
                auto result = fc.getResult();
                if (result.isDirectory())
                {
                    proc.sampleDirectory = result;
                    statusLabel.setText ("Folder: " + result.getFileName(), juce::dontSendNotification);
                }
            });
        };
        addAndMakeVisible (dirButton);

        openFolderButton.onClick = [this] {
            proc.sampleDirectory.startAsProcess();
        };
        addAndMakeVisible (openFolderButton);

        infoButton.onClick = [this] {
            if (infoOverlay != nullptr)
                infoOverlay->setVisible (true);
        };
        addAndMakeVisible (infoButton);

        searchInput.setTextToShowWhenEmpty ("Search sounds (808, snare, ambient)...", juce::Colours::grey);
        searchInput.onReturnKey = [this] { startSearch(); };
        addAndMakeVisible (searchInput);

        searchButton.onClick = [this] { startSearch(); };
        addAndMakeVisible (searchButton);

        channelBox.addItem ("All Ch", 1);
        channelBox.addItem ("Mono", 2);
        channelBox.addItem ("Stereo", 3);
        channelBox.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (channelBox);

        durationSlider.setRange (1.0, 60.0, 1.0);
        durationSlider.setValue (15.0);
        durationSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 18);
        durationSlider.setTextValueSuffix ("s");
        addAndMakeVisible (durationSlider);

        durationLabel.setText ("Max:", juce::dontSendNotification);
        durationLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (durationLabel);

        table.setModel (this);
        table.getHeader().addColumn ("NAME", 1, 195);
        table.getHeader().addColumn ("DUR", 2, 65);
        table.getHeader().addColumn ("STATUS", 3, 60);
        table.setMultipleSelectionAllowed (false);
        addAndMakeVisible (table);

        waveformDisplay.onClicked = [this] {
            proc.playFromStart();
        };
        waveformDisplay.onDragged = [this] {
            startExternalDrag();
        };
        addAndMakeVisible (waveformDisplay);

        playButton.onClick = [this] { proc.playFromStart(); };
        addAndMakeVisible (playButton);

        stopButton.onClick = [this] { proc.stopPreview(); };
        addAndMakeVisible (stopButton);

        pitchSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pitchSlider.setRange (-12.0, 12.0, 1.0);
        pitchSlider.setValue (0.0);
        pitchSlider.setDoubleClickReturnValue (true, 0.0);
        pitchSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pitchSlider.setLookAndFeel (&knobLookAndFeel);
        pitchSlider.onValueChange = [this] { proc.setPitchSemitones (pitchSlider.getValue()); };
        addAndMakeVisible (pitchSlider);

        normCheckbox.setButtonText ("Norm -6dB");
        normCheckbox.setToggleState (true, juce::dontSendNotification);
        normCheckbox.onClick = [this] { proc.setNormalization (normCheckbox.getToggleState()); };
        addAndMakeVisible (normCheckbox);

        volSlider.setSliderStyle (juce::Slider::LinearBar);
        volSlider.setRange (0.0, 1.0, 0.01);
        volSlider.setValue (0.8);
        volSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volSlider.onValueChange = [this] { proc.setGain ((float) volSlider.getValue()); };
        addAndMakeVisible (volSlider);

        volLabel.setText ("Vol", juce::dontSendNotification);
        volLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (volLabel);

        statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (statusLabel);

        infoOverlay = std::make_unique<InfoOverlay> ([this] { infoOverlay->setVisible (false); });
        infoOverlay->setVisible (false);
        addChildComponent (infoOverlay.get());

        refreshWaveformFromProcessor();
    }

    ~FreesoundMiniEditor() override
    {
        signalThreadShouldExit();
        stopThread (4000);
        pitchSlider.setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0b0d));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        auto topBar = area.removeFromTop (26);
        infoButton.setBounds (topBar.removeFromRight (26));
        topBar.removeFromRight (4);
        openFolderButton.setBounds (topBar.removeFromRight (26));
        topBar.removeFromRight (4);
        dirButton.setBounds (topBar.removeFromRight (52));
        apiStatusLabel.setBounds (topBar.removeFromLeft (88));
        topBar.removeFromLeft (4);
        tokenInput.setBounds (topBar);

        area.removeFromTop (6);

        auto searchRow = area.removeFromTop (28);
        searchButton.setBounds (searchRow.removeFromRight (58));
        searchRow.removeFromRight (4);
        searchInput.setBounds (searchRow);

        area.removeFromTop (6);

        auto filterRow = area.removeFromTop (24);
        channelBox.setBounds (filterRow.removeFromLeft (85));
        filterRow.removeFromLeft (8);
        durationLabel.setBounds (filterRow.removeFromLeft (35));
        durationSlider.setBounds (filterRow);

        area.removeFromTop (8);

        auto bottomControl = area.removeFromBottom (46);
        area.removeFromBottom (6);
        waveformDisplay.setBounds (area.removeFromBottom (75));

        area.removeFromBottom (4);
        statusLabel.setBounds (area.removeFromBottom (16));

        table.setBounds (area);

        auto bArea = bottomControl;
        playButton.setBounds (bArea.removeFromLeft (36));
        bArea.removeFromLeft (4);
        stopButton.setBounds (bArea.removeFromLeft (36));

        bArea.removeFromLeft (8);
        pitchSlider.setBounds (bArea.removeFromLeft (60));

        bArea.removeFromLeft (8);
        normCheckbox.setBounds (bArea.removeFromLeft (82));

        bArea.removeFromLeft (6);
        volLabel.setBounds (bArea.removeFromLeft (22));
        volSlider.setBounds (bArea);

        if (infoOverlay != nullptr)
            infoOverlay->setBounds (getLocalBounds());
    }

    int getNumRows() override
    {
        const juce::ScopedLock sl (resultsLock);
        return static_cast<int> (results.size());
    }

    void paintRowBackground (juce::Graphics& g, int, int, int, bool rowIsSelected) override
    {
        if (rowIsSelected)
            g.fillAll (juce::Colour (0xff1f2a24));
        else
            g.fillAll (juce::Colour (0xff111215));
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
        g.setFont (11.5f);

        if (columnId == 1)
        {
            g.drawText (item.name, 6, 0, width - 12, height, juce::Justification::centredLeft, true);
        }
        else if (columnId == 2)
        {
            g.setColour (juce::Colours::grey);
            g.drawText (juce::String (item.duration, 1) + "s", 0, 0, width, height, juce::Justification::centred);
        }
        else if (columnId == 3)
        {
            if (item.localFile.existsAsFile())
            {
                g.setColour (juce::Colour (0xff52b788));
                g.drawText ("Ready", 0, 0, width, height, juce::Justification::centred);
            }
            else
            {
                g.setColour (juce::Colour (0xffcca43b));
                g.drawText ("Load", 0, 0, width, height, juce::Justification::centred);
            }
        }
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        if (lastRowSelected >= 0 && lastRowSelected != lastTriggeredRow)
        {
            lastTriggeredRow = lastRowSelected;
            pitchSlider.setValue (0.0, juce::dontSendNotification);
            triggerLoadAndPlay (lastRowSelected);
        }
    }

    void cellClicked (int rowNumber, int, const juce::MouseEvent&) override
    {
        if (rowNumber >= 0)
        {
            lastTriggeredRow = rowNumber;
            pitchSlider.setValue (0.0, juce::dontSendNotification);
            triggerLoadAndPlay (rowNumber);
        }
    }

private:
    void startSearch()
    {
        if (isThreadRunning())
            return;

        searchTokenSnapshot = tokenInput.getText().trim();
        searchQuerySnapshot = searchInput.getText().trim();
        maxDurationSnapshot = durationSlider.getValue();
        channelFilterSnapshot = channelBox.getSelectedId();

        if (searchTokenSnapshot.isEmpty())
        {
            statusLabel.setText ("Paste API Key first!", juce::dontSendNotification);
            return;
        }
        if (searchQuerySnapshot.isEmpty())
        {
            statusLabel.setText ("Enter search query!", juce::dontSendNotification);
            return;
        }

        lastTriggeredRow = -1;
        statusLabel.setText ("Searching...", juce::dontSendNotification);
        startThread();
    }

    void startExternalDrag()
    {
        juce::File fileToDrag = proc.getCurrentFile();

        if (!fileToDrag.existsAsFile())
        {
            int row = table.getSelectedRow();
            const juce::ScopedLock sl (resultsLock);
            if (row >= 0 && row < static_cast<int> (results.size()))
                fileToDrag = results[static_cast<size_t> (row)].localFile;
        }

        if (fileToDrag.existsAsFile())
        {
            juce::StringArray files;
            files.add (fileToDrag.getFullPathName());
            performExternalDragDropOfFiles (files, false);
        }
    }

    void refreshWaveformFromProcessor()
    {
        juce::AudioBuffer<float> temp;
        proc.copyCachedBuffer (temp);
        waveformDisplay.setWaveformData (temp);
    }

    void updateAPIStatus()
    {
        if (proc.savedToken.length() > 5)
        {
            apiStatusLabel.setText ("✔ API OK", juce::dontSendNotification);
            apiStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff52b788));
        }
        else
        {
            apiStatusLabel.setText ("✕ NO API", juce::dontSendNotification);
            apiStatusLabel.setColour (juce::Label::textColourId, juce::Colours::indianred);
        }
    }

    void triggerLoadAndPlay (int index)
    {
        juce::File existing;
        {
            const juce::ScopedLock sl (resultsLock);
            if (index < 0 || index >= static_cast<int> (results.size()))
                return;
            existing = results[static_cast<size_t> (index)].localFile;
        }

        if (existing.existsAsFile())
        {
            proc.loadAndPlayFile (existing, true);
            refreshWaveformFromProcessor();
            statusLabel.setText ("Playing. Drag waveform to Ableton!", juce::dontSendNotification);
        }
        else
        {
            downloadAndPlay (index);
        }
    }

    void run() override
    {
        juce::String filterStr = "duration:[0.05 TO " + juce::String (maxDurationSnapshot, 1) + "]";
        if (channelFilterSnapshot == 2) filterStr += " channels:1";
        else if (channelFilterSnapshot == 3) filterStr += " channels:2";

        auto urlStr = "https://freesound.org/apiv2/search/text/?query="
                    + juce::URL::addEscapeChars (searchQuerySnapshot, true)
                    + "&filter=" + juce::URL::addEscapeChars (filterStr, true)
                    + "&token=" + juce::URL::addEscapeChars (searchTokenSnapshot, true)
                    + "&fields=id,name,duration,previews&page_size=40";

        juce::URL url (urlStr);
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs (10000);

        juce::String response;
        if (auto stream = url.createInputStream (options))
            response = stream->readEntireStreamAsString();

        if (threadShouldExit())
            return;

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
                    if (threadShouldExit())
                        return;

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

                    auto sanitizedName = juce::File::createLegalFileName (r.name);
                    auto target = proc.sampleDirectory.getChildFile (juce::String (r.id) + "_" + sanitizedName + ".mp3");
                    if (target.existsAsFile())
                        r.localFile = target;

                    newResults.push_back (r);
                }

                juce::Component::SafePointer<FreesoundMiniEditor> safeThis (this);
                juce::MessageManager::callAsync ([safeThis, res = std::move (newResults)]() mutable {
                    if (safeThis == nullptr)
                        return;
                    {
                        const juce::ScopedLock sl (safeThis->resultsLock);
                        safeThis->results = std::move (res);
                    }
                    safeThis->table.updateContent();
                    safeThis->statusLabel.setText ("Found: " + juce::String (safeThis->getNumRows()) + " sounds", juce::dontSendNotification);
                });
                return;
            }
        }

        juce::Component::SafePointer<FreesoundMiniEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis] {
            if (safeThis != nullptr)
                safeThis->statusLabel.setText ("Error: Check API Key or Connection", juce::dontSendNotification);
        });
    }

    void downloadAndPlay (int index)
    {
        statusLabel.setText ("Downloading preview...", juce::dontSendNotification);

        juce::String previewUrl;
        juce::String name;
        int soundId = 0;

        {
            const juce::ScopedLock sl (resultsLock);
            if (index < 0 || index >= static_cast<int> (results.size()))
                return;
            previewUrl = results[static_cast<size_t> (index)].previewUrl;
            name = results[static_cast<size_t> (index)].name;
            soundId = results[static_cast<size_t> (index)].id;
        }

        auto targetDir = proc.sampleDirectory;
        juce::Component::SafePointer<FreesoundMiniEditor> safeThis (this);

        juce::Thread::launch ([safeThis, index, previewUrl, name, soundId, targetDir]() {
            auto sanitized = juce::File::createLegalFileName (name);
            auto local = targetDir.getChildFile (juce::String (soundId) + "_" + sanitized + ".mp3");

            if (!local.existsAsFile() && previewUrl.isNotEmpty())
            {
                juce::URL url (previewUrl);
                auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                   .withConnectionTimeoutMs (12000);

                if (auto stream = url.createInputStream (options))
                {
                    juce::MemoryBlock mb;
                    stream->readIntoMemoryBlock (mb);
                    if (mb.getSize() > 0)
                        local.replaceWithData (mb.getData(), mb.getSize());
                }
            }

            juce::MessageManager::callAsync ([safeThis, index, local]() {
                if (safeThis == nullptr)
                    return;

                if (local.existsAsFile())
                {
                    {
                        const juce::ScopedLock sl (safeThis->resultsLock);
                        if (index >= 0 && index < static_cast<int> (safeThis->results.size()))
                            safeThis->results[static_cast<size_t> (index)].localFile = local;
                    }
                    safeThis->table.repaintRow (index);
                    safeThis->proc.loadAndPlayFile (local, true);
                    safeThis->refreshWaveformFromProcessor();
                    safeThis->statusLabel.setText ("Ready! Drag waveform to track.", juce::dontSendNotification);
                }
                else
                {
                    safeThis->statusLabel.setText ("Download failed.", juce::dontSendNotification);
                }
            });
        });
    }

    FreesoundMiniAudioProcessor& proc;

    juce::TextEditor tokenInput;
    juce::Label apiStatusLabel;
    juce::TextEditor searchInput;
    juce::TextButton searchButton { "Search" };

    juce::ComboBox channelBox;
    juce::Slider durationSlider;
    juce::Label durationLabel;

    juce::TextButton dirButton { "Folder" };
    juce::TextButton openFolderButton { "Open" };
    juce::TextButton infoButton { "i" };

    juce::TableListBox table;
    WaveformDisplay waveformDisplay;

    juce::TextButton playButton { "Play" };
    juce::TextButton stopButton { "Stop" };

    juce::Slider pitchSlider;
    CustomKnobLookAndFeel knobLookAndFeel;

    juce::ToggleButton normCheckbox;
    juce::Slider volSlider;
    juce::Label volLabel;

    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<InfoOverlay> infoOverlay;
    std::vector<SoundResult> results;
    juce::CriticalSection resultsLock;

    juce::String searchTokenSnapshot;
    juce::String searchQuerySnapshot;
    double maxDurationSnapshot = 15.0;
    int channelFilterSnapshot = 1;
    int lastTriggeredRow = -1;

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
