#include "PluginProcessor.h"
#include "PluginEditor.h"

FreesoundMiniAudioProcessor::FreesoundMiniAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();
}

FreesoundMiniAudioProcessor::~FreesoundMiniAudioProcessor() {}

void FreesoundMiniAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    transportSource.prepareToPlay (samplesPerBlock, sampleRate);
}

void FreesoundMiniAudioProcessor::releaseResources()
{
    transportSource.releaseResources();
}

void FreesoundMiniAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();
    juce::AudioSourceChannelInfo channelInfo (&buffer, 0, buffer.getNumSamples());
    transportSource.getNextAudioBlock (channelInfo);
}

void FreesoundMiniAudioProcessor::playPreview (const juce::File& file)
{
    transportSource.stop();
    readerSource.reset();

    if (auto* reader = formatManager.createReaderFor (file))
    {
        readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
        transportSource.setSource (readerSource.get(), 0, nullptr, reader->sampleRate);
        transportSource.start();
    }
}

void FreesoundMiniAudioProcessor::stopPreview()
{
    transportSource.stop();
}

juce::AudioProcessorEditor* FreesoundMiniAudioProcessor::createEditor()
{
    return new FreesoundMiniAudioProcessorEditor (*this);
}

void FreesoundMiniAudioProcessor::getStateInformation (juce::MemoryBlock&) {}
void FreesoundMiniAudioProcessor::setStateInformation (const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FreesoundMiniAudioProcessor();
}
