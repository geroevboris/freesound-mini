#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <atomic>

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
    bool isPinnedRef = false;
};

struct SearchHistoryEntry
{
    juce::String query;
    std::vector<SoundResult> results;
    double minSlider = 0.0;
    double maxSlider = 42.0;
    int channelId = 1;
    int typeId = 1;
    int page = 1;
    bool hasMore = false;
};

//==============================================================================
class MemoryBufferAudioSource : public juce::PositionableAudioSource
{
public:
    MemoryBufferAudioSource() = default;

    void setBuffer (const juce::AudioBuffer<float>& newBuf)
    {
        const juce::ScopedLock sl (lock);
        buffer.makeCopyOf (newBuf);
        nextPlayPos.store (0);
    }

    void setReversed (bool shouldReverse)
    {
        if (reversed.load() != shouldReverse)
        {
            const juce::ScopedLock sl (lock);
            int64_t total = buffer.getNumSamples();
            int64_t cur = nextPlayPos.load();
            if (total > 0 && cur >= 0 && cur < total)
                nextPlayPos.store (total - 1 - cur);
            reversed.store (shouldReverse);
        }
    }

    void setLoopingMode (bool shouldLoop) { looping.store (shouldLoop); }
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const juce::ScopedLock sl (lock);
        int64_t total = buffer.getNumSamples();
        int numCh = buffer.getNumChannels();

        if (total <= 0 || numCh <= 0)
        {
            info.clearActiveBufferRegion();
            return;
        }

        int64_t pos = nextPlayPos.load();
        bool rev = reversed.load();
        bool loop = looping.load();

        for (int i = 0; i < info.numSamples; ++i)
        {
            if (pos >= total)
            {
                if (loop)
                {
                    pos = 0;
                }
                else
                {
                    for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                        info.buffer->clear (ch, info.startSample + i, info.numSamples - i);
                    pos = total + 1;
                    break;
                }
            }

            int64_t srcIdx = rev ? (total - 1 - pos) : pos;
            srcIdx = juce::jlimit ((int64_t) 0, total - 1, srcIdx);

            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->setSample (ch, info.startSample + i, buffer.getSample (ch % numCh, (int) srcIdx));

            ++pos;
        }

        nextPlayPos.store (pos);
    }

    void setNextReadPosition (juce::int64 newPosition) override
    {
        const juce::ScopedLock sl (lock);
        int64_t total = buffer.getNumSamples();
        nextPlayPos.store (juce::jlimit ((int64_t) 0, std::max ((int64_t) 0, total), (int64_t) newPosition));
    }

    juce::int64 getNextReadPosition() const override { return nextPlayPos.load(); }

    juce::int64 getTotalLength() const override
    {
        const juce::ScopedLock sl (lock);
        return buffer.getNumSamples();
    }

    bool isLooping() const override { return looping.load(); }

private:
    mutable juce::CriticalSection lock;
    juce::AudioBuffer<float> buffer;
    std::atomic<int64_t> nextPlayPos { 0 };
    std::atomic<bool> reversed { false };
    std::atomic<bool> looping { false };
};

//==============================================================================
class FreesoundMiniAudioProcessor : public juce::AudioProcessor
{
public:
    enum PlaybackMode { ModeNormal = 0, ModeReverse = 1, ModeLoop = 2 };

    FreesoundMiniAudioProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        formatManager.registerBasicFormats();
        memorySource = std::make_unique<MemoryBufferAudioSource>();
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
        memorySource.reset();
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

    juce::File getSampleFileFor (int soundId, const juce::String& name) const
    {
        auto sanitized = juce::File::createLegalFileName (name);
        return sampleDirectory.getChildFile (juce::String (soundId) + "_" + sanitized + ".mp3");
    }

    juce::File getFavoriteFileFor (int soundId, const juce::String& name) const
    {
        auto sanitized = juce::File::createLegalFileName (name);
        return getFavoritesDirectory().getChildFile (juce::String (soundId) + "_" + sanitized + ".mp3");
    }

    void storeRamCachedMp3 (int soundId, const juce::MemoryBlock& mb)
    {
        if (soundId <= 0 || mb.getSize() == 0)
            return;

        const juce::ScopedLock sl (ramCacheLock);
        if (ramMp3Cache.size() >= 45 && ramMp3Cache.find (soundId) == ramMp3Cache.end())
            ramMp3Cache.erase (ramMp3Cache.begin());

        ramMp3Cache[soundId] = mb;
    }

    bool getRamCachedMp3 (int soundId, juce::MemoryBlock& outBlock) const
    {
        const juce::ScopedLock sl (ramCacheLock);
        auto it = ramMp3Cache.find (soundId);
        if (it != ramMp3Cache.end() && it->second.getSize() > 0)
        {
            outBlock = it->second;
            return true;
        }
        return false;
    }

    // Мгновенно материализует сэмпл из RAM-кэша на диск (при нажатии Get, Сердечка или при Drag-and-Drop)
    juce::File materializeSoundToDisk (SoundResult& sound)
    {
        if (sound.id <= 0)
            return {};

        auto target = getSampleFileFor (sound.id, sound.name);
        if (target.existsAsFile())
        {
            sound.localFile = target;
            if (currentSampleMeta.id == sound.id)
                currentSampleMeta.localFile = target;
            return target;
        }

        juce::MemoryBlock mb;
        if (getRamCachedMp3 (sound.id, mb))
        {
            sampleDirectory.createDirectory();
            if (target.replaceWithData (mb.getData(), mb.getSize()))
            {
                sound.localFile = target;
                if (currentSampleMeta.id == sound.id)
                    currentSampleMeta.localFile = target;
                return target;
            }
        }

        return {};
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

    static juce::ValueTree soundToValueTree (const SoundResult& item)
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
        child.setProperty ("isPinnedRef", item.isPinnedRef, nullptr);
        return child;
    }

    SoundResult valueTreeToSound (const juce::ValueTree& c) const
    {
        SoundResult r;
        r.id = (int) c.getProperty ("id", 0);
        r.name = c.getProperty ("name", "").toString();
        r.duration = (double) c.getProperty ("duration", 0.0);
        r.previewUrl = c.getProperty ("previewUrl", "").toString();

        auto fPath = c.getProperty ("localFile", "").toString();
        if (fPath.isNotEmpty())
        {
            juce::File f (fPath);
            if (f.existsAsFile())
                r.localFile = f;
        }

        if (!r.localFile.existsAsFile())
        {
            auto sampleFile = getSampleFileFor (r.id, r.name);
            auto favCopy = getFavoriteFileFor (r.id, r.name);
            if (sampleFile.existsAsFile())
                r.localFile = sampleFile;
            else if (favCopy.existsAsFile())
                r.localFile = favCopy;
        }

        r.username = c.getProperty ("username", "").toString();
        r.packName = c.getProperty ("packName", "").toString();
        r.formatType = c.getProperty ("formatType", "").toString();
        r.sampleRate = (int) c.getProperty ("sampleRate", 0);
        r.bitDepth = (int) c.getProperty ("bitDepth", 0);
        r.channels = (int) c.getProperty ("channels", 0);
        r.license = c.getProperty ("license", "").toString();
        r.tags.addTokens (c.getProperty ("tags", "").toString(), ",", "");
        r.detectedBpmKey = c.getProperty ("detectedBpmKey", "").toString();
        r.isPinnedRef = (bool) c.getProperty ("isPinnedRef", false);
        return r;
    }

