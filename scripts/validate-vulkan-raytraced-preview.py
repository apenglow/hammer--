#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
cmake_root = (root / "CMakeLists.txt").read_text()
cmake = (root / "src/linux_qt/CMakeLists.txt").read_text()
main_cpp = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
main_hpp = (root / "src/linux_qt/app/MainWindow.hpp").read_text()
view_cpp = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
view_hpp = (root / "src/linux_qt/app/MapViewWidget.hpp").read_text()
renderer = (root / "src/linux_qt/app/VulkanRayTracedViewport.cpp").read_text()
scene_hpp = (root / "src/linux_qt/app/RayTracingScene.hpp").read_text()
scene_cpp = (root / "src/linux_qt/app/RayTracingScene.cpp").read_text()
shader = (root / "src/linux_qt/shaders/raytraced_preview.comp").read_text()
material_hpp = (root / "src/linux_qt/assets/MaterialSystem.hpp").read_text()
material_cpp = (root / "src/linux_qt/assets/MaterialSystem.cpp").read_text()

required = {
    "project version": ("VERSION 0.15.26", cmake_root),
    "build option": ("HAMMER_ENABLE_VULKAN_RAY_TRACING", cmake_root + cmake),
    "Vulkan discovery": ("find_package(Vulkan QUIET)", cmake),
    "GLSL compiler discovery": ("glslangValidator", cmake),
    "Vulkan 1.2 shader target": ("--target-env vulkan1.2", cmake),
    "conditional backend definition": ("HAMMER_HAVE_VULKAN_RAY_TRACING=1", cmake),
    "ray preview enum": ("RayTracedPreview", view_hpp),
    "ray preview action": ("3D &Ray-Traced Preview", main_cpp),
    "persistent mode": ("ray-traced-preview", main_cpp),
    "model/map view propagation": ("rayTracedViewport_->requestUpdate", view_cpp),
    "BLAS geometry": ("VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR", renderer),
    "TLAS geometry": ("VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR", renderer),
    "ray-query device extension": ("VK_KHR_RAY_QUERY_EXTENSION_NAME", renderer),
    "acceleration structure device extension": ("VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME", renderer),
    "real AS build command": ("vkCmdBuildAccelerationStructuresKHR", renderer),
    "ray-query shader extension": ("#extension GL_EXT_ray_query : require", shader),
    "primary ray query": ("rayQueryInitializeEXT", shader),
    "alpha-aware candidate confirmation": ("rayQueryConfirmIntersectionEXT", shader),
    "ray-traced sun visibility": ("visibleToLight", shader),
    "ray-traced reflections": ("reflectedHit = traceScene", shader),
    "ray-traced water refraction": ("refractedHit = traceScene", shader),
    "detail material support": ("detailTexture", material_hpp + material_cpp + scene_hpp + scene_cpp + shader),
    "alpha-test material support": ("alphaTestReference", material_hpp + material_cpp),
    "lightwarp material support": ("warpedDiffuse", shader),
    "Phong material support": ("CONTROL_PHONG", shader),
    "self illumination support": ("CONTROL_SELFILLUM", shader),
    "rim-light support": ("CONTROL_RIMLIGHT", shader),
    "Source atlas": ("materialAtlas", shader),
    "OpenGL fallback retained": ("Hardware3DViewport", view_cpp + view_hpp),
}
for label, (token, text) in required.items():
    if token not in text:
        raise SystemExit(f"Vulkan ray-traced preview validation failed: {label}: {token!r}")

if shader.count("binding =") < 8:
    raise SystemExit("Vulkan ray-traced preview validation failed: incomplete descriptor layout")
if renderer.count("VK_DESCRIPTOR_TYPE_STORAGE_BUFFER") < 2:
    raise SystemExit("Vulkan ray-traced preview validation failed: scene SSBO bindings missing")
if "rayTracedPreviewAction_{nullptr}" not in main_hpp:
    raise SystemExit("Vulkan ray-traced preview validation failed: action member missing")
if "VulkanRayTracedViewport* rayTracedViewport_{nullptr}" not in view_hpp:
    raise SystemExit("Vulkan ray-traced preview validation failed: viewport member missing")

# Lightweight source sanity for the shader when glslangValidator is unavailable.
for opening, closing, label in [("{", "}", "braces"), ("(", ")", "parentheses"), ("[", "]", "brackets")]:
    if shader.count(opening) != shader.count(closing):
        raise SystemExit(f"Vulkan ray-traced preview validation failed: unbalanced shader {label}")

print("Vulkan KHR ray-traced preview, BLAS/TLAS, UI, and Source material validation passed")
