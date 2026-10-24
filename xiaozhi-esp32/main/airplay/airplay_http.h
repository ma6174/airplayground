#ifndef AIRPLAY_HTTP_H
#define AIRPLAY_HTTP_H

/**
 * Small HTTP control surface for bench work: start/stop a session, feed the
 * pairing code and read the status without going through the assistant.
 * Serves on port 8080; safe to call more than once.
 */
void AirplayHttpStart();

#endif  // AIRPLAY_HTTP_H