    void saveFavoritesIndex()
    {
        const juce::ScopedLock sl (favLock);
        juce::ValueTree root ("FavoritesList");
        for (const auto& item : favoriteItems)
            root.appendChild (soundToValueTree (item), nullptr);

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
                        auto r = valueTreeToSound (root.getChild (i));
                        r.isPinnedRef = false;
                        if (r.id > 0)
                            favoriteItems.push_back (r);
                    }
                }
            }
        }
    }

    bool isSoundFavorite (int soundId) const
    {
        const juce::ScopedLock sl (favLock);
        for (const auto& item : favoriteItems)
            if (item.id == soundId)
                return true;
        return false;
    }

    void setSoundFavorite (SoundResult& sound, bool shouldBeFav)
    {
        auto favFile = getFavoriteFileFor (sound.id, sound.name);
        {
            const juce::ScopedLock sl (favLock);
            auto it = std::remove_if (favoriteItems.begin(), favoriteItems.end(),
                                      [&] (const SoundResult& s) { return s.id == sound.id; });
            favoriteItems.erase (it, favoriteItems.end());

            if (shouldBeFav)
            {
                if (!sound.localFile.existsAsFile())
                    materializeSoundToDisk (sound);

                SoundResult copy = sound;
                copy.isPinnedRef = false;
                if (sound.localFile.existsAsFile())
                {
                    sound.localFile.copyFileTo (favFile);
                    copy.localFile = sound.localFile;
                }
                favoriteItems.push_back (copy);
            }
            else if (favFile.existsAsFile())
            {
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

    void releaseResources() override { transportSource.releaseResources(); }

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
        juce::ValueTree tree ("FreesoundMiniInstanceState");
        tree.setProperty ("apiToken", savedToken, nullptr);
        tree.setProperty ("saveDirectory", sampleDirectory.getFullPathName(), nullptr);
        tree.setProperty ("query", persistedQuery, nullptr);
        tree.setProperty ("minSlider", persistedMinSlider, nullptr);
        tree.setProperty ("maxSlider", persistedMaxSlider, nullptr);
        tree.setProperty ("channelId", persistedChannelId, nullptr);
        tree.setProperty ("typeId", persistedTypeId, nullptr);
        tree.setProperty ("playbackMode", (int) playbackMode, nullptr);
        tree.setProperty ("pitch", pitchSemitones, nullptr);
        tree.setProperty ("gain", (double) previewGain, nullptr);
        tree.setProperty ("norm", useNormalization, nullptr);
        tree.setProperty ("showingFavs", persistedShowingFavs, nullptr);
        tree.setProperty ("selectedRow", persistedSelectedRow, nullptr);
        tree.setProperty ("currentPage", persistedCurrentPage, nullptr);
        tree.setProperty ("hasMorePages", persistedHasMorePages, nullptr);

        juce::ValueTree resNode ("Results");
        {
            const juce::ScopedLock sl (stateLock);
            int limit = std::min ((int) persistedResults.size(), 120);
            for (int i = 0; i < limit; ++i)
                resNode.appendChild (soundToValueTree (persistedResults[(size_t) i]), nullptr);
        }
        tree.appendChild (resNode, nullptr);

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

                persistedQuery = tree.getProperty ("query", "").toString();
                persistedMinSlider = (double) tree.getProperty ("minSlider", 0.0);
                persistedMaxSlider = (double) tree.getProperty ("maxSlider", 42.0);
                persistedChannelId = (int) tree.getProperty ("channelId", 1);
                persistedTypeId = (int) tree.getProperty ("typeId", 1);
                setPlaybackMode ((PlaybackMode) (int) tree.getProperty ("playbackMode", 0));
                pitchSemitones = (double) tree.getProperty ("pitch", 0.0);
                previewGain = (float) (double) tree.getProperty ("gain", 0.8);
                useNormalization = (bool) tree.getProperty ("norm", true);
                persistedShowingFavs = (bool) tree.getProperty ("showingFavs", false);
                persistedSelectedRow = (int) tree.getProperty ("selectedRow", -1);
                persistedCurrentPage = (int) tree.getProperty ("currentPage", 1);
                persistedHasMorePages = (bool) tree.getProperty ("hasMorePages", false);

                auto resNode = tree.getChildWithName ("Results");
                if (resNode.isValid())
                {
                    const juce::ScopedLock sl (stateLock);
                    persistedResults.clear();
                    for (int i = 0; i < resNode.getNumChildren(); ++i)
                        persistedResults.push_back (valueTreeToSound (resNode.getChild (i)));
                }

                saveGlobalSettings();
            }
        }

        if (savedToken.isEmpty())
            loadGlobalSettings();
    }

    juce::String loadAndPlayFromReader (std::unique_ptr<juce::AudioFormatReader> reader,
                                        const SoundResult& meta,
                                        bool resetPitchToZero)
    {
        if (reader == nullptr)
            return {};

        if (resetPitchToZero)
            pitchSemitones = 0.0;

        transportSource.stop();
        transportSource.setSource (nullptr);
        currentSampleMeta = meta;

        juce::String badgeText;
        int numCh = (int) reader->numChannels;
        int numSamples = (int) reader->lengthInSamples;

        if (numCh > 0 && numSamples > 0)
        {
            juce::AudioBuffer<float> tempBuf (numCh, numSamples);
            reader->read (&tempBuf, 0, numSamples, 0, true, true);

            float peak = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                peak = std::max (peak, tempBuf.getMagnitude (ch, 0, numSamples));

            loadedFilePeak = (peak > 0.0001f) ? peak : 1.0f;
            originalFileSampleRate = reader->sampleRate;

            badgeText = analyzeAudioMusicalBadge (tempBuf, originalFileSampleRate, meta);
            currentSampleMeta.detectedBpmKey = badgeText;

            memorySource->setBuffer (tempBuf);
            memorySource->setReversed (playbackMode == ModeReverse);
            memorySource->setLoopingMode (playbackMode == ModeLoop);

            const juce::ScopedLock sl (bufferLock);
            cachedBuffer = std::move (tempBuf);
        }

        rebuildTransport (0.0, true);
        return badgeText;
    }

    juce::String loadAndPlayFile (const juce::File& file, const SoundResult& meta, bool resetPitchToZero)
    {
        if (!file.existsAsFile())
            return {};

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        SoundResult updatedMeta = meta;
        updatedMeta.localFile = file;
        return loadAndPlayFromReader (std::move (reader), updatedMeta, resetPitchToZero);
    }

    // Загрузка и проигрывание напрямую из оперативной памяти (без записи файла на диск)
    juce::String loadAndPlayMemoryBlock (const juce::MemoryBlock& mb, const SoundResult& meta, bool resetPitchToZero)
    {
        if (mb.getSize() == 0)
            return {};

        auto memStream = std::make_unique<juce::MemoryInputStream> (mb, false);
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (std::move (memStream)));
        return loadAndPlayFromReader (std::move (reader), meta, resetPitchToZero);
    }

    void rebuildTransport (double startPosSeconds, bool autoStart)
    {
        if (memorySource == nullptr || memorySource->getTotalLength() <= 0)
            return;

        bool wasPlaying = transportSource.isPlaying();
        transportSource.stop();
        transportSource.setSource (nullptr);

        double speedRatio = std::pow (2.0, pitchSemitones / 12.0);
        transportSource.setSource (memorySource.get(), 0, nullptr, originalFileSampleRate * speedRatio);

        updateGainInternal();
        transportSource.setPosition (startPosSeconds);

        if (autoStart || wasPlaying)
            transportSource.start();
    }

    void setPlaybackMode (PlaybackMode newMode)
    {
        playbackMode = newMode;
        if (memorySource != nullptr)
        {
            memorySource->setReversed (playbackMode == ModeReverse);
            memorySource->setLoopingMode (playbackMode == ModeLoop);
        }
    }

    PlaybackMode getPlaybackMode() const { return playbackMode; }

    PlaybackMode cyclePlaybackMode()
    {
        int next = ((int) playbackMode + 1) % 3;
        setPlaybackMode ((PlaybackMode) next);
        return playbackMode;
    }

    void togglePlayPause()
    {
        if (transportSource.isPlaying())
            pausePreview();
        else
            playOrResume();
    }

    void playOrResume()
    {
        if (memorySource != nullptr && memorySource->getTotalLength() > 0)
        {
            double len = transportSource.getLengthInSeconds();
            double pos = transportSource.getCurrentPosition();
            if (len > 0.0 && pos >= len - 0.03)
                transportSource.setPosition (0.0);

            transportSource.start();
        }
        else if (currentSampleMeta.localFile.existsAsFile())
        {
            loadAndPlayFile (currentSampleMeta.localFile, currentSampleMeta, false);
        }
    }

    void pausePreview() { transportSource.stop(); }

    void stopPreview()
    {
        transportSource.stop();
        transportSource.setPosition (0.0);
    }

    bool isAudioPlaying() const { return transportSource.isPlaying(); }

    void seekToVisualProportion (float visualProp)
    {
        visualProp = juce::jlimit (0.0f, 1.0f, visualProp);
        if (memorySource == nullptr || memorySource->getTotalLength() <= 0)
            return;

        double length = transportSource.getLengthInSeconds();
        if (length > 0.0)
        {
            float engineProp = (playbackMode == ModeReverse) ? (1.0f - visualProp) : visualProp;
            transportSource.setPosition (length * (double) engineProp);
            transportSource.start();
        }
    }

    float getVisualPlayheadPosition() const
    {
        double length = transportSource.getLengthInSeconds();
        if (length <= 0.0)
            return 0.0f;

        float raw = (float) juce::jlimit (0.0, 1.0, transportSource.getCurrentPosition() / length);
        return (playbackMode == ModeReverse) ? (1.0f - raw) : raw;
    }

    void setGain (float g) { previewGain = g; updateGainInternal(); }
    float getGain() const { return previewGain; }

    void setPitchSemitones (double semitones)
    {
        if (std::abs (pitchSemitones - semitones) < 0.001)
            return;

        pitchSemitones = semitones;
        if (memorySource != nullptr && memorySource->getTotalLength() > 0)
        {
            double pos = transportSource.getCurrentPosition();
            rebuildTransport (pos, false);
        }
    }

    double getPitchSemitones() const { return pitchSemitones; }

    void setNormalization (bool shouldNormalize) { useNormalization = shouldNormalize; updateGainInternal(); }
    bool getNormalization() const { return useNormalization; }

    void copyCachedBuffer (juce::AudioBuffer<float>& dest)
    {
        const juce::ScopedLock sl (bufferLock);
        dest.makeCopyOf (cachedBuffer);
    }

    SoundResult getCurrentSampleMeta() const { return currentSampleMeta; }

    juce::String savedToken;
    juce::File sampleDirectory;

    juce::CriticalSection stateLock;
    std::vector<SoundResult> persistedResults;
    std::vector<SearchHistoryEntry> searchHistory;
    juce::String persistedQuery;
    double persistedMinSlider = 0.0;
    double persistedMaxSlider = 42.0;
    int persistedChannelId = 1;
    int persistedTypeId = 1;
    int persistedCurrentPage = 1;
    bool persistedHasMorePages = false;
    bool persistedShowingFavs = false;
    int persistedSelectedRow = -1;

