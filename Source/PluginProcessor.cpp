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

    void playOrResume()
    {
        if (readerSource != nullptr)
        {
            double len = transportSource.getLengthInSeconds();
            double pos = transportSource.getCurrentPosition();
            if (len > 0.0 && pos >= len - 0.03)
                transportSource.setPosition (0.0);

            transportSource.start();
        }
        else if (currentFile.existsAsFile())
        {
            rebuildTransport (0.0, true);
        }
    }

    void pausePreview()
    {
        transportSource.stop();
    }

    void stopPreview()
    {
        transportSource.stop();
        transportSource.setPosition (0.0);
    }

    void seekToRelativePosition (float proportion)
    {
        proportion = juce::jlimit (0.0f, 1.0f, proportion);

        if (readerSource == nullptr && currentFile.existsAsFile())
            rebuildTransport (0.0, false);

        double length = transportSource.getLengthInSeconds();
        if (length > 0.0)
        {
            transportSource.setPosition (length * (double) proportion);
            transportSource.start();
        }
    }

    float getRelativePlayheadPosition() const
    {
        double length = transportSource.getLengthInSeconds();
        if (length <= 0.0)
            return 0.0f;
        return (float) juce::jlimit (0.0, 1.0, transportSource.getCurrentPosition() / length);
    }

    bool isAudioPlaying() const
    {
        return transportSource.isPlaying();
    }

    void setGain (float g)
    {
        previewGain = g;
        updateGainInternal();
    }

    void setPitchSemitones (double semitones)
    {
        if (std::abs (pitchSemitones - semitones) < 0.001)
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
        float normGain = useNormalization ? (0.501187f / loadedFilePeak) : 1.0f;
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
class IconButton : public juce::Button
{
public:
    enum Type { Play, Pause, Stop, Search, Folder, OpenExternal, Info, Close };

    explicit IconButton (Type t) : juce::Button (""), iconType (t) {}

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);

        juce::Colour bg = juce::Colour (0xff16181d);
        if (isButtonDown)      bg = juce::Colour (0xff24332c);
        else if (isMouseOver)  bg = juce::Colour (0xff1e2229);

        g.setColour (bg);
        g.fillRoundedRectangle (b, 7.0f);

        g.setColour (isMouseOver ? juce::Colour (0xff52b788).withAlpha (0.5f) : juce::Colour (0xff262930));
        g.drawRoundedRectangle (b, 7.0f, 1.0f);

        juce::Colour iconCol = isButtonDown ? juce::Colour (0xff52b788)
                             : (isMouseOver ? juce::Colour (0xff74c69d) : juce::Colours::lightgrey);
        g.setColour (iconCol);

        auto cx = b.getCentreX();
        auto cy = b.getCentreY();
        auto s = std::min (b.getWidth(), b.getHeight()) * 0.28f;

        juce::Path p;
        switch (iconType)
        {
            case Play:
                p.addTriangle (cx - s * 0.7f, cy - s, cx - s * 0.7f, cy + s, cx + s * 1.1f, cy);
                g.fillPath (p);
                break;

            case Pause:
                g.fillRoundedRectangle (cx - s * 0.85f, cy - s, s * 0.55f, s * 2.0f, 1.5f);
                g.fillRoundedRectangle (cx + s * 0.30f, cy - s, s * 0.55f, s * 2.0f, 1.5f);
                break;

            case Stop:
                g.fillRoundedRectangle (cx - s * 0.85f, cy - s * 0.85f, s * 1.7f, s * 1.7f, 2.0f);
                break;

            case Search:
                p.addEllipse (cx - s * 0.9f, cy - s * 0.9f, s * 1.4f, s * 1.4f);
                p.startNewSubPath (cx + s * 0.3f, cy + s * 0.3f);
                p.lineTo (cx + s * 1.0f, cy + s * 1.0f);
                g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;

            case Folder:
                p.startNewSubPath (cx - s * 1.1f, cy - s * 0.7f);
                p.lineTo (cx - s * 0.2f, cy - s * 0.7f);
                p.lineTo (cx + s * 0.1f, cy - s * 0.35f);
                p.lineTo (cx + s * 1.1f, cy - s * 0.35f);
                p.lineTo (cx + s * 1.1f, cy + s * 0.8f);
                p.lineTo (cx - s * 1.1f, cy + s * 0.8f);
                p.closeSubPath();
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;

            case OpenExternal:
                p.startNewSubPath (cx + s * 0.1f, cy - s * 0.8f);
                p.lineTo (cx + s * 0.9f, cy - s * 0.8f);
                p.lineTo (cx + s * 0.9f, cy);
                p.startNewSubPath (cx + s * 0.9f, cy - s * 0.8f);
                p.lineTo (cx - s * 0.3f, cy + s * 0.4f);
                p.startNewSubPath (cx, cy - s * 0.5f);
                p.lineTo (cx - s * 0.8f, cy - s * 0.5f);
                p.lineTo (cx - s * 0.8f, cy + s * 0.8f);
                p.lineTo (cx + s * 0.5f, cy + s * 0.8f);
                p.lineTo (cx + s * 0.5f, cy + s * 0.1f);
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;

            case Info:
                g.drawEllipse (cx - s, cy - s, s * 2.0f, s * 2.0f, 1.5f);
                g.fillEllipse (cx - 1.2f, cy - s * 0.55f, 2.4f, 2.4f);
                g.fillRoundedRectangle (cx - 1.0f, cy - s * 0.1f, 2.0f, s * 0.75f, 1.0f);
                break;

            case Close:
                p.startNewSubPath (cx - s * 0.7f, cy - s * 0.7f);
                p.lineTo (cx + s * 0.7f, cy + s * 0.7f);
                p.startNewSubPath (cx + s * 0.7f, cy - s * 0.7f);
                p.lineTo (cx - s * 0.7f, cy + s * 0.7f);
                g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
        }
    }

