#pragma once

#include <d3d11_1.h>

ID3D11Device* hook_device(ID3D11Device *orig_device, ID3D11Device *hacker_device);
ID3D11Device* lookup_hooked_device(ID3D11Device *orig_device);
