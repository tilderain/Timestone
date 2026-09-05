Findings
Launch-menu display settings edit the existing native INI on Save, without a persistent
Timestone override. Cancel does not write display settings. The path is
`%LOCALAPPDATA%\CAPCOM\ULTIMATE MARVEL VS. CAPCOM 3\config.ini`:
the call at `0x1402559F3` selects INI storage with the CAPCOM subdirectory and
local-app-data flag; `MtFile__getLocalAppDataFolder` uses CSIDL_LOCAL_APPDATA.
`sApp__initInstance` reads `[DISPLAY] Resolution` as `WIDTHxHEIGHT` and clamps
width to 640-8192 and height to 480-8192. `sRender__createD3D9Device` reads
`[DISPLAY] FullScreen` through `MtRegistry__getBool`, which accepts ON/OFF.
These edits target game startup, not a live device reset when returning to the launcher.

The mod currently has no dedicated low-spec graphics menu. The game itself exposes five safe native controls through sMvc3Main__syncGraphicsSettings at 0x140259BC0:
1. Resolution
2. FullScreen
3. AltAntiAlias
4. TextureDetail
5. EffectVolume
Matching command-line/config strings also exist:
- -Resolution
- -FullScreen
- -AltAntiAlias
- -AntiAlias
- -TextureDetail
- -EffectVolume
These use native setters and are the safest foundation for a low-spec preset.
Best Candidates
Option	Current exposure	Expected benefit	Risk
Lowest native resolution	Retail graphics path	Very high	Low
AA off	Retail graphics path	Medium/high	Low
Lowest texture detail	Retail graphics path	Medium on low-VRAM hardware	Low
Lowest visual effect volume	Retail graphics path	Medium/high during busy fights	Some effects become less visible
Custom sub-720p internal resolution	Not exposed	Very high	UI/aspect and render-target testing needed
Disable expensive post-processing	Not exposed	Medium/high	Must preserve gameplay-significant outlines
Disable or reduce shadows	Not exposed	Medium	Requires native refresh/recreation
Increase mip LOD bias	DTI property, not reliably exposed	Low/medium	Blurry textures
Render every second frame	Debug mFrameSkip candidate	High	30 FPS presentation/judder
Disable stage rendering	Not exposed	High on complex stages	Visually severe; ownership filtering needed
Useful Engine Controls
sRender is available through the singleton pointer at 0x140E179A8.
- sRender+0x12C: mMipLODBias
- sRender+0x3B: mDisableRendering
- sRender+0x39: mFrameSkip
- sRender+0x3D: mParallelRendering
- sRender+0x139: mDeferredLightingForcedForwardRendering
- sRender+0x150: mDeferredLightingLodThreshold
- mScreenSize, mDisplaySize, mBackBufferSize, PresentWidth, PresentHeight, and ScaleMode are registered properties.
Arbitrary resolution should go through the resize/device path, particularly sRender__resizeBackbuffer at 0x14053EDE0. Raw writes to size fields would leave render targets and viewport state inconsistent.
The shadow system at singleton pointer 0x140E1A8C0 registers:
- mDrawEnable
- mMapSize
- mStandBy
- mDeferredShadow
- ShadowMapCacheEnable
- mVSMBlurType
- mPCFType
- mShadowViewDistance
Changing mMapSize after initialization may not resize existing shadow targets. A proper option should call the associated property setter or recreate shadow resources. Completely disabling shadow drawing is likely the safer first experiment.
Post-Processing
The executable contains independent draw paths for:
- Bloom: uBloomFilter__draw, 0x1406022C0
- Depth of field: uDOFFilter__draw, 0x140609080
- Motion blur: uMotionBlurFilter__draw, 0x14060E010
- Ambient occlusion: uAmbientOcclusionFilter__draw, 0x140601A80
- God rays: uGodRaysFilter__draw, 0x140609F00
- Radial blur, haze, distortion, chromatic aberration, TV noise and bokeh
Returning early from selected draw methods is a practical implementation, but first instrument call counts because engine support does not prove UMVC3 actively uses every filter. I would preserve toon outlines, hatching, color correction, HUD, and hit effects for readability.
Mod Exposure
The F1 DTI Inspector can edit captured scalar properties, but sRender and sShadow currently have createPropertyAddr = 0 in dtieditor.cpp, so their properties cannot be reliably enumerated on demand. Adding:
- sRender: 0x140538610
- sShadow: 0x1406671E0
would make these useful for runtime experiments before building permanent controls.
The rollback render suppression should not be reused as a playable low-spec mode. It deliberately skips presentation during re-simulation and can produce a completely undrawn frame.
Recommended Presets
A practical first implementation would be:
- Low: native resolution, AA off, lowest texture detail, low effect volume.
- Potato: 640x360 or 854x480 internal resolution, AA off, low effects, shadows off, bloom/DOF/motion blur/SSAO off.
- Extreme: Potato plus optional 30 FPS presentation and stage-background suppression.
Do not change simulation FPS, mBaseFps, or effect timing. UMVC3 gameplay and rollback need to remain at 60 ticks per second.
I also annotated the relevant graphics synchronization, renderer-property, shadow-property, and audio-volume functions in Ghidra.