private:
    Type iconType;
};

//==============================================================================
class WaveformDisplay : public juce::Component,
                        private juce::Timer
{
public:
    std::function<void(float)> onSeekRequested;
    std::function<void()> onDragged;
    std::function<float()> getPlayheadPos;

    WaveformDisplay()
    {
        startTimerHz (30);
    }

    ~WaveformDisplay() override
    {
        stopTimer();
    }

    void setWaveformData (const juce::AudioBuffer<float>& newBuffer)
    {
        localBuffer.makeCopyOf (newBuffer);
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStarted = false;
        if (e.mods.isLeftButtonDown() && localBuffer.getNumSamples() > 0)
        {
            float prop = juce::jlimit (0.0f, 1.0f, (float) e.x / (float) std::max (1, getWidth()));
            lastCursorProp = prop;
            if (onSeekRequested)
                onSeekRequested (prop);
            repaint();
        }
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

    void mouseUp (const juce::MouseEvent&) override
    {
        dragStarted = false;
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff121418));
        g.fillRoundedRectangle (b, 12.0f);

        g.setColour (juce::Colour (0xff22262e));
        g.drawRoundedRectangle (b.reduced (0.5f), 12.0f, 1.0f);

        if (localBuffer.getNumSamples() == 0)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.55f));
            g.drawText ("No Audio Loaded", getLocalBounds().withTrimmedBottom (18), juce::Justification::centred);
            g.drawText ("(Click anywhere to play/seek, hold & drag to Ableton)",
                        getLocalBounds().removeFromBottom (20), juce::Justification::centred);
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

        const int numPoints = std::min (width, 180);
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

            float targetHeight = midY - (maxVal * midY * 0.76f);
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

        juce::ColourGradient grad (juce::Colour (0xff2d6a4f), 0.0f, midY - height * 0.35f,
                                   juce::Colour (0xff52b788), (float) width * 0.75f, midY + height * 0.35f, false);
        g.setGradientFill (grad);
        g.fillPath (path);

        // Отрисовка живого курсора воспроизведения
        float cursorX = lastCursorProp * (float) width;
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawVerticalLine ((int) std::round (cursorX), 4.0f, (float) height - 18.0f);
        g.fillEllipse (cursorX - 3.0f, 3.0f, 6.0f, 6.0f);

        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.drawText ("(Click to seek, hold & drag into project)",
                    getLocalBounds().removeFromBottom (16), juce::Justification::centred);
    }

