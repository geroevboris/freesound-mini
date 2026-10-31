#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
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
    void paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    void cellClicked (int rowNumber, int columnId, const juce::MouseEvent& e) override;

private:
    void run() override;
    void downloadAndPlay (int index);

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreesoundMiniAudioProcessorEditor)
};
