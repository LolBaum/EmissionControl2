/**
 * ecSynth.cpp
 *
 * AUTHOR: Jack Kilgore
 */

/**** Emission Control LIB ****/
#include "ecSynth.h"
/**** ALLOLIB ****/
#include "al/io/al_File.hpp"

/**** ecSynth Implementation ****/

void ecSynth::setIO(al::AudioIOData *io) {
  mPrevSR = mGlobalSamplingRate;
  grainScheduler.setSamplingRate(io->fps());
  mGlobalSamplingRate = io->fps();
  mScanner.setSamplingRate(mGlobalSamplingRate);
  pleaseResetScanner = true;
  ControlRate.spu(mGlobalSamplingRate / consts::CR_EVERY_N_SAMPLES);

  float min_grain_dur_ms = 2000 / mGlobalSamplingRate;
  ECParameters[GRAIN_DUR]->setAbsoluteMinMax(min_grain_dur_ms,
                                             ECParameters[GRAIN_DUR]->getAbsoluteMax());
  if (ECParameters[GRAIN_DUR]->getCurrentMin() < min_grain_dur_ms)
    ECParameters[GRAIN_DUR]->setCurrentMin(min_grain_dur_ms);
  if (ECParameters[GRAIN_DUR]->getCurrentMax() < min_grain_dur_ms)
    ECParameters[GRAIN_DUR]->setCurrentMax(min_grain_dur_ms);
  ECParameters[GRAIN_DUR]->setParam(ECParameters[GRAIN_DUR]->getParam());

  float max_lfo_freq = mGlobalSamplingRate / (2 * consts::CR_EVERY_N_SAMPLES);
  for (int index = 0; index < consts::NUM_LFOS; index++) {
    LFOParameters[index]->frequency->setAbsoluteMinMax(0.001, max_lfo_freq);
    if (LFOParameters[index]->frequency->getCurrentMax() > max_lfo_freq)
      LFOParameters[index]->frequency->setCurrentMax(max_lfo_freq);
    if (LFOParameters[index]->frequency->getCurrentMin() > max_lfo_freq)
      LFOParameters[index]->frequency->setCurrentMin(max_lfo_freq);
    LFOParameters[index]->frequency->setParam(LFOParameters[index]->frequency->getParam());
  }
}

void ecSynth::initialize(al::AudioIOData *io) {
  initParameters();

  mPrevSR = mGlobalSamplingRate;
  grainScheduler.setSamplingRate(io->fps());
  mGlobalSamplingRate = io->fps();
  mScanner.setSamplingRate(mGlobalSamplingRate);
  ControlRate.spu(mGlobalSamplingRate / consts::CR_EVERY_N_SAMPLES);

  float min_grain_dur_ms = 2000 / mGlobalSamplingRate;
  ECParameters[GRAIN_DUR]->setAbsoluteMinMax(min_grain_dur_ms,
                                             ECParameters[GRAIN_DUR]->getAbsoluteMax());
  ECParameters[GRAIN_DUR]->setCurrentMin(min_grain_dur_ms);

  mPActiveVoices = &mActiveVoices;

  for (int index = 0; index < consts::NUM_LFOS; index++) {
    Modulators.push_back(std::make_shared<ecModulator>());
    ControlRate << Modulators[index]->getLFO();

    LFOParameters.push_back(std::make_shared<LFOstruct>(index));
    LFOParameters[index]->frequency->setAbsoluteMinMax(
        0.001, mGlobalSamplingRate / (2 * consts::CR_EVERY_N_SAMPLES));

    LFOParameters[index]->shape->registerChangeCallback(
        [this, index](int value) { Modulators[index]->setWaveform(value); });

    LFOParameters[index]->polarity->registerChangeCallback(
        [this, index](int value) { Modulators[index]->setPolarity(value); });

    LFOParameters[index]->frequency->mParameter->registerChangeCallback(
        [this, index](float value) { Modulators[index]->setFrequency(value); });

    LFOParameters[index]->duty->registerChangeCallback(
        [this, index](float value) { Modulators[index]->setWidth(value); });
  }

  std::vector<std::string> lfo_names{"LFO1", "LFO2", "LFO3", "LFO4", "LFO5", "LFO6"};
  for (int index = 0; index < consts::NUM_PARAMS; index++) {
    ECParameters[index]->setModulationSource(Modulators[0]);
    ECModParameters[index]->setMenuElements(lfo_names);
    ECModParameters[index]->registerMenuChangeCallback(
        [this, index](int value) { ECParameters[index]->setModulationSource(Modulators[value]); });
  }

  grainScheduler.configure(ECParameters[consts::GRAIN_RATE]->getParam(), 0.0, 0.0);
  mScanner.set(ECParameters[consts::SCAN_HEAD]->getParam() * soundClip[0]->frames,
               soundClip[0]->frames, mGlobalSamplingRate);

  grainSynth.allocatePolyphony<Grain>(2048);
  grainSynth.setDefaultUserData(this);
}

