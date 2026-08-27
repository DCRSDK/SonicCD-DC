#ifndef DRAWING_H
#define DRAWING_H

// #define SURFACE_COUNT (24)
#define SURFACE_COUNT (32) // originally 24, updated to 32 in sega forever vers
#define GFXDATA_SIZE  (0x800 * 0x800)

// usually 7, but origins has an extra one for some reason
#define DRAWLAYER_COUNT (8)

enum FlipFlags { FLIP_NONE, FLIP_X, FLIP_Y, FLIP_XY };
enum InkFlags { INK_NONE, INK_BLEND, INK_ALPHA, INK_ADD, INK_SUB };
enum DrawFXFlags { FX_SCALE, FX_ROTATE, FX_ROTOZOOM, FX_INK, FX_TINT, FX_FLIP };

struct DrawListEntry {
    int entityRefs[ENTITY_COUNT];
    int listSize;
};

struct GFXSurface {
    // char fileName[0x40];
    char fileName[0x80]; // originally 0x40, updated to 0x80 in sega forever vers
    int height;
    int width;
    int widthShifted;
    int texStartX;
    int texStartY;
    int dataPosition;
};

extern ushort blendLookupTable[0x100 * 0x20];
extern ushort subtractLookupTable[0x100 * 0x20];
extern ushort tintLookupTable[0x10000];

extern int SCREEN_XSIZE;
extern int SCREEN_CENTERX;
extern int SCREEN_XSIZE_CONFIG;

extern int touchWidth;
extern int touchHeight;

extern DrawListEntry drawListEntries[DRAWLAYER_COUNT];

extern int gfxDataPosition;
extern GFXSurface gfxSurface[SURFACE_COUNT];
extern byte graphicData[GFXDATA_SIZE];

#if RETRO_USE_ORIGINAL_CODE
#define VERTEX_COUNT (0x2000)
#else
#define VERTEX_COUNT (0x4000) // doubled so debug overlays & etc work
#endif
#define INDEX_COUNT         (VERTEX_COUNT * 6)

// Special-stage floor draw distance, in tiles per side of the sampled grid.
//
// Draw3DFloorLayer walks a GRID x GRID square of 16-unit floor tiles centred on
// the camera, biased forward along its facing, and emits a quad for every one
// that carries graphics. The floor simply stops where the grid stops, so this IS
// the draw distance — stock RSDKv3 hardcodes 32 (a 512x512 unit window) on the
// high-quality path and 20 (320x320) on the low one.
//
// Cost is QUADRATIC. Every tile is four vertices built by the tile loop here and
// four perspective-divided vertex transforms in the backend each frame, so 40 is
// 1.6x the work of 32 and 64 is 4x. On the Dreamcast it also all lands in the
// PVR's translucent list.
#ifndef HQ_FLOOR_GRID
#if RETRO_USING_KOS && defined(DC_FLOOR_GRID)
#define HQ_FLOOR_GRID (DC_FLOOR_GRID)
#else
#define HQ_FLOOR_GRID (32) // stock RSDKv3
#endif
#endif

// Distant floor, drawn coarse. THE FINE GRID ABOVE IS NOT THE DRAW DISTANCE ANY
// MORE — it is only the distance at which the floor is drawn at full detail.
//
// The fine grid costs GRID^2 quads for a reach of GRID*16 units, so buying
// distance with it is quadratic and runs out fast. Past the point where a floor
// tile is a few pixels tall on screen, none of that detail survives the
// perspective divide: the tile is squashed to nothing and all you paid for is
// vertex transforms. So beyond the fine grid the floor is sampled at
// LOD_FLOOR_STEP units instead of 16, and each coarse quad is textured by
// stretching the ONE 16x16 sub-tile that its top-left corner lands in over the
// whole thing. That is the same trick the iOS build and the Mania Dreamcast port
// use, and at distance the difference is invisible while the cost is
// (STEP/16)^2 times lower per unit of ground covered.
//
// With STEP 64 a coarse quad covers 16 fine tiles for the price of one.
//
// Set LOD_FLOOR_GRID to 0 to switch the coarse ring off entirely and get the
// old fine-grid-only behaviour back.
#ifndef LOD_FLOOR_STEP
#if RETRO_USING_KOS && defined(DC_FLOOR_LOD_STEP)
#define LOD_FLOOR_STEP (DC_FLOOR_LOD_STEP)
#else
#define LOD_FLOOR_STEP (64)
#endif
#endif

