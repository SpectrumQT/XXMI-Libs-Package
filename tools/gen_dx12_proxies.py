#!/usr/bin/env python3
"""Generates the pass-through half of the D3D12 proxy classes.

Each proxy in DirectX11/dx12 derives from the newest version of an interface
in the Windows SDK and has to implement every method of it. This script reads
the interface definitions from the SDK headers and writes, per proxy class,

  generated/<Class>.decl.inl  declarations of all methods, included in the
                              class body
  generated/<Class>.fwd.inl   definitions of the methods that only forward to
                              the original object, included by <Class>.cpp

Methods named in CUSTOM below are declared but left for <Class>.cpp to define.
D3D12_CPU_DESCRIPTOR_HANDLE arguments passed by value are translated back to
the original handle on the way through (see Dx12DescriptorHeap.h).

Run it again after moving to a newer SDK or changing the tables:

  python tools/gen_dx12_proxies.py [path to the SDK's Include/<version>]
"""

import glob
import os
import re
import sys

HEADERS = ['um/d3d12.h', 'shared/dxgi.h', 'shared/dxgi1_2.h', 'shared/dxgi1_3.h',
           'shared/dxgi1_4.h', 'shared/dxgi1_5.h']

IUNKNOWN = ['QueryInterface', 'AddRef', 'Release']

# class name -> (newest interface, methods implemented by hand)
CLASSES = {
    'Dx12Device': ('ID3D12Device14', [
        'CreateCommandQueue', 'CreateCommandQueue1',
        'CreateCommandList', 'CreateCommandList1',
        'CreateDescriptorHeap', 'CopyDescriptors',
        'MakeResident', 'Evict', 'EnqueueMakeResident', 'SetResidencyPriority',
    ]),
    'Dx12CommandQueue': ('ID3D12CommandQueue', [
        'GetDevice', 'ExecuteCommandLists',
    ]),
    'Dx12GraphicsCommandList': ('ID3D12GraphicsCommandList10', [
        'GetDevice', 'Close', 'Reset',
        'DrawInstanced', 'DrawIndexedInstanced', 'Dispatch', 'ExecuteIndirect',
        'DispatchMesh', 'DispatchRays', 'ExecuteBundle',
        'SetDescriptorHeaps', 'OMSetRenderTargets', 'BeginRenderPass',
    ]),
    'Dx12DescriptorHeap': ('ID3D12DescriptorHeap', [
        'GetDevice', 'GetCPUDescriptorHandleForHeapStart',
    ]),
    'Dx12SwapChain': ('IDXGISwapChain4', [
        'GetDevice', 'Present', 'Present1', 'ResizeBuffers1',
    ]),
}

# Argument types that may carry one of our proxies or an encoded descriptor
# handle. A forwarded method taking one of these would hand it to the original
# object as is, so every such method has to be either implemented by hand or
# listed in REVIEWED with the reason it is safe.
SUSPECT_TYPES = re.compile(
    r'\b(ID3D12CommandQueue|ID3D12CommandList|ID3D12GraphicsCommandList\d*|ID3D12DescriptorHeap|'
    r'ID3D12Pageable|ID3D12DeviceChild|ID3D12Object|IUnknown|D3D12_CPU_DESCRIPTOR_HANDLE\s*\*|'
    r'D3D12_RENDER_PASS_RENDER_TARGET_DESC|D3D12_RENDER_PASS_DEPTH_STENCIL_DESC)\b')
SUSPECT_HANDLE_POINTER = re.compile(r'D3D12_CPU_DESCRIPTOR_HANDLE\s*\*')

REVIEWED = {
    # Private data is opaque to us
    'SetPrivateDataInterface',
    # Only heaps, resources and fences can be shared
    'CreateSharedHandle',
}

INTERFACE = re.compile(
    r'MIDL_INTERFACE\("[0-9a-fA-F-]+"\)\s+(\w+) : public (\w+)\s*\{\s*public:(.*?)\n\s*\};', re.S)
METHOD = re.compile(r'virtual\s+(.+?)\s*STDMETHODCALLTYPE\s+(\w+)\(\s*(.*?)\)\s*= 0;', re.S)
# MIDL emits a second, GCC flavoured declaration for methods returning structs
GCC_VARIANT = re.compile(
    r'#if defined\(_MSC_VER\) \|\| !defined\(_WIN32\)\n(.*?)#else\n.*?#endif\n', re.S)
SAL = re.compile(r'^_\w+_\b')