private:
    void timerCallback() override
    {
        if (getPlayheadPos && localBuffer.getNumSamples() > 0)
        {
            float newProp = getPlayheadPos();
            if (std::abs (newProp - lastCursorProp) > 0.001f)
            {
                lastCursorProp = newProp;
                repaint();
            }
        }
    }

    juce::AudioBuffer<float> localBuffer;
    float lastCursorProp = 0.0f;
    bool dragStarted = false;
};

//==============================================================================
class ModernLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ModernLookAndFeel()
    {
        setColour (juce::Slider::thumbColourId, juce::Colour (0xff52b788));
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff1a1c1e));
        setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14161a));
        setColour (juce::TextEditor::textColourId, juce::Colours::white);
        setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff232730));
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff52b788));
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff14161a));
        setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff232730));
        setColour (juce::ComboBox::textColourId, juce::Colours::lightgrey);
        setColour (juce::ComboBox::arrowColourId, juce::Colour (0xff52b788));
        setColour (juce::TableHeaderComponent::backgroundColourId, juce::Colour (0xff14161a));
        setColour (juce::TableHeaderComponent::textColourId, juce::Colours::grey);
        setColour (juce::TableHeaderComponent::outlineColourId, juce::Colour (0xff1f2229));
        setColour (juce::TableListBox::backgroundColourId, juce::Colour (0xff0f1114));
        setColour (juce::TableListBox::outlineColourId, juce::Colour (0xff1f2229));
        setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff52b788));
    }

    void fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& textEditor) override
    {
        g.setColour (textEditor.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (0.0f, 0.0f, (float) width, (float) height, 7.0f);
    }

    void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& textEditor) override
    {
        if (textEditor.isEnabled())
        {
            auto col = textEditor.hasKeyboardFocus (true)
                     ? textEditor.findColour (juce::TextEditor::focusedOutlineColourId)
                     : textEditor.findColour (juce::TextEditor::outlineColourId);
            g.setColour (col);
            g.drawRoundedRectangle (0.5f, 0.5f, (float) width - 1.0f, (float) height - 1.0f, 7.0f, 1.0f);
        }
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
        auto valText = juce::String ((int) std::round (slider.getValue())) + " st";
        g.drawText (valText, (int) (toX - radius), (int) (toY - 8.0f), (int) (radius * 2.0f), 16, juce::Justification::centred);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style == juce::Slider::TwoValueHorizontal)
        {
            auto trackY = (float) y + (float) height * 0.5f;
            float trackH = 4.0f;

            g.setColour (juce::Colour (0xff1a1d24));
            g.fillRoundedRectangle ((float) x, trackY - trackH * 0.5f, (float) width, trackH, 2.0f);

            g.setColour (juce::Colour (0xff52b788));
            float activeW = std::max (2.0f, maxSliderPos - minSliderPos);
            g.fillRoundedRectangle (minSliderPos, trackY - trackH * 0.5f, activeW, trackH, 2.0f);

            float r = 6.0f;
            g.setColour (juce::Colours::white);
            g.fillEllipse (minSliderPos - r, trackY - r, r * 2.0f, r * 2.0f);
            g.fillEllipse (maxSliderPos - r, trackY - r, r * 2.0f, r * 2.0f);

            g.setColour (juce::Colour (0xff52b788));
            g.drawEllipse (minSliderPos - r, trackY - r, r * 2.0f, r * 2.0f, 2.0f);
            g.drawEllipse (maxSliderPos - r, trackY - r, r * 2.0f, r * 2.0f, 2.0f);
            return;
        }

        if (style == juce::Slider::LinearBar)
        {
            auto b = juce::Rectangle<int> (x, y, width, height).toFloat();
            g.setColour (juce::Colour (0xff16181d));
            g.fillRoundedRectangle (b, 5.0f);

            float fillW = juce::jlimit (0.0f, b.getWidth(), sliderPos - (float) x);
            g.setColour (juce::Colour (0xff52b788).withAlpha (0.75f));
            g.fillRoundedRectangle (b.withWidth (fillW), 5.0f);
            return;
        }

        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
};

