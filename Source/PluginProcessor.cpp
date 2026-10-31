#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <map>

//==============================================================================
struct SoundResult
{
    int id = 0;
    juce::String name;
    double duration = 0.0;
    juce::String previewUrl;
    juce::File localFile;

    juce::String username;
    juce::String packName;
    juce::String formatType;
    int sampleRate = 0;
    int bitDepth = 0;
    int channels = 0;
    juce::String license;
    juce::StringArray tags;

    juce::String detectedBpmKey;
};

//==============================================================================
class FreesoundMiniAudioProcessor : public juce::AudioProcessor
{
public:
    FreesoundMiniAudioProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        formatManager.registerBasicFormats();
        loadGlobalSettings();

        if (sampleDirectory == juce::File())
        {
            sampleDirectory = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                  .getChildFile ("FreesoundSamples");
        }
        sampleDirectory.createDirectory();
        getFavoritesDirectory().createDirectory();
        loadFavoritesIndex();
    }

    ~FreesoundMiniAudioProcessor() override
    {
        transportSource.stop();
        transportSource.setSource (nullptr);
        readerSource.reset();
    }

    static juce::File getGlobalSettingsFile()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("FreesoundMini");
        dir.createDirectory();
        return dir.getChildFile ("settings.xml");
    }

    juce::File getFavoritesDirectory() const
    {
        auto favDir = sampleDirectory.getChildFile ("Favorites");
        favDir.createDirectory();
        return favDir;
    }

    juce::File getFavoritesIndexFile() const
    {
        return getFavoritesDirectory().getChildFile ("favorites_index.xml");
    }

    void saveGlobalSettings()
    {
        juce::ValueTree tree ("FreesoundMiniGlobalSettings");
        tree.setProperty ("apiToken", savedToken, nullptr);
        tree.setProperty ("saveDirectory", sampleDirectory.getFullPathName(), nullptr);

        if (auto xml = tree.createXml())
            getGlobalSettingsFile().replaceWithText (xml->toString());
    }

    void loadGlobalSettings()
    {
        auto file = getGlobalSettingsFile();
        if (file.existsAsFile())
        {
            if (auto xml = juce::XmlDocument::parse (file))
            {
                auto tree = juce::ValueTree::fromXml (*xml);
                if (tree.isValid())
                {
                    auto tok = tree.getProperty ("apiToken", "").toString().trim();
                    if (tok.isNotEmpty())
                        savedToken = tok;

                    auto dir = tree.getProperty ("saveDirectory", "").toString().trim();
                    if (dir.isNotEmpty())
                        sampleDirectory = juce::File (dir);
                }
            }
        }
    }

    void saveFavoritesIndex()
    {
        const juce::ScopedLock sl (favLock);
        juce::ValueTree root ("FavoritesList");

        for (const auto& item : favoriteItems)
        {
            juce::ValueTree child ("Sound");
            child.setProperty ("id", item.id, nullptr);
            child.setProperty ("name", item.name, nullptr);
            child.setProperty ("duration", item.duration, nullptr);
            child.setProperty ("previewUrl", item.previewUrl, nullptr);
            child.setProperty ("localFile", item.localFile.getFullPathName(), nullptr);
            child.setProperty ("username", item.username, nullptr);
            child.setProperty ("packName", item.packName, nullptr);
            child.setProperty ("formatType", item.formatType, nullptr);
            child.setProperty ("sampleRate", item.sampleRate, nullptr);
            child.setProperty ("bitDepth", item.bitDepth, nullptr);
            child.setProperty ("channels", item.channels, nullptr);
            child.setProperty ("license", item.license, nullptr);
            child.setProperty ("tags", item.tags.joinIntoString (","), nullptr);
            child.setProperty ("detectedBpmKey", item.detectedBpmKey, nullptr);
            root.appendChild (child, nullptr);
        }

        if (auto xml = root.createXml())
            getFavoritesIndexFile().replaceWithText (xml->toString());
    }

    void loadFavoritesIndex()
    {
        const juce::ScopedLock sl (favLock);
        favoriteItems.clear();

        auto idxFile = getFavoritesIndexFile();
        if (idxFile.existsAsFile())
        {
            if (auto xml = juce::XmlDocument::parse (idxFile))
            {
                auto root = juce::ValueTree::fromXml (*xml);
                if (root.isValid())
                {
                    for (int i = 0; i < root.getNumChildren(); ++i)
                    {
                        auto c = root.getChild (i);
                        SoundResult r;
                        r.id = (int) c.getProperty ("id", 0);
                        r.name = c.getProperty ("name", "").toString();
                        r.duration = (double) c.getProperty ("duration", 0.0);
                        r.previewUrl = c.getProperty ("previewUrl", "").toString();

                        auto fPath = c.getProperty ("localFile", "").toString();
                        if (fPath.isNotEmpty())
                            r.localFile = juce::File (fPath);

                        auto favCopy = getFavoriteFileFor (r.id, r.name);
                        if (!r.localFile.existsAsFile() && favCopy.existsAsFile())
                            r.localFile = favCopy;

                        r.username = c.getProperty ("username", "").toString();
                        r.packName = c.getProperty ("packName", "").toString();
                        r.formatType = c.getProperty ("formatType", "").toString();
                        r.sampleRate = (int) c.getProperty ("sampleRate", 0);
                        r.bitDepth = (int) c.getProperty ("bitDepth", 0);
                        r.channels = (int) c.getProperty ("channels", 0);
                        r.license = c.getProperty ("license", "").toString();
                        r.tags.addTokens (c.getProperty ("tags", "").toString(), ",", "");
                        r.detectedBpmKey = c.getProperty ("detectedBpmKey", "").toString();

                        if (r.id > 0)
                            favoriteItems.push_back (r);
                    }
                }
            }
        }
    }

    juce::File getFavoriteFileFor (int soundId, const juce::String& name) const
    {
        auto sanitized = juce::File::createLegalFileName (name);
        return getFavoritesDirectory().getChildFile (juce::String (soundId) + "_" + sanitized + ".mp3");
    }

    bool isSoundFavorite (int soundId) const
    {
        const juce::ScopedLock sl (favLock);
        for (const auto& item : favoriteItems)
            if (item.id == soundId)
                return true;
        return false;
    }

    void setSoundFavorite (const SoundResult& sound, bool shouldBeFav)
    {
        auto favFile = getFavoriteFileFor (sound.id, sound.name);

        {
            const juce::ScopedLock sl (favLock);
            auto it = std::remove_if (favoriteItems.begin(), favoriteItems.end(),
                                      [&] (const SoundResult& s) { return s.id == sound.id; });
            favoriteItems.erase (it, favoriteItems.end());

            if (shouldBeFav)
            {
                SoundResult copy = sound;
                if (sound.localFile.existsAsFile())
                {
                    sound.localFile.copyFileTo (favFile);
                    copy.localFile = sound.localFile;
                }
                favoriteItems.push_back (copy);
            }
            else
            {
                if (favFile.existsAsFile())
                    favFile.deleteFile();
            }
        }
        saveFavoritesIndex();
    }

    std::vector<SoundResult> getFavoriteSoundsList() const
    {
        const juce::ScopedLock sl (favLock);
        return favoriteItems;
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
                auto tok = tree.getProperty ("apiToken", "").toString().trim();
                if (tok.isNotEmpty())
                    savedToken = tok;

                auto dir = tree.getProperty ("saveDirectory", "").toString().trim();
                if (dir.isNotEmpty())
                    sampleDirectory = juce::File (dir);

                saveGlobalSettings();
            }
        }

        if (savedToken.isEmpty())
            loadGlobalSettings();
    }

    juce::String loadAndPlayFile (const juce::File& file, const SoundResult& meta, bool resetPitchToZero)
    {
        if (!file.existsAsFile())
            return {};

        if (resetPitchToZero)
            pitchSemitones = 0.0;

        transportSource.stop();
        transportSource.setSource (nullptr);
        readerSource.reset();
        currentFile = file;
        currentSampleMeta = meta;
        currentSampleMeta.localFile = file;

        juce::String badgeText;

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

                badgeText = analyzeAudioMusicalBadge (tempBuf, originalFileSampleRate, meta);
                currentSampleMeta.detectedBpmKey = badgeText;

                const juce::ScopedLock sl (bufferLock);
                cachedBuffer = std::move (tempBuf);
            }
        }

        rebuildTransport (0.0, true);
        return badgeText;
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
    SoundResult getCurrentSampleMeta() const { return currentSampleMeta; }

    juce::String savedToken;
    juce::File sampleDirectory;

