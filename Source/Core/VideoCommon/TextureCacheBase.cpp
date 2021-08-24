// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/TextureCacheBase.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#if defined(_M_X86) || defined(_M_X86_64)
#include <pmmintrin.h>
#endif

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/MemoryUtil.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/SamplerCommon.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureConversionShader.h"
#include "VideoCommon/TextureConverterShaderGen.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

static const u64 TEXHASH_INVALID = 0;
// Sonic the Fighters (inside Sonic Gems Collection) loops a 64 frames animation
static const int TEXTURE_KILL_THRESHOLD = 64;
static const int TEXTURE_POOL_KILL_THRESHOLD = 3;


// both of these were generated with xxd -i
// yes I know having a huge 400line array is here stupid, tell me a better way please
unsigned const char frogArray[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x08, 0x06, 0x00, 0x00, 0x00, 0xc3, 0x3e, 0x61,
    0xcb, 0x00, 0x00, 0x01, 0x85, 0x69, 0x43, 0x43, 0x50, 0x49, 0x43, 0x43, 0x20, 0x70, 0x72, 0x6f,
    0x66, 0x69, 0x6c, 0x65, 0x00, 0x00, 0x28, 0x91, 0x7d, 0x91, 0x3d, 0x48, 0xc3, 0x40, 0x1c, 0xc5,
    0x5f, 0xd3, 0x6a, 0x45, 0x2a, 0x1d, 0x2c, 0x54, 0xc4, 0x21, 0x43, 0x75, 0xb2, 0x20, 0x2a, 0xe2,
    0xa8, 0x55, 0x28, 0x42, 0x85, 0x50, 0x2b, 0xb4, 0xea, 0x60, 0x72, 0xe9, 0x87, 0xd0, 0xa4, 0x21,
    0x69, 0x71, 0x71, 0x14, 0x5c, 0x0b, 0x0e, 0x7e, 0x2c, 0x56, 0x1d, 0x5c, 0x9c, 0x75, 0x75, 0x70,
    0x15, 0x04, 0xc1, 0x0f, 0x10, 0x37, 0x37, 0x27, 0x45, 0x17, 0x29, 0xf1, 0x7f, 0x49, 0xa1, 0x45,
    0x8c, 0x07, 0xc7, 0xfd, 0x78, 0x77, 0xef, 0x71, 0xf7, 0x0e, 0x10, 0x1a, 0x65, 0xa6, 0x59, 0x81,
    0x31, 0x40, 0xd3, 0xab, 0x66, 0x3a, 0x99, 0x10, 0xb3, 0xb9, 0x15, 0x31, 0xf8, 0x8a, 0x6e, 0x04,
    0x10, 0x46, 0x14, 0x51, 0x99, 0x59, 0xc6, 0xac, 0x24, 0xa5, 0xe0, 0x39, 0xbe, 0xee, 0xe1, 0xe3,
    0xeb, 0x5d, 0x9c, 0x67, 0x79, 0x9f, 0xfb, 0x73, 0xf4, 0xa9, 0x79, 0x8b, 0x01, 0x3e, 0x91, 0x78,
    0x86, 0x19, 0x66, 0x95, 0x78, 0x9d, 0x78, 0x6a, 0xb3, 0x6a, 0x70, 0xde, 0x27, 0x8e, 0xb0, 0x92,
    0xac, 0x12, 0x9f, 0x13, 0x8f, 0x9a, 0x74, 0x41, 0xe2, 0x47, 0xae, 0x2b, 0x2e, 0xbf, 0x71, 0x2e,
    0x3a, 0x2c, 0xf0, 0xcc, 0x88, 0x99, 0x49, 0xcf, 0x11, 0x47, 0x88, 0xc5, 0x62, 0x07, 0x2b, 0x1d,
    0xcc, 0x4a, 0xa6, 0x46, 0x3c, 0x49, 0x1c, 0x53, 0x35, 0x9d, 0xf2, 0x85, 0xac, 0xcb, 0x2a, 0xe7,
    0x2d, 0xce, 0x5a, 0xb9, 0xc6, 0x5a, 0xf7, 0xe4, 0x2f, 0x0c, 0xe5, 0xf5, 0xe5, 0x25, 0xae, 0xd3,
    0x1c, 0x42, 0x12, 0x0b, 0x58, 0x84, 0x04, 0x11, 0x0a, 0x6a, 0xd8, 0x40, 0x19, 0x55, 0xc4, 0x69,
    0xd5, 0x49, 0xb1, 0x90, 0xa6, 0xfd, 0x84, 0x87, 0x7f, 0xd0, 0xf1, 0x4b, 0xe4, 0x52, 0xc8, 0xb5,
    0x01, 0x46, 0x8e, 0x79, 0x54, 0xa0, 0x41, 0x76, 0xfc, 0xe0, 0x7f, 0xf0, 0xbb, 0x5b, 0xab, 0x30,
    0x31, 0xee, 0x26, 0x85, 0x12, 0x40, 0xd7, 0x8b, 0x6d, 0x7f, 0x0c, 0x03, 0xc1, 0x5d, 0xa0, 0x59,
    0xb7, 0xed, 0xef, 0x63, 0xdb, 0x6e, 0x9e, 0x00, 0xfe, 0x67, 0xe0, 0x4a, 0x6f, 0xfb, 0x2b, 0x0d,
    0x60, 0xfa, 0x93, 0xf4, 0x7a, 0x5b, 0x8b, 0x1d, 0x01, 0xe1, 0x6d, 0xe0, 0xe2, 0xba, 0xad, 0x29,
    0x7b, 0xc0, 0xe5, 0x0e, 0x30, 0xf0, 0x64, 0xc8, 0xa6, 0xec, 0x48, 0x7e, 0x9a, 0x42, 0xa1, 0x00,
    0xbc, 0x9f, 0xd1, 0x37, 0xe5, 0x80, 0xfe, 0x5b, 0xa0, 0x77, 0xd5, 0xed, 0xad, 0xb5, 0x8f, 0xd3,
    0x07, 0x20, 0x43, 0x5d, 0xa5, 0x6e, 0x80, 0x83, 0x43, 0x60, 0xa4, 0x48, 0xd9, 0x6b, 0x1e, 0xef,
    0xee, 0xe9, 0xec, 0xed, 0xdf, 0x33, 0xad, 0xfe, 0x7e, 0x00, 0x1e, 0x03, 0x72, 0x85, 0xab, 0x4b,
    0xc8, 0x46, 0x00, 0x00, 0x00, 0x06, 0x62, 0x4b, 0x47, 0x44, 0x00, 0x0a, 0x00, 0x0a, 0x00, 0x0a,
    0x5e, 0xb1, 0xcf, 0x16, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0d, 0xd7,
    0x00, 0x00, 0x0d, 0xd7, 0x01, 0x42, 0x28, 0x9b, 0x78, 0x00, 0x00, 0x00, 0x07, 0x74, 0x49, 0x4d,
    0x45, 0x07, 0xe5, 0x06, 0x04, 0x0f, 0x17, 0x36, 0x4b, 0x1a, 0x14, 0x54, 0x00, 0x00, 0x14, 0xf8,
    0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0xed, 0x5d, 0x69, 0x60, 0x15, 0xd5, 0xd9, 0x7e, 0xce, 0xcc,
    0xdc, 0x7d, 0xc9, 0xbe, 0x90, 0x84, 0x40, 0x10, 0x12, 0xc0, 0x85, 0x8a, 0x40, 0xd8, 0x31, 0x05,
    0xaa, 0x04, 0x02, 0x6a, 0x49, 0xb4, 0xf5, 0x53, 0xdb, 0xaa, 0x54, 0xed, 0x87, 0xd6, 0xba, 0x80,
    0x75, 0x21, 0x5a, 0x37, 0xd2, 0xcd, 0xda, 0x4d, 0xd0, 0x8a, 0xa2, 0x75, 0x49, 0x22, 0x1a, 0x82,
    0x51, 0x0a, 0x02, 0x16, 0x65, 0x09, 0x82, 0x95, 0x82, 0x90, 0xa0, 0x65, 0xc9, 0xbe, 0xdd, 0xe4,
    0xee, 0x73, 0xef, 0x9d, 0x99, 0xd3, 0x1f, 0x2c, 0x15, 0xc9, 0xdc, 0x2d, 0x37, 0x24, 0xf7, 0x32,
    0xcf, 0x9f, 0x2c, 0xe7, 0xcc, 0x9c, 0x33, 0xcf, 0xfb, 0x9c, 0x73, 0xde, 0xf7, 0x2c, 0x33, 0x80,
    0x02, 0x05, 0x0a, 0x14, 0x28, 0x50, 0xa0, 0x40, 0x81, 0x02, 0x05, 0x0a, 0x14, 0x28, 0xb8, 0x80,
    0x40, 0xfa, 0xeb, 0xc6, 0xa5, 0x35, 0x37, 0x9a, 0x9d, 0x82, 0x67, 0x26, 0x0b, 0x32, 0x99, 0x52,
    0xe4, 0x81, 0x60, 0x18, 0x28, 0xe2, 0x40, 0xc0, 0x01, 0x10, 0x29, 0xd0, 0x09, 0xe0, 0x04, 0x08,
    0x3d, 0x44, 0x08, 0xb3, 0x4b, 0x47, 0xdd, 0xdb, 0x4b, 0x8b, 0xaa, 0x5d, 0x17, 0x02, 0xe9, 0xf7,
    0x6f, 0xbc, 0xc9, 0xc0, 0xf1, 0xfc, 0x0c, 0x89, 0xa1, 0xf9, 0xa0, 0x64, 0x0c, 0x01, 0x86, 0x01,
    0x48, 0x02, 0xc0, 0x82, 0x42, 0x00, 0x41, 0x0f, 0x08, 0x39, 0x0e, 0x09, 0x75, 0x00, 0xdd, 0x45,
    0xe1, 0xf9, 0x67, 0xd9, 0xa2, 0xf5, 0xf6, 0x41, 0x2f, 0x80, 0xd2, 0xf2, 0x62, 0xb5, 0x5b, 0x87,
    0xc5, 0x94, 0xe2, 0x26, 0x02, 0xcc, 0x01, 0xc0, 0x85, 0x70, 0xb9, 0x0b, 0x14, 0x1b, 0x01, 0xfc,
    0x4d, 0xf7, 0xf9, 0xc5, 0x1f, 0x94, 0x96, 0x96, 0x4a, 0xb1, 0x64, 0xf4, 0xe2, 0xf2, 0x62, 0x76,
    0x84, 0x8e, 0x14, 0x82, 0xd2, 0x9f, 0x00, 0xb8, 0x0a, 0x80, 0x2e, 0x84, 0xcb, 0x05, 0x00, 0x9b,
    0x08, 0xc8, 0x6b, 0x5a, 0x9e, 0xbe, 0x53, 0x5a, 0x52, 0xe1, 0x1d, 0x54, 0x02, 0x58, 0xb2, 0x6a,
    0x89, 0x2a, 0x3e, 0xa3, 0xe7, 0x4e, 0x22, 0xd1, 0x07, 0x40, 0x90, 0x15, 0x81, 0x5b, 0x1e, 0xa1,
    0x84, 0x3e, 0x5e, 0xb6, 0xa0, 0xf2, 0x0d, 0x10, 0xd0, 0x68, 0x36, 0x7c, 0x69, 0x69, 0x29, 0xe3,
    0x1a, 0x7f, 0xf0, 0x46, 0x02, 0x3c, 0x06, 0x60, 0x64, 0x5f, 0xef, 0x47, 0x81, 0x06, 0x50, 0x94,
    0xf5, 0xb4, 0x26, 0xac, 0x5a, 0xfd, 0xd3, 0xd5, 0xbe, 0x01, 0x17, 0xc0, 0xb2, 0xaa, 0x92, 0x99,
    0x20, 0xf4, 0xaf, 0x00, 0xc6, 0xf6, 0x03, 0x7f, 0x5b, 0x40, 0xf0, 0xe3, 0x95, 0x45, 0x15, 0x27,
    0xa2, 0xd1, 0xf8, 0xcb, 0xdf, 0xbd, 0x61, 0x38, 0x65, 0xc5, 0x35, 0x00, 0xae, 0x8c, 0xf4, 0xbd,
    0x29, 0x70, 0x80, 0xa5, 0xe4, 0x8e, 0x67, 0x16, 0x95, 0x7f, 0x3a, 0x20, 0x02, 0x28, 0x2e, 0x2f,
    0x66, 0x2f, 0xd2, 0x90, 0x47, 0x29, 0xa1, 0x8f, 0x00, 0x60, 0xfb, 0x91, 0x47, 0x1b, 0x01, 0xee,
    0x79, 0x76, 0x61, 0xc5, 0x2b, 0x51, 0x65, 0xfc, 0xaa, 0x92, 0x9f, 0x50, 0x42, 0x7f, 0x0f, 0xc0,
    0xdc, 0x8f, 0xc5, 0x88, 0x94, 0x92, 0xc7, 0xf5, 0x9f, 0x8f, 0x7d, 0x2a, 0xdc, 0x21, 0x33, 0x2c,
    0x01, 0x2c, 0xad, 0x99, 0xa7, 0xd1, 0x8b, 0xa6, 0xd7, 0x40, 0x69, 0xf1, 0xf9, 0x22, 0x94, 0x82,
    0xae, 0x2c, 0x5b, 0x58, 0xb9, 0x3c, 0x1a, 0x8c, 0xbf, 0x6c, 0x7d, 0xf1, 0xe3, 0x38, 0xd9, 0xe5,
    0x9f, 0x2f, 0xbc, 0xa7, 0x33, 0x19, 0x7e, 0x50, 0x5a, 0xf0, 0x0a, 0xdf, 0xef, 0x02, 0x28, 0xdd,
    0xfa, 0x23, 0xad, 0xdb, 0xee, 0xdc, 0x00, 0x60, 0xf6, 0x00, 0x04, 0x2d, 0xcf, 0xad, 0x2c, 0x2a,
    0xff, 0xc5, 0xa0, 0xf5, 0x0b, 0x28, 0xc8, 0xf2, 0xea, 0xc5, 0xcf, 0x51, 0x90, 0xbb, 0x07, 0xa0,
    0xf4, 0x4d, 0x2e, 0xce, 0x51, 0xf4, 0xc7, 0xc2, 0x0f, 0x3c, 0xfd, 0x26, 0x80, 0xe2, 0xf2, 0x62,
    0x76, 0x84, 0x16, 0xe5, 0x00, 0xae, 0x0b, 0x94, 0x57, 0xcd, 0x6a, 0x90, 0x9b, 0xfa, 0x1d, 0x0c,
    0x4f, 0x1c, 0x0d, 0xb3, 0x36, 0x01, 0x46, 0x4d, 0x1c, 0x3c, 0x02, 0x0f, 0x1b, 0x6f, 0x41, 0xab,
    0xed, 0x04, 0x0e, 0xb5, 0xed, 0x45, 0x8f, 0xbb, 0x33, 0xa6, 0x7a, 0x82, 0x65, 0xeb, 0x17, 0xff,
    0x1a, 0x20, 0xf7, 0x87, 0x7a, 0x5d, 0xbc, 0x2e, 0x19, 0x63, 0xd3, 0x27, 0x20, 0xcd, 0x34, 0x14,
    0x66, 0x6d, 0x22, 0x34, 0x9c, 0x16, 0x0e, 0x8f, 0x15, 0x36, 0xbe, 0x1b, 0xc7, 0x2c, 0x87, 0x51,
    0xdf, 0xfe, 0x2f, 0x78, 0xc5, 0x20, 0xec, 0x4a, 0x48, 0x85, 0x6e, 0xef, 0xd8, 0x1b, 0x42, 0x19,
    0x0e, 0x42, 0x12, 0xc0, 0xf2, 0xaa, 0x92, 0x52, 0x4a, 0xe8, 0x0a, 0x7f, 0x79, 0x12, 0x74, 0x29,
    0x98, 0x93, 0x57, 0x82, 0x71, 0x99, 0x53, 0xc1, 0x31, 0x2a, 0xbf, 0xf7, 0x3b, 0x6e, 0xa9, 0xc3,
    0xa6, 0xba, 0x72, 0x7c, 0xd5, 0xf9, 0xef, 0x10, 0xfd, 0x1f, 0x7a, 0xed, 0xca, 0x85, 0x95, 0x55,
    0x83, 0xc9, 0xf8, 0x0f, 0xac, 0x5f, 0xbc, 0x80, 0x01, 0x59, 0x1f, 0x0a, 0xa7, 0xa3, 0x52, 0x2e,
    0xc3, 0xdc, 0xbc, 0x12, 0x64, 0x27, 0xe4, 0xfa, 0xcd, 0xe7, 0x13, 0xbd, 0xd8, 0xdf, 0xbc, 0x03,
    0x9b, 0xea, 0xca, 0x03, 0x36, 0x1a, 0x0a, 0x3c, 0x52, 0xb6, 0xb0, 0xe2, 0xa9, 0x88, 0x0b, 0xe0,
    0x94, 0xb7, 0xbf, 0x45, 0xce, 0xe1, 0x23, 0x20, 0x28, 0x18, 0x75, 0x2d, 0xbe, 0x9b, 0xfb, 0xfd,
    0x80, 0x86, 0xff, 0x36, 0x0e, 0xb5, 0xed, 0x45, 0xc5, 0xe7, 0x7f, 0x86, 0xcb, 0xe7, 0x08, 0xf6,
    0x92, 0x2e, 0x4e, 0x62, 0x2f, 0x7f, 0xea, 0x9a, 0xb7, 0x1a, 0x06, 0x83, 0xf1, 0xef, 0x7b, 0xff,
    0xba, 0x61, 0x9c, 0xc8, 0x7e, 0x0e, 0x20, 0x21, 0x98, 0xfc, 0x7a, 0xb5, 0x09, 0xd7, 0x5f, 0xfe,
    0xff, 0xc8, 0x4b, 0xbd, 0x3c, 0xa4, 0x72, 0x04, 0xc9, 0x87, 0xcd, 0x75, 0x15, 0xf8, 0xf8, 0xab,
    0x2a, 0x50, 0xf9, 0x51, 0x50, 0x84, 0x44, 0x67, 0xae, 0xbc, 0xa6, 0x72, 0x47, 0x30, 0xf7, 0x64,
    0x82, 0x9d, 0xe0, 0x39, 0x15, 0xea, 0xf5, 0x6a, 0x7c, 0x15, 0xab, 0xc6, 0x0f, 0xae, 0xf8, 0x39,
    0xbe, 0x37, 0xfa, 0x86, 0x90, 0x8d, 0x0f, 0x00, 0x63, 0xd2, 0xae, 0xc0, 0xcf, 0x66, 0x3c, 0x8d,
    0x14, 0x63, 0x66, 0xb0, 0x97, 0x24, 0x89, 0x8c, 0xf8, 0x97, 0xc1, 0xd2, 0xfa, 0x39, 0x91, 0xfd,
    0x6b, 0xb0, 0xc6, 0x4f, 0x35, 0x65, 0xe1, 0x67, 0xd3, 0x9f, 0x0e, 0xd9, 0xf8, 0x00, 0xc0, 0x31,
    0x2a, 0x5c, 0x3d, 0xe6, 0x87, 0xb8, 0x61, 0xfc, 0xdd, 0xfe, 0x78, 0x66, 0xc1, 0x92, 0x17, 0x4a,
    0xb7, 0x5e, 0xc9, 0x45, 0x4c, 0x00, 0x2e, 0x1d, 0xb9, 0x43, 0x2e, 0xce, 0x27, 0x20, 0x58, 0x3c,
    0xee, 0x4e, 0x5c, 0x96, 0x31, 0xa5, 0x4f, 0x24, 0x26, 0x19, 0xd2, 0x71, 0xfb, 0x94, 0xc7, 0x60,
    0xd6, 0x26, 0x06, 0x3b, 0x0e, 0x2c, 0x78, 0x70, 0xfd, 0xf5, 0x93, 0x07, 0xda, 0xf8, 0x0f, 0x55,
    0x95, 0x4c, 0x03, 0x30, 0x2f, 0x98, 0xbc, 0x26, 0x6d, 0x02, 0x6e, 0x9d, 0xfc, 0x30, 0x92, 0x0c,
    0x69, 0x7d, 0x2a, 0x73, 0x5c, 0xe6, 0x34, 0x5c, 0x3f, 0x7e, 0x29, 0x88, 0x5c, 0x07, 0x4e, 0x71,
    0xa9, 0xcb, 0x9e, 0xbc, 0x24, 0x22, 0x02, 0x58, 0xb2, 0x6a, 0x89, 0x8a, 0x50, 0x7a, 0x9f, 0x5c,
    0x7a, 0xc1, 0xa8, 0x6b, 0x31, 0x2e, 0x73, 0x5a, 0x44, 0xc8, 0x34, 0x6b, 0x13, 0xf0, 0x7f, 0x13,
    0xee, 0x03, 0xcb, 0x04, 0x37, 0xad, 0x40, 0x88, 0xf4, 0xab, 0x81, 0x16, 0x80, 0xc4, 0xd0, 0xa0,
    0xea, 0xc0, 0x32, 0x1c, 0x6e, 0x99, 0xf8, 0x20, 0xe2, 0xb4, 0x49, 0x11, 0x29, 0xf7, 0xd2, 0x21,
    0x93, 0x31, 0x73, 0xe4, 0x42, 0x7f, 0xec, 0x2c, 0x2f, 0x2d, 0x2f, 0x56, 0xf7, 0x59, 0x00, 0x09,
    0x19, 0xdd, 0xc5, 0x00, 0xb2, 0xe5, 0x1c, 0xbe, 0xef, 0xe6, 0x7e, 0x3f, 0xa2, 0x84, 0x66, 0x27,
    0x8c, 0xc2, 0xc4, 0xec, 0x20, 0x23, 0x4c, 0x8a, 0x39, 0x0f, 0x56, 0x17, 0x4f, 0x1a, 0xb0, 0xd6,
    0x5f, 0x5d, 0x3c, 0x05, 0x14, 0x05, 0xc1, 0xe4, 0xcd, 0x1f, 0x36, 0x17, 0x59, 0xf1, 0x17, 0x45,
    0xb4, 0xfc, 0xb9, 0x79, 0x25, 0x88, 0xd7, 0x25, 0xcb, 0x39, 0x77, 0x43, 0x79, 0x1d, 0xae, 0x0d,
    0x38, 0xac, 0x04, 0xe4, 0xf8, 0xe4, 0xc2, 0x4e, 0xaf, 0x98, 0x93, 0x57, 0x12, 0x70, 0xcc, 0x3f,
    0x5c, 0x57, 0x8f, 0x0f, 0x37, 0x6d, 0x46, 0x73, 0x53, 0x33, 0xe2, 0xe2, 0xcc, 0xc8, 0x9f, 0x34,
    0x11, 0x05, 0x57, 0xce, 0x02, 0xcb, 0xc8, 0x6b, 0x6f, 0x76, 0xee, 0x62, 0xec, 0x6b, 0xf8, 0x38,
    0xa8, 0xd0, 0x87, 0x48, 0xf4, 0x66, 0x00, 0xb5, 0xbd, 0xfa, 0x2e, 0xd5, 0x45, 0x7a, 0x37, 0xb4,
    0xe3, 0x89, 0x44, 0x72, 0x29, 0x91, 0x2e, 0x02, 0x30, 0x12, 0x20, 0x19, 0x00, 0xf4, 0x00, 0xe2,
    0x4f, 0xfd, 0x04, 0x00, 0x17, 0x80, 0x6e, 0x00, 0x6e, 0x0a, 0x34, 0x11, 0xd0, 0xaf, 0x29, 0xc1,
    0x57, 0x04, 0xe4, 0x88, 0x0e, 0xfc, 0x3e, 0xb9, 0x55, 0x4a, 0x51, 0xa2, 0x37, 0x13, 0x12, 0xd8,
    0x8f, 0x56, 0x73, 0x5a, 0x7c, 0x77, 0x94, 0xff, 0xc8, 0x59, 0x94, 0x24, 0x6c, 0xd9, 0xba, 0x0d,
    0xb5, 0x7b, 0x3e, 0x83, 0xd5, 0x6a, 0x43, 0x66, 0x66, 0x06, 0xae, 0xfe, 0xde, 0x1c, 0xe4, 0xe5,
    0xe6, 0xfa, 0xf5, 0x09, 0xe6, 0xe4, 0x15, 0xa3, 0xf2, 0x5f, 0x7f, 0xed, 0xbd, 0x77, 0xa2, 0xb8,
    0x19, 0xc0, 0xdb, 0x61, 0x47, 0x01, 0xa5, 0x35, 0x37, 0x9a, 0xdd, 0x82, 0xb7, 0xab, 0x37, 0xa1,
    0xa8, 0x39, 0x2d, 0x1e, 0xbb, 0xea, 0x6f, 0xb2, 0x02, 0x10, 0x04, 0x01, 0x4f, 0x3e, 0x53, 0x86,
    0x8a, 0x75, 0xef, 0x82, 0xd2, 0xb3, 0x3d, 0xd6, 0xd1, 0x79, 0xb9, 0x78, 0xfe, 0x77, 0xbf, 0x41,
    0x56, 0x66, 0x86, 0x6c, 0xd9, 0x6f, 0xec, 0xfd, 0x3d, 0xf6, 0x37, 0xef, 0x0c, 0xa6, 0x21, 0x34,
    0xaf, 0x2c, 0xaa, 0xc8, 0x02, 0x01, 0x5d, 0x5a, 0x33, 0x4f, 0xa3, 0x15, 0x0c, 0x73, 0x19, 0x30,
    0xb3, 0x01, 0x3a, 0x15, 0x04, 0x97, 0x83, 0x42, 0xd5, 0xa7, 0x66, 0x46, 0xe0, 0x03, 0xb0, 0x0f,
    0x94, 0xee, 0x00, 0x25, 0x1f, 0xb9, 0x54, 0x8e, 0xcd, 0x7f, 0x2c, 0xfc, 0xc0, 0x03, 0x0a, 0xb2,
    0xac, 0xba, 0xb8, 0x19, 0x40, 0x7a, 0x30, 0x63, 0xf6, 0x0f, 0xc6, 0xdf, 0x23, 0x9b, 0xde, 0xd0,
    0xd8, 0x84, 0xbb, 0x7f, 0x71, 0x3f, 0xea, 0xea, 0x8f, 0x7c, 0x6b, 0x88, 0x23, 0xb8, 0xbe, 0xf8,
    0xfb, 0xf8, 0xe5, 0x83, 0xf7, 0x83, 0xe3, 0x38, 0xd9, 0x10, 0xf1, 0x57, 0x1b, 0x6f, 0xed, 0xbd,
    0xb1, 0x10, 0xf8, 0xa8, 0xe4, 0x49, 0xf2, 0xb7, 0x94, 0xec, 0xb7, 0x07, 0x70, 0x0a, 0x9e, 0x99,
    0x0c, 0x48, 0xaf, 0x79, 0xf2, 0x52, 0xbe, 0xe3, 0xb7, 0xf5, 0x3f, 0xf9, 0x4c, 0x19, 0xca, 0xdf,
    0x59, 0x27, 0xdb, 0x2b, 0xdc, 0x7a, 0xc7, 0x5d, 0xa8, 0x7c, 0xf3, 0x75, 0x98, 0x8c, 0xc6, 0x5e,
    0xf3, 0x8c, 0x4d, 0x9f, 0x18, 0xac, 0x00, 0x32, 0x96, 0xad, 0x2f, 0xbe, 0x9b, 0x54, 0x91, 0x89,
    0x54, 0xa0, 0x45, 0x00, 0xcc, 0x38, 0x1d, 0x22, 0x45, 0x62, 0xbe, 0xf0, 0xa4, 0x80, 0xf2, 0x01,
    0x92, 0x0f, 0x82, 0x7b, 0xf5, 0xa2, 0xd1, 0xba, 0xbc, 0xaa, 0x64, 0x03, 0x5d, 0x4f, 0xf7, 0x80,
    0x04, 0x36, 0x3e, 0x00, 0x5c, 0x9c, 0x3e, 0x51, 0x36, 0xcd, 0x66, 0xb3, 0xe1, 0xd6, 0x9f, 0xde,
    0x89, 0xc6, 0xa6, 0xe6, 0x5e, 0x7a, 0x5f, 0x8a, 0xb7, 0xca, 0x2b, 0x41, 0x29, 0xc5, 0x8a, 0x87,
    0x1f, 0x82, 0x5c, 0x04, 0x36, 0x2a, 0x65, 0x1c, 0x0e, 0xb6, 0xd6, 0xf6, 0x5a, 0x77, 0x96, 0xd1,
    0xce, 0x00, 0x50, 0x13, 0x96, 0x0f, 0xc0, 0x50, 0x26, 0x5f, 0x2e, 0x6d, 0x58, 0x62, 0x9e, 0xdf,
    0x6e, 0xbf, 0x62, 0xdd, 0xbb, 0x7e, 0x49, 0x69, 0x68, 0x68, 0xc4, 0x2b, 0x6b, 0x5f, 0x97, 0x4d,
    0xcf, 0x49, 0x1c, 0x13, 0x4a, 0x2b, 0x7d, 0x8e, 0x12, 0x7a, 0x23, 0xfa, 0x77, 0xe1, 0xe5, 0x34,
    0xa9, 0x71, 0x94, 0xd0, 0x1b, 0x41, 0xf0, 0x5c, 0xb0, 0x97, 0xf8, 0xe3, 0xea, 0xe5, 0xb5, 0xaf,
    0xf7, 0x6a, 0xfc, 0x6f, 0xa2, 0xbc, 0x72, 0x1d, 0xea, 0xea, 0xeb, 0x65, 0xd3, 0x87, 0x27, 0x8d,
    0x96, 0x77, 0x52, 0x29, 0xcd, 0xef, 0x83, 0x13, 0x48, 0x47, 0xcb, 0x7b, 0xec, 0xf2, 0xe1, 0xda,
    0xc6, 0x4d, 0x9b, 0xcf, 0xe9, 0xf6, 0xe5, 0xf2, 0xc9, 0x87, 0x4c, 0xf1, 0x08, 0x66, 0x7c, 0x1d,
    0xec, 0x20, 0x84, 0xc0, 0xa4, 0x89, 0x97, 0x4d, 0xdf, 0xb4, 0xf9, 0xa3, 0xc0, 0x9a, 0xa3, 0x14,
    0xff, 0xd8, 0xbc, 0x45, 0x3e, 0x7a, 0xd2, 0x24, 0xf8, 0x0b, 0x97, 0xf3, 0xc2, 0x17, 0x00, 0xc1,
    0x30, 0xb9, 0x24, 0xa3, 0x46, 0xbe, 0xb1, 0x35, 0xb7, 0xb4, 0x06, 0x45, 0x8e, 0x3f, 0xe5, 0x33,
    0x84, 0x85, 0x5e, 0x65, 0x8a, 0x7a, 0x01, 0xe8, 0x55, 0x26, 0x30, 0x44, 0x3e, 0xac, 0x6d, 0x6a,
    0x6e, 0xe9, 0x33, 0x57, 0x26, 0x6d, 0xbc, 0x3f, 0x27, 0x2f, 0xa7, 0x2f, 0x61, 0xa0, 0xac, 0x95,
    0x79, 0x9f, 0x5b, 0xfe, 0x22, 0x93, 0x31, 0xb8, 0xb8, 0xdf, 0x6c, 0xf2, 0xa3, 0x5c, 0x0a, 0x8f,
    0xe0, 0x8e, 0x7a, 0x01, 0x78, 0x04, 0xb7, 0xbf, 0x69, 0x5b, 0x98, 0x22, 0xc0, 0x95, 0xdb, 0xe7,
    0x77, 0x2b, 0xa5, 0x29, 0x6c, 0x01, 0x10, 0x40, 0x2b, 0x97, 0x66, 0xf7, 0x74, 0xcb, 0xc7, 0xbc,
    0x93, 0x26, 0x06, 0xf5, 0x50, 0x93, 0x27, 0xc9, 0x87, 0xf0, 0x2e, 0xaf, 0x1d, 0x82, 0xe4, 0x8b,
    0x7a, 0x01, 0x08, 0x92, 0x0f, 0x6e, 0xaf, 0xc3, 0x0f, 0x07, 0xc1, 0x71, 0x35, 0x25, 0x5f, 0x9e,
    0x2b, 0x1b, 0x6f, 0xf1, 0x77, 0xa9, 0x2e, 0x6c, 0x01, 0x50, 0x40, 0xb6, 0xe6, 0x6d, 0x76, 0xf9,
    0x75, 0x98, 0x82, 0x2b, 0x67, 0x61, 0x74, 0x9e, 0xff, 0x15, 0x2e, 0xb5, 0x5a, 0x8d, 0xdb, 0x7f,
    0xf2, 0x23, 0xd9, 0xf4, 0x56, 0x5b, 0x03, 0x62, 0x05, 0xad, 0x7e, 0xb8, 0xba, 0xed, 0xc7, 0x3f,
    0x82, 0x4a, 0xe5, 0x3f, 0x52, 0x1d, 0x3b, 0x66, 0x34, 0x66, 0xcd, 0x9c, 0x21, 0x9b, 0xde, 0x6e,
    0x6f, 0xf4, 0xd7, 0x8a, 0xed, 0x7d, 0xf1, 0x01, 0xac, 0x72, 0x49, 0x07, 0x5a, 0x6a, 0x65, 0xbb,
    0x36, 0x96, 0x61, 0xf0, 0xfc, 0xef, 0x7e, 0x83, 0xa1, 0x43, 0xb3, 0x64, 0x8d, 0xff, 0xd4, 0xe3,
    0x2b, 0x30, 0x6a, 0xa4, 0xfc, 0xcc, 0xd8, 0x97, 0x6d, 0x7b, 0x62, 0x46, 0x00, 0x87, 0x5a, 0x3f,
    0x93, 0x4d, 0xcb, 0xcb, 0x1d, 0x85, 0xa7, 0x9e, 0x58, 0x21, 0x2b, 0x82, 0xec, 0xa1, 0x43, 0xf1,
    0x87, 0xdf, 0x96, 0xc9, 0x4e, 0x9c, 0x51, 0x50, 0x7c, 0xe9, 0xe7, 0xfe, 0x94, 0xc2, 0x16, 0x7e,
    0x0f, 0x40, 0xe9, 0x71, 0x7f, 0xdd, 0x4e, 0x43, 0xf7, 0x11, 0xd9, 0x6b, 0xb3, 0x32, 0x33, 0x50,
    0xf9, 0xe6, 0xeb, 0xb8, 0x73, 0xc9, 0x6d, 0x18, 0x91, 0x33, 0x1c, 0x2a, 0x95, 0x0a, 0x29, 0xc9,
    0xc9, 0x58, 0x50, 0x38, 0x0f, 0x15, 0x6f, 0xbc, 0x86, 0xf9, 0xf3, 0xae, 0xf2, 0x33, 0x2b, 0x26,
    0xe2, 0x60, 0x4b, 0x6d, 0xcc, 0x08, 0xe0, 0x40, 0xeb, 0x6e, 0x48, 0x54, 0x94, 0x4d, 0x5f, 0x30,
    0xef, 0x6a, 0x94, 0xff, 0x7d, 0x2d, 0x16, 0xcc, 0xbb, 0x1a, 0x29, 0xc9, 0xc9, 0x50, 0xa9, 0x54,
    0x18, 0x91, 0x33, 0x1c, 0x77, 0xfd, 0xf4, 0x76, 0x54, 0xbe, 0xf5, 0x3a, 0x32, 0x33, 0xe4, 0x27,
    0xcc, 0x4e, 0x58, 0xea, 0x61, 0xe5, 0xbb, 0xfc, 0xb4, 0x61, 0x7a, 0xd4, 0x5f, 0xdd, 0x38, 0xff,
    0x1d, 0x00, 0x73, 0xc0, 0xdf, 0x6c, 0xca, 0x87, 0x87, 0xde, 0xc4, 0x92, 0xa9, 0xf2, 0xfb, 0x43,
    0x4c, 0x46, 0x23, 0x96, 0xde, 0x75, 0x07, 0x96, 0xde, 0x75, 0x47, 0x48, 0x84, 0xd5, 0x9e, 0xd8,
    0x1c, 0xd6, 0x6e, 0xa1, 0xc1, 0x8a, 0x6e, 0x57, 0x07, 0xf6, 0x9c, 0xd8, 0x82, 0xfc, 0x61, 0x73,
    0xfd, 0xf6, 0x04, 0x65, 0xcf, 0x3c, 0x19, 0xf2, 0xbd, 0xff, 0x51, 0xf7, 0x36, 0x02, 0xdb, 0x30,
    0x6c, 0x27, 0x50, 0xfa, 0xcc, 0x5f, 0xfa, 0x7f, 0xba, 0x0e, 0xe2, 0x50, 0xdb, 0xde, 0x88, 0x92,
    0xe5, 0xf2, 0x39, 0xf0, 0x51, 0x7d, 0x25, 0x62, 0x0d, 0x9b, 0xeb, 0x2b, 0xe1, 0xf6, 0x39, 0x23,
    0x7a, 0xcf, 0x2f, 0x5b, 0xf7, 0xe0, 0xeb, 0xce, 0x03, 0xfe, 0x33, 0x49, 0xd8, 0x1b, 0xb6, 0x00,
    0x9c, 0x9c, 0xf3, 0x63, 0xf8, 0x71, 0x04, 0x01, 0xa0, 0xe2, 0xf3, 0x3f, 0xa3, 0xd3, 0xd9, 0x12,
    0x91, 0x07, 0x92, 0xa8, 0x84, 0xb7, 0xf7, 0xfd, 0x11, 0x0e, 0x8f, 0x35, 0xe6, 0x04, 0x60, 0xe7,
    0xbb, 0xf1, 0xc6, 0xde, 0xdf, 0xfb, 0x1d, 0x0a, 0x42, 0x81, 0xc5, 0xd5, 0x8e, 0x77, 0xbe, 0x78,
    0x21, 0x50, 0x36, 0x87, 0x53, 0x6d, 0xdf, 0x1e, 0xb6, 0x00, 0x4e, 0x2d, 0x7a, 0x6c, 0x0a, 0xd4,
    0x62, 0x5f, 0xad, 0x2d, 0xf3, 0x3b, 0x0e, 0x05, 0x67, 0x7c, 0x11, 0xef, 0xee, 0x5f, 0x8d, 0xba,
    0xf6, 0xcf, 0x11, 0xab, 0x38, 0xd2, 0xb1, 0x1f, 0xef, 0xed, 0x7f, 0xa9, 0xcf, 0x22, 0xb0, 0xba,
    0xbb, 0xb0, 0x66, 0xf7, 0xd3, 0x70, 0x7a, 0x03, 0x1c, 0x17, 0x24, 0xe4, 0x83, 0x40, 0xbb, 0x84,
    0x03, 0xee, 0x07, 0x20, 0xc0, 0xea, 0x40, 0x79, 0x3a, 0x1c, 0x4d, 0x78, 0xfe, 0xe3, 0x65, 0x38,
    0xda, 0x75, 0x28, 0xec, 0x6e, 0xff, 0xe5, 0xdd, 0x4f, 0x63, 0xcf, 0x89, 0x2d, 0x88, 0x75, 0xd4,
    0x9e, 0xf8, 0x08, 0x6b, 0x76, 0x3f, 0x13, 0xf6, 0x70, 0x70, 0xa2, 0xbb, 0x1e, 0x7f, 0xda, 0xfe,
    0x10, 0x3a, 0x1c, 0xcd, 0x01, 0xf3, 0x12, 0x31, 0xb0, 0xed, 0x02, 0x4f, 0xb6, 0x53, 0x90, 0x65,
    0xeb, 0x8b, 0x0f, 0x83, 0x20, 0x37, 0x50, 0x56, 0x86, 0xb0, 0x98, 0x90, 0x5d, 0x80, 0xb9, 0xb9,
    0xc5, 0x30, 0x69, 0x03, 0x6f, 0x91, 0x13, 0x25, 0x11, 0x7b, 0x1b, 0xb6, 0x62, 0x53, 0x5d, 0x39,
    0xec, 0x9e, 0x1e, 0x5c, 0x48, 0x30, 0xa8, 0x4d, 0x98, 0x79, 0xd1, 0x42, 0x4c, 0x1f, 0x31, 0x1f,
    0x2c, 0x13, 0x78, 0xfb, 0x9e, 0xd3, 0x6b, 0xc7, 0x96, 0x23, 0xef, 0x60, 0xe7, 0xd1, 0x8d, 0xc1,
    0xf6, 0x20, 0x87, 0x57, 0x16, 0x55, 0x8c, 0x0d, 0x74, 0x86, 0x22, 0xa8, 0xd5, 0x96, 0xe5, 0xd5,
    0xc5, 0xd7, 0x53, 0x8a, 0xb7, 0x82, 0x7d, 0x38, 0x35, 0xab, 0xc1, 0xe8, 0xb4, 0xf1, 0x18, 0x9b,
    0x3e, 0x11, 0x39, 0x89, 0x63, 0x60, 0xd4, 0xc4, 0x9f, 0xd9, 0xe6, 0xe5, 0xf4, 0xda, 0xd0, 0x6a,
    0x6b, 0xc0, 0xa1, 0xb6, 0xcf, 0x70, 0xa0, 0x65, 0x77, 0x4c, 0x79, 0xfb, 0xe1, 0x20, 0x41, 0x97,
    0x82, 0x8b, 0x87, 0x4c, 0x3a, 0x73, 0x2e, 0xc0, 0xa0, 0x36, 0x9f, 0x69, 0x1c, 0x76, 0xcf, 0xc9,
    0x73, 0x01, 0x07, 0x5b, 0xf7, 0xe0, 0x70, 0xdb, 0x3e, 0xf8, 0xc4, 0xe0, 0xcf, 0x7c, 0x10, 0x90,
    0xc5, 0xcf, 0x2e, 0x2c, 0x7f, 0xa7, 0xef, 0x3d, 0xc0, 0xe9, 0x5e, 0x60, 0x43, 0xf1, 0x27, 0xa0,
    0x98, 0x1a, 0xce, 0x43, 0x12, 0x10, 0xe8, 0xd5, 0x46, 0x78, 0x04, 0x3e, 0x26, 0xa6, 0x77, 0xfb,
    0x13, 0x1c, 0xa3, 0x82, 0x86, 0xd3, 0xc2, 0xe5, 0x75, 0xf8, 0x5d, 0x43, 0xf0, 0xcb, 0x37, 0xc5,
    0xf6, 0x67, 0x17, 0x55, 0xcc, 0x0c, 0x26, 0x2f, 0x13, 0xa4, 0x05, 0x29, 0x91, 0x70, 0x4b, 0xa0,
    0x88, 0x40, 0x5e, 0x3f, 0x14, 0xce, 0x18, 0x99, 0xdb, 0xef, 0x6f, 0x08, 0x92, 0x0f, 0x4e, 0xaf,
    0x3d, 0x6c, 0xe3, 0x03, 0x70, 0x4a, 0xc0, 0xed, 0xc1, 0x66, 0x66, 0x82, 0xcd, 0xf8, 0xec, 0xa2,
    0x8a, 0xaf, 0x08, 0xa1, 0xf7, 0x2a, 0x26, 0x1a, 0xdc, 0xa0, 0x84, 0x2e, 0x2d, 0x5b, 0x54, 0x51,
    0x17, 0x71, 0x01, 0x00, 0xc0, 0xb3, 0x45, 0x95, 0x2f, 0x51, 0x90, 0xdf, 0x2a, 0x34, 0x0f, 0x52,
    0xe3, 0x83, 0xae, 0x2c, 0x2b, 0xaa, 0x5c, 0x13, 0xca, 0x35, 0x4c, 0xa8, 0x85, 0xe8, 0xf7, 0x8d,
    0x7d, 0x10, 0xc0, 0x2b, 0x0a, 0xdd, 0x83, 0x0c, 0x84, 0xbe, 0x5c, 0x56, 0x54, 0xf9, 0x50, 0xa8,
    0x97, 0x85, 0xfc, 0x62, 0x87, 0x6d, 0xdb, 0xb6, 0xd1, 0x4f, 0xdf, 0xf8, 0x72, 0xfd, 0x8c, 0xba,
    0x4b, 0x00, 0x12, 0xf9, 0x37, 0x5f, 0x28, 0x08, 0xc7, 0xc9, 0xa6, 0xcf, 0xeb, 0xf6, 0x5d, 0xf2,
    0xb3, 0x6d, 0x05, 0xdb, 0x42, 0x76, 0x1c, 0x98, 0x70, 0x4b, 0xcc, 0x49, 0x1b, 0x93, 0x10, 0x03,
    0x5b, 0xf6, 0xa2, 0xdf, 0xf8, 0x04, 0x18, 0x91, 0x36, 0x56, 0x0c, 0xf7, 0x0d, 0x21, 0x61, 0x09,
    0xe0, 0xc5, 0x3d, 0x8f, 0x3f, 0x70, 0xac, 0xa3, 0xee, 0x6e, 0x4a, 0x15, 0x03, 0x0c, 0xf8, 0xb8,
    0x4f, 0x81, 0xa3, 0xed, 0x75, 0xf7, 0xbe, 0xb4, 0xeb, 0x89, 0xfb, 0xc3, 0x6b, 0xcb, 0xa1, 0x1a,
    0x7f, 0xe7, 0x8a, 0xeb, 0x8e, 0x59, 0x8e, 0x54, 0x0a, 0xa2, 0x4f, 0x69, 0xff, 0x83, 0x08, 0x2c,
    0xcb, 0xd1, 0x9c, 0xe4, 0x11, 0x37, 0xde, 0x9e, 0xff, 0xd4, 0x9b, 0xfd, 0x26, 0x80, 0x17, 0x76,
    0x3f, 0x3a, 0xa5, 0xb9, 0xeb, 0xd8, 0x76, 0x8f, 0xc0, 0xb3, 0x0a, 0xe5, 0x83, 0x0f, 0x1a, 0x4e,
    0x27, 0x65, 0x27, 0x8c, 0x9c, 0x79, 0xdb, 0x94, 0xc7, 0x82, 0x7e, 0x73, 0x58, 0xd0, 0x43, 0x40,
    0xd5, 0x7f, 0x5e, 0x4c, 0xeb, 0xb2, 0xb5, 0x6c, 0x51, 0x8c, 0x3f, 0x78, 0xe1, 0x11, 0xdc, 0x4c,
    0xab, 0xfd, 0xc4, 0xe6, 0xaa, 0x7f, 0xbf, 0x98, 0x16, 0x71, 0x01, 0x7c, 0xdd, 0x70, 0x70, 0x87,
    0xcd, 0x6d, 0xd5, 0x2a, 0x34, 0x0f, 0x6e, 0xd8, 0x79, 0xab, 0xf6, 0x6b, 0xcb, 0xa1, 0x9d, 0x11,
    0x15, 0xc0, 0x9f, 0x77, 0x2c, 0x7f, 0xb5, 0xcd, 0xda, 0x34, 0x42, 0xa1, 0x37, 0x3a, 0xd0, 0x66,
    0x6d, 0xc8, 0xf9, 0xd3, 0xa7, 0x0f, 0xbd, 0x1c, 0x11, 0x01, 0xac, 0xda, 0xf1, 0xc8, 0x35, 0x4d,
    0x96, 0x63, 0x37, 0x2b, 0xb4, 0x46, 0x17, 0x9a, 0xbb, 0x8f, 0xfe, 0x78, 0xf5, 0xce, 0x47, 0x16,
    0xf6, 0x49, 0x00, 0xab, 0x3e, 0x5b, 0xa5, 0xef, 0x74, 0xb6, 0xff, 0x5d, 0x94, 0x44, 0x85, 0xd1,
    0x28, 0x83, 0x28, 0x89, 0xe8, 0x70, 0xb4, 0xbf, 0xb5, 0xf6, 0x8b, 0xb5, 0x86, 0xb0, 0x05, 0x20,
    0x78, 0x8f, 0xaf, 0xb7, 0xb9, 0xbb, 0xf5, 0x0a, 0x9d, 0xd1, 0x09, 0x9b, 0xbb, 0x5b, 0xe7, 0x70,
    0x1e, 0x7e, 0x27, 0x2c, 0x01, 0xbc, 0x54, 0xfb, 0xe4, 0xcc, 0x26, 0xcb, 0xd1, 0xd9, 0x0a, 0x8d,
    0xd1, 0x8d, 0x06, 0xcb, 0xd1, 0xab, 0x56, 0xd7, 0x3e, 0x31, 0x2b, 0x64, 0x01, 0x74, 0xd8, 0x9b,
    0x2a, 0x45, 0x49, 0x50, 0x18, 0x8c, 0x72, 0x48, 0x92, 0x80, 0x2e, 0x5b, 0xf3, 0xdb, 0x21, 0x09,
    0xe0, 0x85, 0x4f, 0x1f, 0x59, 0xd6, 0xe3, 0xec, 0x4c, 0x51, 0xe8, 0x8b, 0x0d, 0xf4, 0xb8, 0xba,
    0xd2, 0x56, 0xef, 0x7e, 0x6c, 0x59, 0x50, 0x02, 0xd8, 0x4a, 0xb7, 0x72, 0x16, 0x77, 0xc7, 0x0a,
    0x85, 0xb6, 0xd8, 0x42, 0x87, 0xbd, 0x75, 0x45, 0x29, 0x2d, 0xe5, 0x02, 0x0a, 0xa0, 0x7e, 0xf7,
    0xd6, 0x67, 0xad, 0x2e, 0x8b, 0x4e, 0xa1, 0x2c, 0xc6, 0x1c, 0x42, 0x57, 0xb7, 0x2e, 0xa3, 0x16,
    0x4f, 0xfa, 0x17, 0x00, 0x05, 0x69, 0xb7, 0x36, 0xdd, 0xa9, 0xd0, 0x15, 0x9b, 0x68, 0xb3, 0x36,
    0x2e, 0x05, 0x3d, 0x7b, 0xfd, 0xe7, 0x2c, 0x01, 0xac, 0xda, 0xf1, 0xc8, 0x2f, 0x1c, 0x1e, 0x9b,
    0x12, 0xf6, 0xc5, 0x28, 0x1c, 0xbc, 0x55, 0xff, 0x97, 0xdd, 0x0f, 0xdf, 0x23, 0x2b, 0x00, 0xbb,
    0xd7, 0x76, 0x9f, 0x42, 0x53, 0x6c, 0xc3, 0xc9, 0xdb, 0x1e, 0xe8, 0x55, 0x00, 0xaf, 0xee, 0xfb,
    0xf5, 0x94, 0x2e, 0x47, 0xeb, 0x10, 0x85, 0xa2, 0xd8, 0x86, 0xc5, 0xde, 0x96, 0xb1, 0xf6, 0xb3,
    0xa7, 0xf3, 0xcf, 0x11, 0x40, 0x8f, 0xab, 0xfd, 0x71, 0x49, 0xd9, 0xe2, 0x13, 0xfb, 0xf3, 0x02,
    0x94, 0xa2, 0x87, 0xb7, 0xac, 0x38, 0x57, 0x00, 0xce, 0xae, 0x19, 0x0a, 0x3d, 0x17, 0xc8, 0xbc,
    0x80, 0xb3, 0xeb, 0xca, 0xb3, 0x04, 0xb0, 0x6a, 0x57, 0xe9, 0x74, 0x97, 0xc7, 0xae, 0xac, 0xf5,
    0x5f, 0x28, 0x7e, 0x80, 0xc7, 0xa1, 0x7b, 0x61, 0xdf, 0xa3, 0x53, 0xce, 0x08, 0x40, 0x90, 0x3c,
    0x51, 0x7b, 0xe2, 0x47, 0xf0, 0xfa, 0xe0, 0xb2, 0xbb, 0xc0, 0xbb, 0x78, 0x50, 0xa9, 0xff, 0x87,
    0x30, 0x49, 0x92, 0xc0, 0xbb, 0x78, 0xb8, 0x1c, 0x2e, 0x08, 0xde, 0xe8, 0x3d, 0xea, 0x26, 0x7a,
    0xc4, 0x9f, 0x03, 0xa7, 0xde, 0x11, 0xe4, 0xe0, 0x6d, 0xb3, 0xa2, 0xf1, 0x21, 0x6c, 0x16, 0x1b,
    0xec, 0x56, 0x27, 0x4e, 0xbf, 0xc7, 0x88, 0xe5, 0x58, 0x24, 0xa6, 0x26, 0x42, 0xad, 0x51, 0xf5,
    0x4b, 0x79, 0x3e, 0xaf, 0x0f, 0x96, 0x36, 0x0b, 0x04, 0xe1, 0xf4, 0xf2, 0x38, 0x81, 0x29, 0xce,
    0x00, 0x73, 0xa2, 0x39, 0xfa, 0x7a, 0x01, 0xde, 0x56, 0x00, 0x00, 0x64, 0xcd, 0xd6, 0x35, 0xda,
    0x23, 0x8e, 0x8d, 0x6e, 0x91, 0x46, 0xd7, 0x9a, 0xbf, 0xdb, 0xe9, 0x86, 0xa5, 0xfd, 0xdc, 0x97,
    0x55, 0x72, 0x2a, 0x0e, 0xa9, 0x99, 0x29, 0x91, 0x7f, 0xcf, 0x30, 0x05, 0x5a, 0x1b, 0xdb, 0x21,
    0x0a, 0xe7, 0x2e, 0x90, 0x25, 0xa6, 0x26, 0x40, 0x67, 0x88, 0xae, 0xc9, 0x53, 0x96, 0xb0, 0x48,
    0x1f, 0x3d, 0x55, 0xcb, 0xc0, 0xd4, 0x54, 0x14, 0x6d, 0xc6, 0x07, 0x00, 0xde, 0xd5, 0xfb, 0x47,
    0x32, 0x05, 0x9f, 0x00, 0x5f, 0x3f, 0x74, 0xcd, 0x5e, 0xaf, 0xaf, 0x57, 0xe3, 0x03, 0x00, 0xef,
    0xe4, 0xa3, 0x8e, 0x3f, 0x91, 0x8a, 0x30, 0x58, 0x6c, 0x85, 0x8c, 0x57, 0xf0, 0xcc, 0x89, 0xca,
    0x70, 0xc6, 0xcf, 0x78, 0x4f, 0xfb, 0xe1, 0xc3, 0xf3, 0xfe, 0xfc, 0x8b, 0x68, 0x0d, 0x9f, 0x05,
    0xea, 0x9d, 0xcd, 0x78, 0x7d, 0xfc, 0xb8, 0x68, 0xac, 0xbc, 0x46, 0xa7, 0xe9, 0xf5, 0xff, 0x84,
    0x21, 0xfd, 0xe2, 0x03, 0xa8, 0x34, 0x1c, 0x18, 0xc2, 0x84, 0x54, 0x97, 0x41, 0xdf, 0x8b, 0xfa,
    0x5c, 0x97, 0x33, 0x6e, 0x9f, 0x6b, 0x58, 0x34, 0x56, 0xde, 0x68, 0xd6, 0x43, 0xa3, 0x3b, 0xfb,
    0xa3, 0x58, 0x84, 0x10, 0xc4, 0x27, 0xc5, 0x83, 0x30, 0x91, 0x3f, 0xb4, 0xc4, 0x30, 0x0c, 0xe2,
    0x53, 0xe2, 0xce, 0xf1, 0x2d, 0x34, 0x5a, 0x0d, 0x0c, 0x26, 0x43, 0x94, 0x0a, 0x80, 0xcf, 0xe1,
    0x7c, 0xa2, 0x2f, 0x4a, 0x5f, 0xca, 0x4f, 0x90, 0x9c, 0x9e, 0x0c, 0x97, 0xc3, 0x05, 0x2f, 0xef,
    0x03, 0xc3, 0x32, 0xd0, 0x19, 0xb4, 0x50, 0xa9, 0x55, 0xfd, 0x56, 0xa2, 0xce, 0xa0, 0x03, 0xa7,
    0xe6, 0xe0, 0x76, 0xf0, 0x90, 0x44, 0x09, 0x6a, 0x8d, 0x1a, 0x7a, 0x53, 0xf4, 0xae, 0x9c, 0x0b,
    0xa2, 0xcf, 0xc4, 0x89, 0x92, 0xa8, 0x41, 0x14, 0x43, 0x6f, 0xd4, 0x43, 0x6f, 0x3c, 0x7f, 0xe5,
    0xa9, 0x54, 0x2a, 0xa8, 0x12, 0x54, 0x88, 0x05, 0x48, 0x54, 0xd0, 0x30, 0x3e, 0xc9, 0xa7, 0x1c,
    0xf5, 0xba, 0x40, 0x21, 0x88, 0x3e, 0x96, 0x11, 0x95, 0x53, 0xbe, 0x17, 0x2c, 0x7c, 0xa2, 0x8f,
    0x61, 0x24, 0xaa, 0x1c, 0xfa, 0xb8, 0x50, 0x21, 0x51, 0x11, 0x0c, 0x21, 0xca, 0x08, 0x70, 0xa1,
    0x82, 0x65, 0x38, 0xca, 0xa8, 0x18, 0x4e, 0x52, 0xa8, 0xb8, 0x40, 0x05, 0x40, 0x58, 0xca, 0xa9,
    0x39, 0x8d, 0xd7, 0x23, 0xf0, 0x31, 0xbf, 0x14, 0x4c, 0x40, 0xc0, 0xb1, 0x2a, 0x30, 0x0c, 0x03,
    0x96, 0x51, 0x81, 0x65, 0x18, 0xb0, 0xa7, 0x3e, 0x8a, 0x2a, 0x52, 0x01, 0xa2, 0x24, 0x41, 0x10,
    0x7d, 0xa0, 0xf4, 0xd4, 0x4f, 0xc4, 0xfe, 0xe6, 0x18, 0x8d, 0x4a, 0xe7, 0xe1, 0x34, 0x6a, 0x9d,
    0xcd, 0xce, 0x47, 0xf7, 0xb9, 0x7f, 0x96, 0xe1, 0x60, 0xd2, 0x9a, 0x61, 0xd0, 0x98, 0xa0, 0x55,
    0x1b, 0xa0, 0x53, 0x1b, 0xa0, 0x55, 0xe9, 0xa0, 0x53, 0x19, 0xa0, 0x55, 0xe9, 0xa1, 0x55, 0xeb,
    0x64, 0x67, 0xf1, 0xe4, 0xc7, 0x47, 0x09, 0xbc, 0xd7, 0x0d, 0xb7, 0xcf, 0x09, 0xde, 0xeb, 0x84,
    0x5b, 0x70, 0x83, 0xf7, 0xba, 0xc0, 0x7b, 0x9d, 0x70, 0x7a, 0xec, 0xb0, 0xf3, 0x56, 0x44, 0xfb,
    0xa1, 0x59, 0x0d, 0xa7, 0xb1, 0x72, 0x46, 0xb5, 0xf9, 0x58, 0x27, 0x5a, 0x53, 0xa3, 0xa2, 0x15,
    0x13, 0x06, 0x66, 0x6d, 0x3c, 0xe2, 0xf4, 0x09, 0x30, 0x6a, 0xe3, 0x60, 0xd2, 0xc5, 0xc3, 0xac,
    0x8d, 0x83, 0x4e, 0x6d, 0x42, 0xa4, 0x17, 0xff, 0x18, 0xc2, 0x40, 0xaf, 0x31, 0x40, 0xaf, 0xe9,
    0x7d, 0x96, 0x8f, 0x52, 0xc0, 0xed, 0xb5, 0xc3, 0xc6, 0x5b, 0x61, 0x77, 0xf7, 0xc0, 0xc1, 0x5b,
    0xd1, 0xe3, 0xb2, 0xc0, 0xce, 0x5b, 0x41, 0x69, 0x74, 0x8c, 0xaa, 0x46, 0x4d, 0xdc, 0x51, 0x2e,
    0x4e, 0x97, 0xb8, 0x03, 0xc0, 0xa4, 0xc1, 0x59, 0x41, 0x33, 0xe2, 0x0d, 0x49, 0x88, 0xd7, 0x27,
    0x21, 0xc1, 0x90, 0x82, 0x38, 0x7d, 0xe2, 0x99, 0xb7, 0x8e, 0x0f, 0xbc, 0x18, 0x01, 0xbd, 0xc6,
    0x04, 0xbd, 0xc6, 0x84, 0xf4, 0xb8, 0xff, 0x7d, 0x1d, 0x4d, 0x94, 0x04, 0x58, 0x5d, 0xdd, 0xe8,
    0x76, 0x76, 0xa0, 0xdb, 0xd5, 0x89, 0x1e, 0x67, 0x17, 0x9c, 0x1e, 0xfb, 0xa0, 0x14, 0x40, 0xbc,
    0x2e, 0x7e, 0x3b, 0x97, 0x9d, 0x76, 0xd1, 0xba, 0xaf, 0xda, 0x0f, 0xfc, 0xdc, 0xe9, 0x71, 0x0c,
    0x78, 0x85, 0xf4, 0x1a, 0x13, 0x52, 0xcc, 0x43, 0x90, 0x64, 0x4c, 0x41, 0xb2, 0x71, 0x08, 0x74,
    0xea, 0xe8, 0x3b, 0xa2, 0xc0, 0x32, 0x1c, 0x12, 0x8d, 0x29, 0x48, 0x34, 0xfe, 0xef, 0x68, 0xa5,
    0xc7, 0xc7, 0xa3, 0xdb, 0xd5, 0x89, 0x2e, 0x47, 0x3b, 0x3a, 0x6d, 0x2d, 0xb0, 0xba, 0x2c, 0x03,
    0xee, 0x63, 0x18, 0x34, 0x26, 0x0c, 0x49, 0x1d, 0x59, 0x45, 0x6a, 0x8e, 0xd4, 0x68, 0xba, 0xec,
    0x07, 0xed, 0x07, 0x1a, 0x6b, 0x55, 0x03, 0x6a, 0x70, 0x53, 0x3a, 0x74, 0xaa, 0xe8, 0x5c, 0x54,
    0x09, 0x15, 0x5e, 0x81, 0x87, 0xc5, 0xf9, 0x0d, 0x41, 0xb8, 0xbb, 0x70, 0xbe, 0x57, 0x94, 0xc7,
    0x65, 0xe7, 0x7b, 0xc7, 0xa9, 0xb2, 0x4d, 0x04, 0x00, 0x3e, 0x69, 0xac, 0xda, 0xb2, 0xbd, 0xee,
    0x83, 0x82, 0x1e, 0x67, 0x57, 0xbf, 0x15, 0x48, 0x08, 0x81, 0x59, 0x97, 0x80, 0x24, 0x53, 0x1a,
    0x92, 0x8d, 0xe9, 0x48, 0x32, 0xa6, 0x42, 0xcd, 0x69, 0xa0, 0x00, 0xf0, 0x08, 0x1e, 0x58, 0x1c,
    0x6d, 0xe8, 0xb4, 0xb7, 0xa2, 0xcb, 0xde, 0x0e, 0x1b, 0x6f, 0xe9, 0x57, 0x41, 0x24, 0xe8, 0x93,
    0x31, 0x7d, 0xf4, 0xd5, 0x9b, 0xa7, 0x67, 0x2d, 0x9a, 0xcb, 0x01, 0x00, 0x61, 0x98, 0xf7, 0x26,
    0x8d, 0x28, 0x28, 0xf8, 0xb4, 0x7e, 0x63, 0xc4, 0xc6, 0x2b, 0x86, 0x30, 0x88, 0xd3, 0x27, 0x22,
    0xd9, 0x74, 0xd2, 0xd8, 0x89, 0xc6, 0x54, 0xa8, 0x58, 0xb5, 0x62, 0xed, 0xde, 0xbd, 0x71, 0x0c,
    0x89, 0xcf, 0xc6, 0x90, 0xf8, 0x6c, 0x00, 0x80, 0x4f, 0xf4, 0xa2, 0xcb, 0xd1, 0x8e, 0x2e, 0x7b,
    0x1b, 0xba, 0x1c, 0x6d, 0xe8, 0x71, 0x59, 0x22, 0xe6, 0x58, 0x1a, 0x35, 0x66, 0x4c, 0x1c, 0x39,
    0x0b, 0x0c, 0x61, 0xde, 0x3b, 0x19, 0x1e, 0x03, 0xa8, 0x6d, 0x7a, 0x6f, 0xa8, 0x00, 0xf6, 0xb8,
    0x47, 0xf0, 0x90, 0x83, 0x0d, 0x7b, 0xd0, 0x68, 0x39, 0x1a, 0xf2, 0x18, 0xc5, 0x32, 0x2c, 0xe2,
    0x0d, 0xc9, 0x48, 0x36, 0xa6, 0x21, 0xc9, 0x98, 0x86, 0x04, 0x63, 0x0a, 0xb8, 0x20, 0xbe, 0x85,
    0xa3, 0x20, 0x30, 0x04, 0x49, 0x80, 0xc5, 0xd1, 0x71, 0xb2, 0x97, 0x70, 0xb4, 0xa1, 0xc7, 0xd9,
    0x19, 0x72, 0x08, 0x4a, 0x40, 0x90, 0x95, 0x94, 0x83, 0x4b, 0xb2, 0x26, 0x42, 0xcd, 0x69, 0x24,
    0x49, 0x94, 0x86, 0x4e, 0xcf, 0x5e, 0xd4, 0x7c, 0x26, 0x78, 0xda, 0xd1, 0x54, 0xbd, 0x19, 0xc0,
    0x6c, 0x00, 0x70, 0x78, 0x6c, 0x68, 0xb2, 0x1c, 0x43, 0x97, 0xa3, 0x1d, 0x2e, 0x8f, 0x0d, 0x5e,
    0xc1, 0x0b, 0x9f, 0xe8, 0x85, 0x8a, 0x55, 0x43, 0xa3, 0xd2, 0x42, 0xc3, 0x69, 0xa1, 0xe1, 0x74,
    0x30, 0xe9, 0xe2, 0x60, 0xd6, 0xc5, 0xc3, 0xac, 0x4b, 0x80, 0x41, 0x63, 0x8e, 0xfc, 0x46, 0x4c,
    0x05, 0x32, 0x21, 0x28, 0x85, 0xd3, 0x63, 0x83, 0xcd, 0xdd, 0x0d, 0xab, 0xeb, 0x64, 0x08, 0xea,
    0x11, 0xdc, 0xf0, 0x08, 0x3c, 0x3c, 0x3e, 0xfe, 0x8c, 0xad, 0x54, 0x9c, 0x06, 0x06, 0x8d, 0x09,
    0xc9, 0xa6, 0x54, 0x64, 0x26, 0xe4, 0xc0, 0xa0, 0x39, 0xbd, 0xf5, 0x83, 0x7e, 0x38, 0x35, 0x73,
    0xe1, 0x3c, 0xe0, 0x9b, 0x9f, 0x8e, 0xa5, 0x64, 0x15, 0x08, 0x9d, 0x7d, 0xba, 0x9b, 0xc8, 0x1b,
    0x72, 0x99, 0xc2, 0xf4, 0xa0, 0x9d, 0x0f, 0x21, 0x30, 0x6a, 0xe3, 0x60, 0xd4, 0xc6, 0x21, 0x23,
    0x21, 0xac, 0x1b, 0xac, 0x3a, 0x33, 0x54, 0x9f, 0xfe, 0xa5, 0x31, 0xd3, 0xb5, 0x0e, 0xc0, 0x57,
    0x0a, 0xbd, 0xb1, 0xae, 0x1e, 0x7c, 0xed, 0x19, 0x62, 0xdc, 0x70, 0x8e, 0x00, 0x4a, 0x48, 0x89,
    0x08, 0x4a, 0x7f, 0xa3, 0x30, 0x14, 0xeb, 0xe3, 0x07, 0x79, 0xba, 0x80, 0x14, 0x08, 0xe7, 0x08,
    0x00, 0x00, 0xe2, 0xac, 0xfc, 0x1a, 0x00, 0x47, 0x14, 0x96, 0x62, 0x16, 0x87, 0xd5, 0x2d, 0xe9,
    0xaf, 0x9d, 0x15, 0xad, 0x7d, 0xf3, 0x8f, 0x8b, 0x2f, 0x2e, 0xf1, 0x12, 0x40, 0x79, 0x49, 0x44,
    0x8c, 0x42, 0x92, 0x70, 0xdf, 0x84, 0x09, 0x13, 0x7c, 0xb2, 0x02, 0x00, 0x80, 0x29, 0x99, 0x45,
    0xd5, 0x20, 0xa8, 0x54, 0xe8, 0x8a, 0xb9, 0xc1, 0xbf, 0x7c, 0xfa, 0xd0, 0xa2, 0x9a, 0x73, 0xe6,
    0x6b, 0x7a, 0xcb, 0xea, 0xf1, 0x08, 0xb7, 0x03, 0x38, 0xa1, 0x90, 0x16, 0x33, 0x68, 0x84, 0xc4,
    0xf5, 0xfa, 0xf2, 0xaf, 0x5e, 0x05, 0x50, 0x90, 0x73, 0x6d, 0x0f, 0x25, 0xcc, 0x4d, 0x00, 0x94,
    0x0d, 0x83, 0x31, 0xd0, 0xf3, 0x53, 0x4a, 0x6f, 0x99, 0x3a, 0xf4, 0x6a, 0x4b, 0xd0, 0x02, 0x00,
    0x80, 0x69, 0x19, 0xf3, 0xff, 0x49, 0x01, 0xe5, 0x23, 0x91, 0x51, 0xef, 0xf5, 0x63, 0xe5, 0xb4,
    0xac, 0x85, 0x5b, 0xe4, 0x92, 0xfd, 0x6e, 0x93, 0x69, 0xca, 0x70, 0xff, 0x52, 0xf1, 0x07, 0xa2,
    0x7a, 0xdc, 0xaf, 0x6a, 0xcc, 0x74, 0x3f, 0xea, 0x7f, 0x5a, 0x20, 0x00, 0x0e, 0x1e, 0x2c, 0x57,
    0xf7, 0xc4, 0xeb, 0x37, 0x10, 0xd0, 0xb9, 0x0a, 0xa1, 0xd1, 0x64, 0x7b, 0x6c, 0xf3, 0x78, 0x8c,
    0xf3, 0x0a, 0x72, 0x0a, 0xf8, 0x3e, 0x09, 0x00, 0x00, 0x76, 0x75, 0xd5, 0x98, 0x25, 0x5e, 0xdc,
    0x0a, 0x60, 0xbc, 0xc2, 0x6c, 0x54, 0x60, 0xbf, 0xc7, 0x2b, 0xcc, 0x2a, 0xc8, 0xb9, 0xb6, 0x27,
    0x50, 0xc6, 0xa0, 0x76, 0x4a, 0x4e, 0x4e, 0x2a, 0xb4, 0x71, 0x2a, 0x66, 0x3e, 0x80, 0xfd, 0x0a,
    0xb7, 0x83, 0x1e, 0xff, 0xa2, 0xac, 0x6f, 0x6e, 0x30, 0xc6, 0x0f, 0x5a, 0x00, 0x00, 0x30, 0x29,
    0x75, 0x7e, 0xab, 0x47, 0xe5, 0x9e, 0x06, 0xe0, 0x7d, 0x85, 0xe3, 0xc1, 0xea, 0xef, 0x91, 0x4d,
    0x8c, 0x96, 0x9d, 0x35, 0x2d, 0xfd, 0xba, 0xf6, 0x60, 0xaf, 0x09, 0x69, 0xaf, 0x74, 0x41, 0x6a,
    0x89, 0xa3, 0x31, 0xc3, 0xbd, 0x08, 0x04, 0x2f, 0x28, 0x74, 0x0f, 0x3a, 0xbc, 0xac, 0x69, 0x49,
    0x9f, 0x3f, 0x39, 0xa9, 0xd0, 0x16, 0x9a, 0xab, 0x10, 0x26, 0x76, 0x34, 0x57, 0x3f, 0x04, 0x8a,
    0x27, 0xf0, 0xcd, 0x25, 0x65, 0x05, 0x03, 0x01, 0x81, 0x00, 0x0f, 0x4f, 0xc9, 0x2c, 0x2a, 0x0b,
    0xcf, 0x57, 0xec, 0x03, 0x76, 0x34, 0x54, 0x4f, 0x02, 0x83, 0xd7, 0x01, 0x8c, 0x52, 0xec, 0x30,
    0x20, 0xae, 0xfe, 0x31, 0x86, 0xd0, 0x9b, 0x26, 0x67, 0x14, 0x7d, 0x12, 0xee, 0x1d, 0x98, 0xbe,
    0x14, 0x3f, 0x75, 0x68, 0x51, 0xad, 0xa4, 0x96, 0xae, 0x00, 0xa5, 0xab, 0x15, 0x63, 0x9c, 0xf7,
    0x01, 0xff, 0x35, 0x8f, 0xca, 0x75, 0x69, 0x5f, 0x8c, 0xdf, 0xe7, 0x1e, 0xe0, 0x9b, 0xd8, 0xd9,
    0xbc, 0xe1, 0x3a, 0x4a, 0xe9, 0x1f, 0x00, 0x64, 0x29, 0xd6, 0xe9, 0x4f, 0xbb, 0xa3, 0x81, 0x10,
    0xdc, 0x33, 0x35, 0xa3, 0xe8, 0xdd, 0x48, 0xdc, 0x8f, 0x89, 0x54, 0xc5, 0xa6, 0x64, 0x2c, 0x58,
    0xa7, 0x26, 0xc8, 0xa3, 0x20, 0xcb, 0x01, 0xd8, 0x15, 0x53, 0x45, 0x1c, 0x2e, 0x02, 0xba, 0xd2,
    0xab, 0x72, 0x8f, 0x8d, 0x94, 0xf1, 0x23, 0xda, 0x03, 0x7c, 0xcb, 0x37, 0xc8, 0x04, 0xa1, 0x8f,
    0x81, 0x90, 0xdb, 0x22, 0x29, 0xb2, 0x0b, 0x37, 0xba, 0x43, 0x25, 0x05, 0xf3, 0xc0, 0xb4, 0x8c,
    0xf9, 0xc7, 0x23, 0xee, 0x45, 0xf4, 0x67, 0xcd, 0x77, 0x34, 0x55, 0x5d, 0x4e, 0xc0, 0x3c, 0x48,
    0x81, 0x62, 0x00, 0xca, 0x9b, 0x28, 0x42, 0xf4, 0xee, 0x01, 0x94, 0x8b, 0x14, 0x65, 0x33, 0xb2,
    0x8a, 0xbe, 0xe8, 0x37, 0x37, 0xf2, 0x7c, 0x3c, 0xc9, 0xce, 0x96, 0x77, 0x87, 0x43, 0xe2, 0xee,
    0xa5, 0xc0, 0x6d, 0x00, 0x94, 0x6f, 0x12, 0xf9, 0x87, 0x07, 0x14, 0xe5, 0x2c, 0x98, 0x27, 0xf3,
    0xb3, 0xe6, 0xd7, 0xf7, 0x7b, 0x1c, 0x71, 0x3e, 0x9f, 0xec, 0x9f, 0x2d, 0x35, 0x29, 0x2a, 0x51,
    0x58, 0x22, 0x11, 0x72, 0x33, 0x01, 0x72, 0x15, 0x5b, 0x9f, 0x85, 0x3a, 0x10, 0xbc, 0xaa, 0x06,
    0x5e, 0x9c, 0x90, 0x51, 0xd4, 0x79, 0xde, 0x02, 0xc9, 0x81, 0x7a, 0xda, 0x5d, 0xcd, 0x1b, 0xae,
    0xa0, 0x94, 0xde, 0x2c, 0x01, 0x3f, 0x24, 0x40, 0xf2, 0x05, 0x6a, 0xf4, 0x1e, 0x50, 0x54, 0x13,
    0x42, 0xd6, 0x4e, 0xce, 0x98, 0xff, 0x11, 0x21, 0xe4, 0xbc, 0x1f, 0x19, 0x1e, 0xf0, 0xa3, 0x3c,
    0x5b, 0x8f, 0x6e, 0xd5, 0x6a, 0x35, 0xce, 0x42, 0x50, 0xba, 0x80, 0x02, 0x85, 0x00, 0xd2, 0x62,
    0xdc, 0xe8, 0xad, 0x00, 0x6a, 0x40, 0xb0, 0xa1, 0xc7, 0xc9, 0xd6, 0x14, 0x8e, 0x2a, 0xf4, 0x0c,
    0x64, 0x65, 0x06, 0xd5, 0x59, 0x2e, 0x4a, 0x29, 0xd9, 0xdd, 0xf2, 0xfe, 0x78, 0x49, 0x42, 0x21,
    0x21, 0xb4, 0x90, 0x02, 0x13, 0x63, 0xc0, 0x79, 0x14, 0x09, 0xb0, 0x47, 0xa2, 0x78, 0x9f, 0x65,
    0xc8, 0x07, 0xf9, 0x43, 0xe6, 0xef, 0x1b, 0x88, 0x96, 0x1e, 0x15, 0x02, 0x38, 0xa7, 0x77, 0x68,
    0x2f, 0x37, 0xea, 0x04, 0xdd, 0x77, 0x24, 0xe0, 0x0a, 0x48, 0xb8, 0x02, 0x84, 0xcc, 0x00, 0xe8,
    0xf0, 0x41, 0x6d, 0x6e, 0x82, 0x16, 0x50, 0xec, 0xa5, 0xc0, 0x5e, 0x06, 0xd8, 0x2b, 0x08, 0xe4,
    0x93, 0x19, 0xc3, 0x16, 0x74, 0x0f, 0xde, 0xea, 0x46, 0x19, 0x3e, 0x69, 0xae, 0xce, 0x66, 0x24,
    0x7a, 0x19, 0x05, 0x93, 0x47, 0x20, 0xe5, 0x82, 0x21, 0xb9, 0x00, 0xf2, 0x40, 0x71, 0xbe, 0xbf,
    0x79, 0xd8, 0x0c, 0x82, 0x7a, 0x48, 0xb4, 0x9e, 0x82, 0xa9, 0x27, 0x90, 0xea, 0x38, 0x22, 0x7d,
    0x31, 0x29, 0xf3, 0x9a, 0x86, 0x68, 0xe2, 0x33, 0x66, 0x8e, 0xf3, 0xee, 0xea, 0xaa, 0x31, 0x8b,
    0x3e, 0x29, 0x9b, 0x08, 0x34, 0x8d, 0x30, 0x48, 0xa7, 0x20, 0x29, 0x90, 0x68, 0x2a, 0x21, 0x48,
    0xa7, 0x40, 0x0a, 0x00, 0x2d, 0x08, 0x38, 0x50, 0x98, 0x4e, 0x3d, 0x79, 0x3c, 0xa4, 0x53, 0xcf,
    0xcf, 0x80, 0x82, 0xa2, 0xe7, 0xd4, 0xff, 0xed, 0xa0, 0x10, 0x00, 0xf0, 0x04, 0xe8, 0x90, 0x28,
    0x6d, 0x21, 0x0c, 0xd3, 0x41, 0x40, 0x3b, 0xa8, 0x84, 0x56, 0xca, 0x91, 0x36, 0xca, 0x8a, 0xc7,
    0xa7, 0xa7, 0x2c, 0x52, 0x66, 0x3b, 0x15, 0x28, 0x50, 0xa0, 0x40, 0x81, 0x02, 0x05, 0x0a, 0x14,
    0x28, 0x50, 0xa0, 0x40, 0x41, 0xb4, 0xe1, 0xbf, 0xc6, 0x30, 0x02, 0xad, 0x4b, 0xd9, 0x15, 0xa1,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
unsigned int frogArrayLength = 5884;

std::unique_ptr<TextureCacheBase> g_texture_cache;

std::bitset<8> TextureCacheBase::valid_bind_points;

TextureCacheBase::TCacheEntry::TCacheEntry(std::unique_ptr<AbstractTexture> tex,
                                           std::unique_ptr<AbstractFramebuffer> fb)
    : texture(std::move(tex)), framebuffer(std::move(fb))
{
}

TextureCacheBase::TCacheEntry::~TCacheEntry()
{
  for (auto& reference : references)
    reference->references.erase(this);
}

void TextureCacheBase::CheckTempSize(size_t required_size)
{
  if (required_size <= temp_size)
    return;

  temp_size = required_size;
  Common::FreeAlignedMemory(temp);
  temp = static_cast<u8*>(Common::AllocateAlignedMemory(temp_size, 16));
}

TextureCacheBase::TextureCacheBase()
{
  SetBackupConfig(g_ActiveConfig);

  temp_size = 2048 * 2048 * 4;
  temp = static_cast<u8*>(Common::AllocateAlignedMemory(temp_size, 16));

  TexDecoder_SetTexFmtOverlayOptions(backup_config.texfmt_overlay,
                                     backup_config.texfmt_overlay_center);

  HiresTexture::Init();

  Common::SetHash64Function();

  InvalidateAllBindPoints();
}

TextureCacheBase::~TextureCacheBase()
{
  // Clear pending EFB copies first, so we don't try to flush them.
  m_pending_efb_copies.clear();

  HiresTexture::Shutdown();
  Invalidate();
  Common::FreeAlignedMemory(temp);
  temp = nullptr;
}

bool TextureCacheBase::Initialize()
{
  if (!CreateUtilityTextures())
  {
    PanicAlertFmt("Failed to create utility textures.");
    return false;
  }

  return true;
}

void TextureCacheBase::Invalidate()
{
  FlushEFBCopies();
  InvalidateAllBindPoints();

  bound_textures.fill(nullptr);
  for (auto& tex : textures_by_address)
  {
    delete tex.second;
  }
  textures_by_address.clear();
  textures_by_hash.clear();

  texture_pool.clear();
}

void TextureCacheBase::ForceReload()
{
  Invalidate();

  // Clear all current hires textures, they are invalid
  HiresTexture::Clear();

  // Load fresh
  HiresTexture::Update();
}

void TextureCacheBase::OnConfigChanged(const VideoConfig& config)
{
  if (config.bHiresTextures != backup_config.hires_textures ||
      config.bCacheHiresTextures != backup_config.cache_hires_textures)
  {
    HiresTexture::Update();
  }

  // TODO: Invalidating texcache is really stupid in some of these cases
  if (config.iSafeTextureCache_ColorSamples != backup_config.color_samples ||
      config.bTexFmtOverlayEnable != backup_config.texfmt_overlay ||
      config.bTexFmtOverlayCenter != backup_config.texfmt_overlay_center ||
      config.bHiresTextures != backup_config.hires_textures ||
      config.bEnableGPUTextureDecoding != backup_config.gpu_texture_decoding ||
      config.bDisableCopyToVRAM != backup_config.disable_vram_copies ||
      config.bArbitraryMipmapDetection != backup_config.arbitrary_mipmap_detection)
  {
    Invalidate();
    TexDecoder_SetTexFmtOverlayOptions(config.bTexFmtOverlayEnable, config.bTexFmtOverlayCenter);
  }

  SetBackupConfig(config);
}

void TextureCacheBase::Cleanup(int _frameCount)
{
  TexAddrCache::iterator iter = textures_by_address.begin();
  TexAddrCache::iterator tcend = textures_by_address.end();
  while (iter != tcend)
  {
    if (iter->second->tmem_only)
    {
      iter = InvalidateTexture(iter);
    }
    else if (iter->second->frameCount == FRAMECOUNT_INVALID)
    {
      iter->second->frameCount = _frameCount;
      ++iter;
    }
    else if (_frameCount > TEXTURE_KILL_THRESHOLD + iter->second->frameCount)
    {
      if (iter->second->IsCopy())
      {
        // Only remove EFB copies when they wouldn't be used anymore(changed hash), because EFB
        // copies living on the
        // host GPU are unrecoverable. Perform this check only every TEXTURE_KILL_THRESHOLD for
        // performance reasons
        if ((_frameCount - iter->second->frameCount) % TEXTURE_KILL_THRESHOLD == 1 &&
            iter->second->hash != iter->second->CalculateHash())
        {
          iter = InvalidateTexture(iter);
        }
        else
        {
          ++iter;
        }
      }
      else
      {
        iter = InvalidateTexture(iter);
      }
    }
    else
    {
      ++iter;
    }
  }

  TexPool::iterator iter2 = texture_pool.begin();
  TexPool::iterator tcend2 = texture_pool.end();
  while (iter2 != tcend2)
  {
    if (iter2->second.frameCount == FRAMECOUNT_INVALID)
    {
      iter2->second.frameCount = _frameCount;
    }
    if (_frameCount > TEXTURE_POOL_KILL_THRESHOLD + iter2->second.frameCount)
    {
      iter2 = texture_pool.erase(iter2);
    }
    else
    {
      ++iter2;
    }
  }
}

bool TextureCacheBase::TCacheEntry::OverlapsMemoryRange(u32 range_address, u32 range_size) const
{
  if (addr + size_in_bytes <= range_address)
    return false;

  if (addr >= range_address + range_size)
    return false;

  return true;
}

void TextureCacheBase::SetBackupConfig(const VideoConfig& config)
{
  backup_config.color_samples = config.iSafeTextureCache_ColorSamples;
  backup_config.texfmt_overlay = config.bTexFmtOverlayEnable;
  backup_config.texfmt_overlay_center = config.bTexFmtOverlayCenter;
  backup_config.hires_textures = config.bHiresTextures;
  backup_config.cache_hires_textures = config.bCacheHiresTextures;
  backup_config.stereo_3d = config.stereo_mode != StereoMode::Off;
  backup_config.efb_mono_depth = config.bStereoEFBMonoDepth;
  backup_config.gpu_texture_decoding = config.bEnableGPUTextureDecoding;
  backup_config.disable_vram_copies = config.bDisableCopyToVRAM;
  backup_config.arbitrary_mipmap_detection = config.bArbitraryMipmapDetection;
}

TextureCacheBase::TCacheEntry*
TextureCacheBase::ApplyPaletteToEntry(TCacheEntry* entry, const u8* palette, TLUTFormat tlutfmt)
{
  DEBUG_ASSERT(g_ActiveConfig.backend_info.bSupportsPaletteConversion);

  const AbstractPipeline* pipeline = g_shader_cache->GetPaletteConversionPipeline(tlutfmt);
  if (!pipeline)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to get conversion pipeline for format {:#04X}",
                  static_cast<u32>(tlutfmt));
    return nullptr;
  }

  TextureConfig new_config = entry->texture->GetConfig();
  new_config.levels = 1;
  new_config.flags |= AbstractTextureFlag_RenderTarget;

  TCacheEntry* decoded_entry = AllocateCacheEntry(new_config);
  if (!decoded_entry)
    return nullptr;

  decoded_entry->SetGeneralParameters(entry->addr, entry->size_in_bytes, entry->format,
                                      entry->should_force_safe_hashing);
  decoded_entry->SetDimensions(entry->native_width, entry->native_height, 1);
  decoded_entry->SetHashes(entry->base_hash, entry->hash);
  decoded_entry->frameCount = FRAMECOUNT_INVALID;
  decoded_entry->should_force_safe_hashing = false;
  decoded_entry->SetNotCopy();
  decoded_entry->may_have_overlapping_textures = entry->may_have_overlapping_textures;

  g_renderer->BeginUtilityDrawing();

  const u32 palette_size = entry->format == TextureFormat::I4 ? 32 : 512;
  u32 texel_buffer_offset;
  if (g_vertex_manager->UploadTexelBuffer(palette, palette_size,
                                          TexelBufferFormat::TEXEL_BUFFER_FORMAT_R16_UINT,
                                          &texel_buffer_offset))
  {
    struct Uniforms
    {
      float multiplier;
      u32 texel_buffer_offset;
      u32 pad[2];
    };
    static_assert(std::is_standard_layout<Uniforms>::value);
    Uniforms uniforms = {};
    uniforms.multiplier = entry->format == TextureFormat::I4 ? 15.0f : 255.0f;
    uniforms.texel_buffer_offset = texel_buffer_offset;
    g_vertex_manager->UploadUtilityUniforms(&uniforms, sizeof(uniforms));

    g_renderer->SetAndDiscardFramebuffer(decoded_entry->framebuffer.get());
    g_renderer->SetViewportAndScissor(decoded_entry->texture->GetRect());
    g_renderer->SetPipeline(pipeline);
    g_renderer->SetTexture(1, entry->texture.get());
    g_renderer->SetSamplerState(1, RenderState::GetPointSamplerState());
    g_renderer->Draw(0, 3);
    g_renderer->EndUtilityDrawing();
    decoded_entry->texture->FinishedRendering();
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "Texel buffer upload of {} bytes failed", palette_size);
    g_renderer->EndUtilityDrawing();
  }

  textures_by_address.emplace(decoded_entry->addr, decoded_entry);

  return decoded_entry;
}

