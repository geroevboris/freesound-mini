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
        cachedBuffer.setSize (0, 0);
        currentFile = file;

        if (std::unique_ptr<juce::AudioFormatReader> reader { formatManager.createReaderFor (file) })
        {
            cachedBuffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&cachedBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

            float peak = 0.0f;
            for (int ch = 0; ch < cachedBuffer.getNumChannels(); ++ch)
                peak = std::max (peak, cachedBuffer.getMagnitude (ch, 0, cachedBuffer.getNumSamples()));

            loadedFilePeak = (peak > 0.0001f) ? peak : 1.0f;
            originalFileSampleRate = reader->sampleRate;

            updatePlaybackSettings();
        }
    }

    void updatePlaybackSettings()
    {
        if (currentFile == juce::File() || !currentFile.existsAsFile())
            return;

        transportSource.stop();
        readerSource.reset();

        if (auto* reader = formatManager.createReaderFor (currentFile))
        {
            readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);

            double speedRatio = std::pow (2.0, pitchSemitones / 12.0);
            transportSource.setSource (readerSource.get(), 0, nullptr, originalFileSampleRate * speedRatio);

            float normGain = 1.0f;
            if (useNormalization)
                normGain = 0.501187f / loadedFilePeak; // -6 dBFS

            transportSource.setGain (previewGain * normGain);
            transportSource.setPosition (0.0);
            transportSource.start();
        }
    }

    void stopPreview()
    {
        transportSource.stop();
    }

    void setGain (float g)
    {
        previewGain = g;
        float normGain = useNormalization ? (0.501187f / loadedFilePeak) : 1.0f;
        transportSource.setGain (previewGain * normGain);
    }

    void setPitchSemitones (double semitones)
    {
        pitchSemitones = semitones;
        updatePlaybackSettings();
    }

    void setNormalization (bool shouldNormalize)
    {
        useNormalization = shouldNormalize;
        float normGain = useNormalization ? (0.501187f / loadedFilePeak) : 1.0f;
        transportSource.setGain (previewGain * normGain);
    }

    juce::AudioBuffer<float>* getCachedBuffer() { return &cachedBuffer; }

    juce::String savedToken;
    juce::File sampleDirectory;

private:
    float previewGain = 0.8f;
    double pitchSemitones = 0.0;
    bool useNormalization = true;
    float loadedFilePeak = 1.0f;
    double originalFileSampleRate = 44100.0;

    juce::File currentFile;
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
    WaveformDisplay() {}

    void setBuffer (juce::AudioBuffer<float>* newBuffer)
    {
        buffer = newBuffer;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff141517));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);

        if (buffer == nullptr || buffer->getNumSamples() == 0)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.6f));
            g.setFont (juce::Font ("Helvetica Neue", 12.0f, juce::Font::plain));
            g.drawText ("No Audio Loaded", getLocalBounds().withSize (getWidth(), getHeight() - 18), juce::Justification::centred);
            g.setFont (juce::Font ("Helvetica Neue", 9.5f, juce::Font::plain));
            g.drawText ("(Click waveform to play, hold & drag into project)", 
                        getLocalBounds().removeFromBottom (18), juce::Justification::centred);
            return;
        }

        auto width = getWidth();
        auto height = getHeight();
        auto midY = height / 2.0f;
        auto numSamples = buffer->getNumSamples();
        auto numChannels = buffer->getNumChannels();

        if (numSamples <= 0 || width <= 0) return;

        juce::Path path;
        path.startNewSubPath (0.0f, midY);

        const int numPoints = std::min (width, 160);
        auto sampleChunk = (double) numSamples / (double) numPoints;
        auto xStep = (float) width / (float) numPoints;

        std::vector<float> topPoints;
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
                    float val = std::abs (buffer->getSample (c, s));
                    if (val > maxVal) maxVal = val;
                }
            }

            float targetHeight = midY - (maxVal * midY * 0.78f);
            topPoints.push_back (targetHeight);
        }

        for (size_t i = 1; i < topPoints.size(); ++i)
        {
            float xPrev = (i - 1) * xStep;
            float xCurr = i * xStep;
            float yPrev = topPoints[i - 1];
            float yCurr = topPoints[i];
            float cX = (xPrev + xCurr) / 2.0f;
            float cY = (yPrev + yCurr) / 2.0f;
            path.quadraticTo (xPrev, yPrev, cX, cY);
        }

        path.lineTo ((float) width, midY);

        for (size_t i = topPoints.size() - 1; i > 0; --i)
        {
            float xCurr = i * xStep;
            float xPrev = (i - 1) * xStep;
            float yCurr = midY + (midY - topPoints[i]);
            float yPrev = midY + (midY - topPoints[i - 1]);
            float cX = (xCurr + xPrev) / 2.0f;
            float cY = (yCurr + yPrev) / 2.0f;
            path.quadraticTo (xCurr, yCurr, cX, cY);
        }

        path.closeSubPath();

        juce::ColourGradient grad (juce::Colour (0xff2d6a4f), 0.0f, midY,
                                   juce::Colour (0xff52b788), (float) width * 0.75f, midY, false);
        g.setGradientFill (grad);
        g.fillPath (path);

        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (juce::Font ("Helvetica Neue", 9.0f, juce::Font::plain));
        g.drawText ("(Click waveform to play, hold & drag into project)", 
                    getLocalBounds().removeFromBottom (16), juce::Justification::centred);
    }

