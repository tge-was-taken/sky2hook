#pragma once
// RenderWare 3.7 ABI overlay: the guest struct layouts and enums as the game lays them out in EE
// RAM. Names mirror the RenderWare API (RwCamera, RpAtomic, ...); declared at global scope.
#include "sky2/guest.h"

enum RwRenderState
{
	rwRENDERSTATETEXTURERASTER = 1,
	rwRENDERSTATETEXTUREADDRESS = 2,
	rwRENDERSTATETEXTUREADDRESSU = 3,
	rwRENDERSTATETEXTUREADDRESSV = 4,
	rwRENDERSTATETEXTUREPERSPECTIVE = 5,
	rwRENDERSTATEZTESTENABLE = 6,
	rwRENDERSTATESHADEMODE = 7,
	rwRENDERSTATEZWRITEENABLE = 8,
	rwRENDERSTATETEXTUREFILTER = 9,
	rwRENDERSTATESRCBLEND = 10,
	rwRENDERSTATEDESTBLEND = 11,
	rwRENDERSTATEVERTEXALPHAENABLE = 12,
	rwRENDERSTATEBORDERCOLOR = 13,
	rwRENDERSTATEFOGENABLE = 14,
	rwRENDERSTATEFOGCOLOR = 15,
	rwRENDERSTATEFOGTYPE = 16,
	rwRENDERSTATEFOGDENSITY = 17,
	// 18 and 19 do not exist in the core enum; the game never sets them.
	rwRENDERSTATECULLMODE = 20,
	rwRENDERSTATESTENCILENABLE = 21,
	rwRENDERSTATESTENCILFAIL = 22,
	rwRENDERSTATESTENCILZFAIL = 23,
	rwRENDERSTATESTENCILPASS = 24,
	rwRENDERSTATESTENCILFUNCTION = 25,
	rwRENDERSTATESTENCILFUNCTIONREF = 26,
	rwRENDERSTATESTENCILFUNCTIONMASK = 27,
	rwRENDERSTATESTENCILFUNCTIONWRITEMASK = 28,
	rwRENDERSTATEALPHATESTFUNCTION = 29,
	rwRENDERSTATEALPHATESTFUNCTIONREF = 30
};

enum RwCullMode
{
	rwCULLMODECULLNONE = 1,
	rwCULLMODECULLBACK = 2,
	rwCULLMODECULLFRONT = 3
};

enum RwStencilOperation // STENCILFAIL/ZFAIL/PASS values; map 1:1 to GL stencil ops
{
	rwSTENCILOPERATIONKEEP = 1,
	rwSTENCILOPERATIONZERO = 2,
	rwSTENCILOPERATIONREPLACE = 3,
	rwSTENCILOPERATIONINCRSAT = 4,
	rwSTENCILOPERATIONDECRSAT = 5,
	rwSTENCILOPERATIONINVERT = 6,
	rwSTENCILOPERATIONINCR = 7, // wraps (GL_INCR_WRAP); INCRSAT clamps (GL_INCR)
	rwSTENCILOPERATIONDECR = 8
};

enum RwStencilFunction // STENCILFUNCTION value; maps 1:1 to GL depth/stencil funcs
{
	rwSTENCILFUNCTIONNEVER = 1,
	rwSTENCILFUNCTIONLESS = 2,
	rwSTENCILFUNCTIONEQUAL = 3,
	rwSTENCILFUNCTIONLESSEQUAL = 4,
	rwSTENCILFUNCTIONGREATER = 5,
	rwSTENCILFUNCTIONNOTEQUAL = 6,
	rwSTENCILFUNCTIONGREATEREQUAL = 7,
	rwSTENCILFUNCTIONALWAYS = 8
};

enum RwShadeMode
{
	rwSHADEMODEFLAT = 1,
	rwSHADEMODEGOURAUD = 2
};

enum RwPrimitiveType
{
	rwPRIMTYPELINELIST = 1,
	rwPRIMTYPEPOLYLINE = 2,
	rwPRIMTYPETRILIST = 3,
	rwPRIMTYPETRISTRIP = 4,
	rwPRIMTYPETRIFAN = 5
};

// RpMeshHeader::flags. The low byte is the primitive type; pointlist means RenderWare does not define
// the topology and the game submits its own packet.
enum RpMeshHeaderFlags
{
	rpMESHHEADERTRISTRIP = 0x0001,
	rpMESHHEADERTRIFAN = 0x0002,
	rpMESHHEADERLINELIST = 0x0004,
	rpMESHHEADERPOLYLINE = 0x0008,
	rpMESHHEADERPOINTLIST = 0x0010,
	rpMESHHEADERPRIMMASK = 0x00FF,
	rpMESHHEADERUNINDEXED = 0x0100
};

enum RwTextureAddressMode
{
	rwTEXTUREADDRESSWRAP = 1,
	rwTEXTUREADDRESSMIRROR = 2,
	rwTEXTUREADDRESSCLAMP = 3,
	rwTEXTUREADDRESSBORDER = 4
};

enum RwTextureFilterMode
{
	rwFILTERNEAREST = 1,
	rwFILTERLINEAR = 2,
	rwFILTERMIPNEAREST = 3,
	rwFILTERMIPLINEAR = 4,
	rwFILTERLINEARMIPNEAREST = 5,
	rwFILTERLINEARMIPLINEAR = 6
};

enum RwBlendFunction
{
	rwBLENDZERO = 1,
	rwBLENDONE = 2,
	rwBLENDSRCCOLOR = 3,
	rwBLENDINVSRCCOLOR = 4,
	rwBLENDSRCALPHA = 5,
	rwBLENDINVSRCALPHA = 6,
	rwBLENDDESTALPHA = 7,
	rwBLENDINVDESTALPHA = 8,
	rwBLENDDESTCOLOR = 9,
	rwBLENDINVDESTCOLOR = 10,
	rwBLENDSRCALPHASAT = 11
};

enum RwCameraProjection
{
	rwPERSPECTIVE = 1,
	rwPARALLEL = 2
};