TextureCacheBase::TCacheEntry* TextureCacheBase::ReinterpretEntry(const TCacheEntry* existing_entry,
                                                                  TextureFormat new_format)
{
  const AbstractPipeline* pipeline =
      g_shader_cache->GetTextureReinterpretPipeline(existing_entry->format.texfmt, new_format);
  if (!pipeline)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to obtain texture reinterpreting pipeline from format {:#04X} to {:#04X}",
                  static_cast<u32>(existing_entry->format.texfmt), static_cast<u32>(new_format));
    return nullptr;
  }

  TextureConfig new_config = existing_entry->texture->GetConfig();
  new_config.levels = 1;
  new_config.flags |= AbstractTextureFlag_RenderTarget;

  TCacheEntry* reinterpreted_entry = AllocateCacheEntry(new_config);
  if (!reinterpreted_entry)
    return nullptr;

  reinterpreted_entry->SetGeneralParameters(existing_entry->addr, existing_entry->size_in_bytes,
                                            new_format, existing_entry->should_force_safe_hashing);
  reinterpreted_entry->SetDimensions(existing_entry->native_width, existing_entry->native_height,
                                     1);
  reinterpreted_entry->SetHashes(existing_entry->base_hash, existing_entry->hash);
  reinterpreted_entry->frameCount = existing_entry->frameCount;
  reinterpreted_entry->SetNotCopy();
  reinterpreted_entry->is_efb_copy = existing_entry->is_efb_copy;
  reinterpreted_entry->may_have_overlapping_textures =
      existing_entry->may_have_overlapping_textures;

  g_renderer->BeginUtilityDrawing();
  g_renderer->SetAndDiscardFramebuffer(reinterpreted_entry->framebuffer.get());
  g_renderer->SetViewportAndScissor(reinterpreted_entry->texture->GetRect());
  g_renderer->SetPipeline(pipeline);
  g_renderer->SetTexture(0, existing_entry->texture.get());
  g_renderer->SetSamplerState(1, RenderState::GetPointSamplerState());
  g_renderer->Draw(0, 3);
  g_renderer->EndUtilityDrawing();
  reinterpreted_entry->texture->FinishedRendering();

  textures_by_address.emplace(reinterpreted_entry->addr, reinterpreted_entry);

  return reinterpreted_entry;
}

