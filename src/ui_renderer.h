#pragma once

class GfxRenderer;
class HalGPIO;

void rendererSetup(GfxRenderer& renderer);
void drawDashboard(GfxRenderer& renderer, HalGPIO& gpio);
void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio);
void drawReadingPage(GfxRenderer& renderer, HalGPIO& gpio);
void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio);
void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio);
void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio);
void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio);
void drawCitiesMenu(GfxRenderer& renderer, HalGPIO& gpio);
void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio);
void drawPairedKeyboardsMenu(GfxRenderer& renderer, HalGPIO& gpio);
void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio);