private:
    void updateGainInternal()
    {
        float normGain = useNormalization ? (0.501187f / loadedFilePeak) : 1.0f;
        transportSource.setGain (previewGain * normGain);
    }

    static juce::String analyzeAudioMusicalBadge (const juce::AudioBuffer<float>& buf, double sr, const SoundResult& meta)
    {
        int numSamples = buf.getNumSamples();
        if (numSamples < 2048 || sr <= 0.0)
            return {};

        const float* data = buf.getReadPointer (0);
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
            int minLag = std::max (8, (int) (sr / 880.0));
            int maxLag = std::min (winSize / 2, (int) (sr / 55.0));

            float r0 = 0.0f;
            for (int i = 0; i < winSize / 2; ++i)
                r0 += data[bestStart + i] * data[bestStart + i];

            float maxCorr = 0.0f;
            int bestLag = 0;

            if (r0 > 0.00001f)
            {
                for (int lag = minLag; lag <= maxLag; ++lag)
                {
                    float corr = 0.0f, rLag = 0.0f;
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

            if (maxCorr >= 0.62f && bestLag > 0)
            {
                double freq = sr / (double) bestLag;
                int midiNote = (int) std::round (69.0 + 12.0 * std::log2 (freq / 440.0));
                static const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
                int pitchClass = ((midiNote % 12) + 12) % 12;
                detectedKey = noteNames[pitchClass];

                juce::String combined = (meta.name + " " + meta.tags.joinIntoString (" ")).toLowerCase();
                if (combined.contains ("minor") || combined.contains ("min"))
                    detectedKey += "m";
            }
        }

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
    PlaybackMode playbackMode = ModeNormal;

    SoundResult currentSampleMeta;
    juce::CriticalSection bufferLock;
    juce::AudioBuffer<float> cachedBuffer;

    mutable juce::CriticalSection ramCacheLock;
    std::map<int, juce::MemoryBlock> ramMp3Cache;

    mutable juce::CriticalSection favLock;
    std::vector<SoundResult> favoriteItems;

    juce::AudioFormatManager formatManager;
    std::unique_ptr<MemoryBufferAudioSource> memorySource;
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

        g.setColour (sage);
        if (isFilled)
            g.fillPath (p);
        else
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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

        g.setColour (accent);
        juce::String labelText = isConnected ? (isMouseOver ? "API OK (Edit)" : "API OK") : "NO API";
        g.drawText (labelText, (int) (cx + 10.0f), 0, getWidth() - (int) (cx + 14.0f), getHeight(), juce::Justification::centredLeft);
    }

private:
    bool isConnected = false;
};

//==============================================================================
// Сдвоенный блок управления папкой [ Выбрать папку | Открыть в Finder ] под единой рамкой
class SegmentedFolderControl : public juce::Component
{
public:
    std::function<void()> onChooseFolder;
    std::function<void()> onOpenFolder;

    void mouseMove (const juce::MouseEvent& e) override
    {
        int newHalf = (e.x < getWidth() / 2) ? 1 : 2;
        if (hoveredHalf != newHalf)
        {
            hoveredHalf = newHalf;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoveredHalf = 0;
        pressedHalf = 0;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressedHalf = (e.x < getWidth() / 2) ? 1 : 2;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        int releasedHalf = (e.x < getWidth() / 2) ? 1 : 2;
        if (pressedHalf == releasedHalf && getLocalBounds().contains (e.getPosition()))
        {
            if (releasedHalf == 1 && onChooseFolder)
                onChooseFolder();
            else if (releasedHalf == 2 && onOpenFolder)
                onOpenFolder();
        }
        pressedHalf = 0;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        float midX = b.getCentreX();

        g.setColour (juce::Colour (0xff16181d));
        g.fillRoundedRectangle (b, 7.0f);

        if (hoveredHalf == 1)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (0, 0, getWidth() / 2, getHeight());
            g.setColour (pressedHalf == 1 ? juce::Colour (0xff24332c) : juce::Colour (0xff1e2229));
            g.fillRoundedRectangle (b, 7.0f);
        }
        else if (hoveredHalf == 2)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (getWidth() / 2, 0, getWidth() - getWidth() / 2, getHeight());
            g.setColour (pressedHalf == 2 ? juce::Colour (0xff24332c) : juce::Colour (0xff1e2229));
            g.fillRoundedRectangle (b, 7.0f);
        }

        g.setColour (hoveredHalf != 0 ? juce::Colour (0xff52b788).withAlpha (0.55f) : juce::Colour (0xff262930));
        g.drawRoundedRectangle (b, 7.0f, 1.0f);

        g.setColour (juce::Colour (0xff262930));
        g.drawVerticalLine ((int) std::round (midX), b.getY() + 4.0f, b.getBottom() - 4.0f);