// RwCameraClear flags (bitmask): which planes RwCameraClear/RwCameraShowRaster touch.
enum RwCameraClearMode
{
	rwCAMERACLEARIMAGE = 1,
	rwCAMERACLEARZ = 2,
	rwCAMERACLEARSTENCIL = 4
};

enum RpLightType
{
	rpLIGHTDIRECTIONAL = 1,
	rpLIGHTAMBIENT = 2,
	rpLIGHTPOSITIONINGSTART = 0x80, // point/spot/soft-spot: positioned lights, tied to sectors not the world list
	rpLIGHTPOINT = 0x80,
	rpLIGHTSPOT = 0x81,
	rpLIGHTSPOTSOFT = 0x82
};

// RwObject.flags bits on an RpLight: which geometry the light affects. Both default on.
enum RpLightFlag
{
	rpLIGHTLIGHTATOMICS = 0x01, // lights RpAtomics
	rpLIGHTLIGHTWORLD = 0x02 // lights RpWorldSectors
};

// Keys for sky2::pluginOffset(): a plugin's data offset within an object, discovered at registration.
// Plugin IDs and object base struct sizes (RwPluginRegistry::origSizeOfStruct) are stable constants.
enum RwPluginId
{
	rwID_SKINPLUGIN = 0x116,
	rwID_MATERIALEFFECTSPLUGIN = 0x120, // MatFX
	rwID_WORLDCAMERAPLUGIN = 0x509, // registers RpWorldCameraExt (frustum sectors + world)
};
enum RwObjectBaseSize
{
	rwGEOMETRYBASE = 0x60,
	rwATOMICBASE = 0x70,
	rwMATERIALBASE = 0x1C,
	rwCAMERABASE = 0x190,
};

enum RpGeometryFlag
{
	rpGEOMETRYPRELIT = 0x08,
	rpGEOMETRYNORMALS = 0x10,
	rpGEOMETRYLIGHT = 0x20,
	rpGEOMETRYMODULATEMATERIALCOLOR = 0x40
};

// RwRasterType (RwRaster.cType low bits): what a raster is for.
enum RwRasterType
{
	rwRASTERTYPENORMAL = 0,
	rwRASTERTYPEZBUFFER = 1,
	rwRASTERTYPECAMERA = 2, // a camera frame buffer (render target)
	rwRASTERTYPETEXTURE = 4,
	rwRASTERTYPECAMERATEXTURE = 5, // render-to-texture (shadow/reflection/FMV target)
	rwRASTERTYPEMASK = 7,
};

struct RwV3d
{
	float x, y, z;
};
struct RwV2d
{
	float x, y;
};
struct RwTexCoords
{
	float u, v;
};
struct RpTriangle
{
	uint16_t vertIndex[3];
	uint16_t matIndex;
};
struct RpVertexNormal // world-sector packed normal: signed bytes, /127
{
	int8_t x, y, z;
	uint8_t pad;
};

struct RwRGBA
{
	uint8_t red, green, blue, alpha;
	uint32_t packed() const
	{
		return (uint32_t(red) << 24) | (uint32_t(green) << 16) | (uint32_t(blue) << 8) | alpha;
	}
	static RwRGBA fromPacked(uint32_t p)
	{
		return {uint8_t(p >> 24), uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
	}
	static RwRGBA fromRwLong(uint32_t p) // RWRGBALONG: a<<24 | r<<16 | g<<8 | b (e.g. FOGCOLOR)
	{
		return {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p), uint8_t(p >> 24)};
	}
	bool opaque() const { return alpha >= 0x80; }
};

struct RwRGBAReal
{
	float red, green, blue, alpha;
};
struct RwSurfaceProperties
{
	float ambient, specular, diffuse;
};
struct RwMatrixTag
{
	RwV3d right;
	uint32_t flags;
	RwV3d up;
	uint32_t pad1;
	RwV3d at;
	uint32_t pad2;
	RwV3d pos;
	uint32_t pad3;
};
struct RwSphere
{
	RwV3d center;
	float radius;
};
struct RpInterpolator
{
	int32_t flags;
	int16_t startMorphTarget;
	int16_t endMorphTarget;
	float time;
	float recipTime;
	float position;
};
struct RpMeshHeader
{
	uint32_t flags;
	uint16_t numMeshes;
	uint16_t serialNum;
	uint32_t totalIndicesInMesh;
	uint32_t firstMeshOffset;
};
struct RwMatrixWeights
{
	float w0, w1, w2, w3;
};

struct RwIm2DVertex
{
	float x, y, z, recipZ; // recipZ = 1/cameraZ
	float u, v;
	float pad0[2];
	float red, green, blue, alpha; // 0..255
	float pad1[4];
};
static_assert(sizeof(RwIm2DVertex) == 0x40, "RwIm2DVertex must be 64 bytes");
static_assert(offsetof(RwIm2DVertex, u) == 0x10 && offsetof(RwIm2DVertex, red) == 0x20, "");

// RwIm3DVertex == RxObjSpace3DLitVertex (rwcore.h): object-space position + color + normal + UV.
struct RwIm3DVertex
{
	RwV3d objVertex; // 0x00 object-space position
	RwRGBA color; // 0x0C per-vertex color (0..255)
	RwV3d objNormal; // 0x10 (unused by our unlit Im3D path)
	float u; // 0x1C
	float v; // 0x20
};
static_assert(sizeof(RwIm3DVertex) == 0x24, "RwIm3DVertex must be 36 bytes");
static_assert(offsetof(RwIm3DVertex, color) == 0x0C && offsetof(RwIm3DVertex, u) == 0x1C, "");

// RwIm3DTransformFlags (rwcore.h). These are distinct bits, not an index: reading UV presence off
// bit 0 read rwIM3D_VERTEXXYZ instead, which every call sets, so a primitive that carries no
// texcoords still had its stale u/v fields taken as real.
enum
{
	rwIM3D_VERTEXXYZ = 0x01,
	rwIM3D_VERTEXRGBA = 0x02,
	rwIM3D_VERTEXUV = 0x04,
	rwIM3D_ALLOPAQUE = 0x08,
	rwIM3D_NOCLIP = 0x10,
	rwIM3D_VERTEXTRANSFORMED = 0x20
};