private:
    void updateGainInternal()
    {
        float normGain = useNormalization ? (0.501187f / loadedFilePeak) : 1.0f;
        transportSource.setGain (previewGain * normGain);
    }

    // Умный детектор BPM и тональности с гейтом гармоничности (не выдаёт фейковую ноту на шумах/ударных)
    static juce::String analyzeAudioMusicalBadge (const juce::AudioBuffer<float>& buf, double sr, const SoundResult& meta)
    {
        int numSamples = buf.getNumSamples();
        if (numSamples < 2048 || sr <= 0.0)
            return {};

        const float* data = buf.getReadPointer (0);

        // 1. Поиск окна максимальной энергии для оценки гармоничности и высоты тона
        const int winSize = std::min (numSamples, 4096);
        int bestStart = 0;
        float bestEnergy = 0.0f;
        int step = std::max (512, numSamples / 20);

        for (int i = 0; i + winSize <= numSamples; i += step)
        {
            float e = 0.0f;
            for (int j = 0; j < winSize; j += 4)
                e += data[i + j] * data[i + j];
            if (e > bestEnergy)
            {
                bestEnergy = e;
                bestStart = i;
            }
        }

        juce::String detectedKey;
        if (bestEnergy > 0.0001f)
        {
            int minLag = std::max (8, (int) (sr / 880.0));  // До A5
            int maxLag = std::min (winSize / 2, (int) (sr / 55.0)); // От A1

            float r0 = 0.0f;
            for (int i = 0; i < winSize / 2; ++i)
                r0 += data[bestStart + i] * data[bestStart + i];

            float maxCorr = 0.0f;
            int bestLag = 0;

            if (r0 > 0.00001f)
            {
                for (int lag = minLag; lag <= maxLag; ++lag)
                {
                    float corr = 0.0f;
                    float rLag = 0.0f;
                    for (int i = 0; i < winSize / 2; ++i)
                    {
                        float a = data[bestStart + i];
                        float b = data[bestStart + i + lag];
                        corr += a * b;
                        rLag += b * b;
                    }
                    float denom = std::sqrt (r0 * rLag);
                    if (denom > 0.00001f)
                    {
                        float normCorr = corr / denom;
                        if (normCorr > maxCorr)
                        {
                            maxCorr = normCorr;
                            bestLag = lag;
                        }
                    }
                }
            }

            // Порог гармоничности 0.62 отсекает белый шум, фоли, дождь и атональную перкуссию
            if (maxCorr >= 0.62f && bestLag > 0)
            {
                double freq = sr / (double) bestLag;
                int midiNote = (int) std::round (69.0 + 12.0 * std::log2 (freq / 440.0));
                static const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
                int pitchClass = ((midiNote % 12) + 12) % 12;
                detectedKey = noteNames[pitchClass];

                // Проверяем теги или имя на явный минор/мажор
                juce::String combined = (meta.name + " " + meta.tags.joinIntoString (" ")).toLowerCase();
                if (combined.contains ("minor") || combined.contains ("min"))
                    detectedKey += "m";
            }
        }

        // 2. Поиск BPM: сначала проверяем явное указание в тегах/имени, затем по огибающей транзиентов
        int detectedBpm = 0;
        for (const auto& t : meta.tags)
        {
            auto low = t.toLowerCase();
            if (low.endsWith ("bpm"))
            {
                int val = low.dropLastCharacters (3).trim().getIntValue();
                if (val >= 50 && val <= 220)
                {
                    detectedBpm = val;
                    break;
                }
            }
        }

        double durationSec = (double) numSamples / sr;
        if (detectedBpm == 0 && durationSec >= 2.0 && durationSec <= 45.0)
        {
            const int envRate = 100;
            int hop = std::max (1, (int) (sr / envRate));
            int numFrames = numSamples / hop;

            if (numFrames > 160)
            {
                std::vector<float> onset (static_cast<size_t> (numFrames), 0.0f);
                float prevE = 0.0f;
                for (int f = 0; f < numFrames; ++f)
                {
                    float e = 0.0f;
                    int base = f * hop;
                    for (int s = 0; s < hop; s += 8)
                        e += std::abs (data[base + s]);
                    onset[static_cast<size_t> (f)] = std::max (0.0f, e - prevE);
                    prevE = e;
                }

                float o0 = 0.0f;
                int halfFrames = std::min (numFrames / 2, 600);
                for (int i = 0; i < halfFrames; ++i)
                    o0 += onset[static_cast<size_t> (i)] * onset[static_cast<size_t> (i)];

                if (o0 > 0.0001f)
                {
                    int minBpmLag = (int) ((60.0 / 175.0) * envRate);
                    int maxBpmLag = std::min (halfFrames - 1, (int) ((60.0 / 70.0) * envRate));

                    float bestBpmCorr = 0.0f;
                    int bestBpmLag = 0;

                    for (int lag = minBpmLag; lag <= maxBpmLag; ++lag)
                    {
                        float c = 0.0f;
                        for (int i = 0; i < halfFrames; ++i)
                            c += onset[static_cast<size_t> (i)] * onset[static_cast<size_t> (i + lag)];
                        float norm = c / o0;
                        if (norm > bestBpmCorr)
                        {
                            bestBpmCorr = norm;
                            bestBpmLag = lag;
                        }
                    }

                    if (bestBpmCorr >= 0.36f && bestBpmLag > 0)
                        detectedBpm = (int) std::round ((60.0 * envRate) / (double) bestBpmLag);
                }
            }
        }

        if (detectedBpm > 0 && detectedKey.isNotEmpty())
            return juce::String (detectedBpm) + " BPM | " + detectedKey;
        if (detectedBpm > 0)
            return juce::String (detectedBpm) + " BPM";
        if (detectedKey.isNotEmpty())
            return "Key: " + detectedKey;
        return {};
    }

    float previewGain = 0.8f;
    double pitchSemitones = 0.0;
    bool useNormalization = true;
    float loadedFilePeak = 1.0f;
    double originalFileSampleRate = 44100.0;

    juce::File currentFile;
    SoundResult currentSampleMeta;
    juce::CriticalSection bufferLock;
    juce::AudioBuffer<float> cachedBuffer;

    mutable juce::CriticalSection favLock;
    std::vector<SoundResult> favoriteItems;

    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transportSource;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreesoundMiniAudioProcessor)
};

//==============================================================================
struct HeartRenderer
{
    static juce::Path createHeartPath (juce::Rectangle<float> area)
    {
        juce::Path p;
        float w = std::min (area.getWidth(), area.getHeight());
        float cx = area.getCentreX();
        float cy = area.getCentreY() + w * 0.04f;
        float s = w * 0.45f;

        p.startNewSubPath (cx, cy + s * 0.85f);
        p.cubicTo (cx - s * 1.15f, cy + s * 0.15f,
                   cx - s * 1.05f, cy - s * 0.85f,
                   cx, cy - s * 0.32f);
        p.cubicTo (cx + s * 1.05f, cy - s * 0.85f,
                   cx + s * 1.15f, cy + s * 0.15f,
                   cx, cy + s * 0.85f);
        p.closeSubPath();
        return p;
    }

