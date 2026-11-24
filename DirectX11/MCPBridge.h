#pragma once

class HackerDevice;

void MCP_Start(bool enabled);
void MCP_Stop();
void MCP_AfterFrameAnalysis();
void MCP_RunDeferredActions(HackerDevice *device);
void MCP_RunFrameActions(HackerDevice *device);
