#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/linux_qt/app/RayTracingScene.cpp').read_text()
required=['output.indices.push_back(first + 0u);','output.indices.push_back(first + 2u);','output.indices.push_back(first + 1u);','opposite winding conventions']
for token in required:
    if token not in s: raise SystemExit('missing winding fix: '+token)
old='output.indices.push_back(first + 0u);\n        output.indices.push_back(first + 1u);\n        output.indices.push_back(first + 2u);'
if old in s: raise SystemExit('obsolete Vulkan winding order remains')
print('Vulkan ray-tracing winding validation passed')