void TextureCacheBase::ScaleTextureCacheEntryTo(TextureCacheBase::TCacheEntry* entry, u32 new_width,
                                                u32 new_height)
{
  if (entry->GetWidth() == new_width && entry->GetHeight() == new_height)
  {
    return;
  }

  const u32 max = g_ActiveConfig.backend_info.MaxTextureSize;
  if (max < new_width || max < new_height)
  {
    ERROR_LOG_FMT(VIDEO, "Texture too big, width = {}, height = {}", new_width, new_height);
    return;
  }

  const TextureConfig newconfig(new_width, new_height, 1, entry->GetNumLayers(), 1,
                                AbstractTextureFormat::RGBA8, AbstractTextureFlag_RenderTarget);
  std::optional<TexPoolEntry> new_texture = AllocateTexture(newconfig);
  if (!new_texture)
  {
    ERROR_LOG_FMT(VIDEO, "Scaling failed due to texture allocation failure");
    return;
  }

  // No need to convert the coordinates here since they'll be the same.
  g_renderer->ScaleTexture(new_texture->framebuffer.get(),
                           new_texture->texture->GetConfig().GetRect(), entry->texture.get(),
                           entry->texture->GetConfig().GetRect());
  entry->texture.swap(new_texture->texture);
  entry->framebuffer.swap(new_texture->framebuffer);

  // At this point new_texture has the old texture in it,
  // we can potentially reuse this, so let's move it back to the pool
  auto config = new_texture->texture->GetConfig();
  texture_pool.emplace(
      config, TexPoolEntry(std::move(new_texture->texture), std::move(new_texture->framebuffer)));
}