private:
    juce::AudioBuffer<float>* buffer = nullptr;
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
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2);
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
        g.setFont (juce::Font ("Helvetica Neue", 11.0f, juce::Font::bold));
        auto valText = juce::String ((int) slider.getValue()) + " st";
        g.drawText (valText, toX - radius, toY - 8.0f, radius * 2.0f, 16.0f, juce::Justification::centred);
    }
};

//==============================================================================
class InfoOverlay : public juce::Component
{
public:
    InfoOverlay (std::function<void()> closeCallback) : onClose (closeCallback)
    {
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.94f));

        auto area = getLocalBounds().reduced (24);
        g.setColour (juce::Colour (0xff52b788));
        g.setFont (juce::Font ("Helvetica Neue", 18.0f, juce::Font::bold));
        g.drawText ("FREESOUND MINI", area.removeFromTop (35), juce::Justification::centredLeft);

        g.setFont (juce::Font ("Helvetica Neue", 12.0f, juce::Font::plain));
        g.setColour (juce::Colours::lightgrey);

        juce::String text = 
            "Быстрый поиск звуков прямо в Ableton Live.\n\n"
            "1. Подключение API Token:\n"
            "   Получите бесплатный токен на freesound.org/apiv2/apply/.\n"
            "   Вставьте в поле 'Enter API Key'. Зелёная плашка [✔ API OK] подтвердит связь.\n\n"
            "2. Фильтры длины и каналов:\n"
            "   [All / Mono / Stereo] отбирает дорожки по количеству каналов.\n"
            "   Слайдер Max Sec ограничивает максимальную длину (например, до 3 сек для шотов).\n\n"
            "3. Предпрослушивание:\n"
            "   Семплы воспроизводятся сразу при выборе строки.\n"
            "   Нормализация [✔ Norm -6dB] масштабирует пик до -6 dBFS без клиппинга.\n"
            "   Ручка Pitch (-12 / +12) меняет высоту тона в реальном времени.\n"
            "   При переходе к следующему звуку Pitch сбрасывается в 0 st.\n\n"
            "4. Drag-and-Drop:\n"
            "   Зажмите ЛКМ на дисплее волны и перетащите файл в проект Ableton.\n"
            "   В дорожку встанет чистый оригинальный файл без изменений.\n\n"
            "Версия 1.1.0 (Concept 4.1 UI)";

        g.drawFittedText (text, area, juce::Justification::topLeft, 24);
    }

    void resized() override
    {
        closeButton.setBounds (getWidth() - 36, 12, 24, 24);
    }

