#ifndef FACTURX_4DPLUGIN_H
#define FACTURX_4DPLUGIN_H

#include "4DPluginAPI.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#define PLUGIN_EXPORT
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" PLUGIN_EXPORT void PluginMain(PA_long32 selector, PA_PluginParameters params);

/* selector 1 -- PDFA Embed FacturX */
static void PDFA_Embed_FacturX(PA_PluginParameters params);

#endif /* FACTURX_4DPLUGIN_H */