bool TextureCacheBase::CheckReadbackTexture(u32 width, u32 height, AbstractTextureFormat format)
{
  if (m_readback_texture && m_readback_texture->GetConfig().width >= width &&
      m_readback_texture->GetConfig().height >= height &&
      m_readback_texture->GetConfig().format == format)
  {
    return true;
  }

  TextureConfig staging_config(std::max(width, 128u), std::max(height, 128u), 1, 1, 1, format, 0);
  m_readback_texture.reset();
  m_readback_texture =
      g_renderer->CreateStagingTexture(StagingTextureType::Readback, staging_config);
  return m_readback_texture != nullptr;
}

void TextureCacheBase::SerializeTexture(AbstractTexture* tex, const TextureConfig& config,
                                        PointerWrap& p)
{
  // If we're in measure mode, skip the actual readback to save some time.
  const bool skip_readback = p.GetMode() == PointerWrap::MODE_MEASURE;
  p.DoPOD(config);

  if (skip_readback || CheckReadbackTexture(config.width, config.height, config.format))
  {
    // First, measure the amount of memory needed.
    u32 total_size = 0;
    for (u32 layer = 0; layer < config.layers; layer++)
    {
      for (u32 level = 0; level < config.levels; level++)
      {
        u32 level_width = std::max(config.width >> level, 1u);
        u32 level_height = std::max(config.height >> level, 1u);

        u32 stride = AbstractTexture::CalculateStrideForFormat(config.format, level_width);
        u32 size = stride * level_height;

        total_size += size;
      }
    }

    // Set aside total_size bytes of space for the textures.
    // When measuring, this will be set aside and not written to,
    // but when writing we'll use this pointer directly to avoid
    // needing to allocate/free an extra buffer.
    u8* texture_data = p.DoExternal(total_size);

    if (!skip_readback)
    {
      // Save out each layer of the texture to the pointer.
      for (u32 layer = 0; layer < config.layers; layer++)
      {
        for (u32 level = 0; level < config.levels; level++)
        {
          u32 level_width = std::max(config.width >> level, 1u);
          u32 level_height = std::max(config.height >> level, 1u);
          auto rect = tex->GetConfig().GetMipRect(level);
          m_readback_texture->CopyFromTexture(tex, rect, layer, level, rect);

          u32 stride = AbstractTexture::CalculateStrideForFormat(config.format, level_width);
          u32 size = stride * level_height;
          m_readback_texture->ReadTexels(rect, texture_data, stride);

          texture_data += size;
        }
      }
    }
  }
  else
  {
    PanicAlertFmt("Failed to create staging texture for serialization");
  }
}

std::optional<TextureCacheBase::TexPoolEntry> TextureCacheBase::DeserializeTexture(PointerWrap& p)
{
  TextureConfig config;
  p.Do(config);

  // Read in the size from the save state, then texture data will point to
  // a region of size total_size where textures are stored.
  u32 total_size = 0;
  u8* texture_data = p.DoExternal(total_size);

  if (p.GetMode() != PointerWrap::MODE_READ || total_size == 0)
    return std::nullopt;

  auto tex = AllocateTexture(config);
  if (!tex)
  {
    PanicAlertFmt("Failed to create texture for deserialization");
    return std::nullopt;
  }

  size_t start = 0;
  for (u32 layer = 0; layer < config.layers; layer++)
  {
    for (u32 level = 0; level < config.levels; level++)
    {
      const u32 level_width = std::max(config.width >> level, 1u);
      const u32 level_height = std::max(config.height >> level, 1u);
      const size_t stride = AbstractTexture::CalculateStrideForFormat(config.format, level_width);
      const size_t size = stride * level_height;
      if ((start + size) > total_size)
      {
        ERROR_LOG_FMT(VIDEO, "Insufficient texture data for layer {} level {}", layer, level);
        return tex;
      }

      tex->texture->Load(level, level_width, level_height, level_width, &texture_data[start], size);
      start += size;
    }
  }

  return tex;
}

