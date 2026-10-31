#include "PluginEditor.h"

FreesoundMiniAudioProcessorEditor::FreesoundMiniAudioProcessorEditor (FreesoundMiniAudioProcessor& p)
    : AudioProcessorEditor (&p), Thread ("FreesoundNetworkThread"), audioProcessor (p)
{
    setSize (620, 480);

    cacheDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("FreesoundCache");
    cacheDir.createDirectory();

    tokenInput.setTextToShowWhenEmpty ("Paste Freesound API Key here...", juce::Colours::grey);
    addAndMakeVisible (tokenInput);

    searchInput.setTextToShowWhenEmpty ("Search vocal, breath, choir, fm...", juce::Colours::grey);
    searchInput.onReturnKey = [this] { searchButton.triggerClick(); };
    addAndMakeVisible (searchInput);

    searchButton.onClick = [this] {
        if (!isThreadRunning())
        {
            currentQuery = searchInput.getText().trim();
            statusLabel.setText("Searching...", juce::dontSendNotification);
            startThread();
        }
    };
    addAndMakeVisible (searchButton);

    table.setModel (this);
    table.getHeader().addColumn ("Title", 1, 380);
    table.getHeader().addColumn ("Duration", 2, 80);
    table.getHeader().addColumn ("Action", 3, 120);
    addAndMakeVisible (table);

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (statusLabel);
}

FreesoundMiniAudioProcessorEditor::~FreesoundMiniAudioProcessorEditor()
{
    stopThread (3000);
}

void FreesoundMiniAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1b1f));
}

void FreesoundMiniAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    tokenInput.setBounds (area.removeFromTop (30));
    area.removeFromTop (8);

    auto searchRow = area.removeFromTop (32);
    searchButton.setBounds (searchRow.removeFromRight (90));
    searchRow.removeFromRight (8);
    searchInput.setBounds (searchRow);
    area.removeFromTop (10);

    statusLabel.setBounds (area.removeFromBottom (24));
    table.setBounds (area);
}

int FreesoundMiniAudioProcessorEditor::getNumRows()
{
    return (int) results.size();
}

void FreesoundMiniAudioProcessorEditor::paintRowBackground (juce::Graphics& g, int rowNumber, int, int, bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll (juce::Colour (0xff323a48));
    else if (rowNumber % 2 == 0)
        g.fillAll (juce::Colour (0xff242429));
    else
        g.fillAll (juce::Colour (0xff1f1f23));
}

void FreesoundMiniAudioProcessorEditor::paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool)
{
    if (rowNumber >= (int) results.size()) return;
    g.setColour (juce::Colours::white);
    auto& item = results[(size_t) rowNumber];

    if (columnId == 1)
        g.drawText (item.name, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
    else if (columnId == 2)
        g.drawText (juce::String (item.duration, 1) + "s", 0, 0, width, height, juce::Justification::centred);
    else if (columnId == 3)
        g.drawText (item.localFile.existsAsFile() ? "[DRAG TO LIVE]" : "[Click to Load]", 0, 0, width, height, juce::Justification::centred);
}

void FreesoundMiniAudioProcessorEditor::cellClicked (int rowNumber, int, const juce::MouseEvent&)
{
    if (rowNumber < 0 || rowNumber >= (int) results.size()) return;

    auto& item = results[(size_t) rowNumber];
    if (item.localFile.existsAsFile())
    {
        audioProcessor.playPreview (item.localFile);
        juce::StringArray files;
        files.add (item.localFile.getFullPathName());
        performExternalDragDropOfFiles (files, false);
    }
    else
    {
        downloadAndPlay (rowNumber);
    }
}

void FreesoundMiniAudioProcessorEditor::run()
{
    auto token = tokenInput.getText().trim();
    if (token.isEmpty() || currentQuery.isEmpty()) return;

    auto urlStr = "https://freesound.org/apiv2/search/text/?query="
                + juce::URL::addEscapeChars (currentQuery, true)
                + "&filter=" + juce::URL::addEscapeChars ("license:\"Creative Commons 0\"", true)
                + "&token=" + token
                + "&fields=id,name,duration,previews&page_size=25";

    juce::URL url (urlStr);
    auto response = url.readEntireTextStream();

    auto varJson = juce::JSON::parse (response);
    if (auto* obj = varJson.getDynamicObject())
    {
        auto resultsArray = obj->getProperty ("results");
        if (resultsArray.isArray())
        {
            std::vector<SoundResult> newResults;
            for (auto& elem : *resultsArray.getArray())
            {
                SoundResult r;
                r.id = elem["id"];
                r.name = elem["name"].toString();
                r.duration = (double) elem["duration"];
                if (auto* prev = elem["previews"].getDynamicObject())
                    r.previewUrl = prev->getProperty ("preview-hq-mp3").toString();
                newResults.push_back (r);
            }

            juce::MessageManager::callAsync ([this, res = std::move(newResults)]() mutable {
                results = std::move (res);
                table.updateContent();
                statusLabel.setText ("Found: " + juce::String ((int) results.size()) + " sounds", juce::dontSendNotification);
            });
            return;
        }
    }

    juce::MessageManager::callAsync ([this] {
        statusLabel.setText ("Error: Check Token or Connection", juce::dontSendNotification);
    });
}

void FreesoundMiniAudioProcessorEditor::downloadAndPlay (int index)
{
    auto& item = results[(size_t) index];
    statusLabel.setText ("Loading preview...", juce::dontSendNotification);

    juce::Thread::launch ([this, index]() {
        auto& target = results[(size_t) index];
        auto local = cacheDir.getChildFile (juce::String (target.id) + ".mp3");

        if (!local.existsAsFile())
        {
            juce::URL url (target.previewUrl);
            juce::MemoryBlock mb;
            if (url.readEntireBinaryStream (mb, 10000))
            {
                local.replaceWithData (mb.getData(), mb.getSize());
            }
        }

        target.localFile = local;
        juce::MessageManager::callAsync ([this, index, local]() {
            table.repaintRow (index);
            audioProcessor.playPreview (local);
            statusLabel.setText ("Ready! Click row again & Drag into Live", juce::dontSendNotification);
        });
    });
}