private:
    std::function<void()> onClose;
    juce::TextButton closeButton { "✕" };
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
        setSize (360, 560);

        if (audioProcessor.sampleDirectory == juce::File())
        {
            audioProcessor.sampleDirectory = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                                .getChildFile ("FreesoundSamples");
        }
        audioProcessor.sampleDirectory.createDirectory();

        // API Bar
        tokenInput.setTextToShowWhenEmpty ("Enter API Key...", juce::Colours::grey);
        tokenInput.setText (audioProcessor.savedToken, false);
        tokenInput.onTextChange = [this] { 
            audioProcessor.savedToken = tokenInput.getText().trim(); 
            updateAPIStatus();
        };
        addAndMakeVisible (tokenInput);

        updateAPIStatus();
        addAndMakeVisible (apiStatusLabel);

        dirButton.onClick = [this] {
            fileChooser = std::make_unique<juce::FileChooser> ("Choose Sample Folder",
                                                                audioProcessor.sampleDirectory,
                                                                "*");
            auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
            fileChooser->launchAsync (flags, [this] (const juce::FileChooser& fc) {
                auto result = fc.getResult();
                if (result.isDirectory())
                {
                    audioProcessor.sampleDirectory = result;
                    statusLabel.setText ("Folder: " + result.getFileName(), juce::dontSendNotification);
                }
            });
        };
        addAndMakeVisible (dirButton);

        openFolderButton.onClick = [this] {
            audioProcessor.sampleDirectory.startAsProcess();
        };
        addAndMakeVisible (openFolderButton);

        infoButton.onClick = [this] {
            infoOverlay->setVisible (true);
        };
        addAndMakeVisible (infoButton);

        // Поиск
        searchInput.setTextToShowWhenEmpty ("Search sounds (ambient, vocal, 808)...", juce::Colours::grey);
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

        // Панель фильтров: Channels (All, Mono, Stereo)
        channelBox.addItem ("All Ch", 1);
        channelBox.addItem ("Mono", 2);
        channelBox.addItem ("Stereo", 3);
        channelBox.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (channelBox);

        // Панель фильтров: Duration Slider (от 0.5с до 60с)
        durationSlider.setRange (1.0, 60.0, 1.0);
        durationSlider.setValue (15.0);
        durationSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 18);
        durationSlider.setTextValueSuffix ("s");
        addAndMakeVisible (durationSlider);

        durationLabel.setText ("Max:", juce::dontSendNotification);
        durationLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (durationLabel);

        // Таблица
        table.setModel (this);
        table.getHeader().addColumn ("NAME", 1, 195);
        table.getHeader().addColumn ("DUR", 2, 65);
        table.getHeader().addColumn ("STATUS", 3, 60);
        table.setMultipleSelectionAllowed (false);
        addAndMakeVisible (table);

        // Волна
        addAndMakeVisible (waveformDisplay);

        // Транспорт
        playButton.onClick = [this] { audioProcessor.updatePlaybackSettings(); };
        addAndMakeVisible (playButton);

        stopButton.onClick = [this] { audioProcessor.stopPreview(); };
        addAndMakeVisible (stopButton);

        pitchSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pitchSlider.setRange (-12.0, 12.0, 1.0);
        pitchSlider.setValue (0.0);
        pitchSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pitchSlider.setLookAndFeel (&knobLookAndFeel);
        pitchSlider.onValueChange = [this] { audioProcessor.setPitchSemitones (pitchSlider.getValue()); };
        addAndMakeVisible (pitchSlider);

        normCheckbox.setButtonText ("Norm -6dB");
        normCheckbox.setToggleState (true, juce::dontSendNotification);
        normCheckbox.onClick = [this] { audioProcessor.setNormalization (normCheckbox.getToggleState()); };
        addAndMakeVisible (normCheckbox);

        volSlider.setSliderStyle (juce::Slider::LinearBar);
        volSlider.setRange (0.0, 1.0, 0.01);
        volSlider.setValue (0.8);
        volSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volSlider.onValueChange = [this] { audioProcessor.setGain ((float) volSlider.getValue()); };
        addAndMakeVisible (volSlider);

        volLabel.setText ("Vol", juce::dontSendNotification);
        volLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (volLabel);

        statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (statusLabel);

        infoOverlay = std::make_unique<InfoOverlay> ([this] { infoOverlay->setVisible (false); });
        infoOverlay->setVisible (false);
        addChildComponent (infoOverlay.get());
    }

    ~FreesoundMiniEditor() override
    {
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

        // Верхний тулбар
        auto topBar = area.removeFromTop (26);
        infoButton.setBounds (topBar.removeFromRight (26));
        topBar.removeFromRight (4);
        openFolderButton.setBounds (topBar.removeFromRight (26));
        topBar.removeFromRight (4);
        dirButton.setBounds (topBar.removeFromRight (52));
        apiStatusLabel.setBounds (topBar.removeFromLeft (95));
        topBar.removeFromLeft (4);
        tokenInput.setBounds (topBar);

        area.removeFromTop (6);

        // Строка поиска
        auto searchRow = area.removeFromTop (28);
        searchButton.setBounds (searchRow.removeFromRight (55));
        searchRow.removeFromRight (4);
        searchInput.setBounds (searchRow);

        area.removeFromTop (6);

        // Ряд фильтров: Длительность и Каналы
        auto filterRow = area.removeFromTop (24);
        channelBox.setBounds (filterRow.removeFromLeft (85));
        filterRow.removeFromLeft (8);
        durationLabel.setBounds (filterRow.removeFromLeft (35));
        durationSlider.setBounds (filterRow);

        area.removeFromTop (8);

        // Нижняя панель плеера
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
        normCheckbox.setBounds (bArea.removeFromLeft (78));

        bArea.removeFromLeft (6);
        volLabel.setBounds (bArea.removeFromLeft (20));
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
            if (rowNumber < 0 || rowNumber >= static_cast<int> (results.size())) return;
            item = results[static_cast<size_t> (rowNumber)];
        }

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font ("Helvetica Neue", 11.5f, juce::Font::plain));

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
        if (lastRowSelected >= 0)
        {
            pitchSlider.setValue (0.0);
            triggerLoadAndPlay (lastRowSelected);
        }
    }

    void cellClicked (int rowNumber, int, const juce::MouseEvent&) override
    {
        if (rowNumber >= 0)
        {
            pitchSlider.setValue (0.0);
            triggerLoadAndPlay (rowNumber);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (!waveformDisplay.getBounds().contains (e.getPosition())) return;

        int row = table.getSelectedRow();
        if (row < 0) return;

        juce::File fileToDrag;
        {
            const juce::ScopedLock sl (resultsLock);
            if (row < static_cast<int> (results.size()))
                fileToDrag = results[static_cast<size_t> (row)].localFile;
        }

        if (fileToDrag.existsAsFile())
        {
            juce::StringArray files;
            files.add (fileToDrag.getFullPathName());
            performExternalDragDropOfFiles (files, false);
        }
    }

private:
    void updateAPIStatus()
    {
        if (audioProcessor.savedToken.length() > 5)
        {
            apiStatusLabel.setText ("✔ API OK", juce::dontSendNotification);
            apiStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff52b788));
        }
        else
        {
            apiStatusLabel.setText ("✕ NO API", juce::dontSendNotification);
            apiStatusLabel.setColour (juce::Label::textColourId, juce::Colours::red.withAlpha (0.8f));
        }
    }

    void triggerLoadAndPlay (int index)
    {
        juce::File existing;
        {
            const juce::ScopedLock sl (resultsLock);
            if (index < 0 || index >= static_cast<int> (results.size())) return;
            existing = results[static_cast<size_t> (index)].localFile;
        }

        if (existing.existsAsFile())
        {
            audioProcessor.loadPreviewFile (existing);
            waveformDisplay.setBuffer (audioProcessor.getCachedBuffer());
        }
        else
        {
            downloadAndPlay (index);
        }
    }

    void run() override
    {
        auto token = tokenInput.getText().trim();
        if (token.isEmpty() || currentQuery.isEmpty()) return;

        // Построение строки фильтра Freesound API