void TextureCacheBase::DoState(PointerWrap& p)
{
  // Flush all pending XFB copies before either loading or saving.
  FlushEFBCopies();

  p.Do(last_entry_id);

  if (p.GetMode() == PointerWrap::MODE_WRITE || p.GetMode() == PointerWrap::MODE_MEASURE)
    DoSaveState(p);
  else
    DoLoadState(p);
}

void TextureCacheBase::DoSaveState(PointerWrap& p)
{
  std::map<const TCacheEntry*, u32> entry_map;
  std::vector<TCacheEntry*> entries_to_save;
  auto ShouldSaveEntry = [](const TCacheEntry* entry) {
    // We skip non-copies as they can be decoded from RAM when the state is loaded.
    // Storing them would duplicate data in the save state file, adding to decompression time.
    return entry->IsCopy();
  };
  auto AddCacheEntryToMap = [&entry_map, &entries_to_save](TCacheEntry* entry) -> u32 {
    auto iter = entry_map.find(entry);
    if (iter != entry_map.end())
      return iter->second;

    // Since we are sequentially allocating texture entries, we need to save the textures in the
    // same order they were collected. This is because of iterating both the address and hash maps.
    // Therefore, the map is used for fast lookup, and the vector for ordering.
    u32 id = static_cast<u32>(entry_map.size());
    entry_map.emplace(entry, id);
    entries_to_save.push_back(entry);
    return id;
  };
  auto GetCacheEntryId = [&entry_map](const TCacheEntry* entry) -> std::optional<u32> {
    auto iter = entry_map.find(entry);
    return iter != entry_map.end() ? std::make_optional(iter->second) : std::nullopt;
  };

  // Transform the textures_by_address and textures_by_hash maps to a mapping
  // of address/hash to entry ID.
  std::vector<std::pair<u32, u32>> textures_by_address_list;
  std::vector<std::pair<u64, u32>> textures_by_hash_list;
  if (Config::Get(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE))
  {
    for (const auto& it : textures_by_address)
    {
      if (ShouldSaveEntry(it.second))
      {
        const u32 id = AddCacheEntryToMap(it.second);
        textures_by_address_list.emplace_back(it.first, id);
      }
    }
    for (const auto& it : textures_by_hash)
    {
      if (ShouldSaveEntry(it.second))
      {
        const u32 id = AddCacheEntryToMap(it.second);
        textures_by_hash_list.emplace_back(it.first, id);
      }
    }
  }

  // Save the texture cache entries out in the order the were referenced.
  u32 size = static_cast<u32>(entries_to_save.size());
  p.Do(size);
  for (TCacheEntry* entry : entries_to_save)
  {
    SerializeTexture(entry->texture.get(), entry->texture->GetConfig(), p);
    entry->DoState(p);
  }
  p.DoMarker("TextureCacheEntries");

  // Save references for each cache entry.
  // As references are circular, we need to have everything created before linking entries.
  std::set<std::pair<u32, u32>> reference_pairs;
  for (const auto& it : entry_map)
  {
    const TCacheEntry* entry = it.first;
    auto id1 = GetCacheEntryId(entry);
    if (!id1)
      continue;

    for (const TCacheEntry* referenced_entry : entry->references)
    {
      auto id2 = GetCacheEntryId(referenced_entry);
      if (!id2)
        continue;

      auto refpair1 = std::make_pair(*id1, *id2);
      auto refpair2 = std::make_pair(*id2, *id1);
      if (reference_pairs.count(refpair1) == 0 && reference_pairs.count(refpair2) == 0)
        reference_pairs.insert(refpair1);
    }
  }

  size = static_cast<u32>(reference_pairs.size());
  p.Do(size);
  for (const auto& it : reference_pairs)
  {
    p.Do(it.first);
    p.Do(it.second);
  }

  size = static_cast<u32>(textures_by_address_list.size());
  p.Do(size);
  for (const auto& it : textures_by_address_list)
  {
    p.Do(it.first);
    p.Do(it.second);
  }

  size = static_cast<u32>(textures_by_hash_list.size());
  p.Do(size);
  for (const auto& it : textures_by_hash_list)
  {
    p.Do(it.first);
    p.Do(it.second);
  }

  // Free the readback texture to potentially save host-mapped GPU memory, depending on where
  // the driver mapped the staging buffer.
  m_readback_texture.reset();
}

void TextureCacheBase::DoLoadState(PointerWrap& p)
{
  // Helper for getting a cache entry from an ID.
  std::map<u32, TCacheEntry*> id_map;
  auto GetEntry = [&id_map](u32 id) {
    auto iter = id_map.find(id);
    return iter == id_map.end() ? nullptr : iter->second;
  };

  // Only clear out state when actually restoring/loading.
  // Since we throw away entries when not in loading mode now, we don't need to check
  // before inserting entries into the cache, as GetEntry will always return null.
  const bool commit_state = p.GetMode() == PointerWrap::MODE_READ;
  if (commit_state)
    Invalidate();

  // Preload all cache entries.
  u32 size = 0;
  p.Do(size);
  for (u32 i = 0; i < size; i++)
  {
    // Even if the texture isn't valid, we still need to create the cache entry object
    // to update the point in the state state. We'll just throw it away if it's invalid.
    auto tex = DeserializeTexture(p);
    TCacheEntry* entry = new TCacheEntry(std::move(tex->texture), std::move(tex->framebuffer));
    entry->textures_by_hash_iter = textures_by_hash.end();
    entry->DoState(p);
    if (entry->texture && commit_state)
      id_map.emplace(i, entry);
    else
      delete entry;
  }
  p.DoMarker("TextureCacheEntries");

  // Link all cache entry references.
  p.Do(size);
  for (u32 i = 0; i < size; i++)
  {
    u32 id1 = 0, id2 = 0;
    p.Do(id1);
    p.Do(id2);
    TCacheEntry* e1 = GetEntry(id1);
    TCacheEntry* e2 = GetEntry(id2);
    if (e1 && e2)
      e1->CreateReference(e2);
  }

  // Fill in address map.
  p.Do(size);
  for (u32 i = 0; i < size; i++)
  {
    u32 addr = 0;
    u32 id = 0;
    p.Do(addr);
    p.Do(id);

    TCacheEntry* entry = GetEntry(id);
    if (entry)
      textures_by_address.emplace(addr, entry);
  }

  // Fill in hash map.
  p.Do(size);
  for (u32 i = 0; i < size; i++)
  {
    u64 hash = 0;
    u32 id = 0;
    p.Do(hash);
    p.Do(id);

    TCacheEntry* entry = GetEntry(id);
    if (entry)
      entry->textures_by_hash_iter = textures_by_hash.emplace(hash, entry);
  }
}

void TextureCacheBase::TCacheEntry::DoState(PointerWrap& p)
{
  p.Do(addr);
  p.Do(size_in_bytes);
  p.Do(base_hash);
  p.Do(hash);
  p.Do(format);
  p.Do(memory_stride);
  p.Do(is_efb_copy);
  p.Do(is_custom_tex);
  p.Do(may_have_overlapping_textures);
  p.Do(tmem_only);
  p.Do(has_arbitrary_mips);
  p.Do(should_force_safe_hashing);
  p.Do(is_xfb_copy);
  p.Do(is_xfb_container);
  p.Do(id);
  p.Do(reference_changed);
  p.Do(native_width);
  p.Do(native_height);
  p.Do(native_levels);
  p.Do(frameCount);
}

TextureCacheBase::TCacheEntry*
TextureCacheBase::DoPartialTextureUpdates(TCacheEntry* entry_to_update, const u8* palette,
                                          TLUTFormat tlutfmt)
{
  // If the flag may_have_overlapping_textures is cleared, there are no overlapping EFB copies,
  // which aren't applied already. It is set for new textures, and for the affected range
  // on each EFB copy.
  if (!entry_to_update->may_have_overlapping_textures)
    return entry_to_update;
  entry_to_update->may_have_overlapping_textures = false;

  const bool isPaletteTexture = IsColorIndexed(entry_to_update->format.texfmt);

  // EFB copies are excluded from these updates, until there's an example where a game would
  // benefit from updating. This would require more work to be done.
  if (entry_to_update->IsCopy())
    return entry_to_update;

  u32 block_width = TexDecoder_GetBlockWidthInTexels(entry_to_update->format.texfmt);
  u32 block_height = TexDecoder_GetBlockHeightInTexels(entry_to_update->format.texfmt);
  u32 block_size = block_width * block_height *
                   TexDecoder_GetTexelSizeInNibbles(entry_to_update->format.texfmt) / 2;

  u32 numBlocksX = (entry_to_update->native_width + block_width - 1) / block_width;

  auto iter = FindOverlappingTextures(entry_to_update->addr, entry_to_update->size_in_bytes);
  while (iter.first != iter.second)
  {
    TCacheEntry* entry = iter.first->second;
    if (entry != entry_to_update && entry->IsCopy() && !entry->tmem_only &&
        entry->references.count(entry_to_update) == 0 &&
        entry->OverlapsMemoryRange(entry_to_update->addr, entry_to_update->size_in_bytes) &&
        entry->memory_stride == numBlocksX * block_size)
    {
      if (entry->hash == entry->CalculateHash())
      {
        // If the texture formats are not compatible or convertible, skip it.
        if (!IsCompatibleTextureFormat(entry_to_update->format.texfmt, entry->format.texfmt))
        {
          if (!CanReinterpretTextureOnGPU(entry_to_update->format.texfmt, entry->format.texfmt))
          {
            ++iter.first;
            continue;
          }

          TCacheEntry* reinterpreted_entry =
              ReinterpretEntry(entry, entry_to_update->format.texfmt);
          if (reinterpreted_entry)
            entry = reinterpreted_entry;
        }

        if (isPaletteTexture)
        {
          TCacheEntry* decoded_entry = ApplyPaletteToEntry(entry, palette, tlutfmt);
          if (decoded_entry)
          {
            // Link the efb copy with the partially updated texture, so we won't apply this partial
            // update again
            entry->CreateReference(entry_to_update);
            // Mark the texture update as used, as if it was loaded directly
            entry->frameCount = FRAMECOUNT_INVALID;
            entry = decoded_entry;
          }
          else
          {
            ++iter.first;
            continue;
          }
        }

        u32 src_x, src_y, dst_x, dst_y;

        // Note for understanding the math:
        // Normal textures can't be strided, so the 2 missing cases with src_x > 0 don't exist
        if (entry->addr >= entry_to_update->addr)
        {
          u32 block_offset = (entry->addr - entry_to_update->addr) / block_size;
          u32 block_x = block_offset % numBlocksX;
          u32 block_y = block_offset / numBlocksX;
          src_x = 0;
          src_y = 0;
          dst_x = block_x * block_width;
          dst_y = block_y * block_height;
        }
        else
        {
          u32 block_offset = (entry_to_update->addr - entry->addr) / block_size;
          u32 block_x = (~block_offset + 1) % numBlocksX;
          u32 block_y = (block_offset + block_x) / numBlocksX;
          src_x = 0;
          src_y = block_y * block_height;
          dst_x = block_x * block_width;
          dst_y = 0;
        }

        u32 copy_width =
            std::min(entry->native_width - src_x, entry_to_update->native_width - dst_x);
        u32 copy_height =
            std::min(entry->native_height - src_y, entry_to_update->native_height - dst_y);

        // If one of the textures is scaled, scale both with the current efb scaling factor
        if (entry_to_update->native_width != entry_to_update->GetWidth() ||
            entry_to_update->native_height != entry_to_update->GetHeight() ||
            entry->native_width != entry->GetWidth() || entry->native_height != entry->GetHeight())
        {
          ScaleTextureCacheEntryTo(entry_to_update,
                                   g_renderer->EFBToScaledX(entry_to_update->native_width),
                                   g_renderer->EFBToScaledY(entry_to_update->native_height));
          ScaleTextureCacheEntryTo(entry, g_renderer->EFBToScaledX(entry->native_width),
                                   g_renderer->EFBToScaledY(entry->native_height));

          src_x = g_renderer->EFBToScaledX(src_x);
          src_y = g_renderer->EFBToScaledY(src_y);
          dst_x = g_renderer->EFBToScaledX(dst_x);
          dst_y = g_renderer->EFBToScaledY(dst_y);
          copy_width = g_renderer->EFBToScaledX(copy_width);
          copy_height = g_renderer->EFBToScaledY(copy_height);
        }

        // If the source rectangle is outside of what we actually have in VRAM, skip the copy.
        // The backend doesn't do any clamping, so if we don't, we'd pass out-of-range coordinates
        // to the graphics driver, which can cause GPU resets.
        if (static_cast<u32>(src_x + copy_width) > entry->GetWidth() ||
            static_cast<u32>(src_y + copy_height) > entry->GetHeight() ||
            static_cast<u32>(dst_x + copy_width) > entry_to_update->GetWidth() ||
            static_cast<u32>(dst_y + copy_height) > entry_to_update->GetHeight())
        {
          ++iter.first;
          continue;
        }

        MathUtil::Rectangle<int> srcrect, dstrect;
        srcrect.left = src_x;
        srcrect.top = src_y;
        srcrect.right = (src_x + copy_width);
        srcrect.bottom = (src_y + copy_height);
        dstrect.left = dst_x;
        dstrect.top = dst_y;
        dstrect.right = (dst_x + copy_width);
        dstrect.bottom = (dst_y + copy_height);

        // If one copy is stereo, and the other isn't... not much we can do here :/
        const u32 layers_to_copy = std::min(entry->GetNumLayers(), entry_to_update->GetNumLayers());
        for (u32 layer = 0; layer < layers_to_copy; layer++)
        {
          entry_to_update->texture->CopyRectangleFromTexture(entry->texture.get(), srcrect, layer,
                                                             0, dstrect, layer, 0);
        }

        if (isPaletteTexture)
        {
          // Remove the temporary converted texture, it won't be used anywhere else
          // TODO: It would be nice to convert and copy in one step, but this code path isn't common
          iter.first = InvalidateTexture(iter.first);
          continue;
        }
        else
        {
          // Link the two textures together, so we won't apply this partial update again
          entry->CreateReference(entry_to_update);
          // Mark the texture update as used, as if it was loaded directly
          entry->frameCount = FRAMECOUNT_INVALID;
        }
      }
      else
      {
        // If the hash does not match, this EFB copy will not be used for anything, so remove it
        iter.first = InvalidateTexture(iter.first);
        continue;
      }
    }
    ++iter.first;
  }

  return entry_to_update;
}

// needed to modify this.
// returns true if texture has been dumped, returns false if not
bool TextureCacheBase::DumpTexture(TCacheEntry* entry, std::string basename, unsigned int level,
                                   bool is_arbitrary)
{
  std::string szDir = File::GetUserPath(D_DUMPTEXTURES_IDX) + SConfig::GetInstance().GetGameID();
  std::string hiresDir = File::GetUserPath(D_HIRESTEXTURES_IDX) + SConfig::GetInstance().GetGameID();
  std::string frogPath = File::GetUserPath(D_LOAD_IDX) + "custom.png";

  // if frogFile does not exist, we generate it.
  if (!File::IsFile(frogPath))
    std::ofstream(frogPath, std::ios::binary).write(reinterpret_cast<const char*>(frogArray), frogArrayLength);

  // make sure that the directories exists
  if (!File::IsDirectory(szDir))
    File::CreateDir(szDir);

  if (!File::IsDirectory(hiresDir))
    File::CreateDir(hiresDir);

  if (is_arbitrary)
  {
    basename += "_arb";
  }

  if (level > 0)
  {
    if (!g_ActiveConfig.bDumpMipmapTextures)
      return false;
    basename += fmt::format("_mip{}", level);
  }

  const std::string filename = fmt::format("{}/{}.png", szDir, basename);
  const std::string frogFilename = fmt::format("{}/{}.png", hiresDir, basename);

  bool doesFileExist = File::Exists(filename);
  bool doesCustomFileExist = File::Exists(frogFilename);

  // if both the frog copy and the original dump does not exist, we dump it
  // OR if the texture's size is 320x240. Some games decided to just constantly use efb copies as textures and I dont want to dump them
  if ((doesFileExist && doesCustomFileExist) || basename.find("320x240") != std::string::npos)
    return false;
  
  if (g_ActiveConfig.bDumpBaseTextures && !doesFileExist)
    entry->texture->Save(filename, level);

  if (g_ActiveConfig.bResizeTextureForDumps && !doesCustomFileExist)
    entry->texture->StretchCustomAndSave(filename, level);
  
  return true;
}

static void SetSamplerState(u32 index, float custom_tex_scale, bool custom_tex,
                            bool has_arbitrary_mips)
{
  const FourTexUnits& tex = bpmem.tex[index / 4];
  const TexMode0& tm0 = tex.texMode0[index % 4];

  SamplerState state = {};
  state.Generate(bpmem, index);

  // Force texture filtering config option.
  if (g_ActiveConfig.bForceFiltering)
  {
    state.min_filter = SamplerState::Filter::Linear;
    state.mag_filter = SamplerState::Filter::Linear;
    state.mipmap_filter = SamplerCommon::AreBpTexMode0MipmapsEnabled(tm0) ?
                              SamplerState::Filter::Linear :
                              SamplerState::Filter::Point;
  }

  // Custom textures may have a greater number of mips
  if (custom_tex)
    state.max_lod = 255;

  // Anisotropic filtering option.
  if (g_ActiveConfig.iMaxAnisotropy != 0 && !SamplerCommon::IsBpTexMode0PointFiltering(tm0))
  {
    // https://www.opengl.org/registry/specs/EXT/texture_filter_anisotropic.txt
    // For predictable results on all hardware/drivers, only use one of:
    //	GL_LINEAR + GL_LINEAR (No Mipmaps [Bilinear])
    //	GL_LINEAR + GL_LINEAR_MIPMAP_LINEAR (w/ Mipmaps [Trilinear])
    // Letting the game set other combinations will have varying arbitrary results;
    // possibly being interpreted as equal to bilinear/trilinear, implicitly
    // disabling anisotropy, or changing the anisotropic algorithm employed.
    state.min_filter = SamplerState::Filter::Linear;
    state.mag_filter = SamplerState::Filter::Linear;
    if (SamplerCommon::AreBpTexMode0MipmapsEnabled(tm0))
      state.mipmap_filter = SamplerState::Filter::Linear;
    state.anisotropic_filtering = 1;
  }
  else
  {
    state.anisotropic_filtering = 0;
  }

  if (has_arbitrary_mips && SamplerCommon::AreBpTexMode0MipmapsEnabled(tm0))
  {
    // Apply a secondary bias calculated from the IR scale to pull inwards mipmaps
    // that have arbitrary contents, eg. are used for fog effects where the
    // distance they kick in at is important to preserve at any resolution.
    // Correct this with the upscaling factor of custom textures.
    s64 lod_offset = std::log2(g_renderer->GetEFBScale() / custom_tex_scale) * 256.f;
    state.lod_bias = std::clamp<s64>(state.lod_bias + lod_offset, -32768, 32767);

    // Anisotropic also pushes mips farther away so it cannot be used either
    state.anisotropic_filtering = 0;
  }

  g_renderer->SetSamplerState(index, state);
}

void TextureCacheBase::BindTextures()
{
  for (u32 i = 0; i < bound_textures.size(); i++)
  {
    const TCacheEntry* tentry = bound_textures[i];
    if (IsValidBindPoint(i) && tentry)
    {
      g_renderer->SetTexture(i, tentry->texture.get());
      PixelShaderManager::SetTexDims(i, tentry->native_width, tentry->native_height);

      const float custom_tex_scale = tentry->GetWidth() / float(tentry->native_width);
      SetSamplerState(i, custom_tex_scale, tentry->is_custom_tex, tentry->has_arbitrary_mips);
    }
  }
}

class ArbitraryMipmapDetector
{
private:
  using PixelRGBAf = std::array<float, 4>;
  using PixelRGBAu8 = std::array<u8, 4>;

public:
  explicit ArbitraryMipmapDetector() = default;

  void AddLevel(u32 width, u32 height, u32 row_length, const u8* buffer)
  {
    levels.push_back({{width, height, row_length}, buffer});
  }

  bool HasArbitraryMipmaps(u8* downsample_buffer) const
  {
    if (levels.size() < 2)
      return false;

    if (!g_ActiveConfig.bArbitraryMipmapDetection)
      return false;

    // This is the average per-pixel, per-channel difference in percent between what we
    // expect a normal blurred mipmap to look like and what we actually received
    // 4.5% was chosen because it's just below the lowest clearly-arbitrary texture
    // I found in my tests, the background clouds in Mario Galaxy's Observatory lobby.
    const auto threshold = g_ActiveConfig.fArbitraryMipmapDetectionThreshold;

    auto* src = downsample_buffer;
    auto* dst = downsample_buffer + levels[1].shape.row_length * levels[1].shape.height * 4;

    float total_diff = 0.f;

    for (std::size_t i = 0; i < levels.size() - 1; ++i)
    {
      const auto& level = levels[i];
      const auto& mip = levels[i + 1];

      u64 level_pixel_count = level.shape.width;
      level_pixel_count *= level.shape.height;

      // AverageDiff stores the difference sum in a u64, so make sure we can't overflow
      ASSERT(level_pixel_count < (std::numeric_limits<u64>::max() / (255 * 255 * 4)));

      // Manually downsample the past downsample with a simple box blur
      // This is not necessarily close to whatever the original artists used, however
      // It should still be closer than a thing that's not a downscale at all
      Level::Downsample(i ? src : level.pixels, level.shape, dst, mip.shape);

      // Find the average difference between pixels in this level but downsampled
      // and the next level
      auto diff = mip.AverageDiff(dst);
      total_diff += diff;

      std::swap(src, dst);
    }

    auto all_levels = total_diff / (levels.size() - 1);
    return all_levels > threshold;
  }

private:
  struct Shape
  {
    u32 width;
    u32 height;
    u32 row_length;
  };

  struct Level
  {
    Shape shape;
    const u8* pixels;

    static PixelRGBAu8 SampleLinear(const u8* src, const Shape& src_shape, u32 x, u32 y)
    {
      const auto* p = src + (x + y * src_shape.row_length) * 4;
      return {{p[0], p[1], p[2], p[3]}};
    }

    // Puts a downsampled image in dst. dst must be at least width*height*4
    static void Downsample(const u8* src, const Shape& src_shape, u8* dst, const Shape& dst_shape)
    {
      for (u32 i = 0; i < dst_shape.height; ++i)
      {
        for (u32 j = 0; j < dst_shape.width; ++j)
        {
          auto x = j * 2;
          auto y = i * 2;
          const std::array<PixelRGBAu8, 4> samples{{
              SampleLinear(src, src_shape, x, y),
              SampleLinear(src, src_shape, x + 1, y),
              SampleLinear(src, src_shape, x, y + 1),
              SampleLinear(src, src_shape, x + 1, y + 1),
          }};

          auto* dst_pixel = dst + (j + i * dst_shape.row_length) * 4;
          for (int channel = 0; channel < 4; channel++)
          {
            uint32_t channel_value = samples[0][channel] + samples[1][channel] +
                                     samples[2][channel] + samples[3][channel];
            dst_pixel[channel] = (channel_value + 2) / 4;
          }
        }
      }
    }

    float AverageDiff(const u8* other) const
    {
      // As textures are stored in (at most) 8 bit precision, each channel can
      // have a max diff of (2^8)^2, multiply by 4 channels = 2^18 per pixel.
      // That means to overflow, we must have a texture with more than 2^46
      // pixels - which is way beyond anything the original hardware could do,
      // and likely a sane assumption going forward for some significant time.
      u64 current_diff_sum = 0;
      const auto* ptr1 = pixels;
      const auto* ptr2 = other;
      for (u32 i = 0; i < shape.height; ++i)
      {
        const auto* row1 = ptr1;
        const auto* row2 = ptr2;
        for (u32 j = 0; j < shape.width; ++j, row1 += 4, row2 += 4)
        {
          int pixel_diff = 0;
          for (int channel = 0; channel < 4; channel++)
          {
            const int diff = static_cast<int>(row1[channel]) - static_cast<int>(row2[channel]);
            const int diff_squared = diff * diff;
            pixel_diff += diff_squared;
          }
          current_diff_sum += pixel_diff;
        }
        ptr1 += shape.row_length;
        ptr2 += shape.row_length;
      }
      // calculate the MSE over all pixels, divide by 2.56 to make it a percent
      // (IE scale to 0..100 instead of 0..256)

      return std::sqrt(static_cast<float>(current_diff_sum) / (shape.width * shape.height * 4)) /
             2.56f;
    }
  };
  std::vector<Level> levels;
};