//==============================================================================
class InfoOverlay : public juce::Component
{
public:
    explicit InfoOverlay (std::function<void()> closeCallback) : onClose (std::move (closeCallback))
    {
        setInterceptsMouseClicks (true, true);
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        // Перехватываем клики, чтобы они не проваливались в таблицу под оверлеем
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0b0d).withAlpha (0.95f));

        auto card = getLocalBounds().reduced (18).toFloat();
        g.setColour (juce::Colour (0xff14161a));
        g.fillRoundedRectangle (card, 12.0f);
        g.setColour (juce::Colour (0xff52b788).withAlpha (0.4f));
        g.drawRoundedRectangle (card, 12.0f, 1.0f);

        auto area = getLocalBounds().reduced (32);
        g.setColour (juce::Colour (0xff52b788));
        g.drawText ("FREESOUND MINI — GUIDE", area.removeFromTop (32), juce::Justification::centredLeft);

        g.setColour (juce::Colours::lightgrey);

        juce::String text =
            "1. API Key:\n"
            "   Get your free key at freesound.org/apiv2/apply/\n"
            "   Paste it into the top bar. Green [✔ API OK] confirms it's saved.\n\n"
            "2. Duration Range (Min — Max up to Infinity):\n"
            "   Use the dual-thumb slider to cut both too-short and too-long sounds.\n"
            "   Up to 60s it scales finely; past 1m it grows geometrically up to Infinity (∞).\n\n"
            "3. Infinite Scroll:\n"
            "   Scroll down the results table — new pages load automatically.\n\n"
            "4. Waveform Seeking & Drag-and-Drop:\n"
            "   Click anywhere on the green waveform to jump & play from that spot.\n"
            "   Hold & drag from the waveform directly into Ableton Live.";

        g.drawFittedText (text, area, juce::Justification::topLeft, 24);
    }

    void resized() override
    {
        closeButton.setBounds (getWidth() - 52, 26, 26, 26);
    }

