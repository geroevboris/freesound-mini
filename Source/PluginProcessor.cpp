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
            normalizationGain = 0.501187f / loadedFilePeak; // -6 dBFS

        double speedRatio = std::pow (2.0, pitchSemitones / 12.0);

        auto* memoryReader = new juce::AudioBufferReader (&loadedBuffer, originalSampleRate, true);
        readerSource = std::make_unique<juce::AudioFormatReaderSource> (memoryReader, true);
        
        transportSource.setSource (readerSource.get(), 0, nullptr, originalSampleRate * speedRatio);
        transportSource.setGain (previewGain * normalizationGain);
    }

    void playPreview()
    {
        if (readerSource != nullptr)
        {
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
        updatePlaySource();
    }

    void setPitchSemitones (double semitones)
    {
        pitchSemitones = semitones;
        updatePlaySource();
    }

    void setNormalization (bool shouldNormalize)
    {
        useNormalization = shouldNormalize;
        updatePlaySource();
    }

    juce::AudioBuffer<float>* getLoadedBuffer() { return &loadedBuffer; }

    juce::String savedToken;
    juce::File sampleDirectory;

private:
    double currentSampleRate = 44100.0;
    double originalSampleRate = 44100.0;
    float previewGain = 0.8f;
    double pitchSemitones = 0.0;
    bool useNormalization = true;
    float loadedFilePeak = 1.0f;

    juce::AudioBuffer<float> loadedBuffer;
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
        // Матовый черный фон с сильным скруглением (Concept 4)
        g.setColour (juce::Colour (0xff141517));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f);

        if (buffer == nullptr || buffer->getNumSamples() == 0)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.6f));
            g.setFont (juce::Font ("Helvetica Neue", 12.0f, juce::Font::plain));
            g.drawText ("No Audio Loaded", getLocalBounds().withSize (getWidth(), getHeight() - 20), juce::Justification::centred);
            g.setFont (juce::Font ("Helvetica Neue", 10.0f, juce::Font::plain));
            g.drawText ("(Click waveform to play, hold & drag into project)", 
                        getLocalBounds().removeFromBottom (22), juce::Justification::centred);
            return;
        }

        auto width = getWidth();
        auto height = getHeight();
        auto midY = height / 2.0f;
        auto numSamples = buffer->getNumSamples();
        auto numChannels = buffer->getNumChannels();

        if (numSamples <= 0) return;

        // Генерация плавной "холмистой" волны через Path (без колючек)
        juce::Path path;
        path.startNewSubPath (0.0f, midY);

        const int numPoints = std::min (width, 180); // Меньше точек — более плавная дюна
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

            // Ограничиваем резкость холма
            float targetHeight = midY - (maxVal * midY * 0.75f);
            topPoints.push_back (targetHeight);
        }

        // Рисуем верхний плавный контур
        for (size_t i = 1; i < topPoints.size(); ++i)
        {
            float x1 = (i - 1) * xStep;
            float x2 = i * xStep;
            float y1 = topPoints[i - 1];
            float y2 = topPoints[i];
            
            // Используем кривые Безье для идеальной обтекаемости "холмика"
            path.quadraticToFit (x1, y1, (x1 + x2) / 2.0f, (y1 + y2) / 2.0f, x2, y2);
        }

        // Спускаемся и рисуем симметричный нижний плавный контур назад
        path.lineTo (width, midY);
        for (size_t i = topPoints.size() - 1; i > 0; --i)
        {
            float x1 = i * xStep;
            float x2 = (i - 1) * xStep;
            // Зеркально отражаем Y вниз
            float y1 = midY + (midY - topPoints[i]);
            float y2 = midY + (midY - topPoints[i - 1]);
            
            path.quadraticToFit (x1, y1, (x1 + x2) / 2.0f, (y1 + y2) / 2.0f, x2, y2);
        }
        path.closeSubPath();

        // Градиент благородного лесного/шалфейного зеленого (Concept 4.1 Green)
        juce::ColourGradient grad (juce::Colour (0xff2d6a4f), 0.0f, midY,
                                   juce::Colour (0xff52b788), (float) width * 0.7f, midY, false);
        g.setGradientFill (grad);
        g.fillPath (path);

        // Информационная плашка
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
        setColour (juce::Slider::thumbColourId, juce::Colour (0xff52b788)); // Шалфейный
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff1a1c1e));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, const float startAngle, const float endAngle,
                           juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3);
        auto radius = std::min (bounds.getWidth(), bounds.getHeight()) / 2.0f;
        auto toX = bounds.getCentreX();
        auto toY = bounds.getCentreY();
        auto arcRadius = radius - 4.0f;

        // Заднее кольцо
        g.setColour (juce::Colour (0xff1a1c1e));
        g.drawEllipse (toX - arcRadius, toY - arcRadius, arcRadius * 2.0f, arcRadius * 2.0f, 4.0f);

        // Заполненный путь (Pitch level)
        auto angle = startAngle + sliderPos * (endAngle - startAngle);
        juce::Path activeArc;
        activeArc.addCentredArc (toX, toY, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
        
        g.setColour (juce::Colour (0xff52b788));
        g.strokePath (activeArc, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Центральная часть ручки
        g.setColour (juce::Colour (0xff0d0e10));
        g.fillEllipse (toX - (radius - 8.0f), toY - (radius - 8.0f), (radius - 8.0f) * 2.0f, (radius - 8.0f) * 2.0f);

        // Отметка направления
        juce::Path p;
        auto pointerLength = radius - 8.0f;
        auto pointerThickness = 2.5f;
        p.addRectangle (-pointerThickness * 0.5f, -pointerLength, pointerThickness, pointerLength * 0.6f);
        p.applyTransform (juce::AffineTransform::rotation (angle).translated (toX, toY));
        g.setColour (juce::Colour (0xff52b788));
        g.fillPath (p);

        // Текст значения в центре
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font ("Helvetica Neue", 11.5f, juce::Font::bold));
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
            "Быстрый доступ к звукам прямо в Ableton Live.\n\n"
            "1. Как подключить API Token:\n"
            "   Перейдите на freesound.org/apiv2/apply/ и создайте бесплатный ключ.\n"
            "   Вставьте его в поле ввода 'Enter API Key'. При успешной настройке\n"
            "   в левом верхнем углу загорится зеленая галочка [✔ API OK].\n\n"
            "2. Выбор папки загрузки:\n"
            "   Нажмите кнопку [Folder], чтобы изменить папку сохранения семплов.\n"
            "   Нажмите [↗], чтобы быстро открыть эту папку в Finder.\n\n"
            "3. Предпрослушивание:\n"
            "   Семплы воспроизводятся автоматически при выборе строки.\n"
            "   Включайте и отключайте [✔] Norm -6dB, чтобы выровнять громкость.\n"
            "   Крутилка Pitch (-12 / +12) позволит подобрать высоту тона в реальном времени.\n"
            "   При переходе к новому звуку Pitch автоматически сбрасывается в 0 полутонов.\n\n"
            "4. Экспорт (Drag-and-Drop):\n"
            "   Зажмите левую кнопку мыши ПРЯМО НА ФОРМЕ ВОЛНЫ и потащите её\n"
            "   в окно аранжировки Ableton Live. Туда упадет оригинальный неискаженный семпл.\n\n"
            "Версия 1.0.0 (Concept 4.1 Elegant Rounded UI)";

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
        setSize (340, 520); // Вертикальный компактный макет (image_15)

        if (audioProcessor.sampleDirectory == juce::File())
        {
            audioProcessor.sampleDirectory = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                                .getChildFile ("FreesoundSamples");
        }
        audioProcessor.sampleDirectory.createDirectory();

        // Поле ввода API
        tokenInput.setTextToShowWhenEmpty ("Enter API Key...", juce::Colours::grey);
        tokenInput.setText (audioProcessor.savedToken, false);
        tokenInput.onTextChange = [this] { 
            audioProcessor.savedToken = tokenInput.getText().trim(); 
            updateAPIStatus();
        };
        addAndMakeVisible (tokenInput);

        // Статус API
        updateAPIStatus();
        addAndMakeVisible (apiStatusLabel);

        // Кнопка Folders & Actions
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

        // Таблица звуков
        table.setModel (this);
        table.getHeader().addColumn ("NAME", 1, 190);
        table.getHeader().addColumn ("DURATION", 2, 70);
        table.getHeader().addColumn ("STATUS", 3, 60);
        table.setMultipleSelectionAllowed (false);
        addAndMakeVisible (table);

        // Скругленная волна
        addAndMakeVisible (waveformDisplay);

        // Плеер
        playButton.onClick = [this] { audioProcessor.playPreview(); };
        addAndMakeVisible (playButton);

        stopButton.onClick = [this] { audioProcessor.stopPreview(); };
        addAndMakeVisible (stopButton);

        // Кастомная крутилка Pitch
        pitchSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pitchSlider.setRange (-12.0, 12.0, 1.0);
        pitchSlider.setValue (0.0);
        pitchSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pitchSlider.setLookAndFeel (&knobLookAndFeel);
        pitchSlider.onValueChange = [this] { audioProcessor.setPitchSemitones (pitchSlider.getValue()); };
        addAndMakeVisible (pitchSlider);

        // Нормализация
        normCheckbox.setButtonText ("Norm -6dB");
        normCheckbox.setToggleState (true, juce::dontSendNotification);
        normCheckbox.onClick = [this] { audioProcessor.setNormalization (normCheckbox.getToggleState()); };
        addAndMakeVisible (normCheckbox);

        // Громкость
        volSlider.setSliderStyle (juce::Slider::LinearBar);
        volSlider.setRange (0.0, 1.0, 0.01);
        volSlider.setValue (0.8);
        volSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volSlider.onValueChange = [this] { audioProcessor.setGain ((float) volSlider.getValue()); };
        addAndMakeVisible (volSlider);

        volLabel.setText ("Vol", juce::dontSendNotification);
        volLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (volLabel);

        // Информационная строка
        statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (statusLabel);

        // Модальный оверлей инструкции
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
        // Ультратемный матовый глубокий черный (Concept 4.1)
        g.fillAll (juce::Colour (0xff0a0b0d));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        // Top API Bar
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

        // Search Bar
        auto searchRow = area.removeFromTop (28);
        searchButton.setBounds (searchRow.removeFromRight (60));
        searchRow.removeFromRight (4);
        searchInput.setBounds (searchRow);

        area.removeFromTop (8);

        // Waveform Display (Нижний центр)
        auto bottomControl = area.removeFromBottom (46);
        area.removeFromBottom (6);
        waveformDisplay.setBounds (area.removeFromBottom (75));

        area.removeFromBottom (6);
        statusLabel.setBounds (area.removeFromBottom (16));

        // Table
        table.setBounds (area);

        // Bottom Player Bar
        auto bArea = bottomControl;
        playButton.setBounds (bArea.removeFromLeft (38));
        bArea.removeFromLeft (4);
        stopButton.setBounds (bArea.removeFromLeft (38));
        
        bArea.removeFromLeft (12);
        pitchSlider.setBounds (bArea.removeFromLeft (64));

        bArea.removeFromLeft (12);
        normCheckbox.setBounds (bArea.removeFromLeft (78));
        
        bArea.removeFromLeft (8);
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
            g.fillAll (juce::Colour (0xff1f2a24)); // Шалфейный оттенок на селекте
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
            g.drawText (juce::String (item.duration, 1) + " s", 0, 0, width, height, juce::Justification::centred);
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
            pitchSlider.setValue (0.0); // Автосброс Pitch на 0 st
            triggerLoadAndPlay (lastRowSelected);
        }
    }

    void cellClicked (int rowNumber, int, const juce::MouseEvent&) override
    {
        if (rowNumber >= 0)
        {
            pitchSlider.setValue (0.0); // Автосброс Pitch на 0 st
            triggerLoadAndPlay (rowNumber);
        }
    }

    // Нативный Drag-and-Drop по жесту таскания мыши из Waveform
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
            waveformDisplay.setBuffer (audioProcessor.getLoadedBuffer());
            audioProcessor.playPreview();
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

        auto urlStr = "https://freesound.org/apiv2/search/text/?query="
                    + juce::URL::addEscapeChars (currentQuery, true)
                    + "&filter=" + juce::URL::addEscapeChars ("license:\"Creative Commons 0\"", true)
                    + "&token=" + token
                    + "&fields=id,name,duration,previews&page_size=40";

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

                    auto sanitizedName = juce::File::createLegalFileName (r.name);
                    auto target = audioProcessor.sampleDirectory.getChildFile (juce::String (r.id) + "_" + sanitizedName + ".mp3");
                    if (target.existsAsFile())
                        r.localFile = target;

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
        statusLabel.setText ("Downloading...", juce::dontSendNotification);

        juce::Thread::launch ([this, index]() {
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

            auto sanitized = juce::File::createLegalFileName (name);
            auto local = audioProcessor.sampleDirectory.getChildFile (juce::String (soundId) + "_" + sanitized + ".mp3");

            if (!local.existsAsFile() && previewUrl.isNotEmpty())
            {
                juce::URL url (previewUrl);
                juce::MemoryBlock mb;
                if (url.readEntireBinaryStream (mb, 12000))
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
                audioProcessor.loadPreviewFile (local);
                waveformDisplay.setBuffer (audioProcessor.getLoadedBuffer());
                audioProcessor.playPreview();
                statusLabel.setText ("Ready! Drag waveform to track.", juce::dontSendNotification);
            });
        });
    }

    FreesoundMiniAudioProcessor& audioProcessor;

    juce::TextEditor tokenInput;
    juce::Label apiStatusLabel;
    juce::TextEditor searchInput;
    juce::TextButton searchButton { "🔍" };
    
    juce::TextButton dirButton { "Folder" };
    juce::TextButton openFolderButton { "↗" };
    juce::TextButton infoButton { "ℹ" };

    juce::TableListBox table;
    WaveformDisplay waveformDisplay;

    juce::TextButton playButton { "▶" };
    juce::TextButton stopButton { "⏹" };
    
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
    juce::String currentQuery;

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