    static void drawHeart (juce::Graphics& g, juce::Rectangle<float> area, bool isFilled, bool isHovered)
    {
        auto p = createHeartPath (area);
        const juce::Colour sage (0xff52b788);

        if (isHovered)
        {
            g.setColour (sage.withAlpha (0.28f));
            g.strokePath (p, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (isFilled)
        {
            g.setColour (sage);
            g.fillPath (p);
        }
        else
        {
            g.setColour (sage);
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
};

//==============================================================================
class ApiStatusBadge : public juce::Button
{
public:
    ApiStatusBadge() : juce::Button ("") {}

    void setConnected (bool connected)
    {
        if (isConnected != connected)
        {
            isConnected = connected;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);

        juce::Colour accent = isConnected ? juce::Colour (0xff52b788) : juce::Colours::indianred;
        juce::Colour bg = isMouseOver ? juce::Colour (0xff1c2226) : juce::Colour (0xff14161a);

        g.setColour (bg);
        g.fillRoundedRectangle (b, 7.0f);

        g.setColour (accent.withAlpha (isMouseOver ? 0.7f : 0.35f));
        g.drawRoundedRectangle (b, 7.0f, 1.0f);

        float cy = b.getCentreY();
        float cx = b.getX() + 14.0f;
        float r = 6.5f;

        g.setColour (accent.withAlpha (0.2f));
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
        g.setColour (accent);
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.2f);

        juce::Path icon;
        if (isConnected)
        {
            icon.startNewSubPath (cx - 3.2f, cy + 0.2f);
            icon.lineTo (cx - 0.8f, cy + 2.6f);
            icon.lineTo (cx + 3.4f, cy - 2.4f);
        }
        else
        {
            icon.startNewSubPath (cx - 2.5f, cy - 2.5f);
            icon.lineTo (cx + 2.5f, cy + 2.5f);
            icon.startNewSubPath (cx + 2.5f, cy - 2.5f);
            icon.lineTo (cx - 2.5f, cy + 2.5f);
        }
        g.strokePath (icon, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (isConnected ? juce::Colour (0xff52b788) : juce::Colours::indianred);
        juce::String labelText = isConnected ? (isMouseOver ? "API OK (Edit)" : "API OK") : "NO API";
        g.drawText (labelText, (int) (cx + 10.0f), 0, getWidth() - (int) (cx + 14.0f), getHeight(), juce::Justification::centredLeft);
    }

private:
    bool isConnected = false;
};

//==============================================================================
class IconButton : public juce::Button
{
public:
    enum Type { Play, Pause, Stop, Search, Dice2D, HashTag, Heart, Folder, OpenExternal, Info, Close };

    explicit IconButton (Type t) : juce::Button (""), iconType (t) {}

    void setActiveState (bool active)
    {
        if (isActiveState != active)
        {
            isActiveState = active;
            repaint();
        }
    }

    bool getActiveState() const { return isActiveState; }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);

        juce::Colour bg = juce::Colour (0xff16181d);
        if (isButtonDown)      bg = juce::Colour (0xff24332c);
        else if (isMouseOver)  bg = juce::Colour (0xff1e2229);

        g.setColour (bg);
        g.fillRoundedRectangle (b, 7.0f);

        bool highlightBorder = isMouseOver || isActiveState;
        g.setColour (highlightBorder ? juce::Colour (0xff52b788).withAlpha (0.55f) : juce::Colour (0xff262930));
        g.drawRoundedRectangle (b, 7.0f, 1.0f);

        if (iconType == Heart)
        {
            HeartRenderer::drawHeart (g, b.reduced (5.5f), isActiveState, isMouseOver);
            return;
        }

        juce::Colour iconCol = (isButtonDown || isActiveState) ? juce::Colour (0xff52b788)
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

            case Dice2D:
            {
                // Схематичный плоский 2D-кубик в стиле Glitchmachines
                float boxR = s * 1.18f;
                juce::Rectangle<float> diceRect (cx - boxR, cy - boxR, boxR * 2.0f, boxR * 2.0f);
                g.drawRoundedRectangle (diceRect, 3.0f, 1.5f);

                float dotR = 1.35f;
                float off = boxR * 0.52f;
                g.fillEllipse (cx - dotR, cy - dotR, dotR * 2.0f, dotR * 2.0f);
                g.fillEllipse (cx - off - dotR, cy - off - dotR, dotR * 2.0f, dotR * 2.0f);
                g.fillEllipse (cx + off - dotR, cy - off - dotR, dotR * 2.0f, dotR * 2.0f);
                g.fillEllipse (cx - off - dotR, cy + off - dotR, dotR * 2.0f, dotR * 2.0f);
                g.fillEllipse (cx + off - dotR, cy + off - dotR, dotR * 2.0f, dotR * 2.0f);
                break;
            }

            case HashTag:
            {
                float d = s * 0.45f;
                float len = s * 0.95f;
                p.startNewSubPath (cx - d + 1.0f, cy - len);
                p.lineTo (cx - d - 1.0f, cy + len);
                p.startNewSubPath (cx + d + 1.0f, cy - len);
                p.lineTo (cx + d - 1.0f, cy + len);
                p.startNewSubPath (cx - len, cy - d);
                p.lineTo (cx + len, cy - d);
                p.startNewSubPath (cx - len, cy + d);
                p.lineTo (cx + len, cy + d);
                g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }

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

            case Heart:
                break;
        }
    }

private:
    Type iconType;
    bool isActiveState = false;
};

//==============================================================================
class WaveformDisplay : public juce::Component,
                        private juce::Timer
{
public:
    std::function<void(float)> onSeekRequested;
    std::function<void()> onDragged;
    std::function<float()> getPlayheadPos;
    std::function<void()> onSampleInfoClicked;
    std::function<void()> onSampleFavClicked;

    WaveformDisplay()
    {
        sampleFavBtn.onClick = [this] { if (onSampleFavClicked) onSampleFavClicked(); };
        addChildComponent (sampleFavBtn);

        sampleInfoBtn.onClick = [this] { if (onSampleInfoClicked) onSampleInfoClicked(); };
        addChildComponent (sampleInfoBtn);

        startTimerHz (30);
    }

    ~WaveformDisplay() override
    {
        stopTimer();
    }

    void setWaveformData (const juce::AudioBuffer<float>& newBuffer,
                          const juce::String& sampleTitle,
                          const juce::String& bpmKeyBadge,
                          bool isFav)
    {
        localBuffer.makeCopyOf (newBuffer);
        currentTitle = sampleTitle;
        currentBadge = bpmKeyBadge;
        sampleFavBtn.setActiveState (isFav);

        bool hasAudio = (localBuffer.getNumSamples() > 0);
        sampleFavBtn.setVisible (hasAudio);
        sampleInfoBtn.setVisible (hasAudio);

        repaint();
    }

    void updateFavoriteState (bool isFav)
    {
        sampleFavBtn.setActiveState (isFav);
    }

    void resized() override
    {
        sampleInfoBtn.setBounds (getWidth() - 28, 6, 22, 22);
        sampleFavBtn.setBounds (getWidth() - 54, 6, 22, 22);
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
            g.drawText ("No Audio Loaded", getLocalBounds(), juce::Justification::centred);
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

            float targetHeight = midY - (maxVal * midY * 0.74f);
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

        // Вертикальный курсор воспроизведения
        float cursorX = lastCursorProp * (float) width;
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawVerticalLine ((int) std::round (cursorX), 4.0f, (float) height - 20.0f);
        g.fillEllipse (cursorX - 3.0f, 3.0f, 6.0f, 6.0f);

        // Жёлтая зона: Умный бейдж BPM и Тональности справа внизу
        int badgeWidth = 0;
        if (currentBadge.isNotEmpty())
        {
            badgeWidth = std::min (115, (int) currentBadge.length() * 7 + 16);
            juce::Rectangle<float> badgeRect ((float) (width - badgeWidth - 8), (float) (height - 21), (float) badgeWidth, 16.0f);
            g.setColour (juce::Colour (0xff0d0f12).withAlpha (0.82f));
            g.fillRoundedRectangle (badgeRect, 5.0f);
            g.setColour (juce::Colour (0xff52b788).withAlpha (0.5f));
            g.drawRoundedRectangle (badgeRect, 5.0f, 1.0f);

            g.setColour (juce::Colour (0xff74c69d));
            g.drawText (currentBadge, badgeRect.toNearestInt(), juce::Justification::centred, false);
        }

        // Красная зона: Название текущего сэмпла по центру внизу волны
        if (currentTitle.isNotEmpty())
        {
            int sideMargin = (badgeWidth > 0) ? (badgeWidth + 14) : 12;
            juce::Rectangle<int> titleArea (sideMargin, height - 20, std::max (60, width - sideMargin * 2), 16);
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.drawText (currentTitle, titleArea, juce::Justification::centred, true);
        }
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
    juce::String currentTitle;
    juce::String currentBadge;
    float lastCursorProp = 0.0f;
    bool dragStarted = false;

    IconButton sampleFavBtn  { IconButton::Heart };
    IconButton sampleInfoBtn { IconButton::Info };
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
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff14161a));
        setColour (juce::PopupMenu::textColourId, juce::Colours::lightgrey);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff1f2d26));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (0xff52b788));
        setColour (juce::TableHeaderComponent::backgroundColourId, juce::Colour (0xff14161a));
        setColour (juce::TableHeaderComponent::textColourId, juce::Colours::grey);
        setColour (juce::TableHeaderComponent::outlineColourId, juce::Colour (0xff1f2229));
        setColour (juce::TableHeaderComponent::highlightColourId, juce::Colour (0xff52b788));
        setColour (juce::TableListBox::backgroundColourId, juce::Colour (0xff0f1114));
        setColour (juce::TableListBox::outlineColourId, juce::Colour (0xff1f2229));
        setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff52b788));
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int, int, int, int, juce::ComboBox& box) override
    {
        auto b = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
        g.setColour (juce::Colour (0xff14161a));
        g.fillRoundedRectangle (b, 7.0f);

        bool active = isButtonDown || box.isPopupActive();
        g.setColour (active ? juce::Colour (0xff52b788).withAlpha (0.65f) : juce::Colour (0xff232730));
        g.drawRoundedRectangle (b, 7.0f, 1.0f);

        float arrowX = (float) width - 15.0f;
        float arrowY = (float) height * 0.5f;
        juce::Path chevron;
        chevron.startNewSubPath (arrowX - 3.5f, arrowY - 1.5f);
        chevron.lineTo (arrowX, arrowY + 2.2f);
        chevron.lineTo (arrowX + 3.5f, arrowY - 1.5f);

        g.setColour (juce::Colour (0xff52b788));
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 24, box.getHeight() - 2);
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
// Карточка подробной информации о сэмпле (по клику на (i) на волне)
class SampleDetailsOverlay : public juce::Component
{
public:
    std::function<void(const juce::String&)> onTagClicked;
    std::function<void(int, const juce::String&)> onFindSimilarClicked;

