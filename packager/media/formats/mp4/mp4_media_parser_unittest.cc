// Copyright 2014 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packager/base/bind.h"
#include "packager/base/logging.h"
#include "packager/media/base/fixed_key_source.h"
#include "packager/media/base/media_sample.h"
#include "packager/media/base/stream_info.h"
#include "packager/media/base/video_stream_info.h"
#include "packager/media/formats/mp4/mp4_media_parser.h"
#include "packager/media/test/test_data_util.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgPointee;

namespace shaka {
namespace media {

namespace {
const char kKey[] =
    "\xeb\xdd\x62\xf1\x68\x14\xd2\x7b\x68\xef\x12\x2a\xfc\xe4\xae\x3c";
const char kKeyId[] = "0123456789012345";

class MockKeySource : public FixedKeySource {
 public:
  MOCK_METHOD1(FetchKeys, Status(const std::vector<uint8_t>& pssh_data));
  MOCK_METHOD2(GetKey,
               Status(const std::vector<uint8_t>& key_id, EncryptionKey* key));
};
}  // namespace

namespace mp4 {

class MP4MediaParserTest : public testing::Test {
 public:
  MP4MediaParserTest() : num_streams_(0), num_samples_(0) {
    parser_.reset(new MP4MediaParser());
  }

 protected:
  typedef std::map<int, scoped_refptr<StreamInfo> > StreamMap;
  StreamMap stream_map_;
  std::unique_ptr<MP4MediaParser> parser_;
  size_t num_streams_;
  size_t num_samples_;

  bool AppendData(const uint8_t* data, size_t length) {
    return parser_->Parse(data, length);
  }

  bool AppendDataInPieces(const uint8_t* data,
                          size_t length,
                          size_t piece_size) {
    const uint8_t* start = data;
    const uint8_t* end = data + length;
    while (start < end) {
      size_t append_size = std::min(piece_size,
                                    static_cast<size_t>(end - start));
      if (!AppendData(start, append_size))
        return false;
      start += append_size;
    }
    return true;
  }

  void InitF(const std::vector<scoped_refptr<StreamInfo> >& streams) {
    for (std::vector<scoped_refptr<StreamInfo> >::const_iterator iter =
             streams.begin();
         iter != streams.end();
         ++iter) {
      DVLOG(2) << (*iter)->ToString();
      stream_map_[(*iter)->track_id()] = *iter;
    }
    num_streams_ = streams.size();
    num_samples_ = 0;
  }

  bool NewSampleF(uint32_t track_id, const scoped_refptr<MediaSample>& sample) {
    DVLOG(2) << "Track Id: " << track_id << " "
             << sample->ToString();
    ++num_samples_;
    return true;
  }

  void InitializeParser(KeySource* decryption_key_source) {
    parser_->Init(
        base::Bind(&MP4MediaParserTest::InitF, base::Unretained(this)),
        base::Bind(&MP4MediaParserTest::NewSampleF, base::Unretained(this)),
        decryption_key_source);
  }

