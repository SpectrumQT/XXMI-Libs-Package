#pragma once

#include "BufferHash.h"
#include "PipelineRegistry.h"

class Dx12GraphicsCommandList;

// Tells how well the identification layer does against the mods that are
// installed: every hash it comes up with is checked against the
// [TextureOverride] and [ShaderOverride] sections of the loaded configuration
// and the totals go to the log with the frame statistics.
//
// With dx12_dump_hashes in [Hunting], everything identified is also written
// to CSV files next to the log, for comparing against a D3D11 session of the
// same scene:
//
//   dx12_shaders.csv   every distinct shader a pipeline was created with
//   dx12_textures.csv  every texture as it gets a hash
//   dx12_buffers.csv   every buffer upload that was hashed
//   dx12_draws.csv     what the draws of one frame use, a frame every few
//                      seconds as resolving that is far from free

// True while the draws being recorded go to dx12_draws.csv
extern volatile bool dx12_dump_draws;

void Dx12DumpShader(Dx12ShaderStage stage, const Dx12ShaderInfo *info);
void Dx12DumpTexture(ID3D12Resource *texture, const char *source);
void Dx12DumpBuffer(ID3D12Resource *buffer, UINT64 offset, const Dx12BufferHash *hash);
void Dx12DumpDraw(Dx12GraphicsCommandList *list, const char *type, UINT count, UINT instances, UINT start, INT base_vertex);

// A line for the log that also goes to dx12_report.txt, as what the draw path
// has to say is needed most from sessions of a game run without a log
void Dx12DumpNote(const char *format, ...);

void Dx12DumpFramePresented();
void Dx12HashReport(bool info);