    SampleDetailsOverlay()
    {
        setInterceptsMouseClicks (true, true);

        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);

        similarBtn.setButtonText ("Find Similar Sounds");
        similarBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1d2b24));
        similarBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff52b788));
        similarBtn.onClick = [this] {
            if (currentSound.id > 0 && onFindSimilarClicked)
            {
                setVisible (false);
                onFindSimilarClicked (currentSound.id, currentSound.name);
            }
        };
        addAndMakeVisible (similarBtn);
    }

    void showSoundDetails (const SoundResult& s)
    {
        currentSound = s;
        tagButtons.clear();

        int maxTags = std::min (10, s.tags.size());
        for (int i = 0; i < maxTags; ++i)
        {
            auto tagStr = s.tags[i].trim();
            if (tagStr.isNotEmpty())
            {
                auto btn = std::make_unique<juce::TextButton> ("#" + tagStr);
                btn->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1a1d24));
                btn->setColour (juce::TextButton::textColourOffId, juce::Colours::lightgrey);
                btn->onClick = [this, tagStr] {
                    setVisible (false);
                    if (onTagClicked)
                        onTagClicked (tagStr);
                };
                addAndMakeVisible (btn.get());
                tagButtons.push_back (std::move (btn));
            }
        }

        resized();
        setVisible (true);
        toFront (true);
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0b0d).withAlpha (0.94f));

        auto card = getLocalBounds().reduced (18).toFloat();
        g.setColour (juce::Colour (0xff14161a));
        g.fillRoundedRectangle (card, 12.0f);
        g.setColour (juce::Colour (0xff52b788).withAlpha (0.45f));
        g.drawRoundedRectangle (card, 12.0f, 1.0f);

        auto area = getLocalBounds().reduced (32);
        g.setColour (juce::Colour (0xff52b788));
        g.drawText ("SAMPLE PASSPORT", area.removeFromTop (26), juce::Justification::centredLeft);

        g.setColour (juce::Colours::white);
        g.drawText (currentSound.name, area.removeFromTop (24), juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::grey);
        juce::String authorLine = "Author: @" + (currentSound.username.isNotEmpty() ? currentSound.username : "unknown")
                                + "   |   ID: #" + juce::String (currentSound.id);
        g.drawText (authorLine, area.removeFromTop (20), juce::Justification::centredLeft, true);

        if (currentSound.packName.isNotEmpty())
            g.drawText ("Pack: " + currentSound.packName, area.removeFromTop (20), juce::Justification::centredLeft, true);

        juce::String specs = "Format: " + (currentSound.formatType.isNotEmpty() ? currentSound.formatType.toUpperCase() : "AUDIO");
        if (currentSound.sampleRate > 0)
            specs += "  |  " + juce::String (currentSound.sampleRate / 1000.0, 1) + " kHz";
        if (currentSound.bitDepth > 0)
            specs += "  |  " + juce::String (currentSound.bitDepth) + "-bit";
        if (currentSound.channels > 0)
            specs += "  |  " + juce::String (currentSound.channels == 1 ? "Mono" : "Stereo");

        g.setColour (juce::Colours::lightgrey);
        g.drawText (specs, area.removeFromTop (22), juce::Justification::centredLeft, true);

        if (currentSound.detectedBpmKey.isNotEmpty())
        {
            g.setColour (juce::Colour (0xff52b788));
            g.drawText ("Analysis: " + currentSound.detectedBpmKey, area.removeFromTop (22), juce::Justification::centredLeft);
        }

        if (currentSound.license.isNotEmpty())
        {
            g.setColour (juce::Colours::grey);
            g.drawText ("License: " + currentSound.license, area.removeFromTop (20), juce::Justification::centredLeft, true);
        }

        area.removeFromTop (44); // Место под кнопку Find Similar Sounds

        g.setColour (juce::Colour (0xff52b788));
        g.drawText ("TAGS (Click any tag to search):", area.removeFromTop (22), juce::Justification::centredLeft);
    }

    void resized() override
    {
        closeBtn.setBounds (getWidth() - 52, 26, 26, 26);
        similarBtn.setBounds (32, 182, getWidth() - 64, 28);

        int x = 32;
        int y = 244;
        int maxX = getWidth() - 32;

        for (auto& btn : tagButtons)
        {
            int w = std::min (150, std::max (54, (int) btn->getButtonText().length() * 8 + 18));
            if (x + w > maxX)
            {
                x = 32;
                y += 28;
            }
            btn->setBounds (x, y, w, 24);
            x += w + 6;
        }
    }

private:
    SoundResult currentSound;
    IconButton closeBtn { IconButton::Close };
    juce::TextButton similarBtn;
    std::vector<std::unique_ptr<juce::TextButton>> tagButtons;
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

        apiLink.setButtonText ("https://freesound.org/apiv2/apply/");
        apiLink.setURL (juce::URL ("https://freesound.org/apiv2/apply/"));
        apiLink.setJustificationType (juce::Justification::centredLeft);
        apiLink.setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xff52b788));
        addAndMakeVisible (apiLink);
    }

    void mouseDown (const juce::MouseEvent&) override {}

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
        g.drawText ("FREESOUND MINI - GUIDE", area.removeFromTop (30), juce::Justification::centredLeft);

        g.setColour (juce::Colours::lightgrey);
        g.drawText ("1. API Key (Saved Globally):", area.removeFromTop (20), juce::Justification::centredLeft);
        area.removeFromTop (24);

        g.drawFittedText ("Paste key & press Enter. Click 'API OK' anytime to edit.",
                          area.removeFromTop (28), juce::Justification::topLeft, 2);

        area.removeFromTop (6);

        juce::String text =
            "2. Search, Tags (#) & Random Dice:\n"
            "   Use [#] for curated noise/texture tags or the 2D Dice for pure random discovery.\n\n"
            "3. Favorites (Heart) & Multi-Drag:\n"
            "   Heart any sound to save a copy into 'FreesoundSamples/Favorites'.\n"
            "   Select multiple rows (Shift/Cmd) and drag directly from the table or waveform.\n\n"
            "4. Sample Passport (i) & Sorting:\n"
            "   Click DUR header to sort by length. Click (i) on waveform for tags & Similar Sounds.";

        g.drawFittedText (text, area, juce::Justification::topLeft, 20);
    }

    void resized() override
    {
        closeButton.setBounds (getWidth() - 52, 26, 26, 26);
        apiLink.setBounds (32, 80, getWidth() - 64, 22);
    }