struct RwLLLink;
struct RwObjectHasFrame;
struct RwFrame;
struct RpMaterial;

using RwLLLinkPtr = sky2::guest::ptr<RwLLLink>;
using VoidPtr = sky2::guest::ptr<void>;
using RwObjectHasFrameFuncPtr = sky2::guest::ptr<sky2::guest::ptr<RwObjectHasFrame>>;
using RpMaterialArrayPtr = sky2::guest::ptr<sky2::guest::ptr<RpMaterial>>;
using RwFramePtr = sky2::guest::ptr<RwFrame>;

struct RwLLLink
{
	RwLLLinkPtr next;
	RwLLLinkPtr prev;
};
struct RwLinkList
{
	RwLLLink link;
};
struct RwObject
{
	uint8_t type;
	uint8_t subType;
	uint8_t flags;
	uint8_t privateFlags;
	VoidPtr parent;
};
struct RwObjectHasFrame
{
	RwObject object;
	RwLLLink lFrame;
	RwObjectHasFrameFuncPtr sync;
};

struct RwFrame;
struct RwCamera;
struct RpAtomic;
struct RpLight;
struct RpGeometry;
struct RpMorphTarget;
struct RpMaterial;
struct RpMesh;
struct RwRaster;
struct RwTexture;

using RwCameraFuncPtr = sky2::guest::ptr<sky2::guest::ptr<RwCamera>>;
using RwRasterPtr = sky2::guest::ptr<RwRaster>;
using RpGeometryPtr = sky2::guest::ptr<RpGeometry>;
using RpAtomicFuncPtr = sky2::guest::ptr<sky2::guest::ptr<RpAtomic>>;
using RpTrianglePtr = sky2::guest::ptr<RpTriangle>;
using RwRGBAPtr = sky2::guest::ptr<RwRGBA>;
using RwTexCoordsPtr = sky2::guest::ptr<RwTexCoords>;
using RpMeshHeaderPtr = sky2::guest::ptr<RpMeshHeader>;
using RpMorphTargetPtr = sky2::guest::ptr<RpMorphTarget>;
using RwTexturePtr = sky2::guest::ptr<RwTexture>;
using RpMaterialPtr = sky2::guest::ptr<RpMaterial>;
using RwMatrixTagPtr = sky2::guest::ptr<RwMatrixTag>;
using RwMatrixWeightsPtr = sky2::guest::ptr<RwMatrixWeights>;
using RwV3dPtr = sky2::guest::ptr<RwV3d>;
using Uint8Ptr = sky2::guest::ptr<uint8_t>;
using Uint16Ptr = sky2::guest::ptr<uint16_t>;
using Uint32Ptr = sky2::guest::ptr<uint32_t>;

struct RpMaterialList
{
	RpMaterialArrayPtr materials;
	int32_t numMaterials;
	int32_t space;
};

struct RwFrame
{
	RwObject object;
	RwLLLink inDirtyListLink;
	RwMatrixTag modelling;
	RwMatrixTag ltm;
	RwLinkList objectList;
	RwFramePtr child;
	RwFramePtr next;
	RwFramePtr root;
	uint8_t tail[176 - 0xA4]; // TODO: add missing members
};
struct RwCamera
{
	RwObjectHasFrame object;
	int32_t projectionType;
	RwCameraFuncPtr beginUpdate;
	RwCameraFuncPtr endUpdate;
	RwMatrixTag viewMatrix;
	RwRasterPtr frameBuffer;
	RwRasterPtr zBuffer;
	RwV2d viewWindow;
	RwV2d recipViewWindow;
	RwV2d viewOffset;
	float nearPlane;
	float farPlane;
	float fogPlane;
	float zScale;
	float zShift;
	uint8_t tail[400 - 0x94]; // TODO: add missing members
};
struct RpAtomic
{
	RwObjectHasFrame object;
	VoidPtr repEntry;
	RpGeometryPtr geometry;
	RwSphere boundingSphere;
	RwSphere worldBoundingSphere;
	VoidPtr clump;
	RwLLLink inClumpLink;
	RpAtomicFuncPtr renderCallBack;
	RpInterpolator interpolator;
	uint16_t renderFrame;
	uint16_t pad;
	RwLinkList llWorldSectorsInAtomic;
	VoidPtr pipeline;
};
struct RpGeometry
{
	RwObject object;
	uint32_t flags;
	uint16_t lockedSinceLastInst;
	int16_t refCount;
	int32_t numTriangles;
	int32_t numVertices;
	int32_t numMorphTargets;
	int32_t numTexCoordSets;
	RpMaterialList matList;
	RpTrianglePtr triangles;
	RwRGBAPtr preLitLum;
	RwTexCoordsPtr texCoords[8];
	RpMeshHeaderPtr mesh;
	VoidPtr repEntry;
	RpMorphTargetPtr morphTarget;
};
// World (BSP) geometry: like RpGeometry but vertices are already in world space and materials come
// from the world material list (windowed by matListWindowBase). Rendered via _rpSectorDefaultRenderCallBack.
struct RpWorldSector
{
	int32_t type; // 0x00
	RpTrianglePtr triangles; // 0x04
	RwV3dPtr vertices; // 0x08
	sky2::guest::ptr<RpVertexNormal> normals; // 0x0C packed signed-byte normals (may be null)
	RwTexCoordsPtr texCoords[8]; // 0x10
	RwRGBAPtr preLitLum; // 0x30
	VoidPtr repEntry; // 0x34
	RwLinkList collAtomicsInWorldSector; // 0x38 (RpTie list)
	// 0x40, not the decomp reference's 0x48: P3's sector omits the noCollAtomics list, which is also
	// why mesh lands at 0x78 (8 bytes earlier). List of RpLightTie for this sector's positioned lights.
	RwLinkList lightsInWorldSector; // 0x40
	uint8_t boundingBoxes[0x78 - 0x48]; // 0x48 boundingBox + tightBoundingBox
	RpMeshHeaderPtr mesh; // 0x78
	VoidPtr pipeline; // 0x7C
	uint16_t matListWindowBase; // 0x80
	uint16_t numVertices; // 0x82
	uint16_t numTriangles; // 0x84
	uint16_t pad; // 0x86
};
using RpWorldSectorPtr = sky2::guest::ptr<RpWorldSector>;

