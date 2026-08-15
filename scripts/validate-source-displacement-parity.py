#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
vmf = (root / 'src/linux_qt/vmf/VmfScene.cpp').read_text()
rt = (root / 'src/linux_qt/app/RayTracingScene.cpp').read_text()
hw = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
mat = (root / 'src/linux_qt/app/MaterialRenderer.cpp').read_text()
shader = (root / 'src/linux_qt/shaders/raytraced_preview.comp').read_text()
hpp = (root / 'src/linux_qt/vmf/VmfScene.hpp').read_text()
cmake = (root / 'CMakeLists.txt').read_text()

checks = {
    'version': 'VERSION 0.15.26' in cmake,
    'start point rotation': 'std::rotate(corners.begin()' in vmf,
    'no displacement winding heuristic': 'std::swap(corners[1], corners[3])' not in vmf,
    'source edge interpolation': 'edgeInterval0' in vmf and 'segmentInterval' in vmf,
    'source power range': 'if (power < 2 || power > 4) return;' in vmf,
    'source field vector defaults': 'std::vector<Vec3> fieldVectors(vertexCount, Vec3{})' in vmf,
    'source raw field displacement': 'multiply(fieldVectors[index], fieldDistances[index])' in vmf and 'normalized(fieldVectors[index]' not in vmf,
    'source alternating topology': 'if ((ndx & 1) != 0)' in vmf and 'BuildTriTLtoBR' in vmf and 'BuildTriBLtoTR' in vmf,
    'source edge normals': 'CalcNormalFromEdges' in vmf and 'normalCount' in vmf,
    'source base-face UV interpolation': 'CalcDispSurfCoords(false,0)' in vmf and 'vertex.textureU = texEndU0' in vmf and 'vertex.textureV = texEndV0' in vmf,
    'source alpha inversion': '1.0 - alphas[index] / 255.0' in vmf,
    'source tangent spaces': 'GenerateDispSurfTangentSpaces' in vmf and 'vertex.tangentS = tangentS' in vmf and 'vertex.tangentT = tangentT' in vmf,
    'source averaged normals remain unnormalized': 'generated = normalized(generated, face.normal)' not in vmf,
    'rt uses stored displacement UV': 'source.textureU' in rt and 'source.textureV' in rt and 'u, v, u, v' in rt,
    'hardware uses stored displacement UV': 'source.textureU' in hw and 'vertex.u2 = vertex.u' in hw and 'vertex.v2 = vertex.v' in hw,
    'software uses stored displacement UV': 'makeDisplacementVertex' in mat and 'source.textureU' in mat,
    'rt consumes authoritative indices': 'face.displacementIndices[tri + 0]' in rt,
    'hardware consumes authoritative indices': 'face.displacementIndices[tri + corner]' in hw,
    'software consumes authoritative indices': 'face.displacementIndices[index + corner]' in mat,
    'no support cap heuristic declaration': 'faceIsDisplacementSupportCap' not in hpp,
    'no support cap heuristic implementation': 'faceIsDisplacementSupportCap' not in vmf,
    'no displacement rt lighting special case': 'TRIANGLE_DISPLACEMENT' not in shader,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    for name in failed:
        print(f'FAIL: {name}')
    sys.exit(1)
for name in checks:
    print(f'PASS: {name}')