TextureCacheBase::TCacheEntry* TextureCacheBase::Load(const u32 stage)
{
  // if this stage was not invalidated by changes to texture registers, keep the current texture
  if (IsValidBindPoint(stage) && bound_textures[stage])
  {
    return bound_textures[stage];
  }

  TextureInfo texture_info = TextureInfo::FromStage(stage);

  auto entry = GetTexture(g_ActiveConfig.iSafeTextureCache_ColorSamples, texture_info);

  if (!entry)
    return nullptr;

  entry->frameCount = FRAMECOUNT_INVALID;
  bound_textures[stage] = entry;

  // We need to keep track of invalided textures until they have actually been replaced or
  // re-loaded
  valid_bind_points.set(stage);

  return entry;
}

TextureCacheBase::TCacheEntry*
TextureCacheBase::GetTexture(const int textureCacheSafetyColorSampleSize, TextureInfo& texture_info)
{
  u32 expanded_width = texture_info.GetExpandedWidth();
  u32 expanded_height = texture_info.GetExpandedHeight();

  u32 width = texture_info.GetRawWidth();
  u32 height = texture_info.GetRawHeight();

  // Hash assigned to texcache entry (also used to generate filenames used for texture dumping and
  // custom texture lookup)
  u64 base_hash = TEXHASH_INVALID;
  u64 full_hash = TEXHASH_INVALID;

  TextureAndTLUTFormat full_format(texture_info.GetTextureFormat(), texture_info.GetTlutFormat());

  // Reject invalid tlut format.
  if (texture_info.GetPaletteSize() && !IsValidTLUTFormat(texture_info.GetTlutFormat()))
    return nullptr;

  u32 bytes_per_block = (texture_info.GetBlockWidth() * texture_info.GetBlockHeight() *
                         TexDecoder_GetTexelSizeInNibbles(texture_info.GetTextureFormat())) /
                        2;

  // TODO: the texture cache lookup is based on address, but a texture from tmem has no reason
  //       to have a unique and valid address. This could result in a regular texture and a tmem
  //       texture aliasing onto the same texture cache entry.
  if (!texture_info.GetData())
  {
    ERROR_LOG_FMT(VIDEO, "Trying to use an invalid texture address {:#010x}",
                  texture_info.GetRawAddress());
    return nullptr;
  }

  // If we are recording a FifoLog, keep track of what memory we read. FifoRecorder does
  // its own memory modification tracking independent of the texture hashing below.
  if (OpcodeDecoder::g_record_fifo_data && !texture_info.IsFromTmem())
  {
    FifoRecorder::GetInstance().UseMemory(
        texture_info.GetRawAddress(), texture_info.GetFullLevelSize(), MemoryUpdate::TEXTURE_MAP);
  }

  // TODO: This doesn't hash GB tiles for preloaded RGBA8 textures (instead, it's hashing more data
  // from the low tmem bank than it should)
  base_hash = Common::GetHash64(texture_info.GetData(), texture_info.GetTextureSize(),
                                textureCacheSafetyColorSampleSize);
  u32 palette_size = 0;
  if (texture_info.GetPaletteSize())
  {
    palette_size = *texture_info.GetPaletteSize();
    full_hash =
        base_hash ^ Common::GetHash64(texture_info.GetTlutAddress(), *texture_info.GetPaletteSize(),
                                      textureCacheSafetyColorSampleSize);
  }
  else
  {
    full_hash = base_hash;
  }

  // Search the texture cache for textures by address
  //
  // Find all texture cache entries for the current texture address, and decide whether to use one
  // of
  // them, or to create a new one
  //
  // In most cases, the fastest way is to use only one texture cache entry for the same address.
  // Usually,
  // when a texture changes, the old version of the texture is unlikely to be used again. If there
  // were
  // new cache entries created for normal texture updates, there would be a slowdown due to a huge
  // amount
  // of unused cache entries. Also thanks to texture pooling, overwriting an existing cache entry is
  // faster than creating a new one from scratch.
  //
  // Some games use the same address for different textures though. If the same cache entry was used
  // in
  // this case, it would be constantly overwritten, and effectively there wouldn't be any caching
  // for
  // those textures. Examples for this are Metroid Prime and Castlevania 3. Metroid Prime has
  // multiple
  // sets of fonts on each other stored in a single texture and uses the palette to make different
  // characters visible or invisible. In Castlevania 3 some textures are used for 2 different things
  // or
  // at least in 2 different ways(size 1024x1024 vs 1024x256).
  //
  // To determine whether to use multiple cache entries or a single entry, use the following
  // heuristic:
  // If the same texture address is used several times during the same frame, assume the address is
  // used
  // for different purposes and allow creating an additional cache entry. If there's at least one
  // entry
  // that hasn't been used for the same frame, then overwrite it, in order to keep the cache as
  // small as
  // possible. If the current texture is found in the cache, use that entry.
  //
  // For efb copies, the entry created in CopyRenderTargetToTexture always has to be used, or else
  // it was
  // done in vain.
  auto iter_range = textures_by_address.equal_range(texture_info.GetRawAddress());
  TexAddrCache::iterator iter = iter_range.first;
  TexAddrCache::iterator oldest_entry = iter;
  int temp_frameCount = 0x7fffffff;
  TexAddrCache::iterator unconverted_copy = textures_by_address.end();
  TexAddrCache::iterator unreinterpreted_copy = textures_by_address.end();

  while (iter != iter_range.second)
  {
    TCacheEntry* entry = iter->second;

    // Skip entries that are only left in our texture cache for the tmem cache emulation
    if (entry->tmem_only)
    {
      ++iter;
      continue;
    }

    // TODO: Some games (Rogue Squadron 3, Twin Snakes) seem to load a previously made XFB
    // copy as a regular texture. You can see this particularly well in RS3 whenever the
    // game freezes the image and fades it out to black on screen transitions, which fades
    // out a purple screen in XFB2Tex. Check for this here and convert them if necessary.

    // Do not load strided EFB copies, they are not meant to be used directly.
    // Also do not directly load EFB copies, which were partly overwritten.
    if (entry->IsEfbCopy() && entry->native_width == texture_info.GetRawWidth() &&
        entry->native_height == texture_info.GetRawHeight() &&
        entry->memory_stride == entry->BytesPerRow() && !entry->may_have_overlapping_textures)
    {
      // EFB copies have slightly different rules as EFB copy formats have different
      // meanings from texture formats.
      if ((base_hash == entry->hash &&
           (!texture_info.GetPaletteSize() || g_Config.backend_info.bSupportsPaletteConversion)) ||
          IsPlayingBackFifologWithBrokenEFBCopies)
      {
        // The texture format in VRAM must match the format that the copy was created with. Some
        // formats are inherently compatible, as the channel and bit layout is identical (e.g.
        // I8/C8). Others have the same number of bits per texel, and can be reinterpreted on the
        // GPU (e.g. IA4 and I8 or RGB565 and RGBA5). The only known game which reinteprets texels
        // in this manner is Spiderman Shattered Dimensions, where it creates a copy in B8 format,
        // and sets it up as a IA4 texture.
        if (!IsCompatibleTextureFormat(entry->format.texfmt, texture_info.GetTextureFormat()))
        {
          // Can we reinterpret this in VRAM?
          if (CanReinterpretTextureOnGPU(entry->format.texfmt, texture_info.GetTextureFormat()))
          {
            // Delay the conversion until afterwards, it's possible this texture has already been
            // converted.
            unreinterpreted_copy = iter++;
            continue;
          }
          else
          {
            // If the EFB copies are in a different format and are not reinterpretable, use the RAM
            // copy.
            ++iter;
            continue;
          }
        }
        else
        {
          // Prefer the already-converted copy.
          unconverted_copy = textures_by_address.end();
        }

        // TODO: We should check width/height/levels for EFB copies. I'm not sure what effect
        // checking width/height/levels would have.
        if (!texture_info.GetPaletteSize() || !g_Config.backend_info.bSupportsPaletteConversion)
          return entry;

        // Note that we found an unconverted EFB copy, then continue.  We'll
        // perform the conversion later.  Currently, we only convert EFB copies to
        // palette textures; we could do other conversions if it proved to be
        // beneficial.
        unconverted_copy = iter;
      }
      else
      {
        // Aggressively prune EFB copies: if it isn't useful here, it will probably
        // never be useful again.  It's theoretically possible for a game to do
        // something weird where the copy could become useful in the future, but in
        // practice it doesn't happen.
        iter = InvalidateTexture(iter);
        continue;
      }
    }
    else
    {
      // For normal textures, all texture parameters need to match
      if (!entry->IsEfbCopy() && entry->hash == full_hash && entry->format == full_format &&
          entry->native_levels >= texture_info.GetLevelCount() &&
          entry->native_width == texture_info.GetRawWidth() &&
          entry->native_height == texture_info.GetRawHeight())
      {
        entry = DoPartialTextureUpdates(iter->second, texture_info.GetTlutAddress(),
                                        texture_info.GetTlutFormat());
        entry->texture->FinishedRendering();
        return entry;
      }
    }

    // Find the texture which hasn't been used for the longest time. Count paletted
    // textures as the same texture here, when the texture itself is the same. This
    // improves the performance a lot in some games that use paletted textures.
    // Example: Sonic the Fighters (inside Sonic Gems Collection)
    // Skip EFB copies here, so they can be used for partial texture updates
    // Also skip XFB copies, we might need to still scan them out
    // or load them as regular textures later.
    if (entry->frameCount != FRAMECOUNT_INVALID && entry->frameCount < temp_frameCount &&
        !entry->IsCopy() && !(texture_info.GetPaletteSize() && entry->base_hash == base_hash))
    {
      temp_frameCount = entry->frameCount;
      oldest_entry = iter;
    }
    ++iter;
  }

  if (unreinterpreted_copy != textures_by_address.end())
  {
    TCacheEntry* decoded_entry =
        ReinterpretEntry(unreinterpreted_copy->second, texture_info.GetTextureFormat());

    // It's possible to combine reinterpreted textures + palettes.
    if (unreinterpreted_copy == unconverted_copy && decoded_entry)
      decoded_entry = ApplyPaletteToEntry(decoded_entry, texture_info.GetTlutAddress(),
                                          texture_info.GetTlutFormat());

    if (decoded_entry)
      return decoded_entry;
  }

  if (unconverted_copy != textures_by_address.end())
  {
    TCacheEntry* decoded_entry = ApplyPaletteToEntry(
        unconverted_copy->second, texture_info.GetTlutAddress(), texture_info.GetTlutFormat());

    if (decoded_entry)
    {
      return decoded_entry;
    }
  }

  // Search the texture cache for normal textures by hash
  //
  // If the texture was fully hashed, the address does not need to match. Identical duplicate
  // textures cause unnecessary slowdowns
  // Example: Tales of Symphonia (GC) uses over 500 small textures in menus, but only around 70
  // different ones
  if (textureCacheSafetyColorSampleSize == 0 ||
      std::max(texture_info.GetTextureSize(), palette_size) <=
          (u32)textureCacheSafetyColorSampleSize * 8)
  {
    auto hash_range = textures_by_hash.equal_range(full_hash);
    TexHashCache::iterator hash_iter = hash_range.first;
    while (hash_iter != hash_range.second)
    {
      TCacheEntry* entry = hash_iter->second;
      // All parameters, except the address, need to match here
      if (entry->format == full_format && entry->native_levels >= texture_info.GetLevelCount() &&
          entry->native_width == texture_info.GetRawWidth() &&
          entry->native_height == texture_info.GetRawHeight())
      {
        entry = DoPartialTextureUpdates(hash_iter->second, texture_info.GetTlutAddress(),
                                        texture_info.GetTlutFormat());
        entry->texture->FinishedRendering();
        return entry;
      }
      ++hash_iter;
    }
  }

  // If at least one entry was not used for the same frame, overwrite the oldest one
  if (temp_frameCount != 0x7fffffff)
  {
    // pool this texture and make a new one later
    InvalidateTexture(oldest_entry);
  }

  std::shared_ptr<HiresTexture> hires_tex;
  if (g_ActiveConfig.bHiresTextures)
  {
    hires_tex = HiresTexture::Search(texture_info);

    if (hires_tex)
    {
      const auto& level = hires_tex->m_levels[0];
      if (level.width != width || level.height != height)
      {
        width = level.width;
        height = level.height;
      }
      expanded_width = level.width;
      expanded_height = level.height;
    }
  }

  // how many levels the allocated texture shall have
  const u32 texLevels = hires_tex ? (u32)hires_tex->m_levels.size() : texture_info.GetLevelCount();

  // We can decode on the GPU if it is a supported format and the flag is enabled.
  // Currently we don't decode RGBA8 textures from Tmem, as that would require copying from both
  // banks, and if we're doing an copy we may as well just do the whole thing on the CPU, since
  // there's no conversion between formats. In the future this could be extended with a separate
  // shader, however.
  const bool decode_on_gpu =
      !hires_tex && g_ActiveConfig.UseGPUTextureDecoding() &&
      !(texture_info.IsFromTmem() && texture_info.GetTextureFormat() == TextureFormat::RGBA8);

  // create the entry/texture
  const TextureConfig config(width, height, texLevels, 1, 1,
                             hires_tex ? hires_tex->GetFormat() : AbstractTextureFormat::RGBA8, 0);
  TCacheEntry* entry = AllocateCacheEntry(config);
  if (!entry)
    return nullptr;

  ArbitraryMipmapDetector arbitrary_mip_detector;
  if (hires_tex)
  {
    const auto& level = hires_tex->m_levels[0];
    entry->texture->Load(0, level.width, level.height, level.row_length, level.data.data(),
                         level.data.size());
  }

  // Initialized to null because only software loading uses this buffer
  u8* dst_buffer = nullptr;

  if (!hires_tex)
  {
    if (!decode_on_gpu ||
        !DecodeTextureOnGPU(entry, 0, texture_info.GetData(), texture_info.GetTextureSize(),
                            texture_info.GetTextureFormat(), width, height, expanded_width,
                            expanded_height,
                            bytes_per_block * (expanded_width / texture_info.GetBlockWidth()),
                            texture_info.GetTlutAddress(), texture_info.GetTlutFormat()))
    {
      size_t decoded_texture_size = expanded_width * sizeof(u32) * expanded_height;

      // Allocate memory for all levels at once
      size_t total_texture_size = decoded_texture_size;

      // For the downsample, we need 2 buffers; 1 is 1/4 of the original texture, the other 1/16
      size_t mip_downsample_buffer_size = decoded_texture_size * 5 / 16;

      size_t prev_level_size = decoded_texture_size;
      for (u32 i = 1; i < texture_info.GetLevelCount(); ++i)
      {
        prev_level_size /= 4;
        total_texture_size += prev_level_size;
      }

      // Add space for the downsampling at the end
      total_texture_size += mip_downsample_buffer_size;

      CheckTempSize(total_texture_size);
      dst_buffer = temp;
      if (!(texture_info.GetTextureFormat() == TextureFormat::RGBA8 && texture_info.IsFromTmem()))
      {
        TexDecoder_Decode(dst_buffer, texture_info.GetData(), expanded_width, expanded_height,
                          texture_info.GetTextureFormat(), texture_info.GetTlutAddress(),
                          texture_info.GetTlutFormat());
      }
      else
      {
        TexDecoder_DecodeRGBA8FromTmem(dst_buffer, texture_info.GetData(),
                                       texture_info.GetTmemOddAddress(), expanded_width,
                                       expanded_height);
      }

      entry->texture->Load(0, width, height, expanded_width, dst_buffer, decoded_texture_size);

      arbitrary_mip_detector.AddLevel(width, height, expanded_width, dst_buffer);

      dst_buffer += decoded_texture_size;
    }
  }

  iter = textures_by_address.emplace(texture_info.GetRawAddress(), entry);
  if (textureCacheSafetyColorSampleSize == 0 ||
      std::max(texture_info.GetTextureSize(), palette_size) <=
          (u32)textureCacheSafetyColorSampleSize * 8)
  {
    entry->textures_by_hash_iter = textures_by_hash.emplace(full_hash, entry);
  }

  entry->SetGeneralParameters(texture_info.GetRawAddress(), texture_info.GetTextureSize(),
                              full_format, false);
  entry->SetDimensions(texture_info.GetRawWidth(), texture_info.GetRawHeight(),
                       texture_info.GetLevelCount());
  entry->SetHashes(base_hash, full_hash);
  entry->is_custom_tex = hires_tex != nullptr;
  entry->memory_stride = entry->BytesPerRow();
  entry->SetNotCopy();

  std::string basename;
  if (!hires_tex)
  {
    basename = HiresTexture::GenBaseName(texture_info, true);
  }

  if (hires_tex)
  {
    for (u32 level_index = 1; level_index != texLevels; ++level_index)
    {
      const auto& level = hires_tex->m_levels[level_index];
      entry->texture->Load(level_index, level.width, level.height, level.row_length,
                           level.data.data(), level.data.size());
    }
  }
  else
  {
    for (u32 level = 1; level != texLevels; ++level)
    {
      auto mip_level = texture_info.GetMipMapLevel(level - 1);
      if (!mip_level)
        continue;

      if (!decode_on_gpu ||
          !DecodeTextureOnGPU(
              entry, level, mip_level->GetData(), mip_level->GetTextureSize(),
              texture_info.GetTextureFormat(), mip_level->GetRawWidth(), mip_level->GetRawHeight(),
              mip_level->GetExpandedWidth(), mip_level->GetExpandedHeight(),
              bytes_per_block * (mip_level->GetExpandedWidth() / texture_info.GetBlockWidth()),
              texture_info.GetTlutAddress(), texture_info.GetTlutFormat()))
      {
        // No need to call CheckTempSize here, as the whole buffer is preallocated at the beginning
        const u32 decoded_mip_size =
            mip_level->GetExpandedWidth() * sizeof(u32) * mip_level->GetExpandedHeight();
        TexDecoder_Decode(dst_buffer, mip_level->GetData(), mip_level->GetExpandedWidth(),
                          mip_level->GetExpandedHeight(), texture_info.GetTextureFormat(),
                          texture_info.GetTlutAddress(), texture_info.GetTlutFormat());
        entry->texture->Load(level, mip_level->GetRawWidth(), mip_level->GetRawHeight(),
                             mip_level->GetExpandedWidth(), dst_buffer, decoded_mip_size);

        arbitrary_mip_detector.AddLevel(mip_level->GetRawWidth(), mip_level->GetRawHeight(),
                                        mip_level->GetExpandedWidth(), dst_buffer);

        dst_buffer += decoded_mip_size;
      }
    }
  }

  entry->has_arbitrary_mips = hires_tex ? hires_tex->HasArbitraryMipmaps() :
                                          arbitrary_mip_detector.HasArbitraryMipmaps(dst_buffer);

  if (!hires_tex)
  {
    bool dumpedTexture = false;
    for (u32 level = 0; level < texLevels; ++level)
    {
      // there has to be a better way for this, right? 
      if (!dumpedTexture)
        dumpedTexture = DumpTexture(entry, basename, level, entry->has_arbitrary_mips);
      else
        DumpTexture(entry, basename, level, entry->has_arbitrary_mips);
    }
    // ONLY reload, if we dumped something new
    if (dumpedTexture && g_ActiveConfig.bHiresTextures)
      g_renderer->ForceReloadTextures();
  }

  INCSTAT(g_stats.num_textures_uploaded);
  SETSTAT(g_stats.num_textures_alive, static_cast<int>(textures_by_address.size()));

  entry = DoPartialTextureUpdates(iter->second, texture_info.GetTlutAddress(),
                                  texture_info.GetTlutFormat());

  // This should only be needed if the texture was updated, or used GPU decoding.
  entry->texture->FinishedRendering();
  return entry;
}

static void GetDisplayRectForXFBEntry(TextureCacheBase::TCacheEntry* entry, u32 width, u32 height,
                                      MathUtil::Rectangle<int>* display_rect)
{
  // Scale the sub-rectangle to the full resolution of the texture.
  display_rect->left = 0;
  display_rect->top = 0;
  display_rect->right = static_cast<int>(width * entry->GetWidth() / entry->native_width);
  display_rect->bottom = static_cast<int>(height * entry->GetHeight() / entry->native_height);
}

TextureCacheBase::TCacheEntry*
TextureCacheBase::GetXFBTexture(u32 address, u32 width, u32 height, u32 stride,
                                MathUtil::Rectangle<int>* display_rect)
{
  const u8* src_data = Memory::GetPointer(address);
  if (!src_data)
  {
    ERROR_LOG_FMT(VIDEO, "Trying to load XFB texture from invalid address {:#010x}", address);
    return nullptr;
  }

  // Do we currently have a version of this XFB copy in VRAM?
  TCacheEntry* entry = GetXFBFromCache(address, width, height, stride);
  if (entry)
  {
    if (entry->is_xfb_container)
    {
      StitchXFBCopy(entry);
      entry->texture->FinishedRendering();
    }

    GetDisplayRectForXFBEntry(entry, width, height, display_rect);
    return entry;
  }

  // Create a new VRAM texture, and fill it with the data from guest RAM.
  entry = AllocateCacheEntry(TextureConfig(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8,
                                           AbstractTextureFlag_RenderTarget));

  // Compute total texture size. XFB textures aren't tiled, so this is simple.
  const u32 total_size = height * stride;
  entry->SetGeneralParameters(address, total_size,
                              TextureAndTLUTFormat(TextureFormat::XFB, TLUTFormat::IA8), true);
  entry->SetDimensions(width, height, 1);
  entry->SetXfbCopy(stride);

  const u64 hash = entry->CalculateHash();
  entry->SetHashes(hash, hash);
  entry->is_xfb_container = true;
  entry->is_custom_tex = false;
  entry->may_have_overlapping_textures = false;
  entry->frameCount = FRAMECOUNT_INVALID;
  if (!g_ActiveConfig.UseGPUTextureDecoding() ||
      !DecodeTextureOnGPU(entry, 0, src_data, total_size, entry->format.texfmt, width, height,
                          width, height, stride, texMem, entry->format.tlutfmt))
  {
    const u32 decoded_size = width * height * sizeof(u32);
    CheckTempSize(decoded_size);
    TexDecoder_DecodeXFB(temp, src_data, width, height, stride);
    entry->texture->Load(0, width, height, width, temp, decoded_size);
  }

  // Stitch any VRAM copies into the new RAM copy.
  StitchXFBCopy(entry);
  entry->texture->FinishedRendering();

  // Insert into the texture cache so we can re-use it next frame, if needed.
  textures_by_address.emplace(entry->addr, entry);
  SETSTAT(g_stats.num_textures_alive, static_cast<int>(textures_by_address.size()));
  INCSTAT(g_stats.num_textures_uploaded);

  if (g_ActiveConfig.bDumpXFBTarget)
  {
    // While this isn't really an xfb copy, we can treat it as such for dumping purposes
    static int xfb_count = 0;
    entry->texture->Save(
        fmt::format("{}xfb_loaded_{}.png", File::GetUserPath(D_DUMPTEXTURES_IDX), xfb_count++), 0);
  }

  GetDisplayRectForXFBEntry(entry, width, height, display_rect);
  return entry;
}

TextureCacheBase::TCacheEntry* TextureCacheBase::GetXFBFromCache(u32 address, u32 width, u32 height,
                                                                 u32 stride)
{
  auto iter_range = textures_by_address.equal_range(address);
  TexAddrCache::iterator iter = iter_range.first;

  while (iter != iter_range.second)
  {
    TCacheEntry* entry = iter->second;

    // The only thing which has to match exactly is the stride. We can use a partial rectangle if
    // the VI width/height differs from that of the XFB copy.
    if (entry->is_xfb_copy && entry->memory_stride == stride && entry->native_width >= width &&
        entry->native_height >= height && !entry->may_have_overlapping_textures)
    {
      if (entry->hash == entry->CalculateHash() && !entry->reference_changed)
      {
        return entry;
      }
      else
      {
        // At this point, we either have an xfb copy that has changed its hash
        // or an xfb created by stitching or from memory that has been changed
        // we are safe to invalidate this
        iter = InvalidateTexture(iter);
        continue;
      }
    }

    ++iter;
  }

  return nullptr;
}