// RW 3.5's sector, which keeps the noCollAtomics list P3's build drops. Same fields otherwise, so
// sky2 reads either through readSectorFields().
struct RpWorldSectorRW35
{
	int32_t type; // 0x00
	RpTrianglePtr triangles; // 0x04
	RwV3dPtr vertices; // 0x08
	sky2::guest::ptr<RpVertexNormal> normals; // 0x0C
	RwTexCoordsPtr texCoords[8]; // 0x10
	RwRGBAPtr preLitLum; // 0x30
	VoidPtr repEntry; // 0x34
	RwLinkList collAtomicsInWorldSector; // 0x38
	RwLinkList noCollAtomicsInWorldSector; // 0x40
	RwLinkList lightsInWorldSector; // 0x48
	uint8_t boundingBoxes[0x80 - 0x50]; // 0x50 boundingBox + tightBoundingBox
	RpMeshHeaderPtr mesh; // 0x80
	VoidPtr pipeline; // 0x84
	uint16_t matListWindowBase; // 0x88
	uint16_t numVertices; // 0x8A
	uint16_t numTriangles; // 0x8C
	uint16_t pad; // 0x8E
};

// Sector<->object ties. An atomic/light touching a sector gets a tie node linked into both the
// sector's list and the object's list; the render path walks these to find an object's sectors and a
// sector's lights.
struct RpTie
{
	RwLLLink lAtomicInWorldSector; // 0x00 (in sector.collAtomicsInWorldSector)
	sky2::guest::ptr<RpAtomic> apAtom; // 0x08
	RwLLLink lWorldSectorInAtomic; // 0x0C (in atomic.llWorldSectorsInAtomic)
	RpWorldSectorPtr worldSector; // 0x14
};
struct RpLightTie
{
	RwLLLink lightInWorldSector; // 0x00 (in sector.lightsInWorldSector)
	sky2::guest::ptr<RpLight> light; // 0x08
	RwLLLink worldSectorInLight; // 0x0C (in light.sectorsInLight)
	RpWorldSectorPtr sect; // 0x14
};
struct RpMorphTarget
{
	RpGeometryPtr parentGeom;
	RwSphere boundingSphere;
	RwV3dPtr verts;
	RwV3dPtr normals;
};
struct RpMaterial
{
	RwTexturePtr texture;
	RwRGBA color;
	VoidPtr pipeline;
	RwSurfaceProperties surfaceProps;
	int16_t refCount;
	int16_t pad;
};
struct RpMesh
{
	Uint16Ptr indices;
	uint32_t numIndices;
	RpMaterialPtr material;
};
struct RwRaster
{
	RwRasterPtr parent;
	Uint8Ptr cpPixels;
	Uint8Ptr palette;
	int32_t width;
	int32_t height;
	int32_t depth;
	int32_t stride;
	int16_t nOffsetX;
	int16_t nOffsetY;
	uint8_t cType;
	uint8_t cFlags;
	uint8_t privateFlags;
	uint8_t cFormat;
	Uint8Ptr originalPixels;
	int32_t originalWidth;
	int32_t originalHeight;
	int32_t originalStride;
};

struct RwTexture
{
	RwRasterPtr raster;
	VoidPtr dict;
	RwLLLink lInDictionary;
	char name[32];
	char mask[32];
	uint32_t filterAddressing;
	int32_t refCount;
};

// ---- Rt2d (the 2D vector toolkit UI is built from) --------------------------------------------
// Layouts are the RW 3.7 ones this game was built against, not the 3.1 SDK's. The two differ most in
// Rt2dFont, which here indirects through an RwSList instead of holding an inline glyph array.

// A path segment. `normal` and `dist` are precomputed by the path builder: stroke expansion offsets
// along the normal, and dist is arc length along the sub-path.
enum rt2dLinetype
{
	rt2dMOVE = 0,
	rt2dLINE = 1,
	rt2dCURVE = 2
};

struct rt2dPathNode
{
	int32_t type; // rt2dLinetype
	RwV2d pos;
	RwV2d normal;
	float dist;
};

// A sub-path, chained through `next`. The PS2 fill/stroke leaves recurse over that chain by calling
// themselves, so code hooked at their entry sees one call per sub-path and must not walk it again.
struct Rt2dPath
{
	sky2::guest::ptr<rt2dPathNode> segments;
	int32_t numSegment;
	int32_t closed; // fill only runs on a closed sub-path
	int32_t flat; // 0 = `segments` holds curve control points, not an outline
	float inset;
	sky2::guest::ptr<Rt2dPath> next;
	sky2::guest::ptr<Rt2dPath> curr;
};

// Colour + UV at one end of a brush's vertical gradient. Colours are 0..1 floats here, unlike the
// 0..255 bytes the Im2D/Im3D vertex paths carry.
struct rt2dShadeParameters
{
	RwRGBAReal col;
	RwV2d uv;
};

struct Rt2dBrush
{
	rt2dShadeParameters top, dtop, bottom, dbottom;
	RwRGBA colorCache;
	int32_t flag;
	RwTexturePtr texture; // null = untextured
	VoidPtr material;
	float halfwidth; // stroke width / 2
	int32_t refCount;
};

// RwSList: a growable array. `entrySize` is the element stride.
struct RwSList
{
	VoidPtr listElements;
	int32_t numElementsFilled;
	int32_t numElementsAlloced;
	int32_t entrySize;
};

