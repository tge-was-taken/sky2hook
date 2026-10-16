// The per-disc addresses the library reads: the RenderWare and sky2 entry points it hooks, the
// engine globals it reads, and the plugin data offsets. Field names mirror the decomp symbols so
// they stay greppable, which is why they keep that casing rather than the project's camelCase. The
// host fills this in once, before anything registers a hook.
#pragma once

#include <cstdint>

namespace sky2
{
	struct Addresses
	{
		uint32_t RpWorldRender = 0;
		uint32_t _rwPluginRegistryAddPlugin = 0;
		uint32_t sMainCamera = 0; // RwCamera**
		uint32_t sAmbientLight = 0; // RpLight**
		uint32_t sDirectionalLight = 0; // RpLight**
		uint32_t sDirectionalLight2 = 0; // RpLight**
		uint32_t debugDrawBoundingSpheres = 0; // int32 debug-draw flag
		uint32_t unk_7CE158 = 0; // int32 debug-draw flag
		uint32_t skyDisplayRaster = 0;
		uint32_t skyDrawRaster = 0;
		uint32_t SkyRasterShowRaster = 0;
		uint32_t SkyCameraBeginUpdate = 0;
		uint32_t SkyCameraEndUpdate = 0;
		uint32_t RwCameraClear = 0;
		uint32_t AtomicDefaultRenderCallBack = 0;
		uint32_t _rpSectorDefaultRenderCallBack = 0;
		uint32_t _rwSkySetRenderState = 0;
		uint32_t RpSkyRenderStateSet = 0;
		uint32_t RpSkyTextureSetMipmapK = 0;
		uint32_t RpSkyRenderStateGet = 0;
		uint32_t RpSkySelectDeepZBuffer = 0;
		uint32_t RpSkySelectTrueTSClipper = 0;
		uint32_t RpSkySelectTrueTLClipper = 0;
		uint32_t RpSkyGetDrawBufferRaster = 0;
		uint32_t RpSkyGetDisplayBufferRaster = 0;
		uint32_t RpSkyTexCacheAccessSpeculate = 0;
		uint32_t RpSkyTexCacheAccessRaster = 0;
		uint32_t RpSkyTexCacheReleaseRaster = 0;
		uint32_t RpSkyTexCacheRasterLock = 0;
		uint32_t RpSkyTexCacheRestore = 0;
		uint32_t RpSkyTexGetTex0 = 0;
		uint32_t RpSkyTexCalcTex1 = 0;
		uint32_t SkyRasterCreate = 0;
		uint32_t SkyRasterUnlock = 0;
		uint32_t SkyRasterUnlockPalette = 0;
		uint32_t SkyRasterDestroy = 0;
		uint32_t sceMpegGetPicture = 0;
		uint32_t SkyIm2DRenderPrimitive = 0;
		uint32_t SkyIm2DRenderTriangle = 0;
		uint32_t SkyIm2DRenderLine = 0;
		uint32_t SkyIm2DRenderIndexedPrimitive = 0;
		uint32_t RwIm3DTransform = 0;
		uint32_t RwIm3DRenderPrimitive = 0;
		uint32_t RwIm3DRenderIndexedPrimitive = 0;
		// The two the Im3D funnel used to miss. Both appear in P4's render command dispatcher beside
		// the primitive entries, so a command list can reach them without touching the hooked ones.
		uint32_t RwIm3DRenderLine = 0;
		uint32_t RwIm3DEnd = 0;
		uint32_t _rt2dPS2PathFill = 0;
		uint32_t _rt2dPS2PathStroke = 0;
		uint32_t _rt2dPS2FontShow = 0;
		uint32_t Rt2dGlobals = 0;
		uint32_t rpPDS_G3x_ADLCloneRenderMtx = 0;
		uint32_t rpPDS_G3x_OPLCloneRenderMtxRGBA = 0;
		uint32_t RxPipelineExecute = 0;
		uint32_t mdlEffectDMAOpenPushAlphaOnlyMaskVIFPkt = 0;
		uint32_t mdlEffectDMAOpenPushColorOnlyMaskVIFPkt = 0;
		uint32_t mdlEffectDMAOpenEmitPopFrameMaskPkt = 0;
		uint32_t mdlEffectDMAOpenPopFrameMaskVIFPkt = 0;
		uint32_t mdlEffectDMAOpenSetClampVIFPkt = 0;
		uint32_t mdlEffectDMAOpenRestoreClampVIFPkt = 0;
		uint32_t RpAtomicCreate = 0;
		uint32_t RpAtomicDestroy = 0;
		uint32_t RpClumpCreate = 0;
		uint32_t RpClumpDestroy = 0;
		uint32_t RpGeometryUnlock = 0;
		uint32_t RpWorldDestroy = 0;
		uint32_t RpWorldUnlock = 0;
		uint32_t RpGeometryDestroy = 0;
		uint32_t RpLightCreate = 0;
		uint32_t RpLightDestroy = 0;
		uint32_t RpMaterialCreate = 0;
		uint32_t RpMaterialDestroy = 0;
		uint32_t RwCameraCreate = 0;
		uint32_t RwCameraDestroy = 0;
		uint32_t RwFrameCreate = 0;
		uint32_t RwFrameDestroy = 0;
		uint32_t RwRasterCreate = 0;
		uint32_t RwRasterDestroy = 0;
		uint32_t RwTextureCreate = 0;
		uint32_t RwTextureDestroy = 0;

		uint32_t RenderWareVersion = 37;

		uint32_t RpSkinGeometryDataOffset = 0;
		uint32_t RpSkinAtomicDataOffset = 0;
		uint32_t MatFXMaterialDataOffset = 0;
		uint32_t MatFXAtomicDataOffset = 0;
		uint32_t rpPTankAtomicDataOffset = 0; // holds a pointer to RpPTankAtomicExtPrv
		uint32_t cameraExtOffset = 0; // curWorld = *(camera + *cameraExtOffset + 0x0c)
		uint32_t worldSectorNativeDataOffset = 0;
		uint32_t geometryNativeDataOffset = 0;
	};

	void setAddresses(const Addresses& a);
	const Addresses& addresses();

	// A plugin's data offset within an object, discovered at registration (the pluginRegistryAddPlugin
	// handler records them). Keyed by the object's base struct size and the plugin id, both stable
	// constants, so a per-disc offset global is not needed on a normal boot. offsetGlobal is the disc's
	// own offset global for that plugin, read when registration was never observed (a savestate
	// restores past it). Returns 0 when neither source has it.
	void recordPluginOffset(uint32_t baseSize, uint32_t pluginID, uint32_t offset);
	uint32_t pluginOffset(uint32_t baseSize, uint32_t pluginID, uint32_t offsetGlobal = 0);
} // namespace sky2