void ecSynth::onProcess(al::AudioIOData &io) {
  //        updateFromParameters();
  /* Manipulate on a Grain Level */

  while (io()) {
    mCurrentIndex = mScanner();

    // CONTROL RATE LOOP (Executes every 4th sample)
    if (controlRateCounter == consts::CR_EVERY_N_SAMPLES) {
      controlRateCounter = 0;

      // Store Modulator Values
      for (int index = 0; index < consts::NUM_LFOS; ++index)
        Modulators[index]->sampleAndStore();

      // SCANNER LOGIC
      prevTapeHeadVal = nowTapeHeadVal;
      nowTapeHeadVal = ECParameters[consts::SCAN_HEAD]->getModParam(
          ECModParameters[consts::SCAN_HEAD]->getWidthParam());
      prev_scan_speed = scan_speed;
      scan_speed = ECParameters[consts::SCAN_SPEED]->getModParam(
          ECModParameters[consts::SCAN_SPEED]->getWidthParam());
      prev_scan_width = scan_width;
      scan_width = ECParameters[consts::SCAN_RANGE]->getModParam(
          ECModParameters[consts::SCAN_RANGE]->getWidthParam());
      float frames = soundClip[mModClip]->frames;
      float start, end;

      // Case where the scanning head is given a hard reset.
      if (mPrevModClip != mModClip || mCurrentIndex == mScanner.getTarget() ||
          prevTapeHeadVal != nowTapeHeadVal || pleaseResetScanner) {
        pleaseResetScanner = false;
        if ((scan_speed >= 0 && scan_width >= 0) || (scan_speed < 0 && scan_width < 0)) {
          start = nowTapeHeadVal * frames;
          end = start + (frames * scan_width);
        } else {
          start = (nowTapeHeadVal + scan_width) * frames;
          end = nowTapeHeadVal * frames;
        }
        mScanner.set(start, end, abs(end - start) / (mGlobalSamplingRate * abs(scan_speed)));
      }

      // On the fly adjustments.
      if (scan_speed != prev_scan_speed) {
        start = mScanner.getValue();
        if ((scan_speed >= 0 && scan_width >= 0) || (scan_speed < 0 && scan_width < 0))
          end = (nowTapeHeadVal * frames) + (frames * scan_width);
        else
          end = (nowTapeHeadVal * frames);
        mScanner.set(start, end, abs(end - start) / (mGlobalSamplingRate * abs(scan_speed)));
      }

      if (scan_width != prev_scan_width) {
        start = mScanner.getValue();

        if (scan_width >= 0) {
          if (start > (nowTapeHeadVal + scan_width) * frames)
            start = nowTapeHeadVal * frames;
        } else {
          if (start < (nowTapeHeadVal + scan_width) * frames)
            start = (nowTapeHeadVal + scan_width) * frames;
        }

        if ((scan_speed >= 0 && scan_width >= 0) || (scan_speed < 0 && scan_width < 0))
          end = (nowTapeHeadVal * frames) + (frames * scan_width);
        else
          end = (nowTapeHeadVal * frames);
        mScanner.set(start, end, abs(end - start) / (mGlobalSamplingRate * abs(scan_speed)));
      }

      // Wrapping Logic
      if (mCurrentIndex >= frames || mCurrentIndex < 0) {
        mCurrentIndex = fmod(mCurrentIndex, (float)frames);
        if (mCurrentIndex < 0) {
          mCurrentIndex += frames;
        }
      }
      // END OF SCANNER LOGIC

      // THIS IS WHERE WE WILL MODULATE THE GRAIN SCHEDULER
      width = ECModParameters[consts::GRAIN_RATE]->getWidthParam();
      if (width > 0)
        grainScheduler.setFrequency(ECParameters[consts::GRAIN_RATE]->getModParam(width));
      else
        grainScheduler.setFrequency(ECParameters[consts::GRAIN_RATE]->getParam());

      width = ECModParameters[consts::ASYNC]->getWidthParam();
      if (width > 0) // modulate the asynchronicity
        grainScheduler.setAsynchronicity(ECParameters[consts::ASYNC]->getModParam(width));
      else
        grainScheduler.setAsynchronicity(ECParameters[consts::ASYNC]->getParam());

      width = ECModParameters[consts::INTERM]->getWidthParam();
      if (width > 0) // modulate the intermittency
        grainScheduler.setIntermittence(ECParameters[consts::INTERM]->getModParam(width));
      else
        grainScheduler.setIntermittence(ECParameters[consts::INTERM]->getParam());

      width = ECModParameters[consts::STREAMS]->getWidthParam();
      if (width > 0) // Modulate the amount of streams playing.
        grainScheduler.setPolyStream(
            consts::synchronous,
            static_cast<int>(ECParameters[consts::STREAMS]->getModParam(width)));
      else
        grainScheduler.setPolyStream(consts::synchronous,
                                     static_cast<int>(ECParameters[consts::STREAMS]->getParam()));

      mPrevModClip = mModClip;
      mModClip = static_cast<int>(ECParameters[consts::SOUND_FILE]->getModParam(
                     ECModParameters[consts::SOUND_FILE]->getWidthParam())) -
                 1;
    }
    controlRateCounter++;

    // Grain by Grain Initilization
    if (grainScheduler.trigger()) {

      auto *voice = static_cast<Grain *>(grainSynth.getFreeVoice());
      if (voice) {
        grainParameters list = {
            ECParameters[consts::PLAYBACK],
            ECModParameters[consts::PLAYBACK]->getWidthParam(),
            ECParameters[consts::FILTER_CENTER],
            ECModParameters[consts::FILTER_CENTER]->getWidthParam(),
            ECParameters[consts::RESONANCE],
            ECModParameters[consts::RESONANCE]->getWidthParam(),
            ECParameters[consts::GRAIN_DUR],
            ECModParameters[consts::GRAIN_DUR]->getWidthParam(),
            ECParameters[consts::ENVELOPE],
            ECModParameters[consts::ENVELOPE]->getWidthParam(),
            ECParameters[consts::PAN],
            ECModParameters[consts::PAN]->getWidthParam(),
            ECParameters[consts::AMPLITUDE],
            ECModParameters[consts::AMPLITUDE]->getWidthParam(),
            soundClip[mModClip],
            mPActiveVoices,
            mCurrentIndex,
        };

        voice->configureGrain(list, mGlobalSamplingRate);

        mActiveVoices++;
        grainSynth.triggerOn(voice, io.frame());
        grainCounter++;

      } else {
        std::cout << "out of voices!" << std::endl;
      }
    }
  }

  grainSynth.render(io);

  io.frame(0);
  // Manipulate on a stream level
  while (io()) {
    // add samples to oscilloscope ringbuffer
    oscBufferL.push_back(io.out(0));
    oscBufferR.push_back(io.out(1));

    // get recent peak
    if (abs(io.out(0)) > peakL) {
      peakL = abs(io.out(0));
      peakResetL = 0;
    };
    peakResetL += 1;
    if (peakResetL > io.framesPerSecond() * 1)
      peakL = 0;
    if (abs(io.out(1)) > peakR) {
      peakR = abs(io.out(1));
      peakResetR = 0;
    };
    peakResetR += 1;
    if (peakResetR > io.framesPerSecond() * 1)
      peakR = 0;

    // Set clip variables to > 0 if clip is detected on any sample
    // 5 is the number of visual frames the meter will turn red for
    if (abs(io.out(0)) > 1.0f)
      clipL = 5;
    if (abs(io.out(1)) > 1.0f)
      clipR = 5;
    // Add samples to VU ringbuffer (squared in advance for RMS calculations)
    vuBufferL.push_back(pow(io.out(0), 2));
    vuBufferR.push_back(pow(io.out(1), 2));
  }
}

