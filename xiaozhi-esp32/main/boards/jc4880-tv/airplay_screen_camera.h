#pragma once

#include <expected>
#include <string>

#include "camera.h"

/* A camera whose pixels are the virtual display's frame buffer, so the
 * assistant can look at what is being mirrored to (or shown by) the TV.
 *
 * It reuses the hardware-camera cloud pipeline unchanged: the MCP layer
 * registers self.camera.take_photo for any board whose GetCamera() returns a
 * camera, and the server's vision capability (a URL and token announced at
 * MCP initialize) is routed here through SetExplainUrl. Capture() snapshots
 * the frame buffer; Explain() encodes it to JPEG and uploads it, and the
 * server's answer comes back as the tool result. */
class AirplayScreenCamera : public Camera {
public:
    AirplayScreenCamera(uint16_t width, uint16_t height);
    ~AirplayScreenCamera() override;

    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::expected<std::string, std::string> Explain(const std::string& question) override;

private:
    uint16_t width_;
    uint16_t height_;
    /* Latest snapshot, RGB565, copied under the render lock. */
    uint8_t* frame_ = nullptr;
    size_t frame_len_ = 0;
    std::string explain_url_;
    std::string explain_token_;
};
