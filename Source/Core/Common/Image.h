// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
bool LoadPNG(const std::vector<u8>& input, std::vector<u8>* data_out, u32* width_out,
             u32* height_out);

enum class ImageByteFormat
{
  RGB,
  RGBA,
};

bool SavePNG(const std::string& path, const u8* input, ImageByteFormat format, u32 width,
             u32 height, int stride = 0);

void ResizeCustomTextureAndSaveItToLoadPath(const std::string& path, u32 width, u32 height);

bool ConvertRGBAToRGBAndSavePNG(const std::string& path, const u8* input, u32 width, u32 height,
                                int stride = 0);

std::vector<u8> RGBAToRGB(const u8* input, u32 width, u32 height, int row_stride = 0);

}  // namespace Common