void ecSynth::onTriggerOn() {}

void ecSynth::onTriggerOff() {}

bool ecSynth::loadSoundFileRT(std::string fileName) {
  if (std::find(soundClipFileName.begin(), soundClipFileName.end(), fileName) !=
      soundClipFileName.end())
    return false;
  bool temp = util::load(fileName, soundClip, mGlobalSamplingRate, true,
                         consts::MAX_NUM_FLOATS_PER_AUDIO_FILE);
  if (temp) {
    soundClipFileName.push_back(fileName);
    mClipNum++;

    ECParameters[consts::SOUND_FILE]->mParameter->max(mClipNum);
    ECParameters[consts::SOUND_FILE]->mLowRange->max(mClipNum);
    ECParameters[consts::SOUND_FILE]->mHighRange->max(mClipNum);
    ECParameters[consts::SOUND_FILE]->mHighRange->set(mClipNum); // stylistic choice, might take out
  }

  return temp;
}

bool ecSynth::loadSoundFileOffline(std::string fileName) {
  if (std::find(soundClipFileName.begin(), soundClipFileName.end(), fileName) !=
      soundClipFileName.end())
    return false;
  bool temp = util::load(fileName, soundClip, mGlobalSamplingRate, true,
                         consts::MAX_NUM_FLOATS_PER_AUDIO_FILE);
  if (temp) {
    soundClipFileName.push_back(fileName);
    mClipNum++;
  }

  return temp;
}