#ifndef LOD_FLOOR_GRID
#if RETRO_USING_KOS && defined(DC_FLOOR_LOD_GRID)
#define LOD_FLOOR_GRID (DC_FLOOR_LOD_GRID)
#else
#define LOD_FLOOR_GRID (0) // off everywhere but the Dreamcast
#endif
#endif

// Power of two and a multiple of 16, because the ring origin is aligned with
// `& ~(STEP-1)` (which has to work for negative world coordinates too) and the
// sub-tile lookup is a 16-unit grid.
static_assert(LOD_FLOOR_GRID == 0 || (LOD_FLOOR_STEP >= 16 && (LOD_FLOOR_STEP & (LOD_FLOOR_STEP - 1)) == 0),
              "LOD_FLOOR_STEP must be a power of two and at least 16");

// polyList3D has to hold the floor's GRID*GRID quads plus the base quad under
// them. Stock is 0x1904 (6404), which fits a grid of 40 exactly and nothing
// larger — so rather than capping the grid at whatever the old constant happened
// to allow, the array grows to fit when asked for more. At the stock 32 this
// evaluates to 0x1904 unchanged, so no other platform moves.
//
// The vertex is 20 bytes, so the cost of a bigger grid is
// (GRID*GRID*4 + 4) * 20 bytes of static: 125KB at 32, 320KB at 64.
// The coarse ring skips every cell that falls entirely inside the fine grid, so
// this is an upper bound rather than the real count — sizing a static array off
// the worst case is the right trade for a few KB.
#define FLOOR3D_QUAD_BUDGET (HQ_FLOOR_GRID * HQ_FLOOR_GRID + LOD_FLOOR_GRID * LOD_FLOOR_GRID)
#define FLOOR3D_VERTEX_NEED (FLOOR3D_QUAD_BUDGET * 4 + 4)
#if FLOOR3D_VERTEX_NEED > 0x1904
#define VERTEX3D_COUNT      (FLOOR3D_VERTEX_NEED)
#else
#define VERTEX3D_COUNT      (0x1904)
#endif
#define TILEUV_SIZE         (0x1000)
#define HW_TEXTURE_COUNT    (6)
#define HW_TEXTURE_SIZE     (0x400)
#define HW_TEXTURE_DATASIZE (HW_TEXTURE_SIZE * HW_TEXTURE_SIZE * 2)
#define HW_TEXBUFFER_SIZE   (HW_TEXTURE_SIZE * HW_TEXTURE_SIZE)

struct DrawVertex {
    short x;
    short y;
    short u;
    short v;

    Colour colour;
};

struct DrawVertex3D {
    float x;
    float y;
    float z;
    short u;
    short v;

    Colour colour;
};

extern DrawVertex gfxPolyList[VERTEX_COUNT];
extern short gfxPolyListIndex[INDEX_COUNT];
extern ushort gfxVertexSize;
extern ushort gfxVertexSizeOpaque;
extern ushort gfxIndexSize;
extern ushort gfxIndexSizeOpaque;

extern DrawVertex3D polyList3D[VERTEX3D_COUNT];

extern ushort vertexSize3D;
extern ushort indexSize3D;

// Both counters above are 16-bit, and the floor drives them harder than anything
// else does. This is the real ceiling on the floor as a whole: the index count
// runs out first, at roughly 104 fine tiles per side with no ring. Caught here rather than as silent wraparound on
// hardware, where it would look like the floor tearing itself apart.
static_assert(FLOOR3D_QUAD_BUDGET * 4 + 4 <= 0xFFFF, "floor grid + LOD ring overflows vertexSize3D (ushort)");
static_assert(FLOOR3D_QUAD_BUDGET * 6 + 6 <= 0xFFFF, "floor grid + LOD ring overflows indexSize3D (ushort)");
extern ushort tileUVArray[TILEUV_SIZE];
extern float floor3DXPos;
extern float floor3DYPos;
extern float floor3DZPos;
extern float floor3DAngle;
extern bool render3DEnabled;
extern bool hq3DFloorEnabled;

