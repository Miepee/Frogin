// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Image.h"

#include <string>
#include <vector>

#include <png.h>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"
#include <Common/FileUtil.h>

namespace Common
{
bool LoadPNG(const std::vector<u8>& input, std::vector<u8>* data_out, u32* width_out,
             u32* height_out)
{
  // Using the 'Simplified API' of libpng; see section V in the libpng manual.

  // Read header
  png_image png = {};
  png.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&png, input.data(), input.size()))
    return false;

  // Prepare output vector
  png.format = PNG_FORMAT_RGBA;
  size_t png_size = PNG_IMAGE_SIZE(png);
  data_out->resize(png_size);

  // Convert to RGBA and write into output vector
  if (!png_image_finish_read(&png, nullptr, data_out->data(), 0, nullptr))
    return false;

  *width_out = png.width;
  *height_out = png.height;

  return true;
}

bool SavePNG(const std::string& path, const u8* input, ImageByteFormat format, u32 width,
             u32 height, int stride)
{
  png_image png = {};
  png.version = PNG_IMAGE_VERSION;
  png.width = width;
  png.height = height;

  size_t byte_per_pixel;
  switch (format)
  {
  case ImageByteFormat::RGB:
    png.format = PNG_FORMAT_RGB;
    byte_per_pixel = 3;
    break;
  case ImageByteFormat::RGBA:
    png.format = PNG_FORMAT_RGBA;
    byte_per_pixel = 4;
    break;
  default:
    return false;
  }

  // libpng doesn't handle non-ASCII characters in path, so write in two steps:
  // first to memory, then to file
  std::vector<u8> buffer(byte_per_pixel * width * height);
  png_alloc_size_t size = buffer.size();
  int success = png_image_write_to_memory(&png, buffer.data(), &size, 0, input, stride, nullptr);
  if (!success && size > buffer.size())
  {
    // initial buffer size guess was too small, set to the now-known size and retry
    buffer.resize(size);
    png.warning_or_error = 0;
    success = png_image_write_to_memory(&png, buffer.data(), &size, 0, input, stride, nullptr);
  }
  if (!success || (png.warning_or_error & PNG_IMAGE_ERROR) != 0)
    return false;

  File::IOFile outfile(path, "wb");
  if (!outfile)
    return false;

  return outfile.WriteBytes(buffer.data(), size);
}

void ResizeCustomTextureAndSaveItToLoadPath(const std::string& path, u32 width, u32 height)
{
  // Frog hook begins here
  // create new path which points to the custom textures load path
  std::string loadPath = path;
  loadPath = loadPath.replace(loadPath.find("Dump"), 4, "Load");

  // some games use this specific texture for what I assume is shader stuff. Since it makes games look really unplayable, I'm ignoring it.
  if (loadPath.find("tex1_4x4_fbf7fa62c3db559f_6.png") != std::string::npos)
  {
    return;
  }

  // these have the width and height of the texture we load in
  int i_width, i_height;

  // we load in the frog, create an empty char* with the size we want to have resize the frogImage
  // to our desired height and then write it
  unsigned char* frogImage = stbi_load((File::GetUserPath(D_LOAD_IDX) + "custom.png").c_str(),
                                       &i_width, &i_height, nullptr, STBI_rgb_alpha);
  unsigned char* frogImageOut = new unsigned char[width * height * 4];
  stbir_resize_uint8(frogImage, i_width, i_height, 0, frogImageOut, width, height, 0, 4);
  stbi_write_png(loadPath.c_str(), width, height, STBI_rgb_alpha, frogImageOut,
                 width * STBI_rgb_alpha);

  // free our stuff so we don't memleak
  stbi_image_free(frogImage);
  delete frogImageOut;
}

bool ConvertRGBAToRGBAndSavePNG(const std::string& path, const u8* input, u32 width, u32 height,
                                int stride)
{
  const std::vector<u8> data = RGBAToRGB(input, width, height, stride);
  return SavePNG(path, data.data(), ImageByteFormat::RGB, width, height);
}

std::vector<u8> RGBAToRGB(const u8* input, u32 width, u32 height, int row_stride)
{
  std::vector<u8> buffer;
  buffer.reserve(width * height * 3);

  for (u32 y = 0; y < height; ++y)
  {
    const u8* pos = input + y * row_stride;
    for (u32 x = 0; x < width; ++x)
    {
      buffer.push_back(pos[x * 4]);
      buffer.push_back(pos[x * 4 + 1]);
      buffer.push_back(pos[x * 4 + 2]);
    }
  }
  return buffer;
}
}  // namespace Common