// One glyph: its advance in units of the show height, the page it lives on, and its UV rect there.
struct rt2dCharRect
{
	float width;
	RwTexturePtr tex;
	RwV2d uv[2]; // uv[0] = min, uv[1] = max
	sky2::guest::ptr<Rt2dPath> path; // outline fonts only
	int8_t charpage;
};

struct Rt2dFont
{
	int32_t isOutline;
	float height, baseline, intergap;
	uint32_t flag; // bit 1: the string is 2-byte characters
	RwTexturePtr textures[4];
	int16_t aCharIndex[128]; // low characters -> charMap index; negative = unmapped
	int32_t uCharOffset; // the extended range starts at this character code
	int32_t uCharIndexCount;
	sky2::guest::ptr<int16_t> uCharIndex; // extended range -> charMap index
	int32_t charCount;
	sky2::guest::ptr<RwSList> charMap; // rt2dCharRect[]
	VoidPtr fontshowCB;
	sky2::guest::ptr<Rt2dFont> nextBatch;
	VoidPtr strCache;
};

// Rt2dGlobalVars fields, as offsets from the Rt2dGlobals base. Only the transform inputs are named:
// the struct is 4400 bytes of mostly unrelated state, so reading it as a whole would be dead weight.
// Verified against the game code rather than taken from the decomp's symbol names, which are
// mislabelled past 0x538.
namespace rt2dGlobalOffset
{
	constexpr uint32_t METRIC_MTX = 0x550; // RwMatrixTag
	constexpr uint32_t CTM_STACK = 0x590; // RwMatrixTag*[32]
	constexpr uint32_t MTOS = 0x610; // int32: live depth of CTM_STACK
	constexpr uint32_t VW = 0x614; // RwV2d: the view size the device metric is built from
	constexpr uint32_t CTM = 0x6b0; // RwMatrixTag: only valid when CTM_VALID is set
	constexpr uint32_t CTM_VALID = 0x6f0; // int32
	constexpr uint32_t TRANSFORM_FLAGS = 0x6f4; // uint32: the rwIM3D_* flags the draw is submitted with
	constexpr uint32_t LAYER_DEPTH = 0x6f8; // float: the 2D layer scale, already folded into CTM
	constexpr uint32_t TOLERANCE = 0x6fc; // float: curve flattening tolerance
	constexpr uint32_t VIEW_CAMERA = 0x724; // RwCamera*
} // namespace rt2dGlobalOffset

// RpMatFX material-effect type (rpmatfx.h). Not a bitfield: BUMPENVMAP/DUALUVTRANSFORM are the
// combined values; the per-slot type never stores those (bump+env or uvxform+dual live in
// separate slots). Dispatch with ==.
enum RpMatFXEffect
{
	rpMATFXEFFECTNULL = 0,
	rpMATFXEFFECTBUMPMAP = 1,
	rpMATFXEFFECTENVMAP = 2,
	rpMATFXEFFECTBUMPENVMAP = 3,
	rpMATFXEFFECTDUAL = 4,
	rpMATFXEFFECTUVTRANSFORM = 5,
	rpMATFXEFFECTDUALUVTRANSFORM = 6
};

// RpMatFX per-material data (a heap block the material plugin stores a POINTER to). Two 0x40-byte
// effect slots then the combined effect type. `type` at slot+0x20 selects the effect; the leading
// words are effect-specific (see accessors below). Layout from disassembly of SLUS_216.21.
//   UVTRANSFORM: word[0]=baseTransform RwMatrix*, word[1]=dualTransform RwMatrix*
//   DUAL:        word[0]=dual RwTexture*, word[1]=srcBlend, word[2]=dstBlend (RwBlendFunction)
//   ENVMAP:      word[0]=RwFrame*, word[1]=RwTexture*, word[2]=coef(float), word[3]=useFBAlpha
// The PS2 build precomputes the second context's GS ALPHA_2 into the slot that owns the second
// texture (DUAL or ENVMAP) and the RpPDS_G3_*_PS2AllMatBridgeCallBack family copies it straight into
// the GIF packet.
// Measured 0x44/FIX=128 (source-alpha) on every dual slot and 0x68/FIX=128 (additive) on every
// envmap slot, matching the two ALPHA values the GS trace sees on context-2 batches.
struct RpMatFXSlot
{
	uint32_t word[8]; // +0x00..0x1F effect-specific
	int32_t type; // +0x20 RpMatFXEffect for this slot (0 = unused)
	uint32_t reserved0; // +0x24
	uint32_t gsAlphaLo; // +0x28 GS ALPHA_2 bits 0..31 (A/B/C/D)
	uint32_t gsAlphaFix; // +0x2C GS ALPHA_2 FIX
	uint32_t reserved[4]; // +0x30..0x3F
};
static_assert(sizeof(RpMatFXSlot) == 0x40, "RpMatFXSlot must be 0x40 bytes");

struct RpMatFXMaterialData
{
	RpMatFXSlot slot[2]; // +0x00 primary, +0x40 secondary
	int32_t effect; // +0x80 combined RpMatFXEffect
};
static_assert(offsetof(RpMatFXMaterialData, effect) == 0x80, "MatFX effect must be at +0x80");

struct RpLight
{
	RwObjectHasFrame object; // object.object.subType = RpLightType (type = Light class id, 3)
	float radius;
	RwRGBAReal color;
	float minusCosAngle;
	RwLinkList sectorsInLight;
	RwLLLink inWorld; // link in the world's directional/local light list
	uint16_t frame;
	uint16_t flags;
};

// RpWorld::flags (rpWorldFlag). Values coincide with the rpGEOMETRY* bits but the source is the
// world, not a geometry: the sector shading regime is read from here, per RW's world render path.
enum RpWorldFlag
{
	rpWORLDPRELIT = 0x08,
	rpWORLDNORMALS = 0x10,
	rpWORLDLIGHT = 0x20,
	rpWORLDMODULATEMATERIALCOLOR = 0x40,
};

struct RpWorld
{
	RwObject object;
	uint32_t flags; // rpWorldFlag, at 0x08
	uint8_t preLightListEtc[0x3C - 0x0C]; // TODO: add missing members
	RwLinkList directionalLightList;
};

