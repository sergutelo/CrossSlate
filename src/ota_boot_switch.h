#pragma once

bool otaBootFindCrossInk();
bool otaBootCrossInkAvailable();
bool otaBootSwitchToCrossInk();

// Confirms the currently running CrossSlate OTA image when the bootloader put
// it in pending-verify state, preventing rollback after deep sleep.
void otaBootConfirmRunningImage();
