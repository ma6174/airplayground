/*
 * The controller identity the receiver remembers between sessions: an Ed25519
 * key pair plus the UUID it is filed under. Keeping it in NVS is what turns the
 * second boot into a pair-verify-only session instead of a new pairing.
 */
#pragma once

#include "esp_err.h"
#include "hap.h"

/* Loads the stored identity, generating and persisting one on first boot. */
esp_err_t app_identity_load(hap_identity_t *identity);