        // Левая половина: Иконка папки
        {
            float cx = b.getX() + b.getWidth() * 0.25f;
            float cy = b.getCentreY();
            float s = b.getHeight() * 0.27f;

            juce::Colour col = (pressedHalf == 1) ? juce::Colour (0xff52b788)
                             : (hoveredHalf == 1 ? juce::Colour (0xff74c69d) : juce::Colours::lightgrey);
            g.setColour (col);

            juce::Path p;
            p.startNewSubPath (cx - s * 1.1f, cy - s * 0.7f);
            p.lineTo (cx - s * 0.2f, cy - s * 0.7f);
            p.lineTo (cx + s * 0.1f, cy - s * 0.35f);
            p.lineTo (cx + s * 1.1f, cy - s * 0.35f);
            p.lineTo (cx + s * 1.1f, cy + s * 0.8f);
            p.lineTo (cx - s * 1.1f, cy + s * 0.8f);
            p.closeSubPath();
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Правая половина: Иконка открытия в Finder
        {
            float cx = b.getX() + b.getWidth() * 0.75f;
            float cy = b.getCentreY();
            float s = b.getHeight() * 0.27f;

            juce::Colour col = (pressedHalf == 2) ? juce::Colour (0xff52b788)
                             : (hoveredHalf == 2 ? juce::Colour (0xff74c69d) : juce::Colours::lightgrey);
            g.setColour (col);

            juce::Path p;
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
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

private:
    int hoveredHalf = 0;
    int pressedHalf = 0;
};

//==============================================================================
class IconButton : public juce::Button
{
public:
    enum Type { PlayPause, Stop, Dice2D, HashTag, Heart, BackArrow, ModeCycle, Info, Close };

    explicit IconButton (Type t, bool noBorder = false)
        : juce::Button (""), iconType (t), frameless (noBorder) {}

    void setActiveState (bool active)
    {
        if (isActiveState != active)
        {
            isActiveState = active;
            repaint();
        }
    }

    bool getActiveState() const { return isActiveState; }

    void setModeIndex (int idx)
    {
        if (modeIndex != idx)
        {
            modeIndex = idx;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);

        if (!frameless)
        {
            juce::Colour bg = juce::Colour (0xff16181d);
            if (isButtonDown)      bg = juce::Colour (0xff24332c);
            else if (isMouseOver)  bg = juce::Colour (0xff1e2229);

            g.setColour (bg);
            g.fillRoundedRectangle (b, 7.0f);

            bool highlightBorder = isMouseOver || isActiveState || (iconType == ModeCycle && modeIndex != 0);
            g.setColour (highlightBorder ? juce::Colour (0xff52b788).withAlpha (0.55f) : juce::Colour (0xff262930));
            g.drawRoundedRectangle (b, 7.0f, 1.0f);
        }

        if (iconType == Heart)
        {
            HeartRenderer::drawHeart (g, b.reduced (5.5f), isActiveState, isMouseOver);
            return;
        }

        juce::Colour iconCol = juce::Colours::lightgrey;
        if (frameless)
        {
            if (iconType == HashTag)
                iconCol = isMouseOver ? juce::Colour (0xff52b788) : juce::Colours::grey;
            else
                iconCol = isButtonDown ? juce::Colour (0xff52b788)
                        : (isMouseOver ? juce::Colours::white : juce::Colours::white.withAlpha (0.9f));
        }
        else
        {
            bool activeTint = isButtonDown || isActiveState || (iconType == ModeCycle && modeIndex != 0);
            iconCol = activeTint ? juce::Colour (0xff52b788)
                    : (isMouseOver ? juce::Colour (0xff74c69d) : juce::Colours::lightgrey);
        }

        if (!isEnabled())
            iconCol = iconCol.withAlpha (0.28f);

        g.setColour (iconCol);

        auto cx = b.getCentreX();
        auto cy = b.getCentreY();
        auto s = std::min (b.getWidth(), b.getHeight()) * (frameless ? 0.32f : 0.28f);

        juce::Path p;
        switch (iconType)
        {
            case PlayPause:
                if (isActiveState)
                {
                    g.fillRoundedRectangle (cx - s * 0.8f, cy - s * 0.95f, s * 0.52f, s * 1.9f, 1.5f);
                    g.fillRoundedRectangle (cx + s * 0.28f, cy - s * 0.95f, s * 0.52f, s * 1.9f, 1.5f);
                }
                else
                {
                    p.addTriangle (cx - s * 0.65f, cy - s * 0.95f, cx - s * 0.65f, cy + s * 0.95f, cx + s * 1.05f, cy);
                    g.fillPath (p);
                }
                break;

            case Stop:
                g.fillRoundedRectangle (cx - s * 0.82f, cy - s * 0.82f, s * 1.64f, s * 1.64f, 2.0f);
                break;

            case BackArrow:
                p.startNewSubPath (cx + s * 0.8f, cy);
                p.lineTo (cx - s * 0.8f, cy);
                p.startNewSubPath (cx - s * 0.2f, cy - s * 0.65f);
                p.lineTo (cx - s * 0.8f, cy);
                p.lineTo (cx - s * 0.2f, cy + s * 0.65f);
                g.strokePath (p, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;

            case ModeCycle:
                if (modeIndex == 0)
                {
                    p.startNewSubPath (cx - s * 0.95f, cy);
                    p.lineTo (cx + s * 0.85f, cy);
                    p.startNewSubPath (cx + s * 0.25f, cy - s * 0.6f);
                    p.lineTo (cx + s * 0.85f, cy);
                    p.lineTo (cx + s * 0.25f, cy + s * 0.6f);
                    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                else if (modeIndex == 1)
                {
                    p.startNewSubPath (cx + s * 0.95f, cy);
                    p.lineTo (cx - s * 0.85f, cy);
                    p.startNewSubPath (cx - s * 0.25f, cy - s * 0.6f);
                    p.lineTo (cx - s * 0.85f, cy);
                    p.lineTo (cx - s * 0.25f, cy + s * 0.6f);
                    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                else
                {
                    p.addCentredArc (cx, cy, s * 0.85f, s * 0.85f, 0.0f, 0.4f, juce::MathConstants<float>::twoPi - 0.35f, true);
                    p.startNewSubPath (cx + s * 0.3f, cy - s * 0.95f);
                    p.lineTo (cx + s * 0.85f, cy - s * 0.55f);
                    p.lineTo (cx + s * 0.2f, cy - s * 0.35f);
                    g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                break;

            case Dice2D:
            {
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
                float d = s * 0.38f;
                float len = s * 0.78f;
                p.startNewSubPath (cx - d + 0.8f, cy - len);
                p.lineTo (cx - d - 0.8f, cy + len);
                p.startNewSubPath (cx + d + 0.8f, cy - len);
                p.lineTo (cx + d - 0.8f, cy + len);
                p.startNewSubPath (cx - len, cy - d);
                p.lineTo (cx + len, cy - d);
                p.startNewSubPath (cx - len, cy + d);
                p.lineTo (cx + len, cy + d);
                g.strokePath (p, juce::PathStrokeType (1.35f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }

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
    bool frameless = false;
    bool isActiveState = false;
    int modeIndex = 0;
};

//==============================================================================
class SamplePassportPopup : public juce::Component
{
public:
    SamplePassportPopup (const SoundResult& s,
                         std::function<void(const juce::String&)> tagCallback,
                         std::function<void(const SoundResult&)> similarCallback)
        : sound (s),
          onTagClick (std::move (tagCallback)),
          onSimilarClick (std::move (similarCallback))
    {
        if (sound.license.isNotEmpty())
        {
            juce::String shortLic = "CC License";
            if (sound.license.containsIgnoreCase ("zero") || sound.license.contains ("cc0"))
                shortLic = "CC0 (Public Domain)";
            else if (sound.license.contains ("by-nc"))
                shortLic = "CC BY-NC";
            else if (sound.license.contains ("by"))
                shortLic = "CC BY";

            licenseLink.setButtonText (shortLic);
            licenseLink.setURL (juce::URL (sound.license));
            licenseLink.setJustificationType (juce::Justification::centredLeft);
            licenseLink.setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xff52b788));
            addAndMakeVisible (licenseLink);
        }

        similarBtn.setButtonText ("Show Similar Sounds");
        similarBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1d2b24));
        similarBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff52b788));
        similarBtn.onClick = [this] {
            auto sCopy = sound;
            auto cb = onSimilarClick;
            if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
                callout->dismiss();
            if (cb)
                cb (sCopy);
        };
        addAndMakeVisible (similarBtn);

        int maxTags = std::min (6, sound.tags.size());
        for (int i = 0; i < maxTags; ++i)
        {
            auto tagStr = sound.tags[i].trim();
            if (tagStr.isNotEmpty())
            {
                auto btn = std::make_unique<juce::TextButton> ("#" + tagStr);
                btn->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff181b22));
                btn->setColour (juce::TextButton::textColourOffId, juce::Colours::lightgrey);
                btn->onClick = [this, tagStr] {
                    auto cb = onTagClick;
                    if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
                        callout->dismiss();
                    if (cb)
                        cb (tagStr);
                };
                addAndMakeVisible (btn.get());
                tagButtons.push_back (std::move (btn));
            }
        }

        setSize (215, 168);
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (12.0f);
        int pad = 10;
        int w = getWidth() - pad * 2;

        g.setColour (juce::Colours::white);
        g.drawText (sound.name, pad, 8, w, 16, juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::grey);
        g.drawText ("@" + (sound.username.isNotEmpty() ? sound.username : "unknown") + " | #" + juce::String (sound.id),
                    pad, 25, w, 15, juce::Justification::centredLeft, true);

        juce::String specs = (sound.formatType.isNotEmpty() ? sound.formatType.toUpperCase() : "AUDIO");
        if (sound.sampleRate > 0)
            specs += " | " + juce::String (sound.sampleRate / 1000.0, 1) + "k";
        if (sound.bitDepth > 0)
            specs += " | " + juce::String (sound.bitDepth) + "b";
        if (sound.channels > 0)
            specs += " | " + juce::String (sound.channels == 1 ? "Mono" : "Stereo");

        g.setColour (juce::Colours::lightgrey);
        g.drawText (specs, pad, 42, w, 15, juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::grey);
        g.drawText ("License:", pad, 59, 48, 16, juce::Justification::centredLeft);
    }

    void resized() override
    {
        int pad = 10;
        int w = getWidth() - pad * 2;

        if (licenseLink.isVisible())
            licenseLink.setBounds (pad + 48, 59, w - 48, 16);

        similarBtn.setBounds (pad, 80, w, 22);

        int x = pad;
        int y = 108;
        int maxX = getWidth() - pad;

        for (auto& btn : tagButtons)
        {
            int bw = std::min (95, std::max (42, (int) btn->getButtonText().length() * 6 + 12));
            if (x + bw > maxX)
            {
                x = pad;
                y += 23;
            }
            if (y + 19 <= getHeight() - 5)
                btn->setBounds (x, y, bw, 19);
            else
                btn->setVisible (false);

            x += bw + 4;
        }
    }

private:
    SoundResult sound;
    std::function<void(const juce::String&)> onTagClick;
    std::function<void(const SoundResult&)> onSimilarClick;
    juce::HyperlinkButton licenseLink;
    juce::TextButton similarBtn;
    std::vector<std::unique_ptr<juce::TextButton>> tagButtons;
};

//==============================================================================
class WaveformDisplay : public juce::Component,
                        private juce::Timer
{
public:
    std::function<void(float)> onSeekRequested;
    std::function<void()> onDragged;
    std::function<float()> getPlayheadPos;
    std::function<bool()> getIsPlaying;
    std::function<void()> onSampleInfoClicked;
    std::function<void()> onSampleFavClicked;
    std::function<void()> onModeCycleClicked;
    std::function<void()> onPlaybackStateChanged;

    WaveformDisplay()
    {
        modeCycleBtn.onClick = [this] { if (onModeCycleClicked) onModeCycleClicked(); };
        addChildComponent (modeCycleBtn);

        sampleFavBtn.onClick = [this] { if (onSampleFavClicked) onSampleFavClicked(); };
        addChildComponent (sampleFavBtn);

        sampleInfoBtn.onClick = [this] { if (onSampleInfoClicked) onSampleInfoClicked(); };
        addChildComponent (sampleInfoBtn);

        startTimerHz (30);
    }

    ~WaveformDisplay() override { stopTimer(); }

    void setWaveformData (const juce::AudioBuffer<float>& newBuffer,
                          const juce::String& sampleTitle,
                          const juce::String& bpmKeyBadge,
                          bool isFav,
                          int modeIdx)
    {
        localBuffer.makeCopyOf (newBuffer);
        currentTitle = sampleTitle;
        currentBadge = bpmKeyBadge;
        sampleFavBtn.setActiveState (isFav);
        modeCycleBtn.setModeIndex (modeIdx);

        bool hasAudio = (localBuffer.getNumSamples() > 0);
        modeCycleBtn.setVisible (hasAudio);
        sampleFavBtn.setVisible (hasAudio);
        sampleInfoBtn.setVisible (hasAudio);

        repaint();
    }

    void updateFavoriteState (bool isFav) { sampleFavBtn.setActiveState (isFav); }
    void updateModeState (int modeIdx)    { modeCycleBtn.setModeIndex (modeIdx); }

    void resized() override
    {
        modeCycleBtn.setBounds (8, 6, 24, 22);
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

    void mouseUp (const juce::MouseEvent&) override { dragStarted = false; }

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

            topPoints.push_back (midY - (maxVal * midY * 0.74f));
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

        float cursorX = lastCursorProp * (float) width;
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawVerticalLine ((int) std::round (cursorX), 4.0f, (float) height - 20.0f);
        g.fillEllipse (cursorX - 3.0f, 3.0f, 6.0f, 6.0f);

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
        if (getIsPlaying)
        {
            bool playingNow = getIsPlaying();
            if (playingNow != lastPlayingState)
            {
                lastPlayingState = playingNow;
                if (onPlaybackStateChanged)
                    onPlaybackStateChanged();
            }
        }

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
    bool lastPlayingState = false;
    bool dragStarted = false;

    IconButton modeCycleBtn  { IconButton::ModeCycle };
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

        float arrowX = (float) width - 14.0f;
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
        label.setBounds (8, 1, box.getWidth() - 22, box.getHeight() - 2);
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
        p.addRectangle (-1.0f, -pointerLength, 2.0f, pointerLength * 0.55f);
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

    void drawCallOutBoxBackground (juce::CallOutBox& box, juce::Graphics& g,
                                   const juce::Path&, juce::Image&) override
    {
        auto b = box.getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff121418));
        g.fillRoundedRectangle (b, 10.0f);
        g.setColour (juce::Colour (0xff52b788).withAlpha (0.55f));
        g.drawRoundedRectangle (b, 10.0f, 1.0f);
    }

    int getCallOutBoxBorderSize (const juce::CallOutBox&) override
    {
        return 2;
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
            "2. Clean Disk & RAM Preview:\n"
            "   Clicking any row previews audio in RAM without cluttering your disk.\n"
            "   Files save to disk ONLY when you click 'Get', Heart, or Drag to Ableton.\n\n"
            "3. Transport & Shortcuts:\n"
            "   Single Space: Play/Pause. Double Space: Stop. Up/Down: Navigate.\n\n"
            "4. Right-Click Menu:\n"
            "   Right-click any row for Show Similar (pins [REF]) or Open in Browser.";

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
                            private juce::Thread,
                            private juce::Timer
{
public:
    explicit FreesoundMiniEditor (FreesoundMiniAudioProcessor& p)
        : AudioProcessorEditor (&p), Thread ("FreesoundSearchThread"), proc (p)
    {
        setLookAndFeel (&modernLF);
        setWantsKeyboardFocus (true);

        if (proc.savedToken.isEmpty())
            proc.loadGlobalSettings();

        isEditingToken = (proc.savedToken.trim().length() <= 5);

        tokenInput.setTextToShowWhenEmpty ("Paste API Key & press Enter...", juce::Colours::grey);
        tokenInput.setJustification (juce::Justification::centredLeft);
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

        // Сдвоенный блок [ Выбрать папку | Открыть в Finder ]
        folderControl.onChooseFolder = [this] {
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
        folderControl.onOpenFolder = [this] {
            if (showingFavoritesOnly)
                proc.getFavoritesDirectory().startAsProcess();
            else
                proc.sampleDirectory.startAsProcess();
        };
        addAndMakeVisible (folderControl);

        infoButton.onClick = [this] {
            if (infoOverlay != nullptr)
            {
                infoOverlay->setBounds (getLocalBounds());
                infoOverlay->setVisible (true);
                infoOverlay->toFront (true);
            }
        };
        addAndMakeVisible (infoButton);

        backHistoryButton.onClick = [this] { restorePreviousSearchFromHistory(); };
        backHistoryButton.setEnabled (!proc.searchHistory.empty());
        addAndMakeVisible (backHistoryButton);

        // Поле поиска (поиск по Enter, без лишней кнопки лупы)
        searchInput.setTextToShowWhenEmpty ("Search sounds (press Enter)...", juce::Colours::grey);
        searchInput.setJustification (juce::Justification::centredLeft);
        searchInput.setIndents (6, 4);
        searchInput.setBorder (juce::BorderSize<int> (1, 6, 1, 28));
        searchInput.setText (proc.persistedQuery, false);
        searchInput.onReturnKey = [this] { startNewSearch (true); };
        addAndMakeVisible (searchInput);

        tagMenuButton.onClick = [this] { showCuratedTagsMenu(); };
        addAndMakeVisible (tagMenuButton);

        randomDiceButton.onClick = [this] { startPureRandomSearch(); };
        addAndMakeVisible (randomDiceButton);

        favFilterButton.onClick = [this] { toggleFavoritesView(); };
        addAndMakeVisible (favFilterButton);

        channelBox.addItem ("All Ch", 1);
        channelBox.addItem ("Mono", 2);
        channelBox.addItem ("Stereo", 3);
        channelBox.setSelectedId (proc.persistedChannelId, juce::dontSendNotification);
        channelBox.onChange = [this] {
            proc.persistedChannelId = channelBox.getSelectedId();
            triggerAutoSearchFromFilter();
        };
        addAndMakeVisible (channelBox);

        typeBox.addItem ("All Types", 1);
        typeBox.addItem ("One-Shots", 2);
        typeBox.addItem ("Loops", 3);
        typeBox.setSelectedId (proc.persistedTypeId, juce::dontSendNotification);
        typeBox.onChange = [this] {
            proc.persistedTypeId = typeBox.getSelectedId();
            triggerAutoSearchFromFilter();
        };
        addAndMakeVisible (typeBox);

        durRangeSlider.setSliderStyle (juce::Slider::TwoValueHorizontal);
        durRangeSlider.setRange (0.0, 100.0, 0.1);
        durRangeSlider.setMinAndMaxValues (proc.persistedMinSlider, proc.persistedMaxSlider, juce::dontSendNotification);
        durRangeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        durRangeSlider.onValueChange = [this] {
            if (durRangeSlider.getMinValue() > durRangeSlider.getMaxValue())
                durRangeSlider.setMinValue (durRangeSlider.getMaxValue(), juce::dontSendNotification);
            proc.persistedMinSlider = durRangeSlider.getMinValue();
            proc.persistedMaxSlider = durRangeSlider.getMaxValue();
            updateDurationLabel();
            startTimer (280);
        };
        addAndMakeVisible (durRangeSlider);

        durValueLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        durValueLabel.setJustificationType (juce::Justification::centredRight);
        durValueLabel.setBorderSize (juce::BorderSize<int> (0));
        updateDurationLabel();
        addAndMakeVisible (durValueLabel);

        table.setModel (this);
        table.getHeader().addColumn ("", 1, 28, 28, 28, juce::TableHeaderComponent::notResizable);
        table.getHeader().addColumn ("NAME", 2, 185, 100, 800, juce::TableHeaderComponent::defaultFlags);
        table.getHeader().addColumn ("DUR", 3, 58, 50, 80, juce::TableHeaderComponent::defaultFlags);
        table.getHeader().addColumn ("STATUS", 4, 56, 50, 70, juce::TableHeaderComponent::notSortable);
        table.setMultipleSelectionEnabled (true);
        addAndMakeVisible (table);

        waveformDisplay.onSeekRequested = [this] (float proportion) {
            proc.seekToVisualProportion (proportion);
            playPauseButton.setActiveState (proc.isAudioPlaying());
        };
        waveformDisplay.onDragged = [this] {
            startExternalDragFromWaveform();
        };
        waveformDisplay.getPlayheadPos = [this] {
            return proc.getVisualPlayheadPosition();
        };
        waveformDisplay.getIsPlaying = [this] {
            return proc.isAudioPlaying();
        };
        waveformDisplay.onPlaybackStateChanged = [this] {
            playPauseButton.setActiveState (proc.isAudioPlaying());
        };
        waveformDisplay.onModeCycleClicked = [this] {
            auto newMode = proc.cyclePlaybackMode();
            waveformDisplay.updateModeState ((int) newMode);
            if (newMode == FreesoundMiniAudioProcessor::ModeNormal)
                statusLabel.setText ("Mode: Normal playback (->)", juce::dontSendNotification);
            else if (newMode == FreesoundMiniAudioProcessor::ModeReverse)
                statusLabel.setText ("Mode: Reverse playback (<-)", juce::dontSendNotification);
            else
                statusLabel.setText ("Mode: Loop playback", juce::dontSendNotification);
        };
        waveformDisplay.onSampleFavClicked = [this] {
            auto cur = proc.getCurrentSampleMeta();
            if (cur.id > 0)
            {
                bool nowFav = !proc.isSoundFavorite (cur.id);
                proc.setSoundFavorite (cur, nowFav);

                if (cur.localFile.existsAsFile())
                    updateRowLocalFile (cur.id, cur.localFile);

                waveformDisplay.updateFavoriteState (nowFav);
                table.repaint();
            }
        };
        waveformDisplay.onSampleInfoClicked = [this] {
            auto cur = proc.getCurrentSampleMeta();
            if (cur.id > 0)
                launchFloatingSamplePopup (cur);
        };
        addAndMakeVisible (waveformDisplay);

        playPauseButton.onClick = [this] {
            proc.togglePlayPause();
            playPauseButton.setActiveState (proc.isAudioPlaying());
        };
        addAndMakeVisible (playPauseButton);

        stopButton.onClick = [this] {
            proc.stopPreview();
            playPauseButton.setActiveState (false);
        };
        addAndMakeVisible (stopButton);

        pitchSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pitchSlider.setRange (-12.0, 12.0, 1.0);
        pitchSlider.setValue (proc.getPitchSemitones(), juce::dontSendNotification);
        pitchSlider.setDoubleClickReturnValue (true, 0.0);
        pitchSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pitchSlider.onValueChange = [this] { proc.setPitchSemitones (pitchSlider.getValue()); };
        addAndMakeVisible (pitchSlider);

        normCheckbox.setButtonText ("Norm -6dB");
        normCheckbox.setToggleState (proc.getNormalization(), juce::dontSendNotification);
        normCheckbox.onClick = [this] { proc.setNormalization (normCheckbox.getToggleState()); };
        addAndMakeVisible (normCheckbox);

        volSlider.setSliderStyle (juce::Slider::LinearBar);
        volSlider.setRange (0.0, 1.0, 0.01);
        volSlider.setValue (proc.getGain(), juce::dontSendNotification);
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

        infoOverlay = std::make_unique<InfoOverlay> ([this] { infoOverlay->setVisible (false); });
        infoOverlay->setVisible (false);
        addChildComponent (infoOverlay.get());

        {
            const juce::ScopedLock sl (proc.stateLock);
            results = proc.persistedResults;
            currentPage = proc.persistedCurrentPage;
            hasMorePages = proc.persistedHasMorePages;
            showingFavoritesOnly = proc.persistedShowingFavs;
            lastTriggeredRow = proc.persistedSelectedRow;
        }
        favFilterButton.setActiveState (showingFavoritesOnly);
        table.updateContent();
        if (lastTriggeredRow >= 0 && lastTriggeredRow < getNumRows())
            table.selectRow (lastTriggeredRow, false, true);

        if (!results.empty())
            statusLabel.setText ("Restored: " + juce::String (getNumRows()) + " sounds", juce::dontSendNotification);

        updateTopBarVisibility();
        refreshWaveformFromProcessor();
        playPauseButton.setActiveState (proc.isAudioPlaying());

        setResizable (true, true);
        setResizeLimits (370, 540, 1100, 1200);
        setSize (400, 620);
    }

    ~FreesoundMiniEditor() override
    {
        stopTimer();
        signalThreadShouldExit();
        stopThread (4000);
        setLookAndFeel (nullptr);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::spaceKey && !searchInput.hasKeyboardFocus (false) && !tokenInput.hasKeyboardFocus (false))
        {
            double now = juce::Time::getMillisecondCounterHiRes();
            if (now - lastSpaceBarTimeMs < 320.0)
            {
                proc.stopPreview();
                playPauseButton.setActiveState (false);
                statusLabel.setText ("Stopped (0.0s)", juce::dontSendNotification);
            }
            else
            {
                proc.togglePlayPause();
                playPauseButton.setActiveState (proc.isAudioPlaying());
            }
            lastSpaceBarTimeMs = now;
            return true;
        }

        if (key == juce::KeyPress::downKey && getNumRows() > 0)
        {
            int nextRow = juce::jlimit (0, getNumRows() - 1, table.getSelectedRow() + 1);
            table.selectRow (nextRow);
            return true;
        }

        if (key == juce::KeyPress::upKey && getNumRows() > 0)
        {
            int prevRow = juce::jlimit (0, getNumRows() - 1, table.getSelectedRow() - 1);
            table.selectRow (prevRow);
            return true;
        }

        return AudioProcessorEditor::keyPressed (key);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0b0d));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        // Верхняя шапка: Статус API слева, сдвоенный блок [ Папка | Открыть ] + (i) справа
        auto topBar = area.removeFromTop (28);
        infoButton.setBounds (topBar.removeFromRight (28));
        topBar.removeFromRight (5);
        folderControl.setBounds (topBar.removeFromRight (56));
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

        // Строка поиска: Назад (<-) + Поле поиска (с # внутри) + Кубик + Сердце
        auto searchRow = area.removeFromTop (30);
        favFilterButton.setBounds (searchRow.removeFromRight (30));
        searchRow.removeFromRight (4);
        randomDiceButton.setBounds (searchRow.removeFromRight (30));
        searchRow.removeFromRight (6);

        backHistoryButton.setBounds (searchRow.removeFromLeft (28));
        searchRow.removeFromLeft (4);

        searchInput.setBounds (searchRow);
        tagMenuButton.setBounds (searchInput.getRight() - 25, searchInput.getY() + 3, 22, 24);
        tagMenuButton.toFront (false);

        area.removeFromTop (6);

        auto filterRow = area.removeFromTop (24);
        channelBox.setBounds (filterRow.removeFromLeft (72));
        filterRow.removeFromLeft (5);
        typeBox.setBounds (filterRow.removeFromLeft (86));
        filterRow.removeFromLeft (6);
        durValueLabel.setBounds (filterRow.removeFromRight (88));
        filterRow.removeFromRight (4);
        durRangeSlider.setBounds (filterRow);

        area.removeFromTop (8);

        auto bottomControl = area.removeFromBottom (48);
        area.removeFromBottom (6);

        int waveHeight = juce::jlimit (82, 175, getHeight() / 6);
        waveformDisplay.setBounds (area.removeFromBottom (waveHeight));

        area.removeFromBottom (4);
        statusLabel.setBounds (area.removeFromBottom (18));

        table.setBounds (area);
        int nameColW = std::max (120, table.getWidth() - (28 + 58 + 56 + 18));
        table.getHeader().setColumnWidth (2, nameColW);

        auto bArea = bottomControl;
        playPauseButton.setBounds (bArea.removeFromLeft (30).withSizeKeepingCentre (28, 28));
        bArea.removeFromLeft (2);
        stopButton.setBounds (bArea.removeFromLeft (30).withSizeKeepingCentre (28, 28));

        bArea.removeFromLeft (8);
        pitchSlider.setBounds (bArea.removeFromLeft (54));

        bArea.removeFromLeft (6);
        normCheckbox.setBounds (bArea.removeFromLeft (80));

        bArea.removeFromLeft (6);
        volLabel.setBounds (bArea.removeFromLeft (26));
        volSlider.setBounds (bArea.reduced (0, 14));

        if (infoOverlay != nullptr)
            infoOverlay->setBounds (getLocalBounds());
    }

    int getNumRows() override
    {
        const juce::ScopedLock sl (resultsLock);
        return static_cast<int> (results.size());
    }

    void paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected) override
    {
        bool isPinned = false;
        {
            const juce::ScopedLock sl (resultsLock);
            if (rowNumber >= 0 && rowNumber < static_cast<int> (results.size()))
                isPinned = results[static_cast<size_t> (rowNumber)].isPinnedRef;
        }

        if (isPinned)
        {
            g.fillAll (rowIsSelected ? juce::Colour (0xff23382e) : juce::Colour (0xff17261f));
            g.setColour (juce::Colour (0xff52b788).withAlpha (0.5f));
            g.drawHorizontalLine (height - 1, 0.0f, (float) width);
        }
        else if (rowIsSelected)
        {
            g.fillAll (juce::Colour (0xff1d2b24));
        }
        else
        {
            g.fillAll (juce::Colour (0xff101216));
        }
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
            int textX = 6;
            if (item.isPinnedRef)
            {
                juce::Rectangle<float> pinBadge (4.0f, (float) (height / 2 - 8), 34.0f, 16.0f);
                g.setColour (juce::Colour (0xff52b788));
                g.fillRoundedRectangle (pinBadge, 4.0f);
                g.setColour (juce::Colour (0xff0a0b0d));
                g.drawText ("REF", pinBadge.toNearestInt(), juce::Justification::centred, false);
                textX = 44;
            }

            g.setColour (item.isPinnedRef ? juce::Colour (0xff74c69d) : juce::Colours::white);
            g.drawText (item.name, textX, 0, width - textX - 6, height, juce::Justification::centredLeft, true);
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
                g.drawText ("Get", 0, 0, width, height, juce::Justification::centred);
            }
        }
    }

    void sortOrderChanged (int newSortColumnId, bool isForwards) override
    {
        if (newSortColumnId == 0)
            return;

        {
            const juce::ScopedLock sl (resultsLock);
            std::stable_sort (results.begin(), results.end(), [newSortColumnId, isForwards] (const SoundResult& a, const SoundResult& b) {
                if (a.isPinnedRef != b.isPinnedRef)
                    return a.isPinnedRef;
                if (newSortColumnId == 3)
                    return isForwards ? (a.duration < b.duration) : (a.duration > b.duration);
                if (newSortColumnId == 2)
                    return isForwards ? (a.name.compareIgnoreCase (b.name) < 0) : (a.name.compareIgnoreCase (b.name) > 0);
                return false;
            });
        }
        syncStateToProcessor();
        table.updateContent();
        table.repaint();
    }

    // При перетаскивании из таблицы мгновенно материализует выбранные звуки из RAM-кэша на диск
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
                    auto& item = results[static_cast<size_t> (r)];
                    if (!item.localFile.existsAsFile())
                        proc.materializeSoundToDisk (item);

                    if (item.localFile.existsAsFile())
                        filesToDrag.addIfNotAlreadyThere (item.localFile.getFullPathName());
                }
            }
        }

        syncStateToProcessor();
        table.repaint();

        if (!filesToDrag.isEmpty())
            performExternalDragDropOfFiles (filesToDrag, false);

        return {};
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        // При мультивыборе подгружаем превью только в RAM-кэш (диск не засоряется до реального перетаскивания)
        auto selected = table.getSelectedRows();
        if (selected.size() > 1)
        {
            for (int i = 0; i < selected.size(); ++i)
                ensureRowInRamQuietly (selected[i]);
        }

        if (lastRowSelected >= 0 && lastRowSelected != lastTriggeredRow)
        {
            lastTriggeredRow = lastRowSelected;
            proc.persistedSelectedRow = lastRowSelected;
            pitchSlider.setValue (0.0, juce::dontSendNotification);
            triggerPreviewInRamOrPlay (lastRowSelected);
        }
    }

    void cellClicked (int rowNumber, int columnId, const juce::MouseEvent& e) override
    {
        if (rowNumber < 0)
            return;

        SoundResult item;
        {
            const juce::ScopedLock sl (resultsLock);
            if (rowNumber >= static_cast<int> (results.size()))
                return;
            item = results[static_cast<size_t> (rowNumber)];
        }

        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Show Similar Sounds");
            m.addItem (2, "Open on Freesound.org");

            m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (e.getScreenX(), e.getScreenY(), 2, 2)),
                             [this, item] (int res) {
                                 if (res == 1)
                                     startSimilarSearchWithPinnedRef (item);
                                 else if (res == 2 && item.id > 0)
                                     juce::URL ("https://freesound.org/s/" + juce::String (item.id) + "/").launchInDefaultBrowser();
                             });
            return;
        }

        // Колонка 1: Сердечко (сохраняет на диск и в папку Favorites)
        if (columnId == 1)
        {
            bool makeFav = !proc.isSoundFavorite (item.id);
            proc.setSoundFavorite (item, makeFav);

            if (item.localFile.existsAsFile())
                updateRowLocalFile (item.id, item.localFile);
            else if (makeFav)
                fetchSoundAsync (rowNumber, false, true);

            if (proc.getCurrentSampleMeta().id == item.id)
                waveformDisplay.updateFavoriteState (makeFav);

            if (showingFavoritesOnly && !makeFav)
                toggleFavoritesView (true);
            else
                table.repaintRow (rowNumber);
            return;
        }

        // Колонка 4 (STATUS): Явное сохранение выбранного сэмпла на диск по клику на "Get"
        if (columnId == 4 && !item.localFile.existsAsFile())
        {
            auto savedFile = proc.materializeSoundToDisk (item);
            if (savedFile.existsAsFile())
            {
                updateRowLocalFile (item.id, savedFile);
                table.repaintRow (rowNumber);
                statusLabel.setText ("Saved to disk: " + savedFile.getFileName(), juce::dontSendNotification);
            }
            else
            {
                fetchSoundAsync (rowNumber, true, true);
            }
            return;
        }

        if (table.getNumSelectedRows() <= 1)
        {
            lastTriggeredRow = rowNumber;
            proc.persistedSelectedRow = rowNumber;
            pitchSlider.setValue (0.0, juce::dontSendNotification);
            triggerPreviewInRamOrPlay (rowNumber);
        }
    }

    void applySearchResults (std::vector<SoundResult> newResults, bool append, bool more)
    {
        {
            const juce::ScopedLock sl (resultsLock);
            if (!append)
            {
                results.clear();
                if (hasPinnedReference && pinnedReferenceSound.id > 0)
                {
                    SoundResult refCopy = pinnedReferenceSound;
                    refCopy.isPinnedRef = true;
                    results.push_back (refCopy);
                }
            }

            filterAndAppendDiverseResults (results, newResults);

            hasMorePages = more;
            isLoadingMore = false;
        }

        syncStateToProcessor();
        table.updateContent();
        statusLabel.setText ("Loaded: " + juce::String (getNumRows()) + (hasMorePages ? "+ sounds (scroll for more)" : " sounds"),
                             juce::dontSendNotification);
    }

    void applyStatusMessage (const juce::String& text)
    {
        isLoadingMore = false;
        statusLabel.setText (text, juce::dontSendNotification);
    }

    void finishFetchForSound (int soundId, const juce::MemoryBlock& mb, const juce::File& savedDiskFile, bool shouldAutoPlay)
    {
        if (mb.getSize() == 0 && !savedDiskFile.existsAsFile())
        {
            if (shouldAutoPlay)
                statusLabel.setText ("Preview fetch failed.", juce::dontSendNotification);
            return;
        }

        if (mb.getSize() > 0)
            proc.storeRamCachedMp3 (soundId, mb);

        SoundResult matchedMeta;
        int matchedRow = -1;

        {
            const juce::ScopedLock sl (resultsLock);
            for (size_t i = 0; i < results.size(); ++i)
            {
                if (results[i].id == soundId)
                {
                    if (savedDiskFile.existsAsFile())
                        results[i].localFile = savedDiskFile;
                    matchedMeta = results[i];
                    matchedRow = static_cast<int> (i);
                    break;
                }
            }
        }

        if (proc.isSoundFavorite (soundId) && matchedMeta.id > 0)
            proc.setSoundFavorite (matchedMeta, true);

        if (matchedMeta.localFile.existsAsFile() && matchedRow >= 0)
        {
            const juce::ScopedLock sl (resultsLock);
            results[static_cast<size_t> (matchedRow)].localFile = matchedMeta.localFile;
        }

        syncStateToProcessor();

        if (matchedRow >= 0)
            table.repaintRow (matchedRow);

        if (shouldAutoPlay && matchedMeta.id > 0)
        {
            juce::String badge;
            if (matchedMeta.localFile.existsAsFile())
                badge = proc.loadAndPlayFile (matchedMeta.localFile, matchedMeta, true);
            else
                badge = proc.loadAndPlayMemoryBlock (mb, matchedMeta, true);

            {
                const juce::ScopedLock sl (resultsLock);
                if (matchedRow >= 0 && matchedRow < static_cast<int> (results.size()))
                    results[static_cast<size_t> (matchedRow)].detectedBpmKey = badge;
            }
            syncStateToProcessor();
            refreshWaveformFromProcessor();
            playPauseButton.setActiveState (proc.isAudioPlaying());
            statusLabel.setText (matchedMeta.localFile.existsAsFile()
                                     ? "Saved & Playing. Drag to Ableton!"
                                     : "RAM Preview (Disk clean). Click 'Get' or Drag to save.",
                                 juce::dontSendNotification);
        }
    }

