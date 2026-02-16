#pragma once

#include <string>
#include <vector>
#include <memory>

struct AudioDevice {
    std::string id;           // platform-specific internal name/ID
    std::string description;  // human-readable description
};

class AudioCapture {
public:
    virtual ~AudioCapture() = default;
    virtual bool open(const std::string& device_id, int sample_rate, int channels) = 0;
    virtual int  read(float* buffer, int frames) = 0;   // blocking; returns 0 on success, -1 on error
    virtual void close() = 0;
};

class AudioPlayback {
public:
    virtual ~AudioPlayback() = default;
    virtual bool open(int sample_rate, int channels) = 0;
    virtual int  write(const float* buffer, int frames) = 0;  // blocking; returns 0 on success, -1 on error
    virtual void flush() = 0;
    virtual void close() = 0;
};

std::vector<AudioDevice>       audio_enumerate_inputs();
std::unique_ptr<AudioCapture>  audio_create_capture();
std::unique_ptr<AudioPlayback> audio_create_playback();
