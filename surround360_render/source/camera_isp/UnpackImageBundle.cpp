/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE_render file in the root directory of this subproject. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "CvUtil.h"
#include "SystemUtil.h"
#include "VrCamException.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

using namespace std;
using namespace cv;
using namespace surround360;
using namespace surround360::util;

DEFINE_int32(image_width,     2048,   "expected image width");
DEFINE_int32(image_height,    2048,   "expected image height");
DEFINE_string(binary_prefix,  "",     "path to binary image disk up to timestamp_ (i.e. before 0,1)");
DEFINE_int32(file_count,      2,      "number of input files");
DEFINE_int32(start_frame,     0,      "start frame (per camera)");
DEFINE_int32(frame_count,     0,      "number of frames to unpack (per camera)");
DEFINE_string(dest_path,      "",     "path to folder to unpack images");
DEFINE_int32(nbits,           8,      "number of bits footage was captured in");
DEFINE_bool(tagged,           false,  "unpack tagged frames");

int main(int argc, char** argv) {
  initSurround360(argc, argv);
  requireArg(FLAGS_binary_prefix, "binary_prefix");
  requireArg(FLAGS_dest_path, "dest_path");
  const size_t imageSize = FLAGS_image_width * FLAGS_image_height * FLAGS_nbits / 8;

  vector<string> cameraNames;
  const string camera_names_path = FLAGS_binary_prefix + "cameranames.txt";
  ifstream cameraNamesFile(camera_names_path);
  if (!cameraNamesFile) {
    throw VrCamException("file read failed:" + camera_names_path);
  }

  string line;
  while (std::getline(cameraNamesFile, line)) {
    cameraNames.push_back(line);
  }
  cameraNamesFile.close();

  const int cameraCount = cameraNames.size();

  // Preallocate the output image
  Mat outImage(FLAGS_image_height, FLAGS_image_width, CV_16U);
  void* outputPtr = outImage.ptr(0);

  // Create dest directory
  struct stat st = {0};
  string destPath(FLAGS_dest_path);
  if (stat(destPath.c_str(), &st) == -1) {
    mkdir(destPath.c_str(), 0755);
  }

  // Read raw bytes and assemble them into images
  int fd[FLAGS_file_count];
  off_t pos[FLAGS_file_count];

  // Each bin file can have different number of frames
  int frameCount[FLAGS_file_count];
  size_t readCount[FLAGS_file_count];

  // Total number of frames is properly updated later if FLAGS_frame_count is 0
  int totalFrameCount = FLAGS_frame_count * cameraCount;

  LOG(INFO) << "Reading binary files...";

  for (int i = 0; i < FLAGS_file_count; ++i) {
    string fileName(FLAGS_binary_prefix + to_string(i) + ".bin");
    fd[i] = open(fileName.c_str(), O_RDONLY);
    if (fd[i] < 0) {
      throw VrCamException(
        "error opening binary file. err: " + string(strerror(errno)) +
        " filename: " + fileName);
    }

    if (FLAGS_frame_count == 0) {
      struct stat st;
      fstat(fd[i], &st);
      frameCount[i] =  st.st_size / imageSize;
      VLOG(1) << "Total frame count binary " << i << ": " << frameCount[i];
    }

    // posix_fadvices speeds things up in Linux, but doesn't work in Darwin (Mac OS X)
#if _XOPEN_SOURCE >= 600 || _POSIX_C_SOURCE >= 200112L
    const size_t fileSizeStart = FLAGS_start_frame * cameraCount * imageSize;
    const int numFrames = FLAGS_frame_count == 0 ? frameCount[i] : (FLAGS_frame_count * cameraCount);
    const int fileSize = numFrames * imageSize;
    posix_fadvise(fd[i], fileSizeStart, fileSize, POSIX_FADV_DONTNEED);
    posix_fadvise(fd[i], fileSizeStart, fileSize, POSIX_FADV_SEQUENTIAL);
#endif

    // Move pointer to start frame
    pos[i] = imageSize * FLAGS_start_frame * ((cameraCount / FLAGS_file_count) + ((i < cameraCount % FLAGS_file_count) ? 1 : 0));

    if (FLAGS_frame_count == 0) {
      totalFrameCount += frameCount[i];
    }

    readCount[i] = -1;
  }

  const int lastFrame = FLAGS_start_frame * cameraCount + totalFrameCount - 1;
  std::vector<unsigned char> imgbuf(imageSize);

  bool isDone = false;
  int percentDonePrev = 0;
  for (int frameNumber = FLAGS_start_frame; frameNumber < FLAGS_start_frame + totalFrameCount / cameraCount; ++frameNumber) {
    for (unsigned int cameraNumber = 0; cameraNumber < cameraCount; ++cameraNumber) {
      const int frameIndex = frameNumber * cameraCount + cameraNumber;
      const int idx = cameraNumber % FLAGS_file_count;
      readCount[idx] = pread(fd[idx], &imgbuf[0], imageSize, pos[idx]);

      // Check if we reached EOF (read returns 0)
      if (readCount[idx] == 0) {
        // Check if all the files have reached EOF
        if (!std::all_of(readCount, readCount + FLAGS_file_count, [](int x){ return x == 0; })) {
          continue;
        }

        isDone = true;
        LOG(WARNING) << "Reached EOF";
        break;
      }

      uint32_t p = 0;
      for (uint32_t y = 0; y < FLAGS_image_height; ++y) {
        for (uint32_t x = 0; x < FLAGS_image_width; ++x) {
          uint32_t pixval;

          if (FLAGS_nbits == 8) {
            pixval = imgbuf[p];
            outImage.at<uint16_t>(y, x) = pixval * 0x101;
            ++p;
          } else if (FLAGS_nbits == 12) {
            uint16_t lo = imgbuf[p];
            uint16_t hi = imgbuf[p + 1];
            uint16_t unswizzled, rep;

            if (x & 1) {
              p += 2;
              unswizzled = hi << 4 | lo >> 4;
            } else {
              p += 1;
              unswizzled = lo << 4 | hi & 0xF;
            }

            rep = unswizzled << 4 | unswizzled >> 8;
            outImage.at<uint16_t>(y, x) = rep;
          }
        }
      }

      pos[idx] += readCount[idx];

      if (frameIndex % 10 == 0 || frameIndex == lastFrame) {
        int percentDoneCurr = frameIndex * 100 / lastFrame;
        LOG_IF(INFO, percentDoneCurr != percentDonePrev) << "Percent done " << percentDoneCurr << "%";
        percentDonePrev = percentDoneCurr;
      }

      string file_tag;
      if (FLAGS_tagged) {
        auto tag = reinterpret_cast<uint32_t*>(&imgbuf[0]);
        file_tag = to_string(tag[1]);
      } else {
        file_tag = cameraNames[cameraNumber];
      }
      string outFilename =
        FLAGS_dest_path + "/img_" +
        to_string(frameNumber) + "_cam_" +
        file_tag + "_raw" + to_string(FLAGS_nbits) + ".tiff";
      imwriteExceptionOnFail(outFilename, outImage);
    }

    if (isDone) {
      break;
    }
  }

  LOG(INFO) << "Closing binary files...";

  for (int i = 0; i < FLAGS_file_count; ++i) {
    close(fd[i]);
  }

  return EXIT_SUCCESS;
}
