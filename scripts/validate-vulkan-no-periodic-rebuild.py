#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
cpp = (root / 'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
hpp = (root / 'src/linux_qt/app/VulkanRayTracedViewport.hpp').read_text()
start = cpp.index('connect(animationTimer_, &QTimer::timeout')
end = cpp.index('    animationTimer_->start();', start)
block = cpp[start:end]
assert 'renderer_->hasAnimatedContent()' in block and 'renderer_->requestAnimationRefresh();' in block, 'animated geometry is not refreshed'
assert 'sceneDirty_ = true;' not in block, 'animation timer still requests full scene rebuilds'
assert 'renderer_->hasPendingDeferredRefresh()' in block, 'deferred animation refresh cannot wake the viewport'
assert 'animationTick_' not in cpp and 'animationTick_' not in hpp
assert 'frameDirty_ = true;' in block and 'update();' in block
assert 'VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR' in cpp, 'dynamic geometry does not refit the BLAS'
assert 'const bool refreshSprites = !interactive && spriteBillboardRefreshPending_' in cpp, 'billboards are not deferred until camera settle'
assert 'updateSpriteGeometry' in cpp, 'sprite refresh still shares the animation sampling path'
print('Vulkan timer refreshes only animated/deferred dynamic RT geometry without periodic full rebuilds')