private:
    void timerCallback() override
    {
        stopTimer();
        triggerAutoSearchFromFilter();
    }

    void updateRowLocalFile (int soundId, const juce::File& f)
    {
        {
            const juce::ScopedLock sl (resultsLock);
            for (auto& r : results)
                if (r.id == soundId)
                    r.localFile = f;
        }
        syncStateToProcessor();
    }

    void triggerAutoSearchFromFilter()
    {
        if (showingFavoritesOnly)
            return;

        if (searchInput.getText().trim().isNotEmpty() || !results.empty())
            startNewSearch (false);
    }

    void syncStateToProcessor()
    {
        const juce::ScopedLock sl (proc.stateLock);
        proc.persistedResults = results;
        proc.persistedQuery = searchInput.getText().trim();
        proc.persistedMinSlider = durRangeSlider.getMinValue();
        proc.persistedMaxSlider = durRangeSlider.getMaxValue();
        proc.persistedChannelId = channelBox.getSelectedId();
        proc.persistedTypeId = typeBox.getSelectedId();
        proc.persistedCurrentPage = currentPage;
        proc.persistedHasMorePages = hasMorePages;
        proc.persistedShowingFavs = showingFavoritesOnly;
        proc.persistedSelectedRow = lastTriggeredRow;
    }

    void pushCurrentStateToHistory()
    {
        const juce::ScopedLock sl (resultsLock);
        if (results.empty())
            return;

        SearchHistoryEntry entry;
        entry.query = searchInput.getText().trim();
        entry.results = results;
        entry.minSlider = durRangeSlider.getMinValue();
        entry.maxSlider = durRangeSlider.getMaxValue();
        entry.channelId = channelBox.getSelectedId();
        entry.typeId = typeBox.getSelectedId();
        entry.page = currentPage;
        entry.hasMore = hasMorePages;

        proc.searchHistory.push_back (std::move (entry));
        if (proc.searchHistory.size() > 15)
            proc.searchHistory.erase (proc.searchHistory.begin());

        backHistoryButton.setEnabled (true);
    }

    void restorePreviousSearchFromHistory()
    {
        if (proc.searchHistory.empty())
            return;

        if (isThreadRunning())
            stopThread (2000);

        auto entry = proc.searchHistory.back();
        proc.searchHistory.pop_back();
        backHistoryButton.setEnabled (!proc.searchHistory.empty());

        showingFavoritesOnly = false;
        favFilterButton.setActiveState (false);
        hasPinnedReference = false;
        similarSoundIdSnapshot = 0;

        searchInput.setText (entry.query, false);
        durRangeSlider.setMinAndMaxValues (entry.minSlider, entry.maxSlider, juce::dontSendNotification);
        updateDurationLabel();
        channelBox.setSelectedId (entry.channelId, juce::dontSendNotification);
        typeBox.setSelectedId (entry.typeId, juce::dontSendNotification);

        {
            const juce::ScopedLock sl (resultsLock);
            results = entry.results;
            currentPage = entry.page;
            hasMorePages = entry.hasMore;
            isLoadingMore = false;
        }

        syncStateToProcessor();
        table.updateContent();
        statusLabel.setText ("Stepped back (" + juce::String (getNumRows()) + " sounds)", juce::dontSendNotification);
    }

    void launchFloatingSamplePopup (const SoundResult& s)
    {
        auto content = std::make_unique<SamplePassportPopup> (
            s,
            [this] (const juce::String& clickedTag) {
                pushCurrentStateToHistory();
                showingFavoritesOnly = false;
                favFilterButton.setActiveState (false);
                hasPinnedReference = false;
                searchInput.setText (clickedTag, false);
                startNewSearch (false);
            },
            [this] (const SoundResult& refSound) {
                startSimilarSearchWithPinnedRef (refSound);
            });

        auto waveScreen = waveformDisplay.getScreenBounds();
        juce::Rectangle<int> upperRightAnchor (waveScreen.getRight() + 6,
                                               waveScreen.getY() - 95,
                                               2, 2);

        auto& box = juce::CallOutBox::launchAsynchronously (std::move (content), upperRightAnchor, nullptr);
        box.setLookAndFeel (&modernLF);
        box.setArrowSize (0.0f);
    }

    static juce::String normalizeSeriesKey (const juce::String& rawName)
    {
        auto lower = rawName.toLowerCase();
        if (lower.contains ("."))
            lower = lower.upToLastOccurrenceOf (".", false, false);

        juce::String lettersOnly;
        for (int i = 0; i < lower.length(); ++i)
        {
            auto ch = lower[i];
            if (juce::CharacterFunctions::isLetter (ch))
                lettersOnly += ch;
        }
        return lettersOnly.substring (0, 12);
    }

    static void filterAndAppendDiverseResults (std::vector<SoundResult>& dest, const std::vector<SoundResult>& incoming)
    {
        std::map<juce::String, int> authorCounts;
        std::map<juce::String, int> seriesCounts;
        std::set<int> existingIds;

        for (const auto& existing : dest)
        {
            existingIds.insert (existing.id);
            if (!existing.isPinnedRef)
            {
                if (existing.username.isNotEmpty())
                    authorCounts[existing.username.toLowerCase()]++;
                auto sKey = normalizeSeriesKey (existing.name);
                if (sKey.length() >= 4)
                    seriesCounts[sKey]++;
            }
        }

        for (const auto& item : incoming)
        {
            if (existingIds.count (item.id) > 0)
                continue;

            auto authKey = item.username.toLowerCase();
            auto sKey = normalizeSeriesKey (item.name);

            if (authKey.isNotEmpty() && authorCounts[authKey] >= 2)
                continue;
            if (sKey.length() >= 4 && seriesCounts[sKey] >= 2)
                continue;

            dest.push_back (item);
            existingIds.insert (item.id);
            if (authKey.isNotEmpty())
                authorCounts[authKey]++;
            if (sKey.length() >= 4)
                seriesCounts[sKey]++;
        }
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
                                    pushCurrentStateToHistory();
                                    showingFavoritesOnly = false;
                                    favFilterButton.setActiveState (false);
                                    hasPinnedReference = false;
                                    searchInput.setText (it->second, false);
                                    startNewSearch (false);
                                }
                            });
    }

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

        pushCurrentStateToHistory();

        showingFavoritesOnly = false;
        favFilterButton.setActiveState (false);
        hasPinnedReference = false;
        searchInput.clear();

        auto& rng = juce::Random::getSystemRandom();
        searchQuerySnapshot = "*";
        currentPage = 1 + rng.nextInt (45);

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

        statusLabel.setText ("Rolling chaotic random sounds...", juce::dontSendNotification);
        startThread();
    }

    void startSimilarSearchWithPinnedRef (const SoundResult& refSound)
    {
        if (isThreadRunning())
            stopThread (2000);

        searchTokenSnapshot = proc.savedToken.trim();
        if (searchTokenSnapshot.isEmpty() || refSound.id <= 0)
            return;

        pushCurrentStateToHistory();

        showingFavoritesOnly = false;
        favFilterButton.setActiveState (false);

        pinnedReferenceSound = refSound;
        pinnedReferenceSound.isPinnedRef = true;
        hasPinnedReference = true;

        similarSoundIdSnapshot = refSound.id;
        lastTriggeredRow = 0;
        currentPage = 1;
        isPaginationFetch = false;
        isLoadingMore = true;
        hasMorePages = false;

        {
            const juce::ScopedLock sl (resultsLock);
            results.clear();
            results.push_back (pinnedReferenceSound);
        }
        table.updateContent();

        statusLabel.setText ("Pinned [REF]: " + refSound.name.substring (0, 22) + " - loading similar...", juce::dontSendNotification);
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
            syncStateToProcessor();
            table.updateContent();
            statusLabel.setText ("Favorites library: " + juce::String (getNumRows()) + " saved sounds", juce::dontSendNotification);
        }
        else
        {
            if (searchInput.getText().trim().isNotEmpty())
                startNewSearch (false);
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

    void startNewSearch (bool recordHistory = true)
    {
        if (isThreadRunning())
            stopThread (2000);

        if (tokenInput.isVisible())
            commitTokenAndHideIfValid();

        if (recordHistory)
            pushCurrentStateToHistory();

        showingFavoritesOnly = false;
        favFilterButton.setActiveState (false);
        hasPinnedReference = false;
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

    // При перетаскивании с волны автоматически сохраняет текущий RAM-сэмпл на диск перед отдачей в Ableton
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
                    auto& item = results[static_cast<size_t> (r)];
                    if (!item.localFile.existsAsFile())
                        proc.materializeSoundToDisk (item);

                    if (item.localFile.existsAsFile())
                        filesToDrag.addIfNotAlreadyThere (item.localFile.getFullPathName());
                }
            }
        }

        if (filesToDrag.isEmpty())
        {
            auto curMeta = proc.getCurrentSampleMeta();
            if (curMeta.id > 0)
            {
                auto saved = proc.materializeSoundToDisk (curMeta);
                if (saved.existsAsFile())
                {
                    updateRowLocalFile (curMeta.id, saved);
                    filesToDrag.add (saved.getFullPathName());
                }
            }
        }

        table.repaint();

        if (!filesToDrag.isEmpty())
            performExternalDragDropOfFiles (filesToDrag, false);
    }

    void refreshWaveformFromProcessor()
    {
        juce::AudioBuffer<float> temp;
        proc.copyCachedBuffer (temp);
        auto meta = proc.getCurrentSampleMeta();
        bool isFav = (meta.id > 0) ? proc.isSoundFavorite (meta.id) : false;
        waveformDisplay.setWaveformData (temp, meta.name, meta.detectedBpmKey, isFav, (int) proc.getPlaybackMode());
    }

    // Запускает прослушивание: если файл уже на диске — с диска, если в RAM-кэше — из памяти, иначе качает в RAM
    void triggerPreviewInRamOrPlay (int index)
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
            syncStateToProcessor();
            refreshWaveformFromProcessor();
            playPauseButton.setActiveState (proc.isAudioPlaying());
            statusLabel.setText ("Playing from disk. Drag to Ableton!", juce::dontSendNotification);
            return;
        }

        juce::MemoryBlock cachedMb;
        if (proc.getRamCachedMp3 (item.id, cachedMb))
        {
            juce::String badge = proc.loadAndPlayMemoryBlock (cachedMb, item, true);
            {
                const juce::ScopedLock sl (resultsLock);
                if (index >= 0 && index < static_cast<int> (results.size()))
                    results[static_cast<size_t> (index)].detectedBpmKey = badge;
            }
            syncStateToProcessor();
            refreshWaveformFromProcessor();
            playPauseButton.setActiveState (proc.isAudioPlaying());
            statusLabel.setText ("RAM Preview (Disk clean). Click 'Get' or Drag to save.", juce::dontSendNotification);
            return;
        }

        fetchSoundAsync (index, true, false);
    }

    void ensureRowInRamQuietly (int index)
    {
        SoundResult item;
        {
            const juce::ScopedLock sl (resultsLock);
            if (index < 0 || index >= static_cast<int> (results.size()))
                return;
            item = results[static_cast<size_t> (index)];
        }

        juce::MemoryBlock dummy;
        if (!item.localFile.existsAsFile() && !proc.getRamCachedMp3 (item.id, dummy))
            fetchSoundAsync (index, false, false);
    }

    void parseJsonIntoList (const juce::var& resultsVar, std::vector<SoundResult>& outList)
    {
        if (!resultsVar.isArray())
            return;

        for (auto& elem : *resultsVar.getArray())
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

            auto target = proc.getSampleFileFor (r.id, r.name);
            if (target.existsAsFile())
                r.localFile = target;
            else
            {
                auto favCopy = proc.getFavoriteFileFor (r.id, r.name);
                if (favCopy.existsAsFile())
                    r.localFile = favCopy;
            }

            outList.push_back (r);
        }
    }

    void run() override
    {
        bool appendFlag = isPaginationFetch;
        std::vector<SoundResult> collected;
        bool more = false;

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (threadShouldExit())
                return;

            juce::String urlStr;
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

                if (typeFilterSnapshot == 2)      filterStr += " NOT tag:loop";
                else if (typeFilterSnapshot == 3) filterStr += " tag:loop";

                bool isChaosRandom = (searchQuerySnapshot == "*");
                static const char* sortModes[] = { "created_desc", "downloads_desc", "rating_desc", "duration_desc" };
                auto& rng = juce::Random::getSystemRandom();

                juce::String qParam = isChaosRandom ? "" : juce::URL::addEscapeChars (searchQuerySnapshot, true);
                juce::String sortParam = isChaosRandom
                    ? (juce::String ("&group_by_pack=1&sort=") + sortModes[rng.nextInt (4)])
                    : juce::String ("&group_by_pack=1");

                int pageToUse = isChaosRandom ? (1 + rng.nextInt (45)) : currentPage;

                urlStr = "https://freesound.org/apiv2/search/text/?query=" + qParam
                       + "&filter=" + juce::URL::addEscapeChars (filterStr, true)
                       + sortParam
                       + "&token=" + juce::URL::addEscapeChars (searchTokenSnapshot, true)
                       + "&fields=id,name,duration,previews,username,pack,type,samplerate,bitdepth,channels,license,tags&page_size=50&page="
                       + juce::String (pageToUse);
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
                more = (similarSoundIdSnapshot == 0) && nextVar.isString() && nextVar.toString().isNotEmpty();

                std::vector<SoundResult> rawBatch;
                parseJsonIntoList (obj->getProperty ("results"), rawBatch);
                filterAndAppendDiverseResults (collected, rawBatch);

                if (collected.size() >= 18 || !more || similarSoundIdSnapshot > 0)
                    break;

                ++currentPage;
            }
            else
            {
                break;
            }
        }

        if (!collected.empty() || similarSoundIdSnapshot > 0)
        {
            juce::Component::SafePointer<FreesoundMiniEditor> safeThis (this);
            juce::MessageManager::callAsync ([safeThis, collected, appendFlag, more]() {
                if (safeThis != nullptr)
                    safeThis->applySearchResults (collected, appendFlag, more);
            });
            return;
        }

        juce::Component::SafePointer<FreesoundMiniEditor> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]() {
            if (safeThis != nullptr)
                safeThis->applyStatusMessage ("No more results or check API Key.");
        });
    }

    // Скачивает сэмпл в RAM (или сразу на диск, если saveToDisk == true)
    void fetchSoundAsync (int index, bool shouldAutoPlay, bool saveToDisk)
    {
        if (shouldAutoPlay)
            statusLabel.setText (saveToDisk ? "Saving to disk..." : "Streaming preview to RAM...", juce::dontSendNotification);

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

        juce::Thread::launch ([safeThis, soundId, previewUrl, name, targetDir, shouldAutoPlay, saveToDisk]() {
            juce::MemoryBlock mb;
            juce::File savedFile;

            if (previewUrl.isNotEmpty())
            {
                juce::URL url (previewUrl);
                auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                   .withConnectionTimeoutMs (12000);

                if (auto stream = url.createInputStream (options))
                    stream->readIntoMemoryBlock (mb);
            }

            if (saveToDisk && mb.getSize() > 0)
            {
                targetDir.createDirectory();
                auto sanitized = juce::File::createLegalFileName (name);
                auto local = targetDir.getChildFile (juce::String (soundId) + "_" + sanitized + ".mp3");
                if (local.replaceWithData (mb.getData(), mb.getSize()))
                    savedFile = local;
            }

            juce::MessageManager::callAsync ([safeThis, soundId, mb, savedFile, shouldAutoPlay]() {
                if (safeThis != nullptr)
                    safeThis->finishFetchForSound (soundId, mb, savedFile, shouldAutoPlay);
            });
        });
    }

    FreesoundMiniAudioProcessor& proc;
    ModernLookAndFeel modernLF;

    ApiStatusBadge apiStatusBadge;
    juce::TextEditor tokenInput;
    bool isEditingToken = true;

    IconButton backHistoryButton { IconButton::BackArrow };
    juce::TextEditor searchInput;
    IconButton tagMenuButton     { IconButton::HashTag, true };
    IconButton randomDiceButton  { IconButton::Dice2D };
    IconButton favFilterButton   { IconButton::Heart };

    juce::ComboBox channelBox;
    juce::ComboBox typeBox;
    juce::Slider durRangeSlider;
    juce::Label durValueLabel;

    SegmentedFolderControl folderControl;
    IconButton infoButton        { IconButton::Info };

    juce::TableListBox table;
    WaveformDisplay waveformDisplay;

    IconButton playPauseButton   { IconButton::PlayPause, true };
    IconButton stopButton        { IconButton::Stop, true };

    juce::Slider pitchSlider;
    juce::ToggleButton normCheckbox;
    juce::Slider volSlider;
    juce::Label volLabel;

    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<InfoOverlay> infoOverlay;
    std::vector<SoundResult> results;
    juce::CriticalSection resultsLock;

    SoundResult pinnedReferenceSound;
    bool hasPinnedReference = false;

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
    double lastSpaceBarTimeMs = 0.0;

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