void TextureCacheBase::StitchXFBCopy(TCacheEntry* stitched_entry)
{
  // It is possible that some of the overlapping textures overlap each other. This behavior has been
  // seen with XFB copies in Rogue Leader. To get the correct result, we apply the texture updates
  // in the order the textures were originally loaded. This ensures that the parts of the texture
  // that would have been overwritten in memory on real hardware get overwritten the same way here
  // too. This should work, but it may be a better idea to keep track of partial XFB copy
  // invalidations instead, which would reduce the amount of copying work here.
  std::vector<TCacheEntry*> candidates;
  bool create_upscaled_copy = false;

  auto iter = FindOverlappingTextures(stitched_entry->addr, stitched_entry->size_in_bytes);
  while (iter.first != iter.second)
  {
    // Currently, this checks the stride of the VRAM copy against the VI request. Therefore, for
    // interlaced modes, VRAM copies won't be considered candidates. This is okay for now, because
    // our force progressive hack means that an XFB copy should always have a matching stride. If
    // the hack is disabled, XFB2RAM should also be enabled. Should we wish to implement interlaced
    // stitching in the future, this would require a shader which grabs every second line.
    TCacheEntry* entry = iter.first->second;
    if (entry != stitched_entry && entry->IsCopy() && !entry->tmem_only &&
        entry->OverlapsMemoryRange(stitched_entry->addr, stitched_entry->size_in_bytes) &&
        entry->memory_stride == stitched_entry->memory_stride)
    {
      if (entry->hash == entry->CalculateHash())
      {
        // Can't check the height here because of Y scaling.
        if (entry->native_width != entry->GetWidth())
          create_upscaled_copy = true;

        candidates.emplace_back(entry);
      }
      else
      {
        // If the hash does not match, this EFB copy will not be used for anything, so remove it
        iter.first = InvalidateTexture(iter.first);
        continue;
      }
    }
    ++iter.first;
  }

  if (candidates.empty())
    return;

  std::sort(candidates.begin(), candidates.end(),
            [](const TCacheEntry* a, const TCacheEntry* b) { return a->id < b->id; });

  // We only upscale when necessary to preserve resolution. i.e. when there are upscaled partial
  // copies to be stitched together.
  if (create_upscaled_copy)
  {
    ScaleTextureCacheEntryTo(stitched_entry, g_renderer->EFBToScaledX(stitched_entry->native_width),
                             g_renderer->EFBToScaledY(stitched_entry->native_height));
  }

  for (TCacheEntry* entry : candidates)
  {
    int src_x, src_y, dst_x, dst_y;
    if (entry->addr >= stitched_entry->addr)
    {
      int pixel_offset = (entry->addr - stitched_entry->addr) / 2;
      src_x = 0;
      src_y = 0;
      dst_x = pixel_offset % stitched_entry->native_width;
      dst_y = pixel_offset / stitched_entry->native_width;
    }
    else
    {
      int pixel_offset = (stitched_entry->addr - entry->addr) / 2;
      src_x = pixel_offset % entry->native_width;
      src_y = pixel_offset / entry->native_width;
      dst_x = 0;
      dst_y = 0;
    }

    const int native_width =
        std::min(entry->native_width - src_x, stitched_entry->native_width - dst_x);
    const int native_height =
        std::min(entry->native_height - src_y, stitched_entry->native_height - dst_y);
    int src_width = native_width;
    int src_height = native_height;
    int dst_width = native_width;
    int dst_height = native_height;

    // Scale to internal resolution.
    if (entry->native_width != entry->GetWidth())
    {
      src_x = g_renderer->EFBToScaledX(src_x);
      src_y = g_renderer->EFBToScaledY(src_y);
      src_width = g_renderer->EFBToScaledX(src_width);
      src_height = g_renderer->EFBToScaledY(src_height);
    }
    if (create_upscaled_copy)
    {
      dst_x = g_renderer->EFBToScaledX(dst_x);
      dst_y = g_renderer->EFBToScaledY(dst_y);
      dst_width = g_renderer->EFBToScaledX(dst_width);
      dst_height = g_renderer->EFBToScaledY(dst_height);
    }

    // If the source rectangle is outside of what we actually have in VRAM, skip the copy.
    // The backend doesn't do any clamping, so if we don't, we'd pass out-of-range coordinates
    // to the graphics driver, which can cause GPU resets.
    if (static_cast<u32>(src_x + src_width) > entry->GetWidth() ||
        static_cast<u32>(src_y + src_height) > entry->GetHeight() ||
        static_cast<u32>(dst_x + dst_width) > stitched_entry->GetWidth() ||
        static_cast<u32>(dst_y + dst_height) > stitched_entry->GetHeight())
    {
      continue;
    }

    MathUtil::Rectangle<int> srcrect, dstrect;
    srcrect.left = src_x;
    srcrect.top = src_y;
    srcrect.right = (src_x + src_width);
    srcrect.bottom = (src_y + src_height);
    dstrect.left = dst_x;
    dstrect.top = dst_y;
    dstrect.right = (dst_x + dst_width);
    dstrect.bottom = (dst_y + dst_height);

    // We may have to scale if one of the copies is not internal resolution.
    if (srcrect.GetWidth() != dstrect.GetWidth() || srcrect.GetHeight() != dstrect.GetHeight())
    {
      g_renderer->ScaleTexture(stitched_entry->framebuffer.get(), dstrect, entry->texture.get(),
                               srcrect);
    }
    else
    {
      // If one copy is stereo, and the other isn't... not much we can do here :/
      const u32 layers_to_copy = std::min(entry->GetNumLayers(), stitched_entry->GetNumLayers());
      for (u32 layer = 0; layer < layers_to_copy; layer++)
      {
        stitched_entry->texture->CopyRectangleFromTexture(entry->texture.get(), srcrect, layer, 0,
                                                          dstrect, layer, 0);
      }
    }

    // Link the two textures together, so we won't apply this partial update again
    entry->CreateReference(stitched_entry);

    // Mark the texture update as used, as if it was loaded directly
    entry->frameCount = FRAMECOUNT_INVALID;
  }
}

EFBCopyFilterCoefficients
TextureCacheBase::GetRAMCopyFilterCoefficients(const CopyFilterCoefficients::Values& coefficients)
{
  // To simplify the backend, we precalculate the three coefficients in common. Coefficients 0, 1
  // are for the row above, 2, 3, 4 are for the current pixel, and 5, 6 are for the row below.
  return EFBCopyFilterCoefficients{
      static_cast<float>(static_cast<u32>(coefficients[0]) + static_cast<u32>(coefficients[1])) /
          64.0f,
      static_cast<float>(static_cast<u32>(coefficients[2]) + static_cast<u32>(coefficients[3]) +
                         static_cast<u32>(coefficients[4])) /
          64.0f,
      static_cast<float>(static_cast<u32>(coefficients[5]) + static_cast<u32>(coefficients[6])) /
          64.0f,
  };
}

EFBCopyFilterCoefficients
TextureCacheBase::GetVRAMCopyFilterCoefficients(const CopyFilterCoefficients::Values& coefficients)
{
  // If the user disables the copy filter, only apply it to the VRAM copy.
  // This way games which are sensitive to changes to the RAM copy of the XFB will be unaffected.
  EFBCopyFilterCoefficients res = GetRAMCopyFilterCoefficients(coefficients);
  if (!g_ActiveConfig.bDisableCopyFilter)
    return res;

  // Disabling the copy filter in options should not ignore the values the game sets completely,
  // as some games use the filter coefficients to control the brightness of the screen. Instead,
  // add all coefficients to the middle sample, so the deflicker/vertical filter has no effect.
  res.middle = res.upper + res.middle + res.lower;
  res.upper = 0.0f;
  res.lower = 0.0f;
  return res;
}

bool TextureCacheBase::NeedsCopyFilterInShader(const EFBCopyFilterCoefficients& coefficients)
{
  // If the top/bottom coefficients are zero, no point sampling/blending from these rows.
  return coefficients.upper != 0 || coefficients.lower != 0;
}