struct SkinBoneData
{
	uint32_t numBones;
	uint32_t numUsedBones;
	Uint8Ptr usedBoneList;
	RwMatrixTagPtr invBoneToSkinMat;
};
struct SkinVertexMaps
{
	uint32_t maxWeights;
	Uint32Ptr matrixIndices; // TODO: 4 packed bone bytes per vertex, make concrete type for it
	RwMatrixWeightsPtr matrixWeights;
};
struct RpSkin
{
	SkinBoneData boneData;
	SkinVertexMaps vertexMaps;
};
struct RpHAnimHierarchy
{
	int32_t flags;
	int32_t numNodes;
	RwMatrixTagPtr pMatrixArray;
	VoidPtr pMatrixArrayUnaligned;
	VoidPtr pNodeInfo;
	RwFramePtr parentFrame;
};

// Sky driver subset only, see SkySystem()
enum RwStandard : int
{
	rwSTANDARDCAMERABEGINUPDATE = 1, // SkyCameraBeginUpdate
	rwSTANDARDRGBTOPIXEL = 2, // _rwSkyRGBToPixel
	rwSTANDARDPIXELTORGB = 3, // _rwSkyPixelToRGB
	rwSTANDARDRASTERCREATE = 4, // SkyRasterCreate
	rwSTANDARDRASTERDESTROY = 5, // SkyRasterDestroy
	rwSTANDARDIMAGEGETRASTER = 6, // _rwSkyImageGetRaster
	rwSTANDARDRASTERSETIMAGE = 7, // _rwSkyRasterSetImage
	rwSTANDARDTEXTURESETRASTER = 8, // SkyTextureSetRaster
	rwSTANDARDIMAGEFINDRASTERFORMAT = 9, // _rwSkyImageFindRasterFormat
	rwSTANDARDCAMERAENDUPDATE = 10, // SkyCameraEndUpdate
	rwSTANDARDRASTERSETCONTEXT = 11, // _rwSkyRasterSetContext
	rwSTANDARDRASTERSUBRASTER = 12, // SkyRasterSubRaster
	rwSTANDARDRASTERCLEARRECT = 13, // _rwSkyRasterClearRect
	rwSTANDARDRASTERCLEAR = 14, // _rwSkyRasterClear
	rwSTANDARDRASTERLOCK = 15, // SkyRasterLock
	rwSTANDARDRASTERUNLOCK = 16, // SkyRasterUnlock
	rwSTANDARDRASTERRENDER = 17, // _rwSkyRasterRender
	rwSTANDARDRASTERRENDERSCALED = 18, // _rwSkyRasterRenderScaled
	rwSTANDARDRASTERRENDERFAST = 19, // _rwSkyRasterRenderFast
	rwSTANDARDRASTERSHOWRASTER = 20, // SkyRasterShowRaster
	rwSTANDARDCAMERACLEAR = 21, // SkyCameraClear
	rwSTANDARDHINTRENDERF2B = 22, // SkyHintRenderFront2Back
	rwSTANDARDRASTERLOCKPALETTE = 23, // SkyRasterLockPalette
	rwSTANDARDRASTERUNLOCKPALETTE = 24, // SkyRasterUnlockPalette
	rwSTANDARDNATIVETEXTUREGETSIZE = 25, // SkyNativeTextureGetSize
	rwSTANDARDNATIVETEXTUREREAD = 26, // SkyNativeTextureRead
	rwSTANDARDNATIVETEXTUREWRITE = 27, // SkyNativeTextureWrite
	rwSTANDARDRASTERGETMIPLEVELS = 28, // SkyRasterGetMipLevels
	rwSTANDARDNUM = 29
};

// Sky driver subset only, see SkySystem()
enum RwDeviceSystemRequest : int
{
	rwDEVICESYSTEMOPEN = 0,
	rwDEVICESYSTEMCLOSE = 1,
	rwDEVICESYSTEMSTART = 2,
	rwDEVICESYSTEMSTOP = 3,
	rwDEVICESYSTEMGETID = 4,
	rwDEVICESYSTEMGETNUMMODES = 5,
	rwDEVICESYSTEMGETMODEINFO = 6,
	rwDEVICESYSTEMUSEMODE = 7,
	rwDEVICESYSTEMFOCUS = 8,
	rwDEVICESYSTEMGETMODE = 9,
	rwDEVICESYSTEMGETTEXMEMSIZE = 10,
	rwDEVICESYSTEMGETSTANDARDS = 11,
	rwDEVICESYSTEMGETMEMSIZE = 12,
	rwDEVICESYSTEMFINALIZESTART = 17,
	rwDEVICESYSTEMINITIATESTOP = 18,
	rwDEVICESYSTEMGETMAXTEXTURESIZE = 19,
	rwDEVICESYSTEMGETMAXCAMERADEPTH = 22
};

using RwStandardFn = int (*)(uint32_t out, uint32_t inOut, int nIn);
using RwSystemFn = int (*)(int request, uint32_t out, uint32_t inOut, int nIn);
using RwRenderStateFn = int (*)(unsigned state, uint32_t value);

struct RwDevice
{
	float gammaCorrection;
	RwSystemFn fpSystem;
	float zBufferNear;
	float zBufferFar;
	RwRenderStateFn fpRenderStateSet;
	RwRenderStateFn fpRenderStateGet;
	void* fpIm2D[4]; // unused by sky, it uses pipelines instead
	void* fpIm3D[4];
};

