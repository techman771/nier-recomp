// Frame captures: one frame's renderer commands saved to a file, and a mode that renders such a
// file over and over without the game (framecapture.cpp). For graphics bugs: the scene is exact,
// starts in seconds and is the same every run.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "renderer.h"

// Writes the frame (shaders with their microcode, textures, draws, resolves) to `path`.
// `microcode` returns a shader's words by stage and hash, or null.
void FrameCaptureSave(const RendererFrame& frame, const std::string& path,
                      const std::function<const std::vector<uint32_t>*(bool pixel, uint64_t hash)>& microcode);

// NIER_REPLAY_FRAME=<file>: renders that capture instead of running the game; never returns then.
// Call once the presenter has been started.
void FrameReplayRunIfRequested();