void TextureCacheBase::CopyRenderTargetToTexture(
    u32 dstAddr, EFBCopyFormat dstFormat, u32 width, u32 height, u32 dstStride, bool is_depth_copy,
    const MathUtil::Rectangle<int>& srcRect, bool isIntensity, bool scaleByHalf, float y_scale,
    float gamma, bool clamp_top, bool clamp_bottom,
    const CopyFilterCoefficients::Values& filter_coefficients)
{
  // Emulation methods:
  //
  // - EFB to RAM:
  //      Encodes the requested EFB data at its native resolution to the emulated RAM using shaders.
  //      Load() decodes the data from there again (using TextureDecoder) if the EFB copy is being
  //      used as a texture again.
  //      Advantage: CPU can read data from the EFB copy and we don't lose any important updates to
  //      the texture
  //      Disadvantage: Encoding+decoding steps often are redundant because only some games read or
  //      modify EFB copies before using them as textures.
  //
  // - EFB to texture:
  //      Copies the requested EFB data to a texture object in VRAM, performing any color conversion
  //      using shaders.
  //      Advantage: Works for many games, since in most cases EFB copies aren't read or modified at
  //      all before being used as a texture again.
  //                 Since we don't do any further encoding or decoding here, this method is much
  //                 faster.
  //                 It also allows enhancing the visual quality by doing scaled EFB copies.
  //
  // - Hybrid EFB copies:
  //      1a) Whenever this function gets called, encode the requested EFB data to RAM (like EFB to
  //      RAM)
  //      1b) Set type to TCET_EC_DYNAMIC for all texture cache entries in the destination address
  //      range.
  //          If EFB copy caching is enabled, further checks will (try to) prevent redundant EFB
  //          copies.
  //      2) Check if a texture cache entry for the specified dstAddr already exists (i.e. if an EFB
  //      copy was triggered to that address before):
  //      2a) Entry doesn't exist:
  //          - Also copy the requested EFB data to a texture object in VRAM (like EFB to texture)
  //          - Create a texture cache entry for the target (type = TCET_EC_VRAM)
  //          - Store a hash of the encoded RAM data in the texcache entry.
  //      2b) Entry exists AND type is TCET_EC_VRAM:
  //          - Like case 2a, but reuse the old texcache entry instead of creating a new one.
  //      2c) Entry exists AND type is TCET_EC_DYNAMIC:
  //          - Only encode the texture to RAM (like EFB to RAM) and store a hash of the encoded
  //          data in the existing texcache entry.
  //          - Do NOT copy the requested EFB data to a VRAM object. Reason: the texture is dynamic,
  //          i.e. the CPU is modifying it. Storing a VRAM copy is useless, because we'd always end
  //          up deleting it and reloading the data from RAM anyway.
  //      3) If the EFB copy gets used as a texture, compare the source RAM hash with the hash you
  //      stored when encoding the EFB data to RAM.
  //      3a) If the two hashes match AND type is TCET_EC_VRAM, reuse the VRAM copy you created
  //      3b) If the two hashes differ AND type is TCET_EC_VRAM, screw your existing VRAM copy. Set
  //      type to TCET_EC_DYNAMIC.
  //          Redecode the source RAM data to a VRAM object. The entry basically behaves like a
  //          normal texture now.
  //      3c) If type is TCET_EC_DYNAMIC, treat the EFB copy like a normal texture.
  //      Advantage: Non-dynamic EFB copies can be visually enhanced like with EFB to texture.
  //                 Compatibility is as good as EFB to RAM.
  //      Disadvantage: Slower than EFB to texture and often even slower than EFB to RAM.
  //                    EFB copy cache depends on accurate texture hashing being enabled. However,
  //                    with accurate hashing you end up being as slow as without a copy cache
  //                    anyway.
  //
  // Disadvantage of all methods: Calling this function requires the GPU to perform a pipeline flush
  // which stalls any further CPU processing.
  const bool is_xfb_copy = !is_depth_copy && !isIntensity && dstFormat == EFBCopyFormat::XFB;
  bool copy_to_vram =
      g_ActiveConfig.backend_info.bSupportsCopyToVram && !g_ActiveConfig.bDisableCopyToVRAM;
  bool copy_to_ram =
      !(is_xfb_copy ? g_ActiveConfig.bSkipXFBCopyToRam : g_ActiveConfig.bSkipEFBCopyToRam) ||
      !copy_to_vram;

  u8* dst = Memory::GetPointer(dstAddr);
  if (dst == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Trying to copy from EFB to invalid address {:#010x}", dstAddr);
    return;
  }

  // tex_w and tex_h are the native size of the texture in the GC memory.
  // The size scaled_* represents the emulated texture. Those differ
  // because of upscaling and because of yscaling of XFB copies.
  // For the latter, we keep the EFB resolution for the virtual XFB blit.
  u32 tex_w = width;
  u32 tex_h = height;
  u32 scaled_tex_w = g_renderer->EFBToScaledX(width);
  u32 scaled_tex_h = g_renderer->EFBToScaledY(height);

  if (scaleByHalf)
  {
    tex_w /= 2;
    tex_h /= 2;
    scaled_tex_w /= 2;
    scaled_tex_h /= 2;
  }

  if (!is_xfb_copy && !g_ActiveConfig.bCopyEFBScaled)
  {
    // No upscaling
    scaled_tex_w = tex_w;
    scaled_tex_h = tex_h;
  }

  // Get the base (in memory) format of this efb copy.
  TextureFormat baseFormat = TexDecoder_GetEFBCopyBaseFormat(dstFormat);

  u32 blockH = TexDecoder_GetBlockHeightInTexels(baseFormat);
  const u32 blockW = TexDecoder_GetBlockWidthInTexels(baseFormat);

  // Round up source height to multiple of block size
  u32 actualHeight = Common::AlignUp(tex_h, blockH);
  const u32 actualWidth = Common::AlignUp(tex_w, blockW);

  u32 num_blocks_y = actualHeight / blockH;
  const u32 num_blocks_x = actualWidth / blockW;

  // RGBA takes two cache lines per block; all others take one
  const u32 bytes_per_block = baseFormat == TextureFormat::RGBA8 ? 64 : 32;

  const u32 bytes_per_row = num_blocks_x * bytes_per_block;
  const u32 covered_range = num_blocks_y * dstStride;

  if (dstStride < bytes_per_row)
  {
    // This kind of efb copy results in a scrambled image.
    // I'm pretty sure no game actually wants to do this, it might be caused by a
    // programming bug in the game, or a CPU/Bounding box emulation issue with dolphin.
    // The copy_to_ram code path above handles this "correctly" and scrambles the image
    // but the copy_to_vram code path just saves and uses unscrambled texture instead.

    // To avoid a "incorrect" result, we simply skip doing the copy_to_vram code path
    // so if the game does try to use the scrambled texture, dolphin will grab the scrambled
    // texture (or black if copy_to_ram is also disabled) out of ram.
    ERROR_LOG_FMT(VIDEO, "Memory stride too small ({} < {})", dstStride, bytes_per_row);
    copy_to_vram = false;
  }

  // We also linear filtering for both box filtering and downsampling higher resolutions to 1x.
  // TODO: This only produces perfect downsampling for 2x IR, other resolutions will need more
  //       complex down filtering to average all pixels and produce the correct result.
  const bool linear_filter =
      !is_depth_copy && (scaleByHalf || g_renderer->GetEFBScale() != 1 || y_scale > 1.0f);

  TCacheEntry* entry = nullptr;
  if (copy_to_vram)
  {
    // create the texture
    const TextureConfig config(scaled_tex_w, scaled_tex_h, 1, g_framebuffer_manager->GetEFBLayers(),
                               1, AbstractTextureFormat::RGBA8, AbstractTextureFlag_RenderTarget);
    entry = AllocateCacheEntry(config);
    if (entry)
    {
      entry->SetGeneralParameters(dstAddr, 0, baseFormat, is_xfb_copy);
      entry->SetDimensions(tex_w, tex_h, 1);
      entry->frameCount = FRAMECOUNT_INVALID;
      if (is_xfb_copy)
      {
        entry->should_force_safe_hashing = is_xfb_copy;
        entry->SetXfbCopy(dstStride);
      }
      else
      {
        entry->SetEfbCopy(dstStride);
      }
      entry->may_have_overlapping_textures = false;
      entry->is_custom_tex = false;

      CopyEFBToCacheEntry(entry, is_depth_copy, srcRect, scaleByHalf, linear_filter, dstFormat,
                          isIntensity, gamma, clamp_top, clamp_bottom,
                          GetVRAMCopyFilterCoefficients(filter_coefficients));

      if (g_ActiveConfig.bDumpEFBTarget && !is_xfb_copy)
      {
        static int efb_count = 0;
        entry->texture->Save(
            fmt::format("{}efb_frame_{}.png", File::GetUserPath(D_DUMPTEXTURES_IDX), efb_count++),
            0);
      }

      if (g_ActiveConfig.bDumpXFBTarget && is_xfb_copy)
      {
        static int xfb_count = 0;
        entry->texture->Save(
            fmt::format("{}xfb_copy_{}.png", File::GetUserPath(D_DUMPTEXTURES_IDX), xfb_count++),
            0);
      }
    }
  }

  if (copy_to_ram)
  {
    EFBCopyFilterCoefficients coefficients = GetRAMCopyFilterCoefficients(filter_coefficients);
    PixelFormat srcFormat = bpmem.zcontrol.pixel_format;
    EFBCopyParams format(srcFormat, dstFormat, is_depth_copy, isIntensity,
                         NeedsCopyFilterInShader(coefficients));

    std::unique_ptr<AbstractStagingTexture> staging_texture = GetEFBCopyStagingTexture();
    if (staging_texture)
    {
      CopyEFB(staging_texture.get(), format, tex_w, bytes_per_row, num_blocks_y, dstStride, srcRect,
              scaleByHalf, linear_filter, y_scale, gamma, clamp_top, clamp_bottom, coefficients);

      // We can't defer if there is no VRAM copy (since we need to update the hash).
      if (!copy_to_vram || !g_ActiveConfig.bDeferEFBCopies)
      {
        // Immediately flush it.
        WriteEFBCopyToRAM(dst, bytes_per_row / sizeof(u32), num_blocks_y, dstStride,
                          std::move(staging_texture));
      }
      else
      {
        // Defer the flush until later.
        entry->pending_efb_copy = std::move(staging_texture);
        entry->pending_efb_copy_width = bytes_per_row / sizeof(u32);
        entry->pending_efb_copy_height = num_blocks_y;
        entry->pending_efb_copy_invalidated = false;
        m_pending_efb_copies.push_back(entry);
      }
    }
  }
  else
  {
    if (is_xfb_copy)
    {
      UninitializeXFBMemory(dst, dstStride, bytes_per_row, num_blocks_y);
    }
    else
    {
      // Hack: Most games don't actually need the correct texture data in RAM
      //       and we can just keep a copy in VRAM. We zero the memory so we
      //       can check it hasn't changed before using our copy in VRAM.
      u8* ptr = dst;
      for (u32 i = 0; i < num_blocks_y; i++)
      {
        std::memset(ptr, 0, bytes_per_row);
        ptr += dstStride;
      }
    }
  }

  // Invalidate all textures, if they are either fully overwritten by our efb copy, or if they
  // have a different stride than our efb copy. Partly overwritten textures with the same stride
  // as our efb copy are marked to check them for partial texture updates.
  // TODO: The logic to detect overlapping strided efb copies is not 100% accurate.
  bool strided_efb_copy = dstStride != bytes_per_row;
  auto iter = FindOverlappingTextures(dstAddr, covered_range);
  while (iter.first != iter.second)
  {
    TCacheEntry* overlapping_entry = iter.first->second;

    if (overlapping_entry->addr == dstAddr && overlapping_entry->is_xfb_copy)
    {
      for (auto& reference : overlapping_entry->references)
      {
        reference->reference_changed = true;
      }
    }

    if (overlapping_entry->OverlapsMemoryRange(dstAddr, covered_range))
    {
      u32 overlap_range = std::min(overlapping_entry->addr + overlapping_entry->size_in_bytes,
                                   dstAddr + covered_range) -
                          std::max(overlapping_entry->addr, dstAddr);
      if (!copy_to_vram || overlapping_entry->memory_stride != dstStride ||
          (!strided_efb_copy && overlapping_entry->size_in_bytes == overlap_range) ||
          (strided_efb_copy && overlapping_entry->size_in_bytes == overlap_range &&
           overlapping_entry->addr == dstAddr))
      {
        // Pending EFB copies which are completely covered by this new copy can simply be tossed,
        // instead of having to flush them later on, since this copy will write over everything.
        iter.first = InvalidateTexture(iter.first, true);
        continue;
      }

      // We don't want to change the may_have_overlapping_textures flag on XFB container entries
      // because otherwise they can't be re-used/updated, leaking textures for several frames.
      if (!overlapping_entry->is_xfb_container)
        overlapping_entry->may_have_overlapping_textures = true;

      // There are cases (Rogue Squadron 2 / Texas Holdem on Wiiware) where
      // for xfb copies the textures overlap which causes the hash of the first copy
      // to be different (from when it was originally created).  This has no implications
      // for XFB2Tex because the underlying memory doesn't change (dummy values) but
      // can affect XFB2Ram when we compare the texture cache copy hash with the
      // newly computed hash
      // By calculating the hash when we receive overlapping xfbs, we are able
      // to mitigate this
      if (overlapping_entry->is_xfb_copy && copy_to_ram)
      {
        overlapping_entry->hash = overlapping_entry->CalculateHash();
      }

      // Do not load textures by hash, if they were at least partly overwritten by an efb copy.
      // In this case, comparing the hash is not enough to check, if two textures are identical.
      if (overlapping_entry->textures_by_hash_iter != textures_by_hash.end())
      {
        textures_by_hash.erase(overlapping_entry->textures_by_hash_iter);
        overlapping_entry->textures_by_hash_iter = textures_by_hash.end();
      }
    }
    ++iter.first;
  }

  if (OpcodeDecoder::g_record_fifo_data)
  {
    // Mark the memory behind this efb copy as dynamicly generated for the Fifo log
    u32 address = dstAddr;
    for (u32 i = 0; i < num_blocks_y; i++)
    {
      FifoRecorder::GetInstance().UseMemory(address, bytes_per_row, MemoryUpdate::TEXTURE_MAP,
                                            true);
      address += dstStride;
    }
  }

  // Even if the copy is deferred, still compute the hash. This way if the copy is used as a texture
  // in a subsequent draw before it is flushed, it will have the same hash.
  if (entry)
  {
    const u64 hash = entry->CalculateHash();
    entry->SetHashes(hash, hash);
    textures_by_address.emplace(dstAddr, entry);
  }
}

void TextureCacheBase::FlushEFBCopies()
{
  if (m_pending_efb_copies.empty())
    return;

  for (TCacheEntry* entry : m_pending_efb_copies)
    FlushEFBCopy(entry);
  m_pending_efb_copies.clear();
}

void TextureCacheBase::WriteEFBCopyToRAM(u8* dst_ptr, u32 width, u32 height, u32 stride,
                                         std::unique_ptr<AbstractStagingTexture> staging_texture)
{
  MathUtil::Rectangle<int> copy_rect(0, 0, static_cast<int>(width), static_cast<int>(height));
  staging_texture->ReadTexels(copy_rect, dst_ptr, stride);
  ReleaseEFBCopyStagingTexture(std::move(staging_texture));
}

void TextureCacheBase::FlushEFBCopy(TCacheEntry* entry)
{
  // Copy from texture -> guest memory.
  u8* const dst = Memory::GetPointer(entry->addr);
  WriteEFBCopyToRAM(dst, entry->pending_efb_copy_width, entry->pending_efb_copy_height,
                    entry->memory_stride, std::move(entry->pending_efb_copy));

  // If the EFB copy was invalidated (e.g. the bloom case mentioned in InvalidateTexture), now is
  // the time to clean up the TCacheEntry. In which case, we don't need to compute the new hash of
  // the RAM copy. But we need to clean up the TCacheEntry, as InvalidateTexture doesn't free it.
  if (entry->pending_efb_copy_invalidated)
  {
    delete entry;
    return;
  }

  // Re-hash the texture now that the guest memory is populated.
  // This should be safe because we'll catch any writes before the game can modify it.
  const u64 hash = entry->CalculateHash();
  entry->SetHashes(hash, hash);

  // Check for any overlapping XFB copies which now need the hash recomputed.
  // See the comment above regarding Rogue Squadron 2.
  if (entry->is_xfb_copy)
  {
    const u32 covered_range = entry->pending_efb_copy_height * entry->memory_stride;
    auto range = FindOverlappingTextures(entry->addr, covered_range);
    for (auto iter = range.first; iter != range.second; ++iter)
    {
      TCacheEntry* overlapping_entry = iter->second;
      if (overlapping_entry->may_have_overlapping_textures && overlapping_entry->is_xfb_copy &&
          overlapping_entry->OverlapsMemoryRange(entry->addr, covered_range))
      {
        const u64 overlapping_hash = overlapping_entry->CalculateHash();
        entry->SetHashes(overlapping_hash, overlapping_hash);
      }
    }
  }
}

std::unique_ptr<AbstractStagingTexture> TextureCacheBase::GetEFBCopyStagingTexture()
{
  // Pull off the back first to re-use the most frequently used textures.
  if (!m_efb_copy_staging_texture_pool.empty())
  {
    auto ptr = std::move(m_efb_copy_staging_texture_pool.back());
    m_efb_copy_staging_texture_pool.pop_back();
    return ptr;
  }

  std::unique_ptr<AbstractStagingTexture> tex = g_renderer->CreateStagingTexture(
      StagingTextureType::Readback, m_efb_encoding_texture->GetConfig());
  if (!tex)
    WARN_LOG_FMT(VIDEO, "Failed to create EFB copy staging texture");

  return tex;
}

void TextureCacheBase::ReleaseEFBCopyStagingTexture(std::unique_ptr<AbstractStagingTexture> tex)
{
  m_efb_copy_staging_texture_pool.push_back(std::move(tex));
}

void TextureCacheBase::UninitializeXFBMemory(u8* dst, u32 stride, u32 bytes_per_row,
                                             u32 num_blocks_y)
{
  // Originally, we planned on using a 'key color'
  // for alpha to address partial xfbs (Mario Strikers / Chicken Little).
  // This work was removed since it was unfinished but there
  // was still a desire to differentiate between the old and the new approach
  // which is why we still set uninitialized xfb memory to fuchsia
  // (Y=1,U=254,V=254) instead of dark green (Y=0,U=0,V=0) in YUV
  // like is done in the EFB path.

#if defined(_M_X86) || defined(_M_X86_64)
  __m128i sixteenBytes = _mm_set1_epi16((s16)(u16)0xFE01);
#endif

  for (u32 i = 0; i < num_blocks_y; i++)
  {
    u32 size = bytes_per_row;
    u8* rowdst = dst;
#if defined(_M_X86) || defined(_M_X86_64)
    while (size >= 16)
    {
      _mm_storeu_si128((__m128i*)rowdst, sixteenBytes);
      size -= 16;
      rowdst += 16;
    }
#endif
    for (u32 offset = 0; offset < size; offset++)
    {
      if (offset & 1)
      {
        rowdst[offset] = 254;
      }
      else
      {
        rowdst[offset] = 1;
      }
    }
    dst += stride;
  }
}

TextureCacheBase::TCacheEntry* TextureCacheBase::AllocateCacheEntry(const TextureConfig& config)
{
  std::optional<TexPoolEntry> alloc = AllocateTexture(config);
  if (!alloc)
    return nullptr;

  TCacheEntry* cacheEntry =
      new TCacheEntry(std::move(alloc->texture), std::move(alloc->framebuffer));
  cacheEntry->textures_by_hash_iter = textures_by_hash.end();
  cacheEntry->id = last_entry_id++;
  return cacheEntry;
}

std::optional<TextureCacheBase::TexPoolEntry>
TextureCacheBase::AllocateTexture(const TextureConfig& config)
{
  TexPool::iterator iter = FindMatchingTextureFromPool(config);
  if (iter != texture_pool.end())
  {
    auto entry = std::move(iter->second);
    texture_pool.erase(iter);
    return std::move(entry);
  }

  std::unique_ptr<AbstractTexture> texture = g_renderer->CreateTexture(config);
  if (!texture)
  {
    WARN_LOG_FMT(VIDEO, "Failed to allocate a {}x{}x{} texture", config.width, config.height,
                 config.layers);
    return {};
  }

  std::unique_ptr<AbstractFramebuffer> framebuffer;
  if (config.IsRenderTarget())
  {
    framebuffer = g_renderer->CreateFramebuffer(texture.get(), nullptr);
    if (!framebuffer)
    {
      WARN_LOG_FMT(VIDEO, "Failed to allocate a {}x{}x{} framebuffer", config.width, config.height,
                   config.layers);
      return {};
    }
  }

  INCSTAT(g_stats.num_textures_created);
  return TexPoolEntry(std::move(texture), std::move(framebuffer));
}

TextureCacheBase::TexPool::iterator
TextureCacheBase::FindMatchingTextureFromPool(const TextureConfig& config)
{
  // Find a texture from the pool that does not have a frameCount of FRAMECOUNT_INVALID.
  // This prevents a texture from being used twice in a single frame with different data,
  // which potentially means that a driver has to maintain two copies of the texture anyway.
  // Render-target textures are fine through, as they have to be generated in a seperated pass.
  // As non-render-target textures are usually static, this should not matter much.
  auto range = texture_pool.equal_range(config);
  auto matching_iter = std::find_if(range.first, range.second, [](const auto& iter) {
    return iter.first.IsRenderTarget() || iter.second.frameCount != FRAMECOUNT_INVALID;
  });
  return matching_iter != range.second ? matching_iter : texture_pool.end();
}

TextureCacheBase::TexAddrCache::iterator
TextureCacheBase::GetTexCacheIter(TextureCacheBase::TCacheEntry* entry)
{
  auto iter_range = textures_by_address.equal_range(entry->addr);
  TexAddrCache::iterator iter = iter_range.first;
  while (iter != iter_range.second)
  {
    if (iter->second == entry)
    {
      return iter;
    }
    ++iter;
  }
  return textures_by_address.end();
}

std::pair<TextureCacheBase::TexAddrCache::iterator, TextureCacheBase::TexAddrCache::iterator>
TextureCacheBase::FindOverlappingTextures(u32 addr, u32 size_in_bytes)
{
  // We index by the starting address only, so there is no way to query all textures
  // which end after the given addr. But the GC textures have a limited size, so we
  // look for all textures which have a start address bigger than addr minus the maximal
  // texture size. But this yields false-positives which must be checked later on.

  // 1024 x 1024 texel times 8 nibbles per texel
  constexpr u32 max_texture_size = 1024 * 1024 * 4;
  u32 lower_addr = addr > max_texture_size ? addr - max_texture_size : 0;
  auto begin = textures_by_address.lower_bound(lower_addr);
  auto end = textures_by_address.upper_bound(addr + size_in_bytes);

  return std::make_pair(begin, end);
}

TextureCacheBase::TexAddrCache::iterator
TextureCacheBase::InvalidateTexture(TexAddrCache::iterator iter, bool discard_pending_efb_copy)
{
  if (iter == textures_by_address.end())
    return textures_by_address.end();

  TCacheEntry* entry = iter->second;

  if (entry->textures_by_hash_iter != textures_by_hash.end())
  {
    textures_by_hash.erase(entry->textures_by_hash_iter);
    entry->textures_by_hash_iter = textures_by_hash.end();
  }

  for (size_t i = 0; i < bound_textures.size(); ++i)
  {
    // If the entry is currently bound and not invalidated, keep it, but mark it as invalidated.
    // This way it can still be used via tmem cache emulation, but nothing else.
    // Spyro: A Hero's Tail is known for using such overwritten textures.
    if (bound_textures[i] == entry && IsValidBindPoint(static_cast<u32>(i)))
    {
      bound_textures[i]->tmem_only = true;
      return ++iter;
    }
  }

  // If this is a pending EFB copy, we don't want to flush it here.
  // Why? Because let's say a game is rendering a bloom-type effect, using EFB copies to essentially
  // downscale the framebuffer. Copy from EFB->Texture, draw texture to EFB, copy EFB->Texture,
  // draw, repeat. The second copy will invalidate the first, forcing a flush. Which means we lose
  // any benefit of EFB copy batching. So instead, let's just leave the EFB copy pending, but remove
  // it from the texture cache. This way we don't use the old VRAM copy. When the EFB copies are
  // eventually flushed, they will overwrite each other, and the end result should be the same.
  if (entry->pending_efb_copy)
  {
    if (discard_pending_efb_copy)
    {
      // If the RAM copy is being completely overwritten by a new EFB copy, we can discard the
      // existing pending copy, and not bother waiting for it in the future. This happens in
      // Xenoblade's sunset scene, where 35 copies are done per frame, and 25 of them are
      // copied to the same address, and can be skipped.
      ReleaseEFBCopyStagingTexture(std::move(entry->pending_efb_copy));
      auto pending_it = std::find(m_pending_efb_copies.begin(), m_pending_efb_copies.end(), entry);
      if (pending_it != m_pending_efb_copies.end())
        m_pending_efb_copies.erase(pending_it);
    }
    else
    {
      entry->pending_efb_copy_invalidated = true;
    }
  }

  auto config = entry->texture->GetConfig();
  texture_pool.emplace(config,
                       TexPoolEntry(std::move(entry->texture), std::move(entry->framebuffer)));

  // Don't delete if there's a pending EFB copy, as we need the TCacheEntry alive.
  if (!entry->pending_efb_copy)
    delete entry;

  return textures_by_address.erase(iter);
}

bool TextureCacheBase::CreateUtilityTextures()
{
  constexpr TextureConfig encoding_texture_config(
      EFB_WIDTH * 4, 1024, 1, 1, 1, AbstractTextureFormat::BGRA8, AbstractTextureFlag_RenderTarget);
  m_efb_encoding_texture = g_renderer->CreateTexture(encoding_texture_config);
  if (!m_efb_encoding_texture)
    return false;

  m_efb_encoding_framebuffer = g_renderer->CreateFramebuffer(m_efb_encoding_texture.get(), nullptr);
  if (!m_efb_encoding_framebuffer)
    return false;

  if (g_ActiveConfig.backend_info.bSupportsGPUTextureDecoding)
  {
    constexpr TextureConfig decoding_texture_config(
        1024, 1024, 1, 1, 1, AbstractTextureFormat::RGBA8, AbstractTextureFlag_ComputeImage);
    m_decoding_texture = g_renderer->CreateTexture(decoding_texture_config);
    if (!m_decoding_texture)
      return false;
  }

  return true;
}

void TextureCacheBase::CopyEFBToCacheEntry(TCacheEntry* entry, bool is_depth_copy,
                                           const MathUtil::Rectangle<int>& src_rect,
                                           bool scale_by_half, bool linear_filter,
                                           EFBCopyFormat dst_format, bool is_intensity, float gamma,
                                           bool clamp_top, bool clamp_bottom,
                                           const EFBCopyFilterCoefficients& filter_coefficients)
{
  // Flush EFB pokes first, as they're expected to be included.
  g_framebuffer_manager->FlushEFBPokes();

  // Get the pipeline which we will be using. If the compilation failed, this will be null.
  const AbstractPipeline* copy_pipeline =
      g_shader_cache->GetEFBCopyToVRAMPipeline(TextureConversionShaderGen::GetShaderUid(
          dst_format, is_depth_copy, is_intensity, scale_by_half,
          NeedsCopyFilterInShader(filter_coefficients)));
  if (!copy_pipeline)
  {
    WARN_LOG_FMT(VIDEO, "Skipping EFB copy to VRAM due to missing pipeline.");
    return;
  }

  const auto scaled_src_rect = g_renderer->ConvertEFBRectangle(src_rect);
  const auto framebuffer_rect = g_renderer->ConvertFramebufferRectangle(
      scaled_src_rect, g_framebuffer_manager->GetEFBFramebuffer());
  AbstractTexture* src_texture =
      is_depth_copy ? g_framebuffer_manager->ResolveEFBDepthTexture(framebuffer_rect) :
                      g_framebuffer_manager->ResolveEFBColorTexture(framebuffer_rect);

  src_texture->FinishedRendering();
  g_renderer->BeginUtilityDrawing();

  // Fill uniform buffer.
  struct Uniforms
  {
    float src_left, src_top, src_width, src_height;
    float filter_coefficients[3];
    float gamma_rcp;
    float clamp_top;
    float clamp_bottom;
    float pixel_height;
    u32 padding;
  };
  Uniforms uniforms;
  const float rcp_efb_width = 1.0f / static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  const float rcp_efb_height = 1.0f / static_cast<float>(g_framebuffer_manager->GetEFBHeight());
  uniforms.src_left = framebuffer_rect.left * rcp_efb_width;
  uniforms.src_top = framebuffer_rect.top * rcp_efb_height;
  uniforms.src_width = framebuffer_rect.GetWidth() * rcp_efb_width;
  uniforms.src_height = framebuffer_rect.GetHeight() * rcp_efb_height;
  uniforms.filter_coefficients[0] = filter_coefficients.upper;
  uniforms.filter_coefficients[1] = filter_coefficients.middle;
  uniforms.filter_coefficients[2] = filter_coefficients.lower;
  uniforms.gamma_rcp = 1.0f / gamma;
  uniforms.clamp_top = clamp_top ? framebuffer_rect.top * rcp_efb_height : 0.0f;
  uniforms.clamp_bottom = clamp_bottom ? framebuffer_rect.bottom * rcp_efb_height : 1.0f;
  uniforms.pixel_height = g_ActiveConfig.bCopyEFBScaled ? rcp_efb_height : 1.0f / EFB_HEIGHT;
  uniforms.padding = 0;
  g_vertex_manager->UploadUtilityUniforms(&uniforms, sizeof(uniforms));

  // Use the copy pipeline to render the VRAM copy.
  g_renderer->SetAndDiscardFramebuffer(entry->framebuffer.get());
  g_renderer->SetViewportAndScissor(entry->framebuffer->GetRect());
  g_renderer->SetPipeline(copy_pipeline);
  g_renderer->SetTexture(0, src_texture);
  g_renderer->SetSamplerState(0, linear_filter ? RenderState::GetLinearSamplerState() :
                                                 RenderState::GetPointSamplerState());
  g_renderer->Draw(0, 3);
  g_renderer->EndUtilityDrawing();
  entry->texture->FinishedRendering();
}

void TextureCacheBase::CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params,
                               u32 native_width, u32 bytes_per_row, u32 num_blocks_y,
                               u32 memory_stride, const MathUtil::Rectangle<int>& src_rect,
                               bool scale_by_half, bool linear_filter, float y_scale, float gamma,
                               bool clamp_top, bool clamp_bottom,
                               const EFBCopyFilterCoefficients& filter_coefficients)
{
  // Flush EFB pokes first, as they're expected to be included.
  g_framebuffer_manager->FlushEFBPokes();

  // Get the pipeline which we will be using. If the compilation failed, this will be null.
  const AbstractPipeline* copy_pipeline = g_shader_cache->GetEFBCopyToRAMPipeline(params);
  if (!copy_pipeline)
  {
    WARN_LOG_FMT(VIDEO, "Skipping EFB copy to VRAM due to missing pipeline.");
    return;
  }

  const auto scaled_src_rect = g_renderer->ConvertEFBRectangle(src_rect);
  const auto framebuffer_rect = g_renderer->ConvertFramebufferRectangle(
      scaled_src_rect, g_framebuffer_manager->GetEFBFramebuffer());
  AbstractTexture* src_texture =
      params.depth ? g_framebuffer_manager->ResolveEFBDepthTexture(framebuffer_rect) :
                     g_framebuffer_manager->ResolveEFBColorTexture(framebuffer_rect);

  src_texture->FinishedRendering();
  g_renderer->BeginUtilityDrawing();

  // Fill uniform buffer.
  struct Uniforms
  {
    std::array<s32, 4> position_uniform;
    float y_scale;
    float gamma_rcp;
    float clamp_top;
    float clamp_bottom;
    float filter_coefficients[3];
    u32 padding;
  };
  Uniforms encoder_params;
  const float rcp_efb_height = 1.0f / static_cast<float>(g_framebuffer_manager->GetEFBHeight());
  encoder_params.position_uniform[0] = src_rect.left;
  encoder_params.position_uniform[1] = src_rect.top;
  encoder_params.position_uniform[2] = static_cast<s32>(native_width);
  encoder_params.position_uniform[3] = scale_by_half ? 2 : 1;
  encoder_params.y_scale = y_scale;
  encoder_params.gamma_rcp = 1.0f / gamma;
  encoder_params.clamp_top = clamp_top ? framebuffer_rect.top * rcp_efb_height : 0.0f;
  encoder_params.clamp_bottom = clamp_bottom ? framebuffer_rect.bottom * rcp_efb_height : 1.0f;
  encoder_params.filter_coefficients[0] = filter_coefficients.upper;
  encoder_params.filter_coefficients[1] = filter_coefficients.middle;
  encoder_params.filter_coefficients[2] = filter_coefficients.lower;
  g_vertex_manager->UploadUtilityUniforms(&encoder_params, sizeof(encoder_params));

  // Because the shader uses gl_FragCoord and we read it back, we must render to the lower-left.
  const u32 render_width = bytes_per_row / sizeof(u32);
  const u32 render_height = num_blocks_y;
  const auto encode_rect = MathUtil::Rectangle<int>(0, 0, render_width, render_height);

  // Render to GPU texture, and then copy to CPU-accessible texture.
  g_renderer->SetAndDiscardFramebuffer(m_efb_encoding_framebuffer.get());
  g_renderer->SetViewportAndScissor(encode_rect);
  g_renderer->SetPipeline(copy_pipeline);
  g_renderer->SetTexture(0, src_texture);
  g_renderer->SetSamplerState(0, linear_filter ? RenderState::GetLinearSamplerState() :
                                                 RenderState::GetPointSamplerState());
  g_renderer->Draw(0, 3);
  dst->CopyFromTexture(m_efb_encoding_texture.get(), encode_rect, 0, 0, encode_rect);
  g_renderer->EndUtilityDrawing();

  // Flush if there's sufficient draws between this copy and the last.
  g_vertex_manager->OnEFBCopyToRAM();
}

bool TextureCacheBase::DecodeTextureOnGPU(TCacheEntry* entry, u32 dst_level, const u8* data,
                                          u32 data_size, TextureFormat format, u32 width,
                                          u32 height, u32 aligned_width, u32 aligned_height,
                                          u32 row_stride, const u8* palette,
                                          TLUTFormat palette_format)
{
  const auto* info = TextureConversionShaderTiled::GetDecodingShaderInfo(format);
  if (!info)
    return false;

  const AbstractShader* shader = g_shader_cache->GetTextureDecodingShader(format, palette_format);
  if (!shader)
    return false;

  // Copy to GPU-visible buffer, aligned to the data type.
  const u32 bytes_per_buffer_elem =
      VertexManagerBase::GetTexelBufferElementSize(info->buffer_format);

  // Allocate space in stream buffer, and copy texture + palette across.
  u32 src_offset = 0, palette_offset = 0;
  if (info->palette_size > 0)
  {
    if (!g_vertex_manager->UploadTexelBuffer(data, data_size, info->buffer_format, &src_offset,
                                             palette, info->palette_size,
                                             TEXEL_BUFFER_FORMAT_R16_UINT, &palette_offset))
    {
      return false;
    }
  }
  else
  {
    if (!g_vertex_manager->UploadTexelBuffer(data, data_size, info->buffer_format, &src_offset))
      return false;
  }

  // Set up uniforms.
  struct Uniforms
  {
    u32 dst_width, dst_height;
    u32 src_width, src_height;
    u32 src_offset, src_row_stride;
    u32 palette_offset, unused;
  } uniforms = {width,          height,     aligned_width,
                aligned_height, src_offset, row_stride / bytes_per_buffer_elem,
                palette_offset};
  g_vertex_manager->UploadUtilityUniforms(&uniforms, sizeof(uniforms));
  g_renderer->SetComputeImageTexture(m_decoding_texture.get(), false, true);

  auto dispatch_groups =
      TextureConversionShaderTiled::GetDispatchCount(info, aligned_width, aligned_height);
  g_renderer->DispatchComputeShader(shader, dispatch_groups.first, dispatch_groups.second, 1);

  // Copy from decoding texture -> final texture
  // This is because we don't want to have to create compute view for every layer
  const auto copy_rect = entry->texture->GetConfig().GetMipRect(dst_level);
  entry->texture->CopyRectangleFromTexture(m_decoding_texture.get(), copy_rect, 0, 0, copy_rect, 0,
                                           dst_level);
  entry->texture->FinishedRendering();
  return true;
}

u32 TextureCacheBase::TCacheEntry::BytesPerRow() const
{
  const u32 blockW = TexDecoder_GetBlockWidthInTexels(format.texfmt);

  // Round up source height to multiple of block size
  const u32 actualWidth = Common::AlignUp(native_width, blockW);

  const u32 numBlocksX = actualWidth / blockW;

  // RGBA takes two cache lines per block; all others take one
  const u32 bytes_per_block = format == TextureFormat::RGBA8 ? 64 : 32;

  return numBlocksX * bytes_per_block;
}

u32 TextureCacheBase::TCacheEntry::NumBlocksY() const
{
  u32 blockH = TexDecoder_GetBlockHeightInTexels(format.texfmt);
  // Round up source height to multiple of block size
  u32 actualHeight = Common::AlignUp(native_height, blockH);

  return actualHeight / blockH;
}

void TextureCacheBase::TCacheEntry::SetXfbCopy(u32 stride)
{
  is_efb_copy = false;
  is_xfb_copy = true;
  is_xfb_container = false;
  memory_stride = stride;

  ASSERT_MSG(VIDEO, memory_stride >= BytesPerRow(), "Memory stride is too small");

  size_in_bytes = memory_stride * NumBlocksY();
}

void TextureCacheBase::TCacheEntry::SetEfbCopy(u32 stride)
{
  is_efb_copy = true;
  is_xfb_copy = false;
  is_xfb_container = false;
  memory_stride = stride;

  ASSERT_MSG(VIDEO, memory_stride >= BytesPerRow(), "Memory stride is too small");

  size_in_bytes = memory_stride * NumBlocksY();
}

void TextureCacheBase::TCacheEntry::SetNotCopy()
{
  is_efb_copy = false;
  is_xfb_copy = false;
  is_xfb_container = false;
}

int TextureCacheBase::TCacheEntry::HashSampleSize() const
{
  if (should_force_safe_hashing)
  {
    return 0;
  }

  return g_ActiveConfig.iSafeTextureCache_ColorSamples;
}

u64 TextureCacheBase::TCacheEntry::CalculateHash() const
{
  const u32 bytes_per_row = BytesPerRow();
  const u32 hash_sample_size = HashSampleSize();
  u8* ptr = Memory::GetPointer(addr);
  if (memory_stride == bytes_per_row)
  {
    return Common::GetHash64(ptr, size_in_bytes, hash_sample_size);
  }
  else
  {
    const u32 num_blocks_y = NumBlocksY();
    u64 temp_hash = size_in_bytes;

    u32 samples_per_row = 0;
    if (hash_sample_size != 0)
    {
      // Hash at least 4 samples per row to avoid hashing in a bad pattern, like just on the left
      // side of the efb copy
      samples_per_row = std::max(hash_sample_size / num_blocks_y, 4u);
    }

    for (u32 i = 0; i < num_blocks_y; i++)
    {
      // Multiply by a prime number to mix the hash up a bit. This prevents identical blocks from
      // canceling each other out
      temp_hash = (temp_hash * 397) ^ Common::GetHash64(ptr, bytes_per_row, samples_per_row);
      ptr += memory_stride;
    }
    return temp_hash;
  }
}

TextureCacheBase::TexPoolEntry::TexPoolEntry(std::unique_ptr<AbstractTexture> tex,
                                             std::unique_ptr<AbstractFramebuffer> fb)
    : texture(std::move(tex)), framebuffer(std::move(fb))
{
}