private:
    std::function<void()> onClose;
    IconButton closeButton { IconButton::Close };
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
        setLookAndFeel (&modernLF);

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
            {
                infoOverlay->setBounds (getLocalBounds());
                infoOverlay->setVisible (true);
                infoOverlay->toFront (true);
            }
        };
        addAndMakeVisible (infoButton);

        searchInput.setTextToShowWhenEmpty ("Search sounds (808, snare, ambient)...", juce::Colours::grey);
        searchInput.onReturnKey = [this] { startNewSearch(); };
        addAndMakeVisible (searchInput);

        searchButton.onClick = [this] { startNewSearch(); };
        addAndMakeVisible (searchButton);

        channelBox.addItem ("All Ch", 1);
        channelBox.addItem ("Mono", 2);
        channelBox.addItem ("Stereo", 3);
        channelBox.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (channelBox);

        durRangeSlider.setSliderStyle (juce::Slider::TwoValueHorizontal);
        durRangeSlider.setRange (0.0, 100.0, 0.1);
        durRangeSlider.setMinAndMaxValues (0.0, 42.0, juce::dontSendNotification); // ~0.0s .. 30s по умолчанию
        durRangeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        durRangeSlider.onValueChange = [this] {
            if (durRangeSlider.getMinValue() > durRangeSlider.getMaxValue())
                durRangeSlider.setMinValue (durRangeSlider.getMaxValue(), juce::dontSendNotification);
            updateDurationLabel();
        };
        addAndMakeVisible (durRangeSlider);

        durValueLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        durValueLabel.setJustificationType (juce::Justification::centredRight);
        updateDurationLabel();
        addAndMakeVisible (durValueLabel);

        table.setModel (this);
        table.getHeader().addColumn ("NAME", 1, 210);
        table.getHeader().addColumn ("DUR", 2, 65);
        table.getHeader().addColumn ("STATUS", 3, 65);
        table.setMultipleSelectionEnabled (false);
        addAndMakeVisible (table);

        waveformDisplay.onSeekRequested = [this] (float proportion) {
            proc.seekToRelativePosition (proportion);
        };
        waveformDisplay.onDragged = [this] {
            startExternalDrag();
        };
        waveformDisplay.getPlayheadPos = [this] {
            return proc.getRelativePlayheadPosition();
        };
        addAndMakeVisible (waveformDisplay);

        playButton.onClick = [this] { proc.playOrResume(); };
        addAndMakeVisible (playButton);

        pauseButton.onClick = [this] { proc.pausePreview(); };
        addAndMakeVisible (pauseButton);

        stopButton.onClick = [this] { proc.stopPreview(); };
        addAndMakeVisible (stopButton);

        pitchSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pitchSlider.setRange (-12.0, 12.0, 1.0);
        pitchSlider.setValue (0.0);
        pitchSlider.setDoubleClickReturnValue (true, 0.0);
        pitchSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
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

        // Важно: setResizable и setSize вызываются строго в конце конструктора, когда все компоненты созданы
        setResizable (true, true);
        setResizeLimits (350, 520, 1000, 1200);
        setSize (380, 600);
    }

    ~FreesoundMiniEditor() override
    {
        signalThreadShouldExit();
        stopThread (4000);
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0b0d));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        // Верхняя панель: статус API, ключ и иконки папок/инфо
        auto topBar = area.removeFromTop (28);
        infoButton.setBounds (topBar.removeFromRight (28));
        topBar.removeFromRight (4);
        openFolderButton.setBounds (topBar.removeFromRight (28));
        topBar.removeFromRight (4);
        dirButton.setBounds (topBar.removeFromRight (28));
        topBar.removeFromRight (6);
        apiStatusLabel.setBounds (topBar.removeFromLeft (78));
        topBar.removeFromLeft (4);
        tokenInput.setBounds (topBar);

        area.removeFromTop (8);

        // Строка поиска
        auto searchRow = area.removeFromTop (30);
        searchButton.setBounds (searchRow.removeFromRight (34));
        searchRow.removeFromRight (6);
        searchInput.setBounds (searchRow);

        area.removeFromTop (6);

        // Панель фильтров: Каналы + Двухсторонний слайдер длительности (Min..Max..∞)
        auto filterRow = area.removeFromTop (24);
        channelBox.setBounds (filterRow.removeFromLeft (82));
        filterRow.removeFromLeft (8);
        durValueLabel.setBounds (filterRow.removeFromRight (105));
        filterRow.removeFromRight (4);
        durRangeSlider.setBounds (filterRow);

        area.removeFromTop (8);

        // Нижняя панель транспорта
        auto bottomControl = area.removeFromBottom (48);
        area.removeFromBottom (6);

        // Адаптивная высота волны при растягивании окна
        int waveHeight = juce::jlimit (78, 170, getHeight() / 6);
        waveformDisplay.setBounds (area.removeFromBottom (waveHeight));

        area.removeFromBottom (4);
        statusLabel.setBounds (area.removeFromBottom (18));

        // Таблица результатов
        table.setBounds (area);
        int nameColW = std::max (140, table.getWidth() - 135);
        table.getHeader().setColumnWidth (1, nameColW);

        // Кнопки управления и ручка Pitch
        auto bArea = bottomControl;
        playButton.setBounds (bArea.removeFromLeft (32).withSizeKeepingCentre (32, 32));
        bArea.removeFromLeft (4);
        pauseButton.setBounds (bArea.removeFromLeft (32).withSizeKeepingCentre (32, 32));
        bArea.removeFromLeft (4);
        stopButton.setBounds (bArea.removeFromLeft (32).withSizeKeepingCentre (32, 32));

        bArea.removeFromLeft (8);
        pitchSlider.setBounds (bArea.removeFromLeft (56));

        bArea.removeFromLeft (6);
        normCheckbox.setBounds (bArea.removeFromLeft (82));

        bArea.removeFromLeft (4);
        volLabel.setBounds (bArea.removeFromLeft (22));
        volSlider.setBounds (bArea.reduced (0, 14));

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
            g.fillAll (juce::Colour (0xff1d2b24));
        else
            g.fillAll (juce::Colour (0xff101216));
    }

    void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool) override
    {
        SoundResult item;
        bool shouldTriggerNextPage = false;

        {
            const juce::ScopedLock sl (resultsLock);
            int total = static_cast<int> (results.size());
            if (rowNumber < 0 || rowNumber >= total)
                return;
            item = results[static_cast<size_t> (rowNumber)];

            if (rowNumber >= total - 5 && !isLoadingMore && hasMorePages)
                shouldTriggerNextPage = true;
        }

        if (shouldTriggerNextPage)
        {
            juce::Component::SafePointer<FreesoundMiniEditor> safeThis (this);
            juce::MessageManager::callAsync ([safeThis]() {
                if (safeThis != nullptr)
                    safeThis->loadNextPageAsync();
            });
        }

        g.setColour (juce::Colours::white);

        if (columnId == 1)
        {
            g.drawText (item.name, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
        }
        else if (columnId == 2)
        {
            g.setColour (juce::Colours::grey);
            g.drawText (formatSecondsCompact (item.duration), 0, 0, width, height, juce::Justification::centred);
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

    void applySearchResults (const std::vector<SoundResult>& newResults, bool append, bool more)
    {
        {
            const juce::ScopedLock sl (resultsLock);
            if (append)
                results.insert (results.end(), newResults.begin(), newResults.end());
            else
                results = newResults;

            hasMorePages = more;
            isLoadingMore = false;
        }
        table.updateContent();
        statusLabel.setText ("Loaded: " + juce::String (getNumRows()) + (hasMorePages ? "+ sounds (scroll for more)" : " sounds"),
                             juce::dontSendNotification);
    }

    void applyStatusMessage (const juce::String& text)
    {
        isLoadingMore = false;
        statusLabel.setText (text, juce::dontSendNotification);
    }

    void finishDownloadForIndex (int index, const juce::File& local)
    {
        if (local.existsAsFile())
        {
            {
                const juce::ScopedLock sl (resultsLock);
                if (index >= 0 && index < static_cast<int> (results.size()))
                    results[static_cast<size_t> (index)].localFile = local;
            }
            table.repaintRow (index);
            proc.loadAndPlayFile (local, true);
            refreshWaveformFromProcessor();
            statusLabel.setText ("Ready! Click waveform to seek or drag to Ableton.", juce::dontSendNotification);
        }
        else
        {
            statusLabel.setText ("Download failed.", juce::dontSendNotification);
        }
    }

private:
    // Преобразование позиции ползунка [0..100] в секунды:
    // 0..50 -> плавная шкала 0.0s .. 60.0s
    // 50..99.5 -> геометрический рост после 1 минуты: 60 * 2^((t - 50) / 8.0) (до ~1 часа)
    // >= 99.5 -> Бесконечность (∞)
    static double sliderPosToSeconds (double pos, bool& isInfinity)
    {
        if (pos >= 99.5)
        {
            isInfinity = true;
            return -1.0;
        }
        isInfinity = false;
        if (pos <= 0.0)
            return 0.0;
        if (pos <= 50.0)
        {
            double norm = pos / 50.0;
            return 60.0 * norm * norm; // квадратичная кривая для высокой точности на коротких звуках (0.1s..10s)
        }
        return 60.0 * std::pow (2.0, (pos - 50.0) / 8.0);
    }

    static juce::String formatSecondsCompact (double sec)
    {
        if (sec < 10.0)
            return juce::String (sec, 1) + "s";
        if (sec < 60.0)
            return juce::String ((int) std::round (sec)) + "s";
        int mins = (int) (sec / 60.0);
        int remSec = (int) std::round (sec) % 60;
        if (remSec == 0)
            return juce::String (mins) + "m";
        return juce::String (mins) + "m" + juce::String (remSec) + "s";
    }

    void updateDurationLabel()
    {
        bool minInf = false, maxInf = false;
        double minS = sliderPosToSeconds (durRangeSlider.getMinValue(), minInf);
        double maxS = sliderPosToSeconds (durRangeSlider.getMaxValue(), maxInf);

        juce::String minText = minInf ? "∞" : formatSecondsCompact (minS);
        juce::String maxText = maxInf ? "∞" : formatSecondsCompact (maxS);

        durValueLabel.setText (minText + " - " + maxText, juce::dontSendNotification);
    }

    void startNewSearch()
    {
        if (isThreadRunning())
            stopThread (2000);

        searchTokenSnapshot = tokenInput.getText().trim();
        searchQuerySnapshot = searchInput.getText().trim();

        bool minInf = false, maxInf = false;
        minDurSnapshot = sliderPosToSeconds (durRangeSlider.getMinValue(), minInf);
        if (minInf) minDurSnapshot = 0.0;

        maxDurSnapshot = sliderPosToSeconds (durRangeSlider.getMaxValue(), maxInf);
        maxIsInfinitySnapshot = maxInf;

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
        currentPage = 1;
        isPaginationFetch = false;
        isLoadingMore = true;
        hasMorePages = false;

        {
            const juce::ScopedLock sl (resultsLock);
            results.clear();
        }
        table.updateContent();

        statusLabel.setText ("Searching...", juce::dontSendNotification);
        startThread();
    }

    void loadNextPageAsync()
    {
        if (isLoadingMore || !hasMorePages || isThreadRunning())
            return;

        isLoadingMore = true;
        isPaginationFetch = true;
        ++currentPage;
        statusLabel.setText ("Loading page " + juce::String (currentPage) + "...", juce::dontSendNotification);
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
            statusLabel.setText ("Playing. Click waveform to seek or drag to Ableton!", juce::dontSendNotification);
        }
        else
        {
            downloadAndPlay (index);
        }
    }

    void run() override
    {
        juce::String maxBoundStr = maxIsInfinitySnapshot ? "*" : juce::String ( std::max (minDurSnapshot + 0.1, maxDurSnapshot), 2 );
        juce::String filterStr = "duration:[" + juce::String (minDurSnapshot, 2) + " TO " + maxBoundStr + "]";

        if (channelFilterSnapshot == 2) filterStr += " channels:1";
        else if (channelFilterSnapshot == 3) filterStr += " channels:2";

        int pageToFetch = currentPage;
        bool appendFlag = isPaginationFetch;

        auto urlStr = "https://freesound.org/apiv2/search/text/?query="
                    + juce::URL::addEscapeChars (searchQuerySnapshot, true)
                    + "&filter=" + juce::URL::addEscapeChars (filterStr, true)
                    + "&token=" + juce::URL::addEscapeChars (searchTokenSnapshot, true)
                    + "&fields=id,name,duration,previews&page_size=40&page="
                    + juce::String (pageToFetch);

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
            auto nextVar = obj->getProperty ("next");
            bool more = nextVar.isString() && nextVar.toString().isNotEmpty();

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
                juce::MessageManager::callAsync ([safeThis, newResults, appendFlag, more]() {
                    if (safeThis != nullptr)
                        safeThis->applySearchResults (newResults, appendFlag, more);
                });
                return;
            }
        }

        juce::Component::SafePointer<FreesoundMiniEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]() {
            if (safeThis != nullptr)
                safeThis->applyStatusMessage ("Error: Check API Key or Connection");
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
                if (safeThis != nullptr)
                    safeThis->finishDownloadForIndex (index, local);
            });
        });
    }

    FreesoundMiniAudioProcessor& proc;
    ModernLookAndFeel modernLF;

    juce::TextEditor tokenInput;
    juce::Label apiStatusLabel;
    juce::TextEditor searchInput;
    IconButton searchButton     { IconButton::Search };

    juce::ComboBox channelBox;
    juce::Slider durRangeSlider;
    juce::Label durValueLabel;

    IconButton dirButton        { IconButton::Folder };
    IconButton openFolderButton { IconButton::OpenExternal };
    IconButton infoButton       { IconButton::Info };

    juce::TableListBox table;
    WaveformDisplay waveformDisplay;

    IconButton playButton       { IconButton::Play };
    IconButton pauseButton      { IconButton::Pause };
    IconButton stopButton       { IconButton::Stop };

    juce::Slider pitchSlider;
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
    double minDurSnapshot = 0.0;
    double maxDurSnapshot = 30.0;
    bool maxIsInfinitySnapshot = false;
    int channelFilterSnapshot = 1;
    int lastTriggeredRow = -1;

    int currentPage = 1;
    bool isLoadingMore = false;
    bool isPaginationFetch = false;
    bool hasMorePages = false;

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