// RpPTank: an atomic that holds a batch of particles instead of a mesh. Field order and RpPTankData
// are the Sky2 ones, from a RenderWare build that shipped with DWARF (WWE Crush Hour, SLUS-20385).
struct RpPTankLockStruct
{
	Uint8Ptr data;
	int32_t stride;
};
// Which attributes a tank instances. The bits are a permutation of the cluster slots they fill
// (colour is 0x02 and size 0x04, against slots 4 and 3), so a bit index is not a slot index. Taken
// from rpPTankSkySetData's assignment chain, which walks the slots in order gating each on one bit.
enum RpPTankDataFlags
{
	rpPTANKDFLAGPOSITION = 0x00000001, // slot 0
	rpPTANKDFLAGMATRIX = 0x00000008, // slot 1
	rpPTANKDFLAGNORMAL = 0x00000010, // slot 2
	rpPTANKDFLAGSIZE = 0x00000004, // slot 3
	rpPTANKDFLAGCOLOR = 0x00000002, // slot 4
	rpPTANKDFLAGVTXCOLOR = 0x00000040, // slot 5
	rpPTANKDFLAG2DROTATE = 0x00000020, // slot 6
	rpPTANKDFLAGVTX2TEXCOORDS = 0x00000080, // slot 7
	rpPTANKDFLAGVTX4TEXCOORDS = 0x00000100, // slot 8
	// One value for the whole tank instead of one per particle, taken from RpPTankData's cColor/cUV
	// and friends. The constant form of an attribute is its bit shifted up by this much, which
	// _rpPTankSkyInstanceSelectCB's branches show for matrix, colour and both texcoord widths.
	rpPTANKDFLAGCNSSHIFT = 12,
	rpPTANKDFLAGUSECENTER = 0x01000000, // the quad hangs off cCenter rather than centring on the particle
	rpPTANKDFLAGCNSVTXCOLOR = 0x00040000, // one set of four corner colours for the whole tank
	rpPTANKDFLAGSTRUCTURE = 0x20000000 // one interleaved block; otherwise a buffer per attribute
};

struct RpPTankFormatDescriptor
{
	int32_t numClusters;
	int32_t stride; // per-particle size in the interleaved form, 0 in the parallel-array form
	uint32_t dataFlags; // RpPTankDataFlags
};
struct RpPTankData
{
	VoidPtr data;
	// Indexed by cluster order, which is not the data-flag bit order: colour, size, matrix and normal
	// are permuted between the two, so an index derived from a flag bit reads the wrong attribute.
	RpPTankLockStruct clusters[9];
	VoidPtr userData;
	RpPTankFormatDescriptor format;
	uint32_t srcBlend, dstBlend;
	int32_t vertexAlphaBlend;
	RwV2d cCenter, cSize;
	float cRotate;
	RwRGBA cColor;
	RwRGBA cVtxColor[4];
	RwTexCoords cUV[4];
	RwMatrixTag cMatrix;
};
struct RpPTankCallBacks
{
	VoidPtr alloc, create, instance, render;
};
struct RpPTankAtomicExtPrv
{
	int32_t maxPCount; // capacity, fixed at create
	int32_t actPCount; // live particles this frame; the buffers past it hold the last burst
	int32_t isAStructure;
	VoidPtr rawdata;
	VoidPtr defaultRenderCB;
	RpPTankCallBacks ptankCallBacks;
	// The instancers the platform picked. Their addresses name the variant in use
	// (rpPTankSkyInsPos{NC,CC}{CS,PS}{NR,PR}, ...PM for a per-particle matrix), so which of centre,
	// constant size and rotation apply is a symbol lookup rather than a guess at the data flags.
	// The 2D sprite generator reuses insNormalsCB for its size instancer
	// (rpPTankSkyInsSizePC_Spr2dGen); the others fill it only when the tank has normals.
	VoidPtr insSetupCB, insPosCB, insUVCB, insColorsCB, insNormalsCB, insEndingCB;
	uint32_t lockFlags;
	uint32_t instFlags;
	uint32_t platFlags;
	// Padding, not fields: publicData is 16-byte aligned so that its trailing RwMatrixTag is.
	uint32_t pad[2];
	RpPTankData publicData;
};

// The Sky platform's per-tank data, hanging off RpPTankData::userData. It carries the GS registers
// the render node writes straight into the DMA packet, so a tank's blend and alpha test are a read
// rather than a translation of the RW srcBlend/dstBlend words, which this path never sets.
struct RwSkySplitBits128
{
	uint64_t lo, hi;
};
struct rpPTankSkyBufferInfo
{
	RwSkySplitBits128 callTag;
	uint32_t refCnt, clrCnt, limit, start;
};
struct rpPTankSkyClusterInfo
{
	uint32_t cluster, batchSize, constSize, batchUnpack, constUnpack, cmd;
};
struct rpPTankSkyTransformInfo
{
	uint64_t prim; // GIFtag; the node ORs TME/FGE/ABE into its PRIM field before sending it
	uint64_t regs;
	uint32_t batchSize, offset, numClusters;
	VoidPtr vuCode[2];
	rpPTankSkyClusterInfo clusters[5];
	uint32_t pad;
};
struct RpPTankSkyData
{
	rpPTankSkyBufferInfo buffers[2];
	RwSkySplitBits128 clusterTags[5];
	uint64_t clusterStrides[5];
	uint64_t alpha, clamp, test, tex1; // GS ALPHA_1 / CLAMP_1 / TEST_1 / TEX1_1
	int32_t fastPipe, refillDMApacket, frameBuffering, frameIdx, bufferedFrame;
	uint32_t instFlag[2];
	uint8_t vuStride[4];
	VoidPtr vuCode;
	sky2::guest::ptr<rpPTankSkyTransformInfo> transformInfo;
	int32_t clusterConst[9];
	uint32_t maxBatches, tagBatchSize, dataBatchSize, tagConstSize, dataConstSize;
	VoidPtr resEntry[2];
	uint8_t clusters[80];
};