#if RETRO_USING_KOS
extern ushort *texBuffer; // heap-backed on DC, see Drawing.cpp
#else
extern ushort texBuffer[HW_TEXBUFFER_SIZE];
#endif
extern byte texBufferMode;

#if !RETRO_USE_ORIGINAL_CODE
extern int viewOffsetX;
extern int viewOffsetY;
#endif
extern int viewWidth;
extern int viewHeight;
extern float viewAspect;
extern int bufferWidth;
extern int bufferHeight;
extern int virtualX;
extern int virtualY;
extern int virtualWidth;
extern int virtualHeight;
extern float viewAngle;
extern float viewAnglePos;

#if RETRO_USING_OPENGL
extern GLuint gfxTextureID[HW_TEXTURE_COUNT];
extern GLuint framebufferHW;
extern GLuint renderbufferHW;
extern GLuint retroBuffer;
extern GLuint retroBuffer2x;
extern GLuint videoBuffer;
#endif
extern DrawVertex screenRect[4];
extern DrawVertex retroScreenRect[4];

int InitRenderDevice();
void FlipScreen();
void FlipScreenFB();
void FlipScreenNoFB();
void FlipScreenHRes();
void RenderFromTexture();
void RenderFromRetroBuffer();

void FlipScreenVideo();

void ReleaseRenderDevice();

void SetFullScreen(bool fs);

void GenerateBlendLookupTable();

inline void ClearGraphicsData()
{
    for (int i = 0; i < SURFACE_COUNT; ++i) StrCopy(gfxSurface[i].fileName, "");
    gfxDataPosition = 0;
}
void ClearScreen(byte index);

void SetScreenSize(int width, int lineSize);
void CopyFrameOverlay2x();
void TransferRetroBuffer();

inline bool CheckSurfaceSize(int size)
{
    for (int cnt = 2; cnt < 2048; cnt <<= 1) {
        if (cnt == size)
            return true;
    }
    return false;
}

void UpdateHardwareTextures();
void SetScreenDimensions(int width, int height, int winWidth, int winHeight);
void ScaleViewport(int width, int height);
void CalcPerspective(float fov, float aspectRatio, float nearPlane, float farPlane);

void SetupPolygonLists();
void UpdateTextureBufferWithTiles();
void UpdateTextureBufferWithSortedSprites();
void UpdateTextureBufferWithSprites();

// Layer Drawing
void DrawObjectList(int layer);
void DrawStageGFX();
#if !RETRO_USE_ORIGINAL_CODE
void DrawDebugOverlays();
#endif

// TileLayer Drawing
void DrawHLineScrollLayer(int layerID);
void DrawVLineScrollLayer(int layerID);
void Draw3DFloorLayer(int layerID);
void Draw3DSkyLayer(int layerID);

// Shape Drawing
void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A);
void SetFadeHQ(int R, int G, int B, int A);
void DrawTintRectangle(int XPos, int YPos, int width, int height);
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int sheetID);

// Sprite Drawing
void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID);
void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID);
void DrawSpriteScaled(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID);
void DrawScaledChar(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                    int sheetID);
void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID);
void DrawSpriteRotozoom(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation, int scale,
                        int sheetID);

void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID);
void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID);
void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID);
void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID);

void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos);

void DrawFace(void *v, uint colour);
void DrawTexturedFace(void *v, byte sheetID);

void DrawBitmapText(void *menu, int XPos, int YPos, int scale, int spacing, int rowStart, int rowCount);

void DrawTextMenu(void *menu, int XPos, int YPos);
void DrawTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight);
void DrawStageTextEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight);
void DrawBlendedTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight);
void DrawBitmapText(void *menu, int XPos, int YPos, int scale, int spacing, int rowStart, int rowCount);

#endif // !DRAWING_H