bool ecSynth::loadInitSoundFiles(std::string directory) {
  al::FileList initAudioFiles = al::fileListFromDir(directory);
  initAudioFiles.sort([](al::FilePath s1, al::FilePath s2) {
    std::string one = s1.file();
    std::string two = s2.file();
    int min_size = one.size() < two.size() ? one.size() : two.size();
    for (int i = 0; i < min_size; i++) {
      if (std::tolower(one[i]) != std::tolower(two[i]))
        return (std::tolower(one[i]) < std::tolower(two[i]));
    }
    return one.size() < two.size();
  });
  bool success = false;
  for (auto i = initAudioFiles.begin(); i != initAudioFiles.end(); i++) {
    if (i->file().substr(i->file().length() - 4) == ".wav" ||
        i->file().substr(i->file().length() - 4) == ".aif") {
      loadSoundFileOffline(i->filepath());
      success = true;
    } else if (i->file().substr(i->file().length() - 5) == ".aiff") {
      loadSoundFileOffline(i->filepath());
      success = true;
    }
  }
  return success;
}

bool ecSynth::removeSoundFile(int index) {
  if (mClipNum == 0)
    return false;
  soundClip.erase(soundClip.begin() + index);
  soundClipFileName.erase(soundClipFileName.begin() + index);
  mClipNum--;
  ECParameters[consts::SOUND_FILE]->mParameter->max(mClipNum);
  ECParameters[consts::SOUND_FILE]->mLowRange->max(mClipNum);
  ECParameters[consts::SOUND_FILE]->mHighRange->max(mClipNum);
  ECParameters[consts::SOUND_FILE]->mHighRange->set(mClipNum); // stylistic choice, might take out

  if (static_cast<int>(ECParameters[consts::SOUND_FILE]->mParameter->get()) >= index)
    ECParameters[consts::SOUND_FILE]->mParameter->set(
        ECParameters[consts::SOUND_FILE]->mParameter->get() - 1);
  return true;
}

bool ecSynth::removeCurrentSoundFile() {
  removeSoundFile(ECParameters[consts::SOUND_FILE]->mParameter->get() - 1);
  return true;
}

void ecSynth::clearSoundFiles() {
  soundClip.clear();
  soundClipFileName.clear();

  mClipNum = 0;
  ECParameters[consts::SOUND_FILE]->mParameter->max(mClipNum);
  ECParameters[consts::SOUND_FILE]->mLowRange->max(mClipNum);
  ECParameters[consts::SOUND_FILE]->mHighRange->max(mClipNum);
  ECParameters[consts::SOUND_FILE]->mHighRange->set(mClipNum); // stylistic choice, might take out
}

void ecSynth::resampleSoundFiles() {
  // If sampling rate is the same as before, no need for resampling.
  if (static_cast<int>(mPrevSR) == static_cast<int>(mGlobalSamplingRate))
    return;
  std::vector<std::string> filePaths;
  long unsigned i;
  // Collect filepaths of audio buffers.
  for (i = 0; i < soundClip.size(); i++) {
    filePaths.push_back(soundClip[i]->filePath);
  }

  clearSoundFiles();
  for (long unsigned i = 0; i < filePaths.size(); i++)
    loadSoundFileRT(filePaths[i]);
}

void ecSynth::copyActiveGrainIndicies(float *array, int *outSize, int maxSize) {
  std::unique_lock<std::mutex> lk(mVoicePassLock);
  auto voices = static_cast<Grain *>(grainSynth.getActiveVoices());
  unsigned counter = 0;
  while (voices && counter < maxSize) {
    array[counter] = voices->getSourceIndex();
    voices = static_cast<Grain *>(voices->next);
    counter++;
  }
  *outSize = counter;
}

void ecSynth::hardClip(al::AudioIOData &io) {
  for (unsigned i = 0; i < io.channelsOut(); ++i) {
    if (io.out(i) > 1)
      io.sum(-1 * io.out(i) + 1, i);
    if (io.out(i) < -1)
      io.sum(-1 * io.out(i) - 1, i);
  }
}