private:
    std::function<void()> onClose;
    IconButton closeButton { IconButton::Close };
    juce::HyperlinkButton apiLink;
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

        if (proc.savedToken.isEmpty())
            proc.loadGlobalSettings();

        isEditingToken = (proc.savedToken.trim().length() <= 5);

        tokenInput.setTextToShowWhenEmpty ("Paste API Key & press Enter...", juce::Colours::grey);
        tokenInput.setText (proc.savedToken, false);
        tokenInput.onTextChange = [this] {
            proc.savedToken = tokenInput.getText().trim();
            proc.saveGlobalSettings();
            apiStatusBadge.setConnected (proc.savedToken.length() > 5);
        };
        tokenInput.onReturnKey = [this] { commitTokenAndHideIfValid(); };
        tokenInput.onFocusLost = [this] { commitTokenAndHideIfValid(); };
        addAndMakeVisible (tokenInput);

        apiStatusBadge.setConnected (proc.savedToken.trim().length() > 5);
        apiStatusBadge.onClick = [this] {
            isEditingToken = !isEditingToken;
            updateTopBarVisibility();
            if (isEditingToken)
                tokenInput.grabKeyboardFocus();
        };
        addAndMakeVisible (apiStatusBadge);

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
                    proc.saveGlobalSettings();
                    proc.loadFavoritesIndex();
                    statusLabel.setText ("Folder: " + result.getFileName(), juce::dontSendNotification);
                }
            });
        };
        addAndMakeVisible (dirButton);

        openFolderButton.onClick = [this] {
            if (showingFavoritesOnly)
                proc.getFavoritesDirectory().startAsProcess();
            else
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

        // Поисковая строка + Лупа + Облако тегов (#) + 2D Кубик рандома + Фильтр Избранного
        searchInput.setTextToShowWhenEmpty ("Search sounds (808, vinyl, bowl)...", juce::Colours::grey);
        searchInput.onReturnKey = [this] { startNewSearch(); };
        addAndMakeVisible (searchInput);

        searchButton.onClick = [this] { startNewSearch(); };
        addAndMakeVisible (searchButton);

        tagMenuButton.onClick = [this] { showCuratedTagsMenu(); };
        addAndMakeVisible (tagMenuButton);

        randomDiceButton.onClick = [this] { startPureRandomSearch(); };
        addAndMakeVisible (randomDiceButton);

        favFilterButton.onClick = [this] { toggleFavoritesView(); };
        addAndMakeVisible (favFilterButton);

        // Панель фильтров: Каналы + Тип (All / One-Shots / Loops) + Диапазон длительности
        channelBox.addItem ("All Ch", 1);
        channelBox.addItem ("Mono", 2);
        channelBox.addItem ("Stereo", 3);
        channelBox.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (channelBox);

        typeBox.addItem ("All Types", 1);
        typeBox.addItem ("One-Shots", 2);
        typeBox.addItem ("Loops", 3);
        typeBox.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (typeBox);

        durRangeSlider.setSliderStyle (juce::Slider::TwoValueHorizontal);
        durRangeSlider.setRange (0.0, 100.0, 0.1);
        durRangeSlider.setMinAndMaxValues (0.0, 42.0, juce::dontSendNotification);
        durRangeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        durRangeSlider.onValueChange = [this] {
            if (durRangeSlider.getMinValue() > durRangeSlider.getMaxValue())
                durRangeSlider.setMinValue (durRangeSlider.getMaxValue(), juce::dontSendNotification);
            updateDurationLabel();
        };
        addAndMakeVisible (durRangeSlider);

        durValueLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        durValueLabel.setJustificationType (juce::Justification::centredRight);
        durValueLabel.setBorderSize (juce::BorderSize<int> (0));
        updateDurationLabel();
        addAndMakeVisible (durValueLabel);

        // Таблица с поддержкой мультивыбора, сортировки по DUR и перетаскивания
        table.setModel (this);
        table.getHeader().addColumn ("", 1, 28, 28, 28, juce::TableHeaderComponent::notResizable);
        table.getHeader().addColumn ("NAME", 2, 185, 100, 800, juce::TableHeaderComponent::defaultFlags);
        table.getHeader().addColumn ("DUR", 3, 58, 50, 80, juce::TableHeaderComponent::defaultFlags);
        table.getHeader().addColumn ("STATUS", 4, 56, 50, 70, juce::TableHeaderComponent::notSortable);
        table.setMultipleSelectionEnabled (true);
        addAndMakeVisible (table);

        // Дисплей волны
        waveformDisplay.onSeekRequested = [this] (float proportion) {
            proc.seekToRelativePosition (proportion);
        };
        waveformDisplay.onDragged = [this] {
            startExternalDragFromWaveform();
        };
        waveformDisplay.getPlayheadPos = [this] {
            return proc.getRelativePlayheadPosition();
        };
        waveformDisplay.onSampleFavClicked = [this] {
            auto cur = proc.getCurrentSampleMeta();
            if (cur.id > 0)
            {
                bool nowFav = !proc.isSoundFavorite (cur.id);
                proc.setSoundFavorite (cur, nowFav);
                waveformDisplay.updateFavoriteState (nowFav);
                table.repaint();
            }
        };
        waveformDisplay.onSampleInfoClicked = [this] {
            auto cur = proc.getCurrentSampleMeta();
            if (cur.id > 0 && sampleDetailsOverlay != nullptr)
            {
                sampleDetailsOverlay->setBounds (getLocalBounds());
                sampleDetailsOverlay->showSoundDetails (cur);
            }
        };
        addAndMakeVisible (waveformDisplay);

        // Транспорт
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
        volLabel.setBorderSize (juce::BorderSize<int> (0));
        volLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (volLabel);

        statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
        statusLabel.setBorderSize (juce::BorderSize<int> (0));
        addAndMakeVisible (statusLabel);

        sampleDetailsOverlay = std::make_unique<SampleDetailsOverlay>();
        sampleDetailsOverlay->onTagClicked = [this] (const juce::String& tag) {
            showingFavoritesOnly = false;
            favFilterButton.setActiveState (false);
            searchInput.setText (tag, false);
            startNewSearch();
        };
        sampleDetailsOverlay->onFindSimilarClicked = [this] (int soundId, const juce::String& soundName) {
            startSimilarSearch (soundId, soundName);
        };
        sampleDetailsOverlay->setVisible (false);
        addChildComponent (sampleDetailsOverlay.get());

        infoOverlay = std::make_unique<InfoOverlay> ([this] { infoOverlay->setVisible (false); });
        infoOverlay->setVisible (false);
        addChildComponent (infoOverlay.get());

        updateTopBarVisibility();
        refreshWaveformFromProcessor();

        setResizable (true, true);
        setResizeLimits (370, 540, 1100, 1200);
        setSize (400, 620);
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

        // Верхняя панель
        auto topBar = area.removeFromTop (28);
        infoButton.setBounds (topBar.removeFromRight (28));
        topBar.removeFromRight (4);
        openFolderButton.setBounds (topBar.removeFromRight (28));
        topBar.removeFromRight (4);
        dirButton.setBounds (topBar.removeFromRight (28));
        topBar.removeFromRight (6);

        if (tokenInput.isVisible())
        {
            apiStatusBadge.setBounds (topBar.removeFromLeft (86));
            topBar.removeFromLeft (6);
            tokenInput.setBounds (topBar);
        }
        else
        {
            apiStatusBadge.setBounds (topBar.removeFromLeft (115));
        }

        area.removeFromTop (8);

        // Строка поиска + Лупа + Теги (#) + 2D Кубик + Сердце (фильтр Favorites)
        auto searchRow = area.removeFromTop (30);
        favFilterButton.setBounds (searchRow.removeFromRight (30));
        searchRow.removeFromRight (4);
        randomDiceButton.setBounds (searchRow.removeFromRight (30));
        searchRow.removeFromRight (4);
        tagMenuButton.setBounds (searchRow.removeFromRight (30));
        searchRow.removeFromRight (4);
        searchButton.setBounds (searchRow.removeFromRight (30));
        searchRow.removeFromRight (6);
        searchInput.setBounds (searchRow);

        area.removeFromTop (6);

        // Строка фильтров: Каналы + Тип (All / One-Shots / Loops) + Диапазон длительности
        auto filterRow = area.removeFromTop (24);
        channelBox.setBounds (filterRow.removeFromLeft (72));
        filterRow.removeFromLeft (5);
        typeBox.setBounds (filterRow.removeFromLeft (86));
        filterRow.removeFromLeft (6);
        durValueLabel.setBounds (filterRow.removeFromRight (88));
        filterRow.removeFromRight (4);
        durRangeSlider.setBounds (filterRow);

        area.removeFromTop (8);

        // Нижняя панель транспорта
        auto bottomControl = area.removeFromBottom (48);
        area.removeFromBottom (6);

        int waveHeight = juce::jlimit (82, 175, getHeight() / 6);
        waveformDisplay.setBounds (area.removeFromBottom (waveHeight));

        area.removeFromBottom (4);
        statusLabel.setBounds (area.removeFromBottom (18));

        // Таблица
        table.setBounds (area);
        int nameColW = std::max (120, table.getWidth() - (28 + 58 + 56 + 18));
        table.getHeader().setColumnWidth (2, nameColW);

        // Кнопки транспорта
        auto bArea = bottomControl;
        playButton.setBounds (bArea.removeFromLeft (32).withSizeKeepingCentre (32, 32));
        bArea.removeFromLeft (4);
        pauseButton.setBounds (bArea.removeFromLeft (32).withSizeKeepingCentre (32, 32));
        bArea.removeFromLeft (4);
        stopButton.setBounds (bArea.removeFromLeft (32).withSizeKeepingCentre (32, 32));

        bArea.removeFromLeft (6);
        pitchSlider.setBounds (bArea.removeFromLeft (54));

        bArea.removeFromLeft (4);
        normCheckbox.setBounds (bArea.removeFromLeft (78));

        bArea.removeFromLeft (4);
        volLabel.setBounds (bArea.removeFromLeft (26));
        volSlider.setBounds (bArea.reduced (0, 14));

        if (sampleDetailsOverlay != nullptr)
            sampleDetailsOverlay->setBounds (getLocalBounds());
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

            if (!showingFavoritesOnly && rowNumber >= total - 5 && !isLoadingMore && hasMorePages)
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

        if (columnId == 1)
        {
            bool isFav = proc.isSoundFavorite (item.id);
            juce::Rectangle<float> heartBounds (0.0f, 0.0f, (float) width, (float) height);
            HeartRenderer::drawHeart (g, heartBounds.reduced (6.5f), isFav, false);
        }
        else if (columnId == 2)
        {
            g.setColour (juce::Colours::white);
            g.drawText (item.name, 6, 0, width - 10, height, juce::Justification::centredLeft, true);
        }
        else if (columnId == 3)
        {
            g.setColour (juce::Colours::grey);
            g.drawText (formatSecondsCompact (item.duration), 0, 0, width, height, juce::Justification::centred);
        }
        else if (columnId == 4)
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

    // Сортировка по клику на заголовок колонки (DUR или NAME)
    void sortOrderChanged (int newSortColumnId, bool isForwards) override
    {
        if (newSortColumnId == 0)
            return;

        {
            const juce::ScopedLock sl (resultsLock);
            std::stable_sort (results.begin(), results.end(), [newSortColumnId, isForwards] (const SoundResult& a, const SoundResult& b) {
                if (newSortColumnId == 3) // DUR
                    return isForwards ? (a.duration < b.duration) : (a.duration > b.duration);
                if (newSortColumnId == 2) // NAME
                    return isForwards ? (a.name.compareIgnoreCase (b.name) < 0) : (a.name.compareIgnoreCase (b.name) > 0);
                return false;
            });
        }
        table.updateContent();
        table.repaint();
    }

    // Мульти-перетаскивание выделенных файлов прямо из таблицы в Ableton Live
    juce::var getDragSourceDescription (const juce::SparseSet<int>& currentlySelectedRows) override
    {
        juce::StringArray filesToDrag;

        {
            const juce::ScopedLock sl (resultsLock);
            for (int i = 0; i < currentlySelectedRows.size(); ++i)
            {
                int r = currentlySelectedRows[i];
                if (r >= 0 && r < static_cast<int> (results.size()))
                {
                    auto f = results[static_cast<size_t> (r)].localFile;
                    if (f.existsAsFile())
                        filesToDrag.addIfNotAlreadyThere (f.getFullPathName());
                }
            }
        }

        if (filesToDrag.isEmpty() && proc.getCurrentFile().existsAsFile())
            filesToDrag.add (proc.getCurrentFile().getFullPathName());

        if (!filesToDrag.isEmpty())
            performExternalDragDropOfFiles (filesToDrag, false);

        return {};
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        // Если выделено несколько строк (через Shift/Cmd), автоматически докачиваем все не скачанные файлы
        auto selected = table.getSelectedRows();
        if (selected.size() > 1)
        {
            for (int i = 0; i < selected.size(); ++i)
                ensureRowDownloadedQuietly (selected[i]);
        }

        if (lastRowSelected >= 0 && lastRowSelected != lastTriggeredRow)
        {
            lastTriggeredRow = lastRowSelected;
            pitchSlider.setValue (0.0, juce::dontSendNotification);
            triggerLoadAndPlay (lastRowSelected);
        }
    }

    void cellClicked (int rowNumber, int columnId, const juce::MouseEvent&) override
    {
        if (rowNumber < 0)
            return;

        // Клик по первому столбцу (Сердце) переключает избранное
        if (columnId == 1)
        {
            SoundResult item;
            {
                const juce::ScopedLock sl (resultsLock);
                if (rowNumber >= static_cast<int> (results.size()))
                    return;
                item = results[static_cast<size_t> (rowNumber)];
            }

            bool makeFav = !proc.isSoundFavorite (item.id);
            proc.setSoundFavorite (item, makeFav);

            if (makeFav && !item.localFile.existsAsFile())
                ensureRowDownloadedQuietly (rowNumber);

            if (proc.getCurrentSampleMeta().id == item.id)
                waveformDisplay.updateFavoriteState (makeFav);

            if (showingFavoritesOnly && !makeFav)
            {
                toggleFavoritesView (true);
            }
            else
            {
                table.repaintRow (rowNumber);
            }
            return;
        }

        if (table.getNumSelectedRows() <= 1)
        {
            lastTriggeredRow = rowNumber;
            pitchSlider.setValue (0.0, juce::dontSendNotification);
            triggerLoadAndPlay (rowNumber);
        }
    }

    void applySearchResults (std::vector<SoundResult> newResults, bool append, bool more)
    {
        declusterResults (newResults);

        {
            const juce::ScopedLock sl (resultsLock);
            if (append)
                results.insert (results.end(), newResults.begin(), newResults.end());
            else
                results = std::move (newResults);

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

    void finishDownloadForIndex (int soundId, const juce::File& local, bool shouldAutoPlay)
    {
        if (local.existsAsFile())
        {
            SoundResult matchedMeta;
            int matchedRow = -1;

            {
                const juce::ScopedLock sl (resultsLock);
                for (size_t i = 0; i < results.size(); ++i)
                {
                    if (results[i].id == soundId)
                    {
                        results[i].localFile = local;
                        matchedMeta = results[i];
                        matchedRow = static_cast<int> (i);
                        break;
                    }
                }
            }

            // Если этот звук уже помечен как Favorite, синхронизируем копию в папку Favorites
            if (proc.isSoundFavorite (soundId) && matchedMeta.id > 0)
                proc.setSoundFavorite (matchedMeta, true);

            if (matchedRow >= 0)
                table.repaintRow (matchedRow);

            if (shouldAutoPlay && matchedMeta.id > 0)
            {
                proc.loadAndPlayFile (local, matchedMeta, true);
                refreshWaveformFromProcessor();
                statusLabel.setText ("Ready! Drag from table or waveform to Ableton.", juce::dontSendNotification);
            }
        }
        else if (shouldAutoPlay)
        {
            statusLabel.setText ("Download failed.", juce::dontSendNotification);
        }
    }

private:
    // Умное разбавление выдачи: не более 2 звуков подряд от одного автора или из одного пака
    static void declusterResults (std::vector<SoundResult>& list)
    {
        if (list.size() <= 3)
            return;

        std::vector<SoundResult> primary;
        std::vector<SoundResult> deferred;
        std::map<juce::String, int> authorCounts;

        for (const auto& item : list)
        {
            juce::String key = item.username.isNotEmpty() ? item.username : item.name.substring (0, 8);
            int count = authorCounts[key];
            if (count < 2)
            {
                primary.push_back (item);
                authorCounts[key] = count + 1;
            }
            else
            {
                deferred.push_back (item);
            }
        }

        primary.insert (primary.end(), deferred.begin(), deferred.end());
        list = std::move (primary);
    }

    void showCuratedTagsMenu()
    {
        juce::PopupMenu menu;

        juce::PopupMenu textures;
        textures.addItem (101, "vinyl");
        textures.addItem (102, "tape");
        textures.addItem (103, "crackle");
        textures.addItem (104, "rain");
        textures.addItem (105, "forest");
        textures.addItem (106, "wind");
        textures.addItem (107, "noise");
        menu.addSubMenu ("Textures & Noise", textures);

        juce::PopupMenu drums;
        drums.addItem (201, "kick");
        drums.addItem (202, "snare");
        drums.addItem (203, "hihat");
        drums.addItem (204, "percussion");
        drums.addItem (205, "breakbeat");
        drums.addItem (206, "808");
        menu.addSubMenu ("Drums & Perc", drums);

        juce::PopupMenu synths;
        synths.addItem (301, "modular");
        synths.addItem (302, "drone");
        synths.addItem (303, "pad");
        synths.addItem (304, "sub");
        synths.addItem (305, "analog");
        synths.addItem (306, "chord");
        menu.addSubMenu ("Synths & Bass", synths);

        juce::PopupMenu foley;
        foley.addItem (401, "foley");
        foley.addItem (402, "glitch");
        foley.addItem (403, "metal");
        foley.addItem (404, "impact");
        foley.addItem (405, "glass");
        foley.addItem (406, "footsteps");
        menu.addSubMenu ("Foley & FX", foley);

        juce::PopupMenu acoustic;
        acoustic.addItem (501, "piano");
        acoustic.addItem (502, "bowl");
        acoustic.addItem (503, "kalimba");
        acoustic.addItem (504, "strings");
        acoustic.addItem (505, "vocal");
        acoustic.addItem (506, "flute");
        menu.addSubMenu ("Acoustic & World", acoustic);

        static const std::map<int, const char*> tagMap = {
            {101, "vinyl"}, {102, "tape"}, {103, "crackle"}, {104, "rain"}, {105, "forest"}, {106, "wind"}, {107, "noise"},
            {201, "kick"}, {202, "snare"}, {203, "hihat"}, {204, "percussion"}, {205, "breakbeat"}, {206, "808"},
            {301, "modular"}, {302, "drone"}, {303, "pad"}, {304, "sub"}, {305, "analog"}, {306, "chord"},
            {401, "foley"}, {402, "glitch"}, {403, "metal"}, {404, "impact"}, {405, "glass"}, {406, "footsteps"},
            {501, "piano"}, {502, "bowl"}, {503, "kalimba"}, {504, "strings"}, {505, "vocal"}, {506, "flute"}
        };

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&tagMenuButton),
                            [this] (int result) {
                                auto it = tagMap.find (result);
                                if (it != tagMap.end())
                                {
                                    showingFavoritesOnly = false;
                                    favFilterButton.setActiveState (false);
                                    searchInput.setText (it->second, false);
                                    startNewSearch();
                                }
                            });
    }

    // Чистый рандом без подтверждающих окон: очищает запрос и достаёт случайный срез Freesound
    void startPureRandomSearch()
    {
        if (isThreadRunning())
            stopThread (2000);

        if (tokenInput.isVisible())
            commitTokenAndHideIfValid();

        searchTokenSnapshot = proc.savedToken.trim();
        if (searchTokenSnapshot.isEmpty())
        {
            isEditingToken = true;
            updateTopBarVisibility();
            statusLabel.setText ("Paste API Key first!", juce::dontSendNotification);
            return;
        }

        showingFavoritesOnly = false;
        favFilterButton.setActiveState (false);
        searchInput.clear();

        static const char* randomSeeds[] = {
            "a", "e", "i", "o", "u", "sound", "field", "synth", "percussion", "texture",
            "recording", "ambient", "metal", "wood", "voice", "analog", "drum", "noise",
            "water", "bass", "acoustic", "glitch", "space", "modular", "organic", "hit"
        };

        auto& rng = juce::Random::getSystemRandom();
        searchQuerySnapshot = randomSeeds[rng.nextInt (26)];
        currentPage = 1 + rng.nextInt (8);

        bool minInf = false, maxInf = false;
        minDurSnapshot = sliderPosToSeconds (durRangeSlider.getMinValue(), minInf);
        if (minInf) minDurSnapshot = 0.0;
        maxDurSnapshot = sliderPosToSeconds (durRangeSlider.getMaxValue(), maxInf);
        maxIsInfinitySnapshot = maxInf;

        channelFilterSnapshot = channelBox.getSelectedId();
        typeFilterSnapshot = typeBox.getSelectedId();

        lastTriggeredRow = -1;
        similarSoundIdSnapshot = 0;
        isPaginationFetch = false;
        isLoadingMore = true;
        hasMorePages = true;

        {
            const juce::ScopedLock sl (resultsLock);
            results.clear();
        }
        table.updateContent();

        statusLabel.setText ("Rolling pure random sounds...", juce::dontSendNotification);
        startThread();
    }

    void startSimilarSearch (int soundId, const juce::String& soundName)
    {
        if (isThreadRunning())
            stopThread (2000);

        searchTokenSnapshot = proc.savedToken.trim();
        if (searchTokenSnapshot.isEmpty())
            return;

        showingFavoritesOnly = false;
        favFilterButton.setActiveState (false);

        similarSoundIdSnapshot = soundId;
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

        statusLabel.setText ("Finding sounds similar to: " + soundName.substring (0, 24) + "...", juce::dontSendNotification);
        startThread();
    }

    void toggleFavoritesView (bool forceRefresh = false)
    {
        if (!forceRefresh)
            showingFavoritesOnly = !showingFavoritesOnly;

        favFilterButton.setActiveState (showingFavoritesOnly);

        if (showingFavoritesOnly)
        {
            if (isThreadRunning())
                stopThread (2000);

            auto favs = proc.getFavoriteSoundsList();
            {
                const juce::ScopedLock sl (resultsLock);
                results = favs;
                hasMorePages = false;
                isLoadingMore = false;
            }
            table.updateContent();
            statusLabel.setText ("Favorites library: " + juce::String (getNumRows()) + " saved sounds", juce::dontSendNotification);
        }
        else
        {
            if (searchInput.getText().trim().isNotEmpty())
                startNewSearch();
            else
                startPureRandomSearch();
        }
    }

    void commitTokenAndHideIfValid()
    {
        proc.savedToken = tokenInput.getText().trim();
        proc.saveGlobalSettings();

        bool valid = (proc.savedToken.length() > 5);
        apiStatusBadge.setConnected (valid);

        if (valid)
        {
            isEditingToken = false;
            updateTopBarVisibility();
        }
    }

    void updateTopBarVisibility()
    {
        bool valid = (proc.savedToken.trim().length() > 5);
        apiStatusBadge.setConnected (valid);
        tokenInput.setVisible (!valid || isEditingToken);
        resized();
    }

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
            return 60.0 * norm * norm;
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

        const juce::String infSymbol (juce::CharPointer_UTF8 ("\xe2\x88\x9e"));
        juce::String minText = minInf ? infSymbol : formatSecondsCompact (minS);
        juce::String maxText = maxInf ? infSymbol : formatSecondsCompact (maxS);

        durValueLabel.setText (minText + " - " + maxText, juce::dontSendNotification);
    }

    void startNewSearch()
    {
        if (isThreadRunning())
            stopThread (2000);

        if (tokenInput.isVisible())
            commitTokenAndHideIfValid();

        showingFavoritesOnly = false;
        favFilterButton.setActiveState (false);
        similarSoundIdSnapshot = 0;

        searchTokenSnapshot = proc.savedToken.trim();
        searchQuerySnapshot = searchInput.getText().trim();

        bool minInf = false, maxInf = false;
        minDurSnapshot = sliderPosToSeconds (durRangeSlider.getMinValue(), minInf);
        if (minInf) minDurSnapshot = 0.0;

        maxDurSnapshot = sliderPosToSeconds (durRangeSlider.getMaxValue(), maxInf);
        maxIsInfinitySnapshot = maxInf;

        channelFilterSnapshot = channelBox.getSelectedId();
        typeFilterSnapshot = typeBox.getSelectedId();

        if (searchTokenSnapshot.isEmpty())
        {
            isEditingToken = true;
            updateTopBarVisibility();
            statusLabel.setText ("Paste API Key first!", juce::dontSendNotification);
            return;
        }
        if (searchQuerySnapshot.isEmpty())
        {
            startPureRandomSearch();
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
        if (showingFavoritesOnly || similarSoundIdSnapshot > 0 || isLoadingMore || !hasMorePages || isThreadRunning())
            return;

        isLoadingMore = true;
        isPaginationFetch = true;
        ++currentPage;
        statusLabel.setText ("Loading page " + juce::String (currentPage) + "...", juce::dontSendNotification);
        startThread();
    }

    void startExternalDragFromWaveform()
    {
        juce::StringArray filesToDrag;

        auto selected = table.getSelectedRows();
        if (selected.size() > 1)
        {
            const juce::ScopedLock sl (resultsLock);
            for (int i = 0; i < selected.size(); ++i)
            {
                int r = selected[i];
                if (r >= 0 && r < static_cast<int> (results.size()))
                {
                    auto f = results[static_cast<size_t> (r)].localFile;
                    if (f.existsAsFile())
                        filesToDrag.addIfNotAlreadyThere (f.getFullPathName());
                }
            }
        }

        if (filesToDrag.isEmpty() && proc.getCurrentFile().existsAsFile())
            filesToDrag.add (proc.getCurrentFile().getFullPathName());

        if (!filesToDrag.isEmpty())
            performExternalDragDropOfFiles (filesToDrag, false);
    }

    void refreshWaveformFromProcessor()
    {
        juce::AudioBuffer<float> temp;
        proc.copyCachedBuffer (temp);
        auto meta = proc.getCurrentSampleMeta();
        bool isFav = (meta.id > 0) ? proc.isSoundFavorite (meta.id) : false;
        waveformDisplay.setWaveformData (temp, meta.name, meta.detectedBpmKey, isFav);
    }

    void triggerLoadAndPlay (int index)
    {
        SoundResult item;
        {
            const juce::ScopedLock sl (resultsLock);
            if (index < 0 || index >= static_cast<int> (results.size()))
                return;
            item = results[static_cast<size_t> (index)];
        }

        if (item.localFile.existsAsFile())
        {
            juce::String badge = proc.loadAndPlayFile (item.localFile, item, true);
            {
                const juce::ScopedLock sl (resultsLock);
                if (index >= 0 && index < static_cast<int> (results.size()))
                    results[static_cast<size_t> (index)].detectedBpmKey = badge;
            }
            refreshWaveformFromProcessor();
            statusLabel.setText ("Playing. Drag from table or waveform to Ableton!", juce::dontSendNotification);
        }
        else
        {
            downloadSoundAtRow (index, true);
        }
    }

    void ensureRowDownloadedQuietly (int index)
    {
        SoundResult item;
        {
            const juce::ScopedLock sl (resultsLock);
            if (index < 0 || index >= static_cast<int> (results.size()))
                return;
            item = results[static_cast<size_t> (index)];
        }

        if (!item.localFile.existsAsFile())
            downloadSoundAtRow (index, false);
    }

    void run() override
    {
        juce::String urlStr;
        bool appendFlag = isPaginationFetch;

        if (similarSoundIdSnapshot > 0)
        {
            urlStr = "https://freesound.org/apiv2/sounds/" + juce::String (similarSoundIdSnapshot)
                   + "/similar/?token=" + juce::URL::addEscapeChars (searchTokenSnapshot, true)
                   + "&fields=id,name,duration,previews,username,pack,type,samplerate,bitdepth,channels,license,tags&page_size=40";
        }
        else
        {
            juce::String maxBoundStr = maxIsInfinitySnapshot ? "*" : juce::String (std::max (minDurSnapshot + 0.1, maxDurSnapshot), 2);
            juce::String filterStr = "duration:[" + juce::String (minDurSnapshot, 2) + " TO " + maxBoundStr + "]";

            if (channelFilterSnapshot == 2) filterStr += " channels:1";
            else if (channelFilterSnapshot == 3) filterStr += " channels:2";

            // Фильтр структуры: One-Shots (без ограничения длительности!) vs Loops
            if (typeFilterSnapshot == 2)      filterStr += " NOT tag:loop";
            else if (typeFilterSnapshot == 3) filterStr += " tag:loop";

            int pageToFetch = currentPage;

            urlStr = "https://freesound.org/apiv2/search/text/?query="
                   + juce::URL::addEscapeChars (searchQuerySnapshot, true)
                   + "&filter=" + juce::URL::addEscapeChars (filterStr, true)
                   + "&token=" + juce::URL::addEscapeChars (searchTokenSnapshot, true)
                   + "&fields=id,name,duration,previews,username,pack,type,samplerate,bitdepth,channels,license,tags&page_size=40&page="
                   + juce::String (pageToFetch);
        }

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
            bool more = (similarSoundIdSnapshot == 0) && nextVar.isString() && nextVar.toString().isNotEmpty();

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
                    r.username = elem["username"].toString();
                    r.packName = elem["pack"].toString();
                    r.formatType = elem["type"].toString();
                    r.sampleRate = static_cast<int> (elem["samplerate"]);
                    r.bitDepth = static_cast<int> (elem["bitdepth"]);
                    r.channels = static_cast<int> (elem["channels"]);
                    r.license = elem["license"].toString();

                    if (auto* tagsArr = elem["tags"].getArray())
                    {
                        for (auto& t : *tagsArr)
                            r.tags.add (t.toString());
                    }

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
                    else
                    {
                        auto favCopy = proc.getFavoriteFileFor (r.id, r.name);
                        if (favCopy.existsAsFile())
                            r.localFile = favCopy;
                    }

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
                safeThis->applyStatusMessage ("No more results or check API Key.");
        });
    }

    void downloadSoundAtRow (int index, bool shouldAutoPlay)
    {
        if (shouldAutoPlay)
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

        juce::Thread::launch ([safeThis, soundId, previewUrl, name, targetDir, shouldAutoPlay]() {
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

            juce::MessageManager::callAsync ([safeThis, soundId, local, shouldAutoPlay]() {
                if (safeThis != nullptr)
                    safeThis->finishDownloadForIndex (soundId, local, shouldAutoPlay);
            });
        });
    }

    FreesoundMiniAudioProcessor& proc;
    ModernLookAndFeel modernLF;

    ApiStatusBadge apiStatusBadge;
    juce::TextEditor tokenInput;
    bool isEditingToken = true;

    juce::TextEditor searchInput;
    IconButton searchButton     { IconButton::Search };
    IconButton tagMenuButton    { IconButton::HashTag };
    IconButton randomDiceButton { IconButton::Dice2D };
    IconButton favFilterButton  { IconButton::Heart };

    juce::ComboBox channelBox;
    juce::ComboBox typeBox;
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
    std::unique_ptr<SampleDetailsOverlay> sampleDetailsOverlay;
    std::unique_ptr<InfoOverlay> infoOverlay;
    std::vector<SoundResult> results;
    juce::CriticalSection resultsLock;

    juce::String searchTokenSnapshot;
    juce::String searchQuerySnapshot;
    double minDurSnapshot = 0.0;
    double maxDurSnapshot = 30.0;
    bool maxIsInfinitySnapshot = false;
    int channelFilterSnapshot = 1;
    int typeFilterSnapshot = 1;
    int similarSoundIdSnapshot = 0;
    int lastTriggeredRow = -1;

    int currentPage = 1;
    bool isLoadingMore = false;
    bool isPaginationFetch = false;
    bool hasMorePages = false;
    bool showingFavoritesOnly = false;

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
