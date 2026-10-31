#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

struct SoundResult
{
    int id = 0;
    juce::String name;
    double duration = 0.0;
    juce::String previewUrl;
    juce::File localFile;
};

class FreesoundMiniAudioProcessorEditor : public juce::AudioProcessorEditor,
                                         public juce::TableListBoxModel,
                                         public juce::DragAndDropContainer,
                                         private juce::Thread
{
public:
    explicit FreesoundMiniAudioProcessorEditor (FreesoundMiniAudioProcessor&);
    ~FreesoundMiniAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int, int, int, bool) override;
    void paintCell (juce::Graphics&, int, int, int, int, bool) override;
    void cellClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;

private:
    void run() override;
    void searchFreesound(const juce::String& query);
    void downloadAndPlay(int index);

    FreesoundMiniAudioProcessor& audioProcessor;

    juce::TextEditor tokenInput;
    juce::TextEditor searchInput;
    juce::TextButton searchButton { "Search" };
    juce::TableListBox table;
    juce::Label statusLabel;

    std::vector<SoundResult> results;
    juce::String currentQuery;
    juce::File cacheDir;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreesoundMiniAudioProcessorEditor)
};