  bool ParseMP4File(const std::string& filename, int append_bytes) {
    InitializeParser(NULL);
    if (!parser_->LoadMoov(GetTestDataFilePath(filename).AsUTF8Unsafe()))
      return false;
    std::vector<uint8_t> buffer = ReadTestDataFile(filename);
    return AppendDataInPieces(buffer.data(), buffer.size(), append_bytes);
  }
};

TEST_F(MP4MediaParserTest, UnalignedAppend) {
  // Test small, non-segment-aligned appends (small enough to exercise
  // incremental append system)
  EXPECT_TRUE(ParseMP4File("bear-640x360-av_frag.mp4", 512));
  EXPECT_EQ(2u, num_streams_);
  EXPECT_EQ(201u, num_samples_);
}

// Verify that the pixel width and pixel height are extracted correctly if
// the container has a 'pasp' box.
TEST_F(MP4MediaParserTest, PixelWidthPixelHeightFromPaspBox) {
  // This content has a 'pasp' box that has the aspect ratio.
  EXPECT_TRUE(ParseMP4File("bear-640x360-non_square_pixel-with_pasp.mp4", 512));

  const int kVideoTrackId = 1;
  EXPECT_EQ(8u,
            reinterpret_cast<VideoStreamInfo*>(stream_map_[kVideoTrackId].get())
                ->pixel_width());
  EXPECT_EQ(9u,
            reinterpret_cast<VideoStreamInfo*>(stream_map_[kVideoTrackId].get())
                ->pixel_height());
}

// Verify that pixel width and height can be extracted from the
// extra data (AVCDecoderConfigurationRecord) for H264.
// No 'pasp' box.
TEST_F(MP4MediaParserTest,
       PixelWidthPixelHeightFromAVCDecoderConfigurationRecord) {
  // This file doesn't have pasp. The stream should extract pixel width and
  // height from SPS.
  EXPECT_TRUE(
      ParseMP4File("bear-640x360-non_square_pixel-without_pasp.mp4", 512));

  const int kVideoTrackId = 1;
  EXPECT_EQ(8u,
            reinterpret_cast<VideoStreamInfo*>(stream_map_[kVideoTrackId].get())
                ->pixel_width());
  EXPECT_EQ(9u,
            reinterpret_cast<VideoStreamInfo*>(stream_map_[kVideoTrackId].get())
                ->pixel_height());
}

// Verify that pixel width and height can be extracted from the
// extra data (AVCDecoderConfigurationRecord) for H264.
// If sar_width and sar_height are not set, then they should both be 1.
TEST_F(MP4MediaParserTest,
       PixelWidthPixelHeightFromAVCDecoderConfigurationRecordNotSet) {
  // This file doesn't have pasp. SPS for the video has
  // sar_width = sar_height = 0. So the stream info should return 1 for both
  // pixel_width and pixel_height.
  EXPECT_TRUE(ParseMP4File("bear-640x360-av_frag.mp4", 512));

  const int kVideoTrackId = 1;
  EXPECT_EQ(1u,
            reinterpret_cast<VideoStreamInfo*>(stream_map_[kVideoTrackId].get())
                ->pixel_width());
  EXPECT_EQ(1u,
            reinterpret_cast<VideoStreamInfo*>(stream_map_[kVideoTrackId].get())
                ->pixel_height());
}

TEST_F(MP4MediaParserTest, BytewiseAppend) {
  // Ensure no incremental errors occur when parsing
  EXPECT_TRUE(ParseMP4File("bear-640x360-av_frag.mp4", 1));
  EXPECT_EQ(2u, num_streams_);
  EXPECT_EQ(201u, num_samples_);
}

TEST_F(MP4MediaParserTest, MultiFragmentAppend) {
  // Large size ensures multiple fragments are appended in one call (size is
  // larger than this particular test file)
  EXPECT_TRUE(ParseMP4File("bear-640x360-av_frag.mp4", 300000));
  EXPECT_EQ(2u, num_streams_);
  EXPECT_EQ(201u, num_samples_);
}

TEST_F(MP4MediaParserTest, TrailingMoov) {
  EXPECT_TRUE(ParseMP4File("bear-640x360-trailing-moov.mp4", 1024));
  EXPECT_EQ(2u, num_streams_);
  EXPECT_EQ(201u, num_samples_);
}

TEST_F(MP4MediaParserTest, Flush) {
  // Flush while reading sample data, then start a new stream.
  InitializeParser(NULL);

  std::vector<uint8_t> buffer = ReadTestDataFile("bear-640x360-av_frag.mp4");
  EXPECT_TRUE(AppendDataInPieces(buffer.data(), 65536, 512));
  EXPECT_TRUE(parser_->Flush());
  EXPECT_EQ(2u, num_streams_);
  EXPECT_NE(0u, num_samples_);
  num_samples_ = 0;
  EXPECT_TRUE(AppendDataInPieces(buffer.data(), buffer.size(), 512));
  EXPECT_EQ(201u, num_samples_);
}

TEST_F(MP4MediaParserTest, MPEG2_AAC_LC) {
  EXPECT_TRUE(ParseMP4File("bear-mpeg2-aac-only_frag.mp4", 512));
  EXPECT_EQ(1u, num_streams_);
  EXPECT_EQ(119u, num_samples_);
}

// Test that a moov box is not always required after Flush() is called.
TEST_F(MP4MediaParserTest, NoMoovAfterFlush) {
  InitializeParser(NULL);

  std::vector<uint8_t> buffer = ReadTestDataFile("bear-640x360-av_frag.mp4");
  EXPECT_TRUE(AppendDataInPieces(buffer.data(), buffer.size(), 512));
  EXPECT_TRUE(parser_->Flush());

  const int kFirstMoofOffset = 1308;
  EXPECT_TRUE(AppendDataInPieces(
      buffer.data() + kFirstMoofOffset, buffer.size() - kFirstMoofOffset, 512));
}

TEST_F(MP4MediaParserTest, NON_FRAGMENTED_MP4) {
  EXPECT_TRUE(ParseMP4File("bear-640x360.mp4", 512));
  EXPECT_EQ(2u, num_streams_);
  EXPECT_EQ(201u, num_samples_);
}

TEST_F(MP4MediaParserTest, CencWithoutDecryptionSource) {
  EXPECT_TRUE(ParseMP4File("bear-640x360-v_frag-cenc-aux.mp4", 512));
  EXPECT_EQ(1u, num_streams_);
  // Check if pssh is present.
  const int kVideoTrackId = 1;
  EXPECT_NE(0u,
            reinterpret_cast<VideoStreamInfo*>(stream_map_[kVideoTrackId].get())
                ->eme_init_data().size());
}

TEST_F(MP4MediaParserTest, CencInitWithoutDecryptionSource) {
  InitializeParser(NULL);

  std::vector<uint8_t> buffer =
      ReadTestDataFile("bear-640x360-v_frag-cenc-aux.mp4");
  const int kFirstMoofOffset = 1646;
  EXPECT_TRUE(AppendDataInPieces(buffer.data(), kFirstMoofOffset, 512));
  EXPECT_EQ(1u, num_streams_);
}

TEST_F(MP4MediaParserTest, CencWithDecryptionSourceAndAuxInMdat) {
  MockKeySource mock_key_source;
  EXPECT_CALL(mock_key_source, FetchKeys(_)).WillOnce(Return(Status::OK));

  EncryptionKey encryption_key;
  encryption_key.key.assign(kKey, kKey + strlen(kKey));
  EXPECT_CALL(mock_key_source,
              GetKey(std::vector<uint8_t>(kKeyId, kKeyId + strlen(kKeyId)), _))
      .WillOnce(DoAll(SetArgPointee<1>(encryption_key), Return(Status::OK)));

  InitializeParser(&mock_key_source);

  std::vector<uint8_t> buffer =
      ReadTestDataFile("bear-640x360-v_frag-cenc-aux.mp4");
  EXPECT_TRUE(AppendDataInPieces(buffer.data(), buffer.size(), 512));
  EXPECT_EQ(1u, num_streams_);
  EXPECT_EQ(82u, num_samples_);
}

TEST_F(MP4MediaParserTest, CencWithDecryptionSourceAndSenc) {
  MockKeySource mock_key_source;
  EXPECT_CALL(mock_key_source, FetchKeys(_)).WillOnce(Return(Status::OK));

  EncryptionKey encryption_key;
  encryption_key.key.assign(kKey, kKey + strlen(kKey));
  EXPECT_CALL(mock_key_source,
              GetKey(std::vector<uint8_t>(kKeyId, kKeyId + strlen(kKeyId)), _))
      .WillOnce(DoAll(SetArgPointee<1>(encryption_key), Return(Status::OK)));

  InitializeParser(&mock_key_source);

  std::vector<uint8_t> buffer =
      ReadTestDataFile("bear-640x360-v_frag-cenc-senc.mp4");
  EXPECT_TRUE(AppendDataInPieces(buffer.data(), buffer.size(), 512));
  EXPECT_EQ(1u, num_streams_);
  EXPECT_EQ(82u, num_samples_);
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
