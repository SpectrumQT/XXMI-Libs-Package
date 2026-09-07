#pragma once

#include "ShaderRegex.h"

bool get_shader_bindings_from_bytecode(const void* data, size_t size, ShaderBindings* out_bindings);