def find_sdk():
    kits = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                        'Windows Kits', '10', 'Include')
    versions = sorted(glob.glob(os.path.join(kits, '10.*')),
                      key=lambda p: [int(x) for x in os.path.basename(p).split('.')])
    if not versions:
        sys.exit('No Windows SDK found under ' + kits)
    return versions[-1]


def split_params(text):
    params, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        elif c == ',' and depth == 0:
            params.append(text[start:i])
            start = i + 1
    params.append(text[start:])
    return params


def strip_sal(param):
    while True:
        m = SAL.match(param)
        if not m:
            return param
        param = param[m.end():].lstrip()
        if param.startswith('('):
            depth = 0
            for i, c in enumerate(param):
                depth += c == '('
                depth -= c == ')'
                if depth == 0:
                    param = param[i + 1:].lstrip()
                    break


def parse_params(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = ' '.join(text.split())
    if text in ('', 'void'):
        return []
    params = []
    for param in split_params(text):
        param = strip_sal(param.strip())
        name = re.search(r'(\w+)\s*(\[[^\]]*\])?$', param).group(1)
        by_value_handle = re.match(r'^D3D12_CPU_DESCRIPTOR_HANDLE \w+$', param) is not None
        params.append((param, name, by_value_handle))
    return params


def parse_interfaces(sdk):
    interfaces = {}
    for header in HEADERS:
        with open(os.path.join(sdk, header), encoding='utf-8', errors='replace') as f:
            text = GCC_VARIANT.sub(r'\1', f.read().replace('\r\n', '\n'))
        for name, base, body in INTERFACE.findall(text):
            methods = [(' '.join(ret.split()), method, parse_params(params))
                       for ret, method, params in METHOD.findall(body)]
            interfaces[name] = (base, methods)
    return interfaces


def interface_chain(interfaces, name):
    chain = []
    while name != 'IUnknown':
        base, methods = interfaces[name]
        chain.append((name, methods))
        name = base
    return list(reversed(chain))


def generate(interfaces, cls, newest, custom, sdk_version, out_dir):
    banner = ('// Generated by tools/gen_dx12_proxies.py for %s from the headers of the\n'
              '// Windows SDK %s. Do not edit.\n' % (newest, sdk_version))
    decl = [banner, '\t// IUnknown\n',
            '\tHRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;\n',
            '\tULONG STDMETHODCALLTYPE AddRef() override;\n',
            '\tULONG STDMETHODCALLTYPE Release() override;\n']
    fwd = [banner]
    seen = set(IUNKNOWN)
    unused = set(custom)

    for interface, methods in interface_chain(interfaces, newest):
        decl.append('\n\t// %s\n' % interface)
        for ret, method, params in methods:
            if method in seen:
                sys.exit('%s: duplicate method %s' % (cls, method))
            seen.add(method)

            signature = ', '.join(p[0] for p in params)
            decl.append('\t%s STDMETHODCALLTYPE %s(%s) override;\n' % (ret, method, signature))

            if method in custom:
                unused.discard(method)
                continue

            suspect = [p[0] for p in params if SUSPECT_TYPES.search(p[0])]
            # Outputs of these types come from the original and need no review
            suspect = [p for p in suspect if SUSPECT_HANDLE_POINTER.search(p) or '**' not in p]
            if suspect and method not in REVIEWED:
                sys.exit('%s::%s forwards "%s" untouched. Implement it by hand or add it to REVIEWED.'
                         % (cls, method, '", "'.join(suspect)))

            args = ', '.join('Dx12OrigCpuHandle(%s)' % p[1] if p[2] else p[1] for p in params)
            fwd.append('\n%s STDMETHODCALLTYPE %s::%s(%s)\n{\n\t%smOrig->%s(%s);\n}\n'
                       % (ret, cls, method, signature, '' if ret == 'void' else 'return ', method, args))

    if unused:
        sys.exit('%s: custom methods not found in %s: %s' % (cls, newest, ', '.join(sorted(unused))))

    for suffix, lines in (('decl', decl), ('fwd', fwd)):
        with open(os.path.join(out_dir, '%s.%s.inl' % (cls, suffix)), 'w', newline='\r\n') as f:
            f.write(''.join(lines))


def main():
    sdk = sys.argv[1] if len(sys.argv) > 1 else find_sdk()
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           '..', 'DirectX11', 'dx12', 'generated')
    os.makedirs(out_dir, exist_ok=True)

    interfaces = parse_interfaces(sdk)
    for cls, (newest, custom) in CLASSES.items():
        generate(interfaces, cls, newest, custom, os.path.basename(os.path.normpath(sdk)), out_dir)
        print('%s: %s' % (cls, newest))


if __name__ == '__main__':
    main()
