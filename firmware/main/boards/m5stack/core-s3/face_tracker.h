// Durk: on-device face tracking (lightweight skin-blob detector) steering the Stack-chan head.
#pragma once
#include <atomic>
class EspVideo;

class FaceTracker {
public:
    static FaceTracker& GetInstance() { static FaceTracker t; return t; }
    void Start(EspVideo* camera);
    void RegisterMcpTools();
    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
private:
    FaceTracker() = default;
    void Task();
    EspVideo* camera_ = nullptr;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> invert_x_{false};
    std::atomic<bool> invert_y_{false};
};