#define GUEST_OFF(S, F, O) static_assert(offsetof(S, F) == (O), #S "." #F)
static_assert(sizeof(VoidPtr) == 4 && sizeof(RwMatrixTag) == 64, "guest ABI mismatch");
static_assert(sizeof(RwObject) == 8 && sizeof(RwObjectHasFrame) == 20, "object size");
GUEST_OFF(RwFrame, modelling, 0x10);
GUEST_OFF(RwFrame, ltm, 0x50);
static_assert(sizeof(RwFrame) == 176, "");
GUEST_OFF(RwCamera, viewMatrix, 0x20);
GUEST_OFF(RwCamera, frameBuffer, 0x60);
GUEST_OFF(RwCamera, nearPlane, 0x80);
GUEST_OFF(RwCamera, zShift, 0x90);
static_assert(sizeof(RwCamera) == 400, "");
GUEST_OFF(RpAtomic, geometry, 0x18);
GUEST_OFF(RpAtomic, pipeline, 0x6c);
static_assert(sizeof(RpAtomic) == 112, "");
GUEST_OFF(RpGeometry, numVertices, 0x14);
GUEST_OFF(RpGeometry, matList, 0x20);
GUEST_OFF(RpGeometry, triangles, 0x2c);
GUEST_OFF(RpGeometry, texCoords, 0x34);
GUEST_OFF(RpGeometry, mesh, 0x54);
GUEST_OFF(RpGeometry, morphTarget, 0x5c);
static_assert(sizeof(RpGeometry) == 96, "");
GUEST_OFF(RpMorphTarget, verts, 0x14);
GUEST_OFF(RpMorphTarget, normals, 0x18);
GUEST_OFF(RpWorldSector, texCoords, 0x10);
GUEST_OFF(RpWorldSector, preLitLum, 0x30);
GUEST_OFF(RpWorldSector, lightsInWorldSector, 0x40);
GUEST_OFF(RpWorldSector, mesh, 0x78);
GUEST_OFF(RpWorldSector, matListWindowBase, 0x80);
GUEST_OFF(RpWorldSector, numTriangles, 0x84);
static_assert(sizeof(RpWorldSector) == 0x88, "");
GUEST_OFF(RpWorldSectorRW35, lightsInWorldSector, 0x48);
GUEST_OFF(RpWorldSectorRW35, mesh, 0x80);
GUEST_OFF(RpWorldSectorRW35, matListWindowBase, 0x88);
GUEST_OFF(RpWorldSectorRW35, numTriangles, 0x8C);
static_assert(sizeof(RpWorldSectorRW35) == 0x90, "");
GUEST_OFF(RpTie, worldSector, 0x14);
GUEST_OFF(RpTie, lWorldSectorInAtomic, 0x0C);
GUEST_OFF(RpLightTie, light, 0x08);
GUEST_OFF(RpMaterial, color, 0x04);
GUEST_OFF(RpMaterial, surfaceProps, 0x0c);
GUEST_OFF(RwRaster, cpPixels, 0x04);
GUEST_OFF(RwRaster, cFormat, 0x23);
static_assert(sizeof(RwRaster) == 52, "");
GUEST_OFF(RwTexture, filterAddressing, 0x50);
GUEST_OFF(RpLight, color, 0x18);
GUEST_OFF(RpLight, inWorld, 0x34);
GUEST_OFF(RpWorld, directionalLightList, 0x3C);
static_assert(sizeof(RpMeshHeader) == 16, "");
GUEST_OFF(SkinBoneData, invBoneToSkinMat, 0x0c);
static_assert(sizeof(SkinBoneData) == 16, "");
GUEST_OFF(SkinVertexMaps, matrixWeights, 0x08);
GUEST_OFF(RpSkin, vertexMaps, 0x10);
GUEST_OFF(RpHAnimHierarchy, pMatrixArray, 0x08);
static_assert(sizeof(rt2dPathNode) == 24, "");
GUEST_OFF(rt2dPathNode, normal, 0x0c);
GUEST_OFF(rt2dPathNode, dist, 0x14);
static_assert(sizeof(Rt2dPath) == 28, "");
GUEST_OFF(Rt2dPath, closed, 0x08);
GUEST_OFF(Rt2dPath, flat, 0x0c);
GUEST_OFF(Rt2dPath, next, 0x14);
static_assert(sizeof(rt2dShadeParameters) == 24, "");
GUEST_OFF(rt2dShadeParameters, uv, 0x10);
static_assert(sizeof(Rt2dBrush) == 120, "");
GUEST_OFF(Rt2dBrush, bottom, 0x30);
GUEST_OFF(Rt2dBrush, texture, 0x68);
GUEST_OFF(Rt2dBrush, halfwidth, 0x70);
static_assert(sizeof(RwSList) == 16, "");
static_assert(sizeof(rt2dCharRect) == 32, "");
GUEST_OFF(rt2dCharRect, uv, 0x08);
GUEST_OFF(rt2dCharRect, path, 0x18);
static_assert(sizeof(Rt2dFont) == 324, "");
GUEST_OFF(Rt2dFont, flag, 0x10);
GUEST_OFF(Rt2dFont, aCharIndex, 0x24);
GUEST_OFF(Rt2dFont, uCharOffset, 0x124);
GUEST_OFF(Rt2dFont, charMap, 0x134);
GUEST_OFF(RpPTankAtomicExtPrv, insSetupCB, 0x24);
GUEST_OFF(RpPTankAtomicExtPrv, platFlags, 0x44);
GUEST_OFF(RpPTankAtomicExtPrv, publicData, 0x50);
static_assert(sizeof(RpPTankAtomicExtPrv) == 0x140, "");
GUEST_OFF(RpPTankData, clusters, 0x04);
GUEST_OFF(RpPTankData, userData, 0x4C);
GUEST_OFF(RpPTankData, format, 0x50);
GUEST_OFF(RpPTankData, srcBlend, 0x5C);
GUEST_OFF(RpPTankData, cSize, 0x70);
GUEST_OFF(RpPTankData, cUV, 0x90);
static_assert(sizeof(RpPTankData) == 0xF0, "");
GUEST_OFF(rpPTankSkyTransformInfo, clusters, 0x24);
static_assert(sizeof(rpPTankSkyTransformInfo) == 0xA0, "");
GUEST_OFF(RpPTankSkyData, alpha, 0xB8);
GUEST_OFF(RpPTankSkyData, transformInfo, 0xFC);
GUEST_OFF(RpPTankSkyData, maxBatches, 0x124);
static_assert(sizeof(RpPTankSkyData) == 0x190, "");
#undef GUEST_OFF
