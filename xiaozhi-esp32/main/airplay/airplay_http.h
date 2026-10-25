#ifndef AIRPLAY_HTTP_H
#define AIRPLAY_HTTP_H

#include <expected>
#include <functional>
#include <string>

/**
 * Small HTTP control surface for bench work: start/stop a session, feed the
 * pairing code and read the status without going through the assistant.
 * Serves on port 8080; safe to call more than once.
 */
void AirplayHttpStart();

/**
 * Ask the device's cloud vision pipeline (the one behind self.screen.look)
 * about an externally-provided JPEG. Board code injects this at startup so
 * the bench API can expose POST /vision/explain without knowing the board.
 * The callback receives JPEG bytes plus a question and returns the vision
 * service's answer text (or an error message).
 */
using VisionExplainFn = std::function<std::expected<std::string, std::string>(
    const uint8_t* jpeg, size_t jpeg_len, const std::string& question)>;
void AirplayHttpSetVisionExplain(VisionExplainFn fn);

#endif  // AIRPLAY_HTTP_H
